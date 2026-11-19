#include "RigExecCharacter.h"

namespace
{
// Bumped whenever the payload layout changes; an older asset is refused at
// load and has to be baked again.
// 2: looks carry a colour texture. 3: and shader parameters. 4: and colours.
// 5: and opacity. 6: the layer and deformer stacks.
constexpr int32 GCharacterVersion = 6;
// The version being read, for the looks' layout.
thread_local int32 GReadingVersion = GCharacterVersion;
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
		if (!Ar.IsLoading() || GReadingVersion >= 2)
		{
			Ar << Look.ColorTextureFile << Look.ColorTexture;
		}
		if (!Ar.IsLoading() || GReadingVersion >= 3)
		{
			Ar << Look.ParamTextureFiles << Look.ParamTextures << Look.ParamScalars;
			if (!Ar.IsLoading() || GReadingVersion >= 4)
			{
				Ar << Look.ParamColors;
			}
			if (!Ar.IsLoading() || GReadingVersion >= 5)
			{
				Ar << Look.Opacity;
			}
		}
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
	if (Ar.IsLoading() && Version != GCharacterVersion && (Version < 1 || Version > GCharacterVersion))
	{
		UE_LOG(LogTemp, Error, TEXT("%s was baked by another RigExec version (%d, expected %d); bake it again"),
		       *GetPathName(), Version, GCharacterVersion);
		Program.Reset();
		return;
	}
	// TArray<uint8> serializes in bulk.
	GReadingVersion = Ar.IsLoading() ? Version : GCharacterVersion;
	Ar << Program << Controls << Picker << MeshSet;
	if (!Ar.IsLoading() || Version >= 6)
	{
		// Only the editor's Stack panel reads it, so a cooked game carries none.
		FString None;
		Ar << (Ar.IsCooking() ? None : Stack);
	}
	GReadingVersion = GCharacterVersion;
}
