// The packaged game: the biped running around, posed by the RigExec runtime
// from its baked character asset (no USD).
using UnrealBuildTool;

public class RigExecBipedTarget : TargetRules
{
	public RigExecBipedTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("RigExecBiped");
		ExtraModuleNames.Add("RigExecBipedLoadingScreen");
	}
}
