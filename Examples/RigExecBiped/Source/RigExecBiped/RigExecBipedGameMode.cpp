#include "RigExecBipedGameMode.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "GameFramework/PlayerController.h"
#include "RigExecBipedCharacter.h"

ARigExecBipedGameMode::ARigExecBipedGameMode()
{
	DefaultPawnClass = ARigExecBipedCharacter::StaticClass();
	HUDClass = ARigExecBipedHUD::StaticClass();
}

void
ARigExecBipedHUD::DrawGame()
{
	ARigExecBipedCharacter* Hero = this->Hero();
	if (!Canvas || !Hero)
	{
		return;
	}
	const float W = Canvas->ClipX, H = Canvas->ClipY, S = H / 720.0f;
	const float Dt = GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0f;

	// The close-up's letterbox, sliding in.
	Bars = FMath::FInterpTo(Bars, Hero->IsInCinematic() ? 1.0f : 0.0f, Dt, 7.0f);
	if (Bars > 0.002f)
	{
		const float Bar = H * 0.1f * Bars;
		DrawRect(FLinearColor::Black, 0.0f, 0.0f, W, Bar);
		DrawRect(FLinearColor::Black, 0.0f, H - Bar, W, Bar);
		if (Hero->IsInCinematic())
		{
			Text(TEXT("ESC  back to play"), W * 0.5f, H - Bar * 0.5f, 1.0f * S, FLinearColor(0.85f, 0.85f, 0.85f, Bars), true);
		}
	}

	// The subdivision level, always: the readout, and - / + to change it.
	const int32 Level = Hero->GetSubdivisionLevel();
	if (!ShowSubdivisionPanel())
	{
		return;
	}
	if (Level < 0)
	{
		Text(TEXT("SUBD  n/a (no Squarebit Subdivs)"), W - 330.0f * S, H * 0.12f, 1.0f * S, FLinearColor(1, 0.6f, 0.4f));
		return;
	}
	const float PanelW = 300.0f * S, PanelH = 96.0f * S;
	const float X = W - PanelW - 24.0f * S, Y = H * 0.12f;
	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.5f), X, Y, PanelW, PanelH);
	Text(TEXT("SUBD LEVEL"), X + 16.0f * S, Y + 10.0f * S, 1.3f * S, FLinearColor(0.75f, 0.9f, 1.0f));
	Text(FString::Printf(TEXT("%d"), Level), X + 16.0f * S, Y + 40.0f * S, 2.6f * S,
	     Level > 0 ? FLinearColor(1.0f, 0.85f, 0.25f) : FLinearColor::White);
	// A pip per level.
	for (int32 Pip = 0; Pip < ARigExecBipedCharacter::MaxSubdivisionLevel; ++Pip)
	{
		DrawRect(Pip < Level ? FLinearColor(1.0f, 0.85f, 0.25f) : FLinearColor(0.35f, 0.35f, 0.35f), X + 76.0f * S + Pip * 22.0f * S,
		         Y + 62.0f * S, 16.0f * S, 10.0f * S);
	}
	const float B = 44.0f * S;
	Button(TEXT("SubdDown"), TEXT("-"), X + PanelW - 2.0f * B - 26.0f * S, Y + 36.0f * S, B, B, S, Level > 0);
	Button(TEXT("SubdUp"), TEXT("+"), X + PanelW - B - 14.0f * S, Y + 36.0f * S, B, B, S,
	       Level < ARigExecBipedCharacter::MaxSubdivisionLevel);
	Text(TEXT("keys  -  =  or  0-2"), X + 16.0f * S, Y + PanelH + 6.0f * S, 0.9f * S, FLinearColor(0.85f, 0.85f, 0.85f));
}

void
ARigExecBipedHUD::NotifyHitBoxClick(FName BoxName)
{
	Super::NotifyHitBoxClick(BoxName);
	if (ARigExecBipedCharacter* Hero = this->Hero())
	{
		if (BoxName == TEXT("SubdUp"))
		{
			Hero->SubdivisionUp();
		}
		else if (BoxName == TEXT("SubdDown"))
		{
			Hero->SubdivisionDown();
		}
	}
}
