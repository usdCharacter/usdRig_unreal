// The game module. In the editor it adds Tools > RigExec > Animation Studio:
// opens the studio's level (L_RigExecStudio) and plays it.
#include "Modules/ModuleManager.h"

#if WITH_EDITOR
#include "Editor.h"
#include "FileHelpers.h"
#include "ToolMenus.h"
#endif

#define LOCTEXT_NAMESPACE "RigExecBiped"

class FRigExecBipedModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
#if WITH_EDITOR
		if (GIsEditor && !IsRunningCommandlet())
		{
			UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FRigExecBipedModule::AddMenu));
		}
#endif
	}

	virtual void ShutdownModule() override
	{
#if WITH_EDITOR
		if (UObjectInitialized())
		{
			UToolMenus::UnRegisterStartupCallback(this);
			UToolMenus::UnregisterOwner(this);
		}
#endif
	}

#if WITH_EDITOR
	static void OpenStudio(bool bPlay)
	{
		if (!GEditor || GEditor->PlayWorld)
		{
			return;
		}
		if (!FEditorFileUtils::SaveDirtyPackages(true, true, false))
		{
			return;
		}
		if (!FEditorFileUtils::LoadMap(TEXT("/Game/RigExec/L_RigExecStudio"), false, true))
		{
			return;
		}
		if (bPlay)
		{
			FRequestPlaySessionParams Args;
			GEditor->RequestPlaySession(Args);
		}
	}

	void AddMenu()
	{
		FToolMenuOwnerScoped Owner(this);
		UToolMenu* Tools = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Tools->FindOrAddSection("RigExec", LOCTEXT("RigExecSection", "RigExec"));
		Section.AddMenuEntry("RigExecStudioPlay", LOCTEXT("StudioPlay", "Play Animation Studio"),
		                     LOCTEXT("StudioPlayTip", "Open the animation studio level and play it: choose a character, "
		                                              "pose it with TouchPose and the hover picker, key it on the timeline."),
		                     FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]() { OpenStudio(true); })));
		Section.AddMenuEntry("RigExecStudioOpen", LOCTEXT("StudioOpen", "Open Animation Studio Level"),
		                     LOCTEXT("StudioOpenTip", "Open L_RigExecStudio (its GameMode is RigExecStudioGameMode)."),
		                     FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]() { OpenStudio(false); })));
	}
#endif
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_PRIMARY_GAME_MODULE(FRigExecBipedModule, RigExecBiped, "RigExecBiped");
