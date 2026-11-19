// The playable biped: a Character whose hidden Mannequin runs the template's
// locomotion animation, retargeted every frame onto the RigExec rig, which
// is what you see.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"

#include "RigExecBipedCharacter.generated.h"

class UCameraComponent;
class UInputAction;
class UInputMappingContext;
class URigExecCharacter;
class URigExecComponent;
class URigExecEyeComponent;
class URigExecRetargetComponent;
class USpringArmComponent;
struct FInputActionValue;

UCLASS()
class RIGEXECBIPED_API ARigExecBipedCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ARigExecBipedCharacter();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RigExec")
	TObjectPtr<URigExecComponent> Rig;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RigExec")
	TObjectPtr<URigExecRetargetComponent> Retarget;

	/** The eyes, drawn with the Squarebit Eye shader when its plugin is in
	 * the project (install_eye.bat). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "RigExec")
	TObjectPtr<URigExecEyeComponent> Eyes;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> FollowCamera;

	/** The baked biped (Setup_RigExecBiped.bat makes it). */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	TSoftObjectPtr<URigExecCharacter> CharacterAsset;

	/** The Mannequin whose animation drives the rig, and its animation. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	TSoftObjectPtr<USkeletalMesh> SourceMesh;

	UPROPERTY(EditAnywhere, Category = "RigExec")
	TSoftClassPtr<UAnimInstance> SourceAnimation;

	/** Draw the Mannequin too, beside the biped (the M key toggles it). */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	bool bShowSource = false;

	/** Toggles drawing the Mannequin beside the biped. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	void ToggleSource();

	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	/** The keys follow the pawn: its mapping goes on when a player takes it and
	 * comes off when they leave it (the character select swaps pawns, and a
	 * mapping left behind would take the keys for a pawn nobody controls). */
	virtual void PossessedBy(AController* NewController) override;
	virtual void UnPossessed() override;
	virtual void Tick(float DeltaSeconds) override;

	/** The close-up: cuts to a camera on the face, subdivides the biped
	 * (level 0 to 1), blinks, opens the jaw, then idles looking around and
	 * blinking until EndCinematic (Esc). ARigExecCinematicPad starts it. */
	void StartCinematic();
	/** Back to play: the follow camera, subdivision level 0, the face at rest. */
	void EndCinematic();
	bool IsInCinematic() const { return bCinematic; }

	/** A baked character the player can be: its label and asset. */
	struct FChoice
	{
		FString Label;
		FSoftObjectPath Asset;
	};
	/** Every baked RigExec character in the project, the biped first. */
	static TArray<FChoice> FindCharacters();
	/** The character to be: -RigExecCharacter=<label>, else the biped (the
	 * default; others are chosen in the lobby). */
	static FSoftObjectPath ChosenCharacter();
	/** Remembers the choice (saved with the settings). */
	static void ChooseCharacter(const FSoftObjectPath& Asset);
	/** Speed settings (the menu's Settings page), for every character:
	 * the rig on worker threads, and how often it runs when seen from afar
	 * (playing, out of the close-up) and in the lobby. The close-up always
	 * runs it every frame. */
	static bool bRigAsync;
	static int32 RigRateFar;
	static int32 RigRateLobby;

	/** A lobby preview (ARigExecCharacterSelect): loads PreviewAsset, has no
	 * controller and runs none of the demos or the close-up. */
	bool bPreview = false;
	FSoftObjectPath PreviewAsset;
	/** Posed by the animation studio (ARigExecStudioGameMode): it stands
	 * still, the keys and mouse leave it alone and its rig runs every frame. */
	bool bStudio = false;
	/** A lobby preview taken as the player's pawn: starts behaving as one. */
	void BecomePlayable();
	/** The player's old pawn, left in the lobby when another is chosen. */
	void BecomePreview();

	/** The asset this character was loaded from. */
	FSoftObjectPath GetCharacterAsset() const { return CharacterAsset.ToSoftObjectPath(); }
	float GetCinematicTime() const { return CinematicTime; }
	/** The subdivision level drawn now; -1 without Squarebit Subdivs. */
	int32 GetSubdivisionLevel() const;
	/** Live, in play or the close-up (keys - = and 0-2, the HUD's buttons).
	 * Capped at 2: on the biped's 143k render vertices level 3 takes many
	 * seconds to build. */
	void SetSubdivisionLevel(int32 Level);
	void SubdivisionUp();
	void SubdivisionDown();
	static constexpr int32 MaxSubdivisionLevel = 2;

protected:
	virtual void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);
	void JumpPressed();
	void JumpReleased();
	/** Esc / gamepad B: back out of the menu, or the close-up, or open the menu. */
	void Back();
	/** P / gamepad Start: the pause menu. */
	void PausePressed();
	/** The level's HUD, for its pause menu. */
	class ARigExecMenuHUD* MenuHUD() const;
	bool InMenu() const;
	/** Quits that many seconds from now (once), as -RigExecQuitAfter does. */
	void QuitSoon(float Seconds);
	/** Seconds since play began. */
	float PlayTime = 0.0f;

private:
	void DriveDemo(float Before);
	/** Screenshots, video frames and quitting, from the command line. */
	void Capture(float Before);
	float QuitAt = -1.0f;
	void ApplySourceVisibility();

	UPROPERTY(Transient)
	TObjectPtr<UInputMappingContext> Mapping;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MoveAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> LookAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> JumpAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> ToggleSourceAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> BackAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> PauseAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MenuUpAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MenuDownAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> MenuSelectAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> SubdUpAction;
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> SubdDownAction;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInputAction>> SubdLevelActions;

	/** -RigExecDemo: runs in a circle and jumps on its own (for tests). */
	bool bDemo = false;
	int32 FrameCount = 0;

	// The cinematic.
	void BeginCinematicSetup();
	void MakeSubdivision();
	void DriveCinematic(float DeltaSeconds);
	void PoseFace(float Lids, float JawDegrees, float Smile = 0.0f);
	/** Turns both eyes in the head (degrees; pitch up positive). */
	void TurnEyes(float Yaw, float Pitch);
	/** The point between the eyes, in the world. */
	FVector FaceCentre() const;
	/** The face's size against the biped's, whose close-ups were framed by
	 * eye: its eyes-to-chin over the biped's 11.15 cm. The close-up cameras
	 * stand back by it, so every character's face fills the frame alike. */
	float FaceScale() const;
	mutable float CachedFaceScale = 0.0f;
	int32 Chin = INDEX_NONE;
	/** Each close-up light's own brightness, before FaceScale. */
	TArray<float> FaceLightCandelas;
	/** -RigExecPadDemo: runs to the pad, watches the close-up, presses Esc. */
	void DrivePadDemo(float Before);
	/** -RigExecPortrait: a posed, lit portrait of the face at subdivision
	 * level 2, a few angles saved as high-resolution screenshots (the
	 * loading screen's picture), then quits. */
	void DrivePortrait(float Before);
	bool bPortrait = false;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UPointLightComponent>> FaceLights;

	/** Squarebit Subdivs' Dynamic Mesh component when the plugin is in the
	 * build (made by class name, so the example builds without it). */
	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> Subdivision;
	UPROPERTY(Transient)
	TObjectPtr<class ACameraActor> FaceCamera;
	bool bCinematic = false;
	float CinematicTime = 0.0f;
	/** The level was set by hand in this close-up, so it is left alone. */
	bool bLevelChosen = false;
	FRandomStream Random{7};
	int32 LeftLid = INDEX_NONE, RightLid = INDEX_NONE, Jaw = INDEX_NONE, LookTarget = INDEX_NONE;
	int32 LeftEye = INDEX_NONE, RightEye = INDEX_NONE;
	int32 LeftCorner = INDEX_NONE, RightCorner = INDEX_NONE, UpLip = INDEX_NONE, LeftUpLip = INDEX_NONE,
	      RightUpLip = INDEX_NONE, LeftInBrow = INDEX_NONE, RightInBrow = INDEX_NONE;
	float BlinkAt = 0.0f, NextBlink = 0.0f;
	float GazeYaw = 0.0f, GazePitch = 0.0f, WantYaw = 0.0f, WantPitch = 0.0f, NextGaze = 0.0f;
	float HeadYaw = 0.0f, HeadPitch = 0.0f;
	/** Where the head is heading: it sets off a beat after the eyes. */
	float HeadWantYaw = 0.0f, HeadWantPitch = 0.0f, HeadFollowAt = 0.0f;
	/** How far the eyes are turned down in the head, 0-1: the lids follow. */
	float EyesDown = 0.0f;
	float JitterYaw = 0.0f, JitterPitch = 0.0f, NextJitter = 0.0f;
	/** M_Look's look-at switch: off in the close-up, the eyes turn in the head. */
	FString LookAtChannel;
	FVector CameraFace = FVector::ZeroVector;
	FVector CameraForward = FVector::ForwardVector;
	/** The pad demo's waypoints and progress. */
	TArray<FVector> Route;
	int32 RouteAt = 0;
	float CinematicDone = -1.0f;
};
