// The player in VR (OpenXR: a Quest over Link or Air Link, or any PC
// headset): the headset's camera, a laser on each controller, and a
// floating panel that shows the HUD. Every HUD draws on a canvas as it does
// on the desktop; in VR that canvas is drawn into this panel's texture
// instead of over the screen.
//
// The panel stands upright through the point being looked at, square to the
// way the player faces, a fixed angle wide. A HUD pixel maps to the world by
// the line from the eye through that pixel on the panel, so the marks the
// HUD draws over the character (the manipulator, TouchPose's dots) sit on
// it as seen from the headset, and the laser's point on the panel is the
// mouse.
//
// -RigExecVRSim runs the same thing on the desktop without a headset: the
// camera at standing height, and the mouse as the right hand's laser
// (left button the trigger, right button the grip).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SceneTypes.h"

#include "RigExecVR.generated.h"

class UCameraComponent;
class UCanvasRenderTarget2D;
class UMaterialInstanceDynamic;
class UMotionControllerComponent;
class UStaticMeshComponent;

UCLASS()
class RIGEXECBIPED_API ARigExecVRRig : public AActor
{
	GENERATED_BODY()

public:
	ARigExecVRRig();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	/** The controllers' buttons and axes as they arrive from the headset,
	 * before the viewport, its focus or the input mode can drop them. */
	static bool XRKeyDown(const FKey& Key);
	static float XRKeyAxis(const FKey& Key);
	/** Pressed since the frame before (see XRInputFrame). */
	static bool XRKeyPressed(const FKey& Key);
	/** Starts a frame of controller input: the presses since the last call. */
	static void XRInputFrame();
	/** The controller keys held now and the last analog values, for the log. */
	static FString XRInputReport();

	/** A headset is showing the game now. */
	static bool HeadsetOn();
	/** -RigExecVRSim: VR on the desktop, for trying it without a headset. */
	static bool Simulated();
	/** The HUDs should be in VR. */
	static bool Wanted() { return HeadsetOn() || Simulated(); }

	/** Stands the player at `Distance` from `Focus`, facing `Yaw`, on the
	 * floor at `FloorZ`, with the panel upright through `Focus`. */
	void Stage(const FVector& Focus, float Yaw, float Distance, float FloorZ);

	/** The eye: the headset, or the desktop camera. */
	FVector Eye() const;
	/** A hand's laser (0 left, 1 right) and its turn in the world. */
	bool HandRay(int32 Hand, FVector& From, FVector& Direction, FQuat& Turn) const;
	/** Where a ray meets the panel: its pixel, and the point. */
	bool HitPanel(const FVector& From, const FVector& Direction, FVector2D& OutPixel, FVector& OutPoint) const;
	/** A world point's pixel on the panel, seen from the eye. */
	bool ToPanel(const FVector& World, FVector2D& Out) const;
	/** The line from the eye through a pixel on the panel. */
	void FromPanel(const FVector2D& Pixel, FVector& Origin, FVector& Direction) const;
	FVector PanelPoint(const FVector2D& Pixel) const;
	/** The world size of a panel pixel, seen at the depth of `At`. */
	double PixelSizeAt(const FVector& At) const;
	/** The view's axes: into the panel, its right, its up. */
	void ViewAxes(FVector& Forward, FVector& Right, FVector& Up) const;
	/** The camera's look for the character lobby (or the studio). */
	void SetLobbyLook(bool bLobby);
	/** The laser drawn from `From` to `To` (or hidden). */
	void SetLaser(int32 Hand, bool bShown, const FVector& From, const FVector& To, bool bActive);
	/** The canvas blend for a HUD item: on the panel one that keeps coverage
	 * in alpha (the panel is see-through where nothing is drawn). */
	static ESimpleElementBlendMode UiBlend() { return bPaintingPanel ? SE_BLEND_AlphaBlend : SE_BLEND_Translucent; }
	/** The same for text (its glyphs are alpha only). */
	static ESimpleElementBlendMode UiTextBlend() { return bPaintingPanel ? SE_BLEND_TranslucentAlphaOnlyWriteAlpha : SE_BLEND_Translucent; }
	static bool bPaintingPanel;
	/** The panel's size in pixels. */
	static FIntPoint Pixels() { return FIntPoint(2048, 1152); }
	UCanvasRenderTarget2D* GetTarget() const { return Target; }
	/** The panel texture's colour and alpha at a few pixels, for the self-test. */
	FString DescribePanel(const TArray<FIntPoint>& At) const;
	/** Sets the panel material's opacity sense (1: alpha is 1 - coverage). */
	void SetPanelInvert(float Invert);

	UPROPERTY(VisibleAnywhere, Category = "VR")
	TObjectPtr<USceneComponent> Origin;
	UPROPERTY(VisibleAnywhere, Category = "VR")
	TObjectPtr<UCameraComponent> Camera;
	UPROPERTY(VisibleAnywhere, Category = "VR")
	TObjectPtr<UMotionControllerComponent> LeftHand;
	UPROPERTY(VisibleAnywhere, Category = "VR")
	TObjectPtr<UMotionControllerComponent> RightHand;
	UPROPERTY(VisibleAnywhere, Category = "VR")
	TObjectPtr<UStaticMeshComponent> LeftLaser;
	UPROPERTY(VisibleAnywhere, Category = "VR")
	TObjectPtr<UStaticMeshComponent> RightLaser;
	UPROPERTY(VisibleAnywhere, Category = "VR")
	TObjectPtr<UStaticMeshComponent> Panel;

private:
	UPROPERTY(Transient)
	TObjectPtr<UCanvasRenderTarget2D> Target;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> PanelMaterial;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> LaserMaterial[2];

	/** The panel: its middle, its axes (normal toward the player), its size. */
	FVector Centre = FVector::ZeroVector;
	FVector Right = FVector::RightVector, Up = FVector::UpVector, Normal = -FVector::ForwardVector;
	double Width = 100.0, Height = 56.25;
	/** How far the tracking origin is raised to put the floor at the floor
	 * (-1 until the headset's first pose). */
	double FloorLift = -1.0;
};
