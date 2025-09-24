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
};
