#include "PanoramaNvencEncoder.h"

#include "PanoramaCaptureModule.h"
#include "RHI.h"
#include "RHIResources.h"
#include "RenderResource.h"
#include "RenderGraphUtils.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "HAL/FileManager.h"
#include "Serialization/Archive.h"

#if PANORAMA_CAPTURE_WITH_NVENC
#include "VideoEncoder.h"
#include "VideoEncoderInput.h"
#include "VideoEncoderFactory.h"
using namespace AVEncoder;
#endif

FPanoNvencEncoder::FPanoNvencEncoder()
    : bInitialized(false)
#if PANORAMA_CAPTURE_WITH_NVENC
    , NvEncHandle(nullptr)
    , NvEncInterface(nullptr)
    , bUsingD3D12(false)
#endif
{
}

FPanoNvencEncoder::~FPanoNvencEncoder()
{
    Shutdown();
}

bool FPanoNvencEncoder::Initialize(const FPanoramaNvencEncodeParams& Params)
{
    ActiveParams = Params;
    PendingFrames.Reset();

#if PANORAMA_CAPTURE_WITH_NVENC
    if (!FPanoramaCaptureModule::IsNvencAvailable())
    {
        UE_LOG(LogTemp, Warning, TEXT("NVENC runtime is unavailable."));
        return false;
    }

    const ERHIInterfaceType InterfaceType = RHIGetInterfaceType();
    bUsingD3D12 = InterfaceType == ERHIInterfaceType::D3D12;

    if (InterfaceType != ERHIInterfaceType::D3D11 && InterfaceType != ERHIInterfaceType::D3D12)
    {
        UE_LOG(LogTemp, Warning, TEXT("NVENC only supports D3D11/D3D12. Current RHI is unsupported."));
        return false;
    }

    if (Params.OutputBitstreamPath.IsEmpty())
    {
        return false;
    }

    IFileManager::Get().Delete(*Params.OutputBitstreamPath);

    BitstreamWriter.Reset(IFileManager::Get().CreateFileWriter(*Params.OutputBitstreamPath));
    if (!BitstreamWriter)
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to create NVENC bitstream output '%s'. Falling back to in-memory buffering."), *Params.OutputBitstreamPath);
    }

    bInitialized = true;
    return true;
#else
    UE_LOG(LogTemp, Warning, TEXT("PANORAMA_CAPTURE_WITH_NVENC is disabled for this build."));
    return false;
#endif
}

void FPanoNvencEncoder::Shutdown()
{
#if PANORAMA_CAPTURE_WITH_NVENC
    PendingFrames.Reset();
    ReleaseResources();
#endif
    if (BitstreamWriter)
    {
        FScopeLock Lock(&BitstreamWriterGuard);
        BitstreamWriter->Flush();
        BitstreamWriter.Reset();
    }
    bInitialized = false;
}

bool FPanoNvencEncoder::EnqueueResource(FTextureRHIRef Texture, uint64 InFrameIndex, double Timecode)
{
#if PANORAMA_CAPTURE_WITH_NVENC
    if (!bInitialized || !Texture.IsValid())
    {
        return false;
    }

    ENQUEUE_RENDER_COMMAND(PanoCapture_EncodeFrame)(
        [this, Texture, InFrameIndex, Timecode](FRHICommandListImmediate& RHICmdList)
        {
            EncodeFrame_RenderThread(Texture, InFrameIndex, Timecode);
        });
    return true;
#else
    return false;
#endif
}

void FPanoNvencEncoder::Flush(TArray<FPanoramaEncodedFrame>& OutFrames)
{
#if PANORAMA_CAPTURE_WITH_NVENC
    FScopeLock Lock(&PendingFramesGuard);
    OutFrames = PendingFrames;
    PendingFrames.Reset();
    if (BitstreamWriter)
    {
        FScopeLock WriterLock(&BitstreamWriterGuard);
        BitstreamWriter->Flush();
    }
#else
    OutFrames.Reset();
#endif
}

#if PANORAMA_CAPTURE_WITH_NVENC
void FPanoNvencEncoder::ReleaseResources()
{
    PendingFrames.Reset();
    RegisteredResources.Reset();
    if (BitstreamWriter)
    {
        FScopeLock WriterLock(&BitstreamWriterGuard);
        BitstreamWriter->Flush();
    }
}

bool FPanoNvencEncoder::EncodeFrame_RenderThread(FTextureRHIRef Texture, uint64 InFrameIndex, double Timecode)
{
    if (!Texture.IsValid())
    {
        return false;
    }

    FRHITexture* TextureRHI = Texture.GetReference();
    if (!TextureRHI)
    {
        return false;
    }

    // Use AVEncoder abstraction to interface with NVENC without explicit SDK dependency.
    FVideoEncoderInput::Parameters InputParams;
    InputParams.Width = TextureRHI->GetDesc().Extent.X;
    InputParams.Height = TextureRHI->GetDesc().Extent.Y;
    InputParams.PixelFormat = EPixelFormat::PF_B8G8R8A8;
    InputParams.NumBuffers = 1;

    static TSharedPtr<FVideoEncoderInput> EncoderInput;
    static TUniquePtr<FVideoEncoder> Encoder;

    if (!EncoderInput.IsValid())
    {
        EncoderInput = FVideoEncoderInput::Create(InputParams, TEXT("PanoramaNvencInput"));
        if (!EncoderInput.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to create NVENC input."));
            return false;
        }

        FVideoEncoder::FLayerConfig LayerConfig;
        LayerConfig.Width = InputParams.Width;
        LayerConfig.Height = InputParams.Height;
        LayerConfig.FrameRate = ActiveParams.FrameRate > 0.f ? static_cast<uint32>(FMath::RoundToInt(ActiveParams.FrameRate)) : 60;
        LayerConfig.MaxBitrate = static_cast<uint32>(ActiveParams.RateControl.BitrateMbps * 1000000.0f);
        LayerConfig.TargetBitrate = LayerConfig.MaxBitrate;
        LayerConfig.GOPLength = ActiveParams.RateControl.GOPLength;
        LayerConfig.NumBFrames = ActiveParams.RateControl.NumBFrames;
        LayerConfig.MinQP = 0;
        LayerConfig.MaxQP = 51;

        FVideoEncoder::FInitConfig InitConfig;
        InitConfig.Codec = ActiveParams.Codec == EPanoramaCaptureCodec::H264 ? EVideoEncoderCodec::H264 : EVideoEncoderCodec::HEVC;
        InitConfig.LatencyMode = FVideoEncoder::ELatencyMode::LowLatency;
        InitConfig.bEnableTemporalSVC = false;

        static const FName NvencName(TEXT("NVENC"));
        Encoder = FVideoEncoderFactory::Get().CreateVideoEncoder(NvencName, LayerConfig, InitConfig, EncoderInput.ToSharedRef());
        if (!Encoder.IsValid())
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to create NVENC encoder."));
            return false;
        }

        Encoder->SetOnEncodedImageReady(FVideoEncoder::FOnEncodedImageReady::CreateLambda([
            this
        ](const FVideoEncoder::FEncodedImage& EncodedImage)
        {
            FPanoramaEncodedFrame EncodedFrame;
            EncodedFrame.FrameIndex = EncodedImage.FrameId;
            EncodedFrame.Timecode = EncodedImage.Timestamp;
            EncodedFrame.EncodedBytes = EncodedImage.Data;
            FScopeLock Lock(&PendingFramesGuard);
            PendingFrames.Add(MoveTemp(EncodedFrame));
            if (BitstreamWriter && EncodedImage.Data.Num() > 0)
            {
                FScopeLock WriterLock(&BitstreamWriterGuard);
                if (BitstreamWriter)
                {
                    BitstreamWriter->Serialize(const_cast<uint8*>(EncodedImage.Data.GetData()), EncodedImage.Data.Num());
                }
            }
        }));
    }

    if (!EncoderInput.IsValid())
    {
        return false;
    }

    TSharedPtr<FVideoEncoderInputFrame> InputFrame = EncoderInput->ObtainInputFrame();
    if (!InputFrame.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("NVENC input frame unavailable."));
        return false;
    }

    InputFrame->SetRHITexture(Texture); // zero-copy
    InputFrame->SetTimestamp(Timecode);
    InputFrame->SetFrameId(static_cast<uint32>(InFrameIndex));

    Encoder->Encode(InputFrame.ToSharedRef());

    return true;
}
#endif
