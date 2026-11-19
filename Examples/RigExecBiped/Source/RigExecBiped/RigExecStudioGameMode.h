// The animation studio: a game mode of its own (L_RigExecStudio, or any
// level with RigExecStudioGameMode as its GameMode override), where you
// choose a character in the lobby, then pose and animate it.
//
//   * TouchPose: hover the skin to light the control under it, click to
//     select it, drag to pose it (rotate, or move for controls that only
//     move; Ctrl swaps, Alt twists about the view).
//   * The hover picker: the character's picker drawn over the view; click a
//     button to select, Shift adds, Ctrl removes, Alt mirrors. Picking a
//     control on a limb's idle IK/FK half switches the limb to it, matched;
//     its IK/FK and space buttons switch, matched.
//   * The channel box: the selected control's values; drag one to change it.
//   * The timeline: scrub, play, key (S), delete keys, drag keys, Auto Key.
//   * The library: animations and poses, the defaults baked from the
//     Mannequin's animation through the retargeter and your own saved ones;
//     apply any of them at any time.
//
// No Control Rig and no editor: everything here ships in the packaged game.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "RigExecMenuHUD.h"
#include "RigExecStudioAnimation.h"
#include "RigExecStudioPicker.h"

#include "RigExecStudioGameMode.generated.h"

class ARigExecBipedCharacter;
class UFont;

UCLASS()
class RIGEXECBIPED_API ARigExecStudioGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ARigExecStudioGameMode();
};

UCLASS()
class RIGEXECBIPED_API ARigExecStudioHUD : public ARigExecMenuHUD
{
	GENERATED_BODY()

public:
	ARigExecStudioHUD();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool HandleBack() override;
	virtual bool IsStudio() const override { return true; }

protected:
	virtual void DrawGame() override;
	/** The FPS counter at the bottom left of the view, clear of every panel. */
	virtual bool CounterAnchor(float W, float H, float S, float& X, float& Y) const override;
	/** In VR: the player stands where the studio's camera would be. */
	virtual bool GetVRStage(FVector& LookAt, float& Yaw, float& Distance, float& FloorZ) const override;

private:
	using FClip = RigExecStudio::FClip;
	using FPose = RigExecStudio::FPose;

	// --- The character ---------------------------------------------------
	/** Takes the pawn on as the studio's (or lets the old one go). */
	void Bind(ARigExecBipedCharacter* Hero);
	void Unbind();
	class URigExecComponent* Rig() const;
	FString CharacterName() const;
	TWeakObjectPtr<ARigExecBipedCharacter> Bound;
	RigExecStudio::FBinding Binding;
	bool bDirty = false;

	// --- The library ---------------------------------------------------------
	void LoadLibrary();
	void SaveDefaults() const;
	void SaveClip(const FString& Name);
	void SavePose(const FString& Name);
	void DeleteItem(bool bPose, int32 Index);
	void UseClip(int32 Index, bool bInsert);
	void UsePose(int32 Index);
	void StartBake();
	TArray<FClip> Clips;
	TArray<FPose> Poses;
	RigExecStudio::FBaker Baker;
	bool bBaking = false;
	float PoseMix = 1.0f;
	int32 ClipScroll = 0, PoseScroll = 0;

	// --- The timeline --------------------------------------------------------
	FClip Edit;
	bool bEditChanged = false;
	float Frame = 0.0f;
	float AppliedFrame = -1.0f;
	bool bPlaying = false;
	bool bWasPlaying = false;
	bool bLoop = true;
	bool bAutoKey = true;
	void SetFrame(float NewFrame);
	void ApplyEdit(bool bForce = false);
	/** Keys the selection (or everything with none) on the current frame. */
	void KeySelection(bool bAll);
	void DeleteKeys();
	/** Keys what changed since `Before`, when Auto Key is on. */
	void AutoKey(const FPose& Before);
	TSet<FString> SelectedPaths(bool bWithChannels) const;
	int32 KeyFrameNear(int32 From, int32 Step) const;

	// --- Selection and posing --------------------------------------------------
	TArray<int32> Selected;
	/** 0 replace, 1 toggle, 2 remove, 3 add. */
	void Select(const TArray<int32>& Controls, int32 Mode);
	/** A newly picked control on a limb's idle half: switch the limb to it, matched. */
	void SwitchPresented(const TArray<int32>& Picked);
	bool SwitchLimbMatched(const FString& SwitchPath);
	bool SwitchSpaceMatched(int32 Control, int32 Value);
	void ZeroControls();
	enum class EPoseMode : uint8
	{
		Auto,
		Rotate,
		Move,
		Scale
	};
	/** What the manipulator shows for the lead control now: Move, Rotate
	 * or Scale (Auto picks by the control's channels). */
	EPoseMode GizmoMode() const;
	EPoseMode PoseMode = EPoseMode::Auto;
	int32 HoverRegion = -1;
	int32 HoverControl = INDEX_NONE;
	/** Controls no touch region names (their dots stand in for the skin). */
	TSet<int32> Untouched;
	/** The control TouchPose finds at a screen point: its region on the skin,
	 * else the dot of an unregioned control near it. */
	int32 ControlUnder(const FVector2D& At, int32& OutRegion);
	FVector2D LastPickMouse = FVector2D(-1.0, -1.0);

	// --- Undo ---------------------------------------------------------------
	struct FSnapshot
	{
		FClip Edit;
		FPose Pose;
		TArray<int32> Selected;
	};
	TArray<FSnapshot> UndoStack, RedoStack;
	void PushUndo();
	void Undo(bool bRedo);
	FSnapshot Snapshot() const;
	void Restore(const FSnapshot& State);

	// --- The camera -------------------------------------------------------------
	float CamYaw = 0.0f, CamPitch = -8.0f, CamDist = 380.0f;
	FVector CamFocus = FVector::ZeroVector, CamFocusWant = FVector::ZeroVector;
	float CamDistWant = 380.0f;
	void HomeCamera();
	void FrameSelection();
	void ApplyCamera(float DeltaSeconds);

	// --- Input, read once a frame --------------------------------------------------
	void ReadInput();
	FVector2D Mouse = FVector2D::ZeroVector;
	FVector2D MouseBefore = FVector2D::ZeroVector;
	bool bPress = false, bRelease = false, bDown = false;
	bool bRightPress = false, bRightDown = false, bMiddlePress = false, bMiddleDown = false;
	bool bShift = false, bCtrl = false, bAlt = false;
	float Wheel = 0.0f;
	/** A widget took this frame's press. */
	bool bTaken = false;
	bool Pressed(const FKey& Key) const;
	void Shortcuts();

	// --- Drags --------------------------------------------------------------------
	enum class EDrag : uint8
	{
		None,
		Orbit,
		Pan,
		Pose,
		Scrub,
		Avar,
		Channel,
		Keys,
		Mix,
		Gizmo,
		/** VR: a control held in the hand by its grip. */
		Grab
	};
	struct FPoseStart
	{
		int32 Control = INDEX_NONE;
		FMatrix Value = FMatrix::Identity;
		FMatrix Frame = FMatrix::Identity;
		FVector Pivot = FVector::ZeroVector;
		double Avars[9] = {};
	};
	/** The manipulator on the lead control: where it is and its handles. */
	struct FGizmo
	{
		FVector Pivot = FVector::ZeroVector;
		FVector2D Centre = FVector2D::ZeroVector;
		/** World length of a handle (a fixed size on screen). */
		double Length = 10.0;
		/** The axes a move goes along (the control's parent space, as its
		 * translate channels run), the axes it turns and scales about (its own). */
		FVector MoveAxes[3], OwnAxes[3];
		uint16 Mask = 0;
		EPoseMode Mode = EPoseMode::Rotate;
	};
	bool GetGizmo(FGizmo& Out) const;
	/** The handle under a screen point, or -1: 0-2 move axes, 3 move in the
	 * view, 4-6 rotate rings, 7 the view ring, 8-10 scale axes, 11 uniform. */
	int32 GizmoHandleAt(const FVector2D& At) const;
	void DrawGizmo();
	int32 GizmoHover = -1;
	/** Applies a drag to the controls in hand, from where each started: a
	 * turn about each pivot, a move, or a scale (axis -1: all three). */
	void ApplyWorldEdit(const FQuat& Turn, const FVector& Move, double ScaleBy, int32 ScaleAxis);
	void UpdateGizmoDrag();
	struct FDrag
	{
		EDrag Kind = EDrag::None;
		FVector2D Start = FVector2D::ZeroVector;
		bool bMoved = false;
		bool bTranslate = false;
		int32 Control = INDEX_NONE;
		int32 Index = INDEX_NONE;
		double StartValue = 0.0;
		int32 KeyFrom = 0;
		int32 KeyAt = 0;
		bool bKeysOfSelection = false;
		TArray<FPoseStart> Targets;
		FPose Before;
		/** The press was on a region not yet selected: a click selects it. */
		int32 Region = -1;
	};
	FDrag Drag;
	void BeginPoseDrag();
	void UpdatePoseDrag();
	void EndDrag();
	void ViewportPress();

	// --- VR: the controllers' own tools -----------------------------------------------
	/** The grip: the control under the laser (or the selection) follows the hand. */
	void UpdateGrab();
	FVector GrabFrom = FVector::ZeroVector;
	FQuat GrabTurn = FQuat::Identity;
	/** The buttons and sticks: play, undo, key, frame; turn, zoom, frames, up and down. */
	void VRControls();

	// --- Drawing ------------------------------------------------------------------
	float S = 1.0f;
	FBox2D LibraryBox, SideBox, TimelineBox, TopBox;
	bool OverPanel(const FVector2D& At) const;
	bool Inside(float X, float Y, float W, float H) const;
	void Label(const FString& Text, float X, float Y, float Size, const FLinearColor& Color, int32 Align = -1);
	float LabelWidth(const FString& Text, float Size) const;
	/** An immediate-mode button: true on the frame it is clicked. */
	bool UiButton(const FString& Text, float X, float Y, float W, float H, bool bOn = false, bool bEnabled = true);
	void Panel(const FBox2D& Box, const FString& Title);
	void DrawTop(float W, float H);
	void DrawLibrary();
	void DrawSide();
	void DrawChannels(float X, float Y, float W, float H);
	void DrawTimeline();
	void DrawViewportMarks();
	void DrawBaking(float W, float H);
	void DrawNaming(float W, float H);
	/** Twice a frame: its press first (false), then drawn on top (true). */
	void DrawSpaceMenu(bool bDraw);
	void Toast(const FString& Text);
	FString ToastText;
	double ToastUntil = 0.0;

	// --- The hover picker -------------------------------------------------------------
	RigExecStudio::FPicker Picker;
	int32 PickerPanel = 0;
	bool bShowPicker = true;
	bool bShowLibrary = true;
	void PickButton(int32 Panel, int32 Button);

	// --- Naming a save ---------------------------------------------------------------
	bool bNaming = false;
	bool bNamingPose = false;
	FString NameText;
	void TypeName();

	// --- A space's menu --------------------------------------------------------------
	bool bSpaceMenu = false;
	FVector2D SpaceMenuAt = FVector2D::ZeroVector;
	int32 SpaceControl = INDEX_NONE;
	FString SpacePath;
	TArray<FString> SpaceLabels;
	bool bSpaceInvert = false;
	void OpenSpaceMenu(int32 Control, const FString& Path, const TArray<FString>& Labels, bool bInvert);

	// --- -RigExecStudioDemo: the studio's part of the recorded tour ----------------
	virtual void BuildDemo() override;
	/** Each character's part of the tour. */
	void BuildDemoFor(const FString& Label, bool bLast);
	/** A control's pivot on screen, or a point between two controls. */
	FVector2D DemoScreenOf(const FString& Control, const FString& Toward = FString(), float Amount = 0.0f) const;
	/** Where to grab a manipulator handle. */
	FVector2D DemoHandle(int32 Handle) const;
	/** The manipulator handle for the axis nearest a world direction. */
	int32 DemoAxisNear(const FVector& World, bool bRing) const;

	// --- -RigExecStudioProfile: where a frame's time goes, logged each second ------
	bool bProfile = false;
	float ProfileTime = 0.0f;
	int32 ProfileFrames = 0;
	/** Seconds this second in each part: rig, picker, marks, timeline, panels, hover pick. */
	double ProfileSpent[6] = {};
	void ProfileTick(float DeltaSeconds);

	// --- -RigExecStudioTest: drives every tool and checks the numbers ----------------
	bool bTest = false;
	int32 TestStep = 0;
	float TestWait = 0.0f;
	TArray<FString> TestLog;
	TMap<FString, FVector> TestAt;
	void RunTest(float DeltaSeconds);
	void TestNote(const FString& Line);
	FVector TestWorld(const FString& Control) const;
	void TestShot(const FString& Name);
};
