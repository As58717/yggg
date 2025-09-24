#pragma once

#include "CoreMinimal.h"
#include "PanoramaCaptureTypes.h"
#include "HAL/CriticalSection.h"

struct ID3D11Device;
struct ID3D11Texture2D;
struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12CommandQueue;
class FArchive;
namespace AVEncoder
{
    class FVideoEncoderInput;
    class FVideoEncoder;
}

struct FPanoramaNvencEncodeParams
{
    FIntPoint Resolution;
    EPanoramaCaptureCodec Codec;
    FPanoNvencRateControl RateControl;
    bool bUseLinear;
    float FrameRate = 0.f;
    FString OutputBitstreamPath;
};

struct FPanoramaEncodedFrame
{
    uint64 FrameIndex = 0;
    double Timecode = 0.0;
    TArray<uint8> EncodedBytes;
};

struct FPanoNvencEncoderLifetimeStats
{
    uint64 FramesEncoded = 0;
    double TotalEncodeTimeMs = 0.0;
    double MaxEncodeTimeMs = 0.0;
    uint64 TotalEncodedBytes = 0;
};

class FPanoNvencEncoder
{
public:
    FPanoNvencEncoder();
    ~FPanoNvencEncoder();

    bool Initialize(const FPanoramaNvencEncodeParams& Params);
    void Shutdown();

    bool EnqueueResource(FTextureRHIRef Texture, uint64 FrameIndex, double Timecode);
    void Flush(TArray<FPanoramaEncodedFrame>& OutFrames);

    const FPanoramaNvencEncodeParams& GetParams() const { return ActiveParams; }

    void GetLifetimeStats(FPanoNvencEncoderLifetimeStats& OutStats) const;

private:
    bool InitializeD3D11();
    bool InitializeD3D12();
    void ReleaseResources();
    bool EncodeFrame_RenderThread(FTextureRHIRef Texture, uint64 FrameIndex, double Timecode);
    void RecordEncodeDuration(double Milliseconds);
    void RecordEncodedBytes(uint64 NumBytes);

    FPanoramaNvencEncodeParams ActiveParams;
    bool bInitialized;
    TArray<FPanoramaEncodedFrame> PendingFrames;
    FCriticalSection PendingFramesGuard;
    FCriticalSection BitstreamWriterGuard;
    TUniquePtr<class FArchive> BitstreamWriter;
    mutable FCriticalSection StatsGuard;
    FPanoNvencEncoderLifetimeStats LifetimeStats;

#if PANORAMA_CAPTURE_WITH_NVENC
    void* NvEncHandle;
    void* NvEncInterface;
    TArray<void*> RegisteredResources;
    bool bUsingD3D12;
    TSharedPtr<class AVEncoder::FVideoEncoderInput> EncoderInput;
    TUniquePtr<class AVEncoder::FVideoEncoder> Encoder;
#endif
};
