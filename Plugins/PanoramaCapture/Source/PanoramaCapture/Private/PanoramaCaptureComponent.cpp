#include "PanoramaCaptureComponent.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "PanoramaCubemapToEquirectCS.h"
#include "PanoramaPngWriter.h"
#include "PanoramaAudioRecorder.h"
#include "PanoramaNvencEncoder.h"
#include "PanoramaCaptureModule.h"
#include "PanoramaCaptureSettings.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RendererInterface.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"
#include "RenderTargetPool.h"
#include "RenderUtils.h"
#include "RenderingThread.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ShaderCore.h"
#include "SceneInterface.h"
#include "SceneView.h"
#include "SceneRendering.h"
#include "Async/Async.h"

namespace
{
    constexpr int32 kCubemapFaceCount = 6;

    FIntPoint GetTargetResolution(const FPanoCaptureOutputSettings& Settings)
    {
        if (Settings.bUse8k)
        {
            return FIntPoint(7680, 3840);
        }
        return FIntPoint(Settings.Resolution.Width, Settings.Resolution.Height);
    }

    FString SanitizeSessionName(const FString& InValue)
    {
        FString Sanitized = FPaths::MakeValidFileName(InValue);
        if (Sanitized.IsEmpty())
        {
            Sanitized = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        }
        return Sanitized;
    }

    FString ResolveSessionLabel(const FString& Input)
    {
        const FString SanitizedInput = FPaths::MakeValidFileName(Input);
        const UPanoramaCaptureSettings* Settings = GetDefault<UPanoramaCaptureSettings>();
        FString Format = Settings ? Settings->OutputFileNameFormat : FString();

        if (!Format.IsEmpty())
        {
            const FDateTime Now = FDateTime::Now();
            Format = Format.Replace(TEXT("{date}"), *Now.ToString(TEXT("%Y%m%d")));
            Format = Format.Replace(TEXT("{time}"), *Now.ToString(TEXT("%H%M%S")));
            Format = Format.Replace(TEXT("{guid}"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

            if (Format.Contains(TEXT("{label}")))
            {
                const FString LabelValue = !SanitizedInput.IsEmpty() ? SanitizedInput : TEXT("Capture");
                Format = Format.Replace(TEXT("{label}"), *LabelValue);
            }
            else if (!SanitizedInput.IsEmpty())
            {
                Format += TEXT("_") + SanitizedInput;
            }

            const FString Sanitized = FPaths::MakeValidFileName(Format);
            if (!Sanitized.IsEmpty())
            {
                return Sanitized;
            }
        }

        if (!SanitizedInput.IsEmpty())
        {
            return SanitizedInput;
        }

        return SanitizeSessionName(TEXT("Panorama"));
    }

    struct FPanoCaptureFrame
    {
        uint64 FrameIndex = 0;
        double Timecode = 0.0;
        FIntPoint Resolution;
        bool bLinear = false;
        bool b16Bit = false;
        TArray<uint8> PixelData;
    };

    FString LocateFfmpegExecutable()
    {
        TArray<FString> CandidatePaths;
        CandidatePaths.Add(TEXT("ffmpeg.exe"));
        CandidatePaths.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries/ThirdParty/ffmpeg.exe")));
        CandidatePaths.Add(FPaths::Combine(FPaths::ProjectDir(), TEXT("ThirdParty/ffmpeg/bin/ffmpeg.exe")));

        for (const FString& Path : CandidatePaths)
        {
            if (FPaths::FileExists(Path))
            {
                return Path;
            }
        }

        return FString();
    }

    bool RunFfmpeg(const FString& CommandLine)
    {
        const FString Executable = LocateFfmpegExecutable();
        if (Executable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("FFmpeg executable not found. Skipping container packaging."));
            return false;
        }

        FProcHandle Proc = FPlatformProcess::CreateProc(*Executable, *CommandLine, true, false, false, nullptr, 0, nullptr, nullptr);
        if (!Proc.IsValid())
        {
            UE_LOG(LogTemp, Warning, TEXT("Failed to launch FFmpeg: %s"), *Executable);
            return false;
        }

        FPlatformProcess::WaitForProc(Proc);
        int32 ReturnCode = 0;
        FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
        return ReturnCode == 0;
    }

    void PackageSequenceToContainer(const FString& SequencePattern, const FString& AudioPath, float FrameRate, const FString& OutputPath, const FPanoNvencRateControl& RateControl, EPanoramaCaptureCodec Codec)
    {
        FString CommandLine = FString::Printf(TEXT(" -y -framerate %.3f -i \"%s\""), FrameRate, *SequencePattern);
        if (!AudioPath.IsEmpty() && FPaths::FileExists(AudioPath))
        {
            CommandLine += FString::Printf(TEXT(" -i \"%s\" -c:a aac"), *AudioPath);
        }
        else
        {
            CommandLine += TEXT(" -an");
        }

        const FString CodecName = Codec == EPanoramaCaptureCodec::H264 ? TEXT("h264_nvenc") : TEXT("hevc_nvenc");
        const int32 Bitrate = FMath::Max(1, FMath::RoundToInt(RateControl.BitrateMbps));
        const FString RateMode = RateControl.bUseCBR ? TEXT("cbr") : TEXT("vbr");
        CommandLine += FString::Printf(TEXT(" -c:v %s -rc:v %s -b:v %dM -g %d -bf %d"), *CodecName, *RateMode, Bitrate, RateControl.GOPLength, RateControl.NumBFrames);
        if (RateControl.bUseCBR)
        {
            CommandLine += FString::Printf(TEXT(" -minrate %dM -maxrate %dM"), Bitrate, Bitrate);
        }
        else
        {
            CommandLine += FString::Printf(TEXT(" -maxrate %dM"), Bitrate);
        }
        CommandLine += FString::Printf(TEXT(" \"%s\""), *OutputPath);

        RunFfmpeg(CommandLine);
    }

    void PackageBitstreamToContainer(const FString& BitstreamPath, const FString& AudioPath, float FrameRate, const FString& OutputPath, EPanoramaCaptureCodec Codec)
    {
        FString CommandLine = FString::Printf(TEXT(" -y -framerate %.3f -i \"%s\""), FrameRate, *BitstreamPath);
        if (!AudioPath.IsEmpty() && FPaths::FileExists(AudioPath))
        {
            CommandLine += FString::Printf(TEXT(" -i \"%s\" -c:a aac"), *AudioPath);
        }
        else
        {
            CommandLine += TEXT(" -an");
        }

        const FString CodecFlag = Codec == EPanoramaCaptureCodec::H264 ? TEXT(" -c:v copy -bsf:v h264_mp4toannexb") : TEXT(" -c:v copy");
        CommandLine += CodecFlag;
        CommandLine += FString::Printf(TEXT(" \"%s\""), *OutputPath);

        RunFfmpeg(CommandLine);
    }

    FString MakeUniqueOutputPath(const FString& BasePath, bool bOverwrite)
    {
        if (bOverwrite || !FPaths::FileExists(BasePath))
        {
            return BasePath;
        }

        FString Directory = FPaths::GetPath(BasePath);
        FString FileName = FPaths::GetBaseFilename(BasePath);
        FString Extension = FPaths::GetExtension(BasePath, true);

        int32 Index = 1;
        FString Candidate;
        do
        {
            Candidate = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%d%s"), *FileName, Index, *Extension));
            ++Index;
        } while (FPaths::FileExists(Candidate));

        return Candidate;
    }

    class FPanoFrameRingBuffer
    {
    public:
        FPanoFrameRingBuffer(int32 InCapacity)
            : Capacity(InCapacity)
            , Head(0)
            , Tail(0)
            , Count(0)
        {
            Buffer.SetNum(Capacity);
        }

        bool Enqueue(FPanoCaptureFrame&& Frame)
        {
            FScopeLock Lock(&CriticalSection);
            if (Count == Capacity)
            {
                return false;
            }

            Buffer[Head] = MoveTemp(Frame);
            Head = (Head + 1) % Capacity;
            ++Count;
            return true;
        }

        bool Dequeue(FPanoCaptureFrame& OutFrame)
        {
            FScopeLock Lock(&CriticalSection);
            if (Count == 0)
            {
                return false;
            }

            OutFrame = MoveTemp(Buffer[Tail]);
            Tail = (Tail + 1) % Capacity;
            --Count;
            return true;
        }

        void Reset()
        {
            FScopeLock Lock(&CriticalSection);
            Head = 0;
            Tail = 0;
            Count = 0;
        }

        int32 Num() const
        {
            return Count;
        }

    private:
        TArray<FPanoCaptureFrame> Buffer;
        int32 Capacity;
        int32 Head;
        int32 Tail;
        int32 Count;
        mutable FCriticalSection CriticalSection;
    };

    class FPanoCaptureWorker
    {
    public:
        FPanoCaptureWorker(FPanoFrameRingBuffer* InRingBuffer, FPanoPngWriter* InPngWriter)
            : RingBuffer(InRingBuffer)
            , PngWriter(InPngWriter)
            , bIsRunning(false)
        {
        }

        void Start()
        {
            if (bIsRunning)
            {
                return;
            }

            bIsRunning = true;
            WorkerThread = Async(EAsyncExecution::Thread, [this]()
            {
                Run();
            });
        }

        void Stop()
        {
            bIsRunning = false;
            if (WorkerThread.IsValid())
            {
                WorkerThread.Wait();
                WorkerThread = TFuture<void>();
            }
        }

        void Run()
        {
            FPanoCaptureFrame Frame;
            while (bIsRunning)
            {
                if (!RingBuffer || !RingBuffer->Dequeue(Frame))
                {
                    FPlatformProcess::Sleep(0.001f);
                    continue;
                }

                if (PngWriter)
                {
                    FPanoPngFrame PngFrame;
                    PngFrame.FrameIndex = Frame.FrameIndex;
                    PngFrame.Timecode = Frame.Timecode;
                    PngFrame.Resolution = Frame.Resolution;
                    PngFrame.PixelData = MoveTemp(Frame.PixelData);
                    PngFrame.b16Bit = Frame.b16Bit;
                    PngWriter->EnqueueFrame(MoveTemp(PngFrame));
                }
            }
        }

    private:
        FPanoFrameRingBuffer* RingBuffer;
        FPanoPngWriter* PngWriter;
        TFuture<void> WorkerThread;
        FThreadSafeBool bIsRunning;
    };
}

UPanoramaCaptureComponent::UPanoramaCaptureComponent(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
    , CaptureMode(EPanoramaCaptureMode::Mono)
    , CaptureFrameRate(30.f)
    , bRecordOnBeginPlay(false)
    , bEnablePreview(true)
    , PreviewScale(0.25f)
    , RingBufferSize(4)
    , bUseLinearGammaForNVENC(false)
    , bUse16BitPng(true)
    , CaptureStatus(EPanoramaCaptureStatus::Idle)
    , TimeSinceLastCapture(0.f)
    , RecordingStartTime(0.0)
    , FrameRingBuffer(nullptr)
    , FrameIndex(0)
    , DroppedFrameCount(0)
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    bAutoActivate = true;

    if (const UPanoramaCaptureSettings* Settings = GetDefault<UPanoramaCaptureSettings>())
    {
        CaptureMode = Settings->DefaultCaptureMode;
        OutputSettings = Settings->DefaultOutputSettings;
    }
}

void UPanoramaCaptureComponent::OnRegister()
{
    Super::OnRegister();
    InitializeCaptureFaces();
}

void UPanoramaCaptureComponent::BeginPlay()
{
    Super::BeginPlay();

    if (bRecordOnBeginPlay)
    {
        StartRecording();
    }
}

void UPanoramaCaptureComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopRecording();

    Super::EndPlay(EndPlayReason);
}

void UPanoramaCaptureComponent::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (CaptureStatus != EPanoramaCaptureStatus::Recording)
    {
        return;
    }

    TimeSinceLastCapture += DeltaTime;
    const float FrameInterval = 1.f / FMath::Max(CaptureFrameRate, 0.001f);
    if (TimeSinceLastCapture < FrameInterval)
    {
        return;
    }

    TimeSinceLastCapture = 0.f;
    EnqueueFrameCapture(DeltaTime);
    ProcessPendingFrames();
}

void UPanoramaCaptureComponent::InitializeCaptureFaces()
{
    if (FaceCaptures.Num() == kCubemapFaceCount)
    {
        return;
    }

    FaceCaptures.Reset();
    FaceRenderTargets.Reset();

    const TArray<FRotator> Rotations = {
        FRotator(0.f, 90.f, 0.f),
        FRotator(0.f, -90.f, 0.f),
        FRotator(-90.f, 0.f, 0.f),
        FRotator(90.f, 0.f, 0.f),
        FRotator(0.f, 0.f, 0.f),
        FRotator(0.f, 180.f, 0.f)
    };

    for (int32 FaceIndex = 0; FaceIndex < kCubemapFaceCount; ++FaceIndex)
    {
        const FString Name = FString::Printf(TEXT("PanoCaptureFace_%d"), FaceIndex);
        USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(this, *Name);
        Capture->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
        Capture->RegisterComponent();
        Capture->FOVAngle = 90.f;
        Capture->bCaptureEveryFrame = false;
        Capture->bCaptureOnMovement = false;
        Capture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
        Capture->SetRelativeRotation(Rotations[FaceIndex]);
        FaceCaptures.Add(Capture);
    }

    AllocateRenderTargets();
}

void UPanoramaCaptureComponent::AllocateRenderTargets()
{
    DestroyRenderTargets();

    const FIntPoint FaceResolution(OutputSettings.bUse8k ? 4096 : 2048, OutputSettings.bUse8k ? 4096 : 2048);
    const FIntPoint BaseEquirectResolution = GetTargetResolution(OutputSettings);
    const int32 EyeCount = CaptureMode == EPanoramaCaptureMode::Stereo ? 2 : 1;
    const FIntPoint EquirectResolution(BaseEquirectResolution.X, BaseEquirectResolution.Y * EyeCount);
    const ETextureRenderTargetFormat TargetFormat = (OutputSettings.OutputMode == EPanoramaCaptureOutputMode::PNGSequence && bUse16BitPng)
        ? ETextureRenderTargetFormat::RTF_RGBA16f
        : ETextureRenderTargetFormat::RTF_RGBA8;

    for (int32 FaceIndex = 0; FaceIndex < FaceCaptures.Num(); ++FaceIndex)
    {
        UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(this);
        RenderTarget->RenderTargetFormat = TargetFormat;
        RenderTarget->InitAutoFormat(FaceResolution.X, FaceResolution.Y);
        RenderTarget->bAutoGenerateMips = false;
        RenderTarget->ClearColor = FLinearColor::Black;
        RenderTarget->UpdateResourceImmediate(true);
        FaceRenderTargets.Add(RenderTarget);
        FaceCaptures[FaceIndex]->TextureTarget = RenderTarget;
    }

    EquirectRenderTarget = NewObject<UTextureRenderTarget2D>(this);
    EquirectRenderTarget->RenderTargetFormat = TargetFormat;
    EquirectRenderTarget->InitAutoFormat(EquirectResolution.X, EquirectResolution.Y);
    EquirectRenderTarget->bAutoGenerateMips = false;
    EquirectRenderTarget->ClearColor = FLinearColor::Black;
    EquirectRenderTarget->UpdateResourceImmediate(true);

    if (bEnablePreview && OutputSettings.bWritePreviewTexture)
    {
        const FIntPoint PreviewRes(EquirectResolution.X, EquirectResolution.Y);
        PreviewRenderTarget = NewObject<UTextureRenderTarget2D>(this);
        PreviewRenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
        PreviewRenderTarget->InitAutoFormat(PreviewRes.X, PreviewRes.Y);
        PreviewRenderTarget->UpdateResourceImmediate(true);
    }
}

void UPanoramaCaptureComponent::DestroyRenderTargets()
{
    for (UTextureRenderTarget2D* Target : FaceRenderTargets)
    {
        if (Target)
        {
            Target->ReleaseResource();
        }
    }
    FaceRenderTargets.Reset();

    if (EquirectRenderTarget)
    {
        EquirectRenderTarget->ReleaseResource();
        EquirectRenderTarget = nullptr;
    }

    if (PreviewRenderTarget)
    {
        PreviewRenderTarget->ReleaseResource();
        PreviewRenderTarget = nullptr;
    }
}

void UPanoramaCaptureComponent::StartRecording()
{
    if (CaptureStatus == EPanoramaCaptureStatus::Recording)
    {
        return;
    }

    if (!ResolveOutputDirectory(ActiveOutputDirectory))
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to resolve output directory."));
        return;
    }

    ActiveSessionName = ResolveSessionLabel(RecordingLabel);

    InitializeCaptureFaces();

    if (OutputSettings.OutputMode == EPanoramaCaptureOutputMode::PNGSequence)
    {
        FrameRingBuffer = new FPanoFrameRingBuffer(FMath::Max(1, RingBufferSize));
        PngWriter = MakeUnique<FPanoPngWriter>();

        FPanoPngWriteParams PngParams;
        PngParams.OutputDirectory = ActiveOutputDirectory;
        PngParams.BaseFileName = ActiveSessionName;
        PngParams.bUse16Bit = bUse16BitPng;
        PngParams.bLinear = OutputSettings.bLinearColorSpace;

        PngWriter->Configure(PngParams);
        CaptureWorker = MakeUnique<FPanoCaptureWorker>(FrameRingBuffer, PngWriter.Get());
        CaptureWorker->Start();
        CaptureStatus = EPanoramaCaptureStatus::Recording;
    }
    else
    {
        FrameRingBuffer = nullptr;
        PngWriter.Reset();
        CaptureWorker.Reset();
#if PANORAMA_CAPTURE_WITH_NVENC
        NvencEncoder = MakeUnique<FPanoNvencEncoder>();
        const FIntPoint BaseResolution = GetTargetResolution(OutputSettings);
        const int32 EyeCount = CaptureMode == EPanoramaCaptureMode::Stereo ? 2 : 1;

        FPanoramaNvencEncodeParams EncodeParams;
        EncodeParams.Resolution = FIntPoint(BaseResolution.X, BaseResolution.Y * EyeCount);
        EncodeParams.Codec = OutputSettings.Codec;
        EncodeParams.RateControl = OutputSettings.NvencRateControl;
        EncodeParams.bUseLinear = bUseLinearGammaForNVENC;
        EncodeParams.FrameRate = CaptureFrameRate;
        const FString BitstreamExtension = OutputSettings.Codec == EPanoramaCaptureCodec::H264 ? TEXT("h264") : TEXT("hevc");
        EncodeParams.OutputBitstreamPath = FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s.%s.annexb"), *ActiveSessionName, *BitstreamExtension));

        if (!NvencEncoder->Initialize(EncodeParams))
        {
            NvencEncoder.Reset();
            UE_LOG(LogTemp, Error, TEXT("Failed to initialize NVENC encoder."));
            return;
        }
        CaptureStatus = EPanoramaCaptureStatus::Recording;
#else
        UE_LOG(LogTemp, Warning, TEXT("NVENC output requested but not supported on this platform."));
        return;
#endif
    }

    AudioRecorder = MakeUnique<FPanoAudioRecorder>();

    USoundSubmixBase* TargetSubmix = OverrideAudioSubmix;
    if (!TargetSubmix)
    {
        TargetSubmix = GetDefault<UPanoramaCaptureSettings>()->TargetSubmix;
    }

    if (TargetSubmix)
    {
        AudioRecorder->StartRecording(TargetSubmix, 48000, 2);
    }

    TimeSinceLastCapture = 0.f;
    RecordingStartTime = FPlatformTime::Seconds();
    FrameIndex = 0;
    DroppedFrameCount = 0;

    UE_LOG(LogTemp, Log, TEXT("Panorama capture started: %s"), *ActiveSessionName);
}

void UPanoramaCaptureComponent::StopRecording()
{
    if (CaptureStatus != EPanoramaCaptureStatus::Recording)
    {
        return;
    }

    CaptureStatus = EPanoramaCaptureStatus::Finalizing;

    FlushRingBuffer();
    FinalizeRecording();
}

void UPanoramaCaptureComponent::TogglePreview(bool bEnableIn)
{
    bEnablePreview = bEnableIn;
    UpdatePreview();
}

void UPanoramaCaptureComponent::ReleaseResources()
{
    if (CaptureWorker)
    {
        CaptureWorker->Stop();
        CaptureWorker.Reset();
    }

    if (FrameRingBuffer)
    {
        delete FrameRingBuffer;
        FrameRingBuffer = nullptr;
    }

    if (PngWriter)
    {
        PngWriter->Flush();
        PngWriter->Shutdown();
        PngWriter.Reset();
    }

    if (AudioRecorder)
    {
        AudioRecorder->StopRecording();
        AudioRecorder.Reset();
    }

#if PANORAMA_CAPTURE_WITH_NVENC
    if (NvencEncoder)
    {
        NvencEncoder->Shutdown();
        NvencEncoder.Reset();
    }
#endif

    DestroyRenderTargets();
}

void UPanoramaCaptureComponent::EnqueueFrameCapture(float DeltaTime)
{
    if (FaceCaptures.Num() != kCubemapFaceCount)
    {
        InitializeCaptureFaces();
    }
    const int32 EyeCount = CaptureMode == EPanoramaCaptureMode::Stereo ? 2 : 1;
    const float EyeOffsetCm = 6.4f;
    const FVector EyeOffsets[2] = { FVector(-EyeOffsetCm * 0.5f, 0.f, 0.f), FVector(EyeOffsetCm * 0.5f, 0.f, 0.f) };

    for (int32 EyeIndex = 0; EyeIndex < EyeCount; ++EyeIndex)
    {
        if (EyeCount == 2)
        {
            for (USceneCaptureComponent2D* Capture : FaceCaptures)
            {
                if (Capture)
                {
                    Capture->SetRelativeLocation(EyeOffsets[EyeIndex]);
                }
            }
        }

        for (USceneCaptureComponent2D* Capture : FaceCaptures)
        {
            if (Capture && Capture->TextureTarget)
            {
                Capture->CaptureScene();
            }
        }

        DispatchCubemapToEquirect(EyeIndex, EyeCount);
    }

    if (EyeCount == 2)
    {
        for (USceneCaptureComponent2D* Capture : FaceCaptures)
        {
            if (Capture)
            {
                Capture->SetRelativeLocation(FVector::ZeroVector);
            }
        }
    }
    UpdatePreview();

    const double Timecode = FPlatformTime::Seconds() - RecordingStartTime;

    if (OutputSettings.OutputMode == EPanoramaCaptureOutputMode::PNGSequence)
    {
        FTextureRenderTargetResource* Resource = EquirectRenderTarget ? EquirectRenderTarget->GameThread_GetRenderTargetResource() : nullptr;
        if (!Resource)
        {
            return;
        }

        const FIntPoint Resolution = FIntPoint(EquirectRenderTarget->SizeX, EquirectRenderTarget->SizeY);

        FPanoCaptureFrame Frame;
        Frame.FrameIndex = FrameIndex;
        Frame.Timecode = Timecode;
        Frame.Resolution = Resolution;
        Frame.b16Bit = bUse16BitPng;
        Frame.bLinear = OutputSettings.bLinearColorSpace;

        if (bUse16BitPng)
        {
            TArray<FLinearColor> LinearPixels;
            Resource->ReadLinearColorPixels(LinearPixels);
            Frame.PixelData.SetNum(LinearPixels.Num() * sizeof(FFloat16Color));
            FFloat16Color* Dest = reinterpret_cast<FFloat16Color*>(Frame.PixelData.GetData());
            for (int32 Index = 0; Index < LinearPixels.Num(); ++Index)
            {
                Dest[Index] = FFloat16Color(LinearPixels[Index]);
            }
        }
        else
        {
            TArray<FColor> Pixels;
            Resource->ReadPixels(Pixels);
            Frame.PixelData.SetNum(Pixels.Num() * sizeof(FColor));
            FMemory::Memcpy(Frame.PixelData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));
        }

        if (!FrameRingBuffer || !FrameRingBuffer->Enqueue(MoveTemp(Frame)))
        {
            HandleDroppedFrame();
        }
    }
    else
    {
#if PANORAMA_CAPTURE_WITH_NVENC
        if (NvencEncoder)
        {
            FTextureRHIRef Texture = EquirectRenderTarget->GetRenderTargetResource()->GetTextureRHI();
            NvencEncoder->EnqueueResource(Texture, FrameIndex, Timecode);
        }
#endif
    }

    ++FrameIndex;
}

void UPanoramaCaptureComponent::ProcessPendingFrames()
{
    if (CaptureWorker)
    {
        // Worker consumes frames asynchronously.
    }
}

void UPanoramaCaptureComponent::DispatchCubemapToEquirect(int32 EyeIndex, int32 EyeCount)
{
    if (!EquirectRenderTarget)
    {
        return;
    }

    FRHITexture* OutputTexture = EquirectRenderTarget->GetRenderTargetResource()->GetRenderTargetTexture();
    if (!OutputTexture)
    {
        return;
    }

    const int32 FullWidth = EquirectRenderTarget->SizeX;
    const int32 BaseHeight = EquirectRenderTarget->SizeY / FMath::Max(1, EyeCount);

    TArray<FMatrix44f> ViewMatrices;
    ViewMatrices.SetNum(kCubemapFaceCount);
    for (int32 Index = 0; Index < kCubemapFaceCount; ++Index)
    {
        if (FaceCaptures.IsValidIndex(Index) && FaceCaptures[Index])
        {
            const FMatrix ViewMatrix = FaceCaptures[Index]->GetComponentTransform().ToInverseMatrixWithScale();
            ViewMatrices[Index] = FMatrix44f(ViewMatrix);
        }
        else
        {
            ViewMatrices[Index] = FMatrix44f(FMatrix::Identity);
        }
    }

    const bool bLinearOutput = (OutputSettings.OutputMode == EPanoramaCaptureOutputMode::NVENC) ? bUseLinearGammaForNVENC : OutputSettings.bLinearColorSpace;

    ENQUEUE_RENDER_COMMAND(PanoramaCapture_DispatchRDG)(
        [FaceRenderTargets = FaceRenderTargets, ViewMatrices, OutputTexture, bLinearOutput, EyeIndex, EyeCount, FullWidth, BaseHeight](FRHICommandListImmediate& RHICmdList)
        {
            FRDGBuilder GraphBuilder(RHICmdList);

            FPanoCubemapToEquirectCS::FParameters* Parameters = GraphBuilder.AllocParameters<FPanoCubemapToEquirectCS::FParameters>();
            Parameters->OutputResolution = FVector2f(FullWidth, BaseHeight);
            Parameters->InvOutputResolution = FVector2f(1.0f / FullWidth, 1.0f / BaseHeight);
            Parameters->FullResolution = FVector2f(FullWidth, BaseHeight * EyeCount);
            Parameters->OutputOffset = FVector2f(0.f, EyeIndex * BaseHeight);
            Parameters->bLinearColorSpace = bLinearOutput ? 1.0f : 0.0f;

            for (int32 Index = 0; Index < ViewMatrices.Num(); ++Index)
            {
                Parameters->ViewMatrices[Index] = ViewMatrices[Index];
            }

            for (int32 Index = 0; Index < FaceRenderTargets.Num(); ++Index)
            {
                FRDGTextureRef FaceTexture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(FaceRenderTargets[Index]->GetRenderTargetResource()->GetRenderTargetTexture(), TEXT("PanoramaFace")));
                Parameters->FaceTextures[Index] = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(FaceTexture));
            }

            Parameters->FaceSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
            FRDGTextureRef Output = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(OutputTexture, TEXT("PanoramaEquirect")));
            Parameters->OutputTexture = GraphBuilder.CreateUAV(Output);

            TShaderMapRef<FPanoCubemapToEquirectCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            const FIntVector GroupCount(
                FMath::DivideAndRoundUp(FullWidth, 8),
                FMath::DivideAndRoundUp(BaseHeight, 8),
                1);

            FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("PanoramaCubemapToEquirect"), ComputeShader, Parameters, GroupCount);
            GraphBuilder.Execute();
        });
}

void UPanoramaCaptureComponent::UpdatePreview()
{
    if (!bEnablePreview || !PreviewRenderTarget || !EquirectRenderTarget)
    {
        return;
    }

    FRHITexture* SourceTexture = EquirectRenderTarget->GetRenderTargetResource()->GetRenderTargetTexture();
    FRHITexture* DestTexture = PreviewRenderTarget->GetRenderTargetResource()->GetRenderTargetTexture();
    if (!SourceTexture || !DestTexture)
    {
        return;
    }

    ENQUEUE_RENDER_COMMAND(PanoramaCapture_UpdatePreview)(
        [SourceTexture, DestTexture](FRHICommandListImmediate& RHICmdList)
        {
            FRHICopyTextureInfo CopyInfo;
            CopyInfo.Size = FIntVector(DestTexture->GetDesc().Extent.X, DestTexture->GetDesc().Extent.Y, 1);
            RHICmdList.CopyTexture(SourceTexture, DestTexture, CopyInfo);
        });
}

void UPanoramaCaptureComponent::HandleDroppedFrame()
{
    ++DroppedFrameCount;
    CaptureStatus = EPanoramaCaptureStatus::DroppedFrames;
    UE_LOG(LogTemp, Warning, TEXT("Panorama capture dropped frame %u"), DroppedFrameCount);
}

void UPanoramaCaptureComponent::FlushRingBuffer()
{
    if (CaptureWorker)
    {
        CaptureWorker->Stop();
    }

    if (FrameRingBuffer)
    {
        FPanoCaptureFrame Frame;
        while (FrameRingBuffer->Dequeue(Frame))
        {
        }
    }

    if (PngWriter)
    {
        PngWriter->Flush();
    }
}

void UPanoramaCaptureComponent::FinalizeRecording()
{
    FlushRenderingCommands();

    FString AudioPath;
    if (AudioRecorder)
    {
        double AudioDuration = 0.0;
        AudioPath = FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s.wav"), *ActiveSessionName));
        AudioRecorder->StopRecording();
        AudioRecorder->WriteToWav(AudioPath, AudioDuration);
        AudioRecorder.Reset();
    }

    const UPanoramaCaptureSettings* Settings = GetDefault<UPanoramaCaptureSettings>();
    const bool bEmbedAudio = Settings ? Settings->bEmbedAudioInContainer : true;
    const bool bOverwriteExisting = Settings ? Settings->bOverwriteExisting : false;
    const bool bGenerateMkv = Settings ? Settings->bGenerateMKV : true;

    if (OutputSettings.OutputMode == EPanoramaCaptureOutputMode::PNGSequence && PngWriter)
    {
        PngWriter->Flush();
        const FString SequencePattern = FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s_%%06d.png"), *ActiveSessionName));

        const FString Mp4Path = MakeUniqueOutputPath(FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s.mp4"), *ActiveSessionName)), bOverwriteExisting);
        PackageSequenceToContainer(SequencePattern, bEmbedAudio ? AudioPath : FString(), CaptureFrameRate, Mp4Path, OutputSettings.NvencRateControl, OutputSettings.Codec);
        UE_LOG(LogTemp, Log, TEXT("Panorama capture packaged to %s"), *Mp4Path);

        if (bGenerateMkv)
        {
            const FString MkvPath = MakeUniqueOutputPath(FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s.mkv"), *ActiveSessionName)), bOverwriteExisting);
            PackageSequenceToContainer(SequencePattern, bEmbedAudio ? AudioPath : FString(), CaptureFrameRate, MkvPath, OutputSettings.NvencRateControl, OutputSettings.Codec);
            UE_LOG(LogTemp, Log, TEXT("Panorama capture packaged to %s"), *MkvPath);
        }
    }

#if PANORAMA_CAPTURE_WITH_NVENC
    if (OutputSettings.OutputMode == EPanoramaCaptureOutputMode::NVENC && NvencEncoder)
    {
        TArray<FPanoramaEncodedFrame> EncodedFrames;
        NvencEncoder->Flush(EncodedFrames);
        const FString EncoderBitstreamPath = NvencEncoder->GetParams().OutputBitstreamPath;
        NvencEncoder->Shutdown();

        FString BitstreamPath = EncoderBitstreamPath;
        if (BitstreamPath.IsEmpty())
        {
            const FString Extension = OutputSettings.Codec == EPanoramaCaptureCodec::H264 ? TEXT("h264") : TEXT("hevc");
            BitstreamPath = FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s.%s.annexb"), *ActiveSessionName, *Extension));
        }

        if (!BitstreamPath.IsEmpty() && !FPaths::FileExists(BitstreamPath) && EncodedFrames.Num() > 0)
        {
            TArray<uint8> OutputData;
            for (const FPanoramaEncodedFrame& Frame : EncodedFrames)
            {
                OutputData.Append(Frame.EncodedBytes);
            }
            if (!FFileHelper::SaveArrayToFile(OutputData, *BitstreamPath))
            {
                UE_LOG(LogTemp, Warning, TEXT("Failed to persist NVENC bitstream to %s"), *BitstreamPath);
            }
        }

        if (!BitstreamPath.IsEmpty() && FPaths::FileExists(BitstreamPath))
        {
            const FString Mp4Path = MakeUniqueOutputPath(FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s.mp4"), *ActiveSessionName)), bOverwriteExisting);
            PackageBitstreamToContainer(BitstreamPath, bEmbedAudio ? AudioPath : FString(), CaptureFrameRate, Mp4Path, OutputSettings.Codec);
            UE_LOG(LogTemp, Log, TEXT("NVENC bitstream packaged to %s"), *Mp4Path);

            if (bGenerateMkv)
            {
                const FString MkvPath = MakeUniqueOutputPath(FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s.mkv"), *ActiveSessionName)), bOverwriteExisting);
                PackageBitstreamToContainer(BitstreamPath, bEmbedAudio ? AudioPath : FString(), CaptureFrameRate, MkvPath, OutputSettings.Codec);
                UE_LOG(LogTemp, Log, TEXT("NVENC bitstream packaged to %s"), *MkvPath);
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("NVENC bitstream not found for session %s."), *ActiveSessionName);
        }
    }
#endif

    CaptureStatus = EPanoramaCaptureStatus::Idle;
    ReleaseResources();

    UE_LOG(LogTemp, Log, TEXT("Panorama capture finalized: %s"), *ActiveSessionName);
}

bool UPanoramaCaptureComponent::ResolveOutputDirectory(FString& OutDirectory) const
{
    FString Dir = OutputSettings.TargetDirectory.Path;
    if (Dir.IsEmpty())
    {
        Dir = TEXT("PanoramaCaptures");
    }

    if (FPaths::IsRelative(Dir))
    {
        Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir(), Dir);
    }

    IFileManager::Get().MakeDirectory(*Dir, true);
    OutDirectory = Dir;
    return true;
}

FString UPanoramaCaptureComponent::GenerateOutputFileName(const FString& Extension) const
{
    const FString FileName = FString::Printf(TEXT("%s.%s"), *ActiveSessionName, *Extension);
    return FPaths::Combine(ActiveOutputDirectory, FileName);
}

void UPanoramaCaptureComponent::BuildStereoViewMatrices(TArray<FMatrix>& OutLeft, TArray<FMatrix>& OutRight) const
{
    OutLeft.Reset();
    OutRight.Reset();

    const float EyeSeparation = 6.4f; // centimeters
    const FVector EyeOffset = FVector(EyeSeparation * 0.5f, 0.f, 0.f);

    for (int32 FaceIndex = 0; FaceIndex < FaceCaptures.Num(); ++FaceIndex)
    {
        const FTransform FaceTransform = FaceCaptures[FaceIndex]->GetComponentTransform();
        OutLeft.Add(FMatrix(FTranslationMatrix(-EyeOffset) * FaceTransform.ToMatrixNoScale()));
        OutRight.Add(FMatrix(FTranslationMatrix(EyeOffset) * FaceTransform.ToMatrixNoScale()));
    }
}

void UPanoramaCaptureComponent::OnCaptureComplete()
{
    CaptureStatus = EPanoramaCaptureStatus::Idle;
}
