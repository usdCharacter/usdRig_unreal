#include "RigExecCharacter.h"

namespace
{
// Bumped whenever the payload layout changes; an older asset is refused at
// load and has to be baked again.
constexpr int32 GCharacterVersion = 1;
} // namespace

FArchive&
operator<<(FArchive& Ar, FRigExecMeshSource& M)
{
	Ar << M.Path << M.Name << M.PointsProperty;
	Ar << M.FaceCounts << M.FaceIndices << M.CornerUVs << M.FaceSlots;
	int32 Slots = M.Slots.Num();
	Ar << Slots;
	if (Ar.IsLoading())
	{
		M.Slots.SetNum(Slots);
	}
	for (FRigExecSurfaceLook& Look : M.Slots)
	{
		Ar << Look.Color << Look.Roughness << Look.Metallic;
	}
	return Ar;
}

FArchive&
operator<<(FArchive& Ar, FRigExecTouchRegionSource& R)
{
	Ar << R.Mesh << R.Name << R.ControlPath << R.HoverColor << R.Faces;
	return Ar;
}

FArchive&
operator<<(FArchive& Ar, FRigExecMeshSet& S)
{
	Ar << S.bZUp << S.MetersPerUnit << S.Meshes << S.Regions;
	bool bLead = S.LeadColor.IsSet(), bSelected = S.SelectedColor.IsSet();
	FLinearColor Lead = S.LeadColor.Get(FLinearColor::Black), Selected = S.SelectedColor.Get(FLinearColor::Black);
	Ar << bLead << Lead << bSelected << Selected;
	if (Ar.IsLoading())
	{
		S.LeadColor = bLead ? TOptional<FLinearColor>(Lead) : TOptional<FLinearColor>();
		S.SelectedColor = bSelected ? TOptional<FLinearColor>(Selected) : TOptional<FLinearColor>();
	}
	return Ar;
}

void
URigExecCharacter::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	int32 Version = GCharacterVersion;
	Ar << Version;
	if (Ar.IsLoading() && Version != GCharacterVersion)
	{
		UE_LOG(LogTemp, Error, TEXT("%s was baked by another RigExec version (%d, expected %d); bake it again"),
		       *GetPathName(), Version, GCharacterVersion);
		Program.Reset();
		return;
	}
	// TArray<uint8> serializes in bulk.
	Ar << Program << Controls << Picker << MeshSet;
}
