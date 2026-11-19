// The Stack panel: a RigExec character's deformer stack (per mesh, first
// runs first), its USD layer stack (strongest first, flagging layers that
// changed since the bake) and the rig's whole execution order.
//
// Docked (Window > RigExec Stack), it follows the selection: an actor with a
// Rig component in the level, or a character asset in the Content Browser.
// In a character asset's Details it shows that asset alone.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"

class FRigExecStack;
class SSearchBox;
struct FAssetData;
struct FRigExecStackMover;

class SRigExecStack : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRigExecStack) {}
		/** Show this source alone (a character asset, a Rig component or its
		 * actor); unset, the panel follows the editor's selection. */
		SLATE_ARGUMENT(const UObject*, Source)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SRigExecStack() override;

	/** Shows a source, as if it had been selected. */
	void SetSource(const UObject* InSource);

	/** The docked panel's tab id, and opening it on a source. */
	static const FName TabId;
	static void OpenTab(const UObject* Source = nullptr);
	static TSharedRef<class SDockTab> SpawnTab();

	/** A row of the deformer stack: a scope's heading or a mover in it. */
	struct FRow
	{
		FString Group;
		int32 Mover = INDEX_NONE;
		/** The mover's place in this mesh's stack, 1 first. */
		int32 Step = 0;
		int32 Count = 0;
		TArray<TSharedPtr<FRow>> Children;
	};
	using FRowPtr = TSharedPtr<FRow>;
	using FIndexPtr = TSharedPtr<int32>;

private:
	enum class EView : int32
	{
		Deformers,
		Layers,
		Execution,
	};
	enum class EFamilyFilter : int32
	{
		All,
		Deformers,
		Constraints,
		Math,
	};

	void Reload();
	void Rebuild();
	void RebuildMeshes();
	void RebuildStack();
	void RebuildLayers();
	void RebuildExecution();
	bool Matches(const FRigExecStackMover& Mover) const;
	const UObject* PickFromSelection() const;

	void OnEditorSelection(UObject*);
	void OnAssetSelection(const TArray<FAssetData>& Assets, bool bPrimary);

	// Header and footer.
	FText TitleText() const;
	FText SubtitleText() const;
	FText BannerText() const;
	FText FooterText() const;
	FText EmptyText() const;
	int32 BodyIndex() const;

	// Rows.
	TSharedRef<ITableRow> MakeMeshRow(FIndexPtr Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeStackRow(FRowPtr Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeLayerRow(FIndexPtr Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeExecRow(FIndexPtr Item, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> MoverCells(const FRigExecStackMover& Mover, const FText& Number, bool bShowWrites);
	TSharedRef<SWidget> TypeChip(const FRigExecStackMover& Mover);
	TSharedRef<SWidget> LayerChip(const FRigExecStackMover& Mover);
	TSharedRef<class IToolTip> MoverToolTip(const FRigExecStackMover& Mover);

	TSharedPtr<SWidget> MoverMenu(int32 Mover);
	TSharedPtr<SWidget> LayerMenu();
	void ShowInExecution(int32 Mover);
	void ShowLayerMovers(int32 Layer);

	TWeakObjectPtr<const UObject> Source;
	bool bFixed = false;
	bool bFollow = true;
	TSharedPtr<FRigExecStack> Stack;
	FString Reason;

	EView View = EView::Deformers;
	EFamilyFilter Family = EFamilyFilter::All;
	FString Search;
	FString SelectedMesh;

	TArray<FIndexPtr> MeshItems;
	TArray<FRowPtr> StackItems;
	TArray<FIndexPtr> LayerRoots;
	TArray<FIndexPtr> LayerItems;
	TArray<TArray<FIndexPtr>> LayerKids;
	TArray<FIndexPtr> ExecItems;
	int32 ShownMovers = 0;

	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SListView<FIndexPtr>> MeshList;
	TSharedPtr<STreeView<FRowPtr>> StackTree;
	TSharedPtr<STreeView<FIndexPtr>> LayerTree;
	TSharedPtr<SListView<FIndexPtr>> ExecList;

	FDelegateHandle SelectionHandle;
	FDelegateHandle SelectObjectHandle;
	FDelegateHandle AssetHandle;
};
