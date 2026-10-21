using UnrealBuildTool;

public class RigExecBipedEditorTarget : TargetRules
{
	public RigExecBipedEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("RigExecBiped");
		ExtraModuleNames.Add("RigExecBipedLoadingScreen");
	}
}
