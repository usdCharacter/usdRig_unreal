// The example's game module: a playable RigExec biped driven by Mannequin
// animation through the RigExec retargeter.
using UnrealBuildTool;

public class RigExecBiped : ModuleRules
{
	public RigExecBiped(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "RigExecRuntime",
			"RenderCore", "RHI", "Slate", "SlateCore" });
	}
}
