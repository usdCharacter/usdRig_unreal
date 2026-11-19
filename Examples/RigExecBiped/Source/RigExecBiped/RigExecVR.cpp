#include "RigExecVR.h"

#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "IXRTrackingSystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "MotionControllerComponent.h"
#include "StereoRendering.h"
#include "TextureResource.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"

namespace
{
/** Watches every controller key on its way into Slate (and lets it through). */
class FRigExecXRInput : public IInputProcessor
{
public:
	TMap<FName, float> Axis;
	TSet<FName> Down, Pending, Pressed;

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
	{
		if (Event.GetKey().IsGamepadKey())
		{
			Down.Add(Event.GetKey().GetFName());
			if (!Event.IsRepeat())
			{
				Pending.Add(Event.GetKey().GetFName());
			}
		}
		return false;
	}
	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
	{
		Down.Remove(Event.GetKey().GetFName());
		return false;
	}
	virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& Event) override
	{
		Axis.Add(Event.GetKey().GetFName(), Event.GetAnalogValue());
		return false;
	}
	virtual const TCHAR* GetDebugName() const override { return TEXT("RigExecXRInput"); }
};
TSharedPtr<FRigExecXRInput> GXRInput;
} // namespace

bool ARigExecVRRig::XRKeyDown(const FKey& Key)
{
	return GXRInput.IsValid() && GXRInput->Down.Contains(Key.GetFName());
}

float ARigExecVRRig::XRKeyAxis(const FKey& Key)
{
	const float* Value = GXRInput.IsValid() ? GXRInput->Axis.Find(Key.GetFName()) : nullptr;
	return Value ? *Value : 0.0f;
}

bool ARigExecVRRig::XRKeyPressed(const FKey& Key)
{
	return GXRInput.IsValid() && GXRInput->Pressed.Contains(Key.GetFName());
}

void ARigExecVRRig::XRInputFrame()
{
	if (GXRInput.IsValid())
	{
		GXRInput->Pressed = MoveTemp(GXRInput->Pending);
		GXRInput->Pending.Reset();
	}
}

FString ARigExecVRRig::XRInputReport()
{
	if (!GXRInput.IsValid())
	{
		return TEXT("(not listening)");
	}
	FString Out = TEXT("held:");
	for (const FName& Key : GXRInput->Down)
	{
		Out += TEXT(" ") + Key.ToString();
	}
	Out += TEXT(" axes:");
	for (const TPair<FName, float>& Each : GXRInput->Axis)
	{
		if (FMath::Abs(Each.Value) > 0.05f)
		{
			Out += FString::Printf(TEXT(" %s=%.2f"), *Each.Key.ToString(), Each.Value);
		}
	}
	Out += FString::Printf(TEXT(" (%d axes seen)"), GXRInput->Axis.Num());
	return Out;
}

namespace
{
/** The panel's width, as an angle seen from where the player stands. */
constexpr double GPanelDegrees = 76.0;
/** The desktop stand-in's eye height. */
constexpr double GSimEyeHeight = 160.0;

UMaterialInterface* LoadMaterial(const TCHAR* Path)
{
	return LoadObject<UMaterialInterface>(nullptr, Path);
}
} // namespace

bool ARigExecVRRig::bPaintingPanel = false;

ARigExecVRRig::ARigExecVRRig()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	// The tracking space: the headset and hands move in it, it moves with the stage.
	Origin = CreateDefaultSubobject<USceneComponent>(TEXT("Origin"));
	Origin->SetupAttachment(RootComponent);
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(Origin);
	Camera->bLockToHmd = true;
	Camera->SetFieldOfView(90.0f);
	// Bloom close up in a headset smears the panel's text: kept light.
	Camera->PostProcessSettings.bOverride_BloomIntensity = true;
	Camera->PostProcessSettings.BloomIntensity = 0.15f;
	LeftHand = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("LeftHand"));
	LeftHand->SetupAttachment(Origin);
	LeftHand->SetTrackingMotionSource(FName(TEXT("LeftAim")));
	RightHand = CreateDefaultSubobject<UMotionControllerComponent>(TEXT("RightHand"));
	RightHand->SetupAttachment(Origin);
	RightHand->SetTrackingMotionSource(FName(TEXT("RightAim")));

	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	auto Laser = [this, Cylinder](const TCHAR* Name) {
		UStaticMeshComponent* Mesh = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Mesh->SetupAttachment(RootComponent);
		Mesh->SetStaticMesh(Cylinder);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCastShadow(false);
		Mesh->SetVisibility(false);
		return Mesh;
	};
	LeftLaser = Laser(TEXT("LeftLaser"));
	RightLaser = Laser(TEXT("RightLaser"));
	Panel = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Panel"));
	Panel->SetupAttachment(RootComponent);
	Panel->SetStaticMesh(Plane);
	Panel->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Panel->SetCastShadow(false);
	Panel->SetTranslucentSortPriority(100);
}

bool ARigExecVRRig::HeadsetOn()
{
	return GEngine && GEngine->XRSystem.IsValid() && GEngine->StereoRenderingDevice.IsValid() &&
	       GEngine->StereoRenderingDevice->IsStereoEnabled();
}

bool ARigExecVRRig::Simulated()
{
	static const bool bSim = FParse::Param(FCommandLine::Get(), TEXT("RigExecVRSim"));
	return bSim;
}

void ARigExecVRRig::BeginPlay()
{
	Super::BeginPlay();
	if (!GXRInput.IsValid() && FSlateApplication::IsInitialized())
	{
		GXRInput = MakeShared<FRigExecXRInput>();
		FSlateApplication::Get().RegisterInputPreProcessor(GXRInput, 0);
	}
	if (GEngine && GEngine->XRSystem.IsValid())
	{
		// Standing: the floor is the floor.
		GEngine->XRSystem->SetTrackingOrigin(EHMDTrackingOrigin::LocalFloor);
	}
	const FIntPoint Size = Pixels();
	// Cleared to see-through each frame, and what the HUD draws is
	// premultiplied over it. The clear colour is fixed when the texture is
	// made, so it is set first.
	Target = NewObject<UCanvasRenderTarget2D>(this);
	Target->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	Target->InitAutoFormat(Size.X, Size.Y);
	Target->UpdateResourceImmediate(true);
	if (UMaterialInterface* Material = LoadMaterial(TEXT("/Game/RigExec/VR/M_RigExecVRPanel.M_RigExecVRPanel")))
	{
		PanelMaterial = UMaterialInstanceDynamic::Create(Material, this);
		PanelMaterial->SetTextureParameterValue(TEXT("Panel"), Target);
		Panel->SetMaterial(0, PanelMaterial);
	}
	if (UMaterialInterface* Material = LoadMaterial(TEXT("/Game/RigExec/VR/M_RigExecVRLaser.M_RigExecVRLaser")))
	{
		for (int32 Hand = 0; Hand < 2; ++Hand)
		{
			LaserMaterial[Hand] = UMaterialInstanceDynamic::Create(Material, this);
			(Hand == 0 ? LeftLaser : RightLaser)->SetMaterial(0, LaserMaterial[Hand]);
		}
	}
}

void ARigExecVRRig::EndPlay(const EEndPlayReason::Type Reason)
{
	if (GXRInput.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(GXRInput);
	}
	GXRInput.Reset();
	Super::EndPlay(Reason);
}

void ARigExecVRRig::Stage(const FVector& Focus, float Yaw, float Distance, float FloorZ)
{
	const FRotator Facing(0.0f, Yaw, 0.0f);
	const FRotationMatrix Axes(Facing);
	const FVector Ahead = Axes.GetUnitAxis(EAxis::X);
	FVector Stand = Focus - Ahead * Distance;
	Stand.Z = FloorZ;
	// A runtime without a floor-level space puts the tracking origin at the
	// eyes, not the floor: then the floor is a standing eye height below
	// where the head was first seen. Decided once, from the first pose.
	FQuat HeadTurn;
	FVector Head;
	if (HeadsetOn() && FloorLift < 0.0 && GEngine->XRSystem->GetCurrentPose(IXRTrackingSystem::HMDDeviceId, HeadTurn, Head))
	{
		FloorLift = Head.Z < 60.0 ? GSimEyeHeight - Head.Z : 0.0;
		UE_LOG(LogTemp, Log, TEXT("RigExec VR: head first seen %.0f cm above the tracking origin: %s"), Head.Z,
		       FloorLift > 0.0 ? TEXT("an eye-level origin, the floor put below it") : TEXT("a floor-level origin"));
	}
	Stand.Z += FMath::Max(FloorLift, 0.0);
	Origin->SetWorldLocationAndRotation(Stand, Facing);
	// On the desktop the camera stands in for a person standing there.
	Camera->SetRelativeLocation(HeadsetOn() ? FVector::ZeroVector : FVector(0.0, 0.0, GSimEyeHeight));
	if (!HeadsetOn())
	{
		Camera->SetRelativeRotation(FRotator::ZeroRotator);
	}

	Normal = -Ahead;
	Right = Axes.GetUnitAxis(EAxis::Y);
	Up = FVector::UpVector;
	Centre = Focus;
	Width = 2.0 * FMath::Max(Distance, 10.0) * FMath::Tan(FMath::DegreesToRadians(GPanelDegrees * 0.5));
	const FIntPoint Size = Pixels();
	Height = Width * Size.Y / Size.X;
	// The plane mesh is 100 units square in its XY, its texture across X and
	// down Y: X to the panel's right, Y down it, its normal toward the player.
	const FMatrix Frame(Right, -Up, Normal, FVector::ZeroVector);
	Panel->SetWorldTransform(FTransform(Frame.ToQuat(), Centre, FVector(Width / 100.0, Height / 100.0, 1.0)));
}

FVector ARigExecVRRig::Eye() const
{
	return Camera->GetComponentLocation();
}

bool ARigExecVRRig::HandRay(int32 Hand, FVector& From, FVector& Direction, FQuat& Turn) const
{
	if (!HeadsetOn())
	{
		// The mouse is the right hand, held at the eye: its laser is the
		// line under the cursor.
		APlayerController* Player = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (Hand != 1 || !Player || !Player->DeprojectMousePositionToWorld(From, Direction))
		{
			return false;
		}
		// Held at arm's length along that line, so a grip moves as the mouse does.
		From += Direction * 60.0;
		Turn = Direction.ToOrientationQuat();
		return true;
	}
	const UMotionControllerComponent* Controller = Hand == 0 ? LeftHand : RightHand;
	if (!Controller || !Controller->IsTracked())
	{
		return false;
	}
	From = Controller->GetComponentLocation();
	Direction = Controller->GetForwardVector();
	Turn = Controller->GetComponentQuat();
	return true;
}

bool ARigExecVRRig::HitPanel(const FVector& From, const FVector& Direction, FVector2D& OutPixel, FVector& OutPoint) const
{
	const double Facing = FVector::DotProduct(Direction, -Normal);
	if (Facing < 1e-3)
	{
		return false;
	}
	const double T = FVector::DotProduct(Centre - From, -Normal) / Facing;
	if (T < 0.0)
	{
		return false;
	}
	OutPoint = From + Direction * T;
	const FIntPoint Size = Pixels();
	OutPixel = FVector2D((FVector::DotProduct(OutPoint - Centre, Right) / Width + 0.5) * Size.X,
	                     (0.5 - FVector::DotProduct(OutPoint - Centre, Up) / Height) * Size.Y);
	return OutPixel.X >= 0.0 && OutPixel.Y >= 0.0 && OutPixel.X <= Size.X && OutPixel.Y <= Size.Y;
}

bool ARigExecVRRig::ToPanel(const FVector& World, FVector2D& Out) const
{
	const FVector E = Eye();
	const FVector Forward = -Normal;
	const double ToPlane = FVector::DotProduct(Centre - E, Forward);
	const double ToPoint = FVector::DotProduct(World - E, Forward);
	if (ToPlane < 1.0 || ToPoint < 1.0)
	{
		return false;
	}
	const FVector OnPanel = E + (World - E) * (ToPlane / ToPoint);
	const FIntPoint Size = Pixels();
	Out = FVector2D((FVector::DotProduct(OnPanel - Centre, Right) / Width + 0.5) * Size.X,
	                (0.5 - FVector::DotProduct(OnPanel - Centre, Up) / Height) * Size.Y);
	return true;
}

FVector ARigExecVRRig::PanelPoint(const FVector2D& Pixel) const
{
	const FIntPoint Size = Pixels();
	return Centre + Right * ((Pixel.X / Size.X - 0.5) * Width) + Up * ((0.5 - Pixel.Y / Size.Y) * Height);
}

void ARigExecVRRig::FromPanel(const FVector2D& Pixel, FVector& OutOrigin, FVector& Direction) const
{
	OutOrigin = Eye();
	Direction = (PanelPoint(Pixel) - OutOrigin).GetSafeNormal();
}

double ARigExecVRRig::PixelSizeAt(const FVector& At) const
{
	const FVector E = Eye();
	const double ToPlane = FMath::Max(FVector::DotProduct(Centre - E, -Normal), 1.0);
	const double ToPoint = FMath::Max(FVector::DotProduct(At - E, -Normal), 1.0);
	return Width / Pixels().X * ToPoint / ToPlane;
}

void ARigExecVRRig::ViewAxes(FVector& Forward, FVector& OutRight, FVector& OutUp) const
{
	Forward = -Normal;
	OutRight = Right;
	OutUp = Up;
}

void ARigExecVRRig::SetLobbyLook(bool bLobby)
{
	// The lobby is lit for a narrow lens far back; stood among its lights it
	// wants a stop less, and its vignette.
	FPostProcessSettings& Look = Camera->PostProcessSettings;
	Look.bOverride_AutoExposureBias = bLobby;
	Look.AutoExposureBias = -1.0f;
	Look.bOverride_VignetteIntensity = bLobby;
	Look.VignetteIntensity = 0.5f;
}

FString ARigExecVRRig::DescribePanel(const TArray<FIntPoint>& At) const
{
	FTextureRenderTargetResource* Resource = Target ? Target->GameThread_GetRenderTargetResource() : nullptr;
	TArray<FLinearColor> Pixels;
	if (!Resource || !Resource->ReadLinearColorPixels(Pixels))
	{
		return TEXT("no panel pixels");
	}
	const FIntPoint Size = this->Pixels();
	FString Out = FString::Printf(TEXT("format %d, visible %d, material %s:"), int32(Target->RenderTargetFormat), Panel->IsVisible(),
	                              PanelMaterial ? TEXT("yes") : TEXT("no"));
	for (const FIntPoint& P : At)
	{
		const int32 Index = FMath::Clamp(P.Y, 0, Size.Y - 1) * Size.X + FMath::Clamp(P.X, 0, Size.X - 1);
		const FLinearColor C = Pixels.IsValidIndex(Index) ? Pixels[Index] : FLinearColor::Transparent;
		Out += FString::Printf(TEXT("  (%d,%d) %.2f %.2f %.2f a%.2f"), P.X, P.Y, C.R, C.G, C.B, C.A);
	}
	return Out;
}

void ARigExecVRRig::SetPanelInvert(float Invert)
{
	if (PanelMaterial)
	{
		PanelMaterial->SetScalarParameterValue(TEXT("Invert"), Invert);
	}
}

void ARigExecVRRig::SetLaser(int32 Hand, bool bShown, const FVector& From, const FVector& To, bool bActive)
{
	UStaticMeshComponent* Mesh = Hand == 0 ? LeftLaser : RightLaser;
	if (!Mesh)
	{
		return;
	}
	// The desktop stand-in has no laser to see: the cursor is the mouse.
	Mesh->SetVisibility(bShown && HeadsetOn());
	if (!bShown)
	{
		return;
	}
	const FVector Along = To - From;
	const double Length = FMath::Max(Along.Size(), 1.0);
	const double Thick = bActive ? 0.006 : 0.004;
	Mesh->SetWorldTransform(FTransform(FRotationMatrix::MakeFromZ(Along / Length).ToQuat(), From + Along * 0.5,
	                                   FVector(Thick, Thick, Length / 100.0)));
	if (LaserMaterial[Hand])
	{
		LaserMaterial[Hand]->SetVectorParameterValue(TEXT("Color"), bActive ? FLinearColor(0.35f, 0.85f, 1.0f) : FLinearColor(0.25f, 0.3f, 0.4f));
	}
}
