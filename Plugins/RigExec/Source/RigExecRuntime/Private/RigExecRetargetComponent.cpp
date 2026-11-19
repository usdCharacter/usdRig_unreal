#include "RigExecRetargetComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkinnedAsset.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ReferenceSkeleton.h"
#include "RigExecComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogRigExecRetarget, Log, All);

// Everything here is worked in the rig's asset space: USD's Y-up axes, row
// vectors (a frame is its three axes, then its origin), rig units. The
// source's component space is Unreal's Z-up, in centimetres; swapping Y and Z
// takes a point across, and the same swap on both sides of a rotation takes
// a rotation across (the swap is a mirror, so the result is a rotation).
namespace
{
FVector
ToAsset(const FVector& Ue, double CentimetresPerUnit)
{
	return FVector(Ue.X, Ue.Z, Ue.Y) / CentimetresPerUnit;
}

FQuat
ToAsset(const FQuat& Ue)
{
	const FMatrix M = FQuatRotationMatrix(Ue);
	FMatrix Out = FMatrix::Identity;
	static const int32 Swap[3] = {0, 2, 1};
	for (int32 R = 0; R < 3; ++R)
	{
		for (int32 C = 0; C < 3; ++C)
		{
			Out.M[R][C] = M.M[Swap[R]][Swap[C]];
		}
	}
	return FQuat(Out).GetNormalized();
}

// A frame as rotation, per-axis scale (a mirrored frame keeps the mirror on
// its X scale) and origin.
struct FRigFrame
{
	FQuat Rotation = FQuat::Identity;
	FVector Scale = FVector::OneVector;
	FVector Origin = FVector::ZeroVector;
};

FRigFrame
Split(const FMatrix& M)
{
	FRigFrame Out;
	FMatrix R = FMatrix::Identity;
	for (int32 Row = 0; Row < 3; ++Row)
	{
		const FVector Axis(M.M[Row][0], M.M[Row][1], M.M[Row][2]);
		const double Length = FMath::Max(Axis.Size(), 1e-12);
		Out.Scale[Row] = Length;
		for (int32 C = 0; C < 3; ++C)
		{
			R.M[Row][C] = Axis[C] / Length;
		}
		Out.Origin[Row] = M.M[3][Row];
	}
	if (R.Determinant() < 0.0)
	{
		Out.Scale.X = -Out.Scale.X;
		for (int32 C = 0; C < 3; ++C)
		{
			R.M[0][C] = -R.M[0][C];
		}
	}
	Out.Rotation = FQuat(R).GetNormalized();
	return Out;
}

FMatrix
Join(const FRigFrame& F)
{
	FMatrix M = FQuatRotationMatrix(F.Rotation);
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 C = 0; C < 3; ++C)
		{
			M.M[Row][C] *= F.Scale[Row];
		}
		M.M[3][Row] = F.Origin[Row];
	}
	return M;
}

FVector
OriginOf(const FMatrix& M)
{
	return FVector(M.M[3][0], M.M[3][1], M.M[3][2]);
}
} // namespace

URigExecRetargetComponent::URigExecRetargetComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// After the source's animation, which finishes during its tick group.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	Bones = MannequinToBiped();
}

TArray<FRigExecRetargetBone>
URigExecRetargetComponent::MannequinToBiped()
{
	auto B = [](const TCHAR* Bone, const TCHAR* Control, const TCHAR* AimBone = nullptr, const TCHAR* AimControl = nullptr,
	            bool bTranslate = false)
	{
		FRigExecRetargetBone Out;
		Out.SourceBone = Bone;
		Out.TargetControl = Control;
		Out.SourceAim = AimBone ? FName(AimBone) : NAME_None;
		Out.TargetAim = AimControl ? AimControl : TEXT("");
		Out.bTranslate = bTranslate;
		return Out;
	};
	TArray<FRigExecRetargetBone> Out;
	// The body, then the spine and head; the pelvis carries the movement.
	Out.Add(B(TEXT("pelvis"), TEXT("M_Body"), nullptr, nullptr, true));
	Out.Add(B(TEXT("spine_01"), TEXT("M_Torso"), TEXT("spine_03"), TEXT("M_Chest")));
	Out.Add(B(TEXT("spine_03"), TEXT("M_Chest"), TEXT("spine_05"), TEXT("M_ChestTop")));
	Out.Add(B(TEXT("spine_05"), TEXT("M_ChestTop"), TEXT("neck_01"), TEXT("M_Neck")));
	Out.Add(B(TEXT("neck_01"), TEXT("M_Neck"), TEXT("head"), TEXT("M_Head")));
	Out.Add(B(TEXT("head"), TEXT("M_Head")));
	for (const TCHAR* Side : {TEXT("l"), TEXT("r")})
	{
		const FString S = Side == FString(TEXT("l")) ? TEXT("L_") : TEXT("R_");
		auto Bone = [Side](const TCHAR* Name) { return FString::Printf(TEXT("%s_%s"), Name, Side); };
		auto Add = [&](const TCHAR* BoneName, const TCHAR* Control, const TCHAR* AimBone, const TCHAR* AimControl)
		{
			const FString BoneString = Bone(BoneName), ControlString = S + Control;
			const FString AimBoneString = AimBone ? Bone(AimBone) : FString();
			const FString AimControlString = AimControl ? S + AimControl : FString();
			Out.Add(B(*BoneString, *ControlString, AimBone ? *AimBoneString : nullptr,
			          AimControl ? *AimControlString : nullptr));
		};
		Add(TEXT("clavicle"), TEXT("Shldr"), TEXT("upperarm"), TEXT("UpArm"));
		Add(TEXT("upperarm"), TEXT("UpArm"), TEXT("lowerarm"), TEXT("LoArm"));
		Add(TEXT("lowerarm"), TEXT("LoArm"), TEXT("hand"), TEXT("Hand"));
		// The hand points at its middle knuckle and rolls so its index and
		// pinky knuckles line up: that lines the palm up for the fingers.
		Add(TEXT("hand"), TEXT("Hand"), TEXT("middle_01"), TEXT("MiddleBase"));
		Out.Last().SourceSideFrom = FName(*Bone(TEXT("pinky_01")));
		Out.Last().SourceSideTo = FName(*Bone(TEXT("index_01")));
		Out.Last().TargetSideFrom = S + TEXT("PinkyBase");
		Out.Last().TargetSideTo = S + TEXT("IndexBase");
		Add(TEXT("thigh"), TEXT("UpLeg"), TEXT("calf"), TEXT("LoLeg"));
		Add(TEXT("calf"), TEXT("LoLeg"), TEXT("foot"), TEXT("Foot"));
		Add(TEXT("foot"), TEXT("Foot"), TEXT("ball"), TEXT("Ball"));
		Add(TEXT("ball"), TEXT("Ball"), nullptr, nullptr);
	}
	// The fingers, after the hands. The Mannequin's four fingers each have a
	// metacarpal and three joints, like the rig's Meta, Base, Mid and Tip; its
	// thumb has three joints, the rig's Base, Mid and Tip (the rig's ThumbMeta
	// and PinkyCup are the palm's own cupping and keep their rest).
	for (const TCHAR* Side : {TEXT("l"), TEXT("r")})
	{
		const FString S = Side == FString(TEXT("l")) ? TEXT("L_") : TEXT("R_");
		auto Add = [&](const FString& Bone, const FString& Control, const FString& AimBone, const FString& AimControl)
		{
			const FString BoneName = Bone + TEXT("_") + Side, ControlName = S + Control;
			const FString AimBoneName = AimBone + TEXT("_") + Side, AimControlName = S + AimControl;
			Out.Add(B(*BoneName, *ControlName, AimBone.IsEmpty() ? nullptr : *AimBoneName,
			          AimControl.IsEmpty() ? nullptr : *AimControlName));
		};
		for (const TCHAR* Finger : {TEXT("index"), TEXT("middle"), TEXT("ring"), TEXT("pinky")})
		{
			const FString F = Finger;
			const FString C = F.Left(1).ToUpper() + F.Mid(1);
			Add(F + TEXT("_metacarpal"), C + TEXT("Meta"), F + TEXT("_01"), C + TEXT("Base"));
			Add(F + TEXT("_01"), C + TEXT("Base"), F + TEXT("_02"), C + TEXT("Mid"));
			Add(F + TEXT("_02"), C + TEXT("Mid"), F + TEXT("_03"), C + TEXT("Tip"));
			Add(F + TEXT("_03"), C + TEXT("Tip"), FString(), FString());
		}
		Add(TEXT("thumb_01"), TEXT("ThumbBase"), TEXT("thumb_02"), TEXT("ThumbMid"));
		Add(TEXT("thumb_02"), TEXT("ThumbMid"), TEXT("thumb_03"), TEXT("ThumbTip"));
		Add(TEXT("thumb_03"), TEXT("ThumbTip"), FString(), FString());
	}
	return Out;
}

void
URigExecRetargetComponent::BeginPlay()
{
	Super::BeginPlay();
	bBound = false;
}

bool
URigExecRetargetComponent::Bind()
{
	AActor* Owner = GetOwner();
	if (!Target && Owner)
	{
		Target = Owner->FindComponentByClass<URigExecComponent>();
	}
	if (!Source && Owner)
	{
		Source = Owner->FindComponentByClass<USkeletalMeshComponent>();
	}
	if (!Target || Target->GetControlCount() == 0)
	{
		Status = TEXT("waiting for the rig to load (it needs a controls description)");
		return false;
	}
	if (!Source || !Source->GetSkinnedAsset())
	{
		Status = TEXT("no source skeletal mesh");
		return false;
	}
	// It may be hidden; it must still animate.
	Source->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	PrimaryComponentTick.AddPrerequisite(Source, Source->PrimaryComponentTick);
	// The retargeter owns the controls now.
	Target->bPullControlRig = false;

	// The rig's rest pose.
	Target->ClearChannels();
	Target->Evaluate();
	const int32 N = Target->GetControlCount();
	RestFrames.SetNum(N);
	for (int32 C = 0; C < N; ++C)
	{
		if (!Target->GetControlAssetFrame(C, RestFrames[C]))
		{
			const int32 Parent = Target->GetControlParent(C);
			RestFrames[C] = Parent >= 0 ? RestFrames[Parent] : FMatrix::Identity;
		}
	}
	if (bForceFk)
	{
		for (const TPair<FString, double>& Switch : Target->GetLimbFkSwitches())
		{
			Target->SetChannel(Switch.Key, float(Switch.Value));
		}
	}

	// The source's reference pose, component space.
	const FReferenceSkeleton& Ref = Source->GetSkinnedAsset()->GetRefSkeleton();
	const TArray<FTransform>& Local = Ref.GetRefBonePose();
	TArray<FTransform> Rest;
	Rest.SetNum(Local.Num());
	for (int32 I = 0; I < Local.Num(); ++I)
	{
		const int32 Parent = Ref.GetParentIndex(I);
		Rest[I] = Parent >= 0 ? Local[I] * Rest[Parent] : Local[I];
	}
	const double Cm = Target->GetCentimetresPerUnit();
	auto SourcePosition = [&](int32 Bone) { return ToAsset(Rest[Bone].GetLocation(), Cm); };
	auto TargetPosition = [&](int32 Control) { return OriginOf(RestFrames[Control]); };

	Bound.Reset();
	BoundOf.Init(INDEX_NONE, N);
	TArray<FString> Missing;
	TArray<int32> EntryOf; // Bones index -> Bound index
	EntryOf.Init(INDEX_NONE, Bones.Num());
	for (int32 E = 0; E < Bones.Num(); ++E)
	{
		const int32 Bone = Ref.FindBoneIndex(Bones[E].SourceBone);
		const int32 Control = Target->FindControlIndex(Bones[E].TargetControl);
		if (Bone == INDEX_NONE || Control == INDEX_NONE || Target->IsControlNull(Control))
		{
			Missing.Add(Bones[E].SourceBone.ToString() + TEXT("->") + Bones[E].TargetControl);
			continue;
		}
		FBound& Out = Bound.AddDefaulted_GetRef();
		Out.Bone = Bone;
		Out.Control = Control;
		Out.bTranslate = Bones[E].bTranslate;
		BoundOf[Control] = Bound.Num() - 1;
		EntryOf[E] = Bound.Num() - 1;
	}
	if (Bound.Num() == 0)
	{
		Status = TEXT("no bone of the map is on both the source and the rig");
		return false;
	}

	// Turn the source to face the way the rig does, from a pair across the
	// body and a pair along it.
	auto FindByControl = [&](const FString& Name) -> const FBound*
	{
		const int32 Control = Target->FindControlIndex(Name);
		return Control != INDEX_NONE && BoundOf[Control] != INDEX_NONE ? &Bound[BoundOf[Control]] : nullptr;
	};
	Facing = FQuat::Identity;
	const FBound* Left = FindByControl(LeftControl);
	const FBound* Right = FindByControl(RightControl);
	const FBound* Low = FindByControl(LowControl);
	const FBound* High = FindByControl(HighControl);
	if (Left && Right && Low && High)
	{
		const FMatrix SourceBasis = FRotationMatrix::MakeFromXZ(SourcePosition(Left->Bone) - SourcePosition(Right->Bone),
		                                                         SourcePosition(High->Bone) - SourcePosition(Low->Bone));
		const FMatrix TargetBasis = FRotationMatrix::MakeFromXZ(TargetPosition(Left->Control) - TargetPosition(Right->Control),
		                                                         TargetPosition(High->Control) - TargetPosition(Low->Control));
		Facing = (FQuat(TargetBasis) * FQuat(SourceBasis).Inverse()).GetNormalized();
	}
	// Movement scales by the two pelvis heights (the rig's up is Y).
	TranslationScale = 1.0;
	if (Low)
	{
		const double SourceHeight = Facing.RotateVector(SourcePosition(Low->Bone)).Y;
		const double TargetHeight = TargetPosition(Low->Control).Y;
		if (FMath::Abs(SourceHeight) > 1e-6)
		{
			TranslationScale = TargetHeight / SourceHeight;
		}
	}

	// Each bone's rest, lined up onto the rig's: walking down the map, a bone
	// with an aim turns (with all below it) so it points the way its control
	// does.
	TArray<FQuat> Lined;
	Lined.Init(FQuat::Identity, Bound.Num());
	for (int32 E = 0; E < Bones.Num(); ++E)
	{
		const int32 K = EntryOf[E];
		if (K == INDEX_NONE)
		{
			continue;
		}
		FBound& Out = Bound[K];
		// The nearest mapped ancestor's turn.
		FQuat Above = FQuat::Identity;
		Out.Follow = INDEX_NONE;
		for (int32 Up = Ref.GetParentIndex(Out.Bone); Up != INDEX_NONE; Up = Ref.GetParentIndex(Up))
		{
			const int32 J = Bound.IndexOfByPredicate([Up](const FBound& X) { return X.Bone == Up; });
			if (J != INDEX_NONE && J < K)
			{
				Above = Lined[J];
				// Is the parent bone's control above this one in the rig?
				bool bUnder = false;
				for (int32 C = Target->GetControlParent(Out.Control); C != INDEX_NONE && !bUnder; C = Target->GetControlParent(C))
				{
					bUnder = C == Bound[J].Control;
				}
				if (!bUnder)
				{
					Out.Follow = Bound[J].Control;
				}
				break;
			}
		}
		FQuat Turn = FQuat::Identity;
		const int32 AimBone = Bones[E].SourceAim.IsNone() ? INDEX_NONE : Ref.FindBoneIndex(Bones[E].SourceAim);
		const int32 AimControl = Bones[E].TargetAim.IsEmpty() ? INDEX_NONE : Target->FindControlIndex(Bones[E].TargetAim);
		if (AimBone != INDEX_NONE && AimControl != INDEX_NONE)
		{
			const FVector From = Above.RotateVector(Facing.RotateVector(SourcePosition(AimBone) - SourcePosition(Out.Bone)));
			const FVector To = TargetPosition(AimControl) - TargetPosition(Out.Control);
			if (!From.IsNearlyZero() && !To.IsNearlyZero())
			{
				Turn = FQuat::FindBetweenVectors(From, To);
				// Then the roll about it, from the side pair.
				const int32 SideFrom = Bones[E].SourceSideFrom.IsNone() ? INDEX_NONE : Ref.FindBoneIndex(Bones[E].SourceSideFrom);
				const int32 SideTo = Bones[E].SourceSideTo.IsNone() ? INDEX_NONE : Ref.FindBoneIndex(Bones[E].SourceSideTo);
				const int32 SideFromControl = Bones[E].TargetSideFrom.IsEmpty() ? INDEX_NONE : Target->FindControlIndex(Bones[E].TargetSideFrom);
				const int32 SideToControl = Bones[E].TargetSideTo.IsEmpty() ? INDEX_NONE : Target->FindControlIndex(Bones[E].TargetSideTo);
				if (SideFrom != INDEX_NONE && SideTo != INDEX_NONE && SideFromControl != INDEX_NONE && SideToControl != INDEX_NONE)
				{
					const FVector Axis = To.GetSafeNormal();
					const FVector SourceSide = FVector::VectorPlaneProject(
						Turn.RotateVector(Above.RotateVector(Facing.RotateVector(SourcePosition(SideTo) - SourcePosition(SideFrom)))), Axis);
					const FVector TargetSide = FVector::VectorPlaneProject(
						TargetPosition(SideToControl) - TargetPosition(SideFromControl), Axis);
					if (!SourceSide.IsNearlyZero() && !TargetSide.IsNearlyZero())
					{
						const double Angle = FMath::Atan2(FVector::DotProduct(Axis, FVector::CrossProduct(SourceSide, TargetSide)),
						                                  FVector::DotProduct(SourceSide, TargetSide));
						Turn = (FQuat(Axis, Angle) * Turn).GetNormalized();
					}
				}
			}
		}
		Lined[K] = (Turn * Above).GetNormalized();
		Out.SourceRest = (Lined[K] * Facing * ToAsset(Rest[Out.Bone].GetRotation())).GetNormalized();
		Out.TargetRest = Split(RestFrames[Out.Control]).Rotation;
		Out.SourceRestPosition = Facing.RotateVector(SourcePosition(Out.Bone));
	}

	// A driven control's frame depends on every control above it.
	InChain.Init(false, N);
	for (const FBound& B : Bound)
	{
		for (int32 C = B.Control; C != INDEX_NONE && !InChain[C]; C = Target->GetControlParent(C))
		{
			InChain[C] = true;
		}
	}
	// Fingers on their helpers, and everything under them, wait for the hands.
	Deferred.Init(false, N);
	for (int32 C = 0; C < N; ++C)
	{
		for (int32 Up = C; Up != INDEX_NONE && !Deferred[C]; Up = Target->GetControlParent(Up))
		{
			Deferred[C] = BoundOf[Up] != INDEX_NONE && Bound[BoundOf[Up]].Follow != INDEX_NONE;
		}
	}
	Wanted.SetNum(N);
	Status = FString::Printf(TEXT("driving %d controls from %s"), Bound.Num(), *Source->GetSkinnedAsset()->GetName());
	if (Missing.Num() > 0)
	{
		Status += TEXT("; not found: ") + FString::Join(Missing, TEXT(", "));
	}
	UE_LOG(LogRigExecRetarget, Log, TEXT("%s: %s (movement scale %.3f)"), *GetPathName(), *Status, TranslationScale);
	return true;
}

void
URigExecRetargetComponent::Drive()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_Retarget);
	const TArray<FTransform>& Pose = Source->GetComponentSpaceTransforms();
	if (Pose.Num() == 0)
	{
		return;
	}
	const double Start = FPlatformTime::Seconds();
	const double Cm = Target->GetCentimetresPerUnit();
	const int32 N = Target->GetControlCount();
	TMap<int32, FQuat> Turns;
	for (const TPair<FString, FQuat>& Turn : ExtraTurns)
	{
		const int32 C = Target->FindControlIndex(Turn.Key);
		if (C != INDEX_NONE)
		{
			Turns.Add(C, Turn.Value);
		}
	}
	// Each frame worked out from the root down: a control's offset is what
	// the rig last put between its value and its parent's frame (constant
	// for a plain hierarchy, and kept up when a constraint moves it).
	auto DriveControl = [&](int32 C)
	{
		const int32 Parent = Target->GetControlParent(C);
		FMatrix ParentWanted = Parent >= 0 ? Wanted[Parent] : FMatrix::Identity;
		FMatrix Now, ParentNow = FMatrix::Identity;
		const bool bNow = Target->GetControlAssetFrame(C, Now);
		if (Parent >= 0 && !Target->GetControlAssetFrame(Parent, ParentNow))
		{
			ParentNow = RestFrames[Parent];
		}
		// A helper the rig attaches to another control moves with it.
		const int32 Follow = BoundOf[C] != INDEX_NONE ? Bound[BoundOf[C]].Follow : INDEX_NONE;
		FMatrix FollowNow;
		if (Follow != INDEX_NONE && Target->GetControlAssetFrame(Follow, FollowNow))
		{
			ParentWanted = ParentNow * FollowNow.Inverse() * Wanted[Follow];
		}
		const FMatrix Value = Target->GetControlValueMatrix(C);
		// The offset from the run that made Now: with the values it ran on.
		const FMatrix Ran = bNow ? Target->GetLandedControlValueMatrix(C) : Value;
		const FMatrix Offset = Ran.Inverse() * (bNow ? Now : RestFrames[C]) * ParentNow.Inverse();
		const FMatrix Base = Value * Offset * ParentWanted;
		const int32 K = BoundOf[C];
		if (K == INDEX_NONE)
		{
			Wanted[C] = Base;
			return;
		}
		const FBound& B = Bound[K];
		const FQuat Bone = (Facing * ToAsset(Pose[B.Bone].GetRotation())).GetNormalized();
		// The bone's turn from its lined-up rest, applied to the control's rest.
		FRigFrame Frame = Split(Base);
		Frame.Rotation = (Bone * B.SourceRest.Inverse() * B.TargetRest).GetNormalized();
		if (const FQuat* Turn = Turns.Find(C))
		{
			Frame.Rotation = (*Turn * Frame.Rotation).GetNormalized();
		}
		if (B.bTranslate)
		{
			const FVector Moved = Facing.RotateVector(ToAsset(Pose[B.Bone].GetLocation(), Cm)) - B.SourceRestPosition;
			Frame.Origin = OriginOf(RestFrames[C]) + Moved * TranslationScale;
		}
		Wanted[C] = Join(Frame);
		FMatrix NewValue = Wanted[C] * ParentWanted.Inverse() * Offset.Inverse();
		if (!B.bTranslate)
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				NewValue.M[3][Axis] = Value.M[3][Axis];
			}
		}
		Target->SetControlValueMatrix(C, NewValue);
	};
	for (const bool bLater : {false, true})
	{
		for (int32 C = 0; C < N; ++C)
		{
			if (InChain[C] && Deferred[C] == bLater)
			{
				DriveControl(C);
			}
		}
	}
	Target->Evaluate();
	// How closely the rig followed: the landed run against what it was asked
	// (a run off the game thread lands a frame later).
	const auto& Asked = Target->bEvaluateAsync && LandedWanted.Num() == Wanted.Num() ? LandedWanted : Wanted;
	double Worst = 0.0;
	for (const FBound& B : Bound)
	{
		FMatrix Got;
		if (Target->GetControlAssetFrame(B.Control, Got) && Asked.IsValidIndex(B.Control))
		{
			Worst = FMath::Max(Worst, FMath::RadiansToDegrees(Split(Got).Rotation.AngularDistance(Split(Asked[B.Control]).Rotation)));
		}
	}
	LandedWanted = Wanted;
	LastErrorDegrees = float(Worst);
	LastMilliseconds = float((FPlatformTime::Seconds() - Start) * 1000.0);
}

void
URigExecRetargetComponent::SetExtraTurn(const FString& Control, const FQuat& Turn)
{
	if (Turn.Equals(FQuat::Identity, 1e-6))
	{
		ExtraTurns.Remove(Control);
	}
	else
	{
		ExtraTurns.Add(Control, Turn.GetNormalized());
	}
}

bool
URigExecRetargetComponent::DriveOnce()
{
	if (!bBound)
	{
		bBound = Bind();
		if (!bBound)
		{
			return false;
		}
	}
	Drive();
	return true;
}

void
URigExecRetargetComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                         FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bEnabled)
	{
		return;
	}
	if (!bBound)
	{
		bBound = Bind();
		if (!bBound)
		{
			return;
		}
	}
	Drive();
}
