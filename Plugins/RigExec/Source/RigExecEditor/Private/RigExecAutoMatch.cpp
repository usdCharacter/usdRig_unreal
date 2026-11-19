#include "RigExecAutoMatch.h"

#include "Containers/Ticker.h"
#include "ControlRig.h"
#include "ControlRigComponent.h"
#include "Editor.h"
#include "Framework/Notifications/NotificationManager.h"
#include "RigExecComponent.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "RigExecAutoMatch"

namespace
{
FTSTicker::FDelegateHandle GTicker;
FDelegateHandle GUndoHandle;

/** Each character's control selection as last seen. */
TMap<TWeakObjectPtr<URigExecComponent>, TArray<FName>> GSeen;
/** An undo or redo just ran: take the selection it restored as seen, never
 * as a new pick (a redone selection would switch again and drop the redo). */
bool GSkipNext = false;

void Notify(const FText& Text)
{
	FNotificationInfo Info(Text);
	Info.ExpireDuration = 2.0f;
	Info.FadeOutDuration = 0.5f;
	FSlateNotificationManager::Get().AddNotification(Info);
}

bool Tick(float)
{
	const bool bSkip = GSkipNext || GIsTransacting;
	GSkipNext = false;
	for (auto It = GSeen.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	for (TObjectIterator<URigExecComponent> It; It; ++It)
	{
		URigExecComponent* Component = *It;
		const UWorld* World = Component ? Component->GetWorld() : nullptr;
		if (!Component || Component->IsTemplate() || !World || World->IsGameWorld() ||
		    !Component->bShowIdleLimbHalf || !Component->ControlRig)
		{
			continue;
		}
		UControlRig* Rig = Component->ControlRig->GetControlRig();
		if (!Rig)
		{
			continue;
		}
		const TArray<FName> Selected = Rig->CurrentControlSelection();
		TArray<FName>& Seen = GSeen.FindOrAdd(Component);
		if (Selected == Seen)
		{
			continue;
		}
		// The limbs a NEW pick lands on the idle half of; a control kept
		// selected never switches its limb back.
		TArray<FString> Switches;
		for (const FName& Control : Selected)
		{
			if (!bSkip && !Seen.Contains(Control))
			{
				const FString Switch = Component->FindPresentedLimbFor(Control);
				if (!Switch.IsEmpty())
				{
					Switches.AddUnique(Switch);
				}
			}
		}
		Seen = Selected;
		if (Switches.IsEmpty())
		{
			continue;
		}
		TArray<FString> Done;
		{
			const FScopedTransaction Transaction(LOCTEXT("MatchLimb", "Match Limb"));
			for (const FString& Switch : Switches)
			{
				const bool bToIk = !Component->IsLimbIk(Switch);
				FString Control;
				Switch.Split(TEXT("."), &Control, nullptr);
				Control = FPaths::GetBaseFilename(Control);
				if (Component->SwitchLimb(Switch))
				{
					Done.Add(FString::Printf(TEXT("%s to %s"), *Control, bToIk ? TEXT("IK") : TEXT("FK")));
				}
			}
		}
		Notify(Done.IsEmpty() ? LOCTEXT("MatchFailed", "IK/FK match failed.")
		                      : FText::Format(LOCTEXT("Matched", "Switched {0} (matched)"),
		                                      FText::FromString(FString::Join(Done, TEXT(", ")))));
		// The switch may change what is selected (nothing today); either way
		// what is selected now is seen.
		Seen = Rig->CurrentControlSelection();
	}
	return true;
}
} // namespace

void FRigExecAutoMatch::Startup()
{
	if (!GTicker.IsValid())
	{
		GTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
	}
	if (!GUndoHandle.IsValid())
	{
		GUndoHandle = FEditorDelegates::PostUndoRedo.AddLambda([]() { GSkipNext = true; });
	}
}

void FRigExecAutoMatch::Shutdown()
{
	FTSTicker::GetCoreTicker().RemoveTicker(GTicker);
	GTicker.Reset();
	FEditorDelegates::PostUndoRedo.Remove(GUndoHandle);
	GUndoHandle.Reset();
	GSeen.Reset();
}

#undef LOCTEXT_NAMESPACE
