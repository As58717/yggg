#pragma once

#include "CoreMinimal.h"

struct FPanoPngWriteParams
{
    FString OutputDirectory;
    FString BaseFileName;
    bool bUse16Bit;
    bool bLinear;
};

struct FPanoPngFrame
{
    uint64 FrameIndex;
    FIntPoint Resolution;
    double Timecode;
    TArray<uint8> PixelData;
    bool b16Bit;
};

class FPanoPngWriter
{
public:
    FPanoPngWriter();

    void EnqueueFrame(FPanoPngFrame&& Frame);
    void Flush();
    void Shutdown();
    void Configure(const FPanoPngWriteParams& Params);

    TArray<FString> GetGeneratedFiles() const;

private:
    void ProcessQueue();

    FPanoPngWriteParams ActiveParams;
    TQueue<FPanoPngFrame, EQueueMode::Mpsc> FrameQueue;
    TArray<FString> GeneratedFiles;
    FThreadSafeBool bRunning;
};
