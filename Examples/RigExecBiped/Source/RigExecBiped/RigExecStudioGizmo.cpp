// The studio's manipulator on the lead control, drawn on the game's canvas:
// Move (an arrow per axis and a square in the middle for the view's
// plane), Rotate (a ring per axis and an outer ring about the view) and
// Scale (a handle per axis and a box in the middle for all three). Only the
// channels the control exposes get a handle. A drag writes through the same
// path as TouchPose's (ApplyWorldEdit), so it keys and undoes alike.
#include "Camera/PlayerCameraManager.h"
#include "RigExecVR.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "GameFramework/PlayerController.h"
#include "RigExecComponent.h"
#include "RigExecStudioGameMode.h"
#include "TextureResource.h"

namespace
{
/** A handle's size on screen, in 720p pixels. */
constexpr float GGizmoPixels = 78.0f;
/** How near the cursor must be to take a handle, in 720p pixels. */
constexpr float GGizmoReach = 9.0f;
const FLinearColor GAxisColor[3] = {FLinearColor(0.95f, 0.22f, 0.18f), FLinearColor(0.35f, 0.9f, 0.25f), FLinearColor(0.25f, 0.5f, 1.0f)};
const FLinearColor GGizmoHot(1.0f, 0.85f, 0.2f);

float SegmentDistance(const FVector2D& P, const FVector2D& A, const FVector2D& B)
{
	const FVector2D AB = B - A;
	const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(AB.SizeSquared(), 1e-6), 0.0, 1.0);
	return float(FVector2D::Distance(P, A + AB * T));
}

/** A ring about `Axis` through `Pivot`, as world points. */
TArray<FVector> Ring(const FVector& Pivot, const FVector& Axis, double Radius, int32 Count = 48)
{
	FVector U, V;
	Axis.FindBestAxisVectors(U, V);
	TArray<FVector> Out;
	for (int32 K = 0; K <= Count; ++K)
	{
		const double A = 2.0 * PI * K / Count;
		Out.Add(Pivot + (U * FMath::Cos(A) + V * FMath::Sin(A)) * Radius);
	}
	return Out;
}
} // namespace

ARigExecStudioHUD::EPoseMode ARigExecStudioHUD::GizmoMode() const
{
	const URigExecComponent* R = Rig();
	if (!R || Selected.Num() == 0)
	{
		return EPoseMode::Rotate;
	}
	const uint16 Mask = R->GetControlAvarMask(Selected.Last());
	const bool bMoves = (Mask & 0x07) != 0, bRotates = (Mask & 0x38) != 0, bScales = (Mask & 0x1C0) != 0;
	switch (PoseMode)
	{
	case EPoseMode::Move:
		return bMoves ? EPoseMode::Move : bRotates ? EPoseMode::Rotate : EPoseMode::Scale;
	case EPoseMode::Rotate:
		return bRotates ? EPoseMode::Rotate : bMoves ? EPoseMode::Move : EPoseMode::Scale;
	case EPoseMode::Scale:
		return bScales ? EPoseMode::Scale : bRotates ? EPoseMode::Rotate : EPoseMode::Move;
	default:
		return bRotates ? EPoseMode::Rotate : bMoves ? EPoseMode::Move : EPoseMode::Scale;
	}
}

bool ARigExecStudioHUD::GetGizmo(FGizmo& Out) const
{
	const URigExecComponent* R = Rig();
	if (!R || !PlayerOwner || Selected.Num() == 0)
	{
		return false;
	}
	const int32 Lead = Selected.Last();
	FMatrix Posed;
	if (!R->GetControlAssetFrame(Lead, Posed))
	{
		return false;
	}
	const FTransform Component = R->GetComponentTransform();
	FMatrix L = R->AssetToComponent(FMatrix::Identity);
	L.M[3][0] = L.M[3][1] = L.M[3][2] = 0.0;
	auto ToWorld = [&](const FVector& Asset) { return Component.TransformVectorNoScale(L.TransformVector(Asset)).GetSafeNormal(); };
	// The control's own axes are its frame's rows; its translate channels
	// run along the rows of what sits between its value and its parent.
	const FMatrix Carried = R->GetLandedControlValueMatrix(Lead).Inverse() * Posed;
	for (int32 A = 0; A < 3; ++A)
	{
		Out.OwnAxes[A] = ToWorld(FVector(Posed.M[A][0], Posed.M[A][1], Posed.M[A][2]));
		Out.MoveAxes[A] = ToWorld(FVector(Carried.M[A][0], Carried.M[A][1], Carried.M[A][2]));
	}
	Out.Pivot = Component.TransformPosition(R->GetRigFrame(R->GetControlPath(Lead)).GetLocation());
	if (!ToScreen(Out.Pivot, Out.Centre))
	{
		return false;
	}
	// A fixed size on screen, whatever the distance.
	Out.Length = PixelSizeAt(Out.Pivot) * GGizmoPixels * S;
	Out.Mask = R->GetControlAvarMask(Lead);
	Out.Mode = GizmoMode();
	return true;
}

int32 ARigExecStudioHUD::GizmoHandleAt(const FVector2D& At) const
{
	FGizmo G;
	// Auto is TouchPose alone: the skin takes every press.
	if (bPlaying || PoseMode == EPoseMode::Auto || !GetGizmo(G))
	{
		return -1;
	}
	const float Reach = GGizmoReach * S;
	auto Screen = [this](const FVector& World, FVector2D& Out) { return ToScreen(World, Out); };
	int32 Best = -1;
	float Nearest = Reach;
	auto Consider = [&](int32 Handle, float Distance) {
		if (Distance < Nearest)
		{
			Nearest = Distance;
			Best = Handle;
		}
	};
	if (G.Mode == EPoseMode::Move || G.Mode == EPoseMode::Scale)
	{
		const int32 First = G.Mode == EPoseMode::Move ? 0 : 8;
		const FVector* Axes = G.Mode == EPoseMode::Move ? G.MoveAxes : G.OwnAxes;
		for (int32 A = 0; A < 3; ++A)
		{
			FVector2D Tip;
			if ((G.Mask & (1 << (A + (G.Mode == EPoseMode::Move ? 0 : 6)))) && Screen(G.Pivot + Axes[A] * G.Length, Tip))
			{
				Consider(First + A, SegmentDistance(At, G.Centre + (Tip - G.Centre) * 0.2, Tip));
			}
		}
		// The middle: the view's plane, or all three axes.
		if (FMath::Abs(At.X - G.Centre.X) < 8.0f * S && FMath::Abs(At.Y - G.Centre.Y) < 8.0f * S)
		{
			Consider(G.Mode == EPoseMode::Move ? 3 : 11, 0.0f);
		}
		return Best;
	}
	for (int32 A = 0; A < 3; ++A)
	{
		if (!(G.Mask & (1 << (3 + A))))
		{
			continue;
		}
		const TArray<FVector> Points = Ring(G.Pivot, G.OwnAxes[A], G.Length * 0.85);
		FVector2D Prev;
		bool bPrev = false;
		for (const FVector& P : Points)
		{
			FVector2D Now;
			const bool bNow = Screen(P, Now);
			if (bNow && bPrev)
			{
				Consider(4 + A, SegmentDistance(At, Prev, Now));
			}
			Prev = Now;
			bPrev = bNow;
		}
	}
	// The outer ring turns about the view.
	const float Outer = float(GGizmoPixels * S * 1.08f);
	Consider(7, FMath::Abs(float(FVector2D::Distance(At, G.Centre)) - Outer));
	return Best;
}

void ARigExecStudioHUD::DrawGizmo()
{
	FGizmo G;
	if (!GetGizmo(G))
	{
		return;
	}
	auto Screen = [this](const FVector& World, FVector2D& Out) { return ToScreen(World, Out); };
	auto Color = [this](int32 Handle, const FLinearColor& Base) { return GizmoHover == Handle ? GGizmoHot : Base; };
	auto Square = [this](const FVector2D& C, float Half, const FLinearColor& Fill) {
		DrawRect(FLinearColor(0, 0, 0, 0.8f), C.X - Half - 1.5f * S, C.Y - Half - 1.5f * S, 2.0f * Half + 3.0f * S, 2.0f * Half + 3.0f * S);
		DrawRect(Fill, C.X - Half, C.Y - Half, 2.0f * Half, 2.0f * Half);
	};
	const float Thick = 2.6f * S;
	if (G.Mode == EPoseMode::Move || G.Mode == EPoseMode::Scale)
	{
		const bool bMove = G.Mode == EPoseMode::Move;
		const FVector* Axes = bMove ? G.MoveAxes : G.OwnAxes;
		for (int32 A = 0; A < 3; ++A)
		{
			FVector2D Tip;
			if (!(G.Mask & (1 << (A + (bMove ? 0 : 6)))) || !Screen(G.Pivot + Axes[A] * G.Length, Tip))
			{
				continue;
			}
			const FLinearColor C = Color((bMove ? 0 : 8) + A, GAxisColor[A]);
			DrawLine(G.Centre.X, G.Centre.Y, Tip.X, Tip.Y, FLinearColor(0, 0, 0, 0.6f), Thick + 2.0f * S);
			DrawLine(G.Centre.X, G.Centre.Y, Tip.X, Tip.Y, C, Thick);
			if (bMove)
			{
				// An arrowhead.
				const FVector2D Dir = (Tip - G.Centre).GetSafeNormal(), Side(-Dir.Y, Dir.X);
				TArray<FCanvasUVTri> Tris;
				FCanvasUVTri& T = Tris.AddDefaulted_GetRef();
				T.V0_Pos = Tip + Dir * (12.0f * S);
				T.V1_Pos = Tip + Side * (6.0f * S);
				T.V2_Pos = Tip - Side * (6.0f * S);
				T.V0_Color = T.V1_Color = T.V2_Color = C;
				FCanvasTriangleItem Item(Tris, GWhiteTexture);
				Item.BlendMode = ARigExecVRRig::UiBlend();
				Canvas->DrawItem(Item);
			}
			else
			{
				Square(Tip, 5.0f * S, C);
			}
		}
		Square(G.Centre, 6.0f * S, Color(bMove ? 3 : 11, FLinearColor(0.95f, 0.95f, 0.95f, 0.9f)));
		return;
	}
	// Rotate: a ring per axis it turns about, and the view's ring.
	for (int32 A = 0; A < 3; ++A)
	{
		if (!(G.Mask & (1 << (3 + A))))
		{
			continue;
		}
		const FLinearColor C = Color(4 + A, GAxisColor[A]);
		const TArray<FVector> Points = Ring(G.Pivot, G.OwnAxes[A], G.Length * 0.85);
		FVector2D Prev;
		bool bPrev = false;
		for (const FVector& P : Points)
		{
			FVector2D Now;
			const bool bNow = Screen(P, Now);
			if (bNow && bPrev)
			{
				DrawLine(Prev.X, Prev.Y, Now.X, Now.Y, C, GizmoHover == 4 + A ? Thick * 1.5f : Thick);
			}
			Prev = Now;
			bPrev = bNow;
		}
	}
	const float Outer = float(GGizmoPixels * S * 1.08f);
	const FLinearColor View = Color(7, FLinearColor(0.85f, 0.85f, 0.85f, 0.75f));
	for (int32 K = 0; K < 64; ++K)
	{
		const float A0 = K * 2.0f * PI / 64.0f, A1 = (K + 1) * 2.0f * PI / 64.0f;
		DrawLine(G.Centre.X + FMath::Cos(A0) * Outer, G.Centre.Y + FMath::Sin(A0) * Outer, G.Centre.X + FMath::Cos(A1) * Outer,
		         G.Centre.Y + FMath::Sin(A1) * Outer, View, 1.8f * S);
	}
	Square(G.Centre, 3.5f * S, FLinearColor(0.95f, 0.85f, 0.25f, 1.0f));
}

void ARigExecStudioHUD::UpdateGizmoDrag()
{
	FGizmo G;
	if (!PlayerOwner || !GetGizmo(G))
	{
		return;
	}
	// The gizmo as the drag began: the controls move, the handles follow.
	const FVector2D Total = Mouse - Drag.Start;
	const double Slow = bShift ? 0.25 : 1.0;
	const int32 H = Drag.Index;
	FVector Eye, Forward, ViewRight, ViewUp;
	GetView(Eye, Forward, ViewRight, ViewUp);
	const FVector Pivot = Drag.Targets.Last().Pivot;
	FVector2D Centre;
	ToScreen(Pivot, Centre);
	const double PerPixel = G.Length / (GGizmoPixels * S);
	// How far along an axis the cursor went: its travel along the axis's
	// image on screen, in world units.
	auto Along = [&](const FVector& Axis) {
		FVector2D Tip;
		if (!ToScreen(Pivot + Axis * G.Length, Tip))
		{
			return 0.0;
		}
		const FVector2D Image = Tip - Centre;
		const double Pixels = Image.Size();
		if (Pixels < 4.0)
		{
			return 0.0;
		}
		return FVector2D::DotProduct(Total, Image / Pixels) / Pixels * G.Length;
	};
	if (H >= 0 && H <= 2)
	{
		ApplyWorldEdit(FQuat::Identity, G.MoveAxes[H] * Along(G.MoveAxes[H]) * Slow, 1.0, -1);
	}
	else if (H == 3)
	{
		ApplyWorldEdit(FQuat::Identity, (ViewRight * Total.X - ViewUp * Total.Y) * PerPixel * Slow, 1.0, -1);
	}
	else if (H >= 4 && H <= 7)
	{
		// The angle the cursor swept about the pivot on screen; a ring seen
		// from behind turns the other way.
		const FVector Axis = H == 7 ? -Forward : G.OwnAxes[H - 4];
		const FVector2D From = Drag.Start - Centre, To = Mouse - Centre;
		double Swept = FMath::Atan2(To.Y, To.X) - FMath::Atan2(From.Y, From.X);
		Swept = FMath::UnwindRadians(Swept);
		if (From.Size() < 6.0 || To.Size() < 6.0)
		{
			Swept = 0.0;
		}
		const double Facing = FVector::DotProduct(Axis, Forward) > 0.0 ? 1.0 : -1.0;
		ApplyWorldEdit(FQuat(Axis, -Swept * Facing * Slow), FVector::ZeroVector, 1.0, -1);
	}
	else if (H >= 8 && H <= 11)
	{
		const double Amount = H == 11 ? Total.X * PerPixel : Along(G.OwnAxes[H - 8]);
		ApplyWorldEdit(FQuat::Identity, FVector::ZeroVector, FMath::Max(1.0 + Amount / G.Length * Slow, 0.05), H == 11 ? -1 : H - 8);
	}
}
