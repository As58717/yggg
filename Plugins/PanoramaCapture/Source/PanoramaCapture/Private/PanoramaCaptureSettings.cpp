#include "PanoramaCaptureSettings.h"

#include "Sound/SoundSubmix.h"

UPanoramaCaptureSettings::UPanoramaCaptureSettings()
{
    CategoryName = TEXT("Plugins");
    SectionName = TEXT("PanoramaCapture");

    DefaultCaptureMode = EPanoramaCaptureMode::Mono;
    DefaultOutputSettings.OutputMode = EPanoramaCaptureOutputMode::PNGSequence;
    DefaultOutputSettings.Resolution = FPanoCaptureResolution(4096, 2048);
    DefaultOutputSettings.bUse8k = false;
    DefaultOutputSettings.bLinearColorSpace = false;
    DefaultOutputSettings.TargetDirectory.Path = TEXT("PanoramaCaptures");

    bEmbedAudioInContainer = true;
    bGenerateMKV = true;
    bOverwriteExisting = false;
    OutputFileNameFormat = TEXT("Panorama_{date}_{time}");
}

FName UPanoramaCaptureSettings::GetCategoryName() const
{
    return CategoryName;
}
