#include "RigExecMenuHUD.h"

#include "CanvasItem.h"
#include "DynamicRHI.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "RenderTimer.h"
#include "RigExecBipedCharacter.h"
#include "RigExecRetargetComponent.h"
#include "UnrealClient.h"

namespace
{
const TCHAR* GSection = TEXT("RigExecBiped.Settings");
const FLinearColor GAccent(0.15f, 0.55f, 0.85f, 0.9f);
const FLinearColor GCyan(0.45f, 0.85f, 1.0f);

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
	}
	Apply();
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
ARigExecMenuHUD::Button(FName Name, const FString& Label, float X, float Y, float W, float H, float S, bool bEnabled)
{
	const bool bCursor = PlayerOwner && PlayerOwner->bShowMouseCursor;
	const FLinearColor Fill = bEnabled ? GAccent : FLinearColor(0.2f, 0.2f, 0.2f, 0.6f);
	DrawRect(Fill, X, Y, W, H);
	Text(Label, X + W * 0.5f, Y + H * 0.5f, 1.5f * S, bEnabled ? FLinearColor::White : FLinearColor(0.5f, 0.5f, 0.5f), true);
	if (bCursor && bEnabled)
	{
		AddHitBox(FVector2D(X, Y), FVector2D(W, H), Name, true);
	}
}

bool
ARigExecMenuHUD::Hovered(float X, float Y, float W, float H) const
{
	float MouseX = 0.0f, MouseY = 0.0f;
	if (!PlayerOwner || !PlayerOwner->bShowMouseCursor || !PlayerOwner->GetMousePosition(MouseX, MouseY))
	{
		return false;
	}
	return MouseX >= X && MouseX <= X + W && MouseY >= Y && MouseY <= Y + H;
}

void
ARigExecMenuHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}
	DrawGame();
	const float W = Canvas->ClipX, H = Canvas->ClipY, S = H / 720.0f;
	DrawCounters(W, H, S);
	if (IsMenuOpen())
	{
		DrawMenu(W, H, S);
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

	float Y = H - 34.0f * S;
	if (bShowFps)
	{
		const FLinearColor Color = ShownFps >= 55.0 ? FLinearColor(0.45f, 1.0f, 0.5f)
		                           : ShownFps >= 30.0 ? FLinearColor(1.0f, 0.85f, 0.3f)
		                                              : FLinearColor(1.0f, 0.4f, 0.35f);
		Text(FString::Printf(TEXT("%3.0f FPS   %5.1f ms   worst %5.1f ms"), ShownFps, ShownMs, ShownWorst), 18.0f * S, Y, 1.0f * S,
		     Color);
		Y -= 24.0f * S;
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
			Lines.Add(Biped->Retarget->Status.Left(70));
		}
	}
	const float Row = 20.0f * S;
	const float Top = Y - Row * Lines.Num() - 6.0f * S;
	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.45f), 10.0f * S, Top, 470.0f * S, Row * Lines.Num() + 10.0f * S);
	for (int32 Each = 0; Each < Lines.Num(); ++Each)
	{
		Text(Lines[Each], 18.0f * S, Top + 4.0f * S + Row * Each, 0.85f * S, FLinearColor(0.85f, 0.92f, 1.0f));
	}
}

TArray<ARigExecMenuHUD::FItem>
ARigExecMenuHUD::Items()
{
	TArray<FItem> List;
	if (Page == EPage::Main)
	{
		List.Add({TEXT("RESUME"), TEXT(""), [this] { CloseMenu(); }});
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
	Text(bSettings ? TEXT("SETTINGS") : TEXT("PAUSED"), W * 0.5f, Y + 48.0f * S, 2.2f * S, FLinearColor::White, true);

	for (int32 Each = 0; Each < List.Num(); ++Each)
	{
		const FItem& Item = List[Each];
		const float RowY = Y + 96.0f * S + RowH * Each;
		const float Inset = 24.0f * S;
		const float BoxX = X + Inset, BoxW = PanelW - 2.0f * Inset, BoxH = RowH - 8.0f * S;
		const FName Name(*FString::Printf(TEXT("Menu.%d"), Each));
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
		if (Item.bEnabled && PlayerOwner && PlayerOwner->bShowMouseCursor)
		{
			AddHitBox(FVector2D(BoxX, RowY), FVector2D(BoxW, BoxH), Name, true, 10);
		}
	}
	Text(bSettings ? TEXT("click or Enter / A to change     Esc / B back") : TEXT("Esc / P / Start  resume"), W * 0.5f,
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
ARigExecMenuHUD::OpenMenu()
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
	UGameplayStatics::SetGamePaused(this, true);
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
		Focus = 1;
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
