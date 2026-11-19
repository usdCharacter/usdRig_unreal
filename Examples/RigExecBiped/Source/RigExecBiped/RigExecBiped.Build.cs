// The example's game module: a playable RigExec biped driven by Mannequin
// animation through the RigExec retargeter, and the animation studio.
using UnrealBuildTool;

public class RigExecBiped : ModuleRules
{
	public RigExecBiped(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "RigExecRuntime",
			"RenderCore", "RHI", "Slate", "SlateCore", "AssetRegistry", "Json",
			// VR: the headset camera, the controllers (OpenXR at run time).
			"HeadMountedDisplay" });
		if (Target.bBuildEditor)
		{
			// Tools > RigExec > Animation Studio, in the editor only.
			PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "ToolMenus", "LevelEditor" });
		}
	}
}
