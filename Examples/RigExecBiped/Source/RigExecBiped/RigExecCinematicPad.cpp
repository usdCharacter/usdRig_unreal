#include "RigExecCinematicPad.h"

#include "Components/BoxComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "RigExecBipedCharacter.h"
#include "UObject/ConstructorHelpers.h"

ARigExecCinematicPad::ARigExecCinematicPad()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(TEXT("/Engine/BasicShapes/Plane.Plane"));
	Disc = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Disc"));
	Disc->SetupAttachment(RootComponent);
	Disc->SetStaticMesh(Plane.Object);
	// 2.6 m across, just above the floor.
	Disc->SetRelativeLocation(FVector(0.0, 0.0, 0.6));
	Disc->SetRelativeScale3D(FVector(2.6, 2.6, 1.0));
	Disc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Disc->CastShadow = false;

	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetRelativeLocation(FVector(0.0, 0.0, 40.0));
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetIntensity(40.0f);
	Light->SetAttenuationRadius(420.0f);
	Light->SetCastShadows(false);

	Trigger = CreateDefaultSubobject<UBoxComponent>(TEXT("Trigger"));
	Trigger->SetupAttachment(RootComponent);
	Trigger->SetBoxExtent(FVector(85.0, 85.0, 60.0));
	Trigger->SetRelativeLocation(FVector(0.0, 0.0, 60.0));
	Trigger->SetCollisionProfileName(TEXT("Trigger"));

	GlowMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/RigExec/M_RigExecGlow.M_RigExecGlow")));
}

void
ARigExecCinematicPad::BeginPlay()
{
	Super::BeginPlay();
	if (UMaterialInterface* Material = GlowMaterial.LoadSynchronous())
	{
		Look = UMaterialInstanceDynamic::Create(Material, this);
		Look->SetVectorParameterValue(TEXT("Color"), Color);
		Disc->SetMaterial(0, Look);
	}
	Light->SetLightColor(Color);
	Trigger->OnComponentBeginOverlap.AddDynamic(this, &ARigExecCinematicPad::Entered);
	Trigger->OnComponentEndOverlap.AddDynamic(this, &ARigExecCinematicPad::Left);
}

void
ARigExecCinematicPad::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Time += DeltaSeconds;
	// A slow breath while armed; dim while used.
	const float Pulse = 0.5f + 0.5f * FMath::Sin(Time * 2.6f);
	const float Glow = bArmed ? 6.0f + 5.0f * Pulse : 1.5f;
	if (Look)
	{
		Look->SetScalarParameterValue(TEXT("Glow"), Glow);
	}
	Light->SetIntensity(bArmed ? 25.0f + 30.0f * Pulse : 6.0f);
}

void
ARigExecCinematicPad::Entered(UPrimitiveComponent* Overlapped, AActor* Other, UPrimitiveComponent* OtherComp,
                              int32 BodyIndex, bool bFromSweep, const FHitResult& Sweep)
{
	ARigExecBipedCharacter* Hero = Cast<ARigExecBipedCharacter>(Other);
	if (bArmed && Hero && !Hero->IsInCinematic())
	{
		bArmed = false;
		Hero->StartCinematic();
	}
}

void
ARigExecCinematicPad::Left(UPrimitiveComponent* Overlapped, AActor* Other, UPrimitiveComponent* OtherComp,
                           int32 BodyIndex)
{
	if (Cast<ARigExecBipedCharacter>(Other))
	{
		bArmed = true;
	}
}
