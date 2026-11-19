#include "RigExecMenuHUD.h"

#include "Camera/PlayerCameraManager.h"
#include "CanvasItem.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/World.h"
#include "IXRTrackingSystem.h"
#include "RigExecVR.h"
#include "StereoRendering.h"
#include "DynamicRHI.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "GameFramework/GameModeBase.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "RenderTimer.h"
#include "RigExecBipedCharacter.h"
#include "RigExecCharacterSelect.h"
#include "RigExecComponent.h"
#include "RigExecRetargetComponent.h"
#include "TextureResource.h"
#include "UnrealClient.h"

namespace
{
const TCHAR* GSection = TEXT("RigExecBiped.Settings");
const FLinearColor GAccent(0.15f, 0.55f, 0.85f, 0.9f);
const FLinearColor GCyan(0.45f, 0.85f, 1.0f);
/** The renderer's settings for a headset, or back as they were (below). */
void UseVRSettings(bool bVR);

FString OnOff(bool b)
{
	return b ? TEXT("ON") : TEXT("OFF");
}

void SetCVar(const TCHAR* Name, int32 Value)
{
	if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
	{
		Variable->Set(Value, ECVF_SetByConsole);
	}
}

/** The next value in a list, wrapping. */
template <typename T>
T Next(const TArray<T>& Values, T Now)
{
	const int32 At = Values.IndexOfByKey(Now);
	return Values[(At + 1) % Values.Num()];
}
} // namespace

ARigExecMenuHUD::ARigExecMenuHUD()
{
	PrimaryActorTick.bCanEverTick = true;
	// The tour carries on behind a paused menu.
	PrimaryActorTick.bTickEvenWhenPaused = true;
}

void
ARigExecMenuHUD::BeginPlay()
{
	Super::BeginPlay();
	if (GConfig)
	{
		GConfig->GetBool(GSection, TEXT("ShowFps"), bShowFps, GGameUserSettingsIni);
		GConfig->GetBool(GSection, TEXT("ShowStats"), bShowStats, GGameUserSettingsIni);
		GConfig->GetBool(GSection, TEXT("ShowSubdPanel"), bShowSubdPanel, GGameUserSettingsIni);
		GConfig->GetBool(GSection, TEXT("VSync"), bVSync, GGameUserSettingsIni);
		GConfig->GetInt(GSection, TEXT("FrameCap"), FrameCap, GGameUserSettingsIni);
		GConfig->GetInt(GSection, TEXT("ScreenPercent"), ScreenPercent, GGameUserSettingsIni);
		GConfig->GetBool(GSection, TEXT("RigAsync"), ARigExecBipedCharacter::bRigAsync, GGameUserSettingsIni);
		GConfig->GetInt(GSection, TEXT("RigRateFar"), ARigExecBipedCharacter::RigRateFar, GGameUserSettingsIni);
		GConfig->GetInt(GSection, TEXT("RigRateLobby"), ARigExecBipedCharacter::RigRateLobby, GGameUserSettingsIni);
	}
	Apply();
	// More than one character baked: choose who to be before playing.
	// Automated runs (demo, portrait, a character on the command line) skip it.
	const TCHAR* Cmd = FCommandLine::Get();
	FString Named;
	// -RigExecShowChooser keeps it in a demo run (for recording it).
	const bool bAutomated = FParse::Param(Cmd, TEXT("RigExecDemo")) || FParse::Param(Cmd, TEXT("RigExecPadDemo")) ||
	                        FParse::Param(Cmd, TEXT("RigExecPortrait")) || FParse::Param(Cmd, TEXT("RigExecStudioTest")) ||
	                        FParse::Value(Cmd, TEXT("RigExecCharacter="), Named);
	if (!bChoseThisRun && ARigExecBipedCharacter::FindCharacters().Num() > 1 &&
	    (!bAutomated || FParse::Param(Cmd, TEXT("RigExecShowChooser"))))
	{
		OpenChooser();
	}
	bDemo = FParse::Param(Cmd, TEXT("RigExecStudioDemo"));
	if (bDemo)
	{
		BuildDemo();
	}
	// In VR the HUD is painted onto its panel once the actors have ticked.
	PostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &ARigExecMenuHUD::PaintVR);
}

void
ARigExecMenuHUD::EndPlay(const EEndPlayReason::Type Reason)
{
	FWorldDelegates::OnWorldPostActorTick.Remove(PostTickHandle);
	if (VR)
	{
		VR->Destroy();
		VR = nullptr;
		// The next level's HUD turns them on again if it is still in VR.
		UseVRSettings(false);
	}
	Super::EndPlay(Reason);
}

void
ARigExecMenuHUD::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateVR(DeltaSeconds);
	if (bDemo)
	{
		DemoTick(FApp::GetDeltaTime());
	}
}

// --- The tour ---------------------------------------------------------------

void
ARigExecMenuHUD::BuildDemo()
{
	// The game level's part: choose, play a moment, then the studio.
	DemoUntil([this]() { return IsChoosing(); }, 30.0f);
	DemoSay(TEXT("The RigExec example game: choose a character"));
	DemoWait(2.0f);
	DemoGlideTo([this]() { FVector2D At; DemoPlate(0, At); return At; }, 1.0f);
	DemoWait(0.5f);
	DemoClick();
	DemoDo([this]() { DemoChooseCharacter(0); });
	DemoUntil([this]() { return !IsMenuOpen(); }, 10.0f);
	DemoSay(TEXT("Playing: the Mannequin's animation drives the rig through the RigExec retargeter"));
	DemoWait(4.0f);
	DemoSay(TEXT("Switching mode: Pause menu > Animation Studio"));
	DemoDo([this]() { OpenMenu(); });
	DemoWait(1.0f);
	DemoGlide(TEXT("menu:ANIMATION STUDIO"), 1.0f);
	DemoWait(0.4f);
	DemoClick();
	DemoDo([this]() { DemoMenuPick(TEXT("ANIMATION STUDIO")); });
}

void
ARigExecMenuHUD::DemoTick(float DeltaSeconds)
{
	bDemoPress = bDemoRelease = false;
	DemoKeys.Reset();
	// Steps that take no time all run now.
	for (int32 Guard = 0; Guard < 64 && DemoSteps.IsValidIndex(DemoAt); ++Guard)
	{
		FDemoStep& Step = DemoSteps[DemoAt];
		if (!bDemoStarted)
		{
			bDemoStarted = true;
			DemoTime = 0.0f;
			DemoFrom = DemoMouse;
			if (Step.Start)
			{
				Step.Start();
			}
		}
		else
		{
			DemoTime += DeltaSeconds;
		}
		const float T = Step.Seconds > 0.0f ? FMath::Clamp(DemoTime / Step.Seconds, 0.0f, 1.0f) : 1.0f;
		if (Step.Update)
		{
			Step.Update(T);
		}
		const bool bDone = Step.Until ? (Step.Until() || DemoTime >= Step.Seconds) : DemoTime >= Step.Seconds;
		if (!bDone)
		{
			return;
		}
		if (Step.End)
		{
			Step.End();
		}
		++DemoAt;
		bDemoStarted = false;
		if (bDemoPress || bDemoRelease || DemoKeys.Num() > 0)
		{
			// One input a frame, so each is seen.
			return;
		}
	}
}

FVector2D
ARigExecMenuHUD::DemoTarget(const FString& Name) const
{
	const FBox2D* Box = DemoTargets.Find(Name);
	return Box ? Box->GetCenter() : DemoMouse;
}

void
ARigExecMenuHUD::DemoGlideTo(TFunction<FVector2D()> Target, float Seconds)
{
	FDemoStep Step;
	Step.Seconds = Seconds;
	Step.Update = [this, Target](float T) {
		const float Ease = FMath::InterpEaseInOut(0.0f, 1.0f, T, 2.5f);
		DemoMouse = FMath::Lerp(DemoFrom, Target(), double(Ease));
	};
	DemoSteps.Add(MoveTemp(Step));
}

void
ARigExecMenuHUD::DemoClick()
{
	FDemoStep Step;
	Step.Seconds = 0.12f;
	Step.Start = [this]() {
		bDemoPress = bDemoDown = true;
		DemoClickAt = FPlatformTime::Seconds();
	};
	Step.End = [this]() {
		bDemoDown = false;
		bDemoRelease = true;
	};
	DemoSteps.Add(MoveTemp(Step));
	DemoWait(0.05f);
}

void
ARigExecMenuHUD::DemoDrag(const FVector2D& Delta, float Seconds)
{
	FDemoStep Press;
	Press.Seconds = 0.1f;
	Press.Start = [this]() {
		bDemoPress = bDemoDown = true;
		DemoClickAt = FPlatformTime::Seconds();
	};
	DemoSteps.Add(MoveTemp(Press));
	FDemoStep Move;
	Move.Seconds = Seconds;
	Move.Update = [this, Delta](float T) { DemoMouse = DemoFrom + Delta * double(FMath::InterpEaseInOut(0.0f, 1.0f, T, 2.0f)); };
	Move.End = [this]() {
		bDemoDown = false;
		bDemoRelease = true;
	};
	DemoSteps.Add(MoveTemp(Move));
	DemoWait(0.05f);
}

void
ARigExecMenuHUD::DemoDragTo(TFunction<FVector2D()> To, float Seconds)
{
	TSharedRef<FVector2D> Target = MakeShared<FVector2D>(FVector2D::ZeroVector);
	FDemoStep Press;
	Press.Seconds = 0.1f;
	Press.Start = [this, To, Target]() {
		*Target = To();
		bDemoPress = bDemoDown = true;
		DemoClickAt = FPlatformTime::Seconds();
	};
	DemoSteps.Add(MoveTemp(Press));
	FDemoStep Move;
	Move.Seconds = Seconds;
	Move.Update = [this, Target](float T) { DemoMouse = FMath::Lerp(DemoFrom, *Target, double(FMath::InterpEaseInOut(0.0f, 1.0f, T, 2.0f))); };
	Move.End = [this]() {
		bDemoDown = false;
		bDemoRelease = true;
	};
	DemoSteps.Add(MoveTemp(Move));
	DemoWait(0.05f);
}

void
ARigExecMenuHUD::DemoWait(float Seconds)
{
	FDemoStep Step;
	Step.Seconds = Seconds;
	DemoSteps.Add(MoveTemp(Step));
}

void
ARigExecMenuHUD::DemoUntil(TFunction<bool()> Done, float Timeout)
{
	FDemoStep Step;
	Step.Seconds = Timeout;
	Step.Until = MoveTemp(Done);
	DemoSteps.Add(MoveTemp(Step));
}

void
ARigExecMenuHUD::DemoDo(TFunction<void()> Action)
{
	FDemoStep Step;
	Step.Start = MoveTemp(Action);
	DemoSteps.Add(MoveTemp(Step));
}

void
ARigExecMenuHUD::DemoSay(const FString& Caption)
{
	DemoDo([this, Caption]() { DemoCaption = Caption; });
}

void
ARigExecMenuHUD::DemoKey(const FKey& Key, bool bShifted, float Seconds)
{
	FDemoStep Step;
	Step.Seconds = Seconds;
	Step.Start = [this, Key, bShifted]() {
		DemoKeys.Add(Key);
		bDemoShift = bShifted;
	};
	Step.End = [this]() { bDemoShift = false; };
	DemoSteps.Add(MoveTemp(Step));
}

void
ARigExecMenuHUD::DemoType(const FString& Text)
{
	static const TCHAR* Digits[10] = {TEXT("Zero"), TEXT("One"), TEXT("Two"), TEXT("Three"), TEXT("Four"),
	                                  TEXT("Five"), TEXT("Six"), TEXT("Seven"), TEXT("Eight"), TEXT("Nine")};
	for (const TCHAR C : Text)
	{
		if (FChar::IsAlpha(C))
		{
			DemoKey(FKey(FName(*FString::Chr(FChar::ToUpper(C)))), FChar::IsUpper(C));
		}
		else if (FChar::IsDigit(C))
		{
			DemoKey(FKey(FName(Digits[C - TEXT('0')])));
		}
		else if (C == TEXT(' '))
		{
			DemoKey(EKeys::SpaceBar);
		}
		DemoWait(0.06f);
	}
}

bool
ARigExecMenuHUD::DemoPlate(int32 Index, FVector2D& Out) const
{
	FVector Feet;
	float Height = 0.0f;
	if (!Lobby || !PlayerOwner || !Canvas || !Lobby->GetPreviewBounds(Index, Feet, Height))
	{
		return false;
	}
	FVector2D Low;
	if (!ToScreen(Feet, Low))
	{
		return false;
	}
	const float S = Canvas->ClipY / 720.0f;
	Out = Low + FVector2D(0.0, 18.0f * S + 20.0f * S);
	return true;
}

void
ARigExecMenuHUD::DemoMenuPick(const FString& Label)
{
	const TArray<FItem> List = Items();
	const int32 At = List.IndexOfByPredicate([&Label](const FItem& Item) { return Item.Label == Label; });
	if (At != INDEX_NONE)
	{
		Focus = At;
		MenuSelect();
	}
}

void
ARigExecMenuHUD::DemoChooseCharacter(int32 Index)
{
	if (!IsChoosing())
	{
		return;
	}
	Focus = Index;
	Lobby->SetFocus(Index);
	MenuSelect();
}

void
ARigExecMenuHUD::DrawDemo()
{
	if (!Canvas)
	{
		return;
	}
	const float W = Canvas->ClipX, H = Canvas->ClipY, S = H / 720.0f;
	// The caption: what is happening, low in the middle.
	if (!DemoCaption.IsEmpty())
	{
		UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
		if (Font)
		{
			float TW = 0.0f, TH = 0.0f;
			Canvas->TextSize(Font, DemoCaption, TW, TH);
			const float Scale = 17.0f * S / FMath::Max(float(Font->GetMaxCharHeight()), 1.0f);
			TW *= Scale;
			TH *= Scale;
			const float BoxW = TW + 44.0f * S, BoxH = TH + 18.0f * S;
			const float X = (W - BoxW) * 0.5f, Y = H - 280.0f * S;
			DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f), X, Y, BoxW, BoxH);
			DrawRect(FLinearColor(1.0f, 0.78f, 0.2f, 1.0f), X, Y, 4.0f * S, BoxH);
			FCanvasTextItem Item(FVector2D(X + 24.0f * S, Y + 9.0f * S), FText::FromString(DemoCaption), Font, FLinearColor::White);
			Item.BlendMode = ARigExecVRRig::UiTextBlend();
			Item.Scale = FVector2D(Scale, Scale);
			Canvas->DrawItem(Item);
		}
	}
	// The cursor: an arrow, and a ring when it clicks.
	if (!PlayerOwner || !PlayerOwner->bShowMouseCursor)
	{
		return;
	}
	const FVector2D P = DemoMouse;
	const float C = 1.25f * S;
	auto Arrow = [&](float Grow, const FLinearColor& Color) {
		const FVector2D Points[7] = {{0, 0}, {0, 17}, {4.5, 13}, {8, 21}, {11, 19.5}, {7.5, 12}, {13, 12}};
		TArray<FCanvasUVTri> Tris;
		const FVector2D Centre(5.0, 11.0);
		auto At = [&](int32 I) { return P + (Points[I] + (Points[I] - Centre).GetSafeNormal() * Grow) * C; };
		const int32 Fan[5][3] = {{0, 1, 2}, {0, 2, 5}, {0, 5, 6}, {2, 3, 4}, {2, 4, 5}};
		for (const auto& F : Fan)
		{
			FCanvasUVTri& T = Tris.AddDefaulted_GetRef();
			T.V0_Pos = At(F[0]);
			T.V1_Pos = At(F[1]);
			T.V2_Pos = At(F[2]);
			T.V0_Color = T.V1_Color = T.V2_Color = Color;
		}
		FCanvasTriangleItem Item(Tris, GWhiteTexture);
		Item.BlendMode = ARigExecVRRig::UiBlend();
		Canvas->DrawItem(Item);
	};
	const double Since = FPlatformTime::Seconds() - DemoClickAt;
	if (Since < 0.45)
	{
		const float R = float(8.0 + Since * 60.0) * S;
		const FLinearColor Ring(1.0f, 0.85f, 0.3f, float(1.0 - Since / 0.45));
		for (int32 K = 0; K < 24; ++K)
		{
			const float A0 = K * 2.0f * PI / 24.0f, A1 = (K + 1) * 2.0f * PI / 24.0f;
			DrawLine(P.X + FMath::Cos(A0) * R, P.Y + FMath::Sin(A0) * R, P.X + FMath::Cos(A1) * R, P.Y + FMath::Sin(A1) * R, Ring, 2.5f * S);
		}
	}
	Arrow(1.6f, FLinearColor(0.0f, 0.0f, 0.0f, 0.9f));
	Arrow(0.0f, bDemoDown ? FLinearColor(1.0f, 0.85f, 0.3f, 1.0f) : FLinearColor::White);
}

bool ARigExecMenuHUD::bChoseThisRun = false;

void
ARigExecMenuHUD::OpenChooser()
{
	// Not paused: the characters idle while you choose.
	OpenMenu(false);
	Page = EPage::Choose;
	Focus = 0;
	const TArray<ARigExecBipedCharacter::FChoice> Choices = ARigExecBipedCharacter::FindCharacters();
	const FSoftObjectPath Current = Hero() ? Hero()->GetCharacterAsset() : ARigExecBipedCharacter::ChosenCharacter();
	TArray<FSoftObjectPath> Assets;
	int32 Mine = INDEX_NONE;
	for (int32 Each = 0; Each < Choices.Num(); ++Each)
	{
		Assets.Add(Choices[Each].Asset);
		if (Choices[Each].Asset == Current)
		{
			Focus = Each;
			Mine = Each;
		}
	}
	// The player, already loaded, stands on its own pad rather than the lobby
	// loading a second copy (not mid-cinematic: that keeps its own staging).
	ARigExecBipedCharacter* Biped = Hero();
	const int32 Lend = (Biped && !Biped->IsInCinematic()) ? Mine : INDEX_NONE;
	if (!Lobby && GetWorld())
	{
		// Above the level, where nothing else stands.
		const FVector Above = (Biped ? Biped->GetActorLocation() : FVector::ZeroVector) + FVector(0.0, 0.0, 800.0);
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Lobby = GetWorld()->SpawnActor<ARigExecCharacterSelect>(Above, FRotator::ZeroRotator, Params);
		Lobby->Stage(Assets, Focus, Lend);
	}
	else if (Lobby)
	{
		// Kept from the last visit, asleep: nothing to load.
		Lobby->SetShown(true);
		Lobby->SetFocus(Focus);
	}
	if (Lobby && Lend != INDEX_NONE)
	{
		Lobby->Lend(Lend, Biped);
	}
	if (PlayerOwner && Lobby)
	{
		PlayerOwner->SetViewTargetWithBlend(Lobby, 0.0f);
	}
}

void
ARigExecMenuHUD::ReopenChooser()
{
	// From the pause menu: unpause and go to the lobby, where the characters idle.
	CloseMenu();
	OpenChooser();
}

void
ARigExecMenuHUD::CloseLobby()
{
	if (Lobby)
	{
		// Kept for the next visit, asleep, with the player given back.
		Lobby->Return();
		Lobby->SetShown(false);
	}
	if (PlayerOwner && PlayerOwner->GetPawn())
	{
		PlayerOwner->SetViewTargetWithBlend(PlayerOwner->GetPawn(), 0.8f, VTBlend_EaseInOut, 2.0f);
	}
}

void
ARigExecMenuHUD::BecomeCharacter(const FSoftObjectPath& Asset)
{
	bChoseThisRun = true;
	ARigExecBipedCharacter::ChooseCharacter(Asset);
	ARigExecBipedCharacter* Biped = Hero();
	// The lobby's preview is already loaded: it becomes the player, where
	// the old pawn stood, so the character does not load twice.
	ARigExecBipedCharacter* Taken = (Lobby && Biped && Biped->GetCharacterAsset() != Asset) ? Lobby->Take(Focus) : nullptr;
	if (Taken && PlayerOwner)
	{
		// The old player stays in the lobby as its own preview, so coming back
		// loads nobody; the new one goes where the old one stood.
		const bool bKeep = Lobby->IsLent(Biped);
		const FTransform Where = bKeep ? Lobby->Keep() : Biped->GetActorTransform();
		const FRotator Facing = PlayerOwner->GetControlRotation();
		PlayerOwner->UnPossess();
		if (bKeep)
		{
			Biped->BecomePreview();
		}
		else
		{
			Biped->Destroy();
		}
		Taken->SetActorTransform(Where, false, nullptr, ETeleportType::TeleportPhysics);
		Taken->BecomePlayable();
		PlayerOwner->Possess(Taken);
		PlayerOwner->SetControlRotation(Facing);
		CloseMenu();
		CloseLobby();
		return;
	}
	CloseMenu();
	CloseLobby();
	AGameModeBase* Mode = GetWorld() ? GetWorld()->GetAuthGameMode() : nullptr;
	if (!Biped || !Mode || !PlayerOwner || Biped->GetCharacterAsset() == Asset)
	{
		return;
	}
	// A fresh pawn loads the new character from the start, where the old one stood.
	const FTransform Where = Biped->GetActorTransform();
	const FRotator Facing = PlayerOwner->GetControlRotation();
	PlayerOwner->UnPossess();
	Biped->Destroy();
	Mode->RestartPlayerAtTransform(PlayerOwner, Where);
	PlayerOwner->SetControlRotation(Facing);
}

void
ARigExecMenuHUD::Apply()
{
	SetCVar(TEXT("r.VSync"), bVSync ? 1 : 0);
	if (IConsoleVariable* Cap = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")))
	{
		Cap->Set(float(FrameCap), ECVF_SetByConsole);
	}
	if (IConsoleVariable* Percent = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage")))
	{
		Percent->Set(float(ScreenPercent), ECVF_SetByConsole);
	}
	if (GetWorld() && GetWorld()->GetWorldSettings())
	{
		GetWorld()->GetWorldSettings()->SetTimeDilation(TimeScale);
	}
}

void
ARigExecMenuHUD::Save() const
{
	if (!GConfig)
	{
		return;
	}
	GConfig->SetBool(GSection, TEXT("ShowFps"), bShowFps, GGameUserSettingsIni);
	GConfig->SetBool(GSection, TEXT("ShowStats"), bShowStats, GGameUserSettingsIni);
	GConfig->SetBool(GSection, TEXT("ShowSubdPanel"), bShowSubdPanel, GGameUserSettingsIni);
	GConfig->SetBool(GSection, TEXT("VSync"), bVSync, GGameUserSettingsIni);
	GConfig->SetInt(GSection, TEXT("FrameCap"), FrameCap, GGameUserSettingsIni);
	GConfig->SetInt(GSection, TEXT("ScreenPercent"), ScreenPercent, GGameUserSettingsIni);
	GConfig->SetBool(GSection, TEXT("RigAsync"), ARigExecBipedCharacter::bRigAsync, GGameUserSettingsIni);
	GConfig->SetInt(GSection, TEXT("RigRateFar"), ARigExecBipedCharacter::RigRateFar, GGameUserSettingsIni);
	GConfig->SetInt(GSection, TEXT("RigRateLobby"), ARigExecBipedCharacter::RigRateLobby, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

ARigExecBipedCharacter*
ARigExecMenuHUD::Hero() const
{
	return Cast<ARigExecBipedCharacter>(GetOwningPawn());
}

void
ARigExecMenuHUD::Text(const FString& Line, float X, float Y, float Scale, const FLinearColor& Color, bool bCentre)
{
	UFont* Font = GEngine ? GEngine->GetLargeFont() : nullptr;
	if (!Font || !Canvas)
	{
		return;
	}
	FCanvasTextItem Item(FVector2D(X, Y), FText::FromString(Line), Font, Color);
	Item.BlendMode = ARigExecVRRig::UiTextBlend();
	Item.Scale = FVector2D(Scale, Scale);
	Item.bOutlined = true;
	Item.OutlineColor = FLinearColor(0, 0, 0, 0.85f);
	if (bCentre)
	{
		Item.bCentreX = true;
		Item.bCentreY = true;
	}
	Canvas->DrawItem(Item);
}

void
ARigExecMenuHUD::DrawRect(FLinearColor RectColor, float ScreenX, float ScreenY, float ScreenW, float ScreenH)
{
	if (!Canvas)
	{
		return;
	}
	FCanvasTileItem Tile(FVector2D(ScreenX, ScreenY), GWhiteTexture, FVector2D(ScreenW, ScreenH), RectColor);
	Tile.BlendMode = ARigExecVRRig::UiBlend();
	Canvas->DrawItem(Tile);
}

void
ARigExecMenuHUD::Button(FName Name, const FString& Label, float X, float Y, float W, float H, float S, bool bEnabled)
{
	const bool bCursor = CursorShown();
	const FLinearColor Fill = bEnabled ? GAccent : FLinearColor(0.2f, 0.2f, 0.2f, 0.6f);
	DrawRect(Fill, X, Y, W, H);
	Text(Label, X + W * 0.5f, Y + H * 0.5f, 1.5f * S, bEnabled ? FLinearColor::White : FLinearColor(0.5f, 0.5f, 0.5f), true);
	if (bCursor && bEnabled)
	{
		HitBox(FVector2D(X, Y), FVector2D(W, H), Name);
	}
}

bool
ARigExecMenuHUD::Hovered(float X, float Y, float W, float H) const
{
	float MouseX = 0.0f, MouseY = 0.0f;
	if (!CursorShown() || !PointerAt(MouseX, MouseY))
	{
		return false;
	}
	return MouseX >= X && MouseX <= X + W && MouseY >= Y && MouseY <= Y + H;
}

void
ARigExecMenuHUD::DrawHUD()
{
	Super::DrawHUD();
	// In VR the panel shows it (DrawVRPanel), not the screen.
	if (!Canvas || VR)
	{
		return;
	}
	DrawFrame();
}

void
ARigExecMenuHUD::DrawFrame()
{
	const bool bLobby = IsMenuOpen() && Page == EPage::Choose && Lobby;
	if (!bLobby)
	{
		DrawGame();
	}
	const float W = Canvas->ClipX, H = Canvas->ClipY, S = H / 720.0f;
	DrawCounters(W, H, S);
	if (IsMenuOpen())
	{
		if (Page == EPage::Choose && Lobby)
		{
			DrawLobby(W, H, S);
		}
		else
		{
			DrawMenu(W, H, S);
		}
	}
	if (bDemo)
	{
		DrawDemo();
	}
}

void
ARigExecMenuHUD::DrawCounters(float W, float H, float S)
{
	// Real time, whatever the time scale or pause.
	const double Delta = FMath::Max(FApp::GetDeltaTime(), 1e-4);
	const double Now = FPlatformTime::Seconds();
	SmoothDelta = FMath::Lerp(SmoothDelta, Delta, 0.1);
	WorstDelta = FMath::Max(WorstDelta, Delta);
	if (Now >= NextShow)
	{
		ShownFps = 1.0 / SmoothDelta;
		ShownMs = SmoothDelta * 1000.0;
		NextShow = Now + 0.25;
	}
	if (Now - WorstSince >= 1.0)
	{
		ShownWorst = WorstDelta * 1000.0;
		WorstDelta = 0.0;
		WorstSince = Now;
	}

	// The counter: frames per second large, on a dark plate so it reads over
	// anything; the frame time and the worst frame under it. Bottom left, or
	// where the level's HUD puts it (the studio: the top of its view).
	float X = 10.0f * S, Y = H - 8.0f * S;
	const bool bTop = CounterAnchor(W, H, S, X, Y);
	if (bShowFps)
	{
		const FLinearColor Color = ShownFps >= 55.0 ? FLinearColor(0.45f, 1.0f, 0.5f)
		                           : ShownFps >= 30.0 ? FLinearColor(1.0f, 0.85f, 0.3f)
		                                              : FLinearColor(1.0f, 0.4f, 0.35f);
		const FString Big = FString::Printf(TEXT("%.0f FPS"), ShownFps);
		const FString Small = FString::Printf(TEXT("%.1f ms   worst %.1f ms"), ShownMs, ShownWorst);
		UFont* Font = GEngine ? GEngine->GetLargeFont() : nullptr;
		float BigW = 0.0f, BigH = 0.0f, SmallW = 0.0f, SmallH = 0.0f;
		if (Font && Canvas)
		{
			Canvas->TextSize(Font, Big, BigW, BigH);
			Canvas->TextSize(Font, Small, SmallW, SmallH);
		}
		const float BigScale = 2.0f * S, SmallScale = 0.9f * S;
		const float PlateW = FMath::Max(BigW * BigScale, SmallW * SmallScale) + 24.0f * S;
		const float PlateH = BigH * BigScale + SmallH * SmallScale + 14.0f * S;
		const float PlateY = bTop ? Y : Y - PlateH;
		DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.62f), X, PlateY, PlateW, PlateH);
		DrawRect(Color, X, PlateY, 4.0f * S, PlateH);
		Text(Big, X + 14.0f * S, PlateY + 4.0f * S, BigScale, Color);
		Text(Small, X + 14.0f * S, PlateY + 6.0f * S + BigH * BigScale, SmallScale, FLinearColor(0.88f, 0.92f, 0.98f));
		Y = bTop ? PlateY + PlateH + 4.0f * S : PlateY - 4.0f * S;
	}
	if (!bShowStats)
	{
		return;
	}
	// Above the counter: where the frame goes, and what the rig is doing.
	TArray<FString> Lines;
	const double Game = FPlatformTime::ToMilliseconds(GGameThreadTime);
	const double Render = FPlatformTime::ToMilliseconds(GRenderThreadTime);
	const double Gpu = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
	Lines.Add(FString::Printf(TEXT("game %.1f ms   render %.1f ms   gpu %.1f ms"), Game, Render, Gpu));
	const FIntPoint Size = GetWorld() && GetWorld()->GetGameViewport() ? GetWorld()->GetGameViewport()->Viewport->GetSizeXY() : FIntPoint(0, 0);
	Lines.Add(FString::Printf(TEXT("%d x %d at %d%%   time x%.2f%s"), Size.X, Size.Y, ScreenPercent, TimeScale,
	                          UGameplayStatics::IsGamePaused(this) ? TEXT("   paused") : TEXT("")));
	if (ARigExecBipedCharacter* Biped = Hero())
	{
		const int32 Level = Biped->GetSubdivisionLevel();
		Lines.Add(FString::Printf(TEXT("speed %.0f cm/s   subd %s   %s"), Biped->GetVelocity().Size2D(),
		                          Level < 0 ? TEXT("n/a") : *FString::FromInt(Level),
		                          Biped->IsInCinematic() ? TEXT("close-up") : TEXT("play")));
		if (Biped->Retarget)
		{
			Lines.Add(FString::Printf(TEXT("retarget %.2f ms   error %.2f deg"), Biped->Retarget->LastMilliseconds,
			                          Biped->Retarget->LastErrorDegrees));
			if (Biped->Rig)
			{
				Lines.Add(FString::Printf(TEXT("rig run %.1f ms %s   publish %.1f ms   rate 1/%d"), Biped->Rig->LastExecuteMs,
				                          Biped->Rig->bEvaluateAsync ? TEXT("(workers)") : TEXT("(game thread)"),
				                          Biped->Rig->LastPublishMs, Biped->Rig->EvaluateEvery));
			}
			Lines.Add(Biped->Retarget->Status.Left(70));
		}
	}
	// Twice the size it was, to read from across the room or on a phone.
	const float Row = 40.0f * S;
	const float Top = bTop ? Y : Y - Row * Lines.Num() - 14.0f * S;
	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.62f), X, Top, 940.0f * S, Row * Lines.Num() + 14.0f * S);
	for (int32 Each = 0; Each < Lines.Num(); ++Each)
	{
		Text(Lines[Each], X + 12.0f * S, Top + 6.0f * S + Row * Each, 1.7f * S, FLinearColor(0.85f, 0.92f, 1.0f));
	}
}

TArray<ARigExecMenuHUD::FItem>
ARigExecMenuHUD::Items()
{
	TArray<FItem> List;
	if (Page == EPage::Choose)
	{
		for (const ARigExecBipedCharacter::FChoice& Choice : ARigExecBipedCharacter::FindCharacters())
		{
			const FSoftObjectPath Asset = Choice.Asset;
			List.Add({Choice.Label, TEXT(""), [this, Asset] { BecomeCharacter(Asset); }});
		}
		return List;
	}
	if (Page == EPage::Main)
	{
		List.Add({TEXT("RESUME"), TEXT(""), [this] { CloseMenu(); }});
		if (ARigExecBipedCharacter::FindCharacters().Num() > 1)
		{
			List.Add({TEXT("CHOOSE CHARACTER"), TEXT(""), [this] { ReopenChooser(); }});
		}
		// The animation studio and the game, each a level of its own (the
		// game is played on the desktop: in VR it is the studio).
		{
			FItem Item{IsStudio() ? (VR ? TEXT("PLAY THE GAME (DESKTOP ONLY)") : TEXT("PLAY THE GAME")) : TEXT("ANIMATION STUDIO"), TEXT(""), [this] {
				           Save();
				           CloseMenu();
				           UGameplayStatics::OpenLevel(this, IsStudio() ? FName(TEXT("/Game/RigExec/L_RigExecGame"))
				                                                        : FName(TEXT("/Game/RigExec/L_RigExecStudio")));
			           }};
			Item.bEnabled = !(IsStudio() && VR);
			List.Add(Item);
		}
		List.Add({TEXT("SETTINGS"), TEXT(""), [this] {
			          Page = EPage::Settings;
			          Focus = 0;
		          }});
		List.Add({TEXT("QUIT GAME"), TEXT(""), [this] {
			          Save();
			          UKismetSystemLibrary::QuitGame(GetWorld(), PlayerOwner, EQuitPreference::Quit, false);
		          }});
		return List;
	}
	ARigExecBipedCharacter* Biped = Hero();
	{
		FString Who = TEXT("?");
		for (const ARigExecBipedCharacter::FChoice& Choice : ARigExecBipedCharacter::FindCharacters())
		{
			if (Biped && Choice.Asset == Biped->GetCharacterAsset())
			{
				Who = Choice.Label;
			}
		}
		FItem Item{TEXT("Character"), Who, [this] {
			           Save();
			           ReopenChooser();
		           }};
		Item.bEnabled = ARigExecBipedCharacter::FindCharacters().Num() > 1;
		List.Add(Item);
	}
	{
		// The headset: on or off without restarting (Meta Quest Link, or any
		// OpenXR runtime, set as the PC's active one).
		const bool bXR = GEngine && GEngine->XRSystem.IsValid() && GEngine->StereoRenderingDevice.IsValid();
		FItem Item{TEXT("VR headset"), bXR ? OnOff(ARigExecVRRig::HeadsetOn()) : FString(TEXT("n/a")), [] {
			           GEngine->StereoRenderingDevice->EnableStereo(!ARigExecVRRig::HeadsetOn());
		           }};
		Item.bEnabled = bXR;
		List.Add(Item);
	}
	List.Add({TEXT("FPS counter"), OnOff(bShowFps), [this] { bShowFps = !bShowFps; }});
	List.Add({TEXT("Debug stats"), OnOff(bShowStats), [this] { bShowStats = !bShowStats; }});
	List.Add({TEXT("Engine stats (stat unit)"), OnOff(bEngineStats), [this] {
		          bEngineStats = !bEngineStats;
		          if (PlayerOwner)
		          {
			          PlayerOwner->ConsoleCommand(TEXT("stat unit"));
		          }
	          }});
	List.Add({TEXT("Subdivision panel"), OnOff(bShowSubdPanel), [this] { bShowSubdPanel = !bShowSubdPanel; }});
	{
		const int32 Level = Biped ? Biped->GetSubdivisionLevel() : -1;
		FItem Item{TEXT("Subdivision level"), Level < 0 ? TEXT("n/a") : FString::FromInt(Level), [Biped, Level] {
			           Biped->SetSubdivisionLevel(Level >= ARigExecBipedCharacter::MaxSubdivisionLevel ? 0 : Level + 1);
		           }};
		Item.bEnabled = Level >= 0;
		List.Add(Item);
	}
	{
		FItem Item{TEXT("Show Mannequin"), OnOff(Biped && Biped->bShowSource), [Biped] { Biped->ToggleSource(); }};
		Item.bEnabled = Biped != nullptr;
		List.Add(Item);
	}
	auto Rate = [](int32 Every) { return Every <= 1 ? FString(TEXT("FULL")) : FString::Printf(TEXT("1/%d"), Every); };
	List.Add({TEXT("Rig on worker threads"), OnOff(ARigExecBipedCharacter::bRigAsync),
	          [] { ARigExecBipedCharacter::bRigAsync = !ARigExecBipedCharacter::bRigAsync; }});
	List.Add({TEXT("Rig rate, far (close-up always full)"), Rate(ARigExecBipedCharacter::RigRateFar),
	          [] { ARigExecBipedCharacter::RigRateFar = Next<int32>({1, 2, 3}, ARigExecBipedCharacter::RigRateFar); }});
	List.Add({TEXT("Rig rate, lobby"), Rate(ARigExecBipedCharacter::RigRateLobby),
	          [] { ARigExecBipedCharacter::RigRateLobby = Next<int32>({1, 2, 3, 4}, ARigExecBipedCharacter::RigRateLobby); }});
	List.Add({TEXT("Time scale"), FString::Printf(TEXT("x%g"), TimeScale), [this] {
		          TimeScale = Next<float>({1.0f, 0.5f, 0.25f, 0.1f}, TimeScale);
		          Apply();
	          }});
	List.Add({TEXT("VSync"), OnOff(bVSync), [this] {
		          bVSync = !bVSync;
		          Apply();
	          }});
	List.Add({TEXT("Frame cap"), FrameCap > 0 ? FString::FromInt(FrameCap) : TEXT("OFF"), [this] {
		          FrameCap = Next<int32>({0, 30, 60, 120}, FrameCap);
		          Apply();
	          }});
	List.Add({TEXT("Screen percentage"), FString::Printf(TEXT("%d%%"), ScreenPercent), [this] {
		          ScreenPercent = Next<int32>({100, 75, 50}, ScreenPercent);
		          Apply();
	          }});
	List.Add({TEXT("BACK"), TEXT(""), [this] { MenuBack(); }});
	return List;
}

void
ARigExecMenuHUD::DrawMenu(float W, float H, float S)
{
	DrawRect(FLinearColor(0.0f, 0.02f, 0.05f, 0.62f), 0.0f, 0.0f, W, H);
	const TArray<FItem> List = Items();
	Focus = FMath::Clamp(Focus, 0, List.Num() - 1);
	const bool bSettings = Page == EPage::Settings;
	const float PanelW = (bSettings ? 620.0f : 420.0f) * S;
	const float RowH = (bSettings ? 40.0f : 62.0f) * S;
	const float PanelH = 110.0f * S + RowH * List.Num() + 46.0f * S;
	const float X = (W - PanelW) * 0.5f, Y = (H - PanelH) * 0.5f;
	DrawRect(FLinearColor(0.02f, 0.05f, 0.1f, 0.92f), X, Y, PanelW, PanelH);
	DrawRect(GCyan, X, Y, PanelW, 3.0f * S);
	const bool bChoose = Page == EPage::Choose;
	Text(bSettings ? TEXT("SETTINGS") : bChoose ? TEXT("CHOOSE CHARACTER") : TEXT("PAUSED"), W * 0.5f, Y + 48.0f * S, 2.2f * S,
	     FLinearColor::White, true);

	for (int32 Each = 0; Each < List.Num(); ++Each)
	{
		const FItem& Item = List[Each];
		const float RowY = Y + 96.0f * S + RowH * Each;
		const float Inset = 24.0f * S;
		const float BoxX = X + Inset, BoxW = PanelW - 2.0f * Inset, BoxH = RowH - 8.0f * S;
		const FName Name(*FString::Printf(TEXT("Menu.%d"), Each));
		DemoTargets.Add(TEXT("menu:") + Item.Label, FBox2D(FVector2D(BoxX, RowY), FVector2D(BoxX + BoxW, RowY + BoxH)));
		if (Item.bEnabled && Hovered(BoxX, RowY, BoxW, BoxH))
		{
			Focus = Each;
		}
		const bool bFocus = Focus == Each;
		const bool bButton = Item.Value.IsEmpty();
		const FLinearColor Dim(0.55f, 0.55f, 0.6f);
		if (bButton)
		{
			DrawRect(bFocus ? GAccent : FLinearColor(0.1f, 0.18f, 0.28f, 0.9f), BoxX, RowY, BoxW, BoxH);
			Text(Item.Label, BoxX + BoxW * 0.5f, RowY + BoxH * 0.5f, (bSettings ? 1.1f : 1.5f) * S, FLinearColor::White, true);
		}
		else
		{
			if (bFocus)
			{
				DrawRect(FLinearColor(0.1f, 0.3f, 0.5f, 0.55f), BoxX, RowY, BoxW, BoxH);
			}
			Text(Item.Label, BoxX + 14.0f * S, RowY + 7.0f * S, 1.05f * S, Item.bEnabled ? FLinearColor::White : Dim);
			const float ValueW = 150.0f * S;
			const bool bOn = Item.Value != TEXT("OFF") && Item.Value != TEXT("n/a");
			DrawRect(Item.bEnabled ? (bOn ? GAccent : FLinearColor(0.2f, 0.22f, 0.26f, 0.95f)) : FLinearColor(0.15f, 0.15f, 0.15f, 0.8f),
			         BoxX + BoxW - ValueW - 6.0f * S, RowY + 4.0f * S, ValueW, BoxH - 8.0f * S);
			Text(Item.Value, BoxX + BoxW - ValueW * 0.5f - 6.0f * S, RowY + BoxH * 0.5f, 1.05f * S,
			     Item.bEnabled ? FLinearColor::White : Dim, true);
		}
		if (Item.bEnabled && CursorShown())
		{
			HitBox(FVector2D(BoxX, RowY), FVector2D(BoxW, BoxH), Name, 10);
		}
	}
	Text(VR          ? TEXT("point and pull the trigger, or stick and A     B back     Menu button resume")
	     : bSettings ? TEXT("click or Enter / A to change     Esc / B back")
	     : bChoose   ? TEXT("click or Enter / A to play")
	                 : TEXT("Esc / P / Start  resume"),
	     W * 0.5f,
	     Y + PanelH - 22.0f * S, 0.9f * S, FLinearColor(0.7f, 0.78f, 0.85f), true);
}

void
ARigExecMenuHUD::NotifyHitBoxClick(FName BoxName)
{
	Super::NotifyHitBoxClick(BoxName);
	const FString Name = BoxName.ToString();
	if (!IsMenuOpen() || !Name.StartsWith(TEXT("Menu.")))
	{
		return;
	}
	Focus = FCString::Atoi(*Name.Mid(5));
	MenuSelect();
}

void
ARigExecMenuHUD::OpenMenu(bool bPause)
{
	if (IsMenuOpen() || !PlayerOwner)
	{
		return;
	}
	Page = EPage::Main;
	Focus = 0;
	bHadCursor = PlayerOwner->bShowMouseCursor;
	bHadClicks = PlayerOwner->bEnableClickEvents;
	PlayerOwner->bShowMouseCursor = true;
	PlayerOwner->bEnableClickEvents = true;
	FInputModeGameAndUI Mode;
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	Mode.SetHideCursorDuringCapture(false);
	PlayerOwner->SetInputMode(Mode);
	UGameplayStatics::SetGamePaused(this, bPause);
}

void
ARigExecMenuHUD::CloseMenu()
{
	if (!IsMenuOpen())
	{
		return;
	}
	Page = EPage::None;
	Save();
	UGameplayStatics::SetGamePaused(this, false);
	if (PlayerOwner)
	{
		PlayerOwner->bShowMouseCursor = bHadCursor;
		PlayerOwner->bEnableClickEvents = bHadClicks;
		PlayerOwner->SetInputMode(FInputModeGameOnly());
	}
}

void
ARigExecMenuHUD::ToggleMenu()
{
	IsMenuOpen() ? CloseMenu() : OpenMenu();
}

void
ARigExecMenuHUD::MenuBack()
{
	if (Page == EPage::Settings)
	{
		Save();
		Page = EPage::Main;
		const TArray<FItem> Main = Items();
		Focus = FMath::Max(0, Main.IndexOfByPredicate([](const FItem& Item) { return Item.Label == TEXT("SETTINGS"); }));
	}
	else if (Page == EPage::Choose)
	{
		// Leaving the chooser keeps the character already loaded.
		bChoseThisRun = true;
		CloseMenu();
		CloseLobby();
	}
	else
	{
		CloseMenu();
	}
}

void
ARigExecMenuHUD::MenuMove(int32 Step)
{
	if (!IsMenuOpen())
	{
		return;
	}
	const TArray<FItem> List = Items();
	for (int32 Tries = 0; Tries < List.Num(); ++Tries)
	{
		Focus = (Focus + Step + List.Num()) % List.Num();
		if (List[Focus].bEnabled)
		{
			break;
		}
	}
	if (Lobby && Page == EPage::Choose)
	{
		Lobby->SetFocus(Focus);
	}
}

void
ARigExecMenuHUD::MenuSelect()
{
	if (!IsMenuOpen())
	{
		return;
	}
	TArray<FItem> List = Items();
	if (List.IsValidIndex(Focus) && List[Focus].bEnabled && List[Focus].Press)
	{
		List[Focus].Press();
	}
}

void
ARigExecMenuHUD::DrawLobby(float W, float H, float S)
{
	const TArray<ARigExecBipedCharacter::FChoice> Choices = ARigExecBipedCharacter::FindCharacters();
	// A band at the top and the bottom, like a lobby.
	DrawRect(FLinearColor(0.0f, 0.01f, 0.03f, 0.75f), 0.0f, 0.0f, W, 92.0f * S);
	DrawRect(GCyan, 0.0f, 92.0f * S, W, 2.0f * S);
	DrawRect(FLinearColor(0.0f, 0.01f, 0.03f, 0.75f), 0.0f, H - 70.0f * S, W, 70.0f * S);
	Text(TEXT("CHOOSE YOUR CHARACTER"), W * 0.5f, 46.0f * S, 2.4f * S, FLinearColor::White, true);
	const bool bCursor = CursorShown();
	// Hover picks only when the mouse moves, so a still cursor does not undo
	// the keyboard's choice.
	float MouseX = 0.0f, MouseY = 0.0f;
	const bool bMouse = PointerAt(MouseX, MouseY);
	const bool bMoved = bMouse && FVector2D(MouseX, MouseY).Equals(LastMouse, 1.0) == false;
	LastMouse = FVector2D(MouseX, MouseY);
	for (int32 Each = 0; Each < Choices.Num() && Each < Lobby->Num(); ++Each)
	{
		FVector Feet;
		float Height = 0.0f;
		if (!Lobby->GetPreviewBounds(Each, Feet, Height))
		{
			continue;
		}
		FVector2D Low, High;
		if (!ToScreen(Feet, Low) ||
		    !ToScreen(Feet + FVector(0.0, 0.0, Height), High))
		{
			continue;
		}
		const float BoxH = float(Low.Y - High.Y), BoxW = BoxH * 0.55f;
		const float BoxX = float(Low.X) - BoxW * 0.5f, BoxY = float(High.Y);
		if (bMoved && Hovered(BoxX, BoxY, BoxW, BoxH) && Focus != Each)
		{
			Focus = Each;
			Lobby->SetFocus(Each);
		}
		const bool bFocus = Focus == Each;
		// The name on a plate under the feet; the chosen one large and lit.
		const float PlateW = (bFocus ? 230.0f : 170.0f) * S, PlateH = (bFocus ? 46.0f : 34.0f) * S;
		const float PlateX = float(Low.X) - PlateW * 0.5f, PlateY = float(Low.Y) + 18.0f * S;
		DrawRect(bFocus ? GAccent : FLinearColor(0.05f, 0.1f, 0.18f, 0.85f), PlateX, PlateY, PlateW, PlateH);
		Text(Choices[Each].Label, float(Low.X), PlateY + PlateH * 0.5f, (bFocus ? 1.7f : 1.2f) * S,
		     bFocus ? FLinearColor::White : FLinearColor(0.7f, 0.78f, 0.88f), true);
		if (bCursor)
		{
			HitBox(FVector2D(BoxX, BoxY), FVector2D(BoxW, BoxH + 80.0f * S), FName(*FString::Printf(TEXT("Menu.%d"), Each)), 10);
		}
	}
	Text(VR ? TEXT("point and pull the trigger, or stick left / right and A          B back")
	        : TEXT("<  >  or  A / D  choose          Enter / click  play"),
	     W * 0.5f, H - 35.0f * S, 1.1f * S,
	     FLinearColor(0.75f, 0.85f, 0.95f), true);
}

// --- VR ----------------------------------------------------------------------

bool
ARigExecMenuHUD::CursorShown() const
{
	return VR != nullptr || (PlayerOwner && PlayerOwner->bShowMouseCursor);
}

bool
ARigExecMenuHUD::PointerAt(float& X, float& Y) const
{
	if (VR)
	{
		X = float(VRIn.Pointer.X);
		Y = float(VRIn.Pointer.Y);
		return VRIn.bOnPanel;
	}
	return PlayerOwner && PlayerOwner->GetMousePosition(X, Y);
}

void
ARigExecMenuHUD::HitBox(const FVector2D& Position, const FVector2D& Size, FName Name, int32 Priority)
{
	if (VR)
	{
		VRHits.Add({FBox2D(Position, Position + Size), Name, Priority});
		return;
	}
	AddHitBox(Position, Size, Name, true, Priority);
}

bool
ARigExecMenuHUD::VRPressed(const FKey& Key) const
{
	return ARigExecVRRig::XRKeyPressed(Key) || (PlayerOwner && PlayerOwner->WasInputKeyJustPressed(Key));
}

bool
ARigExecMenuHUD::ToScreen(const FVector& World, FVector2D& Out) const
{
	if (VR)
	{
		return VR->ToPanel(World, Out);
	}
	return PlayerOwner && PlayerOwner->ProjectWorldLocationToScreen(World, Out);
}

bool
ARigExecMenuHUD::FromScreen(const FVector2D& At, FVector& Origin, FVector& Direction) const
{
	if (VR)
	{
		VR->FromPanel(At, Origin, Direction);
		return true;
	}
	return PlayerOwner && PlayerOwner->DeprojectScreenPositionToWorld(At.X, At.Y, Origin, Direction);
}

void
ARigExecMenuHUD::GetView(FVector& Eye, FVector& Forward, FVector& Right, FVector& Up) const
{
	if (VR)
	{
		Eye = VR->Eye();
		VR->ViewAxes(Forward, Right, Up);
		return;
	}
	const APlayerCameraManager* Camera = PlayerOwner ? PlayerOwner->PlayerCameraManager.Get() : nullptr;
	Eye = Camera ? Camera->GetCameraLocation() : FVector::ZeroVector;
	const FRotationMatrix Axes(Camera ? Camera->GetCameraRotation() : FRotator::ZeroRotator);
	Forward = Axes.GetUnitAxis(EAxis::X);
	Right = Axes.GetUnitAxis(EAxis::Y);
	Up = Axes.GetUnitAxis(EAxis::Z);
}

double
ARigExecMenuHUD::PixelSizeAt(const FVector& At) const
{
	if (VR)
	{
		return VR->PixelSizeAt(At);
	}
	const APlayerCameraManager* Camera = PlayerOwner ? PlayerOwner->PlayerCameraManager.Get() : nullptr;
	if (!Camera)
	{
		return 0.1;
	}
	int32 ViewX = 0, ViewY = 0;
	PlayerOwner->GetViewportSize(ViewX, ViewY);
	const double Depth = FMath::Max(FVector::DotProduct(At - Camera->GetCameraLocation(), Camera->GetCameraRotation().Vector()), 1.0);
	return 2.0 * Depth * FMath::Tan(FMath::DegreesToRadians(Camera->GetFOVAngle() * 0.5)) / FMath::Max(double(ViewX), 1.0);
}

bool
ARigExecMenuHUD::GetVRStage(FVector& LookAt, float& Yaw, float& Distance, float& FloorZ) const
{
	if (IsChoosing())
	{
		// The lobby: everyone on their pads, from in front.
		FBox Feet(ForceInit);
		float Tallest = 0.0f;
		for (int32 Each = 0; Each < Lobby->Num(); ++Each)
		{
			FVector At;
			float Height = 0.0f;
			if (Lobby->GetPreviewBounds(Each, At, Height))
			{
				Feet += At;
				Tallest = FMath::Max(Tallest, Height);
			}
		}
		if (!Feet.IsValid)
		{
			return false;
		}
		FloorZ = float(Feet.Min.Z);
		LookAt = FVector(Feet.GetCenter().X, Feet.GetCenter().Y, FloorZ + Tallest * 0.55);
		Yaw = Lobby->GetActorRotation().Yaw;
		Distance = FMath::Max(400.0f, float(Feet.GetSize().Size2D()) + 280.0f);
		return true;
	}
	const APawn* Pawn = PlayerOwner ? PlayerOwner->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}
	FloorZ = float(Pawn->GetActorLocation().Z - Pawn->GetSimpleCollisionHalfHeight());
	LookAt = Pawn->GetActorLocation();
	Yaw = PlayerOwner->GetControlRotation().Yaw;
	Distance = 350.0f;
	return true;
}

namespace
{
/** What a headset does without: blur and lens effects (sickening when the
 * view is your head), the costly shadows and anti-aliasing. Each value as it
 * was, to put back. */
struct FVRSetting
{
	const TCHAR* Name;
	float InVR;
	float Was;
};
FVRSetting GVRSettings[] = {
	{TEXT("r.MotionBlurQuality"), 0.0f, 0.0f},
	{TEXT("r.DepthOfFieldQuality"), 0.0f, 0.0f},
	{TEXT("r.LensFlareQuality"), 0.0f, 0.0f},
	{TEXT("r.SceneColorFringeQuality"), 0.0f, 0.0f},
	{TEXT("r.Shadow.Virtual.Enable"), 0.0f, 0.0f},
	{TEXT("r.AntiAliasingMethod"), 2.0f, 0.0f},
};

void UseVRSettings(bool bVR)
{
	for (FVRSetting& Setting : GVRSettings)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Setting.Name))
		{
			if (bVR)
			{
				Setting.Was = Variable->GetFloat();
			}
			Variable->Set(bVR ? Setting.InVR : Setting.Was, ECVF_SetByCode);
		}
	}
}
} // namespace

void
ARigExecMenuHUD::UpdateVR(float DeltaSeconds)
{
	const bool bWant = PlayerOwner && GetWorld() && ARigExecVRRig::Wanted();
	if (bWant && !VR)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		// Owned by the player: a motion controller only reads its hand when
		// its actor belongs to a local player.
		Params.Owner = PlayerOwner;
		VR = GetWorld()->SpawnActor<ARigExecVRRig>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		UseVRSettings(true);
		UE_LOG(LogTemp, Log, TEXT("RigExec VR: on (%s)"), ARigExecVRRig::HeadsetOn() ? TEXT("headset") : TEXT("desktop stand-in"));
	}
	else if (!bWant && VR)
	{
		// Back to the screen: the view the level had.
		VR->Destroy();
		VR = nullptr;
		UseVRSettings(false);
		if (PlayerOwner)
		{
			PlayerOwner->SetViewTarget(IsChoosing() ? static_cast<AActor*>(Lobby.Get()) : static_cast<AActor*>(PlayerOwner->GetPawn()));
		}
		UE_LOG(LogTemp, Log, TEXT("RigExec VR: off"));
		return;
	}
	if (!VR)
	{
		return;
	}
	// The game is played on the desktop: in a headset it is the studio.
	if (!IsStudio() && !bDemo)
	{
		if (!bSentToStudio)
		{
			bSentToStudio = true;
			Save();
			UGameplayStatics::OpenLevel(this, FName(TEXT("/Game/RigExec/L_RigExecStudio")));
		}
		return;
	}
	if (PlayerOwner->GetViewTarget() != VR)
	{
		PlayerOwner->SetViewTarget(VR);
	}
	FVector LookAt = FVector::ZeroVector;
	float Yaw = 0.0f, Distance = 350.0f, FloorZ = 0.0f;
	if (GetVRStage(LookAt, Yaw, Distance, FloorZ))
	{
		VR->Stage(LookAt, Yaw, Distance, FloorZ);
	}
	VR->SetLobbyLook(IsChoosing());
	ARigExecVRRig::XRInputFrame();
	ReadVRInput();
	if (ARigExecVRRig::HeadsetOn())
	{
		// Once a second: what the controllers report, for the log.
		VRReportTime += DeltaSeconds;
		if (VRReportTime > 1.0f)
		{
			VRReportTime = 0.0f;
			FVector From, Direction;
			FQuat Turn;
			const bool bLeft = VR->HandRay(0, From, Direction, Turn), bRight = VR->HandRay(1, From, Direction, Turn);
			UE_LOG(LogTemp, Log, TEXT("RigExec VR input: focus %d, tracked L%d R%d, hand %d, on panel %d at (%.0f %.0f), %d click boxes, trigger %d grip %d, player R trigger %.2f; %s"),
				FApp::HasVRFocus() ? 1 : 0, bLeft ? 1 : 0, bRight ? 1 : 0, VRIn.Hand, VRIn.bOnPanel ? 1 : 0, VRIn.Pointer.X, VRIn.Pointer.Y,
				VRHits.Num(), VRIn.bTrigger ? 1 : 0, VRIn.bGrip ? 1 : 0,
				PlayerOwner->GetInputAnalogKeyState(EKeys::OculusTouch_Right_Trigger_Axis), *ARigExecVRRig::XRInputReport());
		}
	}
	if (IsMenuOpen())
	{
		VRMenuInput();
	}
	else if (VRPressed(EKeys::OculusTouch_Left_Menu_Click))
	{
		OpenMenu();
	}
}

void
ARigExecMenuHUD::ReadVRInput()
{
	FVRInput& In = VRIn;
	const bool bHeadset = ARigExecVRRig::HeadsetOn();
	// Both ways in: straight from the headset's input, and the player's.
	auto Axis = [this](const FKey& Key)
	{
		const float Direct = ARigExecVRRig::XRKeyAxis(Key);
		const float Player = PlayerOwner ? PlayerOwner->GetInputAnalogKeyState(Key) : 0.0f;
		return FMath::Abs(Direct) > FMath::Abs(Player) ? Direct : Player;
	};
	auto Down = [this](const FKey& Key) { return ARigExecVRRig::XRKeyDown(Key) || (PlayerOwner && PlayerOwner->IsInputKeyDown(Key)); };
	bool Trigger[2] = {false, false}, Grip[2] = {false, false};
	if (bHeadset)
	{
		const FKey TriggerAxis[2] = {EKeys::OculusTouch_Left_Trigger_Axis, EKeys::OculusTouch_Right_Trigger_Axis};
		const FKey TriggerClick[2] = {EKeys::OculusTouch_Left_Trigger_Click, EKeys::OculusTouch_Right_Trigger_Click};
		const FKey GripAxis[2] = {EKeys::OculusTouch_Left_Grip_Axis, EKeys::OculusTouch_Right_Grip_Axis};
		const FKey GripClick[2] = {EKeys::OculusTouch_Left_Grip_Click, EKeys::OculusTouch_Right_Grip_Click};
		for (int32 H = 0; H < 2; ++H)
		{
			// Pressed past 60%, let go under 35%: no chatter at the edge.
			Trigger[H] = Down(TriggerClick[H]) || Axis(TriggerAxis[H]) > (In.bWasTrigger[H] ? 0.35f : 0.6f);
			Grip[H] = Down(GripClick[H]) || Axis(GripAxis[H]) > (In.bWasGrip[H] ? 0.35f : 0.6f);
		}
		In.Stick[0] = FVector2D(Axis(EKeys::OculusTouch_Left_Thumbstick_X), Axis(EKeys::OculusTouch_Left_Thumbstick_Y));
		In.Stick[1] = FVector2D(Axis(EKeys::OculusTouch_Right_Thumbstick_X), Axis(EKeys::OculusTouch_Right_Thumbstick_Y));
	}
	else
	{
		// The desktop stand-in: the mouse is the right hand.
		Trigger[1] = Down(EKeys::LeftMouseButton);
		Grip[1] = Down(EKeys::RightMouseButton);
		In.Stick[0] = In.Stick[1] = FVector2D::ZeroVector;
	}
	for (int32 H = 0; H < 2; ++H)
	{
		if (Trigger[H] && !In.bWasTrigger[H])
		{
			In.Hand = H;
		}
		if (Grip[H] && !In.bWasGrip[H])
		{
			In.GripHand = H;
		}
		// A flick: a stick pushed past 60%, once until it comes back under 35%.
		int32 Flick[2] = {0, 0};
		for (int32 A = 0; A < 2; ++A)
		{
			const double V = A == 0 ? In.Stick[H].X : In.Stick[H].Y;
			if (!In.bHeld[H][A] && FMath::Abs(V) > 0.6)
			{
				In.bHeld[H][A] = true;
				Flick[A] = V > 0.0 ? 1 : -1;
			}
			else if (In.bHeld[H][A] && FMath::Abs(V) < 0.35)
			{
				In.bHeld[H][A] = false;
			}
		}
		In.Flick[H] = FIntPoint(Flick[0], Flick[1]);
	}
	In.bTrigger = Trigger[In.Hand];
	In.bTriggerPress = Trigger[In.Hand] && !In.bWasTrigger[In.Hand];
	In.bTriggerRelease = !Trigger[In.Hand] && In.bWasTrigger[In.Hand];
	In.bGrip = Grip[In.GripHand];
	In.bGripPress = Grip[In.GripHand] && !In.bWasGrip[In.GripHand];
	In.bGripRelease = !Grip[In.GripHand] && In.bWasGrip[In.GripHand];
	for (int32 H = 0; H < 2; ++H)
	{
		In.bWasTrigger[H] = Trigger[H];
		In.bWasGrip[H] = Grip[H];
	}

	// The lasers, and the pointing one's point on the panel.
	const bool bWasOn = In.bOnPanel;
	In.bOnPanel = false;
	for (int32 H = 0; H < 2; ++H)
	{
		FVector From, Direction;
		FQuat Turn;
		if (!VR->HandRay(H, From, Direction, Turn))
		{
			VR->SetLaser(H, false, FVector::ZeroVector, FVector::ZeroVector, false);
			continue;
		}
		FVector2D Pixel;
		FVector Point;
		const bool bHit = VR->HitPanel(From, Direction, Pixel, Point);
		VR->SetLaser(H, true, From, bHit ? Point : From + Direction * 400.0, H == In.Hand);
		if (H == In.Hand && bHit)
		{
			// A hand shakes: steady the pointer when it is nearly still, follow
			// it at once when it moves.
			if (bHeadset && bWasOn)
			{
				const double Alpha = FMath::Clamp(FVector2D::Distance(Pixel, In.Pointer) / 30.0, 0.3, 1.0);
				In.Pointer = FMath::Lerp(In.Pointer, Pixel, Alpha);
			}
			else
			{
				In.Pointer = Pixel;
			}
			In.bOnPanel = true;
		}
		if (H == In.GripHand)
		{
			In.GripAt = From;
			In.GripTurn = Turn;
		}
	}
}

void
ARigExecMenuHUD::VRMenuInput()
{
	// The sticks move the highlight (across in the lobby, up and down in
	// the menus); A takes it, B goes back. The trigger clicks what it points at.
	const bool bChoose = Page == EPage::Choose;
	int32 Step = 0;
	for (int32 H = 0; H < 2; ++H)
	{
		Step += bChoose ? VRIn.Flick[H].X : -VRIn.Flick[H].Y;
	}
	if (Step != 0)
	{
		MenuMove(Step > 0 ? 1 : -1);
	}
	if (VRPressed(EKeys::OculusTouch_Right_A_Click) || VRPressed(EKeys::OculusTouch_Left_X_Click))
	{
		MenuSelect();
	}
	else if (VRPressed(EKeys::OculusTouch_Right_B_Click) || VRPressed(EKeys::OculusTouch_Left_Y_Click) ||
	         VRPressed(EKeys::OculusTouch_Left_Menu_Click))
	{
		MenuBack();
	}
}

void
ARigExecMenuHUD::PaintVR(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	if (!VR || World != GetWorld())
	{
		return;
	}
	UCanvasRenderTarget2D* Target = VR->GetTarget();
	if (!Target)
	{
		return;
	}
	if (!Target->OnCanvasRenderTargetUpdate.IsAlreadyBound(this, &ARigExecMenuHUD::DrawVRPanel))
	{
		Target->OnCanvasRenderTargetUpdate.AddDynamic(this, &ARigExecMenuHUD::DrawVRPanel);
	}
	VRHits.Reset();
	Target->FastUpdateResource();
	// The laser's click on what the menus drew as clickable.
	if (VRIn.bTriggerPress && VRIn.bOnPanel)
	{
		const FVRHit* Best = nullptr;
		for (const FVRHit& Hit : VRHits)
		{
			if (Hit.Box.IsInside(VRIn.Pointer) && (!Best || Hit.Priority >= Best->Priority))
			{
				Best = &Hit;
			}
		}
		if (Best)
		{
			NotifyHitBoxClick(Best->Name);
		}
	}
}

void
ARigExecMenuHUD::DrawVRPanel(UCanvas* InCanvas, int32 Width, int32 Height)
{
	// The HUD as on the desktop, onto the panel's canvas.
	UCanvas* WasCanvas = Canvas;
	UCanvas* WasDebug = DebugCanvas;
	Canvas = InCanvas;
	DebugCanvas = InCanvas;
	ARigExecVRRig::bPaintingPanel = true;
	DrawFrame();
	DrawVRCursor();
	ARigExecVRRig::bPaintingPanel = false;
	Canvas = WasCanvas;
	DebugCanvas = WasDebug;
}

void
ARigExecMenuHUD::DrawVRCursor()
{
	if (!Canvas || !VRIn.bOnPanel || bDemo)
	{
		return;
	}
	// A ring where the laser meets the panel, lit while the trigger is held.
	const float S = Canvas->ClipY / 720.0f;
	const FVector2D P = VRIn.Pointer;
	const FLinearColor Ring = VRIn.bTrigger ? FLinearColor(1.0f, 0.85f, 0.3f, 1.0f) : FLinearColor(1.0f, 1.0f, 1.0f, 0.95f);
	const float R = 7.0f * S;
	for (int32 K = 0; K < 20; ++K)
	{
		const float A0 = K * 2.0f * PI / 20.0f, A1 = (K + 1) * 2.0f * PI / 20.0f;
		const FVector2D From = P + FVector2D(FMath::Cos(A0), FMath::Sin(A0)) * R, To = P + FVector2D(FMath::Cos(A1), FMath::Sin(A1)) * R;
		DrawLine(From.X, From.Y, To.X, To.Y, FLinearColor(0.0f, 0.0f, 0.0f, 0.8f), 4.0f * S);
		DrawLine(From.X, From.Y, To.X, To.Y, Ring, 2.0f * S);
	}
	DrawRect(Ring, P.X - 1.5f * S, P.Y - 1.5f * S, 3.0f * S, 3.0f * S);
}
