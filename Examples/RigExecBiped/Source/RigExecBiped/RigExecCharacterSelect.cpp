#include "RigExecCharacterSelect.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "RigExecBipedCharacter.h"

namespace
{
// Characters stand along +Y (screen right), facing -X (the camera).
constexpr double GSpacing = 190.0;
constexpr double GStep = 70.0;
const FLinearColor GPadColor(0.25f, 0.75f, 1.0f);
} // namespace

ARigExecCharacterSelect::ARigExecCharacterSelect()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);
	Camera->SetFieldOfView(38.0f);
	Camera->PostProcessSettings.bOverride_VignetteIntensity = true;
	Camera->PostProcessSettings.VignetteIntensity = 0.6f;
	// The lobby is cut to from the level: adapt the exposure at once.
	Camera->PostProcessSettings.bOverride_AutoExposureSpeedUp = true;
	Camera->PostProcessSettings.AutoExposureSpeedUp = 20.0f;
	Camera->PostProcessSettings.bOverride_AutoExposureSpeedDown = true;
	Camera->PostProcessSettings.AutoExposureSpeedDown = 20.0f;
}

UStaticMeshComponent*
ARigExecCharacterSelect::AddShape(const TCHAR* Mesh, const FVector& At, const FVector& Scale, const FLinearColor& Color)
{
	UStaticMeshComponent* Shape = NewObject<UStaticMeshComponent>(this);
	Shape->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, Mesh));
	Shape->SetupAttachment(RootComponent);
	Shape->SetRelativeLocation(At);
	Shape->SetRelativeScale3D(Scale);
	if (UMaterialInterface* Basic = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
	{
		UMaterialInstanceDynamic* Look = UMaterialInstanceDynamic::Create(Basic, Shape);
		Look->SetVectorParameterValue(TEXT("Color"), Color);
		Shape->SetMaterial(0, Look);
	}
	Shape->RegisterComponent();
	AddInstanceComponent(Shape);
	return Shape;
}

FVector
ARigExecCharacterSelect::Slot(int32 Index) const
{
	const double Middle = 0.5 * (Previews.Num() - 1);
	return FVector(0.0, (Index - Middle) * GSpacing, 0.0);
}

void
ARigExecCharacterSelect::Stage(const TArray<FSoftObjectPath>& Characters, int32 InFocus, int32 Skip)
{
	UWorld* World = GetWorld();
	const int32 Count = Characters.Num();
	const double Wide = FMath::Max(Count, 2) * GSpacing;

	// A deep blue floor and a backdrop that hides the level behind.
	AddShape(TEXT("/Engine/BasicShapes/Cube.Cube"), FVector(60.0, 0.0, -10.0), FVector(8.0, Wide / 100.0 + 8.0, 0.2),
	         FLinearColor(0.012f, 0.025f, 0.06f));
	AddShape(TEXT("/Engine/BasicShapes/Cube.Cube"), FVector(320.0, 0.0, 250.0), FVector(0.2, Wide / 100.0 + 12.0, 7.0),
	         FLinearColor(0.02f, 0.05f, 0.12f));

	UMaterialInterface* Glow = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/RigExec/M_RigExecGlow.M_RigExecGlow"));
	Previews.SetNum(Count);
	Forward.Init(0.0f, Count);
	for (int32 Each = 0; Each < Count; ++Each)
	{
		// Pads first so Slot() spaces them by the final count.
		Previews[Each] = nullptr;
	}
	for (int32 Each = 0; Each < Count; ++Each)
	{
		const FVector At = Slot(Each);
		UStaticMeshComponent* Pad = NewObject<UStaticMeshComponent>(this);
		Pad->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
		Pad->SetupAttachment(RootComponent);
		Pad->SetRelativeLocation(At + FVector(0.0, 0.0, 1.0));
		Pad->SetRelativeScale3D(FVector(1.7, 1.7, 1.0));
		Pad->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pad->CastShadow = false;
		UMaterialInstanceDynamic* Look = Glow ? UMaterialInstanceDynamic::Create(Glow, Pad) : nullptr;
		if (Look)
		{
			Look->SetVectorParameterValue(TEXT("Color"), GPadColor);
			Pad->SetMaterial(0, Look);
		}
		Pad->RegisterComponent();
		AddInstanceComponent(Pad);
		Pads.Add(Pad);
		PadLooks.Add(Look);

		UPointLightComponent* Up = NewObject<UPointLightComponent>(this);
		Up->SetupAttachment(RootComponent);
		Up->SetRelativeLocation(At + FVector(-30.0, 0.0, 30.0));
		Up->SetIntensityUnits(ELightUnits::Candelas);
		Up->SetLightColor(GPadColor);
		Up->SetAttenuationRadius(260.0f);
		Up->SetCastShadows(false);
		Up->RegisterComponent();
		AddInstanceComponent(Up);
		PadLights.Add(Up);

		if (Each == Skip)
		{
			continue;
		}
		// The preview: the game's own character, loading this asset, with no
		// controller, so it stands and idles.
		const FTransform Where(FRotator(0.0, 180.0, 0.0), GetActorLocation() + At);
		ARigExecBipedCharacter* Preview = World->SpawnActorDeferred<ARigExecBipedCharacter>(
		    ARigExecBipedCharacter::StaticClass(), Where, this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Preview)
		{
			Preview->bPreview = true;
			Preview->PreviewAsset = Characters[Each];
			Preview->AutoPossessPlayer = EAutoReceiveInput::Disabled;
			Preview->AutoPossessAI = EAutoPossessAI::Disabled;
			Preview->FinishSpawning(Where);
			// Feet on the floor: the capsule's centre sits half its height up.
			const float Half = Preview->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			Preview->SetActorLocation(GetActorLocation() + At + FVector(0.0, 0.0, Half));
		}
		Previews[Each] = Preview;
	}

	// A warm key from camera left and a cool rim from behind.
	Key = NewObject<UPointLightComponent>(this);
	Key->SetupAttachment(RootComponent);
	Key->SetRelativeLocation(FVector(-420.0, -260.0, 300.0));
	Key->SetIntensityUnits(ELightUnits::Candelas);
	Key->SetIntensity(900.0f);
	Key->SetLightColor(FLinearColor(1.0f, 0.92f, 0.82f));
	Key->SetAttenuationRadius(1600.0f);
	Key->RegisterComponent();
	AddInstanceComponent(Key);
	Rim = NewObject<UPointLightComponent>(this);
	Rim->SetupAttachment(RootComponent);
	Rim->SetRelativeLocation(FVector(220.0, 180.0, 260.0));
	Rim->SetIntensityUnits(ELightUnits::Candelas);
	Rim->SetIntensity(600.0f);
	Rim->SetLightColor(FLinearColor(0.45f, 0.75f, 1.0f));
	Rim->SetAttenuationRadius(900.0f);
	Rim->SetCastShadows(false);
	Rim->RegisterComponent();
	AddInstanceComponent(Rim);

	// Framed to show everyone head to toe, a little from below.
	const double Back = 560.0 + 150.0 * Count;
	Camera->SetRelativeLocation(FVector(-Back, 0.0, 120.0));
	Camera->SetRelativeRotation((FVector(0.0, 0.0, 88.0) - FVector(-Back, 0.0, 120.0)).Rotation());
	SetFocus(InFocus);
}

void
ARigExecCharacterSelect::SetFocus(int32 Index)
{
	Focus = Previews.Num() ? (Index % Previews.Num() + Previews.Num()) % Previews.Num() : 0;
}

bool
ARigExecCharacterSelect::Lend(int32 Index, ARigExecBipedCharacter* Character)
{
	if (!Character || !Previews.IsValidIndex(Index) || Previews[Index] || Lent != INDEX_NONE)
	{
		return false;
	}
	Lent = Index;
	LentFrom = Character->GetActorTransform();
	// Idles at the lobby's rig rate while it stands here.
	Character->bPreview = true;
	Previews[Index] = Character;
	Forward[Index] = Index == Focus ? 1.0f : 0.0f;
	return true;
}

void
ARigExecCharacterSelect::Return()
{
	if (Lent == INDEX_NONE)
	{
		return;
	}
	if (ARigExecBipedCharacter* Character = Previews[Lent])
	{
		Character->bPreview = false;
		Character->SetActorTransform(LentFrom, false, nullptr, ETeleportType::TeleportPhysics);
	}
	Previews[Lent] = nullptr;
	Lent = INDEX_NONE;
}

FTransform
ARigExecCharacterSelect::Keep()
{
	Lent = INDEX_NONE;
	return LentFrom;
}

bool
ARigExecCharacterSelect::IsLent(const ARigExecBipedCharacter* Character) const
{
	return Character && Lent != INDEX_NONE && Previews[Lent] == Character;
}

namespace
{
void
Wake(AActor* Actor, bool bAwake)
{
	Actor->SetActorHiddenInGame(!bAwake);
	Actor->SetActorTickEnabled(bAwake);
	for (UActorComponent* Component : Actor->GetComponents())
	{
		if (Component && Component->PrimaryComponentTick.bCanEverTick)
		{
			Component->SetComponentTickEnabled(bAwake);
		}
	}
}
} // namespace

void
ARigExecCharacterSelect::SetShown(bool bShown)
{
	SetActorHiddenInGame(!bShown);
	SetActorTickEnabled(bShown);
	for (int32 Each = 0; Each < Previews.Num(); ++Each)
	{
		if (Previews[Each] && Each != Lent)
		{
			Wake(Previews[Each], bShown);
		}
	}
}

ARigExecBipedCharacter*
ARigExecCharacterSelect::Take(int32 Index)
{
	if (!Previews.IsValidIndex(Index))
	{
		return nullptr;
	}
	ARigExecBipedCharacter* Preview = Previews[Index];
	Previews[Index] = nullptr;
	return Preview;
}

bool
ARigExecCharacterSelect::GetPreviewBounds(int32 Index, FVector& Feet, float& Height) const
{
	if (!Previews.IsValidIndex(Index) || !Previews[Index])
	{
		return false;
	}
	FVector Origin, Extent;
	Previews[Index]->GetActorBounds(true, Origin, Extent);
	Feet = Previews[Index]->GetActorLocation() -
	       FVector(0.0, 0.0, Previews[Index]->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	Height = FMath::Max(float(Extent.Z * 2.0), 120.0f);
	return true;
}

void
ARigExecCharacterSelect::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Time += DeltaSeconds;
	for (int32 Each = 0; Each < Previews.Num(); ++Each)
	{
		const bool bFocus = Each == Focus;
		Forward[Each] = FMath::FInterpTo(Forward[Each], bFocus ? 1.0f : 0.0f, DeltaSeconds, 6.0f);
		if (ARigExecBipedCharacter* Preview = Previews[Each])
		{
			const float Half = Preview->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			Preview->SetActorLocation(GetActorLocation() + Slot(Each) + FVector(-GStep * Forward[Each], 0.0, Half));
			// The focused one turns a touch toward the camera, the others to it.
			const double Turn = bFocus ? 0.0 : (Slot(Each).Y < 0.0 ? -14.0 : 14.0);
			Preview->SetActorRotation(FRotator(0.0, 180.0 + Turn, 0.0));
		}
		const float Pulse = 0.5f + 0.5f * FMath::Sin(Time * 3.0f);
		if (PadLooks.IsValidIndex(Each) && PadLooks[Each])
		{
			PadLooks[Each]->SetScalarParameterValue(TEXT("Glow"), bFocus ? 7.0f + 5.0f * Pulse : 1.2f);
		}
		if (Pads.IsValidIndex(Each))
		{
			Pads[Each]->SetRelativeLocation(Slot(Each) + FVector(-GStep * Forward[Each], 0.0, 1.0));
		}
		if (PadLights.IsValidIndex(Each))
		{
			PadLights[Each]->SetIntensity(bFocus ? 60.0f + 30.0f * Pulse : 6.0f);
			PadLights[Each]->SetRelativeLocation(Slot(Each) + FVector(-GStep * Forward[Each] - 30.0, 0.0, 30.0));
		}
	}
}

void
ARigExecCharacterSelect::EndPlay(const EEndPlayReason::Type Reason)
{
	for (int32 Each = 0; Each < Previews.Num(); ++Each)
	{
		// The lent one is the player's pawn, not the lobby's.
		if (Previews[Each] && Each != Lent)
		{
			Previews[Each]->Destroy();
		}
	}
	Previews.Reset();
	Super::EndPlay(Reason);
}
