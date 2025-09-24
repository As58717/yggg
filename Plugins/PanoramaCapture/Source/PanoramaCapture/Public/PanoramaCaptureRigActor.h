#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PanoramaCaptureTypes.h"
#include "PanoramaCaptureRigActor.generated.h"

class UCameraComponent;
class UBillboardComponent;
class UTextRenderComponent;
class UPanoramaCaptureComponent;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;

UCLASS(Blueprintable)
class PANORAMACAPTURE_API APanoramaCaptureRigActor : public AActor
{
    GENERATED_BODY()

public:
    APanoramaCaptureRigActor(const FObjectInitializer& ObjectInitializer);

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rig")
    TObjectPtr<USceneComponent> Root;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rig")
    TArray<TObjectPtr<UCameraComponent>> Cameras;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rig")
    TObjectPtr<UPanoramaCaptureComponent> CaptureComponent;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Rig")
    TObjectPtr<UTextRenderComponent> StatusText;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Preview")
    TObjectPtr<UStaticMeshComponent> PreviewQuad;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preview")
    bool bShowPreviewWindow;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preview", meta = (ClampMin = "0.1", ClampMax = "10.0"))
    float PreviewWindowScale;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preview")
    FVector PreviewWindowOffset;

#if WITH_EDITORONLY_DATA
    UPROPERTY()
    TObjectPtr<UBillboardComponent> SpriteComponent;
#endif

    UFUNCTION(BlueprintCallable, Category = "Rig")
    void RefreshRig();

    UFUNCTION(BlueprintCallable, Category = "Capture")
    void StartCapture();

    UFUNCTION(BlueprintCallable, Category = "Capture")
    void StopCapture();

protected:
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void BeginPlay() override;

private:
    void UpdateStatusText();
    void UpdatePreviewWindow();

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> PreviewMID;
};
