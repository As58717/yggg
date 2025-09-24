#include "PanoramaPngWriter.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

FPanoPngWriter::FPanoPngWriter()
    : bRunning(false)
{
}

void FPanoPngWriter::Configure(const FPanoPngWriteParams& Params)
{
    ActiveParams = Params;
    GeneratedFiles.Reset();
}

void FPanoPngWriter::EnqueueFrame(FPanoPngFrame&& Frame)
{
    FrameQueue.Enqueue(MoveTemp(Frame));

    if (!bRunning)
    {
        bRunning = true;
        Async(EAsyncExecution::ThreadPool, [this]()
        {
            ProcessQueue();
        });
    }
}

void FPanoPngWriter::Flush()
{
    while (!FrameQueue.IsEmpty() || bRunning)
    {
        FPlatformProcess::Sleep(0.01f);
    }
    bRunning = false;
}

void FPanoPngWriter::Shutdown()
{
    Flush();
    FrameQueue.Empty();
    GeneratedFiles.Reset();
}

void FPanoPngWriter::ProcessQueue()
{
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

    FPanoPngFrame Frame;
    while (FrameQueue.Dequeue(Frame))
    {
        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (!ImageWrapper.IsValid())
        {
            continue;
        }

        const ERGBFormat Format = ERGBFormat::RGBA;
        const int32 BitDepth = Frame.b16Bit ? 16 : 8;
        if (!ImageWrapper->SetRaw(Frame.PixelData.GetData(), Frame.PixelData.Num(), Frame.Resolution.X, Frame.Resolution.Y, Format, BitDepth))
        {
            continue;
        }

        const TArray64<uint8>& PngData = ImageWrapper->GetCompressed(0);
        TArray<uint8> Compressed;
        Compressed.Append(PngData.GetData(), PngData.Num());

        const FString FileName = FString::Printf(TEXT("%s_%06llu.png"), *ActiveParams.BaseFileName, Frame.FrameIndex);
        const FString FilePath = FPaths::Combine(ActiveParams.OutputDirectory, FileName);
        if (FFileHelper::SaveArrayToFile(Compressed, *FilePath))
        {
            GeneratedFiles.Add(FilePath);
        }
    }

    bRunning = false;
}

TArray<FString> FPanoPngWriter::GetGeneratedFiles() const
{
    return GeneratedFiles;
}
