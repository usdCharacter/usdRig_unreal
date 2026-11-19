// Editor tools for RigExec characters: the control picker (docked and over
// the viewport), Touch Pose, the Stack panel and the Rig component's and
// character asset's details.
using UnrealBuildTool;

public class RigExecEditor : ModuleRules
{
	public RigExecEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore", "Slate", "SlateCore",
			"UnrealEd", "LevelEditor", "ToolMenus", "WorkspaceMenuStructure", "Json",
			"ControlRig", "RigVM", "RigExecRuntime",
			"InteractiveToolsFramework", "EditorInteractiveToolsFramework",
			"PropertyEditor", "ContentBrowser", "ApplicationCore" });
	}
}
