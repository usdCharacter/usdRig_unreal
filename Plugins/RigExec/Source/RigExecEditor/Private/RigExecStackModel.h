// A character's layer stack and deformer stack, as export_stack.py wrote
// them (baked into URigExecCharacter::Stack, or the <rig>_stack.json beside
// a component's .rigexec). Plain data for the Stack panel; no Slate here.
#pragma once

#include "CoreMinimal.h"

class UObject;

/** What a mover does, for its colour and the panel's filters. */
enum class ERigExecMoverFamily : uint8
{
	Deformer,   // writes a mesh's points
	Constraint, // writes a transform
	Math,       // writes a number (driven keys, gains, PSD drivers)
	Other,
};

/** One sublayer, strongest first in the panel's order. */
struct FRigExecStackLayer
{
	FString Name;
	FString File;
	int32 Depth = 0;
	bool bMissing = false;
	/** The file's time (unix seconds) and size when the stack was exported. */
	int64 MTime = 0;
	int64 Size = 0;
	int32 PrimSpecs = 0;
	/** Movers this layer defines, and movers it overrides another layer's. */
	int32 Defines = 0;
	int32 Overrides = 0;
	/** The file on disk is newer or different than at export (set by Refresh). */
	bool bChanged = false;
	int32 Parent = INDEX_NONE;
	TArray<int32> Children;
};

/** One mover, in execution order. */
struct FRigExecStackMover
{
	/** Its step in the whole rig, 0 first. */
	int32 Ordinal = 0;
	FString Path;
	FString Name;
	/** The scope it sits in ("mouth", "bendy_geo", "body_psd_drivers"). */
	FString Group;
	/** Its schema type without the RigExec prefix ("SkinMover"). */
	FString Type;
	TArray<FString> Targets;
	/** The layer that defines it and the layers that override it, as indices
	 * into FRigExecStack::Layers; INDEX_NONE when unknown. */
	int32 Layer = INDEX_NONE;
	TArray<int32> Over;
	bool bEnabled = true;
	ERigExecMoverFamily Family = ERigExecMoverFamily::Other;
};

/** A deformed mesh and the movers that write its points, first runs first. */
struct FRigExecStackMesh
{
	FString Path;
	FString Name;
	/** Indices into FRigExecStack::Movers. */
	TArray<int32> Movers;
};

struct FRigExecStackSolver
{
	FString Type;
	FString Path;
	int32 Joints = 0;
};

class FRigExecStack
{
public:
	/** Parses export_stack.py's JSON; null with the reason on bad input. */
	static TSharedPtr<FRigExecStack> Parse(const FString& Json, FString& OutError);

	/** The stack of a URigExecCharacter, a URigExecComponent or an actor
	 * holding one, cached per source; null with the reason when it has none. */
	static TSharedPtr<FRigExecStack> For(const UObject* Source, FString& OutReason);

	/** The character or component a source shows (an actor's rig component). */
	static const UObject* Resolve(const UObject* Source);
	/** A short name for it: the asset's, or "<actor> (<asset>)". */
	static FString DisplayName(const UObject* Source);

	/** Compares each layer's file on disk with its export-time stamp. */
	void RefreshChanged();
	int32 ChangedLayerCount() const;

	/** The layer's name, or empty. */
	FString LayerName(int32 Index) const { return Layers.IsValidIndex(Index) ? Layers[Index].Name : FString(); }

	static FLinearColor FamilyColor(ERigExecMoverFamily Family);
	/** A colour per deformer type, so a stack reads at a glance. */
	static FLinearColor TypeColor(const FRigExecStackMover& Mover);

	FString Stage;
	FString RigRoot;
	FString Exported;
	TArray<FRigExecStackLayer> Layers;
	TArray<FRigExecStackMover> Movers;
	TArray<FRigExecStackMesh> Meshes;
	TArray<FRigExecStackSolver> Solvers;
	/** Operations the compile set aside: path and reason. */
	TArray<TPair<FString, FString>> Skipped;
	int32 DeformerCount = 0;
	int32 ConstraintCount = 0;
	int32 MathCount = 0;
};
