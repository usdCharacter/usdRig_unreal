#include "RigExecPlatformerCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputActionValue.h"
#include "Misc/CommandLine.h"
#include "RigExecPlatformerLevel.h"

DEFINE_LOG_CATEGORY_STATIC(LogRigExecPlatformer, Log, All);

ARigExecPlatformerCharacter::ARigExecPlatformerCharacter()
{
	// The camera watches from the side and never turns: from +Y looking
	// back, so the course runs left to right on screen.
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bInheritPitch = CameraBoom->bInheritYaw = CameraBoom->bInheritRoll = false;
	CameraBoom->SetUsingAbsoluteRotation(true);
	// Placed by Tick, as a platformer camera: it leads along the course and
	// follows the height of the ground stood on, not every jump.
	CameraBoom->SetUsingAbsoluteLocation(true);
	CameraBoom->SetRelativeRotation(FRotator(-5.0, -90.0, 0.0));
	CameraBoom->TargetArmLength = 1900.0f;
	CameraBoom->TargetOffset = FVector::ZeroVector;
	CameraBoom->bDoCollisionTest = false;
	CameraBoom->bEnableCameraLag = false;
	FollowCamera->FieldOfView = 62.0f;
	// A crisp side view: no motion blur, no depth of field.
	FollowCamera->PostProcessSettings.bOverride_MotionBlurAmount = true;
	FollowCamera->PostProcessSettings.MotionBlurAmount = 0.0f;
	FollowCamera->PostProcessSettings.bOverride_DepthOfFieldFocalDistance = true;
	FollowCamera->PostProcessSettings.DepthOfFieldFocalDistance = 0.0f;

	// Along the course only, with a high, floaty-then-fast jump.
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->SetPlaneConstraintEnabled(true);
	Movement->SetPlaneConstraintNormal(FVector(0.0, 1.0, 0.0));
	Movement->bSnapToPlaneAtStart = true;
	Movement->MaxWalkSpeed = 600.0f;
	Movement->JumpZVelocity = 1150.0f;
	Movement->GravityScale = 2.0f;
	Movement->AirControl = 0.85f;
	Movement->BrakingDecelerationWalking = 2600.0f;
	Movement->RotationRate = FRotator(0.0, 1800.0, 0.0);
	JumpMaxHoldTime = 0.22f;
}

void
ARigExecPlatformerCharacter::BeginPlay()
{
	Super::BeginPlay();
	bAutoplay |= FParse::Param(FCommandLine::Get(), TEXT("RigExecAutoplay"));
	for (TActorIterator<ARigExecPlatformerLevel> It(GetWorld()); It; ++It)
	{
		Level = *It;
		break;
	}
	if (Level)
	{
		const FVector Start = Level->GetStart() + FVector(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0);
		SetActorLocationAndRotation(Start, FRotator::ZeroRotator, false, nullptr, ETeleportType::TeleportPhysics);
		LastSafe = Start;
		CameraAt = FVector(Start.X + CameraLead, 0.0, Level->GetStart().Z + CameraHeight);
		CameraBoom->SetWorldLocation(CameraAt);
	}
	else
	{
		UE_LOG(LogRigExecPlatformer, Error, TEXT("no ARigExecPlatformerLevel in the level"));
	}
}

void
ARigExecPlatformerCharacter::Move(const FInputActionValue& Value)
{
	// Left and right along the course; up and down do nothing.
	AddMovementInput(FVector(1.0, 0.0, 0.0), Value.Get<FVector2D>().X);
}

void
ARigExecPlatformerCharacter::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp,
                                       bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse,
                                       const FHitResult& Hit)
{
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);
	// A surface facing down: the head hit a block from below.
	if (Level && Other == Level && Hit.ImpactNormal.Z < -0.6)
	{
		Level->HeadHit(Hit.GetComponent(), Hit.Item);
	}
}

bool
ARigExecPlatformerCharacter::Trace(const FVector& From, const FVector& To, FHitResult& Out) const
{
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RigExecAutopilot), false, this);
	return GetWorld()->LineTraceSingleByChannel(Out, From, To, ECC_Visibility, Params);
}

void
ARigExecPlatformerCharacter::Autopilot()
{
	const FVector At = GetActorLocation();
	if (bFinished)
	{
		// Walk on to the castle door, then stop.
		if (At.X < Level->GetDoorX())
		{
			AddMovementInput(FVector(1.0, 0.0, 0.0), 0.55f);
		}
		else if (!bAtDoor)
		{
			bAtDoor = true;
			UE_LOG(LogRigExecPlatformer, Log, TEXT("at the castle: %d of %d coins in %.1f s"), Level->GetCoins(),
			       Level->GetCoinTotal(), CourseTime);
			QuitSoon(2.5f);
		}
		return;
	}
	AddMovementInput(FVector(1.0, 0.0, 0.0), 1.0f);
	const bool bGround = !GetCharacterMovement()->IsFalling();
	if (bGround && PlayTime >= NextJump)
	{
		const double Feet = At.Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		FHitResult Hit;
		// Something to climb: a wall at the knee or the chest.
		const bool bWall = Trace(FVector(At.X, 0, Feet + 30.0), FVector(At.X + 250.0, 0, Feet + 30.0), Hit) ||
		                   Trace(FVector(At.X, 0, Feet + 140.0), FVector(At.X + 250.0, 0, Feet + 140.0), Hit);
		// Nothing to stand on just ahead.
		const bool bGap = !Trace(FVector(At.X + 120.0, 0, Feet + 10.0), FVector(At.X + 120.0, 0, Feet - 160.0), Hit);
		// A ? block overhead, worth a headbutt.
		const bool bQuestion = Trace(At + FVector(70.0, 0, 0), At + FVector(70.0, 0, 420.0), Hit) &&
		                       Level->IsLiveQuestion(Hit.GetComponent(), Hit.Item);
		if (bWall || bGap || bQuestion)
		{
			Jump();
			HoldJumpUntil = PlayTime + 0.22f;
			NextJump = PlayTime + 0.25f;
		}
	}
	if (PlayTime >= HoldJumpUntil)
	{
		StopJumping();
	}
}

void
ARigExecPlatformerCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Level)
	{
		return;
	}
	if (!bFinished)
	{
		CourseTime += DeltaSeconds;
	}
	const FVector At = GetActorLocation();
	// Fell in a gap: back to the last ground stood on.
	if (At.Z < -800.0)
	{
		SetActorLocation(LastSafe, false, nullptr, ETeleportType::TeleportPhysics);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
	}
	else if (!GetCharacterMovement()->IsFalling())
	{
		LastSafe = At;
	}
	// The camera: quick along the course, slow and ground-bound in height.
	{
		const double GroundZ = LastSafe.Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		CameraAt.X = FMath::FInterpTo(CameraAt.X, At.X + CameraLead, DeltaSeconds, 6.0f);
		CameraAt.Z = FMath::FInterpTo(CameraAt.Z, FMath::Max(GroundZ, 0.0) * 0.8 + CameraHeight, DeltaSeconds, 2.5f);
		CameraBoom->SetWorldLocation(FVector(CameraAt.X, 0.0, CameraAt.Z));
	}
	if (!bFinished && At.X >= Level->GetGoalX() - 20.0)
	{
		bFinished = true;
		UE_LOG(LogRigExecPlatformer, Log, TEXT("course clear in %.1f s, %d coins"), CourseTime, Level->GetCoins());
	}
	if (bAutoplay)
	{
		Autopilot();
		if (FMath::FloorToInt(PlayTime) != FMath::FloorToInt(PlayTime - DeltaSeconds))
		{
			UE_LOG(LogRigExecPlatformer, Log, TEXT("t=%.0f column %.1f height %.0f coins %d%s"), PlayTime, At.X / 100.0, At.Z,
			       Level->GetCoins(), GetCharacterMovement()->IsFalling() ? TEXT(" (in the air)") : TEXT(""));
		}
	}
}
