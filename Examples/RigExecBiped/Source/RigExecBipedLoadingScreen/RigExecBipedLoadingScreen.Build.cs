// The game's loading screen: up from the moment the window opens until the
// first level, biped and all, has loaded. Loaded before the engine starts,
// so it uses no assets -- its picture is a loose PNG in Content/Splash.
using UnrealBuildTool;

public class RigExecBipedLoadingScreen : ModuleRules
{
	public RigExecBipedLoadingScreen(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "ImageWrapper", "MoviePlayer", "Slate", "SlateCore" });
	}
}
