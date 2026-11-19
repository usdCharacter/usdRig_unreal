#include "RigExecBipedCharacter.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/HUD.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "RigExecCharacter.h"
#include "RigExecComponent.h"
#include "RigExecEyeComponent.h"
#include "RigExecMenuHUD.h"
#include "RigExecCinematicPad.h"
#include "RigExecRetargetComponent.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogRigExecBiped, Log, All);

namespace
{
// Where the meshes sit under the capsule: feet on its bottom, facing its
// forward (both the Mannequin and the rig face +Y in their own space).
const FVector GFeet(0.0, 0.0, -92.0);
const FRotator GFacing(0.0, -90.0, 0.0);
} // namespace

ARigExecBipedCharacter::ARigExecBipedCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	GetCapsuleComponent()->InitCapsuleSize(42.0f, 92.0f);

	// Turn toward the way it moves; the camera turns with the mouse.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = true;
	Movement->RotationRate = FRotator(0.0f, 500.0f, 0.0f);
	Movement->JumpZVelocity = 500.0f;
	Movement->AirControl = 0.35f;
	Movement->MaxWalkSpeed = 500.0f;
	Movement->MinAnalogWalkSpeed = 20.0f;
	Movement->BrakingDecelerationWalking = 2000.0f;
	Movement->BrakingDecelerationFalling = 1500.0f;

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// The Mannequin: animated, normally not drawn.
	GetMesh()->SetRelativeLocationAndRotation(GFeet, GFacing);
	GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// The rig, placed like the Mannequin. Nothing pulls a Control Rig: the
	// retargeter sets its controls.
	Rig = CreateDefaultSubobject<URigExecComponent>(TEXT("Rig"));
	Rig->SetupAttachment(RootComponent);
	Rig->SetRelativeLocationAndRotation(GFeet, GFacing);
	Rig->bPullControlRig = false;

	Retarget = CreateDefaultSubobject<URigExecRetargetComponent>(TEXT("Retarget"));
	Eyes = CreateDefaultSubobject<URigExecEyeComponent>(TEXT("Eyes"));

	CharacterAsset = TSoftObjectPtr<URigExecCharacter>(FSoftObjectPath(TEXT("/Game/RigExec/RXC_Biped.RXC_Biped")));
	SourceMesh = TSoftObjectPtr<USkeletalMesh>(
		FSoftObjectPath(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple")));
	SourceAnimation = TSoftClassPtr<UAnimInstance>(
		FSoftObjectPath(TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed.ABP_Unarmed_C")));
}

bool ARigExecBipedCharacter::bRigAsync = true;
int32 ARigExecBipedCharacter::RigRateFar = 2;
int32 ARigExecBipedCharacter::RigRateLobby = 3;

TArray<ARigExecBipedCharacter::FChoice>
ARigExecBipedCharacter::FindCharacters()
{
	TArray<FChoice> Out;
	TArray<FAssetData> Assets;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
#if WITH_EDITOR
	// An editor build scans the content while it starts; a packaged game
	// ships its registry ready.
	static bool bScanned = false;
	if (!bScanned)
	{
		Registry.ScanPathsSynchronous({TEXT("/Game/RigExec")}, false);
		bScanned = true;
	}
#endif
	Registry.GetAssetsByClass(URigExecCharacter::StaticClass()->GetClassPathName(), Assets);
	for (const FAssetData& Asset : Assets)
	{
		FString Label = Asset.AssetName.ToString();
		Label.RemoveFromStart(TEXT("RXC_"));
		Out.Add({Label.ToUpper(), Asset.GetSoftObjectPath()});
	}
	Out.Sort([](const FChoice& A, const FChoice& B) {
		const bool bA = A.Label == TEXT("BIPED"), bB = B.Label == TEXT("BIPED");
		return bA != bB ? bA : A.Label < B.Label;
	});
	return Out;
}

FSoftObjectPath
ARigExecBipedCharacter::ChosenCharacter()
{
	const TArray<FChoice> Choices = FindCharacters();
	FString Wanted;
	if (FParse::Value(FCommandLine::Get(), TEXT("RigExecCharacter="), Wanted))
	{
		for (const FChoice& Choice : Choices)
		{
			if (Choice.Label.Equals(Wanted, ESearchCase::IgnoreCase))
			{
				return Choice.Asset;
			}
		}
	}
	return FSoftObjectPath();
}

void
ARigExecBipedCharacter::ChooseCharacter(const FSoftObjectPath& Asset)
{
	if (GConfig)
	{
		GConfig->SetString(TEXT("RigExecBiped.Settings"), TEXT("Character"), *Asset.ToString(), GGameUserSettingsIni);
		GConfig->Flush(false, GGameUserSettingsIni);
	}
}

void
ARigExecBipedCharacter::BeginPlay()
{
	Super::BeginPlay();
	const FSoftObjectPath Chosen = bPreview ? PreviewAsset : ChosenCharacter();
	if (Chosen.IsValid())
	{
		CharacterAsset = TSoftObjectPtr<URigExecCharacter>(Chosen);
	}
	UE_LOG(LogRigExecBiped, Log, TEXT("%s %s"), bPreview ? TEXT("previewing") : TEXT("playing as"), *CharacterAsset.ToString());
	bDemo = !bPreview && FParse::Param(FCommandLine::Get(), TEXT("RigExecDemo"));
	bShowSource |= FParse::Param(FCommandLine::Get(), TEXT("RigExecShowSource"));

	if (USkeletalMesh* Manny = SourceMesh.LoadSynchronous())
	{
		GetMesh()->SetSkeletalMesh(Manny);
	}
	else
	{
		UE_LOG(LogRigExecBiped, Error, TEXT("no Mannequin at %s; run Setup_RigExecBiped.bat"), *SourceMesh.ToString());
	}
	if (UClass* Anim = SourceAnimation.LoadSynchronous())
	{
		GetMesh()->SetAnimInstanceClass(Anim);
	}
	else
	{
		UE_LOG(LogRigExecBiped, Error, TEXT("no animation blueprint at %s"), *SourceAnimation.ToString());
	}
	ApplySourceVisibility();

	Rig->Character = CharacterAsset.LoadSynchronous();
	if (!Rig->Character)
	{
		UE_LOG(LogRigExecBiped, Error, TEXT("no baked character at %s; run Setup_RigExecBiped.bat"), *CharacterAsset.ToString());
	}
	else if (!Rig->Load())
	{
		UE_LOG(LogRigExecBiped, Error, TEXT("the biped did not load; see LogRigExec"));
	}
	Retarget->Source = GetMesh();
	Retarget->Target = Rig;
	Retarget->Rebind();
	// The eye materials go on before a subdivision surface gathers the meshes.
	Eyes->Rig = Rig;
	Eyes->Bind();
	if (!bPreview)
	{
		BeginCinematicSetup();
	}
}

void
ARigExecBipedCharacter::BecomePlayable()
{
	bPreview = false;
	bDemo = FParse::Param(FCommandLine::Get(), TEXT("RigExecDemo"));
	PlayTime = 0.0f;
	BeginCinematicSetup();
}

void
ARigExecBipedCharacter::BecomePreview()
{
	bPreview = true;
	bDemo = false;
	PreviewAsset = CharacterAsset.ToSoftObjectPath();
}

void
ARigExecBipedCharacter::BeginCinematicSetup()
{
	// The face the cinematic poses: lids translate, the jaw turns, the eyes
	// follow the look target.
	LeftLid = Rig->FindControlIndex(TEXT("L_UpLid"));
	RightLid = Rig->FindControlIndex(TEXT("R_UpLid"));
	Jaw = Rig->FindControlIndex(TEXT("M_Jaw"));
	LookTarget = Rig->FindControlIndex(TEXT("M_Look"));
	LeftEye = Rig->FindControlIndex(TEXT("L_Eye"));
	RightEye = Rig->FindControlIndex(TEXT("R_Eye"));
	Chin = Rig->FindControlIndex(TEXT("M_Chin"));
	LeftCorner = Rig->FindControlIndex(TEXT("L_Mouth"));
	RightCorner = Rig->FindControlIndex(TEXT("R_Mouth"));
	UpLip = Rig->FindControlIndex(TEXT("M_UpLip"));
	LeftUpLip = Rig->FindControlIndex(TEXT("L_UpLip"));
	RightUpLip = Rig->FindControlIndex(TEXT("R_UpLip"));
	LeftInBrow = Rig->FindControlIndex(TEXT("L_InBrow"));
	RightInBrow = Rig->FindControlIndex(TEXT("R_InBrow"));
	if (LookTarget != INDEX_NONE)
	{
		LookAtChannel = Rig->GetControlPath(LookTarget) + TEXT(".avars:lookAt");
	}
	// Only on a level with a close-up to subdivide for.
	if (TActorIterator<ARigExecCinematicPad>(GetWorld()))
	{
		MakeSubdivision();
	}
	bPortrait = FParse::Param(FCommandLine::Get(), TEXT("RigExecPortrait"));
	if (FParse::Param(FCommandLine::Get(), TEXT("RigExecPadDemo")))
	{
		for (TActorIterator<ARigExecCinematicPad> It(GetWorld()); It; ++It)
		{
			const FVector Pad = It->GetActorLocation();
			// A lap of the floor, then onto the pad.
			Route = {FVector(650.0, 250.0, 0.0), FVector(850.0, -250.0, 0.0), FVector(150.0, -500.0, 0.0),
			         FVector(Pad.X, Pad.Y, 0.0)};
			break;
		}
	}
}

void
ARigExecBipedCharacter::MakeSubdivision()
{
	UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/SquarebitSubdivs.SquarebitSubdivDynamicMeshComponent"));
	if (!Class)
	{
		UE_LOG(LogRigExecBiped, Log, TEXT("Squarebit Subdivs is not in this build; the biped stays unsubdivided"));
		return;
	}
	Subdivision = NewObject<UPrimitiveComponent>(this, Class, TEXT("Subdivision"));
	// Under the rig, so it sits where the meshes it gathers do.
	Subdivision->SetupAttachment(Rig);
	SetSubdivisionLevel(0);
	Subdivision->RegisterComponent();
	AddInstanceComponent(Subdivision);
}

void
ARigExecBipedCharacter::SetSubdivisionLevel(int32 Level)
{
	if (!Subdivision)
	{
		return;
	}
	Level = FMath::Clamp(Level, 0, MaxSubdivisionLevel);
	// Set as Sequencer would: the component picks the change up on its tick.
	if (FIntProperty* Property = FindFProperty<FIntProperty>(Subdivision->GetClass(), TEXT("SubdivisionLevel")))
	{
		if (Property->GetPropertyValue_InContainer(Subdivision) != Level)
		{
			Property->SetPropertyValue_InContainer(Subdivision, Level);
			UE_LOG(LogRigExecBiped, Log, TEXT("subdivision level %d"), Level);
		}
	}
	// Once someone has chosen, the cinematic leaves the level alone.
	if (bCinematic && CinematicTime > 0.0f)
	{
		bLevelChosen = true;
	}
}

void
ARigExecBipedCharacter::SubdivisionUp()
{
	if (InMenu())
	{
		return;
	}
	SetSubdivisionLevel(GetSubdivisionLevel() + 1);
}

void
ARigExecBipedCharacter::SubdivisionDown()
{
	if (InMenu())
	{
		return;
	}
	SetSubdivisionLevel(GetSubdivisionLevel() - 1);
}

int32
ARigExecBipedCharacter::GetSubdivisionLevel() const
{
	if (!Subdivision)
	{
		return -1;
	}
	const FIntProperty* Property = FindFProperty<FIntProperty>(Subdivision->GetClass(), TEXT("SubdivisionLevel"));
	return Property ? Property->GetPropertyValue_InContainer(Subdivision) : -1;
}

FVector
ARigExecBipedCharacter::FaceCentre() const
{
	if (LeftEye == INDEX_NONE || RightEye == INDEX_NONE)
	{
		return GetActorLocation() + FVector(0.0, 0.0, 70.0);
	}
	const FVector L = Rig->GetRigFrame(Rig->GetControlPath(LeftEye)).GetLocation();
	const FVector R = Rig->GetRigFrame(Rig->GetControlPath(RightEye)).GetLocation();
	return Rig->GetComponentTransform().TransformPosition((L + R) * 0.5);
}

float
ARigExecBipedCharacter::FaceScale() const
{
	// Measured once, from the first frame the rig has: the chin moves with
	// the jaw, and the framing should not breathe with the mouth.
	if (CachedFaceScale > 0.0f)
	{
		return CachedFaceScale;
	}
	if (LeftEye == INDEX_NONE || RightEye == INDEX_NONE || Chin == INDEX_NONE)
	{
		return 1.0f;
	}
	const FVector L = Rig->GetRigFrame(Rig->GetControlPath(LeftEye)).GetLocation();
	const FVector R = Rig->GetRigFrame(Rig->GetControlPath(RightEye)).GetLocation();
	const FVector C = Rig->GetRigFrame(Rig->GetControlPath(Chin)).GetLocation();
	const double EyesToChin = FVector::Dist((L + R) * 0.5, C) * Rig->GetComponentScale().Z;
	if (EyesToChin <= 1.0)
	{
		return 1.0f;
	}
	CachedFaceScale = FMath::Clamp(float(EyesToChin / 11.15), 0.6f, 3.0f);
	return CachedFaceScale;
}

void
ARigExecBipedCharacter::PoseFace(float Lids, float JawDegrees, float Smile)
{
	// A full upper blink is the lid control 2.0 down: where the upper lid
	// meets the lower one on every rig here. Past it a lid without collision
	// keeps turning through the lower lid. The jaw opens about its X.
	for (const int32 Lid : {LeftLid, RightLid})
	{
		if (Lid != INDEX_NONE)
		{
			FMatrix Value = FMatrix::Identity;
			Value.M[3][1] = -2.0 * FMath::Clamp(Lids, 0.0f, 1.0f);
			Rig->SetControlValueMatrix(Lid, Value);
		}
	}
	if (Jaw != INDEX_NONE)
	{
		Rig->SetControlValueMatrix(Jaw, FQuatRotationMatrix(FQuat(FVector(1.0, 0.0, 0.0), FMath::DegreesToRadians(JawDegrees))));
	}
	// The smile: the mouth corners out (their X, mirrored side to side, so
	// the same value on both) and up (Y); the upper lip lifted a little to
	// show the teeth (M_UpLip's Y points down, its sides' up). The values are
	// centimetres on the biped's face, so they grow with a bigger face (the
	// lids need not: 2.0 down is shut on every rig).
	const double K = FaceScale();
	auto Move = [this, K](int32 Control, double X, double Y) {
		if (Control != INDEX_NONE)
		{
			FMatrix Value = FMatrix::Identity;
			Value.M[3][0] = X * K;
			Value.M[3][1] = Y * K;
			Rig->SetControlValueMatrix(Control, Value);
		}
	};
	const double S = FMath::Clamp(Smile, 0.0f, 1.2f);
	Move(LeftCorner, 0.55 * S, 0.75 * S);
	Move(RightCorner, 0.55 * S, 0.75 * S);
	Move(UpLip, 0.0, -0.35 * S);
	Move(LeftUpLip, 0.0, 0.3 * S);
	Move(RightUpLip, 0.0, 0.3 * S);
	// With it the inner brows lift (their Y is up): a pleasantly surprised look.
	Move(LeftInBrow, 0.0, 1.8 * S);
	Move(RightInBrow, 0.0, 1.8 * S);
}

void
ARigExecBipedCharacter::TurnEyes(float Yaw, float Pitch)
{
	// Both eye controls sit in plain asset axes (the right one's mirror is
	// undone by its socket's), so one rotation turns both the same way:
	// about the rig's up for yaw, its side axis for pitch.
	const FMatrix Value = FQuatRotationMatrix(FQuat(FVector(0.0, 1.0, 0.0), FMath::DegreesToRadians(Yaw)) *
	                                          FQuat(FVector(1.0, 0.0, 0.0), FMath::DegreesToRadians(-Pitch)));
	for (const int32 Eye : {LeftEye, RightEye})
	{
		if (Eye != INDEX_NONE)
		{
			Rig->SetControlValueMatrix(Eye, Value);
		}
	}
}

void
ARigExecBipedCharacter::StartCinematic()
{
	APlayerController* Player = Cast<APlayerController>(GetController());
	if (bCinematic || !Player)
	{
		return;
	}
	bCinematic = true;
	bLevelChosen = false;
	CinematicTime = 0.0f;
	GetCharacterMovement()->StopMovementImmediately();
	CameraForward = Rig->GetComponentTransform().TransformVectorNoScale(FVector(0.0, 1.0, 0.0)).GetSafeNormal2D();
	if (!FaceCamera)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		FaceCamera = GetWorld()->SpawnActor<ACameraActor>(Params);
		UCameraComponent* Lens = FaceCamera->GetCameraComponent();
		Lens->SetFieldOfView(30.0f);
		Lens->SetConstraintAspectRatio(false);
		Lens->PostProcessSettings.bOverride_MotionBlurAmount = true;
		Lens->PostProcessSettings.MotionBlurAmount = 0.0f;
		// Lights of its own: a warm key up to one side, a cool soft fill.
		auto AddLight = [this, Lens](const TCHAR* Name, const FVector& At, const FLinearColor& Color, float Candelas,
		                             bool bShadows) {
			UPointLightComponent* Light = NewObject<UPointLightComponent>(FaceCamera, Name);
			Light->SetupAttachment(Lens);
			Light->SetRelativeLocation(At);
			Light->SetIntensityUnits(ELightUnits::Candelas);
			Light->SetIntensity(Candelas);
			Light->SetLightColor(Color);
			Light->SetAttenuationRadius(400.0f);
			Light->SetSourceRadius(12.0f);
			Light->SetCastShadows(bShadows);
			Light->RegisterComponent();
			FaceCamera->AddInstanceComponent(Light);
			FaceLights.Add(Light);
			FaceLightCandelas.Add(Candelas);
		};
		AddLight(TEXT("Key"), FVector(10.0, -55.0, 45.0), FLinearColor(1.0f, 0.9f, 0.8f), 14.0f, true);
		AddLight(TEXT("Fill"), FVector(0.0, 60.0, -5.0), FLinearColor(0.75f, 0.85f, 1.0f), 4.0f, false);
		if (bPortrait)
		{
			// A cool rim from behind, to lift the head off the background.
			AddLight(TEXT("Rim"), FVector::ZeroVector, FLinearColor(0.6f, 0.8f, 1.0f), 30.0f, false);
		}
	}
	BlinkAt = 1.6f;
	NextBlink = 6.0f;
	NextGaze = 4.6f;
	// Eyes in the head, not on the look target out in front.
	if (!LookAtChannel.IsEmpty())
	{
		Rig->SetChannel(LookAtChannel, 0.0f);
	}
	// Already looking down at something in front of him.
	GazeYaw = WantYaw = HeadWantYaw = HeadYaw = 0.0f;
	GazePitch = WantPitch = -14.0f;
	HeadWantPitch = HeadPitch = -14.0f * 0.6f;
	EyesDown = 0.0f;
	DriveCinematic(0.0f);
	// A cut, not a blend.
	Player->SetViewTarget(FaceCamera);
	// The cursor, for the HUD's subdivision buttons; the mouse no longer looks.
	Player->bShowMouseCursor = true;
	Player->bEnableClickEvents = true;
	UE_LOG(LogRigExecBiped, Log, TEXT("cinematic: cut to the face"));
}

void
ARigExecBipedCharacter::EndCinematic()
{
	if (!bCinematic)
	{
		return;
	}
	bCinematic = false;
	SetSubdivisionLevel(0);
	PoseFace(0.0f, 0.0f);
	TurnEyes(0.0f, 0.0f);
	if (!LookAtChannel.IsEmpty())
	{
		Rig->SetChannel(LookAtChannel, 1.0f);
	}
	Retarget->ClearExtraTurns();
	if (APlayerController* Player = Cast<APlayerController>(GetController()))
	{
		Player->SetViewTarget(this);
		Player->bShowMouseCursor = false;
		Player->bEnableClickEvents = false;
	}
	UE_LOG(LogRigExecBiped, Log, TEXT("cinematic: back to play"));
}

ARigExecMenuHUD*
ARigExecBipedCharacter::MenuHUD() const
{
	const APlayerController* Player = Cast<APlayerController>(GetController());
	return Player ? Cast<ARigExecMenuHUD>(Player->GetHUD()) : nullptr;
}

bool
ARigExecBipedCharacter::InMenu() const
{
	const ARigExecMenuHUD* Hud = MenuHUD();
	return Hud && Hud->IsMenuOpen();
}

void
ARigExecBipedCharacter::Back()
{
	ARigExecMenuHUD* Hud = MenuHUD();
	if (Hud && Hud->IsMenuOpen())
	{
		Hud->MenuBack();
	}
	else if (Hud && Hud->HandleBack())
	{
		// The level's HUD took it (the studio: a name being typed, a menu).
	}
	else if (bCinematic)
	{
		EndCinematic();
	}
	else if (Hud)
	{
		Hud->OpenMenu();
	}
}

void
ARigExecBipedCharacter::PausePressed()
{
	if (ARigExecMenuHUD* Hud = MenuHUD())
	{
		Hud->ToggleMenu();
	}
}

void
ARigExecBipedCharacter::DriveCinematic(float DeltaSeconds)
{
	const float Before = CinematicTime;
	CinematicTime += DeltaSeconds;
	const float T = CinematicTime;
	// A moment on the plain cage, then the subdivided surface -- unless the
	// level has been chosen by hand.
	if (Before < 1.2f && T >= 1.2f && !bLevelChosen)
	{
		SetSubdivisionLevel(1);
		bLevelChosen = false;
	}

	// Blinks: quick down, a beat shut, slower up.
	if (T >= NextBlink && T > 5.0f)
	{
		BlinkAt = T;
		// Now and then a double blink.
		NextBlink = T + (Random.FRand() < 0.2f ? 0.38f : Random.FRandRange(2.2f, 4.8f));
	}
	const float Since = T - BlinkAt;
	float Lids = 0.0f;
	if (Since >= 0.0f && Since < 0.07f)
	{
		Lids = Since / 0.07f;
	}
	else if (Since >= 0.07f && Since < 0.12f)
	{
		Lids = 1.0f;
	}
	else if (Since >= 0.12f && Since < 0.27f)
	{
		Lids = 1.0f - (Since - 0.12f) / 0.15f;
	}
	Lids = FMath::InterpEaseInOut(0.0f, 1.0f, FMath::Clamp(Lids, 0.0f, 1.0f), 2.0f);
	// The upper lids ride down with the eyes.
	Lids = Lids + (1.0f - Lids) * 0.32f * EyesDown;

	// The jaw: opens after the blink, holds, settles to just parted.
	float JawOpen = 0.0f;
	if (T >= 2.3f && T < 3.0f)
	{
		JawOpen = FMath::InterpEaseInOut(0.0f, 1.0f, (T - 2.3f) / 0.7f, 2.0f);
	}
	else if (T >= 3.0f && T < 4.2f)
	{
		JawOpen = 1.0f;
	}
	else if (T >= 4.2f)
	{
		JawOpen = FMath::InterpEaseInOut(1.0f, 0.12f, FMath::Min((T - 4.2f) / 0.8f, 1.0f), 2.0f);
	}
	// The smile: grows in as the jaw settles, then breathes a little.
	float Smile = 0.0f;
	if (T >= 4.4f)
	{
		Smile = FMath::InterpEaseInOut(0.0f, 1.0f, FMath::Min((T - 4.4f) / 0.9f, 1.0f), 2.0f) *
		        (0.92f + 0.08f * FMath::Sin((T - 4.4f) * 0.9f));
	}
	PoseFace(Lids, 12.0f * JawOpen, Smile);

	// Looking at something low in front of him, and around it: the eyes
	// get to each new spot in a few frames, the head sets off after them a
	// beat later and the eyes ease back toward centre as it arrives.
	if (T >= NextGaze)
	{
		WantYaw = Random.FRandRange(-24.0f, 24.0f);
		// Mostly down; now and then a glance up.
		WantPitch = Random.FRand() < 0.22f ? Random.FRandRange(-3.0f, 4.0f) : Random.FRandRange(-26.0f, -9.0f);
		NextGaze = T + Random.FRandRange(1.1f, 2.4f);
		HeadFollowAt = T + Random.FRandRange(0.15f, 0.25f);
		// A big look often comes with a blink.
		if (FMath::Abs(WantYaw - GazeYaw) > 22.0f && T - BlinkAt > 0.8f && Random.FRand() < 0.6f)
		{
			BlinkAt = T;
		}
	}
	if (T >= HeadFollowAt)
	{
		HeadWantYaw = WantYaw * 0.7f;
		HeadWantPitch = WantPitch * 0.6f;
	}
	// Small fixational movements, so the eyes never freeze.
	if (T >= NextJitter)
	{
		JitterYaw = Random.FRandRange(-0.7f, 0.7f);
		JitterPitch = Random.FRandRange(-0.5f, 0.5f);
		NextJitter = T + Random.FRandRange(0.3f, 0.7f);
	}
	if (DeltaSeconds > 0.0f)
	{
		GazeYaw = FMath::FInterpTo(GazeYaw, WantYaw, DeltaSeconds, 30.0f);
		GazePitch = FMath::FInterpTo(GazePitch, WantPitch, DeltaSeconds, 30.0f);
		HeadYaw = FMath::FInterpTo(HeadYaw, HeadWantYaw, DeltaSeconds, 3.2f);
		HeadPitch = FMath::FInterpTo(HeadPitch, HeadWantPitch, DeltaSeconds, 3.2f);
	}
	// The eyes in the head: the gaze less what the head has already turned.
	const float EyeYaw = FMath::Clamp(GazeYaw - HeadYaw + JitterYaw, -28.0f, 28.0f);
	const float EyePitch = FMath::Clamp(GazePitch - HeadPitch + JitterPitch, -22.0f, 16.0f);
	TurnEyes(EyeYaw, EyePitch);
	EyesDown = FMath::Clamp(-EyePitch / 20.0f, 0.0f, 1.0f);
	auto Turn = [](float Yaw, float Pitch) {
		return FQuat(FVector(0.0, 1.0, 0.0), FMath::DegreesToRadians(Yaw)) *
		       FQuat(FVector(1.0, 0.0, 0.0), FMath::DegreesToRadians(-Pitch));
	};
	// The neck takes a third of the turn, the head the whole of it.
	Retarget->SetExtraTurn(TEXT("M_Neck"), Turn(HeadYaw * 0.35f, HeadPitch * 0.35f));
	Retarget->SetExtraTurn(TEXT("M_Head"), Turn(HeadYaw, HeadPitch));

	// The camera: in front of the face, a touch to one side, easing in.
	if (FaceCamera)
	{
		const FVector Face = FaceCentre();
		CameraFace = DeltaSeconds > 0.0f ? FMath::VInterpTo(CameraFace, Face, DeltaSeconds, 3.0f) : Face;
		const FVector Up(0.0, 0.0, 1.0);
		const FVector Right = FVector::CrossProduct(Up, CameraForward);
		const float Push = FMath::InterpEaseInOut(0.0f, 1.0f, FMath::Clamp(T / 9.0f, 0.0f, 1.0f), 2.0f);
		// Framed on the biped's face; a bigger face, the camera further back.
		const float K = FaceScale();
		const float Distance = FMath::Lerp(115.0f, 78.0f, Push) * K;
		const FVector Eye = CameraFace + CameraForward * Distance + Right * (Distance * 0.16f) + Up * (2.0f * K);
		const FVector Aim = CameraFace - Up * (4.0f * K);
		// The lights ride the camera: as far again from the face, as bright.
		for (int32 Each = 0; Each < FaceLights.Num() && Each < FaceLightCandelas.Num(); ++Each)
		{
			FaceLights[Each]->SetIntensity(FaceLightCandelas[Each] * K * K);
			FaceLights[Each]->SetAttenuationRadius(400.0f * K);
		}
		FaceCamera->SetActorLocationAndRotation(Eye, (Aim - Eye).Rotation());
		UCameraComponent* Lens = FaceCamera->GetCameraComponent();
		Lens->PostProcessSettings.bOverride_DepthOfFieldFocalDistance = true;
		Lens->PostProcessSettings.DepthOfFieldFocalDistance = float((Aim - Eye).Size());
		Lens->PostProcessSettings.bOverride_DepthOfFieldFstop = true;
		Lens->PostProcessSettings.DepthOfFieldFstop = 2.0f;
	}
}

void
ARigExecBipedCharacter::DrivePadDemo(float Before)
{
	const float Dt = PlayTime - Before;
	if (bCinematic)
	{
		// What a viewer would do: step the level up, down to the cage, back
		// to 1, then Esc.
		const float T = CinematicTime, B = T - Dt;
		auto At = [T, B](float S) { return B < S && T >= S; };
		if (At(9.0f))
		{
			SubdivisionUp();
		}
		if (At(11.5f))
		{
			SetSubdivisionLevel(0);
		}
		if (At(13.5f))
		{
			SetSubdivisionLevel(1);
		}
		if (T >= 17.0f)
		{
			EndCinematic();
			CinematicDone = PlayTime;
		}
		return;
	}
	if (CinematicDone >= 0.0f)
	{
		// Off the pad and away, then done.
		AddMovementInput(FVector(1.0, 0.7, 0.0).GetSafeNormal(), 1.0f);
		if (Controller)
		{
			Controller->SetControlRotation(
			    FMath::RInterpTo(Controller->GetControlRotation(), FRotator(-14.0, 20.0, 0.0), Dt, 3.0f));
		}
		QuitSoon(3.0f);
		return;
	}
	if (PlayTime < 1.0f || RouteAt >= Route.Num())
	{
		return;
	}
	FVector To = Route[RouteAt] - GetActorLocation();
	To.Z = 0.0;
	const bool bLast = RouteAt + 1 == Route.Num();
	if (To.Size() < (bLast ? 20.0 : 90.0))
	{
		++RouteAt;
		if (RouteAt == 2)
		{
			Jump();
		}
		return;
	}
	// The last stretch at a walk.
	AddMovementInput(To.GetSafeNormal(), bLast && To.Size() < 260.0 ? 0.3f : 1.0f);
	if (Controller)
	{
		// The camera swings round behind.
		const FRotator Behind(-14.0, GetActorRotation().Yaw - 20.0, 0.0);
		Controller->SetControlRotation(FMath::RInterpTo(Controller->GetControlRotation(), Behind, Dt, 2.0f));
	}
}

void
ARigExecBipedCharacter::DrivePortrait(float Before)
{
	APlayerController* Player = Cast<APlayerController>(GetController());
	if (!bCinematic)
	{
		if (PlayTime < 0.3f)
		{
			return;
		}
		StartCinematic();
		SetSubdivisionLevel(2);
		if (Player && Player->GetHUD())
		{
			Player->GetHUD()->bShowHUD = false;
		}
		if (FaceCamera)
		{
			UCameraComponent* Lens = FaceCamera->GetCameraComponent();
			Lens->SetFieldOfView(24.0f);
			Lens->PostProcessSettings.bOverride_VignetteIntensity = true;
			Lens->PostProcessSettings.VignetteIntensity = 0.55f;
		}
	}
	if (!FaceCamera || FaceLights.Num() < 3)
	{
		return;
	}
	// The angles: the camera round to one side of the face (degrees, toward
	// his left positive), the head turned partway to it and tilted.
	struct FShot
	{
		float Angle, HeadYaw, HeadPitch, Tilt, Fov, AimDown;
	};
	// The first is the chosen one (the loading screen's); the others frame
	// it a little wider, so the top of the head stays in.
	static const FShot Shots[] = {{14.0f, 6.0f, 1.0f, 3.0f, 17.0f, 7.0f}, {14.0f, 6.0f, 1.0f, 3.0f, 20.0f, 3.0f},
	                              {14.0f, 6.0f, 1.0f, 3.0f, 23.0f, 1.0f}, {18.0f, 7.0f, 1.0f, 3.0f, 20.0f, 3.0f}};
	const int32 Count = UE_ARRAY_COUNT(Shots);
	const int32 Index = FMath::Clamp(FMath::FloorToInt((PlayTime - 1.0f) / 3.0f), 0, Count - 1);
	const FShot& Shot = Shots[Index];

	// A warm, relaxed smile, the lids a touch low, eyes on the lens.
	PoseFace(0.12f, 2.5f, 0.85f);
	auto Turn = [](float Yaw, float Pitch, float Roll) {
		return FQuat(FVector(0.0, 1.0, 0.0), FMath::DegreesToRadians(Yaw)) *
		       FQuat(FVector(1.0, 0.0, 0.0), FMath::DegreesToRadians(-Pitch)) *
		       FQuat(FVector(0.0, 0.0, 1.0), FMath::DegreesToRadians(Roll));
	};
	Retarget->SetExtraTurn(TEXT("M_Neck"), Turn(Shot.HeadYaw * 0.35f, Shot.HeadPitch * 0.35f, Shot.Tilt * 0.3f));
	Retarget->SetExtraTurn(TEXT("M_Head"), Turn(Shot.HeadYaw, Shot.HeadPitch, Shot.Tilt));
	TurnEyes(Shot.Angle - Shot.HeadYaw * 1.35f, 3.0f - Shot.HeadPitch * 1.35f);

	const FVector Face = FaceCentre();
	const float K = FaceScale();
	const FVector Up(0.0, 0.0, 1.0);
	const FVector Left = -FVector::CrossProduct(Up, CameraForward);
	auto Round = [&](float Degrees) {
		const float R = FMath::DegreesToRadians(Degrees);
		return CameraForward * FMath::Cos(R) + Left * FMath::Sin(R);
	};
	const float Distance = 175.0f * K;
	const FVector Eye = Face + Round(Shot.Angle) * Distance + Up * (3.0f * K);
	const FVector Aim = Face - Up * (Shot.AimDown * K);
	FaceCamera->SetActorLocationAndRotation(Eye, (Aim - Eye).Rotation());
	UCameraComponent* Lens = FaceCamera->GetCameraComponent();
	Lens->SetFieldOfView(Shot.Fov);
	Lens->PostProcessSettings.bOverride_DepthOfFieldFocalDistance = true;
	Lens->PostProcessSettings.DepthOfFieldFocalDistance = float((Face - Eye).Size());
	Lens->PostProcessSettings.bOverride_DepthOfFieldFstop = true;
	Lens->PostProcessSettings.DepthOfFieldFstop = 1.4f;
	// Short lighting: the key on the side turned away from the camera, the
	// fill low from the camera's side, the rim behind on the key's.
	const float Side = Shot.Angle >= 0.0f ? 1.0f : -1.0f;
	FaceLights[0]->SetWorldLocation(Face + (Round(-38.0f * Side) * 80.0f + Up * 50.0f) * K);
	FaceLights[1]->SetWorldLocation(Face + (Round(Shot.Angle + 20.0f * Side) * 95.0f - Up * 8.0f) * K);
	FaceLights[2]->SetWorldLocation(Face + (-CameraForward * 55.0f - Left * (45.0f * Side) + Up * 35.0f) * K);

	// A shot near the end of each angle, then done.
	for (int32 Each = 0; Each < Count; ++Each)
	{
		const float At = 1.0f + 3.0f * (Each + 1) - 0.4f;
		if (Before < At && PlayTime >= At && Player)
		{
			const FString Path =
			    FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RigExecPortrait"), FString::Printf(TEXT("portrait_%d.png"), Each));
			Player->ConsoleCommand(FString::Printf(TEXT("HighResShot 2 filename=%s"), *FPaths::ConvertRelativePathToFull(Path)));
			UE_LOG(LogRigExecBiped, Log, TEXT("portrait %s"), *Path);
		}
	}
	if (PlayTime >= 1.0f + 3.0f * Count + 0.5f)
	{
		QuitSoon(0.5f);
	}
}

void
ARigExecBipedCharacter::ApplySourceVisibility()
{
	// Shown, the Mannequin stands a metre to the side so the two can be compared.
	GetMesh()->SetVisibility(bShowSource);
	GetMesh()->SetRelativeLocation(GFeet + (bShowSource ? FVector(0.0, 100.0, 0.0) : FVector::ZeroVector));
}

void
ARigExecBipedCharacter::ToggleSource()
{
	bShowSource = !bShowSource;
	ApplySourceVisibility();
}

namespace
{
UEnhancedInputLocalPlayerSubsystem*
InputOf(AController* Controller)
{
	APlayerController* Player = Cast<APlayerController>(Controller);
	return Player ? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Player->GetLocalPlayer()) : nullptr;
}
} // namespace

void
ARigExecBipedCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	// Taken again after being left (its input component, and so this mapping,
	// may outlive the swap): put its keys back.
	if (Mapping)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Input = InputOf(NewController))
		{
			Input->AddMappingContext(Mapping, 0);
		}
	}
}

void
ARigExecBipedCharacter::UnPossessed()
{
	if (Mapping)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Input = InputOf(GetController()))
		{
			Input->RemoveMappingContext(Mapping);
		}
	}
	Super::UnPossessed();
}

void
ARigExecBipedCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	// Set up again (a pawn possessed a second time): the old mapping goes first.
	if (Mapping)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Input = InputOf(GetController()))
		{
			Input->RemoveMappingContext(Mapping);
		}
	}
	// The actions and their keys are made here rather than as assets, so the
	// example carries no input content of its own.
	MoveAction = NewObject<UInputAction>(this, TEXT("IA_Move"));
	MoveAction->ValueType = EInputActionValueType::Axis2D;
	LookAction = NewObject<UInputAction>(this, TEXT("IA_Look"));
	LookAction->ValueType = EInputActionValueType::Axis2D;
	JumpAction = NewObject<UInputAction>(this, TEXT("IA_Jump"));
	JumpAction->ValueType = EInputActionValueType::Boolean;
	ToggleSourceAction = NewObject<UInputAction>(this, TEXT("IA_ToggleSource"));
	ToggleSourceAction->ValueType = EInputActionValueType::Boolean;
	BackAction = NewObject<UInputAction>(this, TEXT("IA_Back"));
	BackAction->ValueType = EInputActionValueType::Boolean;
	// The menu's: these work while the game is paused.
	BackAction->bTriggerWhenPaused = true;
	PauseAction = NewObject<UInputAction>(this, TEXT("IA_Pause"));
	MenuUpAction = NewObject<UInputAction>(this, TEXT("IA_MenuUp"));
	MenuDownAction = NewObject<UInputAction>(this, TEXT("IA_MenuDown"));
	MenuSelectAction = NewObject<UInputAction>(this, TEXT("IA_MenuSelect"));
	for (UInputAction* Action : {PauseAction.Get(), MenuUpAction.Get(), MenuDownAction.Get(), MenuSelectAction.Get()})
	{
		Action->ValueType = EInputActionValueType::Boolean;
		Action->bTriggerWhenPaused = true;
	}
	SubdUpAction = NewObject<UInputAction>(this, TEXT("IA_SubdUp"));
	SubdUpAction->ValueType = EInputActionValueType::Boolean;
	SubdDownAction = NewObject<UInputAction>(this, TEXT("IA_SubdDown"));
	SubdDownAction->ValueType = EInputActionValueType::Boolean;
	for (int32 Level = 0; Level <= MaxSubdivisionLevel; ++Level)
	{
		SubdLevelActions.Add(NewObject<UInputAction>(this, *FString::Printf(TEXT("IA_Subd%d"), Level)));
	}

	Mapping = NewObject<UInputMappingContext>(this, TEXT("IMC_RigExecBiped"));
	auto Swizzle = [this]() { return NewObject<UInputModifierSwizzleAxis>(this); };
	auto Negate = [this]() { return NewObject<UInputModifierNegate>(this); };
	// Move: X is right, Y is forward.
	Mapping->MapKey(MoveAction, EKeys::W).Modifiers.Add(Swizzle());
	{
		FEnhancedActionKeyMapping& S = Mapping->MapKey(MoveAction, EKeys::S);
		S.Modifiers.Add(Swizzle());
		S.Modifiers.Add(Negate());
	}
	Mapping->MapKey(MoveAction, EKeys::D);
	Mapping->MapKey(MoveAction, EKeys::A).Modifiers.Add(Negate());
	Mapping->MapKey(MoveAction, EKeys::Gamepad_Left2D);
	{
		FEnhancedActionKeyMapping& Mouse = Mapping->MapKey(LookAction, EKeys::Mouse2D);
		UInputModifierNegate* Up = Negate();
		Up->bX = false;
		Up->bZ = false;
		Mouse.Modifiers.Add(Up);
	}
	{
		FEnhancedActionKeyMapping& Stick = Mapping->MapKey(LookAction, EKeys::Gamepad_Right2D);
		UInputModifierNegate* Up = Negate();
		Up->bX = false;
		Up->bZ = false;
		Stick.Modifiers.Add(Up);
	}
	Mapping->MapKey(JumpAction, EKeys::SpaceBar);
	Mapping->MapKey(JumpAction, EKeys::Gamepad_FaceButton_Bottom);
	Mapping->MapKey(ToggleSourceAction, EKeys::M);
	Mapping->MapKey(BackAction, EKeys::Escape);
	Mapping->MapKey(BackAction, EKeys::Gamepad_FaceButton_Right);
	Mapping->MapKey(PauseAction, EKeys::P);
	Mapping->MapKey(PauseAction, EKeys::Gamepad_Special_Right);
	Mapping->MapKey(MenuUpAction, EKeys::Up);
	Mapping->MapKey(MenuUpAction, EKeys::Gamepad_DPad_Up);
	Mapping->MapKey(MenuUpAction, EKeys::W);
	Mapping->MapKey(MenuDownAction, EKeys::Down);
	Mapping->MapKey(MenuDownAction, EKeys::Gamepad_DPad_Down);
	Mapping->MapKey(MenuDownAction, EKeys::S);
	// Across, for the lobby's row of characters.
	Mapping->MapKey(MenuUpAction, EKeys::Left);
	Mapping->MapKey(MenuUpAction, EKeys::A);
	Mapping->MapKey(MenuUpAction, EKeys::Gamepad_DPad_Left);
	Mapping->MapKey(MenuDownAction, EKeys::Right);
	Mapping->MapKey(MenuDownAction, EKeys::D);
	Mapping->MapKey(MenuDownAction, EKeys::Gamepad_DPad_Right);
	Mapping->MapKey(MenuSelectAction, EKeys::Enter);
	Mapping->MapKey(MenuSelectAction, EKeys::SpaceBar);
	Mapping->MapKey(MenuSelectAction, EKeys::Gamepad_FaceButton_Bottom);
	// The subdivision level, live: - and = (or the d-pad) step it, 0-4 set it.
	Mapping->MapKey(SubdUpAction, EKeys::Equals);
	Mapping->MapKey(SubdUpAction, EKeys::Add);
	Mapping->MapKey(SubdUpAction, EKeys::Gamepad_DPad_Up);
	Mapping->MapKey(SubdDownAction, EKeys::Hyphen);
	Mapping->MapKey(SubdDownAction, EKeys::Subtract);
	Mapping->MapKey(SubdDownAction, EKeys::Gamepad_DPad_Down);
	{
		const FKey Keys[3] = {EKeys::Zero, EKeys::One, EKeys::Two};
		for (int32 Level = 0; Level <= MaxSubdivisionLevel; ++Level)
		{
			Mapping->MapKey(SubdLevelActions[Level], Keys[Level]);
		}
	}

	if (APlayerController* Player = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Input =
		        ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Player->GetLocalPlayer()))
		{
			Input->AddMappingContext(Mapping, 0);
		}
	}
	if (UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ARigExecBipedCharacter::Move);
		Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &ARigExecBipedCharacter::Look);
		Input->BindAction(JumpAction, ETriggerEvent::Started, this, &ARigExecBipedCharacter::JumpPressed);
		Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &ARigExecBipedCharacter::JumpReleased);
		Input->BindAction(BackAction, ETriggerEvent::Started, this, &ARigExecBipedCharacter::Back);
		Input->BindAction(PauseAction, ETriggerEvent::Started, this, &ARigExecBipedCharacter::PausePressed);
		Input->BindActionValueLambda(MenuUpAction, ETriggerEvent::Started, [this](const FInputActionValue&) {
			if (ARigExecMenuHUD* Hud = MenuHUD())
			{
				Hud->MenuMove(-1);
			}
		});
		Input->BindActionValueLambda(MenuDownAction, ETriggerEvent::Started, [this](const FInputActionValue&) {
			if (ARigExecMenuHUD* Hud = MenuHUD())
			{
				Hud->MenuMove(1);
			}
		});
		Input->BindActionValueLambda(MenuSelectAction, ETriggerEvent::Started, [this](const FInputActionValue&) {
			if (ARigExecMenuHUD* Hud = MenuHUD())
			{
				Hud->MenuSelect();
			}
		});
		Input->BindAction(SubdUpAction, ETriggerEvent::Started, this, &ARigExecBipedCharacter::SubdivisionUp);
		Input->BindAction(SubdDownAction, ETriggerEvent::Started, this, &ARigExecBipedCharacter::SubdivisionDown);
		for (int32 Level = 0; Level < SubdLevelActions.Num(); ++Level)
		{
			Input->BindActionValueLambda(SubdLevelActions[Level], ETriggerEvent::Started,
			                             [this, Level](const FInputActionValue&) { SetSubdivisionLevel(Level); });
		}
		Input->BindAction(ToggleSourceAction, ETriggerEvent::Started, this, &ARigExecBipedCharacter::ToggleSource);
	}
	else
	{
		UE_LOG(LogRigExecBiped, Error, TEXT("the project is not using Enhanced Input; no controls"));
	}
}

void
ARigExecBipedCharacter::Move(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (!Controller || bCinematic || bStudio || InMenu())
	{
		return;
	}
	const FRotator Yaw(0.0, Controller->GetControlRotation().Yaw, 0.0);
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::X), Axis.Y);
	AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y), Axis.X);
}

void
ARigExecBipedCharacter::Look(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (bCinematic || bStudio || InMenu())
	{
		return;
	}
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

void
ARigExecBipedCharacter::JumpPressed()
{
	if (!bCinematic && !bStudio && !InMenu())
	{
		Jump();
	}
}

void
ARigExecBipedCharacter::JumpReleased()
{
	StopJumping();
}

void
ARigExecBipedCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// Level of detail: every frame for the close-up, less often from afar.
	if (Rig && !bStudio)
	{
		// (The studio sets its own: see ARigExecStudioHUD::Tick.)
		Rig->bEvaluateAsync = bRigAsync;
		// Behind a menu or the lobby the player's own rig is not seen: rest
		// it. The studio is posing it: every frame.
		const int32 Rate = bPreview ? RigRateLobby : (bCinematic || bPortrait) ? 1 : InMenu() ? 8 : RigRateFar;
		Rig->EvaluateEvery = FMath::Clamp(Rate, 1, 8);
	}
	// The menu (or the lobby) holds the player's demo clock.
	if (InMenu())
	{
		return;
	}
	const float Before = PlayTime;
	PlayTime += DeltaSeconds;
	if (bDemo)
	{
		DriveDemo(Before);
	}
	if (Route.Num() > 0)
	{
		DrivePadDemo(Before);
	}
	if (bPortrait)
	{
		DrivePortrait(Before);
	}
	else if (bCinematic)
	{
		DriveCinematic(DeltaSeconds);
	}
	Capture(Before);
}

void
ARigExecBipedCharacter::DriveDemo(float Before)
{
	// Runs a slow circle (walk, then run), jumping now and then, with the
	// camera swinging round so every side is seen; logs how the retarget
	// keeps up.
	const float Speed = PlayTime < 6.0f ? 0.35f : 1.0f;
	const FRotator Heading(0.0, PlayTime * 25.0, 0.0);
	AddMovementInput(Heading.Vector(), Speed);
	if (Controller)
	{
		Controller->SetControlRotation(FRotator(-15.0, PlayTime * 25.0 + 120.0, 0.0));
	}
	if (FMath::Fmod(PlayTime, 7.0f) < FMath::Fmod(Before, 7.0f))
	{
		Jump();
	}
	if (FMath::FloorToInt(PlayTime) != FMath::FloorToInt(Before))
	{
		UE_LOG(LogRigExecBiped, Log, TEXT("demo t=%.0f speed=%.0f: frame %.1f ms, rig run %.1f ms, publish %.1f ms, rate 1/%d, retarget %.2f ms, error %.3f deg"),
		       PlayTime, GetVelocity().Size2D(), FApp::GetDeltaTime() * 1000.0, Rig->LastExecuteMs, Rig->LastPublishMs,
		       Rig->EvaluateEvery, Retarget->LastMilliseconds, Retarget->LastErrorDegrees);
	}
}

void
ARigExecBipedCharacter::QuitSoon(float Seconds)
{
	if (QuitAt < 0.0f)
	{
		QuitAt = PlayTime + Seconds;
	}
}

void
ARigExecBipedCharacter::Capture(float Before)
{
	// -RigExecShots: a screenshot every 2.5 s.
	if (FParse::Param(FCommandLine::Get(), TEXT("RigExecShots")) && FMath::Fmod(PlayTime, 2.5f) < FMath::Fmod(Before, 2.5f))
	{
		const FString Shot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RigExecShots"),
		                                     FString::Printf(TEXT("shot_%04.1f.png"), PlayTime));
		FScreenshotRequest::RequestScreenshot(Shot, true, false);
		UE_LOG(LogRigExecBiped, Log, TEXT("screenshot %s"), *Shot);
	}
	// -RigExecFrames: every frame to Saved/RigExecFrames, for a video (run
	// with -UseFixedTimeStep -FPS=30 so each frame is 1/30 s of play).
	if (FParse::Param(FCommandLine::Get(), TEXT("RigExecFrames")))
	{
		const FString Frame = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RigExecFrames"),
		                                      FString::Printf(TEXT("frame_%05d.png"), FrameCount++));
		FScreenshotRequest::RequestScreenshot(Frame, true, false);
	}
	// -RigExecQuitAfter=<s>, or a mode that is done (QuitSoon).
	float Quit = 0.0f;
	const bool bTimeUp = FParse::Value(FCommandLine::Get(), TEXT("RigExecQuitAfter="), Quit) && PlayTime >= Quit && Before < Quit;
	const bool bDone = QuitAt >= 0.0f && PlayTime >= QuitAt && Before < QuitAt;
	if (bTimeUp || bDone)
	{
		UE_LOG(LogRigExecBiped, Log, TEXT("RIGEXEC_DEMO_DONE"));
		UKismetSystemLibrary::QuitGame(this, nullptr, EQuitPreference::Quit, false);
	}
}
