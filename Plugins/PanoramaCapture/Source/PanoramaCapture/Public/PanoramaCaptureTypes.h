#pragma once

#include "CoreMinimal.h"
#include "PanoramaCaptureTypes.generated.h"

UENUM(BlueprintType)
enum class EPanoramaCaptureMode : uint8
{
    Mono,
    Stereo
};

UENUM(BlueprintType)
enum class EPanoramaStereoLayout : uint8
{
    OverUnder UMETA(DisplayName = "Over/Under (Top-Bottom)"),
    SideBySide UMETA(DisplayName = "Side-by-Side")
};

UENUM(BlueprintType)
enum class EPanoramaCaptureOutputMode : uint8
{
    PNGSequence,
    NVENC
};

UENUM(BlueprintType)
enum class EPanoramaCaptureCodec : uint8
{
    H264,
    HEVC
};

UENUM(BlueprintType)
enum class EPanoramaCaptureStatus : uint8
{
    Idle,
    Recording,
    Finalizing,
    DroppedFrames
};

USTRUCT(BlueprintType)
struct FPanoCaptureResolution
{
    GENERATED_BODY()

    FPanoCaptureResolution()
        : Width(4096)
        , Height(2048)
    {
    }

    FPanoCaptureResolution(int32 InWidth, int32 InHeight)
        : Width(InWidth)
        , Height(InHeight)
    {
    }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    int32 Width;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    int32 Height;
};

USTRUCT(BlueprintType)
struct FPanoNvencRateControl
{
    GENERATED_BODY()

    FPanoNvencRateControl()
        : BitrateMbps(60.f)
        , bUseCBR(true)
        , GOPLength(60)
        , NumBFrames(2)
    {
    }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    float BitrateMbps;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bUseCBR;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    int32 GOPLength;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    int32 NumBFrames;
};

UENUM(BlueprintType)
enum class EPanoramaAudioFormat : uint8
{
    Wav,
    Ogg
};

UENUM(BlueprintType)
enum class EPanoramaAudioChannelLayout : uint8
{
    Mono,
    Stereo,
    Quad,
    FivePointOne,
    FirstOrderAmbisonics
};

USTRUCT(BlueprintType)
struct FPanoAudioCaptureSettings
{
    GENERATED_BODY()

    FPanoAudioCaptureSettings()
        : SampleRate(48000)
        , Format(EPanoramaAudioFormat::Wav)
        , ChannelLayout(EPanoramaAudioChannelLayout::Stereo)
        , bEnableSpatialMetadata(true)
        , bAutoSyncCorrection(true)
        , SyncDriftThresholdMs(45.f)
    {
    }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    int32 SampleRate;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    EPanoramaAudioFormat Format;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    EPanoramaAudioChannelLayout ChannelLayout;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bEnableSpatialMetadata;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bAutoSyncCorrection;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama", meta = (EditCondition = "bAutoSyncCorrection", ClampMin = "0.0"))
    float SyncDriftThresholdMs;

    int32 GetChannelCount() const
    {
        switch (ChannelLayout)
        {
        case EPanoramaAudioChannelLayout::Mono:
            return 1;
        case EPanoramaAudioChannelLayout::Stereo:
            return 2;
        case EPanoramaAudioChannelLayout::Quad:
            return 4;
        case EPanoramaAudioChannelLayout::FivePointOne:
            return 6;
        case EPanoramaAudioChannelLayout::FirstOrderAmbisonics:
            return 4;
        default:
            break;
        }
        return 2;
    }

    FString GetChannelLayoutName() const
    {
        switch (ChannelLayout)
        {
        case EPanoramaAudioChannelLayout::Mono:
            return TEXT("mono");
        case EPanoramaAudioChannelLayout::Stereo:
            return TEXT("stereo");
        case EPanoramaAudioChannelLayout::Quad:
            return TEXT("quad");
        case EPanoramaAudioChannelLayout::FivePointOne:
            return TEXT("5.1");
        case EPanoramaAudioChannelLayout::FirstOrderAmbisonics:
            return TEXT("first_order_ambisonics");
        default:
            break;
        }
        return TEXT("stereo");
    }
};

USTRUCT(BlueprintType)
struct FPanoSegmentedRecordingSettings
{
    GENERATED_BODY()

    FPanoSegmentedRecordingSettings()
        : bEnableSegmentation(false)
        , SegmentLengthSeconds(600.f)
        , bResetTimestampsPerSegment(true)
        , bGenerateSegmentManifest(true)
    {
    }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bEnableSegmentation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama", meta = (EditCondition = "bEnableSegmentation", ClampMin = "5.0"))
    float SegmentLengthSeconds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama", meta = (EditCondition = "bEnableSegmentation"))
    bool bResetTimestampsPerSegment;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama", meta = (EditCondition = "bEnableSegmentation"))
    bool bGenerateSegmentManifest;
};

USTRUCT(BlueprintType)
struct FPanoRecoverySettings
{
    GENERATED_BODY()

    FPanoRecoverySettings()
        : bWriteRecoveryFile(true)
        , bAutoRecoverOnBegin(true)
        , HeartbeatIntervalSeconds(5.0f)
    {
    }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bWriteRecoveryFile;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama", meta = (EditCondition = "bWriteRecoveryFile"))
    bool bAutoRecoverOnBegin;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama", meta = (EditCondition = "bWriteRecoveryFile", ClampMin = "1.0"))
    float HeartbeatIntervalSeconds;
};

USTRUCT(BlueprintType)
struct FPanoCaptureOutputSettings
{
    GENERATED_BODY()

    FPanoCaptureOutputSettings()
        : Resolution(4096, 2048)
        , bUse8k(false)
        , bLinearColorSpace(false)
        , OutputMode(EPanoramaCaptureOutputMode::PNGSequence)
        , Codec(EPanoramaCaptureCodec::HEVC)
        , NvencRateControl()
        , TargetDirectory(FDirectoryPath{TEXT("/Game")})
        , bWritePreviewTexture(true)
        , StereoLayout(EPanoramaStereoLayout::OverUnder)
        , bInjectSphericalMetadata(true)
        , bInjectStereoMetadata(true)
        , bInjectSpatialAudioMetadata(true)
        , bAllowFfmpegFallbackScript(true)
        , Segmentation()
    {
    }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    FPanoCaptureResolution Resolution;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bUse8k;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bLinearColorSpace;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    EPanoramaCaptureOutputMode OutputMode;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama", meta = (EditCondition = "OutputMode == EPanoramaCaptureOutputMode::NVENC"))
    EPanoramaCaptureCodec Codec;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama", meta = (EditCondition = "OutputMode == EPanoramaCaptureOutputMode::NVENC"))
    FPanoNvencRateControl NvencRateControl;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    FDirectoryPath TargetDirectory;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bWritePreviewTexture;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    EPanoramaStereoLayout StereoLayout;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bInjectSphericalMetadata;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bInjectStereoMetadata;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bInjectSpatialAudioMetadata;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bAllowFfmpegFallbackScript;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    FPanoSegmentedRecordingSettings Segmentation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    TMap<FString, FString> AdditionalMetadata;
};

USTRUCT(BlueprintType)
struct FPanoCapturePerformanceStats
{
    GENERATED_BODY()

    FPanoCapturePerformanceStats()
        : TotalFramesCaptured(0)
        , TotalFramesDropped(0)
        , AverageCaptureTimeMs(0.f)
        , AverageEncodeTimeMs(0.f)
        , MaxCaptureTimeMs(0.f)
        , MaxEncodeTimeMs(0.f)
        , AudioDriftMs(0.f)
        , TotalDataWrittenMB(0.f)
    {
    }

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Panorama")
    uint64 TotalFramesCaptured;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Panorama")
    uint64 TotalFramesDropped;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Panorama")
    float AverageCaptureTimeMs;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Panorama")
    float AverageEncodeTimeMs;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Panorama")
    float MaxCaptureTimeMs;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Panorama")
    float MaxEncodeTimeMs;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Panorama")
    float AudioDriftMs;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Panorama")
    float TotalDataWrittenMB;

    void Reset()
    {
        TotalFramesCaptured = 0;
        TotalFramesDropped = 0;
        AverageCaptureTimeMs = 0.f;
        AverageEncodeTimeMs = 0.f;
        MaxCaptureTimeMs = 0.f;
        MaxEncodeTimeMs = 0.f;
        AudioDriftMs = 0.f;
        TotalDataWrittenMB = 0.f;
    }
};

USTRUCT(BlueprintType)
struct FPanoCaptureProfile
{
    GENERATED_BODY()

    FPanoCaptureProfile()
        : Name(NAME_None)
        , CaptureMode(EPanoramaCaptureMode::Mono)
        , OutputSettings()
        , FrameRate(30.f)
        , bEnablePreview(true)
        , AudioSettings()
    {
    }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    FName Name;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    EPanoramaCaptureMode CaptureMode;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    FPanoCaptureOutputSettings OutputSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama", meta = (ClampMin = "1.0", ClampMax = "120.0"))
    float FrameRate;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bEnablePreview;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    FPanoAudioCaptureSettings AudioSettings;
};
