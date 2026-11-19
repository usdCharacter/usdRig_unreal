// Drives a RigExec character from an animated skeletal mesh: each frame the
// source's bone rotations are carried onto the rig's FK controls, the
// pelvis's movement onto the body control, and the rig evaluates. A simple
// retargeter for playing UE5 Mannequin animation on a RigExec rig.
#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "RigExecRetargetComponent.generated.h"

class URigExecComponent;
class USkeletalMeshComponent;

/** One source bone driving one rig control. */
USTRUCT(BlueprintType)
struct RIGEXECRUNTIME_API FRigExecRetargetBone
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	FName SourceBone;

	/** The control's Control Rig element name or rig path ("L_UpArm"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	FString TargetControl;

	/** A child of the source bone and the control standing for it: their
	 * directions line up the two rest poses (an A-pose onto a T-pose).
	 * None keeps the bone's rest as it is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	FName SourceAim;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	FString TargetAim;

	/** A second pair, across the aim, that lines up the roll about it too
	 * (a hand's index and pinky knuckles line up its palm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	FName SourceSideFrom;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	FName SourceSideTo;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	FString TargetSideFrom;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	FString TargetSideTo;

	/** Carry the bone's movement too, scaled by the two pelvis heights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	bool bTranslate = false;
};

UCLASS(ClassGroup = (Animation), meta = (BlueprintSpawnableComponent))
class RIGEXECRUNTIME_API URigExecRetargetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	URigExecRetargetComponent();

	/** The animated mesh to follow; the owner's first skeletal mesh when empty.
	 * It can be hidden: it is made to keep animating. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	TObjectPtr<USkeletalMeshComponent> Source;

	/** The rig to drive; the owner's when empty. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	TObjectPtr<URigExecComponent> Target;

	/** Which bone drives which control, parents before children. Defaults to
	 * the UE5 Mannequin onto the usdRig biped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	TArray<FRigExecRetargetBone> Bones;

	/** The targets of two entries on either side of the body (left first) and
	 * two along it (low first): they turn the source to face the way the rig
	 * does. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FString LeftControl = TEXT("L_UpLeg");
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FString RightControl = TEXT("R_UpLeg");
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FString LowControl = TEXT("M_Body");
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FString HighControl = TEXT("M_Head");

	/** Hand each IK/FK limb to FK, which is what the bones drive. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	bool bForceFk = true;

	/** Off: the rig is left alone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	bool bEnabled = true;

	/** What it is driving, or why not. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "RigExec")
	FString Status;

	/** The last frame's retarget and evaluation, milliseconds. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "RigExec")
	float LastMilliseconds = 0.0f;

	/** Worst gap, in degrees, between a driven control's frame the rig
	 * computed and the one asked for, over the last frame: near zero when
	 * the controls follow the bones. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "RigExec")
	float LastErrorDegrees = 0.0f;

	/** The UE5 Mannequin (Manny, Quinn) onto the usdRig biped. */
	static TArray<FRigExecRetargetBone> MannequinToBiped();

	/** Binds again on the next tick (after changing Bones or the meshes). */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	void Rebind() { bBound = false; }

	/** An extra turn on a driven control, on top of the animation: `Turn` is
	 * in the rig's asset space (USD Y-up), applied after the bone's own
	 * rotation, so a turn about +Y turns the control about the rig's up
	 * whatever the pose. A head looking around while an idle plays. Identity
	 * clears it. */
	void SetExtraTurn(const FString& Control, const FQuat& Turn);
	void ClearExtraTurns() { ExtraTurns.Reset(); }

	/** Binds if it has not, then carries the source's current pose onto the
	 * rig once and evaluates: what a tick does, on demand, whether enabled
	 * or not. A bake poses the source at each sample and calls this. False
	 * while it cannot bind. */
	bool DriveOnce();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool Bind();
	void Drive();

	struct FBound
	{
		int32 Bone = INDEX_NONE;
		int32 Control = INDEX_NONE;
		/** The source bone's rest, lined up onto the rig's rest pose (USD space, facing turned). */
		FQuat SourceRest = FQuat::Identity;
		/** The control's rest rotation (asset space). */
		FQuat TargetRest = FQuat::Identity;
		FVector SourceRestPosition = FVector::ZeroVector;
		bool bTranslate = false;
		/** A driven control whose rig parent is not under the control of the
		 * bone's parent (a finger on a helper the rig attaches to the hand):
		 * that control, so the helper is carried with it. */
		int32 Follow = INDEX_NONE;
	};
	TArray<FBound> Bound;
	/** Each control's entry in Bound, or -1. */
	TArray<int32> BoundOf;
	/** Whether a control is driven or above one, so its frame is worked out. */
	TArray<bool> InChain;
	/** Whether a control is at or below a Follow entry, so it is worked out
	 * once the rest are. */
	TArray<bool> Deferred;
	TArray<FMatrix> RestFrames;
	/** The turn from the source's facing onto the rig's. */
	FQuat Facing = FQuat::Identity;
	double TranslationScale = 1.0;
	bool bBound = false;
	/** Frames asked for last time, for the error check. */
	TArray<FMatrix> Wanted;
	/** What the run now landing was asked for. */
	TArray<FMatrix> LandedWanted;
	/** SetExtraTurn's turns, by control name. */
	TMap<FString, FQuat> ExtraTurns;
};
