// What every level's HUD shares: an FPS counter, a debug stats panel, and
// the pause menu (Resume, Settings, Quit) with a settings page of testing
// switches. Esc (when not in the close-up), P or gamepad Start opens it.
// Each level's HUD draws its own things in DrawGame; this draws over them.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"

#include "RigExecMenuHUD.generated.h"

class ARigExecBipedCharacter;

UCLASS()
class RIGEXECBIPED_API ARigExecMenuHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void DrawHUD() override;
	virtual void NotifyHitBoxClick(FName BoxName) override;

	/** The menu: open pauses the game and shows the cursor. */
	bool IsMenuOpen() const { return Page != EPage::None; }
	void OpenMenu();
	void CloseMenu();
	void ToggleMenu();
	/** Esc / gamepad B in the menu: settings back to the menu, the menu shut. */
	void MenuBack();
	/** Keyboard and gamepad: move the highlight, press the highlighted item. */
	void MenuMove(int32 Step);
	void MenuSelect();

protected:
	/** The level's own HUD, under the overlay. */
	virtual void DrawGame() {}

	void Text(const FString& Line, float X, float Y, float Scale, const FLinearColor& Color, bool bCentre = false);
	/** A button: a box, its label, and a hit box when the cursor is up. */
	void Button(FName Name, const FString& Label, float X, float Y, float W, float H, float S, bool bEnabled);
	ARigExecBipedCharacter* Hero() const;
	/** The subdivision panel is wanted (a setting). */
	bool ShowSubdivisionPanel() const { return bShowSubdPanel; }

private:
	enum class EPage : uint8
	{
		None,
		Main,
		Settings
	};
	struct FItem
	{
		FString Label;
		/** Empty for a plain button; a setting's current value otherwise. */
		FString Value;
		TFunction<void()> Press;
		bool bEnabled = true;
	};
	TArray<FItem> Items();
	void DrawCounters(float W, float H, float S);
	void DrawMenu(float W, float H, float S);
	bool Hovered(float X, float Y, float W, float H) const;
	void Apply();
	void Save() const;

	EPage Page = EPage::None;
	int32 Focus = 0;
	/** The cursor as it was before the menu, to put back. */
	bool bHadCursor = false, bHadClicks = false;

	// The settings (saved, but for the ones that only make sense for now).
	bool bShowFps = true;
	bool bShowStats = false;
	bool bShowSubdPanel = true;
	bool bEngineStats = false;
	bool bVSync = false;
	int32 FrameCap = 0;
	int32 ScreenPercent = 100;
	float TimeScale = 1.0f;

	// The FPS counter, smoothed and refreshed a few times a second.
	double SmoothDelta = 1.0 / 60.0;
	double WorstDelta = 0.0, WorstSince = 0.0;
	double ShownFps = 0.0, ShownMs = 0.0, ShownWorst = 0.0, NextShow = 0.0;
};
