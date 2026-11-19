#include "SRigExecStack.h"

#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "RigExecCharacter.h"
#include "RigExecComponent.h"
#include "RigExecStackModel.h"
#include "Selection.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SToolTip.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "RigExecStack"

const FName SRigExecStack::TabId("RigExecStack");

namespace
{
TWeakPtr<SRigExecStack> GDocked;

// Column widths, shared by each list's heading and its rows.
constexpr float GNumberWidth = 44.0f;
constexpr float GTypeWidth = 136.0f;
constexpr float GLayerWidth = 170.0f;
constexpr float GWritesWidth = 230.0f;

FText
Num(int64 Value)
{
	return FText::AsNumber(Value);
}

FString
Tail(const FString& Path)
{
	int32 Slash = INDEX_NONE;
	Path.FindLastChar(TEXT('/'), Slash);
	return Slash == INDEX_NONE ? Path : Path.Mid(Slash + 1);
}

FString
Bytes(int64 Size)
{
	if (Size >= 1024 * 1024)
	{
		return FString::Printf(TEXT("%.1f MB"), Size / (1024.0 * 1024.0));
	}
	if (Size >= 1024)
	{
		return FString::Printf(TEXT("%.0f KB"), Size / 1024.0);
	}
	return FString::Printf(TEXT("%lld B"), Size);
}

TSharedRef<SWidget>
Heading(const FText& Text, float Width = 0.0f)
{
	TSharedRef<STextBlock> Block = SNew(STextBlock)
	                                   .Text(Text)
	                                   .Font(FAppStyle::GetFontStyle("SmallFontBold"))
	                                   .ColorAndOpacity(FSlateColor::UseSubduedForeground());
	if (Width <= 0.0f)
	{
		return Block;
	}
	return SNew(SBox).WidthOverride(Width)[Block];
}

TSharedRef<SWidget>
HeadingBar(TSharedRef<SWidget> Content)
{
	return SNew(SBorder)
	    .BorderImage(FAppStyle::GetBrush("Brushes.Header"))
	    .Padding(FMargin(6.0f, 3.0f))[Content];
}
} // namespace

// -- life -----------------------------------------------------------------

void
SRigExecStack::Construct(const FArguments& InArgs)
{
	bFixed = InArgs._Source != nullptr;
	Source = InArgs._Source;

	if (!bFixed)
	{
		SelectionHandle = USelection::SelectionChangedEvent.AddSP(this, &SRigExecStack::OnEditorSelection);
		SelectObjectHandle = USelection::SelectObjectEvent.AddSP(this, &SRigExecStack::OnEditorSelection);
		FContentBrowserModule& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		AssetHandle = Browser.GetOnAssetSelectionChanged().AddSP(this, &SRigExecStack::OnAssetSelection);
		Source = PickFromSelection();
		// A character asset picked in the Content Browser before the panel opened.
		TArray<FAssetData> Assets;
		Browser.Get().GetSelectedAssets(Assets);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath == URigExecCharacter::StaticClass()->GetClassPathName())
			{
				Source = Asset.GetAsset();
				break;
			}
		}
	}

	SAssignNew(MeshList, SListView<FIndexPtr>)
	    .ListItemsSource(&MeshItems)
	    .SelectionMode(ESelectionMode::Single)
	    .OnGenerateRow(this, &SRigExecStack::MakeMeshRow)
	    .OnSelectionChanged_Lambda([this](FIndexPtr Item, ESelectInfo::Type) {
		    if (Item.IsValid() && Stack.IsValid() && Stack->Meshes.IsValidIndex(*Item))
		    {
			    SelectedMesh = Stack->Meshes[*Item].Path;
			    RebuildStack();
		    }
	    });

	SAssignNew(StackTree, STreeView<FRowPtr>)
	    .TreeItemsSource(&StackItems)
	    .SelectionMode(ESelectionMode::Single)
	    .OnGenerateRow(this, &SRigExecStack::MakeStackRow)
	    .OnGetChildren_Lambda([](FRowPtr Item, TArray<FRowPtr>& Out) { Out = Item->Children; })
	    .OnContextMenuOpening_Lambda([this]() -> TSharedPtr<SWidget> {
		    const TArray<FRowPtr> Picked = StackTree->GetSelectedItems();
		    return Picked.Num() && Picked[0]->Mover != INDEX_NONE ? MoverMenu(Picked[0]->Mover) : nullptr;
	    });

	SAssignNew(LayerTree, STreeView<FIndexPtr>)
	    .TreeItemsSource(&LayerRoots)
	    .SelectionMode(ESelectionMode::Single)
	    .OnGenerateRow(this, &SRigExecStack::MakeLayerRow)
	    .OnGetChildren_Lambda([this](FIndexPtr Item, TArray<FIndexPtr>& Out) {
		    if (LayerKids.IsValidIndex(*Item))
		    {
			    Out = LayerKids[*Item];
		    }
	    })
	    .OnMouseButtonDoubleClick_Lambda([this](FIndexPtr Item) { ShowLayerMovers(*Item); })
	    .OnContextMenuOpening(this, &SRigExecStack::LayerMenu);

	SAssignNew(ExecList, SListView<FIndexPtr>)
	    .ListItemsSource(&ExecItems)
	    .SelectionMode(ESelectionMode::Single)
	    .OnGenerateRow(this, &SRigExecStack::MakeExecRow)
	    .OnContextMenuOpening_Lambda([this]() -> TSharedPtr<SWidget> {
		    const TArray<FIndexPtr> Picked = ExecList->GetSelectedItems();
		    return Picked.Num() ? MoverMenu(*Picked[0]) : nullptr;
	    });

	// The deformer view: meshes on the left, the chosen mesh's stack on the right.
	TSharedRef<SWidget> Deformers =
		SNew(SSplitter)
		+ SSplitter::Slot().Value(0.27f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				HeadingBar(SNew(SHorizontalBox)
				           + SHorizontalBox::Slot().FillWidth(1.0f)[Heading(LOCTEXT("MeshCol", "MESH"))]
				           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("DeformersCol", "DEFORMERS"))])
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)[MeshList.ToSharedRef()]
		]
		+ SSplitter::Slot().Value(0.73f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				HeadingBar(SNew(SHorizontalBox)
				           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("StepCol", "#"), GNumberWidth)]
				           + SHorizontalBox::Slot().FillWidth(1.0f)
				           [
					           Heading(LOCTEXT("DeformerCol", "DEFORMER   (top runs first)"))
				           ]
				           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("TypeCol", "TYPE"), GTypeWidth)]
				           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("LayerCol", "FROM LAYER"), GLayerWidth)])
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)[StackTree.ToSharedRef()]
		];

	TSharedRef<SWidget> Layers =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			HeadingBar(SNew(SHorizontalBox)
			           + SHorizontalBox::Slot().FillWidth(1.0f)
			           [
				           Heading(LOCTEXT("LayerNameCol", "LAYER   (strongest first)"))
			           ]
			           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("MoversCol", "MOVERS"), 150.0f)]
			           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("PrimsCol", "PRIMS"), 64.0f)]
			           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("SizeCol", "SIZE"), 72.0f)]
			           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("ModifiedCol", "ON DISK"), 120.0f)])
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)[LayerTree.ToSharedRef()];

	TSharedRef<SWidget> Execution =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("ShowLabel", "Show")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SSegmentedControl<EFamilyFilter>)
				.Value_Lambda([this]() { return Family; })
				.OnValueChanged_Lambda([this](EFamilyFilter Value) {
					Family = Value;
					RebuildExecution();
				})
				+ SSegmentedControl<EFamilyFilter>::Slot(EFamilyFilter::All).Text(LOCTEXT("FamilyAll", "All"))
				+ SSegmentedControl<EFamilyFilter>::Slot(EFamilyFilter::Deformers).Text(LOCTEXT("FamilyDeformers", "Deformers"))
				+ SSegmentedControl<EFamilyFilter>::Slot(EFamilyFilter::Constraints).Text(LOCTEXT("FamilyConstraints", "Constraints"))
				+ SSegmentedControl<EFamilyFilter>::Slot(EFamilyFilter::Math).Text(LOCTEXT("FamilyMath", "Math"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			HeadingBar(SNew(SHorizontalBox)
			           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("OrdCol", "STEP"), GNumberWidth)]
			           + SHorizontalBox::Slot().FillWidth(1.0f)[Heading(LOCTEXT("NameCol", "MOVER   (first runs first)"))]
			           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("TypeCol2", "TYPE"), GTypeWidth)]
			           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("WritesCol", "WRITES"), GWritesWidth)]
			           + SHorizontalBox::Slot().AutoWidth()[Heading(LOCTEXT("LayerCol2", "FROM LAYER"), GLayerWidth)])
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)[ExecList.ToSharedRef()];


	ChildSlot
	[
		SNew(SVerticalBox)
		// Which character, and where it came from.
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(10.0f, 8.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(this, &SRigExecStack::TitleText).Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(this, &SRigExecStack::SubtitleText)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SCheckBox)
					.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
					.Visibility(bFixed ? EVisibility::Collapsed : EVisibility::Visible)
					.ToolTipText(LOCTEXT("FollowTip", "On: show whichever RigExec character is selected in the level or the "
					                                  "Content Browser. Off: keep showing this one."))
					.IsChecked_Lambda([this]() { return bFollow ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State) {
						bFollow = State == ECheckBoxState::Checked;
						if (bFollow)
						{
							if (const UObject* Picked = PickFromSelection())
							{
								SetSource(Picked);
							}
						}
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 2.0f)
						[
							SNew(SImage)
							.Image_Lambda([this]() { return FAppStyle::GetBrush(bFollow ? "Icons.Unlock" : "Icons.Lock"); })
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 2.0f, 6.0f, 2.0f)
						[
							SNew(STextBlock).Text_Lambda([this]() {
								return bFollow ? LOCTEXT("Following", "Follows selection") : LOCTEXT("Pinned", "Pinned");
							})
						]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("RefreshTip", "Read the stack again and recheck the layer files on disk."))
					.OnClicked_Lambda([this]() {
						Reload();
						return FReply::Handled();
					})
					[
						SNew(SImage).Image(FAppStyle::GetBrush("Icons.Refresh")).ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
		]
		// Layers that changed after the bake: what is drawn is older than the files.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.BorderBackgroundColor(FLinearColor(0.9f, 0.6f, 0.1f, 1.0f))
			.Padding(FMargin(8.0f, 6.0f))
			.Visibility_Lambda([this]() {
				return Stack.IsValid() && Stack->ChangedLayerCount() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 1.0f, 8.0f, 0.0f)
				[
					SNew(SImage).Image(FAppStyle::GetBrush("Icons.WarningWithColor"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(STextBlock).Text(this, &SRigExecStack::BannerText).AutoWrapText(true)
				]
			]
		]
		// The views, and one search for all of them.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SSegmentedControl<EView>)
				.Value_Lambda([this]() { return View; })
				.OnValueChanged_Lambda([this](EView Value) { View = Value; })
				+ SSegmentedControl<EView>::Slot(EView::Deformers).Text(LOCTEXT("ViewDeformers", "Deformer Stack")).ToolTip(LOCTEXT("ViewDeformersTip", "Each mesh's deformers, in the order they run."))
				+ SSegmentedControl<EView>::Slot(EView::Layers).Text(LOCTEXT("ViewLayers", "Layer Stack")).ToolTip(LOCTEXT("ViewLayersTip", "The USD layers the character is composed from, strongest first."))
				+ SSegmentedControl<EView>::Slot(EView::Execution).Text(LOCTEXT("ViewExecution", "Execution Order")).ToolTip(LOCTEXT("ViewExecutionTip", "Every mover in the rig (constraints, math and deformers), in the "
				                                         "order they run."))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SAssignNew(SearchBox, SSearchBox)
				.HintText(LOCTEXT("SearchHint", "Filter by name, type, scope or layer (e.g. lip, Skin, Biped_body_psd)"))
				.OnTextChanged_Lambda([this](const FText& Text) {
					Search = Text.ToString().TrimStartAndEnd();
					Rebuild();
				})
			]
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex(this, &SRigExecStack::BodyIndex)
			+ SWidgetSwitcher::Slot()[Deformers]
			+ SWidgetSwitcher::Slot()[Layers]
			+ SWidgetSwitcher::Slot()[Execution]
			+ SWidgetSwitcher::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SBox).MaxDesiredWidth(460.0f)
				[
					SNew(STextBlock)
					.Text(this, &SRigExecStack::EmptyText)
					.AutoWrapText(true)
					.Justification(ETextJustify::Center)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 4.0f, 2.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(this, &SRigExecStack::FooterText)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		]
	];

	Reload();
}

SRigExecStack::~SRigExecStack()
{
	USelection::SelectionChangedEvent.Remove(SelectionHandle);
	USelection::SelectObjectEvent.Remove(SelectObjectHandle);
	if (FContentBrowserModule* Browser = FModuleManager::GetModulePtr<FContentBrowserModule>("ContentBrowser"))
	{
		Browser->GetOnAssetSelectionChanged().Remove(AssetHandle);
	}
}

void
SRigExecStack::SetSource(const UObject* InSource)
{
	if (Source.Get() == InSource && Stack.IsValid())
	{
		return;
	}
	Source = InSource;
	Reload();
}

void
SRigExecStack::Reload()
{
	Stack.Reset();
	Reason.Reset();
	if (const UObject* Object = Source.Get())
	{
		Stack = FRigExecStack::For(Object, Reason);
		if (Stack.IsValid())
		{
			Stack->RefreshChanged();
		}
	}
	Rebuild();
}

// -- selection ------------------------------------------------------------

const UObject*
SRigExecStack::PickFromSelection() const
{
	if (!GEditor)
	{
		return nullptr;
	}
	for (FSelectionIterator It(*GEditor->GetSelectedComponents()); It; ++It)
	{
		if (const URigExecComponent* Rig = Cast<URigExecComponent>(*It))
		{
			return Rig;
		}
	}
	for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
	{
		const AActor* Actor = Cast<AActor>(*It);
		if (Actor && Actor->FindComponentByClass<URigExecComponent>())
		{
			return Actor;
		}
	}
	// Nothing selected: the one already showing, else the level's first character.
	if (Source.IsValid())
	{
		return Source.Get();
	}
	if (UWorld* World = GEditor->GetEditorWorldContext().World())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->FindComponentByClass<URigExecComponent>())
			{
				return *It;
			}
		}
	}
	return nullptr;
}

void
SRigExecStack::OnEditorSelection(UObject*)
{
	if (!bFixed && bFollow)
	{
		if (const UObject* Picked = PickFromSelection())
		{
			SetSource(Picked);
		}
	}
}

void
SRigExecStack::OnAssetSelection(const TArray<FAssetData>& Assets, bool)
{
	if (bFixed || !bFollow)
	{
		return;
	}
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetClassPath == URigExecCharacter::StaticClass()->GetClassPathName())
		{
			if (const UObject* Loaded = Asset.GetAsset())
			{
				SetSource(Loaded);
			}
			return;
		}
	}
}

// -- the lists --------------------------------------------------------------

bool
SRigExecStack::Matches(const FRigExecStackMover& Mover) const
{
	if (Search.IsEmpty())
	{
		return true;
	}
	return Mover.Name.Contains(Search) || Mover.Type.Contains(Search) || Mover.Group.Contains(Search) ||
	       Stack->LayerName(Mover.Layer).Contains(Search) ||
	       Mover.Over.ContainsByPredicate([this](int32 L) { return Stack->LayerName(L).Contains(Search); });
}

void
SRigExecStack::Rebuild()
{
	RebuildMeshes();
	RebuildStack();
	RebuildLayers();
	RebuildExecution();
}

void
SRigExecStack::RebuildMeshes()
{
	MeshItems.Reset();
	FIndexPtr Keep;
	if (Stack.IsValid())
	{
		for (int32 Index = 0; Index < Stack->Meshes.Num(); ++Index)
		{
			MeshItems.Add(MakeShared<int32>(Index));
			if (Stack->Meshes[Index].Path == SelectedMesh)
			{
				Keep = MeshItems.Last();
			}
		}
		if (!Keep && MeshItems.Num())
		{
			// The mesh with the most deformers (the body) first.
			Keep = MeshItems[0];
			SelectedMesh = Stack->Meshes[0].Path;
		}
	}
	if (MeshList.IsValid())
	{
		MeshList->RequestListRefresh();
		if (Keep)
		{
			MeshList->SetSelection(Keep, ESelectInfo::Direct);
		}
	}
}

void
SRigExecStack::RebuildStack()
{
	StackItems.Reset();
	ShownMovers = 0;
	const FRigExecStackMesh* Mesh =
		Stack.IsValid() ? Stack->Meshes.FindByPredicate([this](const FRigExecStackMesh& M) { return M.Path == SelectedMesh; })
		                : nullptr;
	if (Mesh)
	{
		// One heading per run of movers from the same scope, in run order: a
		// scope can come back later in the stack, and that is worth seeing.
		for (int32 Step = 0; Step < Mesh->Movers.Num(); ++Step)
		{
			const int32 Index = Mesh->Movers[Step];
			const FRigExecStackMover& Mover = Stack->Movers[Index];
			if (!Matches(Mover))
			{
				continue;
			}
			if (StackItems.Num() == 0 || StackItems.Last()->Group != Mover.Group)
			{
				FRowPtr Group = MakeShared<FRow>();
				Group->Group = Mover.Group;
				StackItems.Add(Group);
			}
			FRowPtr Row = MakeShared<FRow>();
			Row->Group = Mover.Group;
			Row->Mover = Index;
			Row->Step = Step + 1;
			StackItems.Last()->Children.Add(Row);
			StackItems.Last()->Count++;
			++ShownMovers;
		}
	}
	if (StackTree.IsValid())
	{
		StackTree->RequestTreeRefresh();
		for (const FRowPtr& Group : StackItems)
		{
			StackTree->SetItemExpansion(Group, true);
		}
	}
}

void
SRigExecStack::RebuildLayers()
{
	LayerRoots.Reset();
	LayerItems.Reset();
	LayerKids.Reset();
	if (Stack.IsValid())
	{
		for (int32 Index = 0; Index < Stack->Layers.Num(); ++Index)
		{
			LayerItems.Add(MakeShared<int32>(Index));
		}
		LayerKids.SetNum(Stack->Layers.Num());
		for (int32 Index = 0; Index < Stack->Layers.Num(); ++Index)
		{
			const int32 Parent = Stack->Layers[Index].Parent;
			(Parent == INDEX_NONE ? LayerRoots : LayerKids[Parent]).Add(LayerItems[Index]);
		}
	}
	if (LayerTree.IsValid())
	{
		LayerTree->RequestTreeRefresh();
		for (const FIndexPtr& Item : LayerItems)
		{
			LayerTree->SetItemExpansion(Item, true);
		}
	}
}

void
SRigExecStack::RebuildExecution()
{
	ExecItems.Reset();
	if (Stack.IsValid())
	{
		for (int32 Index = 0; Index < Stack->Movers.Num(); ++Index)
		{
			const FRigExecStackMover& Mover = Stack->Movers[Index];
			const bool bFamily = Family == EFamilyFilter::All ||
			                     (Family == EFamilyFilter::Deformers && Mover.Family == ERigExecMoverFamily::Deformer) ||
			                     (Family == EFamilyFilter::Constraints && Mover.Family == ERigExecMoverFamily::Constraint) ||
			                     (Family == EFamilyFilter::Math && Mover.Family == ERigExecMoverFamily::Math);
			if (bFamily && Matches(Mover))
			{
				ExecItems.Add(MakeShared<int32>(Index));
			}
		}
	}
	if (ExecList.IsValid())
	{
		ExecList->RequestListRefresh();
	}
}

// -- rows -----------------------------------------------------------------

TSharedRef<SWidget>
SRigExecStack::TypeChip(const FRigExecStackMover& Mover)
{
	const FLinearColor Color = FRigExecStack::TypeColor(Mover);
	FString Type = Mover.Type;
	Type.RemoveFromEnd(TEXT("Mover"));
	return SNew(SBox)
	    .WidthOverride(GTypeWidth)
	    .HAlign(HAlign_Left)
	    .VAlign(VAlign_Center)
	    [
		    SNew(SBorder)
		    .BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		    .BorderBackgroundColor(Color.CopyWithNewOpacity(0.22f))
		    .Padding(FMargin(6.0f, 0.0f))
		    [
			    SNew(STextBlock).Text(FText::FromString(Type)).ColorAndOpacity(Color).Font(FAppStyle::GetFontStyle("SmallFont"))
		    ]
	    ];
}

TSharedRef<SWidget>
SRigExecStack::LayerChip(const FRigExecStackMover& Mover)
{
	FString Text = Stack->LayerName(Mover.Layer);
	if (Text.IsEmpty())
	{
		Text = TEXT("-");
	}
	if (Mover.Over.Num() > 0)
	{
		Text += FString::Printf(TEXT("  +%d"), Mover.Over.Num());
	}
	const bool bChanged = Stack->Layers.IsValidIndex(Mover.Layer) && Stack->Layers[Mover.Layer].bChanged;
	return SNew(SBox)
	    .WidthOverride(GLayerWidth)
	    .VAlign(VAlign_Center)
	    [
		    SNew(STextBlock)
		    .Text(FText::FromString(Text))
		    .HighlightText_Lambda([this]() { return FText::FromString(Search); })
		    .ColorAndOpacity(bChanged ? FSlateColor(FLinearColor(0.95f, 0.7f, 0.2f)) : FSlateColor::UseSubduedForeground())
		    .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
	    ];
}

TSharedRef<IToolTip>
SRigExecStack::MoverToolTip(const FRigExecStackMover& Mover)
{
	FString Text = Mover.Path;
	Text += FString::Printf(TEXT("\n\n%s, step %d of %d in the rig."), *Mover.Type, Mover.Ordinal + 1, Stack->Movers.Num());
	if (!Mover.bEnabled)
	{
		Text += TEXT("\nDisabled (inputs:enabled is off): it passes its input through.");
	}
	if (Mover.Targets.Num())
	{
		Text += TEXT("\n\nWrites:");
		for (int32 I = 0; I < FMath::Min(Mover.Targets.Num(), 6); ++I)
		{
			Text += TEXT("\n  ") + Mover.Targets[I];
		}
		if (Mover.Targets.Num() > 6)
		{
			Text += FString::Printf(TEXT("\n  and %d more"), Mover.Targets.Num() - 6);
		}
	}
	if (Stack->Layers.IsValidIndex(Mover.Layer))
	{
		Text += TEXT("\n\nDefined in ") + Stack->Layers[Mover.Layer].Name;
		if (Stack->Layers[Mover.Layer].bChanged)
		{
			Text += TEXT(" (changed on disk since the bake)");
		}
	}
	if (Mover.Over.Num())
	{
		TArray<FString> Names;
		for (int32 L : Mover.Over)
		{
			Names.Add(Stack->LayerName(L));
		}
		Text += TEXT("\nOverridden in ") + FString::Join(Names, TEXT(", "));
	}
	return SNew(SToolTip).Text(FText::FromString(Text));
}

TSharedRef<SWidget>
SRigExecStack::MoverCells(const FRigExecStackMover& Mover, const FText& Number, bool bShowWrites)
{
	const FLinearColor Color = FRigExecStack::TypeColor(Mover);
	const FSlateColor NameColor = Mover.bEnabled ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();
	FString Writes;
	if (bShowWrites && Mover.Targets.Num())
	{
		Writes = Tail(Mover.Targets[0]);
		if (Mover.Targets.Num() > 1)
		{
			Writes += FString::Printf(TEXT("  +%d"), Mover.Targets.Num() - 1);
		}
	}
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
	    .ToolTip(MoverToolTip(Mover))
	    + SHorizontalBox::Slot().AutoWidth()
	    [
		    SNew(SBox).WidthOverride(GNumberWidth).VAlign(VAlign_Center)
		    [
			    SNew(STextBlock).Text(Number).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Font(FAppStyle::GetFontStyle("SmallFont"))
		    ]
	    ]
	    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 2.0f, 6.0f, 2.0f)
	    [
		    SNew(SBox).WidthOverride(3.0f)
		    [
			    SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(Color.CopyWithNewOpacity(Mover.bEnabled ? 1.0f : 0.35f))
		    ]
	    ]
	    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
	    [
		    SNew(STextBlock)
		    .Text(FText::FromString(Mover.bEnabled ? Mover.Name : Mover.Name + TEXT("   (off)")))
		    .HighlightText_Lambda([this]() { return FText::FromString(Search); })
		    .ColorAndOpacity(NameColor)
		    .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
	    ]
	    + SHorizontalBox::Slot().AutoWidth()[TypeChip(Mover)];
	if (bShowWrites)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(GWritesWidth)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Writes))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		];
	}
	Row->AddSlot().AutoWidth()[LayerChip(Mover)];
	return Row;
}

TSharedRef<ITableRow>
SRigExecStack::MakeMeshRow(FIndexPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	const FRigExecStackMesh& Mesh = Stack->Meshes[*Item];
	int32 Matching = 0;
	for (int32 Index : Mesh.Movers)
	{
		Matching += Matches(Stack->Movers[Index]) ? 1 : 0;
	}
	const FText Count = Search.IsEmpty() ? Num(Mesh.Movers.Num())
	                                     : FText::Format(LOCTEXT("MeshCount", "{0} / {1}"), Num(Matching), Num(Mesh.Movers.Num()));
	return SNew(STableRow<FIndexPtr>, Owner)
	    .Padding(FMargin(6.0f, 3.0f))
	    .ToolTipText(FText::FromString(Mesh.Path))
	    [
		    SNew(SHorizontalBox)
		    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		    [
			    SNew(STextBlock)
			    .Text(FText::FromString(Mesh.Name))
			    .ColorAndOpacity(Matching > 0 || Search.IsEmpty() ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground())
			    .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
		    ]
		    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
		    [
			    SNew(STextBlock).Text(Count).ColorAndOpacity(FSlateColor::UseSubduedForeground())
		    ]
	    ];
}

TSharedRef<ITableRow>
SRigExecStack::MakeStackRow(FRowPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	if (Item->Mover == INDEX_NONE)
	{
		// A scope heading: where these deformers live, and how many.
		return SNew(STableRow<FRowPtr>, Owner)
		    .Padding(FMargin(2.0f, 3.0f))
		    [
			    SNew(SHorizontalBox)
			    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			    [
				    SNew(STextBlock).Text(FText::FromString(Item->Group)).Font(FAppStyle::GetFontStyle("NormalFontBold"))
			    ]
			    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			    [
				    SNew(STextBlock)
				    .Text(FText::Format(LOCTEXT("GroupCount", "{0} {0}|plural(one=deformer,other=deformers)"), Item->Count))
				    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
			    ]
		    ];
	}
	return SNew(STableRow<FRowPtr>, Owner)
	    .Padding(FMargin(0.0f, 2.0f))
	    [MoverCells(Stack->Movers[Item->Mover], Num(Item->Step), false)];
}

TSharedRef<ITableRow>
SRigExecStack::MakeExecRow(FIndexPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	const FRigExecStackMover& Mover = Stack->Movers[*Item];
	return SNew(STableRow<FIndexPtr>, Owner)
	    .Padding(FMargin(4.0f, 2.0f))
	    [MoverCells(Mover, Num(Mover.Ordinal + 1), true)];
}

TSharedRef<ITableRow>
SRigExecStack::MakeLayerRow(FIndexPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	const FRigExecStackLayer& Layer = Stack->Layers[*Item];
	FString Movers;
	if (Layer.Defines || Layer.Overrides)
	{
		Movers = Layer.Overrides ? FString::Printf(TEXT("%d new, %d edited"), Layer.Defines, Layer.Overrides)
		                         : FString::Printf(TEXT("%d new"), Layer.Defines);
	}
	const FDateTime Stamp = FDateTime::FromUnixTimestamp(Layer.MTime);
	FString Status;
	const FSlateBrush* Icon = nullptr;
	if (Layer.bMissing)
	{
		Status = TEXT("missing");
		Icon = FAppStyle::GetBrush("Icons.ErrorWithColor");
	}
	else if (Layer.bChanged)
	{
		Status = TEXT("changed since bake");
		Icon = FAppStyle::GetBrush("Icons.WarningWithColor");
	}
	else
	{
		Status = Layer.MTime ? Stamp.ToString(TEXT("%Y-%m-%d %H:%M")) : FString();
	}
	FString Tip = Layer.File;
	if (Layer.bChanged)
	{
		Tip += TEXT("\n\nThis file changed after the character was baked, so Unreal does not show its latest edits yet.");
	}
	Tip += TEXT("\n\nDouble-click to filter the stacks to the movers this layer adds or edits.");
	return SNew(STableRow<FIndexPtr>, Owner)
	    .Padding(FMargin(0.0f, 2.0f))
	    .ToolTipText(FText::FromString(Tip))
	    [
		    SNew(SHorizontalBox)
		    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		    [
			    SNew(SHorizontalBox)
			    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			    [
				    SNew(SImage)
				    .Image(FAppStyle::GetBrush(Layer.Depth == 0 ? "Icons.Layout" : "LevelEditor.Tabs.Layers"))
				    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
			    ]
			    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			    [
				    SNew(STextBlock)
				    .Text(FText::FromString(Layer.Name))
				    .HighlightText_Lambda([this]() { return FText::FromString(Search); })
				    .Font(FAppStyle::GetFontStyle(Layer.Defines || Layer.Overrides ? "NormalFontBold" : "NormalFont"))
			    ]
		    ]
		    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		    [
			    SNew(SBox).WidthOverride(150.0f)[SNew(STextBlock).Text(FText::FromString(Movers))]
		    ]
		    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		    [
			    SNew(SBox).WidthOverride(64.0f)
			    [
				    SNew(STextBlock).Text(Layer.PrimSpecs ? Num(Layer.PrimSpecs) : FText::GetEmpty()).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			    ]
		    ]
		    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		    [
			    SNew(SBox).WidthOverride(72.0f)
			    [
				    SNew(STextBlock).Text(FText::FromString(Layer.Size ? Bytes(Layer.Size) : FString())).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			    ]
		    ]
		    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		    [
			    SNew(SBox).WidthOverride(120.0f)
			    [
				    SNew(SHorizontalBox)
				    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
				    [
					    SNew(SImage).Image(Icon).Visibility(Icon ? EVisibility::Visible : EVisibility::Collapsed)
				    ]
				    + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				    [
					    SNew(STextBlock)
					    .Text(FText::FromString(Status))
					    .ColorAndOpacity(Layer.bChanged ? FSlateColor(FLinearColor(0.95f, 0.7f, 0.2f)) : FSlateColor::UseSubduedForeground())
				    ]
			    ]
		    ]
	    ];
}

// -- menus ----------------------------------------------------------------

TSharedPtr<SWidget>
SRigExecStack::MoverMenu(int32 Index)
{
	if (!Stack.IsValid() || !Stack->Movers.IsValidIndex(Index))
	{
		return nullptr;
	}
	const FRigExecStackMover Mover = Stack->Movers[Index];
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("CopyPath", "Copy Prim Path"), LOCTEXT("CopyPathTip", "The mover's USD path, to find it in usdview."),
	                  FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
	                  FUIAction(FExecuteAction::CreateLambda([Mover]() { FPlatformApplicationMisc::ClipboardCopy(*Mover.Path); })));
	if (Mover.Targets.Num())
	{
		Menu.AddMenuEntry(LOCTEXT("CopyTargets", "Copy What It Writes"), FText::GetEmpty(), FSlateIcon(),
		                  FUIAction(FExecuteAction::CreateLambda([Mover]() {
			                  FPlatformApplicationMisc::ClipboardCopy(*FString::Join(Mover.Targets, TEXT("\n")));
		                  })));
	}
	if (View != EView::Execution)
	{
		Menu.AddMenuEntry(LOCTEXT("ShowInOrder", "Show in Execution Order"),
		                  LOCTEXT("ShowInOrderTip", "What runs just before and after it, across the whole rig."), FSlateIcon(),
		                  FUIAction(FExecuteAction::CreateLambda([this, Index]() { ShowInExecution(Index); })));
	}
	if (Stack->Layers.IsValidIndex(Mover.Layer))
	{
		const int32 Layer = Mover.Layer;
		Menu.AddMenuEntry(FText::Format(LOCTEXT("ShowLayer", "Show Everything From {0}"), FText::FromString(Stack->Layers[Layer].Name)),
		                  FText::GetEmpty(), FSlateIcon(),
		                  FUIAction(FExecuteAction::CreateLambda([this, Layer]() { ShowLayerMovers(Layer); })));
	}
	return Menu.MakeWidget();
}

TSharedPtr<SWidget>
SRigExecStack::LayerMenu()
{
	const TArray<FIndexPtr> Picked = LayerTree->GetSelectedItems();
	if (!Stack.IsValid() || Picked.Num() == 0 || !Stack->Layers.IsValidIndex(*Picked[0]))
	{
		return nullptr;
	}
	const int32 Index = *Picked[0];
	const FString File = Stack->Layers[Index].File;
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(LOCTEXT("LayerMovers", "Show Its Movers"),
	                  LOCTEXT("LayerMoversTip", "Filter the stacks to the movers this layer adds or edits."), FSlateIcon(),
	                  FUIAction(FExecuteAction::CreateLambda([this, Index]() { ShowLayerMovers(Index); })));
	Menu.AddMenuEntry(LOCTEXT("CopyFile", "Copy File Path"), FText::GetEmpty(),
	                  FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
	                  FUIAction(FExecuteAction::CreateLambda([File]() { FPlatformApplicationMisc::ClipboardCopy(*File); })));
	Menu.AddMenuEntry(LOCTEXT("Explore", "Show in Explorer"), FText::GetEmpty(), FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.FolderOpen"),
	                  FUIAction(FExecuteAction::CreateLambda([File]() { FPlatformProcess::ExploreFolder(*File); }),
	                            FCanExecuteAction::CreateLambda([File]() { return FPaths::FileExists(File); })));
	return Menu.MakeWidget();
}

void
SRigExecStack::ShowInExecution(int32 Index)
{
	Family = EFamilyFilter::All;
	Search.Reset();
	if (SearchBox.IsValid())
	{
		SearchBox->SetText(FText::GetEmpty());
	}
	View = EView::Execution;
	Rebuild();
	for (const FIndexPtr& Item : ExecItems)
	{
		if (*Item == Index)
		{
			ExecList->SetSelection(Item);
			ExecList->RequestScrollIntoView(Item);
			break;
		}
	}
}

void
SRigExecStack::ShowLayerMovers(int32 Layer)
{
	if (!Stack.IsValid() || !Stack->Layers.IsValidIndex(Layer))
	{
		return;
	}
	const FString Name = Stack->Layers[Layer].Name;
	if (SearchBox.IsValid())
	{
		SearchBox->SetText(FText::FromString(Name));
	}
	Search = Name;
	// The deformers first, when this layer has any on the mesh showing; else
	// everything it touches.
	View = EView::Execution;
	for (const FRigExecStackMesh& Mesh : Stack->Meshes)
	{
		for (int32 Index : Mesh.Movers)
		{
			if (Matches(Stack->Movers[Index]))
			{
				View = EView::Deformers;
				if (!Mesh.Path.Equals(SelectedMesh))
				{
					// Stay on the current mesh if it has some; else the first that does.
					const FRigExecStackMesh* Current = Stack->Meshes.FindByPredicate(
						[this](const FRigExecStackMesh& M) { return M.Path == SelectedMesh; });
					const bool bCurrentHas = Current && Current->Movers.ContainsByPredicate(
						[this](int32 I) { return Matches(Stack->Movers[I]); });
					if (!bCurrentHas)
					{
						SelectedMesh = Mesh.Path;
					}
				}
				break;
			}
		}
		if (View == EView::Deformers)
		{
			break;
		}
	}
	Family = EFamilyFilter::All;
	Rebuild();
}

// -- text -----------------------------------------------------------------

int32
SRigExecStack::BodyIndex() const
{
	return Stack.IsValid() ? int32(View) : 3;
}

FText
SRigExecStack::TitleText() const
{
	const UObject* Object = Source.Get();
	return Object ? FText::FromString(FRigExecStack::DisplayName(Object)) : LOCTEXT("NoCharacter", "No RigExec character");
}

FText
SRigExecStack::SubtitleText() const
{
	if (!Stack.IsValid())
	{
		return FText::GetEmpty();
	}
	FDateTime When;
	const FString Exported = FDateTime::ParseIso8601(*Stack->Exported, When) ? When.ToString(TEXT("%Y-%m-%d %H:%M")) : Stack->Exported;
	return FText::Format(LOCTEXT("Subtitle", "{0}   \u00B7   rig {1}   \u00B7   stack read {2}"), FText::FromString(FPaths::GetCleanFilename(Stack->Stage)),
	                     FText::FromString(Stack->RigRoot), FText::FromString(Exported));
}

FText
SRigExecStack::BannerText() const
{
	if (!Stack.IsValid())
	{
		return FText::GetEmpty();
	}
	TArray<FString> Names;
	for (const FRigExecStackLayer& Layer : Stack->Layers)
	{
		if (Layer.bChanged)
		{
			Names.Add(Layer.Name);
		}
	}
	const FString List = Names.Num() > 4 ? FString::Join(TArray<FString>(Names.GetData(), 4), TEXT(", ")) + FString::Printf(TEXT(" and %d more"), Names.Num() - 4)
	                                     : FString::Join(Names, TEXT(", "));
	return FText::Format(LOCTEXT("Banner", "{0} {0}|plural(one=layer has,other=layers have) changed on disk since this character was baked: "
	                                       "{1}. What Unreal draws is the older rig; run the prepare script and bake the character "
	                                       "again to bring the changes in."),
	                     Names.Num(), FText::FromString(List));
}

FText
SRigExecStack::EmptyText() const
{
	if (!Source.IsValid())
	{
		return LOCTEXT("EmptyNone", "Select a RigExec character in the level, or a RigExec character asset in the Content "
		                            "Browser, to see its deformer and layer stacks.");
	}
	return FText::Format(LOCTEXT("EmptyReason", "No stack to show: {0}."), FText::FromString(Reason));
}

FText
SRigExecStack::FooterText() const
{
	if (!Stack.IsValid())
	{
		return FText::GetEmpty();
	}
	switch (View)
	{
	case EView::Deformers:
	{
		const FRigExecStackMesh* Mesh = Stack->Meshes.FindByPredicate([this](const FRigExecStackMesh& M) { return M.Path == SelectedMesh; });
		if (!Mesh)
		{
			return LOCTEXT("NoMeshes", "No mesh in this rig has deformers.");
		}
		return FText::Format(LOCTEXT("DeformerFooter", "{0}: {1} of {2} deformers shown   \u00B7   {3} deformed meshes   \u00B7   hover a row for its "
		                                               "path and layers; right-click to copy it"),
		                     FText::FromString(Mesh->Name), ShownMovers, Mesh->Movers.Num(), Stack->Meshes.Num());
	}
	case EView::Layers:
	{
		const int32 Changed = Stack->ChangedLayerCount();
		return FText::Format(LOCTEXT("LayerFooter", "{0} layers   \u00B7   {1} changed since the bake   \u00B7   double-click a layer to see its movers"),
		                     Stack->Layers.Num(), Changed);
	}
	default:
	{
		FText Skipped = FText::GetEmpty();
		if (Stack->Skipped.Num())
		{
			Skipped = FText::Format(LOCTEXT("Skipped", "   \u00B7   {0} set aside by the compile (broken)"), Stack->Skipped.Num());
		}
		return FText::Format(LOCTEXT("ExecFooter", "{0} of {1} movers shown   \u00B7   {2} deformers, {3} constraints, {4} math   \u00B7   {5} pose solvers{6}"),
		                     ExecItems.Num(), Stack->Movers.Num(), Stack->DeformerCount, Stack->ConstraintCount, Stack->MathCount,
		                     Stack->Solvers.Num(), Skipped);
	}
	}
}

// -- the tab --------------------------------------------------------------

TSharedRef<SDockTab>
SRigExecStack::SpawnTab()
{
	TSharedRef<SRigExecStack> Panel = SNew(SRigExecStack);
	GDocked = Panel;
	return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SBorder).BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder")).Padding(6.0f)[Panel]];
}

void
SRigExecStack::OpenTab(const UObject* Source)
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabId);
	if (TSharedPtr<SRigExecStack> Panel = GDocked.Pin())
	{
		if (Source)
		{
			Panel->SetSource(Source);
		}
	}
}

#undef LOCTEXT_NAMESPACE
