// A RigExec character baked into one asset: the .rigexec program, its
// controls and picker descriptions, and every mesh's topology, UVs and
// material looks read once from the USD stage. A component loading from it
// needs no USD and no files on disk, so it cooks into a packaged game.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "RigExecCharacter.generated.h"

/** A material's UsdPreviewSurface look; anything else keeps these defaults. */
USTRUCT(BlueprintType)
struct RIGEXECRUNTIME_API FRigExecSurfaceLook
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "RigExec")
	FLinearColor Color = FLinearColor(0.7f, 0.7f, 0.7f);

	UPROPERTY(EditAnywhere, Category = "RigExec")
	float Roughness = 0.6f;

	UPROPERTY(EditAnywhere, Category = "RigExec")
	float Metallic = 0.0f;
};

/** One deformed mesh as the stage describes it. Its points come from the
 * rig: vertex i is the rig's point i of `PointsProperty`. */
struct RIGEXECRUNTIME_API FRigExecMeshSource
{
	/** The USD prim path and name. */
	FString Path;
	FString Name;
	/** The rig output that poses it ("<path>.points"). */
	FString PointsProperty;
	TArray<int32> FaceCounts;
	TArray<int32> FaceIndices;
	/** One UV per face corner (V already flipped for Unreal); empty without UVs. */
	TArray<FVector2f> CornerUVs;
	/** Each face's material slot: 0 is the mesh's binding, then one per bound subset. */
	TArray<int32> FaceSlots;
	TArray<FRigExecSurfaceLook> Slots;

	friend FArchive& operator<<(FArchive& Ar, FRigExecMeshSource& M);
};

/** A touch region: faces of one mesh that select one control. */
struct RIGEXECRUNTIME_API FRigExecTouchRegionSource
{
	int32 Mesh = -1;
	FString Name;
	FString ControlPath;
	FLinearColor HoverColor = FLinearColor(1.0f, 0.6f, 0.0f);
	TArray<int32> Faces;

	friend FArchive& operator<<(FArchive& Ar, FRigExecTouchRegionSource& R);
};

/** Everything the stage contributes, in plain data. */
struct RIGEXECRUNTIME_API FRigExecMeshSet
{
	/** The stage's up axis ('Y' or 'Z') and units. */
	bool bZUp = false;
	double MetersPerUnit = 0.01;
	TArray<FRigExecMeshSource> Meshes;
	TArray<FRigExecTouchRegionSource> Regions;
	/** The touch scopes' lead and selected colours, as authored; unset falls
	 * back to usdview's defaults. */
	TOptional<FLinearColor> LeadColor;
	TOptional<FLinearColor> SelectedColor;

	/** A stage point in Unreal's space, as UsdToUnreal::ConvertVector does it. */
	FVector3d ToUnreal(double X, double Y, double Z) const
	{
		const double Scale = MetersPerUnit / 0.01;
		return bZUp ? FVector3d(X, -Y, Z) * Scale : FVector3d(X, Z, Y) * Scale;
	}

	friend FArchive& operator<<(FArchive& Ar, FRigExecMeshSet& S);
};

/**
 * A baked RigExec character. Made in the editor from a .rigexec, its USD
 * stage and its controls file (URigExecRuntimeLibrary::BakeCharacter, or
 * RigExec > Bake Character Asset); a URigExecComponent with this set loads
 * from it alone.
 */
UCLASS(BlueprintType)
class RIGEXECRUNTIME_API URigExecCharacter : public UObject
{
	GENERATED_BODY()

public:
	/** Where it was baked from, for rebaking. */
	UPROPERTY(VisibleAnywhere, Category = "RigExec")
	FString SourceRigExec;
	UPROPERTY(VisibleAnywhere, Category = "RigExec")
	FString SourceStage;
	UPROPERTY(VisibleAnywhere, Category = "RigExec")
	FString SourceControls;

	UPROPERTY(VisibleAnywhere, Category = "RigExec")
	int32 MeshCount = 0;
	UPROPERTY(VisibleAnywhere, Category = "RigExec")
	int32 TouchRegionCount = 0;
	UPROPERTY(VisibleAnywhere, Category = "RigExec")
	int32 RigExecBytes = 0;

	/** The .rigexec file's bytes. */
	TArray<uint8> Program;
	/** export_controls.py's description, with the Control Rig element names. */
	FString Controls;
	/** export_picker.py's layout, or empty. */
	FString Picker;
	FRigExecMeshSet MeshSet;

	virtual void Serialize(FArchive& Ar) override;
};
