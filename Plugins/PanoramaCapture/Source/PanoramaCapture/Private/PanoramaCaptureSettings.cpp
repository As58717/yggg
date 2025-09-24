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

    DefaultAudioSettings = FPanoAudioCaptureSettings();
    DefaultRecoverySettings = FPanoRecoverySettings();

    bEmbedAudioInContainer = true;
    bGenerateMKV = true;
    bOverwriteExisting = false;
    OutputFileNameFormat = TEXT("Panorama_{date}_{time}");
    bCollectPerformanceStats = true;
    StatsUpdateInterval = 1.0f;
    bAutoApplyActiveProfile = true;
    ActiveProfileName = NAME_None;
}

FName UPanoramaCaptureSettings::GetCategoryName() const
{
    return CategoryName;
}

const FPanoCaptureProfile* UPanoramaCaptureSettings::FindProfileByName(FName ProfileName) const
{
    for (const FPanoCaptureProfile& Profile : Profiles)
    {
        if (Profile.Name == ProfileName)
        {
            return &Profile;
        }
    }
    return nullptr;
}
