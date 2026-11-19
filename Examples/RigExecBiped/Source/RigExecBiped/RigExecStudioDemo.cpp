// -RigExecStudioDemo, the studio's part: for each character, choose it in
// the lobby, apply an animation and poses from the library, pose the face
// with TouchPose, turn the head with the manipulator and the channel box,
// raise an arm and crouch the body with the manipulator, key two frames and
// play them, switch the arm to IK and its hand's space (both matched), and
// save the pose. All through the real UI: the tour's cursor is the mouse.
#include "Camera/PlayerCameraManager.h"
#include "Engine/Canvas.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/CommandLine.h"
#include "RigExecBipedCharacter.h"
#include "RigExecComponent.h"
#include "RigExecStudioGameMode.h"

using namespace RigExecStudio;

FVector2D ARigExecStudioHUD::DemoScreenOf(const FString& Control, const FString& Toward, float Amount) const
{
	FVector World = TestWorld(Control);
	if (!Toward.IsEmpty())
	{
		World = FMath::Lerp(World, TestWorld(Toward), double(Amount));
	}
	FVector2D Out = DemoMouse;
	if (PlayerOwner)
	{
		ToScreen(World, Out);
	}
	return Out;
}

int32 ARigExecStudioHUD::DemoAxisNear(const FVector& World, bool bRing) const
{
	FGizmo G;
	if (!GetGizmo(G))
	{
		return bRing ? 7 : 3;
	}
	int32 Best = INDEX_NONE;
	double Most = -1.0;
	for (int32 A = 0; A < 3; ++A)
	{
		const bool bOn = (G.Mask & (1 << (A + (bRing ? 3 : 0)))) != 0;
		const double Dot = FMath::Abs(FVector::DotProduct(bRing ? G.OwnAxes[A] : G.MoveAxes[A], World));
		if (bOn && Dot > Most)
		{
			Most = Dot;
			Best = A;
		}
	}
	return Best == INDEX_NONE ? (bRing ? 7 : 3) : (bRing ? 4 : 0) + Best;
}

FVector2D ARigExecStudioHUD::DemoHandle(int32 Handle) const
{
	FGizmo G;
	if (!GetGizmo(G))
	{
		return DemoMouse;
	}
	FVector2D Out = G.Centre;
	if (Handle == 3 || Handle == 11)
	{
		return Out;
	}
	if (Handle == 7)
	{
		return G.Centre + FVector2D(78.0 * S * 1.08, 0.0);
	}
	if (Handle <= 2 || Handle >= 8)
	{
		const FVector Axis = Handle <= 2 ? G.MoveAxes[Handle] : G.OwnAxes[Handle - 8];
		ToScreen(G.Pivot + Axis * G.Length * 0.8, Out);
		return Out;
	}
	// A ring: its lowest point on screen.
	FVector U, V;
	G.OwnAxes[Handle - 4].FindBestAxisVectors(U, V);
	double Lowest = -1e9;
	for (int32 K = 0; K < 48; ++K)
	{
		const double A = 2.0 * PI * K / 48;
		FVector2D At;
		if (ToScreen(G.Pivot + (U * FMath::Cos(A) + V * FMath::Sin(A)) * G.Length * 0.85, At) && At.Y > Lowest)
		{
			Lowest = At.Y;
			Out = At;
		}
	}
	return Out;
}

void ARigExecStudioHUD::BuildDemo()
{
	// Every character in turn, or only -RigExecDemoCharacter=<Name>'s.
	FString Only;
	FParse::Value(FCommandLine::Get(), TEXT("RigExecDemoCharacter="), Only);
	TArray<FString> Labels;
	for (const ARigExecBipedCharacter::FChoice& Choice : ARigExecBipedCharacter::FindCharacters())
	{
		if (Only.IsEmpty() || Choice.Label.Equals(Only, ESearchCase::IgnoreCase))
		{
			Labels.Add(Choice.Label);
		}
	}
	DemoDo([this]() {
		DemoMouse = Canvas ? FVector2D(Canvas->ClipX * 0.5f, Canvas->ClipY * 0.6f) : FVector2D(960.0, 640.0);
	});
	for (int32 K = 0; K < Labels.Num(); ++K)
	{
		BuildDemoFor(Labels[K], K == Labels.Num() - 1);
	}
	DemoSay(TEXT("TouchPose, the hover picker, manipulators, the timeline and the library: all in the packaged game"));
	DemoWait(4.0f);
	DemoDo([this]() { UKismetSystemLibrary::QuitGame(this, PlayerOwner, EQuitPreference::Quit, false); });
}

void ARigExecStudioHUD::BuildDemoFor(const FString& Label, bool bLast)
{
	const FString Name = Label.Left(1) + Label.Mid(1).ToLower();
	auto Hold = [this]() { DemoWait(0.35f); };

	// The lobby.
	DemoUntil([this]() { return IsChoosing(); }, 30.0f);
	DemoSay(TEXT("Animation Studio: choose who to animate"));
	DemoWait(1.6f);
	DemoGlideTo(
		[this, Label]() {
			const TArray<ARigExecBipedCharacter::FChoice> Choices = ARigExecBipedCharacter::FindCharacters();
			const int32 Index = Choices.IndexOfByPredicate([&Label](const ARigExecBipedCharacter::FChoice& C) { return C.Label == Label; });
			FVector2D At = DemoMouse;
			DemoPlate(FMath::Max(Index, 0), At);
			return At;
		},
		1.1f);
	Hold();
	DemoClick();
	DemoDo([this, Label]() {
		const TArray<ARigExecBipedCharacter::FChoice> Choices = ARigExecBipedCharacter::FindCharacters();
		DemoChooseCharacter(FMath::Max(0, Choices.IndexOfByPredicate([&Label](const ARigExecBipedCharacter::FChoice& C) { return C.Label == Label; })));
	});
	DemoUntil([this, Label]() { return !IsMenuOpen() && Rig() && !bBaking && CharacterName().ToUpper() == Label; }, 90.0f);
	DemoWait(1.2f);

	// The library: an animation, then poses.
	DemoSay(Name + TEXT(": apply an animation from the library (Walk, baked from the Mannequin)"));
	DemoGlide(TEXT("clip:Walk"), 1.0f);
	Hold();
	DemoClick();
	DemoWait(0.7f);
	DemoGlide(TEXT("Play"), 0.9f);
	DemoClick();
	DemoWait(3.2f);
	DemoGlide(TEXT("Pause"), 0.5f);
	DemoClick();
	DemoSay(Name + TEXT(": apply poses from the library"));
	DemoGlide(TEXT("pose:Jump - Air"), 1.0f);
	DemoClick();
	DemoWait(1.3f);
	DemoGlide(TEXT("pose:Punch"), 0.7f);
	DemoClick();
	DemoWait(1.3f);
	DemoSay(Name + TEXT(": a new animation, from the Rest pose"));
	DemoGlide(TEXT("New"), 1.0f);
	DemoClick();
	DemoGlide(TEXT("pose:Rest"), 1.0f);
	DemoClick();
	DemoWait(0.8f);

	// The face: the picker picks each control; the channel box and the
	// manipulators move it a set, visible amount.
	DemoSay(Name + TEXT(": the face: pick a control in the picker, then pose it"));
	DemoDo([this]() {
		const FVector L = TestWorld(TEXT("L_Eye")), Rr = TestWorld(TEXT("R_Eye"));
		const float Eyes = float(FVector::Dist(L, Rr));
		CamFocusWant = (L + Rr) * 0.5 - FVector(0.0, 0.0, Eyes * 0.7);
		CamDistWant = FMath::Max(30.0f, Eyes * 9.0f);
		CamYaw = Bound->GetActorRotation().Yaw + 180.0f - 14.0f;
		CamPitch = -3.0f;
	});
	DemoGlide(TEXT("Face"), 0.9f);
	DemoClick();
	DemoWait(0.6f);
	// The jaw: open it from the channel box (its rx, the way that drops the chin).
	DemoSay(Name + TEXT(": open the jaw: pick it, drag its rotate value in the channel box"));
	DemoGlide(TEXT("pick:M_Jaw"), 0.9f);
	DemoClick();
	DemoGlide(TEXT("avar:rx"), 0.9f);
	DemoWait(0.3f);
	DemoDragTo([this]() {
		// Which way opens it: a degree each way, the chin's drop measured.
		URigExecComponent* R = Rig();
		const int32 Jaw = R->FindControlIndex(TEXT("M_Jaw"));
		const TCHAR* Chin = R->FindControlIndex(TEXT("M_Chin")) != INDEX_NONE ? TEXT("M_Chin") : TEXT("M_Jaw");
		TGuardValue<bool> OnThisThread(R->bEvaluateAsync, false);
		R->Settle();
		double V[9];
		R->GetControlAvars(Jaw, V);
		const double Was = V[3];
		const double Z0 = TestWorld(Chin).Z;
		V[3] = Was + 4.0;
		R->SetControlAvars(Jaw, V);
		R->Evaluate();
		const bool bOpens = TestWorld(Chin).Z < Z0;
		V[3] = Was;
		R->SetControlAvars(Jaw, V);
		R->Evaluate();
		// 0.5 degrees a pixel (720p): 14 degrees.
		return DemoMouse + FVector2D((bOpens ? 28.0 : -28.0) * S, 0.0);
	}, 1.0f);
	DemoWait(0.6f);
	// The mouth corners: up and out with the move manipulator's middle.
	DemoSay(Name + TEXT(": the move manipulator: mouth corners up and out"));
	DemoGlide(TEXT("Move (W)"), 0.9f);
	DemoClick();
	for (const TCHAR* Corner : {TEXT("L_Mouth"), TEXT("R_Mouth")})
	{
		const FString Control = Corner;
		DemoGlide(TEXT("pick:") + Control, 0.8f);
		DemoClick();
		DemoGlideTo([this]() { return DemoHandle(3); }, 0.8f);
		DemoWait(0.3f);
		DemoDragTo([this, Control]() {
			// 1.2 cm up and 0.6 cm out from the middle of the mouth.
			const FVector At = TestWorld(Control);
			const FVector Out = (At - (TestWorld(TEXT("L_Mouth")) + TestWorld(TEXT("R_Mouth"))) * 0.5).GetSafeNormal();
			FVector2D From, To;
			ToScreen(At, From);
			ToScreen(At + FVector(0.0, 0.0, 1.2) + Out * 0.6, To);
			return DemoMouse + (To - From);
		}, 0.9f);
		DemoWait(0.4f);
	}
	// The brows and inner brows: up.
	DemoSay(Name + TEXT(": brows and inner brows up"));
	for (const TCHAR* Brow : {TEXT("L_Brow"), TEXT("R_Brow"), TEXT("L_InBrow"), TEXT("R_InBrow")})
	{
		const FString Control = Brow;
		DemoGlide(TEXT("pick:") + Control, 0.7f);
		DemoClick();
		DemoGlideTo([this]() { return DemoHandle(3); }, 0.7f);
		DemoWait(0.2f);
		DemoDragTo([this, Control]() {
			const FVector At = TestWorld(Control);
			FVector2D From, To;
			ToScreen(At, From);
			ToScreen(At + FVector(0.0, 0.0, Control.Contains(TEXT("In")) ? 1.0 : 0.9), To);
			return DemoMouse + (To - From);
		}, 0.7f);
	}
	DemoWait(0.8f);

	// The head: tilt it with the rotate manipulator's view ring, turn it from the channel box.
	DemoSay(Name + TEXT(": the head: tilt it with the rotate manipulator, turn it from the channel box"));
	DemoDo([this]() { HomeCamera(); CamDistWant *= 0.5f; CamFocusWant += FVector(0.0, 0.0, CamDistWant * 0.4f); });
	DemoGlide(TEXT("Body"), 0.9f);
	DemoClick();
	DemoGlide(TEXT("pick:M_Head"), 0.9f);
	DemoClick();
	DemoGlide(TEXT("Rotate (E)"), 0.9f);
	DemoClick();
	DemoWait(0.6f);
	DemoGlideTo([this]() { return DemoHandle(7); }, 1.0f);
	DemoWait(0.3f);
	DemoDragTo([this]() {
		// 15 degrees round the view ring.
		FGizmo G;
		GetGizmo(G);
		const double Outer = 78.0 * S * 1.08, A = FMath::DegreesToRadians(-15.0);
		return G.Centre + FVector2D(Outer * FMath::Cos(A), Outer * FMath::Sin(A));
	}, 1.0f);
	DemoWait(0.4f);
	DemoGlide(TEXT("avar:ry"), 0.9f);
	DemoWait(0.3f);
	DemoDragTo([this]() { return DemoMouse + FVector2D(50.0 * S, 0.0); }, 1.0f);
	DemoWait(0.8f);

	// The body: the arm and the hips with the manipulators.
	DemoSay(Name + TEXT(": raise the arm: pick it, then drag the view ring"));
	DemoDo([this]() { HomeCamera(); });
	DemoGlide(TEXT("pick:L_UpArm"), 1.0f);
	DemoClick();
	DemoGlideTo([this]() {
		FGizmo G;
		GetGizmo(G);
		const double Outer = 78.0 * S * 1.08;
		const FVector2D Arm = (DemoScreenOf(TEXT("L_LoArm")) - G.Centre).GetSafeNormal();
		return G.Centre + Arm * Outer;
	}, 1.1f);
	DemoWait(0.4f);
	DemoDrag(FVector2D(18.0, -58.0) * 1.5, 1.2f);
	DemoWait(0.4f);
	DemoSay(Name + TEXT(": the move manipulator: lower the hips"));
	DemoGlide(TEXT("Move (W)"), 0.9f);
	DemoClick();
	DemoGlide(TEXT("pick:M_Body"), 0.9f);
	DemoClick();
	DemoGlideTo([this]() { return DemoHandle(DemoAxisNear(FVector::UpVector, false)); }, 1.0f);
	DemoWait(0.4f);
	DemoDrag(FVector2D(0.0, 22.0) * 1.5, 0.9f);
	DemoWait(0.5f);

	// Keys: Auto Key keyed frame 0; frame 20 the other way; play.
	DemoSay(Name + TEXT(": Auto Key keyed frame 0. Frame 20: hips up, arm down. Then play"));
	DemoGlideTo([this]() {
		const FBox2D* Ruler = DemoTargets.Find(TEXT("ruler"));
		return Ruler ? FVector2D(Ruler->Min.X + (Ruler->Max.X - Ruler->Min.X) * 20.0 / FMath::Max(Edit.Length, 1), Ruler->GetCenter().Y)
		             : DemoMouse;
	}, 1.0f);
	DemoClick();
	DemoGlideTo([this]() { return DemoHandle(DemoAxisNear(FVector::UpVector, false)); }, 1.0f);
	DemoWait(0.3f);
	DemoDrag(FVector2D(0.0, -30.0) * 1.5, 0.9f);
	DemoGlide(TEXT("Rotate (E)"), 0.9f);
	DemoClick();
	DemoGlide(TEXT("pick:L_UpArm"), 0.9f);
	DemoClick();
	DemoGlideTo([this]() {
		FGizmo G;
		GetGizmo(G);
		const FVector2D Arm = (DemoScreenOf(TEXT("L_LoArm")) - G.Centre).GetSafeNormal();
		return G.Centre + Arm * (78.0 * S * 1.08);
	}, 1.0f);
	DemoDrag(FVector2D(-10.0, 70.0) * 1.5, 1.1f);
	DemoGlide(TEXT("Play"), 1.0f);
	DemoClick();
	DemoWait(3.5f);
	DemoGlide(TEXT("Pause"), 0.5f);
	DemoClick();

	// IK/FK and spaces, matched.
	DemoSay(Name + TEXT(": switch the arm to IK, matched: nothing moves"));
	DemoGlide(TEXT("Switch to IK (matched)"), 1.1f);
	Hold();
	DemoClick();
	DemoWait(1.4f);
	DemoSay(Name + TEXT(": the IK hand into world space, matched: it stays put"));
	DemoGlide(TEXT("pick:L_ArmIK"), 1.0f);
	DemoClick();
	DemoGlide(TEXT("space"), 1.0f);
	DemoClick();
	DemoWait(0.5f);
	DemoGlide(TEXT("spaceitem:world"), 0.8f);
	DemoClick();
	DemoWait(1.4f);

	// Save the pose.
	DemoSay(Name + TEXT(": save the pose to the library"));
	DemoGlide(TEXT("+ Save Pose"), 1.0f);
	DemoClick();
	DemoWait(0.5f);
	for (int32 K = 0; K < 12; ++K)
	{
		DemoKey(EKeys::BackSpace, false, 0.04f);
	}
	DemoType(TEXT("Wave"));
	DemoWait(0.5f);
	DemoKey(EKeys::Enter);
	DemoWait(2.0f);

	if (!bLast)
	{
		DemoSay(TEXT("Switch character"));
		DemoGlide(TEXT("Character"), 1.2f);
		Hold();
		DemoClick();
	}
}
