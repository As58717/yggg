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
#include "JsonObjectConverter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/StringBuilder.h"
#include "Misc/LexicalConversion.h"
#include "Algo/Sort.h"

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

    bool RunFfmpeg(const FString& CommandLine, FString* OutFullCommandLine = nullptr)
    {
        const FString Executable = LocateFfmpegExecutable();
        if (Executable.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("FFmpeg executable not found. Skipping container packaging."));
            if (OutFullCommandLine)
            {
                *OutFullCommandLine = FString::Printf(TEXT("ffmpeg.exe%s"), *CommandLine);
            }
            return false;
        }

        if (OutFullCommandLine)
        {
            *OutFullCommandLine = FString::Printf(TEXT("\"%s\"%s"), *Executable, *CommandLine);
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

    constexpr const TCHAR* kRecoveryFileExtension = TEXT(".panrec.json");

    FString BuildMetadataArgs(
        const FPanoCaptureOutputSettings& OutputSettings,
        EPanoramaCaptureMode CaptureMode,
        const FPanoAudioCaptureSettings& AudioSettings,
        bool bIncludeAudio)
    {
        FString Metadata;
        if (OutputSettings.bInjectSphericalMetadata)
        {
            Metadata += TEXT(" -metadata:s:v:0 spherical_video=1 -metadata:s:v:0 projection=equirectangular");
        }

        if (CaptureMode == EPanoramaCaptureMode::Stereo && OutputSettings.bInjectStereoMetadata)
        {
            const TCHAR* StereoString = OutputSettings.StereoLayout == EPanoramaStereoLayout::SideBySide ? TEXT("left-right") : TEXT("top-bottom");
            Metadata += FString::Printf(TEXT(" -metadata:s:v:0 stereo_mode=%s"), StereoString);
        }

        if (bIncludeAudio)
        {
            Metadata += FString::Printf(TEXT(" -metadata:s:a:0 channel_layout=%s"), *AudioSettings.GetChannelLayoutName());
            if (AudioSettings.bEnableSpatialMetadata && OutputSettings.bInjectSpatialAudioMetadata)
            {
                Metadata += TEXT(" -metadata:s:a:0 spatial_audio=1");
                if (AudioSettings.ChannelLayout == EPanoramaAudioChannelLayout::FirstOrderAmbisonics)
                {
                    Metadata += TEXT(" -metadata:s:a:0 ambisonic_order=1");
                }
            }
        }

        for (const TPair<FString, FString>& Pair : OutputSettings.AdditionalMetadata)
        {
            Metadata += FString::Printf(TEXT(" -metadata %s=\"%s\""), *Pair.Key, *Pair.Value);
        }

        return Metadata;
    }

    struct FPanoRecoveryRecord
    {
        FString SessionName;
        FString OutputDirectory;
        EPanoramaCaptureOutputMode OutputMode = EPanoramaCaptureOutputMode::PNGSequence;
        FPanoCaptureOutputSettings OutputSettings;
        FPanoAudioCaptureSettings AudioSettings;
        EPanoramaCaptureCodec Codec = EPanoramaCaptureCodec::HEVC;
        EPanoramaCaptureMode CaptureMode = EPanoramaCaptureMode::Mono;
        FString BitstreamPath;
        FString SequencePattern;
        FString AudioPath;
        float FrameRate = 0.f;
        double LastTimecode = 0.0;
        uint64 LastFrameIndex = 0;
        bool bCompleted = false;
        bool bUse16BitPng = true;
        bool bUseLinearGammaForNVENC = false;
    };

    UEnum* GetOutputModeEnum()
    {
        static UEnum* Enum = StaticEnum<EPanoramaCaptureOutputMode>();
        return Enum;
    }

    UEnum* GetCodecEnum()
    {
        static UEnum* Enum = StaticEnum<EPanoramaCaptureCodec>();
        return Enum;
    }

    UEnum* GetCaptureModeEnum()
    {
        static UEnum* Enum = StaticEnum<EPanoramaCaptureMode>();
        return Enum;
    }

    FString EnumToString(UEnum* Enum, int64 Value)
    {
        return Enum ? Enum->GetNameStringByValue(Value) : FString();
    }

    template <typename EnumType>
    bool StringToEnum(const FString& InValue, EnumType& OutEnumValue)
    {
        if (UEnum* Enum = StaticEnum<EnumType>())
        {
            int64 Value = Enum->GetValueByNameString(InValue);
            if (Value != INDEX_NONE)
            {
                OutEnumValue = static_cast<EnumType>(Value);
                return true;
            }
        }
        return false;
    }

    bool WriteRecoveryRecord(const FString& FilePath, const FPanoRecoveryRecord& Record)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("SessionName"), Record.SessionName);
        Root->SetStringField(TEXT("OutputDirectory"), Record.OutputDirectory);
        Root->SetStringField(TEXT("OutputMode"), EnumToString(GetOutputModeEnum(), static_cast<int64>(Record.OutputMode)));
        Root->SetStringField(TEXT("Codec"), EnumToString(GetCodecEnum(), static_cast<int64>(Record.Codec)));
        Root->SetStringField(TEXT("CaptureMode"), EnumToString(GetCaptureModeEnum(), static_cast<int64>(Record.CaptureMode)));
        Root->SetStringField(TEXT("BitstreamPath"), Record.BitstreamPath);
        Root->SetStringField(TEXT("SequencePattern"), Record.SequencePattern);
        Root->SetStringField(TEXT("AudioPath"), Record.AudioPath);
        Root->SetNumberField(TEXT("FrameRate"), Record.FrameRate);
        Root->SetNumberField(TEXT("LastTimecode"), Record.LastTimecode);
        Root->SetStringField(TEXT("LastFrameIndex"), LexToString(Record.LastFrameIndex));
        Root->SetBoolField(TEXT("Completed"), Record.bCompleted);
        Root->SetBoolField(TEXT("Use16BitPng"), Record.bUse16BitPng);
        Root->SetBoolField(TEXT("UseLinearGammaNVENC"), Record.bUseLinearGammaForNVENC);

        TSharedRef<FJsonObject> OutputSettingsObject = MakeShared<FJsonObject>();
        FJsonObjectConverter::UStructToJsonObject(FPanoCaptureOutputSettings::StaticStruct(), &Record.OutputSettings, OutputSettingsObject, 0, 0);
        Root->SetObjectField(TEXT("OutputSettings"), OutputSettingsObject);

        TSharedRef<FJsonObject> AudioSettingsObject = MakeShared<FJsonObject>();
        FJsonObjectConverter::UStructToJsonObject(FPanoAudioCaptureSettings::StaticStruct(), &Record.AudioSettings, AudioSettingsObject, 0, 0);
        Root->SetObjectField(TEXT("AudioSettings"), AudioSettingsObject);

        FString Serialized;
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
        if (!FJsonSerializer::Serialize(Root, Writer))
        {
            return false;
        }

        return FFileHelper::SaveStringToFile(Serialized, *FilePath);
    }

    bool ReadRecoveryRecord(const FString& FilePath, FPanoRecoveryRecord& OutRecord)
    {
        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *FilePath))
        {
            return false;
        }

        TSharedPtr<FJsonObject> Root;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return false;
        }

        Root->TryGetStringField(TEXT("SessionName"), OutRecord.SessionName);
        Root->TryGetStringField(TEXT("OutputDirectory"), OutRecord.OutputDirectory);

        FString EnumString;
        if (Root->TryGetStringField(TEXT("OutputMode"), EnumString))
        {
            StringToEnum(EnumString, OutRecord.OutputMode);
        }
        if (Root->TryGetStringField(TEXT("Codec"), EnumString))
        {
            StringToEnum(EnumString, OutRecord.Codec);
        }
        if (Root->TryGetStringField(TEXT("CaptureMode"), EnumString))
        {
            StringToEnum(EnumString, OutRecord.CaptureMode);
        }

        Root->TryGetStringField(TEXT("BitstreamPath"), OutRecord.BitstreamPath);
        Root->TryGetStringField(TEXT("SequencePattern"), OutRecord.SequencePattern);
        Root->TryGetStringField(TEXT("AudioPath"), OutRecord.AudioPath);

        double FrameRateValue = 0.0;
        if (Root->TryGetNumberField(TEXT("FrameRate"), FrameRateValue))
        {
            OutRecord.FrameRate = static_cast<float>(FrameRateValue);
        }

        Root->TryGetNumberField(TEXT("LastTimecode"), OutRecord.LastTimecode);

        FString LastFrameIndexString;
        if (Root->TryGetStringField(TEXT("LastFrameIndex"), LastFrameIndexString))
        {
            LexFromString(OutRecord.LastFrameIndex, *LastFrameIndexString);
        }
        bool bBoolValue = false;
        if (Root->TryGetBoolField(TEXT("Completed"), bBoolValue))
        {
            OutRecord.bCompleted = bBoolValue;
        }
        if (Root->TryGetBoolField(TEXT("Use16BitPng"), bBoolValue))
        {
            OutRecord.bUse16BitPng = bBoolValue;
        }
        if (Root->TryGetBoolField(TEXT("UseLinearGammaNVENC"), bBoolValue))
        {
            OutRecord.bUseLinearGammaForNVENC = bBoolValue;
        }

        const TSharedPtr<FJsonObject>* OutputSettingsObject = nullptr;
        if (Root->TryGetObjectField(TEXT("OutputSettings"), OutputSettingsObject) && OutputSettingsObject)
        {
            FJsonObjectConverter::JsonObjectToUStruct(OutputSettingsObject->ToSharedRef(), FPanoCaptureOutputSettings::StaticStruct(), &OutRecord.OutputSettings, 0, 0);
        }

        const TSharedPtr<FJsonObject>* AudioSettingsObject = nullptr;
        if (Root->TryGetObjectField(TEXT("AudioSettings"), AudioSettingsObject) && AudioSettingsObject)
        {
            FJsonObjectConverter::JsonObjectToUStruct(AudioSettingsObject->ToSharedRef(), FPanoAudioCaptureSettings::StaticStruct(), &OutRecord.AudioSettings, 0, 0);
        }

        return true;
    }

    class FPanoRecordingRecoveryHandle
    {
    public:
        FPanoRecordingRecoveryHandle(const FString& InFilePath, float InHeartbeatSeconds)
            : FilePath(InFilePath)
            , HeartbeatSeconds(InHeartbeatSeconds)
            , LastWriteSeconds(0.0)
        {
        }

        bool Start(const FPanoRecoveryRecord& InRecord)
        {
            Record = InRecord;
            LastWriteSeconds = 0.0;
            return WriteRecoveryRecord(FilePath, Record);
        }

        void Update(uint64 FrameIndex, double Timecode)
        {
            Record.LastFrameIndex = FrameIndex;
            Record.LastTimecode = Timecode;
            const double Now = FPlatformTime::Seconds();
            if (LastWriteSeconds <= 0.0 || (Now - LastWriteSeconds) >= HeartbeatSeconds)
            {
                WriteRecoveryRecord(FilePath, Record);
                LastWriteSeconds = Now;
            }
        }

        void Complete()
        {
            Record.bCompleted = true;
            WriteRecoveryRecord(FilePath, Record);
            IFileManager::Get().Delete(*FilePath);
        }

        const FPanoRecoveryRecord& GetRecord() const
        {
            return Record;
        }

        const FString& GetFilePath() const
        {
            return FilePath;
        }

        void SetAudioPath(const FString& InPath)
        {
            Record.AudioPath = InPath;
            WriteRecoveryRecord(FilePath, Record);
        }

    private:
        FString FilePath;
        FPanoRecoveryRecord Record;
        float HeartbeatSeconds;
        double LastWriteSeconds;
    };

    void WriteFallbackScript(const FString& Directory, const FString& BaseName, const FString& FullCommand)
    {
        if (Directory.IsEmpty() || FullCommand.IsEmpty())
        {
            return;
        }

        FString SanitizedBase = FPaths::MakeValidFileName(BaseName);
        if (SanitizedBase.IsEmpty())
        {
            SanitizedBase = TEXT("PanoramaCapture");
        }

        const FString ScriptPath = FPaths::Combine(Directory, SanitizedBase + TEXT("_ffmpeg.bat"));
        FString Contents;
        Contents += TEXT("@echo off\r\n");
        Contents += FString::Printf(TEXT("REM Panorama Capture fallback for %s\r\n"), *SanitizedBase);
        Contents += FullCommand + TEXT("\r\n");
        Contents += TEXT("pause\r\n");
        if (FFileHelper::SaveStringToFile(Contents, *ScriptPath))
        {
            UE_LOG(LogTemp, Warning, TEXT("Generated FFmpeg fallback script at %s"), *ScriptPath);
        }
    }

    bool ExecuteContainerPackaging(
        const FString& VideoInput,
        const FString& AudioPath,
        float FrameRate,
        const FString& OutputDirectory,
        const FString& SessionName,
        const FString& ContainerExtension,
        EPanoramaCaptureCodec Codec,
        const FPanoCaptureOutputSettings& OutputSettings,
        EPanoramaCaptureMode CaptureMode,
        const FPanoAudioCaptureSettings& AudioSettings,
        bool bEmbedAudio,
        bool bCopyVideoStream,
        const FPanoNvencRateControl& RateControl,
        const FPanoSegmentedRecordingSettings& Segmentation,
        bool bAutoSyncAudio,
        bool bOverwriteExisting,
        FString& OutCommandLine,
        TArray<FString>& OutGeneratedFiles)
    {
        FString CommandLine;
        CommandLine += FString::Printf(TEXT(" -y -framerate %.3f -i \"%s\""), FrameRate, *VideoInput);

        const bool bHasAudio = bEmbedAudio && !AudioPath.IsEmpty() && FPaths::FileExists(AudioPath);
        if (bHasAudio)
        {
            CommandLine += FString::Printf(TEXT(" -i \"%s\""), *AudioPath);
            CommandLine += FString::Printf(TEXT(" -c:a aac -ar %d -ac %d"), AudioSettings.SampleRate, AudioSettings.GetChannelCount());
        }
        else
        {
            CommandLine += TEXT(" -an");
        }

        if (bCopyVideoStream)
        {
            CommandLine += TEXT(" -c:v copy");
            if (ContainerExtension.Equals(TEXT("mp4"), ESearchCase::IgnoreCase))
            {
                if (Codec == EPanoramaCaptureCodec::H264)
                {
                    CommandLine += TEXT(" -bsf:v h264_mp4toannexb");
                }
                else if (Codec == EPanoramaCaptureCodec::HEVC)
                {
                    CommandLine += TEXT(" -bsf:v hevc_mp4toannexb");
                }
            }
        }
        else
        {
            const FString CodecName = Codec == EPanoramaCaptureCodec::H264 ? TEXT("h264_nvenc") : TEXT("hevc_nvenc");
            CommandLine += FString::Printf(TEXT(" -c:v %s"), *CodecName);
            const FString RateMode = RateControl.bUseCBR ? TEXT("cbr") : TEXT("vbr");
            const int32 Bitrate = FMath::Max(1, FMath::RoundToInt(RateControl.BitrateMbps));
            CommandLine += FString::Printf(TEXT(" -rc:v %s -b:v %dM -g %d -bf %d"), *RateMode, Bitrate, RateControl.GOPLength, RateControl.NumBFrames);
            if (RateControl.bUseCBR)
            {
                CommandLine += FString::Printf(TEXT(" -minrate %dM -maxrate %dM"), Bitrate, Bitrate);
            }
            else
            {
                CommandLine += FString::Printf(TEXT(" -maxrate %dM"), Bitrate);
            }
            CommandLine += TEXT(" -pix_fmt yuv420p");
        }

        if (bAutoSyncAudio && bHasAudio)
        {
            CommandLine += TEXT(" -af \"aresample=async=1:first_pts=0\"");
        }

        CommandLine += BuildMetadataArgs(OutputSettings, CaptureMode, AudioSettings, bHasAudio);

        if (ContainerExtension.Equals(TEXT("mp4"), ESearchCase::IgnoreCase))
        {
            CommandLine += TEXT(" -movflags +faststart");
        }

        FString OutputPath;
        FString SegmentSearchPath;
        FString UniqueBasePath = FPaths::Combine(OutputDirectory, FString::Printf(TEXT("%s.%s"), *SessionName, *ContainerExtension));
        if (!bOverwriteExisting)
        {
            UniqueBasePath = MakeUniqueOutputPath(UniqueBasePath, false);
        }
        const FString UniqueBaseName = FPaths::GetBaseFilename(UniqueBasePath);

        if (Segmentation.bEnableSegmentation)
        {
            SegmentSearchPath = FPaths::Combine(OutputDirectory, FString::Printf(TEXT("%s_segment_*.%s"), *UniqueBaseName, *ContainerExtension));
            if (bOverwriteExisting)
            {
                TArray<FString> ExistingSegments;
                IFileManager::Get().FindFiles(ExistingSegments, *SegmentSearchPath, true, false);
                for (const FString& Existing : ExistingSegments)
                {
                    IFileManager::Get().Delete(*FPaths::Combine(OutputDirectory, Existing));
                }
            }
            OutputPath = FPaths::Combine(OutputDirectory, FString::Printf(TEXT("%s_segment_%%03d.%s"), *UniqueBaseName, *ContainerExtension));
            CommandLine += FString::Printf(TEXT(" -f segment -segment_time %.3f"), Segmentation.SegmentLengthSeconds);
            if (Segmentation.bResetTimestampsPerSegment)
            {
                CommandLine += TEXT(" -reset_timestamps 1");
            }
            CommandLine += FString::Printf(TEXT(" \"%s\""), *OutputPath);
        }
        else
        {
            OutputPath = UniqueBasePath;
            CommandLine += FString::Printf(TEXT(" \"%s\""), *OutputPath);
        }

        const bool bSuccess = RunFfmpeg(CommandLine, &OutCommandLine);

        OutGeneratedFiles.Reset();
        if (bSuccess)
        {
            if (Segmentation.bEnableSegmentation)
            {
                TArray<FString> FoundFiles;
                const FString EffectiveSearchPath = SegmentSearchPath.IsEmpty() ? FPaths::Combine(OutputDirectory, FString::Printf(TEXT("%s_segment_*.%s"), *UniqueBaseName, *ContainerExtension)) : SegmentSearchPath;
                IFileManager::Get().FindFiles(FoundFiles, *EffectiveSearchPath, true, false);
                FoundFiles.Sort();
                for (const FString& File : FoundFiles)
                {
                    OutGeneratedFiles.Add(FPaths::Combine(OutputDirectory, File));
                }
            }
            else
            {
                OutGeneratedFiles.Add(OutputPath);
            }
        }

        return bSuccess;
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
    , AudioSettings()
    , RecoverySettings()
    , bCollectPerformanceStats(true)
    , StatsUpdateInterval(1.0f)
    , PerformanceStats()
    , CaptureStatus(EPanoramaCaptureStatus::Idle)
    , TimeSinceLastCapture(0.f)
    , RecordingStartTime(0.0)
    , FrameRingBuffer(nullptr)
    , FrameIndex(0)
    , DroppedFrameCount(0)
    , LastStatsUpdateTime(0.0)
    , CaptureTimeAccumulator(0.0)
    , EncodeTimeAccumulator(0.0)
    , CaptureSamples(0)
    , EncodeSamples(0)
    , LastRecoveryUpdateTime(0.0)
    , LastRecordedAudioDuration(0.0)
    , TotalDataWrittenMB(0.0)
    , LastReportedEncodedFrames(0)
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = true;
    bAutoActivate = true;

    if (const UPanoramaCaptureSettings* Settings = GetDefault<UPanoramaCaptureSettings>())
    {
        CaptureMode = Settings->DefaultCaptureMode;
        OutputSettings = Settings->DefaultOutputSettings;
        AudioSettings = Settings->DefaultAudioSettings;
        bCollectPerformanceStats = Settings->bCollectPerformanceStats;
        StatsUpdateInterval = FMath::Max(0.1f, Settings->StatsUpdateInterval);
        RecoverySettings = Settings->DefaultRecoverySettings;

        if (Settings->bAutoApplyActiveProfile && Settings->ActiveProfileName != NAME_None)
        {
            if (const FPanoCaptureProfile* Profile = Settings->FindProfileByName(Settings->ActiveProfileName))
            {
                ApplyProfileInternal(*Profile);
            }
        }
    }

    PerformanceStats.Reset();
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

    if (bCollectPerformanceStats && CaptureStatus == EPanoramaCaptureStatus::Recording)
    {
        const double Now = FPlatformTime::Seconds();
        if (StatsUpdateInterval > 0.f && (Now - LastStatsUpdateTime) >= StatsUpdateInterval)
        {
            UpdateEncoderStats();
            UpdateAudioDrift();
            LastStatsUpdateTime = Now;
        }
    }
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
    FIntPoint EquirectResolution = BaseEquirectResolution;
    if (EyeCount == 2)
    {
        if (OutputSettings.StereoLayout == EPanoramaStereoLayout::SideBySide)
        {
            EquirectResolution.X *= EyeCount;
        }
        else
        {
            EquirectResolution.Y *= EyeCount;
        }
    }
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

    GeneratedContainerFiles.Reset();
    PendingFfmpegCommandLine.Reset();
    ResetPerformanceStats();
    TotalDataWrittenMB = 0.0;
    LastReportedEncodedFrames = 0;
    LastStatsUpdateTime = FPlatformTime::Seconds();

    AttemptAutoRecovery(ActiveOutputDirectory);

    InitializeCaptureFaces();

    FPanoRecoveryRecord RecoveryRecord;
    const bool bShouldWriteRecovery = RecoverySettings.bWriteRecoveryFile;
    if (bShouldWriteRecovery)
    {
        RecoveryFilePath = FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s%s"), *ActiveSessionName, *GetMetadataFileExtension()));
        RecoveryRecord.SessionName = ActiveSessionName;
        RecoveryRecord.OutputDirectory = ActiveOutputDirectory;
        RecoveryRecord.OutputMode = OutputSettings.OutputMode;
        RecoveryRecord.OutputSettings = OutputSettings;
        RecoveryRecord.AudioSettings = AudioSettings;
        RecoveryRecord.Codec = OutputSettings.Codec;
        RecoveryRecord.CaptureMode = CaptureMode;
        RecoveryRecord.FrameRate = CaptureFrameRate;
        RecoveryRecord.bUse16BitPng = bUse16BitPng;
        RecoveryRecord.bUseLinearGammaForNVENC = bUseLinearGammaForNVENC;
        if (OutputSettings.OutputMode == EPanoramaCaptureOutputMode::PNGSequence)
        {
            RecoveryRecord.SequencePattern = FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s_%%06d.png"), *ActiveSessionName));
        }
        RecoveryRecord.AudioPath = FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s.wav"), *ActiveSessionName));
        RecoveryHandle = MakeUnique<FPanoRecordingRecoveryHandle>(RecoveryFilePath, FMath::Max(1.f, RecoverySettings.HeartbeatIntervalSeconds));
    }

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

        if (bShouldWriteRecovery)
        {
            RecoveryRecord.BitstreamPath = EncodeParams.OutputBitstreamPath;
        }

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

    if (bShouldWriteRecovery && RecoveryHandle)
    {
        RecoveryHandle->Start(RecoveryRecord);
        LastRecoveryUpdateTime = FPlatformTime::Seconds();
    }

    AudioRecorder = MakeUnique<FPanoAudioRecorder>();

    USoundSubmixBase* TargetSubmix = OverrideAudioSubmix;
    if (!TargetSubmix)
    {
        TargetSubmix = GetDefault<UPanoramaCaptureSettings>()->TargetSubmix;
    }

    if (TargetSubmix)
    {
        AudioRecorder->StartRecording(TargetSubmix, AudioSettings.SampleRate, AudioSettings.GetChannelCount());
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

    RecoveryHandle.Reset();

    DestroyRenderTargets();
    GeneratedContainerFiles.Reset();
}

void UPanoramaCaptureComponent::EnqueueFrameCapture(float DeltaTime)
{
    if (FaceCaptures.Num() != kCubemapFaceCount)
    {
        InitializeCaptureFaces();
    }
    const double CaptureStartTime = FPlatformTime::Seconds();
    double FrameBytesMb = 0.0;
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

        FrameBytesMb = static_cast<double>(Frame.PixelData.Num()) / (1024.0 * 1024.0);

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

    const double CaptureDurationMs = (FPlatformTime::Seconds() - CaptureStartTime) * 1000.0;
    RefreshPerformanceStats(CaptureDurationMs, FrameBytesMb);
    UpdateRecoveryRecord(FrameIndex, Timecode);
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
    const int32 FullHeight = EquirectRenderTarget->SizeY;
    const int32 PerEyeWidth = (EyeCount == 2 && OutputSettings.StereoLayout == EPanoramaStereoLayout::SideBySide) ? FullWidth / FMath::Max(1, EyeCount) : FullWidth;
    const int32 PerEyeHeight = (EyeCount == 2 && OutputSettings.StereoLayout == EPanoramaStereoLayout::OverUnder) ? FullHeight / FMath::Max(1, EyeCount) : FullHeight;

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
        [FaceRenderTargets = FaceRenderTargets, ViewMatrices, OutputTexture, bLinearOutput, EyeIndex, EyeCount, FullWidth, FullHeight, PerEyeWidth, PerEyeHeight, StereoLayout = OutputSettings.StereoLayout](FRHICommandListImmediate& RHICmdList)
        {
            FRDGBuilder GraphBuilder(RHICmdList);

            FPanoCubemapToEquirectCS::FParameters* Parameters = GraphBuilder.AllocParameters<FPanoCubemapToEquirectCS::FParameters>();
            Parameters->OutputResolution = FVector2f(PerEyeWidth, PerEyeHeight);
            Parameters->InvOutputResolution = FVector2f(1.0f / PerEyeWidth, 1.0f / PerEyeHeight);
            Parameters->FullResolution = FVector2f(FullWidth, FullHeight);
            FVector2f OutputOffset(0.f, 0.f);
            if (EyeCount == 2)
            {
                if (StereoLayout == EPanoramaStereoLayout::SideBySide)
                {
                    OutputOffset.X = EyeIndex * PerEyeWidth;
                }
                else
                {
                    OutputOffset.Y = EyeIndex * PerEyeHeight;
                }
            }
            Parameters->OutputOffset = OutputOffset;
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
                FMath::DivideAndRoundUp(PerEyeWidth, 8),
                FMath::DivideAndRoundUp(PerEyeHeight, 8),
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
    PerformanceStats.TotalFramesDropped = DroppedFrameCount;
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

    FString WavAudioPath;
    FString FinalAudioPath;
    if (AudioRecorder)
    {
        double AudioDuration = 0.0;
        WavAudioPath = FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s.wav"), *ActiveSessionName));
        AudioRecorder->StopRecording();
        if (AudioRecorder->WriteToWav(WavAudioPath, AudioDuration))
        {
            LastRecordedAudioDuration = AudioDuration;
            FinalAudioPath = WavAudioPath;
        }
        AudioRecorder.Reset();
    }

    if (!FinalAudioPath.IsEmpty() && AudioSettings.Format == EPanoramaAudioFormat::Ogg)
    {
        const FString OggPath = FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s.ogg"), *ActiveSessionName));
        FString ConversionCommand;
        const FString Command = FString::Printf(TEXT(" -y -i \"%s\" -c:a libvorbis \"%s\""), *FinalAudioPath, *OggPath);
        const bool bConverted = RunFfmpeg(Command, &ConversionCommand);
        EmitFallbackScriptIfNeeded(ActiveSessionName + TEXT("_audio"), ConversionCommand, bConverted);
        if (bConverted)
        {
            FinalAudioPath = OggPath;
        }
    }

    RecordedAudioPath = FinalAudioPath;
    if (RecoveryHandle)
    {
        RecoveryHandle->SetAudioPath(RecordedAudioPath);
    }

    const UPanoramaCaptureSettings* Settings = GetDefault<UPanoramaCaptureSettings>();
    const bool bEmbedAudio = Settings ? Settings->bEmbedAudioInContainer : true;
    const bool bOverwriteExisting = Settings ? Settings->bOverwriteExisting : false;
    const bool bGenerateMkv = Settings ? Settings->bGenerateMKV : true;

    GeneratedContainerFiles.Reset();

    if (OutputSettings.OutputMode == EPanoramaCaptureOutputMode::PNGSequence && PngWriter)
    {
        PngWriter->Flush();
        const FString SequencePattern = FPaths::Combine(ActiveOutputDirectory, FString::Printf(TEXT("%s_%%06d.png"), *ActiveSessionName));

        FString CommandLine;
        TArray<FString> GeneratedFiles;
        const bool bMp4Success = ExecuteContainerPackaging(
            SequencePattern,
            bEmbedAudio ? FinalAudioPath : FString(),
            CaptureFrameRate,
            ActiveOutputDirectory,
            ActiveSessionName,
            TEXT("mp4"),
            OutputSettings.Codec,
            OutputSettings,
            CaptureMode,
            AudioSettings,
            bEmbedAudio,
            false,
            OutputSettings.NvencRateControl,
            OutputSettings.Segmentation,
            AudioSettings.bAutoSyncCorrection,
            bOverwriteExisting,
            CommandLine,
            GeneratedFiles);
        GeneratedContainerFiles.Append(GeneratedFiles);
        EmitFallbackScriptIfNeeded(ActiveSessionName + TEXT("_mp4"), CommandLine, bMp4Success);
        if (bMp4Success)
        {
            UE_LOG(LogTemp, Log, TEXT("Panorama capture packaged to MP4 (%d file(s))."), GeneratedFiles.Num());
            GenerateSegmentManifestIfNeeded(TEXT("mp4"));
        }

        if (bGenerateMkv)
        {
            GeneratedFiles.Reset();
            CommandLine.Reset();
            const bool bMkvSuccess = ExecuteContainerPackaging(
                SequencePattern,
                bEmbedAudio ? FinalAudioPath : FString(),
                CaptureFrameRate,
                ActiveOutputDirectory,
                ActiveSessionName,
                TEXT("mkv"),
                OutputSettings.Codec,
                OutputSettings,
                CaptureMode,
                AudioSettings,
                bEmbedAudio,
                false,
                OutputSettings.NvencRateControl,
                OutputSettings.Segmentation,
                AudioSettings.bAutoSyncCorrection,
                bOverwriteExisting,
                CommandLine,
                GeneratedFiles);
            GeneratedContainerFiles.Append(GeneratedFiles);
            EmitFallbackScriptIfNeeded(ActiveSessionName + TEXT("_mkv"), CommandLine, bMkvSuccess);
            if (bMkvSuccess)
            {
                UE_LOG(LogTemp, Log, TEXT("Panorama capture packaged to MKV (%d file(s))."), GeneratedFiles.Num());
                GenerateSegmentManifestIfNeeded(TEXT("mkv"));
            }
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
            FString CommandLine;
            TArray<FString> GeneratedFiles;
            const bool bMp4Success = ExecuteContainerPackaging(
                BitstreamPath,
                bEmbedAudio ? FinalAudioPath : FString(),
                CaptureFrameRate,
                ActiveOutputDirectory,
                ActiveSessionName,
                TEXT("mp4"),
                OutputSettings.Codec,
                OutputSettings,
                CaptureMode,
                AudioSettings,
                bEmbedAudio,
                true,
                OutputSettings.NvencRateControl,
                OutputSettings.Segmentation,
                AudioSettings.bAutoSyncCorrection,
                bOverwriteExisting,
                CommandLine,
                GeneratedFiles);
            GeneratedContainerFiles.Append(GeneratedFiles);
            EmitFallbackScriptIfNeeded(ActiveSessionName + TEXT("_mp4"), CommandLine, bMp4Success);
            if (bMp4Success)
            {
                UE_LOG(LogTemp, Log, TEXT("NVENC bitstream packaged to MP4 (%d file(s))."), GeneratedFiles.Num());
                GenerateSegmentManifestIfNeeded(TEXT("mp4"));
            }

            if (bGenerateMkv)
            {
                GeneratedFiles.Reset();
                CommandLine.Reset();
                const bool bMkvSuccess = ExecuteContainerPackaging(
                    BitstreamPath,
                    bEmbedAudio ? FinalAudioPath : FString(),
                    CaptureFrameRate,
                    ActiveOutputDirectory,
                    ActiveSessionName,
                    TEXT("mkv"),
                    OutputSettings.Codec,
                    OutputSettings,
                    CaptureMode,
                    AudioSettings,
                    bEmbedAudio,
                    true,
                    OutputSettings.NvencRateControl,
                    OutputSettings.Segmentation,
                    AudioSettings.bAutoSyncCorrection,
                    bOverwriteExisting,
                    CommandLine,
                    GeneratedFiles);
                GeneratedContainerFiles.Append(GeneratedFiles);
                EmitFallbackScriptIfNeeded(ActiveSessionName + TEXT("_mkv"), CommandLine, bMkvSuccess);
                if (bMkvSuccess)
                {
                    UE_LOG(LogTemp, Log, TEXT("NVENC bitstream packaged to MKV (%d file(s))."), GeneratedFiles.Num());
                    GenerateSegmentManifestIfNeeded(TEXT("mkv"));
                }
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("NVENC bitstream not found for session %s."), *ActiveSessionName);
        }
    }
#endif

    if (RecoveryHandle)
    {
        RecoveryHandle->Complete();
        RecoveryHandle.Reset();
    }

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

void UPanoramaCaptureComponent::ApplyProfileInternal(const FPanoCaptureProfile& Profile)
{
    CaptureMode = Profile.CaptureMode;
    OutputSettings = Profile.OutputSettings;
    CaptureFrameRate = Profile.FrameRate;
    bEnablePreview = Profile.bEnablePreview;
    AudioSettings = Profile.AudioSettings;
}

void UPanoramaCaptureComponent::ApplyProfileByName(FName ProfileName)
{
    if (ProfileName.IsNone())
    {
        return;
    }

    if (const UPanoramaCaptureSettings* Settings = GetDefault<UPanoramaCaptureSettings>())
    {
        if (const FPanoCaptureProfile* Profile = Settings->FindProfileByName(ProfileName))
        {
            ApplyProfileInternal(*Profile);
        }
    }
}

void UPanoramaCaptureComponent::AttemptAutoRecovery(const FString& Directory)
{
    if (!RecoverySettings.bWriteRecoveryFile || !RecoverySettings.bAutoRecoverOnBegin)
    {
        return;
    }
    RecoverIncompleteRecordings(Directory);
}

void UPanoramaCaptureComponent::UpdateRecoveryRecord(uint64 InFrameIndex, double Timecode)
{
    if (RecoveryHandle && RecoverySettings.bWriteRecoveryFile)
    {
        RecoveryHandle->Update(InFrameIndex, Timecode);
    }
}

void UPanoramaCaptureComponent::RefreshPerformanceStats(double CaptureTimeMs, double FrameBytesMb)
{
    if (!bCollectPerformanceStats)
    {
        return;
    }

    ++PerformanceStats.TotalFramesCaptured;
    CaptureTimeAccumulator += CaptureTimeMs;
    ++CaptureSamples;
    PerformanceStats.AverageCaptureTimeMs = CaptureSamples > 0 ? static_cast<float>(CaptureTimeAccumulator / CaptureSamples) : 0.f;
    PerformanceStats.MaxCaptureTimeMs = FMath::Max(PerformanceStats.MaxCaptureTimeMs, static_cast<float>(CaptureTimeMs));

    if (FrameBytesMb > 0.0)
    {
        TotalDataWrittenMB += FrameBytesMb;
        PerformanceStats.TotalDataWrittenMB = static_cast<float>(TotalDataWrittenMB);
    }
}

void UPanoramaCaptureComponent::UpdateEncoderStats()
{
    if (!bCollectPerformanceStats || !NvencEncoder)
    {
        return;
    }

    FPanoNvencEncoderLifetimeStats Stats;
    NvencEncoder->GetLifetimeStats(Stats);
    if (Stats.FramesEncoded == 0)
    {
        return;
    }

    const uint64 DeltaFrames = Stats.FramesEncoded > LastReportedEncodedFrames ? (Stats.FramesEncoded - LastReportedEncodedFrames) : 0;
    const double DeltaEncodeTime = Stats.TotalEncodeTimeMs - EncodeTimeAccumulator;

    PerformanceStats.MaxEncodeTimeMs = FMath::Max(PerformanceStats.MaxEncodeTimeMs, static_cast<float>(Stats.MaxEncodeTimeMs));

    if (DeltaFrames > 0 && DeltaEncodeTime > 0.0)
    {
        PerformanceStats.AverageEncodeTimeMs = static_cast<float>(DeltaEncodeTime / static_cast<double>(DeltaFrames));
    }
    else
    {
        PerformanceStats.AverageEncodeTimeMs = static_cast<float>(Stats.TotalEncodeTimeMs / FMath::Max<uint64>(Stats.FramesEncoded, 1));
    }

    if (Stats.TotalEncodedBytes > 0)
    {
        TotalDataWrittenMB = static_cast<double>(Stats.TotalEncodedBytes) / (1024.0 * 1024.0);
        PerformanceStats.TotalDataWrittenMB = static_cast<float>(TotalDataWrittenMB);
    }

    LastReportedEncodedFrames = Stats.FramesEncoded;
    EncodeTimeAccumulator = Stats.TotalEncodeTimeMs;
}

void UPanoramaCaptureComponent::UpdateAudioDrift()
{
    if (!bCollectPerformanceStats || !AudioRecorder)
    {
        return;
    }

    const double CaptureDuration = FPlatformTime::Seconds() - RecordingStartTime;
    const double DriftSeconds = AudioRecorder->GetEstimatedDriftSeconds(CaptureDuration);
    LastRecordedAudioDuration = AudioRecorder->GetAccumulatedDurationSeconds();
    PerformanceStats.AudioDriftMs = static_cast<float>(DriftSeconds * 1000.0);
}

void UPanoramaCaptureComponent::ResetPerformanceStats()
{
    PerformanceStats.Reset();
    CaptureTimeAccumulator = 0.0;
    EncodeTimeAccumulator = 0.0;
    CaptureSamples = 0;
    EncodeSamples = 0;
    TotalDataWrittenMB = 0.0;
    LastReportedEncodedFrames = 0;
}

void UPanoramaCaptureComponent::RecoverIncompleteRecordings(const FString& Directory)
{
    FString SearchDirectory = Directory;
    if (SearchDirectory.IsEmpty())
    {
        SearchDirectory = FPaths::ProjectSavedDir();
    }

    TArray<FString> RecoveryFiles;
    IFileManager::Get().FindFilesRecursive(RecoveryFiles, *SearchDirectory, TEXT("*.panrec.json"), true, false);
    if (RecoveryFiles.Num() == 0)
    {
        return;
    }

    const UPanoramaCaptureSettings* Settings = GetDefault<UPanoramaCaptureSettings>();
    const bool bEmbedAudio = Settings ? Settings->bEmbedAudioInContainer : true;
    const bool bGenerateMkv = Settings ? Settings->bGenerateMKV : true;
    const bool bOverwriteExisting = Settings ? Settings->bOverwriteExisting : false;

    for (const FString& FilePath : RecoveryFiles)
    {
        FPanoRecoveryRecord Record;
        if (!ReadRecoveryRecord(FilePath, Record))
        {
            continue;
        }

        if (Record.bCompleted)
        {
            IFileManager::Get().Delete(*FilePath);
            continue;
        }

        FString VideoInput;
        bool bCopyVideoStream = false;
        if (Record.OutputMode == EPanoramaCaptureOutputMode::PNGSequence && !Record.SequencePattern.IsEmpty())
        {
            VideoInput = Record.SequencePattern;
        }
        else if (Record.OutputMode == EPanoramaCaptureOutputMode::NVENC && !Record.BitstreamPath.IsEmpty())
        {
            VideoInput = Record.BitstreamPath;
            bCopyVideoStream = true;
        }
        else
        {
            continue;
        }

        FString CommandLine;
        TArray<FString> GeneratedFiles;
        const bool bMp4Success = ExecuteContainerPackaging(
            VideoInput,
            bEmbedAudio ? Record.AudioPath : FString(),
            Record.FrameRate,
            Record.OutputDirectory,
            Record.SessionName,
            TEXT("mp4"),
            Record.Codec,
            Record.OutputSettings,
            Record.CaptureMode,
            Record.AudioSettings,
            bEmbedAudio,
            bCopyVideoStream,
            Record.OutputSettings.NvencRateControl,
            Record.OutputSettings.Segmentation,
            Record.AudioSettings.bAutoSyncCorrection,
            bOverwriteExisting,
            CommandLine,
            GeneratedFiles);

        if (!bMp4Success && !CommandLine.IsEmpty())
        {
            WriteFallbackScript(Record.OutputDirectory, Record.SessionName + TEXT("_mp4"), CommandLine);
        }

        if (bGenerateMkv)
        {
            GeneratedFiles.Reset();
            CommandLine.Reset();
            const bool bMkvSuccess = ExecuteContainerPackaging(
                VideoInput,
                bEmbedAudio ? Record.AudioPath : FString(),
                Record.FrameRate,
                Record.OutputDirectory,
                Record.SessionName,
                TEXT("mkv"),
                Record.Codec,
                Record.OutputSettings,
                Record.CaptureMode,
                Record.AudioSettings,
                bEmbedAudio,
                bCopyVideoStream,
                Record.OutputSettings.NvencRateControl,
                Record.OutputSettings.Segmentation,
                Record.AudioSettings.bAutoSyncCorrection,
                bOverwriteExisting,
                CommandLine,
                GeneratedFiles);

            if (!bMkvSuccess && !CommandLine.IsEmpty())
            {
                WriteFallbackScript(Record.OutputDirectory, Record.SessionName + TEXT("_mkv"), CommandLine);
            }
        }

        IFileManager::Get().Delete(*FilePath);
    }
}

FString UPanoramaCaptureComponent::GetMetadataFileExtension() const
{
    return FString(kRecoveryFileExtension);
}

void UPanoramaCaptureComponent::EmitFallbackScriptIfNeeded(const FString& BaseName, const FString& CommandLine, bool bCommandSucceeded)
{
    if (bCommandSucceeded || !OutputSettings.bAllowFfmpegFallbackScript)
    {
        return;
    }

    WriteFallbackScript(ActiveOutputDirectory, BaseName, CommandLine);
}

void UPanoramaCaptureComponent::GenerateSegmentManifestIfNeeded(const FString& Extension)
{
    if (!OutputSettings.Segmentation.bEnableSegmentation || !OutputSettings.Segmentation.bGenerateSegmentManifest)
    {
        return;
    }

    TArray<FString> SegmentFiles;
    const FString ExtensionToken = FString::Printf(TEXT(".%s"), *Extension);
    for (const FString& File : GeneratedContainerFiles)
    {
        if (File.EndsWith(ExtensionToken, ESearchCase::IgnoreCase))
        {
            SegmentFiles.Add(FPaths::GetCleanFilename(File));
        }
    }

    if (SegmentFiles.Num() == 0)
    {
        return;
    }

    SegmentFiles.Sort();

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> SegmentsArray;
    for (const FString& Segment : SegmentFiles)
    {
        SegmentsArray.Add(MakeShared<FJsonValueString>(Segment));
    }

    Root->SetArrayField(TEXT("segments"), SegmentsArray);

    FString LowerExt = Extension;
    LowerExt.ToLowerInline();
    const FString ManifestName = FString::Printf(TEXT("%s_%s_segments.json"), *ActiveSessionName, *LowerExt);
    FString Serialized;
    TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
    if (FJsonSerializer::Serialize(Root, Writer))
    {
        const FString ManifestPath = FPaths::Combine(ActiveOutputDirectory, ManifestName);
        FFileHelper::SaveStringToFile(Serialized, *ManifestPath);
    }
}
