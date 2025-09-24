#include "PanoramaCaptureRigActor.h"

#include "Camera/CameraComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PanoramaCaptureComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/StaticMesh.h"

APanoramaCaptureRigActor::APanoramaCaptureRigActor(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryActorTick.bCanEverTick = true;

    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent = Root;

    CaptureComponent = CreateDefaultSubobject<UPanoramaCaptureComponent>(TEXT("PanoramaCapture"));
    CaptureComponent->SetupAttachment(Root);

    StatusText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("StatusText"));
    StatusText->SetupAttachment(Root);
    StatusText->SetHorizontalAlignment(EHTA_Center);
    StatusText->SetTextRenderColor(FColor::Green);
    StatusText->SetWorldSize(30.f);

    PreviewQuad = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PreviewQuad"));
    PreviewQuad->SetupAttachment(Root);
    PreviewQuad->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    PreviewQuad->SetRelativeRotation(FRotator(0.f, 0.f, 0.f));
    PreviewQuad->SetCastShadow(false);

    static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
    if (PlaneMesh.Succeeded())
    {
        PreviewQuad->SetStaticMesh(PlaneMesh.Object);
    }

    static ConstructorHelpers::FObjectFinder<UMaterialInterface> PreviewMat(TEXT("/Engine/EngineMaterials/Widget3DPassThrough.Widget3DPassThrough"));
    if (PreviewMat.Succeeded())
    {
        PreviewQuad->SetMaterial(0, PreviewMat.Object);
    }

    bShowPreviewWindow = true;
    PreviewWindowScale = 0.3f;
    PreviewWindowOffset = FVector(0.f, 100.f, 120.f);

#if WITH_EDITORONLY_DATA
    SpriteComponent = CreateDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
    SpriteComponent->SetupAttachment(Root);
#endif

    RefreshRig();
}

void APanoramaCaptureRigActor::BeginPlay()
{
    Super::BeginPlay();
    UpdateStatusText();
    if (PreviewQuad && !PreviewMID)
    {
        PreviewMID = PreviewQuad->CreateDynamicMaterialInstance(0);
    }
    UpdatePreviewWindow();
}

void APanoramaCaptureRigActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    RefreshRig();
    UpdatePreviewWindow();
}

void APanoramaCaptureRigActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    UpdateStatusText();
    UpdatePreviewWindow();
}

void APanoramaCaptureRigActor::RefreshRig()
{
    const int32 ExpectedCameraCount = 6;
    if (Cameras.Num() != ExpectedCameraCount)
    {
        for (TObjectPtr<UCameraComponent>& Camera : Cameras)
        {
            if (Camera)
            {
                Camera->DestroyComponent();
            }
        }
        Cameras.Empty();

        const TArray<FRotator> Rotations = {
            FRotator(0.f, 90.f, 0.f),   // +X
            FRotator(0.f, -90.f, 0.f),  // -X
            FRotator(-90.f, 0.f, 0.f),  // +Y
            FRotator(90.f, 0.f, 0.f),   // -Y
            FRotator(0.f, 0.f, 0.f),    // +Z
            FRotator(0.f, 180.f, 0.f)   // -Z
        };

        for (int32 Index = 0; Index < ExpectedCameraCount; ++Index)
        {
            const FString CameraName = FString::Printf(TEXT("CaptureCamera_%d"), Index);
            TObjectPtr<UCameraComponent> NewCamera = CreateDefaultSubobject<UCameraComponent>(*CameraName);
            NewCamera->SetupAttachment(Root);
            NewCamera->SetRelativeRotation(Rotations[Index]);
            NewCamera->FieldOfView = 90.f;
            NewCamera->bConstrainAspectRatio = true;
            NewCamera->AspectRatio = 1.f;
            NewCamera->bUsePawnControlRotation = false;
            Cameras.Add(NewCamera);
        }
    }
}

void APanoramaCaptureRigActor::UpdateStatusText()
{
    if (!StatusText)
    {
        return;
    }

    const EPanoramaCaptureStatus Status = CaptureComponent ? CaptureComponent->GetCaptureStatus() : EPanoramaCaptureStatus::Idle;
    FLinearColor Color = FLinearColor::Green;
    FString StatusLabel = TEXT("Idle");

    switch (Status)
    {
    case EPanoramaCaptureStatus::Recording:
        Color = FLinearColor::Red;
        StatusLabel = TEXT("Recording");
        break;
    case EPanoramaCaptureStatus::Finalizing:
        Color = FLinearColor::Yellow;
        StatusLabel = TEXT("Finalizing");
        break;
    case EPanoramaCaptureStatus::DroppedFrames:
        Color = FLinearColor::Orange;
        StatusLabel = TEXT("Dropped Frames");
        break;
    default:
        break;
    }

    if (CaptureComponent && CaptureComponent->GetDroppedFrameCount() > 0)
    {
        StatusLabel += FString::Printf(TEXT(" (%u)"), CaptureComponent->GetDroppedFrameCount());
    }

    StatusText->SetText(FText::FromString(StatusLabel));
    StatusText->SetTextRenderColor(Color.ToFColor(true));
}

void APanoramaCaptureRigActor::StartCapture()
{
    if (CaptureComponent)
    {
        CaptureComponent->StartRecording();
    }
}

void APanoramaCaptureRigActor::UpdatePreviewWindow()
{
    if (!PreviewQuad)
    {
        return;
    }

    UTextureRenderTarget2D* PreviewTexture = CaptureComponent ? CaptureComponent->GetPreviewRenderTarget() : nullptr;
    const bool bShouldShow = bShowPreviewWindow && PreviewTexture != nullptr;
    PreviewQuad->SetVisibility(bShouldShow);
    PreviewQuad->SetHiddenInGame(!bShouldShow);
    PreviewQuad->SetRelativeLocation(PreviewWindowOffset);
    const float ClampedScale = FMath::Clamp(PreviewWindowScale, 0.1f, 10.f);
    PreviewQuad->SetRelativeScale3D(FVector(ClampedScale, ClampedScale, 1.f));

    if (!PreviewWindowOffset.IsNearlyZero())
    {
        const FRotator FacingRotation = (-PreviewWindowOffset).Rotation();
        PreviewQuad->SetRelativeRotation(FacingRotation);
    }

    if (bShouldShow)
    {
        if (!PreviewMID)
        {
            PreviewMID = PreviewQuad->CreateDynamicMaterialInstance(0);
        }

        if (PreviewMID && PreviewTexture)
        {
            PreviewMID->SetTextureParameterValue(TEXT("BaseTexture"), PreviewTexture);
            PreviewMID->SetTextureParameterValue(TEXT("SlateUI"), PreviewTexture);
        }
    }
}

void APanoramaCaptureRigActor::StopCapture()
{
    if (CaptureComponent)
    {
        CaptureComponent->StopRecording();
    }
}
