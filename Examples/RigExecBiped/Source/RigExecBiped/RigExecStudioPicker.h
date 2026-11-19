// The studio's hover picker: the character's picker layout (export_picker.py,
// baked into the character asset) drawn straight over the game view with no
// window around it, as usdview's hover picker draws it. The same buttons the
// editor's picker shows; here they select the studio's controls, with no
// Control Rig behind them, so it ships in a packaged game.
#pragma once

#include "CoreMinimal.h"

class UCanvas;
class URigExecComponent;

namespace RigExecStudio
{
struct FPickerButton
{
	FString Id;
	FBox2f Box;
	TArray<FVector2f> Vertices;
	TArray<uint32> Triangles;
	TArray<TArray<FVector2f>> Outlines;
	FColor Fill, Stroke, TextColor, ValueColor;
	float StrokeWidth = 1.0f;
	FString Text;
	float FontSize = 8.0f;
	/** -1 left, 0 centre, 1 right. */
	int32 Align = 0;
	bool bDecoration = false;
	/** The controls (indices) a click selects. */
	TArray<int32> Targets;
	/** It does something when clicked: selects (every target found), switches or runs a command. */
	bool bLive = false;
	/** An attribute button: the channel it switches, its value names. */
	FString ChannelPath;
	TArray<FString> Labels;
	bool bInvert = false;
	/** A space dial's own control: it stays put when the space changes. */
	int32 SwitchedControl = INDEX_NONE;
	FString Command;
	/** "ik" / "fk": the limb half this button belongs to, by the dial at DialPath. */
	FString Mode;
	FString DialPath;
	FString Mirror;
};

struct FPickerPanel
{
	FString Label;
	FVector2f Size = FVector2f(400.0f, 600.0f);
	TArray<FPickerButton> Buttons;
};

class FPicker
{
public:
	/** Reads the rig's picker layout; false (and no panels) when it has none. */
	bool Load(URigExecComponent* Rig);
	bool IsLoadedFor(const URigExecComponent* Rig) const { return Loaded.Get() == Rig && Rig; }

	TArray<FPickerPanel> Panels;

	/** How a button shows now: 0 not at all, 1 live, 2 on the limb half the
	 * limb is not driving with (drawn dim; picking it switches the limb). */
	int32 Shown(const FPickerButton& Button) const;
	/** The label of the value an attribute button's channel holds. */
	FString ValueLabel(const FPickerButton& Button) const;
	/** The topmost live button under a panel point, or INDEX_NONE. */
	int32 HitAt(int32 Panel, const FVector2f& At) const;

	/** Draws panel `Panel` with its point P at Offset + P * Scale. */
	void Draw(UCanvas* Canvas, int32 Panel, const FVector2f& Offset, float Scale, int32 Hovered,
	          const TArray<int32>& Selected, float Opacity) const;

private:
	TWeakObjectPtr<URigExecComponent> Loaded;
};
} // namespace RigExecStudio
