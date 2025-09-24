#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "PanoramaCaptureTypes.h"
#include "PanoramaCaptureComponent.generated.h"

class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class USoundSubmixBase;
struct FPanoramaEncodedFrame;
struct FPanoCaptureProfile;
class FPanoRecordingRecoveryHandle;

UCLASS(ClassGroup = (PanoramaCapture), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class PANORAMACAPTURE_API UPanoramaCaptureComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UPanoramaCaptureComponent(const FObjectInitializer& ObjectInitializer);

    UFUNCTION(BlueprintCallable, Category = "Panorama")
    void StartRecording();

    UFUNCTION(BlueprintCallable, Category = "Panorama")
    void StopRecording();

    UFUNCTION(BlueprintCallable, Category = "Panorama")
    void TogglePreview(bool bEnable);

    UFUNCTION(BlueprintCallable, Category = "Panorama")
    void ApplyProfileByName(FName ProfileName);

    UFUNCTION(BlueprintPure, Category = "Panorama")
    EPanoramaCaptureStatus GetCaptureStatus() const { return CaptureStatus; }

    UFUNCTION(BlueprintPure, Category = "Panorama")
    UTextureRenderTarget2D* GetPreviewRenderTarget() const { return PreviewRenderTarget; }

    UFUNCTION(BlueprintPure, Category = "Panorama")
    uint32 GetDroppedFrameCount() const { return DroppedFrameCount; }

    UFUNCTION(BlueprintPure, Category = "Panorama")
    const FPanoCapturePerformanceStats& GetPerformanceStats() const { return PerformanceStats; }

    UFUNCTION(BlueprintCallable, Category = "Panorama")
    void ResetPerformanceStats();

    UFUNCTION(BlueprintCallable, Category = "Panorama")
    static void RecoverIncompleteRecordings(const FString& Directory);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    EPanoramaCaptureMode CaptureMode;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    FPanoCaptureOutputSettings OutputSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    float CaptureFrameRate;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bRecordOnBeginPlay;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bEnablePreview;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    float PreviewScale;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    int32 RingBufferSize;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    TObjectPtr<USoundSubmixBase> OverrideAudioSubmix;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bUseLinearGammaForNVENC;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    bool bUse16BitPng;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama")
    FString RecordingLabel;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama|Audio")
    FPanoAudioCaptureSettings AudioSettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama|Recovery")
    FPanoRecoverySettings RecoverySettings;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama|Monitoring")
    bool bCollectPerformanceStats;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Panorama|Monitoring", meta = (EditCondition = "bCollectPerformanceStats", ClampMin = "0.1"))
    float StatsUpdateInterval;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Panorama|Monitoring")
    FPanoCapturePerformanceStats PerformanceStats;

protected:
    virtual void OnRegister() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    void InitializeCaptureFaces();
    void ReleaseResources();
    void AllocateRenderTargets();
    void DestroyRenderTargets();
    void EnqueueFrameCapture(float DeltaTime);
    void ProcessPendingFrames();
    void DispatchCubemapToEquirect(int32 EyeIndex, int32 EyeCount);
    void OnCaptureComplete();
    void UpdatePreview();
    bool ResolveOutputDirectory(FString& OutDirectory) const;
    FString GenerateOutputFileName(const FString& Extension) const;
    void FinalizeRecording();

    void HandleDroppedFrame();
    void FlushRingBuffer();

    void BuildStereoViewMatrices(TArray<FMatrix>& OutLeft, TArray<FMatrix>& OutRight) const;
    void ApplyProfileInternal(const FPanoCaptureProfile& Profile);
    void AttemptAutoRecovery(const FString& Directory);
    void UpdateRecoveryRecord(uint64 InFrameIndex, double Timecode);
    void RefreshPerformanceStats(double CaptureTimeMs, double FrameBytesMB);
    void UpdateEncoderStats();
    void UpdateAudioDrift();
    FString GetMetadataFileExtension() const;
    void EmitFallbackScriptIfNeeded(const FString& BaseName, const FString& CommandLine, bool bCommandSucceeded);
    void GenerateSegmentManifestIfNeeded(const FString& Extension);

    EPanoramaCaptureStatus CaptureStatus;
    float TimeSinceLastCapture;
    double RecordingStartTime;
    FString ActiveOutputDirectory;
    FString ActiveSessionName;

    TArray<TObjectPtr<USceneCaptureComponent2D>> FaceCaptures;
    TArray<TObjectPtr<UTextureRenderTarget2D>> FaceRenderTargets;
    TObjectPtr<UTextureRenderTarget2D> EquirectRenderTarget;

    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> PreviewRenderTarget;

    TArray<FMatrix> MonoViewMatrices;

    class FPanoFrameRingBuffer* FrameRingBuffer;
    TUniquePtr<class FPanoCaptureWorker> CaptureWorker;
    TUniquePtr<class FPanoAudioRecorder> AudioRecorder;
    TUniquePtr<class FPanoNvencEncoder> NvencEncoder;
    TUniquePtr<class FPanoPngWriter> PngWriter;

    uint64 FrameIndex;
    uint32 DroppedFrameCount;

    double LastStatsUpdateTime;
    double CaptureTimeAccumulator;
    double EncodeTimeAccumulator;
    uint64 CaptureSamples;
    uint64 EncodeSamples;
    double LastRecoveryUpdateTime;
    double LastRecordedAudioDuration;
    FString RecordedAudioPath;
    FString RecoveryFilePath;
    FString PendingFfmpegCommandLine;
    TArray<FString> GeneratedContainerFiles;
    double TotalDataWrittenMB;
    uint64 LastReportedEncodedFrames;
    TUniquePtr<FPanoRecordingRecoveryHandle> RecoveryHandle;

    FDelegateHandle OnBeginFrameHandle;
    FDelegateHandle OnEndFrameHandle;

};
