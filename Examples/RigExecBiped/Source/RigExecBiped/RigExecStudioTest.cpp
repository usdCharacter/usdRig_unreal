// -RigExecStudioTest: drives the studio's tools one after another, as a
// person would, and checks what they did to the rig: the default bake, a
// clip and a pose applied, TouchPose's pick and its rotate and move drags,
// keys and the curve between them, saving and loading, the IK/FK and space
// switches (matched: nothing moves), the hover picker. Writes
// Saved/RigExecStudioTest/<character>.txt and screenshots, then quits.
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RigExecBipedCharacter.h"
#include "RigExecComponent.h"
#include "Misc/CommandLine.h"
#include "RigExecStudioGameMode.h"
#include "RigExecVR.h"
#include "UnrealClient.h"

using namespace RigExecStudio;

void ARigExecStudioHUD::TestNote(const FString& Line)
{
	TestLog.Add(Line);
	UE_LOG(LogTemp, Log, TEXT("RXSTUDIO %s"), *Line);
	const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RigExecStudioTest"), CharacterName() + TEXT(".txt"));
	FFileHelper::SaveStringToFile(FString::Join(TestLog, TEXT("\n")) + TEXT("\n"), *File);
}

FVector ARigExecStudioHUD::TestWorld(const FString& Control) const
{
	const URigExecComponent* R = Rig();
	const int32 C = R ? R->FindControlIndex(Control) : INDEX_NONE;
	return C == INDEX_NONE ? FVector::ZeroVector
	                       : R->GetComponentTransform().TransformPosition(R->GetRigFrame(R->GetControlPath(C)).GetLocation());
}

void ARigExecStudioHUD::TestShot(const FString& Name)
{
	const FString Shot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RigExecStudioTest"), CharacterName() + TEXT("_") + Name + TEXT(".png"));
	FScreenshotRequest::RequestScreenshot(FPaths::ConvertRelativePathToFull(Shot), true, false);
}

void ARigExecStudioHUD::RunTest(float DeltaSeconds)
{
	URigExecComponent* R = Rig();
	if (!R || !PlayerOwner)
	{
		return;
	}
	TestWait -= DeltaSeconds;
	if (TestWait > 0.0f)
	{
		return;
	}
	auto Pass = [](bool b) { return b ? TEXT("PASS") : TEXT("FAIL"); };
	auto Find = [](const auto& List, const FString& Name) {
		return List.IndexOfByPredicate([&Name](const auto& Item) { return Item.Name == Name; });
	};
	auto Evaluate = [this, R]() {
		ApplyEdit();
		R->Evaluate();
		bDirty = false;
	};
	// A drag on the skin, as the mouse would make it: pressed at the origin,
	// let go at (Dx, Dy).
	auto DragControl = [this, R](const FString& Name, float Dx, float Dy, EPoseMode Mode) {
		Selected = {R->FindControlIndex(Name)};
		PoseMode = Mode;
		bCtrl = bAlt = bShift = false;
		Drag = FDrag();
		Drag.Kind = EDrag::Pose;
		Drag.Start = FVector2D::ZeroVector;
		Drag.bMoved = true;
		BeginPoseDrag();
		Mouse = FVector2D(Dx, Dy);
		UpdatePoseDrag();
		R->Evaluate();
		EndDrag();
		R->Evaluate();
		bDirty = false;
	};
	auto Angle = [](const FVector& A, const FVector& B) {
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(A.GetSafeNormal(), B.GetSafeNormal()), -1.0, 1.0)));
	};
	const int32 Step = TestStep++;
	TestWait = 0.4f;
	switch (Step)
	{
	case 0:
	{
		TestNote(FString::Printf(TEXT("character %s: %d controls, %d channels, %d touch regions, %d picker panels"), *CharacterName(),
		                         Binding.ControlOf.Num(), Binding.ChannelOf.Num(), R->GetTouchRegionCount(), Picker.Panels.Num()));
		FString Names;
		for (const FClip& Clip : Clips)
		{
			Names += FString::Printf(TEXT(" %s(%d frames, %d tracks%s)"), *Clip.Name, Clip.Length, Clip.Controls.Num(), Clip.bBuiltIn ? TEXT("") : TEXT(", mine"));
		}
		TestNote(TEXT("animations:") + Names);
		Names.Reset();
		for (const FPose& Pose : Poses)
		{
			Names += TEXT(" ") + Pose.Name;
		}
		TestNote(TEXT("poses:") + Names);
		const int32 Builtin = Clips.FilterByPredicate([](const FClip& C) { return C.bBuiltIn; }).Num();
		TestNote(FString::Printf(TEXT("default clips %d  %s"), Builtin, Pass(Builtin >= 5)));
		TestAt.Add(TEXT("rest hand"), TestWorld(TEXT("L_Hand")));
		TestShot(TEXT("0_start"));
		break;
	}
	case 1:
	{
		const int32 Walk = Find(Clips, TEXT("Walk"));
		UseClip(Walk, false);
		SetFrame(10.0f);
		Evaluate();
		const double Moved = FVector::Dist(TestWorld(TEXT("L_Hand")), TestAt[TEXT("rest hand")]);
		TestNote(FString::Printf(TEXT("walk applied, frame 10: hand %.1f cm from rest  %s"), Moved, Pass(Walk != INDEX_NONE && Moved > 2.0)));
		TestAt.Add(TEXT("walk10"), TestWorld(TEXT("L_Hand")));
		TestShot(TEXT("1_walk"));
		bPlaying = true;
		TestWait = 1.0f;
		break;
	}
	case 2:
	{
		bPlaying = false;
		const float At = Frame;
		Evaluate();
		TestNote(FString::Printf(TEXT("played 1 s: frame %.1f, hand %.1f cm from frame 10  %s"), At,
		                         FVector::Dist(TestWorld(TEXT("L_Hand")), TestAt[TEXT("walk10")]), Pass(At > 20.0f || At < 9.0f)));
		const int32 Air = Find(Poses, TEXT("Jump - Air"));
		const FVector Before = TestWorld(TEXT("L_Hand"));
		PoseMix = 1.0f;
		UsePose(Air);
		Evaluate();
		TestNote(FString::Printf(TEXT("pose Jump - Air: hand moved %.1f cm  %s"), FVector::Dist(TestWorld(TEXT("L_Hand")), Before),
		                         Pass(Air != INDEX_NONE)));
		TestShot(TEXT("2_pose"));
		break;
	}
	case 3:
	{
		// A fresh animation from rest; rotate the upper arm by dragging the skin.
		Edit = FClip();
		Edit.Name = TEXT("Untitled");
		SetFrame(0.0f);
		UsePose(Find(Poses, TEXT("Rest")));
		Evaluate();
		// The control's own turn, in the world: what the drag asked for, about
		// the view's up (the rig may carry the shoulder along with the arm).
		auto Turn = [this, R](const TCHAR* Name) {
			const FTransform Posed = R->GetRigFrame(R->GetControlPath(R->FindControlIndex(Name)));
			return (R->GetComponentTransform().GetRotation() * Posed.GetRotation()).GetNormalized();
		};
		FVector Eye, Forward, ViewRight, Up;
		GetView(Eye, Forward, ViewRight, Up);
		for (const TCHAR* Name : {TEXT("M_Head"), TEXT("L_UpArm")})
		{
			const FQuat Q0 = Turn(Name);
			const FVector Pivot0 = TestWorld(Name);
			DragControl(Name, 80.0f, 0.0f, EPoseMode::Rotate);
			const FQuat Delta = (Turn(Name) * Q0.Inverse()).GetNormalized();
			FVector Axis;
			double Radians = 0.0;
			Delta.ToAxisAndAngle(Axis, Radians);
			const double Degrees = FMath::RadiansToDegrees(Radians);
			const double Along = FMath::Abs(FVector::DotProduct(Axis, Up));
			TestNote(FString::Printf(TEXT("rotate drag 80 px on %s: turned %.2f deg (want 36) about an axis %.3f along the view's up, pivot moved %.3f cm  %s"),
			                         Name, Degrees, Along, FVector::Dist(Pivot0, TestWorld(Name)),
			                         Pass(FMath::Abs(Degrees - 36.0) < 1.0 && Along > 0.99)));
		}
		const FVector P1 = TestWorld(TEXT("L_UpArm")), E1 = TestWorld(TEXT("L_LoArm"));
		const FTrack* Track = Edit.Controls.Find(R->GetControlPath(R->FindControlIndex(TEXT("L_UpArm"))));
		TestNote(FString::Printf(TEXT("auto key: L_UpArm keyed on frame 0  %s"), Pass(Track && Track->FindKey(0) != INDEX_NONE)));
		// Directions as the hand expects: the surface under the cursor goes
		// where the cursor goes. Down on the jaw drops the chin; right on the
		// head carries the eyes (which face the camera) to the right.
		{
			const TCHAR* Chin = R->FindControlIndex(TEXT("M_Chin")) != INDEX_NONE ? TEXT("M_Chin") : TEXT("M_Jaw");
			const FVector C0 = TestWorld(Chin);
			DragControl(TEXT("M_Jaw"), 0.0f, 30.0f, EPoseMode::Rotate);
			const double Down = FVector::DotProduct(TestWorld(Chin) - C0, Up);
			const FVector Eye0 = TestWorld(TEXT("L_Eye"));
			DragControl(TEXT("M_Head"), 30.0f, 0.0f, EPoseMode::Rotate);
			const double Right = FVector::DotProduct(TestWorld(TEXT("L_Eye")) - Eye0, ViewRight);
			TestNote(FString::Printf(TEXT("drag down on the jaw: chin %.2f cm along the view's up (want < 0); drag right on the head: eye %.2f cm along its right (want > 0)  %s"),
			                         Down, Right, Pass(Down < 0.0 && Right > 0.0)));
		}
		TestAt.Add(TEXT("e1"), E1);
		TestAt.Add(TEXT("p1"), P1);
		TestShot(TEXT("3_rotate"));
		break;
	}
	case 4:
	{
		// The other way on frame 20, then the curve between at 10.
		SetFrame(20.0f);
		ApplyEdit(true);
		DragControl(TEXT("L_UpArm"), -80.0f, 0.0f, EPoseMode::Rotate);
		const FVector E20 = TestWorld(TEXT("L_LoArm"));
		SetFrame(10.0f);
		Evaluate();
		const FVector P = TestAt[TEXT("p1")];
		const FVector E10 = TestWorld(TEXT("L_LoArm"));
		const double A = Angle(TestAt[TEXT("e1")] - P, E10 - P), B = Angle(E20 - P, E10 - P);
		TestNote(FString::Printf(TEXT("keys on 0 and 20: frame 10 is %.1f deg from 0 and %.1f from 20 (want about equal)  %s"), A, B,
		                         Pass(FMath::Abs(A - B) < 6.0 && A > 10.0)));
		TestNote(FString::Printf(TEXT("all key frames: %s"), *FString::JoinBy(Edit.KeyFrames(), TEXT(" "), [](int32 F) { return FString::FromInt(F); })));
		break;
	}
	case 5:
	{
		// Move: the body follows the cursor across the view.
		SetFrame(30.0f);
		const FVector B0 = TestWorld(TEXT("M_Body"));
		FVector Eye, Forward, Right, Up;
		GetView(Eye, Forward, Right, Up);
		const double Want = 100.0 * PixelSizeAt(B0);
		DragControl(TEXT("M_Body"), 100.0f, 0.0f, EPoseMode::Move);
		const FVector Moved = TestWorld(TEXT("M_Body")) - B0;
		TestNote(FString::Printf(TEXT("move drag 100 px: body moved %.2f cm (want %.2f), along the view's right %.3f  %s"), Moved.Size(), Want,
		                         FVector::DotProduct(Moved.GetSafeNormal(), Right),
		                         Pass(FMath::Abs(Moved.Size() - Want) < Want * 0.05 && FVector::DotProduct(Moved.GetSafeNormal(), Right) > 0.98)));
		// The manipulator: drag a handle as the mouse would.
		auto Handle = [this, R](const TCHAR* Name, EPoseMode Mode, int32 Which, const FVector2D& From, const FVector2D& Delta) {
			Selected = {R->FindControlIndex(Name)};
			PoseMode = Mode;
			bCtrl = bAlt = bShift = false;
			Drag = FDrag();
			Drag.Kind = EDrag::Gizmo;
			Drag.Start = From;
			Drag.bMoved = true;
			BeginPoseDrag();
			Drag.Index = Which;
			Mouse = From + Delta;
			UpdateGizmoDrag();
			R->Evaluate();
			EndDrag();
			R->Evaluate();
			bDirty = false;
		};
		{
			// Move along the body's first axis: it goes along that axis only.
			Selected = {R->FindControlIndex(TEXT("M_Body"))};
			PoseMode = EPoseMode::Move;
			FGizmo G;
			GetGizmo(G);
			FVector2D Tip;
			ToScreen(G.Pivot + G.MoveAxes[0] * G.Length, Tip);
			const FVector2D Image = Tip - G.Centre;
			const FVector P0 = TestWorld(TEXT("M_Body"));
			Handle(TEXT("M_Body"), EPoseMode::Move, 0, Tip, Image * 0.5);
			const FVector D = TestWorld(TEXT("M_Body")) - P0;
			TestNote(FString::Printf(TEXT("gizmo move arrow 0 by half its length: moved %.2f cm (want %.2f), %.3f along the axis  %s"), D.Size(),
			                         G.Length * 0.5, FVector::DotProduct(D.GetSafeNormal(), G.MoveAxes[0]),
			                         Pass(FMath::Abs(D.Size() - G.Length * 0.5) < G.Length * 0.05 && FVector::DotProduct(D.GetSafeNormal(), G.MoveAxes[0]) > 0.99)));
		}
		{
			// The view ring: a quarter turn of the cursor about the head turns
			// the eyes a quarter turn about it on screen, the same way.
			Selected = {R->FindControlIndex(TEXT("M_Head"))};
			PoseMode = EPoseMode::Rotate;
			FGizmo G;
			GetGizmo(G);
			const double Outer = 78.0 * S * 1.08;
			FVector2D Eye0, Eye1;
			ToScreen(TestWorld(TEXT("L_Eye")), Eye0);
			const FVector2D From = G.Centre + FVector2D(Outer, 0.0);
			Handle(TEXT("M_Head"), EPoseMode::Rotate, 7, From, G.Centre + FVector2D(Outer * FMath::Cos(PI / 6), Outer * FMath::Sin(PI / 6)) - From);
			ToScreen(TestWorld(TEXT("L_Eye")), Eye1);
			const double Turned = FMath::RadiansToDegrees(FMath::UnwindRadians(FMath::Atan2((Eye1 - G.Centre).Y, (Eye1 - G.Centre).X) -
			                                                                    FMath::Atan2((Eye0 - G.Centre).Y, (Eye0 - G.Centre).X)));
			TestNote(FString::Printf(TEXT("gizmo view ring swept 30 deg on screen: the eye swept %.1f deg about the head  %s"), Turned,
			                         Pass(FMath::Abs(Turned - 30.0) < 6.0)));
		}
		{
			// Scale: the middle box, across by half a handle, grows all three.
			const int32 C = R->FindControlIndex(TEXT("M_Head"));
			double Before[9], After[9];
			R->GetControlAvars(C, Before);
			Selected = {C};
			PoseMode = EPoseMode::Scale;
			FGizmo G;
			GetGizmo(G);
			if (G.Mode == EPoseMode::Scale)
			{
				Handle(TEXT("M_Head"), EPoseMode::Scale, 11, G.Centre, FVector2D(78.0 * S * 0.5, 0.0));
				R->GetControlAvars(C, After);
				TestNote(FString::Printf(TEXT("gizmo scale box: sx %.3f -> %.3f, sy %.3f -> %.3f  %s"), Before[6], After[6], Before[7], After[7],
				                         Pass(FMath::Abs(After[6] / Before[6] - 1.5) < 0.02 && FMath::Abs(After[7] / Before[7] - 1.5) < 0.02)));
				Undo(false);
				R->Evaluate();
			}
			else
			{
				TestNote(TEXT("M_Head has no scale channels: scale gizmo not checked"));
			}
		}
		PoseMode = EPoseMode::Auto;
		TestShot(TEXT("5_move"));
		break;
	}
	case 6:
	{
		// Saving and finding them again.
		SaveClip(TEXT("Studio Test Anim"));
		SavePose(TEXT("Studio Test Pose"));
		LoadLibrary();
		const int32 C = Find(Clips, TEXT("Studio Test Anim")), P = Find(Poses, TEXT("Studio Test Pose"));
		TestNote(FString::Printf(TEXT("saved and reloaded: animation %s, pose %s, %d keyed controls  %s"), C != INDEX_NONE ? TEXT("found") : TEXT("missing"),
		                         P != INDEX_NONE ? TEXT("found") : TEXT("missing"), C != INDEX_NONE ? Clips[C].Controls.Num() : 0,
		                         Pass(C != INDEX_NONE && P != INDEX_NONE && !bBaking)));
		if (C != INDEX_NONE)
		{
			UseClip(C, false);
			DeleteItem(false, C);
		}
		const int32 P2 = Find(Poses, TEXT("Studio Test Pose"));
		DeleteItem(true, P2);
		break;
	}
	case 7:
	{
		// IK/FK: the explicit switch, then picking an FK control in IK.
		SetFrame(10.0f);
		Evaluate();
		const int32 UpArm = R->FindControlIndex(TEXT("L_UpArm"));
		const FString Switch = R->FindLimbSwitchFor(R->GetControlElement(UpArm));
		const FVector W0 = TestWorld(TEXT("L_Hand"));
		const bool bOk = SwitchLimbMatched(Switch);
		R->Evaluate();
		const int32 Ik = R->FindControlIndex(TEXT("L_ArmIK"));
		const FVector W1 = Ik != INDEX_NONE ? TestWorld(TEXT("L_ArmIK")) : W0;
		TestNote(FString::Printf(TEXT("switch %s to IK: ok=%d now IK=%d, IK hand %.3f cm from the FK hand  %s"), *Switch, bOk, R->IsLimbIk(Switch),
		                         FVector::Dist(W0, W1), Pass(bOk && R->IsLimbIk(Switch) && FVector::Dist(W0, W1) < 0.5)));
		const FVector E0 = TestWorld(TEXT("L_LoArm"));
		Select({R->FindControlIndex(TEXT("L_LoArm"))}, 0);
		R->Evaluate();
		TestNote(FString::Printf(TEXT("pick L_LoArm while in IK: now FK=%d, elbow moved %.3f cm  %s"), !R->IsLimbIk(Switch),
		                         FVector::Dist(E0, TestWorld(TEXT("L_LoArm"))),
		                         Pass(!R->IsLimbIk(Switch) && FVector::Dist(E0, TestWorld(TEXT("L_LoArm"))) < 0.5)));
		if (Ik != INDEX_NONE)
		{
			const FVector H0 = TestWorld(TEXT("L_Hand"));
			Select({Ik}, 0);
			R->Evaluate();
			TestNote(FString::Printf(TEXT("pick L_ArmIK while in FK: now IK=%d, hand moved %.3f cm  %s"), R->IsLimbIk(Switch),
			                         FVector::Dist(H0, TestWorld(TEXT("L_ArmIK"))), Pass(R->IsLimbIk(Switch) && FVector::Dist(H0, TestWorld(TEXT("L_ArmIK"))) < 0.5)));
		}
		TestShot(TEXT("7_ikfk"));
		break;
	}
	case 8:
	{
		// Spaces on the IK hand: each one keeps it still.
		const int32 Ik = R->FindControlIndex(TEXT("L_ArmIK"));
		FName Channel;
		TArray<FString> Labels;
		if (Ik == INDEX_NONE || !R->FindSpaceChannel(R->GetControlElement(Ik), Channel, Labels))
		{
			TestNote(TEXT("no space switch on L_ArmIK: skipped"));
			break;
		}
		for (int32 I = 1; I <= Labels.Num(); ++I)
		{
			const int32 To = I % Labels.Num();
			const FVector H0 = TestWorld(TEXT("L_ArmIK"));
			const bool bOk = SwitchSpaceMatched(Ik, To);
			R->Evaluate();
			const double D = FVector::Dist(H0, TestWorld(TEXT("L_ArmIK")));
			TestNote(FString::Printf(TEXT("space -> %s: ok=%d, IK hand moved %.3f cm  %s"), *Labels[To], bOk, D, Pass(bOk && D < 0.05)));
		}
		break;
	}
	case 9:
	{
		// TouchPose's pick: the cursor over each control finds one, by the
		// skin's regions or, where the skin has none, the control's dot.
		int32 Hits = 0;
		TestNote(FString::Printf(TEXT("%d controls have no touch region (dots)"), Untouched.Num()));
		for (const TCHAR* Name : {TEXT("L_UpArm"), TEXT("M_Head"), TEXT("M_Chest"), TEXT("R_UpLeg")})
		{
			FVector2D At;
			int32 Region = -1;
			const int32 Control = ToScreen(TestWorld(Name), At) ? ControlUnder(At, Region) : INDEX_NONE;
			Hits += Control != INDEX_NONE ? 1 : 0;
			FString Found = Control != INDEX_NONE ? R->GetControlPath(Control) : FString(TEXT("nothing"));
			int32 Slash = INDEX_NONE;
			if (Found.FindLastChar(TEXT('/'), Slash))
			{
				Found = Found.Mid(Slash + 1);
			}
			TestNote(FString::Printf(TEXT("cursor on %s: region %d (%s) -> %s"), Name, Region,
			                         Region >= 0 ? *R->GetTouchRegionName(Region) : TEXT("dot"), *Found));
		}
		TestNote(FString::Printf(TEXT("touch pick: %d of 4 found a control  %s"), Hits, Pass(Hits == 4)));
		// The hover picker: a button for the upper arm selects it.
		int32 Found = 0;
		const int32 UpArm = R->FindControlIndex(TEXT("L_UpArm"));
		for (int32 P = 0; P < Picker.Panels.Num() && !Found; ++P)
		{
			for (int32 B = 0; B < Picker.Panels[P].Buttons.Num(); ++B)
			{
				const FPickerButton& Button = Picker.Panels[P].Buttons[B];
				if (Button.Targets.Num() == 1 && Button.Targets[0] == UpArm && Picker.Shown(Button) > 0)
				{
					Selected.Reset();
					PickButton(P, B);
					Found = Selected.Num() == 1 && Selected[0] == UpArm ? 1 : -1;
					break;
				}
			}
		}
		TestNote(FString::Printf(TEXT("hover picker button for L_UpArm selects it  %s"), Pass(Found == 1 || Picker.Panels.Num() == 0)));
		HomeCamera();
		TestWait = 1.0f;
		break;
	}
	case 10:
	{
		// The face: each obvious control moved a set amount through the
		// manipulator's path. Its frame and the skin must both move.
		auto Skin = [R]() {
			TArray<FVector> Points;
			const TArray<UDynamicMeshComponent*> Meshes = R->GetMeshes();
			for (int32 M = 0; M < Meshes.Num(); ++M)
			{
				for (int32 V = 0;; ++V)
				{
					const FVector P = R->GetVertex(M, V);
					if (P.IsZero() && V > 0)
					{
						break;
					}
					Points.Add(P);
				}
			}
			return Points;
		};
		const FVector Up = R->GetComponentTransform().GetUnitAxis(EAxis::Z);
		FVector Eye, Forward, Side, ViewUp;
		GetView(Eye, Forward, Side, ViewUp);
		struct FTry
		{
			const TCHAR* Name;
			bool bTurn;
		};
		for (const FTry& Try : {FTry{TEXT("M_Jaw"), true}, FTry{TEXT("M_Head"), true}, FTry{TEXT("L_Mouth"), false},
		                        FTry{TEXT("R_Mouth"), false}, FTry{TEXT("L_Brow"), false}, FTry{TEXT("R_Brow"), false},
		                        FTry{TEXT("L_InBrow"), false}, FTry{TEXT("R_InBrow"), false}})
		{
			const int32 C = R->FindControlIndex(Try.Name);
			if (C == INDEX_NONE)
			{
				TestNote(FString::Printf(TEXT("face %s: no such control"), Try.Name));
				continue;
			}
			R->Evaluate();
			const TArray<FVector> Before = Skin();
			const FVector P0 = TestWorld(Try.Name);
			Selected = {C};
			Drag = FDrag();
			Drag.Kind = EDrag::Gizmo;
			BeginPoseDrag();
			const int32 Targets = Drag.Targets.Num();
			FMatrix Posed;
			const bool bFrame = R->GetControlAssetFrame(C, Posed);
			ApplyWorldEdit(Try.bTurn ? FQuat(Try.Name == FString(TEXT("M_Head")) ? Up : Side, FMath::DegreesToRadians(15.0)) : FQuat::Identity,
			               Try.bTurn ? FVector::ZeroVector : Up * 1.0, 1.0, -1);
			R->Evaluate();
			double Avars[9];
			R->GetControlAvars(C, Avars);
			const TArray<FVector> After = Skin();
			double Most = 0.0;
			for (int32 K = 0; K < Before.Num() && K < After.Num(); ++K)
			{
				Most = FMath::Max(Most, FVector::Dist(Before[K], After[K]));
			}
			TestNote(FString::Printf(TEXT("face %s %s: frame=%d targets=%d, control moved %.2f cm, skin moved up to %.2f cm, avars t(%.2f %.2f %.2f) r(%.1f %.1f %.1f)  %s"),
			                         Try.Name, Try.bTurn ? TEXT("turn 15") : TEXT("up 1 cm"), bFrame, Targets, FVector::Dist(P0, TestWorld(Try.Name)), Most,
			                         Avars[0], Avars[1], Avars[2], Avars[3], Avars[4], Avars[5], Pass(Most > 0.3)));
			EndDrag();
			Undo(false);
			R->Evaluate();
		}
		break;
	}
	case 11:
	{
		// VR (-RigExecVRSim, or a headset): the panel's view and the grip.
		if (!IsVR())
		{
			TestNote(TEXT("not in VR: the VR checks skipped"));
			break;
		}
		HomeCamera();
		// A point on the character to the panel and back: the line from the
		// eye through its pixel passes through it.
		const FVector Head = TestWorld(TEXT("M_Head"));
		FVector2D At;
		FVector Origin, Direction;
		const bool bOn = ToScreen(Head, At);
		FromScreen(At, Origin, Direction);
		const double Miss = FMath::PointDistToLine(Head, Direction, Origin);
		TestNote(FString::Printf(TEXT("vr: the head at panel pixel (%.0f, %.0f), the eye's line through it misses it by %.4f cm  %s"), At.X, At.Y,
		                         Miss, Pass(bOn && Miss < 0.05)));
		// The grip, held from afar (at the eye): the body goes where the hand
		// goes; the upper arm turns as the hand turns.
		FVector Eye, Forward, ViewRight, ViewUp;
		GetView(Eye, Forward, ViewRight, ViewUp);
		auto Grab = [this, R, Eye](const TCHAR* Name, const FVector& Move, const FQuat& Turn) {
			Selected = {R->FindControlIndex(Name)};
			Drag = FDrag();
			VRIn.bOnPanel = false;
			VRIn.bGrip = VRIn.bGripPress = true;
			VRIn.GripAt = Eye;
			VRIn.GripTurn = FQuat::Identity;
			UpdateGrab();
			VRIn.bGripPress = false;
			VRIn.GripAt = Eye + Move;
			VRIn.GripTurn = Turn;
			UpdateGrab();
			R->Evaluate();
		};
		auto Let = [this, R]() {
			VRIn.bGrip = false;
			UpdateGrab();
			R->Evaluate();
			Undo(false);
			R->Evaluate();
			bDirty = false;
		};
		const FVector B0 = TestWorld(TEXT("M_Body"));
		Grab(TEXT("M_Body"), FVector(0.0, 0.0, 10.0), FQuat::Identity);
		const FVector Moved = TestWorld(TEXT("M_Body")) - B0;
		const bool bHolding = Drag.Kind == EDrag::Grab;
		Let();
		TestNote(FString::Printf(TEXT("vr grip: hand up 10 cm, body moved (%.2f %.2f %.2f)  %s"), Moved.X, Moved.Y, Moved.Z,
		                         Pass(bHolding && FMath::Abs(Moved.Z - 10.0) < 0.5 && Moved.Size2D() < 0.5)));
		auto Turned = [this, R](const TCHAR* Name) {
			const FTransform Posed = R->GetRigFrame(R->GetControlPath(R->FindControlIndex(Name)));
			return (R->GetComponentTransform().GetRotation() * Posed.GetRotation()).GetNormalized();
		};
		const FQuat Q0 = Turned(TEXT("L_UpArm"));
		Grab(TEXT("L_UpArm"), FVector::ZeroVector, FQuat(FVector::UpVector, FMath::DegreesToRadians(20.0)));
		const FQuat Delta = (Turned(TEXT("L_UpArm")) * Q0.Inverse()).GetNormalized();
		Let();
		FVector Axis;
		double Radians = 0.0;
		Delta.ToAxisAndAngle(Axis, Radians);
		TestNote(FString::Printf(TEXT("vr grip: hand turned 20 deg, upper arm turned %.2f deg about an axis %.3f along up  %s"),
		                         FMath::RadiansToDegrees(Radians), FMath::Abs(Axis.Z),
		                         Pass(FMath::Abs(FMath::RadiansToDegrees(Radians) - 20.0) < 1.0 && FMath::Abs(Axis.Z) > 0.99)));
		TestNote(TEXT("vr panel: ") + GetVR()->DescribePanel({{20, 18}, {1024, 576}, {1024, 18}, {100, 300}, {1900, 300}}));
		TestShot(TEXT("8_vr"));
		break;
	}
	case 12:
		if (GetVR() && FParse::Param(FCommandLine::Get(), TEXT("RigExecVRInvert0")))
		{
			GetVR()->SetPanelInvert(0.0f);
		}
		TestShot(TEXT("9_final"));
		TestWait = 1.0f;
		break;
	default:
		TestNote(TEXT("DONE"));
		UKismetSystemLibrary::QuitGame(this, PlayerOwner, EQuitPreference::Quit, false);
		bTest = false;
		break;
	}
}
