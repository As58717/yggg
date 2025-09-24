#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "PanoramaCaptureTypes.h"
#include "PanoramaCaptureSettings.generated.h"

class USoundSubmixBase;

UCLASS(config=Engine, defaultconfig, meta = (DisplayName = "Panorama Capture"))
class PANORAMACAPTURE_API UPanoramaCaptureSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UPanoramaCaptureSettings();

    UPROPERTY(EditAnywhere, config, Category = "Defaults")
    EPanoramaCaptureMode DefaultCaptureMode;

    UPROPERTY(EditAnywhere, config, Category = "Defaults")
    FPanoCaptureOutputSettings DefaultOutputSettings;

    UPROPERTY(EditAnywhere, config, Category = "Audio")
    TObjectPtr<USoundSubmixBase> TargetSubmix;

    UPROPERTY(EditAnywhere, config, Category = "Audio")
    FPanoAudioCaptureSettings DefaultAudioSettings;

    UPROPERTY(EditAnywhere, config, Category = "Output")
    bool bEmbedAudioInContainer;

    UPROPERTY(EditAnywhere, config, Category = "Output")
    bool bGenerateMKV;

    UPROPERTY(EditAnywhere, config, Category = "Output")
    bool bOverwriteExisting;

    UPROPERTY(EditAnywhere, config, Category = "Output")
    FString OutputFileNameFormat;

    UPROPERTY(EditAnywhere, config, Category = "Output")
    bool bCollectPerformanceStats;

    UPROPERTY(EditAnywhere, config, Category = "Output")
    float StatsUpdateInterval;

    UPROPERTY(EditAnywhere, config, Category = "Recovery")
    FPanoRecoverySettings DefaultRecoverySettings;

    UPROPERTY(EditAnywhere, config, Category = "Profiles")
    bool bAutoApplyActiveProfile;

    UPROPERTY(EditAnywhere, config, Category = "Profiles")
    TArray<FPanoCaptureProfile> Profiles;

    UPROPERTY(EditAnywhere, config, Category = "Profiles")
    FName ActiveProfileName;

    virtual FName GetCategoryName() const override;

    const FPanoCaptureProfile* FindProfileByName(FName ProfileName) const;
};
