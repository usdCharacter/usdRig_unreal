// What every level's HUD shares: an FPS counter, a debug stats panel, and
// the pause menu (Resume, Settings, Quit) with a settings page of testing
// switches. Esc (when not in the close-up), P or gamepad Start opens it.
// Each level's HUD draws its own things in DrawGame; this draws over them.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "RigExecMenuHUD.generated.h"

class ARigExecBipedCharacter;
class ARigExecCharacterSelect;

UCLASS()
class RIGEXECBIPED_API ARigExecMenuHUD : public AHUD
{
	GENERATED_BODY()

public:
	ARigExecMenuHUD();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void DrawHUD() override;
	virtual void NotifyHitBoxClick(FName BoxName) override;

	/** The menu: open pauses the game and shows the cursor. */
	bool IsMenuOpen() const { return Page != EPage::None; }
	void OpenMenu(bool bPause = true);
	void CloseMenu();
	void ToggleMenu();
	/** Esc / gamepad B in the menu: settings back to the menu, the menu shut. */
	void MenuBack();
	/** Keyboard and gamepad: move the highlight, press the highlighted item. */
	void MenuMove(int32 Step);
	void MenuSelect();
	/** The character chooser, as the menu's page. */
	void OpenChooser();
	/** Closes the menu that is open and opens the character lobby. */
	void ReopenChooser();
	/** Esc / gamepad B with no menu open: true when the level's HUD used it. */
	virtual bool HandleBack() { return false; }
	/** The animation studio's HUD (its menu offers the game instead). */
	virtual bool IsStudio() const { return false; }
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	/** In a headset (or -RigExecVRSim): the HUD is on a panel in the world. */
	bool IsVR() const { return VR != nullptr; }
	class ARigExecVRRig* GetVR() const { return VR; }

protected:
	// --- VR -------------------------------------------------------------------
	// The same HUD on a panel in the world (ARigExecVRRig), the laser on it
	// as the mouse. The level's HUD says where the player stands.
	/** What the player looks at, from which way, how far, and the floor. */
	virtual bool GetVRStage(FVector& LookAt, float& Yaw, float& Distance, float& FloorZ) const;
	struct FVRInput
	{
		/** The laser's point on the panel in HUD pixels (the mouse), if on it. */
		FVector2D Pointer = FVector2D(-1.0, -1.0);
		bool bOnPanel = false;
		/** The pointing hand (0 left, 1 right): the last to press its trigger. */
		int32 Hand = 1;
		bool bTrigger = false, bTriggerPress = false, bTriggerRelease = false;
		/** The gripping hand, where it is and how it is turned. */
		int32 GripHand = 1;
		bool bGrip = false, bGripPress = false, bGripRelease = false;
		FVector GripAt = FVector::ZeroVector;
		FQuat GripTurn = FQuat::Identity;
		/** The sticks (left, right), and a push past half way this frame (-1, 0, 1). */
		FVector2D Stick[2] = {FVector2D::ZeroVector, FVector2D::ZeroVector};
		FIntPoint Flick[2] = {FIntPoint::ZeroValue, FIntPoint::ZeroValue};
		bool bHeld[2][2] = {{false, false}, {false, false}};
		bool bWasTrigger[2] = {false, false}, bWasGrip[2] = {false, false};
	};
	FVRInput VRIn;
	float VRReportTime = 0.0f;
	/** A controller button pressed this frame. */
	bool VRPressed(const FKey& Key) const;

	// --- The view, for drawing over the world and picking in it: the
	// screen, or in VR the panel seen from the eye. -------------------------------
	bool ToScreen(const FVector& World, FVector2D& Out) const;
	bool FromScreen(const FVector2D& At, FVector& Origin, FVector& Direction) const;
	/** The eye and the view's axes: forward, right, up. */
	void GetView(FVector& Eye, FVector& Forward, FVector& Right, FVector& Up) const;
	/** The world size of a HUD pixel at the depth of `At`. */
	double PixelSizeAt(const FVector& At) const;
	/** A clickable box: the engine's hit box, or in VR the laser's. */
	void HitBox(const FVector2D& Position, const FVector2D& Size, FName Name, int32 Priority = 0);
	/** The cursor: the mouse, or the laser on the panel. */
	bool PointerAt(float& X, float& Y) const;
	bool CursorShown() const;

	/** The level's own HUD, under the overlay. */
	virtual void DrawGame() {}
	/** Where the FPS counter goes: its left edge and its bottom (false) or
	 * its top (true). Bottom left unless the level's HUD has a better place. */
	virtual bool CounterAnchor(float W, float H, float S, float& X, float& Y) const { return false; }

	// --- -RigExecStudioDemo: a scripted tour for a recording ------------------
	// A cursor drawn on screen glides, clicks and drags through the real UI
	// (the studio reads it as its mouse), with a caption saying what is
	// happening. Each level's HUD adds its part of the tour (BuildDemo).
	struct FDemoStep
	{
		float Seconds = 0.0f;
		TFunction<void()> Start;
		/** Called each frame with the step's progress, 0-1. */
		TFunction<void(float)> Update;
		/** Ends the step early (or holds it, up to Seconds) when given. */
		TFunction<bool()> Until;
		TFunction<void()> End;
	};
	bool bDemo = false;
	TArray<FDemoStep> DemoSteps;
	int32 DemoAt = 0;
	float DemoTime = 0.0f;
	bool bDemoStarted = false;
	FVector2D DemoMouse = FVector2D(-100.0, -100.0);
	FVector2D DemoFrom = FVector2D::ZeroVector;
	/** This frame's synthetic input (pressed and released last one frame). */
	bool bDemoPress = false, bDemoRelease = false, bDemoDown = false, bDemoShift = false;
	TArray<FKey> DemoKeys;
	FString DemoCaption;
	double DemoClickAt = -10.0;
	/** Where named things were drawn this frame: buttons by label, rows,
	 * menu items ("menu:<label>"), for the tour to aim at. */
	TMap<FString, FBox2D> DemoTargets;
	/** The level's part of the tour. */
	virtual void BuildDemo();
	void DemoTick(float DeltaSeconds);
	void DrawDemo();
	FVector2D DemoTarget(const FString& Name) const;
	void DemoGlideTo(TFunction<FVector2D()> Target, float Seconds = 0.8f);
	void DemoGlide(const FString& Target, float Seconds = 0.8f) { DemoGlideTo([this, Target]() { return DemoTarget(Target); }, Seconds); }
	void DemoClick();
	void DemoDrag(const FVector2D& Delta, float Seconds);
	/** Presses where the cursor is and drags to a point worked out as the drag starts. */
	void DemoDragTo(TFunction<FVector2D()> To, float Seconds);
	void DemoWait(float Seconds);
	void DemoUntil(TFunction<bool()> Done, float Timeout = 60.0f);
	void DemoDo(TFunction<void()> Action);
	void DemoSay(const FString& Caption);
	void DemoType(const FString& Text);
	void DemoKey(const FKey& Key, bool bShifted = false, float Seconds = 0.09f);
	/** A lobby plate's centre on screen. */
	bool DemoPlate(int32 Index, FVector2D& Out) const;
	/** Whether the lobby is up. */
	bool IsChoosing() const { return IsMenuOpen() && Page == EPage::Choose && Lobby; }
	/** Presses a menu item by its label. */
	void DemoMenuPick(const FString& Label);
	void DemoChooseCharacter(int32 Index);

	void Text(const FString& Line, float X, float Y, float Scale, const FLinearColor& Color, bool bCentre = false);
	/** A filled box (AHUD::DrawRect, with the blend the VR panel needs). */
	void DrawRect(FLinearColor RectColor, float ScreenX, float ScreenY, float ScreenW, float ScreenH);
	/** A button: a box, its label, and a hit box when the cursor is up. */
	void Button(FName Name, const FString& Label, float X, float Y, float W, float H, float S, bool bEnabled);
	ARigExecBipedCharacter* Hero() const;
	/** The subdivision panel is wanted (a setting). */
	bool ShowSubdivisionPanel() const { return bShowSubdPanel; }

private:
	enum class EPage : uint8
	{
		None,
		Main,
		Settings,
		Choose
	};
	struct FItem
	{
		FString Label;
		/** Empty for a plain button; a setting's current value otherwise. */
		FString Value;
		TFunction<void()> Press;
		bool bEnabled = true;
	};
	TArray<FItem> Items();
	void DrawCounters(float W, float H, float S);
	void DrawMenu(float W, float H, float S);
	bool Hovered(float X, float Y, float W, float H) const;
	void Apply();
	void Save() const;
	/** Becomes `Asset`: replaces the pawn when it is another character. */
	void BecomeCharacter(const FSoftObjectPath& Asset);
	/** The chooser shows itself once per run, at the first level. */
	static bool bChoseThisRun;
	/** The lobby while choosing: the characters idling on their pads. */
	UPROPERTY(Transient)
	TObjectPtr<ARigExecCharacterSelect> Lobby;
	void DrawLobby(float W, float H, float S);
	FVector2D LastMouse = FVector2D(-1.0, -1.0);
	void CloseLobby();

	// VR: the rig, the panel's paint, the laser's clicks.
	UPROPERTY(Transient)
	TObjectPtr<class ARigExecVRRig> VR;
	void UpdateVR(float DeltaSeconds);
	void ReadVRInput();
	void VRMenuInput();
	void PaintVR(UWorld* World, ELevelTick TickType, float DeltaSeconds);
	UFUNCTION()
	void DrawVRPanel(UCanvas* InCanvas, int32 Width, int32 Height);
	/** Everything the HUD draws, on whatever canvas it has. */
	void DrawFrame();
	void DrawVRCursor();
	FDelegateHandle PostTickHandle;
	struct FVRHit
	{
		FBox2D Box;
		FName Name;
		int32 Priority = 0;
	};
	TArray<FVRHit> VRHits;
	bool bSentToStudio = false;

	EPage Page = EPage::None;
	int32 Focus = 0;
	/** The cursor as it was before the menu, to put back. */
	bool bHadCursor = false, bHadClicks = false;

	// The settings (saved, but for the ones that only make sense for now).
	bool bShowFps = true;
	bool bShowStats = false;
	bool bShowSubdPanel = true;
	bool bEngineStats = false;
	bool bVSync = false;
	int32 FrameCap = 0;
	int32 ScreenPercent = 100;
	float TimeScale = 1.0f;

	// The FPS counter, smoothed and refreshed a few times a second.
	double SmoothDelta = 1.0 / 60.0;
	double WorstDelta = 0.0, WorstSince = 0.0;
	double ShownFps = 0.0, ShownMs = 0.0, ShownWorst = 0.0, NextShow = 0.0;
};
