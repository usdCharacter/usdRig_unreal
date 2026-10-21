// Blueprint- and Python-callable entry points for the RigExec runtime.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "RigExecRuntimeLibrary.generated.h"

class UControlRig;
class URigExecCharacter;

UCLASS()
class RIGEXECRUNTIME_API URigExecRuntimeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Loads a .rigexec file, poses it once per Values entry on the named
	 * channel (a property path such as "/Biped/Rig/.../M_Jaw.avars:rx"), and
	 * reports what ran: whether the file carries live property chains, each
	 * pose's evaluation time, and how far the deformed points moved from
	 * the first pose. A self-test of the runtime inside Unreal's process.
	 */
	/**
	 * Opens a .rigexec with the plugin's runtime and reports whether it
	 * plays here: its inputs, any plugin movers it needs a kernel for, its
	 * outputs, and which of its number inputs move it (each is nudged by
	 * 0.5 in turn, and the points and frames compared with the rest pose).
	 * Starts with "OK" when it opens, evaluates and responds.
	 */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	static FString RigExecInspect(const FString& RigExecFile);

	UFUNCTION(BlueprintCallable, Category = "RigExec")
	static FString RigExecSelfTest(const FString& RigExecFile, const FString& Channel,
	                               const TArray<float>& Values);

	/**
	 * Editor only: turns on the Control Rig animation mode in the level
	 * viewport, hands it this rig, and makes its controls visible -- the
	 * state the Animation Outliner's eye toggles. False outside the editor.
	 */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	static bool ShowControlRigInViewport(UControlRig* ControlRig);

	/**
	 * Editor only: the Animation mode's "Controls As Overlay" setting, which
	 * draws control shapes over the meshes (most of a body rig's controls
	 * sit inside the body). Read when the mode starts, so set it before
	 * opening a sequence. False when the setting is not found.
	 */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	static bool SetControlsAsOverlay(bool bOverlay);

	/**
	 * Editor only: bakes a character into one asset at `AssetPath`
	 * ("/Game/RigExec/RXC_Biped"): the .rigexec, the controls description
	 * (with its Control Rig element names) and the picker beside it, and
	 * every posed mesh's topology, UVs, material looks and touch regions
	 * read from the stage. Relative files are relative to the project. A
	 * component loading the asset needs no USD, so it runs in a packaged
	 * game. Saves the asset; returns it, or None with the reason logged.
	 */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	static URigExecCharacter* BakeCharacter(const FString& RigExecFile, const FString& StageFile,
	                                        const FString& ControlsFile, const FString& AssetPath);
};
