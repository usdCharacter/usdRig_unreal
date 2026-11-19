#include "RigExecComponentDetails.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "RigExecCharacter.h"
#include "RigExecComponent.h"
#include "RigExecStackModel.h"
#include "SRigExecStack.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "RigExecComponentDetails"

namespace
{
FDelegateHandle GFallbackHandle;

// One toast per reason per session: a level with several characters, or a
// reload, does not repeat it.
void
ToastFallback(URigExecComponent* Component, const FString& Reason)
{
	static TSet<FString> Shown;
	if (Shown.Contains(Reason))
	{
		return;
	}
	Shown.Add(Reason);
	FNotificationInfo Info(LOCTEXT("FallbackTitle", "RigExec is drawing on the CPU"));
	Info.SubText = FText::Format(LOCTEXT("FallbackBody", "{0}, so posing recomputes the meshes on the CPU and is slower. "
	                                                     "The Rig component's Details say more."),
	                             FText::FromString(Reason.Left(1).ToUpper() + Reason.Mid(1)));
	Info.Image = FAppStyle::GetBrush(TEXT("Icons.WarningWithColor"));
	Info.ExpireDuration = 10.0f;
	Info.bFireAndForget = true;
	FSlateNotificationManager::Get().AddNotification(Info);
}
} // namespace

void
FRigExecComponentDetails::Register()
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	PropertyEditor.RegisterCustomClassLayout(URigExecComponent::StaticClass()->GetFName(),
	                                         FOnGetDetailCustomizationInstance::CreateStatic(&FRigExecComponentDetails::MakeInstance));
	PropertyEditor.RegisterCustomClassLayout(URigExecCharacter::StaticClass()->GetFName(),
	                                         FOnGetDetailCustomizationInstance::CreateStatic(&FRigExecCharacterDetails::MakeInstance));
	GFallbackHandle = URigExecComponent::OnRenderFallback.AddStatic(&ToastFallback);
}

void
FRigExecComponentDetails::Unregister()
{
	URigExecComponent::OnRenderFallback.Remove(GFallbackHandle);
	if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
	{
		PropertyEditor->UnregisterCustomClassLayout(URigExecComponent::StaticClass()->GetFName());
		PropertyEditor->UnregisterCustomClassLayout(URigExecCharacter::StaticClass()->GetFName());
	}
}

void
FRigExecComponentDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);

	// The stacks in a line, and the way to the panel.
	if (Objects.Num() == 1)
	{
		if (const URigExecComponent* Rig = Cast<URigExecComponent>(Objects[0].Get()))
		{
			FString Why;
			const TSharedPtr<FRigExecStack> Stack = FRigExecStack::For(Rig, Why);
			FText Summary;
			if (Stack.IsValid())
			{
				const int32 Changed = Stack->ChangedLayerCount();
				Summary = FText::Format(LOCTEXT("StackSummary", "{0} deformed meshes, {1} deformers, {2} layers{3}"), Stack->Meshes.Num(),
				                        Stack->DeformerCount, Stack->Layers.Num(),
				                        Changed ? FText::Format(LOCTEXT("StackChanged", " ({0} changed since the bake)"), Changed)
				                                : FText::GetEmpty());
			}
			else
			{
				Summary = FText::FromString(Why);
			}
			const TWeakObjectPtr<const URigExecComponent> Weak = Rig;
			IDetailCategoryBuilder& Stacks = DetailBuilder.EditCategory(TEXT("RigExecStack"), LOCTEXT("StackCategory", "RigExec Stack"));
			Stacks.AddCustomRow(LOCTEXT("StackFilter", "Stack Deformers Layers"))
				.NameContent()
				[
					SNew(STextBlock).Text(LOCTEXT("StackName", "Deformers & Layers")).Font(IDetailLayoutBuilder::GetDetailFont())
				]
				.ValueContent()
				.MinDesiredWidth(250.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(Summary).Font(IDetailLayoutBuilder::GetDetailFont()).AutoWrapText(true)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("OpenStack", "Open Stack"))
						.ToolTipText(LOCTEXT("OpenStackTip", "Open the RigExec Stack panel on this character."))
						.OnClicked_Lambda([Weak]() {
							SRigExecStack::OpenTab(Weak.Get());
							return FReply::Handled();
						})
					]
				];
		}
	}

	FString Reason;
	bool bOff = false;
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		const URigExecComponent* Rig = Cast<URigExecComponent>(Object.Get());
		if (Rig && !Rig->IsDrawingOnGPU() && !Rig->GetFallbackReason().IsEmpty())
		{
			Reason = Rig->GetFallbackReason();
			bOff = !Rig->bDrawOnGPU;
			break;
		}
	}
	if (Reason.IsEmpty())
	{
		return;
	}
	// First thing in the panel: what is happening, why, and what to do.
	const FText Message =
		bOff ? LOCTEXT("OffMessage", "Drawing on the CPU: Draw On GPU is off. Turn it on (RigExec > Render) to upload each pose "
		                             "once and compute the normals on the GPU.")
		     : FText::Format(LOCTEXT("FallbackMessage", "Drawing on the CPU: {0}. Each pose recomputes the normals and re-uploads "
		                                                "the meshes on the CPU, which is slower; the GPU path runs on a "
		                                                "Shader Model 5 renderer (DirectX 12, Vulkan or Metal)."),
		                     FText::FromString(Reason));
	IDetailCategoryBuilder& Category =
		DetailBuilder.EditCategory(TEXT("RigExecStatus"), LOCTEXT("StatusCategory", "RigExec Status"), ECategoryPriority::Important);
	Category.AddCustomRow(LOCTEXT("StatusFilter", "Render Path CPU GPU"))
		.WholeRowContent()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
			.Padding(FMargin(8.0f, 6.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 1.0f, 8.0f, 0.0f)
				[
					SNew(SImage).Image(FAppStyle::GetBrush(TEXT("Icons.WarningWithColor")))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock).Text(Message).AutoWrapText(true)
				]
			]
		];
}

void
FRigExecCharacterDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	const URigExecCharacter* Character = Objects.Num() == 1 ? Cast<URigExecCharacter>(Objects[0].Get()) : nullptr;
	if (!Character)
	{
		return;
	}
	// The whole panel, on this asset alone, first in its Details.
	IDetailCategoryBuilder& Category =
		DetailBuilder.EditCategory(TEXT("RigExecStack"), LOCTEXT("CharacterStackCategory", "Stack"), ECategoryPriority::Important);
	Category.AddCustomRow(LOCTEXT("CharacterStackFilter", "Stack Deformers Layers Execution"))
		.WholeRowContent()
		[
			SNew(SBox)
			.HeightOverride(560.0f)
			.Padding(FMargin(0.0f, 4.0f))
			[
				SNew(SRigExecStack).Source(Character)
			]
		];
}

#undef LOCTEXT_NAMESPACE
