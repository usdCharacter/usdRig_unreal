#include "RigExecRuntimeLibrary.h"

#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include "ImageUtils.h"

#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

#include "ControlRig.h"
#include "Misc/Paths.h"
#include "RigExecCharacter.h"
#include "RigExecComponent.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#endif
#include "ControlRigGizmoLibrary.h"
#if WITH_EDITOR
#include "EditMode/ControlRigEditMode.h"
#include "Editor.h"
#include "EditorModeManager.h"
#endif

// The runtime's headers are plain C++; Unreal's single-word macros must not
// reach them.
THIRD_PARTY_INCLUDES_START
#pragma push_macro("check")
#pragma push_macro("verify")
#undef check
#undef verify
#include "rigExecRuntime/runtime.h"
#pragma pop_macro("verify")
#pragma pop_macro("check")
THIRD_PARTY_INCLUDES_END

#include <algorithm>
#include <cmath>
#include <string>

IMPLEMENT_MODULE(FDefaultModuleImpl, RigExecRuntime)

FString
URigExecRuntimeLibrary::RigExecSelfTest(const FString& RigExecFile, const FString& Channel,
                                        const TArray<float>& Values)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *RigExecFile))
	{
		return FString::Printf(TEXT("cannot read %s"), *RigExecFile);
	}
	std::string Error;
	const double OpenStart = FPlatformTime::Seconds();
	std::unique_ptr<rigExec::RigExecRuntimeReader> Reader =
		rigExec::RigExecRuntimeReader::Open(Bytes.GetData(), size_t(Bytes.Num()), &Error);
	const double OpenSeconds = FPlatformTime::Seconds() - OpenStart;
	if (!Reader)
	{
		return FString::Printf(TEXT("open failed: %s"), UTF8_TO_TCHAR(Error.c_str()));
	}
	FString Report = FString::Printf(TEXT("opened %d bytes in %.0f ms; %d inputs\n"), Bytes.Num(),
	                                 OpenSeconds * 1000.0, int32(Reader->GetInputCount()));

	std::vector<std::vector<rigExec::RrVec3f>> First;
	const std::string ChannelPath(TCHAR_TO_UTF8(*Channel));
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		Reader->ResetInputs();
		std::string Why;
		if (!Reader->SetInput(ChannelPath, double(Values[Index]), &Why))
		{
			return Report + FString::Printf(TEXT("SetInput refused: %s"), UTF8_TO_TCHAR(Why.c_str()));
		}
		const double Start = FPlatformTime::Seconds();
		if (!Reader->Execute(&Error))
		{
			return Report + FString::Printf(TEXT("Execute failed: %s"), UTF8_TO_TCHAR(Error.c_str()));
		}
		const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
		double Moved = 0.0;
		size_t PointCount = 0;
		const auto& Points = Reader->GetPoints();
		if (Index == 0)
		{
			for (const auto& Mesh : Points)
			{
				First.push_back(Mesh.points.Vector());
			}
		}
		for (size_t M = 0; M < Points.size() && M < First.size(); ++M)
		{
			const auto& Now = Points[M].points;
			PointCount += Now.size();
			for (size_t P = 0; P < Now.size() && P < First[M].size(); ++P)
			{
				const double DX = double(Now[P][0]) - double(First[M][P][0]);
				const double DY = double(Now[P][1]) - double(First[M][P][1]);
				const double DZ = double(Now[P][2]) - double(First[M][P][2]);
				Moved = FMath::Max(Moved, FMath::Sqrt(DX * DX + DY * DY + DZ * DZ));
			}
		}
		Report += FString::Printf(TEXT("  %s = %.3f: %.1f ms, %d points, max move %.4f\n"), *Channel,
		                          Values[Index], Milliseconds, int32(PointCount), Moved);
	}
	return Report;
}

namespace
{
// Every output a rig publishes, flattened, for telling whether a nudge
// moved it: the points, then each provider's final frame.
std::vector<double>
Snapshot(const rigExec::RigExecRuntimeReader& Reader)
{
	std::vector<double> Out;
	for (const auto& Mesh : Reader.GetPoints())
	{
		for (const auto& P : Mesh.points)
		{
			Out.insert(Out.end(), {double(P[0]), double(P[1]), double(P[2])});
		}
	}
	for (const auto& Frames : Reader.GetProviderFramePublications())
	{
		for (const auto& P : Frames.final.points)
		{
			Out.insert(Out.end(), {P[0], P[1], P[2]});
		}
	}
	return Out;
}
} // namespace

FString
URigExecRuntimeLibrary::RigExecInspect(const FString& RigExecFile)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *RigExecFile))
	{
		return FString::Printf(TEXT("FAILED cannot read %s"), *RigExecFile);
	}
	std::string Error;
	std::unique_ptr<rigExec::RigExecRuntimeReader> Reader =
		rigExec::RigExecRuntimeReader::Open(Bytes.GetData(), size_t(Bytes.Num()), &Error);
	if (!Reader)
	{
		return FString::Printf(TEXT("FAILED open: %s"), UTF8_TO_TCHAR(Error.c_str()));
	}
	if (!Reader->Execute(&Error))
	{
		return FString::Printf(TEXT("FAILED execute: %s"), UTF8_TO_TCHAR(Error.c_str()));
	}
	FString Body;
	const std::vector<std::string> External = Reader->GetExternalMoverTypes();
	const std::vector<std::string> Missing = Reader->GetMissingExternalKernels();
	size_t PointCount = 0;
	for (const auto& Mesh : Reader->GetPoints())
	{
		PointCount += Mesh.points.size();
	}
	Body += FString::Printf(TEXT("  %d inputs, %d point outputs (%d points), %d frames, %d property values\n"),
	                        int32(Reader->GetInputCount()), int32(Reader->GetPoints().size()), int32(PointCount),
	                        int32(Reader->GetProviderFramePublications().size()), int32(Reader->GetPropertyValues().size()));
	for (const std::string& Type : External)
	{
		Body += FString::Printf(TEXT("  plugin mover %s%s\n"), UTF8_TO_TCHAR(Type.c_str()),
		                        std::find(Missing.begin(), Missing.end(), Type) != Missing.end() ? TEXT(" (no kernel: its points pass through)") : TEXT(""));
	}
	for (const std::string& Line : Reader->GetDiagnostics())
	{
		Body += FString::Printf(TEXT("  diagnostic: %s\n"), UTF8_TO_TCHAR(Line.c_str()));
	}
	const std::vector<double> Rest = Snapshot(*Reader);
	int32 Numbers = 0, Moving = 0;
	double Seconds = 0.0;
	TArray<FString> Movers;
	for (size_t I = 0; I < Reader->GetInputCount(); ++I)
	{
		const rigExec::RigExecRuntimeInputInfo& Info = Reader->GetInputInfo(I);
		if (Info.type != rigExec::RrInputTag::Double && Info.type != rigExec::RrInputTag::Float)
		{
			continue;
		}
		++Numbers;
		const double Default = Info.type == rigExec::RrInputTag::Double ? Info.defaultValue.f64 : double(Info.defaultValue.f32);
		if (!Reader->SetInput(Info.name, Default + 0.5, &Error))
		{
			continue;
		}
		const double Start = FPlatformTime::Seconds();
		if (!Reader->Execute(&Error))
		{
			return FString::Printf(TEXT("FAILED execute with %s nudged: %s\n"), UTF8_TO_TCHAR(Info.name.c_str()),
			                       UTF8_TO_TCHAR(Error.c_str())) + Body;
		}
		Seconds += FPlatformTime::Seconds() - Start;
		const std::vector<double> Now = Snapshot(*Reader);
		double Moved = 0.0;
		for (size_t K = 0; K < Now.size() && K < Rest.size(); ++K)
		{
			Moved = FMath::Max(Moved, FMath::Abs(Now[K] - Rest[K]));
		}
		if (Moved > 1e-6)
		{
			++Moving;
			if (Movers.Num() < 6)
			{
				Movers.Add(FString::Printf(TEXT("%s (%.3g)"), UTF8_TO_TCHAR(Info.name.c_str()), Moved));
			}
		}
		Reader->ResetInput(Info.name, &Error);
	}
	Reader->Execute(&Error);
	const bool bBack = Snapshot(*Reader) == Rest;
	Body += FString::Printf(TEXT("  %d of %d number inputs move the rig; %.2f ms an evaluation; reset %s\n"), Moving, Numbers,
	                        Numbers ? Seconds * 1000.0 / Numbers : 0.0, bBack ? TEXT("restores the rest pose") : TEXT("DOES NOT restore the rest pose"));
	if (Movers.Num() > 0)
	{
		Body += TEXT("  e.g. ") + FString::Join(Movers, TEXT(", ")) + TEXT("\n");
	}
	const bool bOk = Missing.empty() && bBack && (Moving > 0 || Numbers == 0);
	return FString::Printf(TEXT("%s %s\n"), bOk ? TEXT("OK") : TEXT("CHECK"), *FPaths::GetCleanFilename(RigExecFile)) + Body;
}

bool
URigExecRuntimeLibrary::ShowControlRigInViewport(UControlRig* ControlRig)
{
#if WITH_EDITOR
	if (!ControlRig || !GIsEditor)
	{
		return false;
	}
	// The shape lookup only searches libraries already in memory; one not
	// yet loaded leaves every control a shape actor with no mesh.
	for (const TSoftObjectPtr<UControlRigShapeLibrary>& Library : ControlRig->GetShapeLibraries())
	{
		Library.LoadSynchronous();
	}
	FEditorModeTools& Tools = GLevelEditorModeTools();
	if (!Tools.IsModeActive(FControlRigEditMode::ModeName))
	{
		Tools.ActivateMode(FControlRigEditMode::ModeName);
	}
	FControlRigEditMode* Mode = static_cast<FControlRigEditMode*>(Tools.GetActiveMode(FControlRigEditMode::ModeName));
	if (!Mode)
	{
		return false;
	}
	Mode->AddControlRigObject(ControlRig, nullptr);
	ControlRig->SetControlsVisible(true);
	Mode->RequestToRecreateControlShapeActors(ControlRig);
	return true;
#else
	return false;
#endif
}

bool
URigExecRuntimeLibrary::SetControlsAsOverlay(bool bOverlay)
{
#if WITH_EDITOR
	// The settings class is private to the Control Rig editor: by reflection.
	UClass* Settings = FindObject<UClass>(nullptr, TEXT("/Script/ControlRigEditor.ControlRigEditModeSettings"));
	FBoolProperty* Overlay = Settings ? FindFProperty<FBoolProperty>(Settings, TEXT("bShowControlsAsOverlay")) : nullptr;
	if (!Overlay)
	{
		return false;
	}
	UObject* Defaults = Settings->GetDefaultObject();
	Overlay->SetPropertyValue_InContainer(Defaults, bOverlay);
	Defaults->SaveConfig();
	return true;
#else
	return false;
#endif
}

URigExecCharacter*
URigExecRuntimeLibrary::BakeCharacter(const FString& RigExecFile, const FString& StageFile, const FString& ControlsFile,
                                      const FString& AssetPath)
{
#if WITH_EDITOR
	auto Full = [](const FString& Path)
	{
		return Path.IsEmpty() || !FPaths::IsRelative(Path) ? Path : FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Path);
	};
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Full(RigExecFile)))
	{
		UE_LOG(LogTemp, Error, TEXT("BakeCharacter: cannot read %s"), *RigExecFile);
		return nullptr;
	}
	std::string Error;
	const std::unique_ptr<rigExec::RigExecRuntimeReader> Reader =
		rigExec::RigExecRuntimeReader::Open(Bytes.GetData(), size_t(Bytes.Num()), &Error);
	if (!Reader || !Reader->Execute(&Error))
	{
		UE_LOG(LogTemp, Error, TEXT("BakeCharacter: cannot open the rig: %s"), UTF8_TO_TCHAR(Error.c_str()));
		return nullptr;
	}
	TArray<FString> Properties;
	for (const auto& Points : Reader->GetPoints())
	{
		Properties.Add(UTF8_TO_TCHAR(Points.path.c_str()));
	}
	FRigExecMeshSet Set;
	FString Why;
	if (!URigExecComponent::ReadStageMeshes(Full(StageFile), Properties, Set, Why))
	{
		UE_LOG(LogTemp, Error, TEXT("BakeCharacter: %s: %s"), *StageFile, *Why);
		return nullptr;
	}
	FString Controls, Picker;
	if (!ControlsFile.IsEmpty())
	{
		if (!FFileHelper::LoadFileToString(Controls, *Full(ControlsFile)))
		{
			UE_LOG(LogTemp, Error, TEXT("BakeCharacter: cannot read %s"), *ControlsFile);
			return nullptr;
		}
		// Biped_controls.json -> Biped_picker.json, when there is one.
		const FString ControlsPath = Full(ControlsFile);
		const int32 At = ControlsPath.Find(TEXT("_controls"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (At != INDEX_NONE)
		{
			FFileHelper::LoadFileToString(Picker, *(ControlsPath.Left(At) + TEXT("_picker.json")));
		}
	}

	// Biped.rigexec -> Biped_stack.json (export_stack.py), when there is one.
	FString Stack;
	{
		const FString RigExecPath = Full(RigExecFile);
		FFileHelper::LoadFileToString(Stack, *(FPaths::Combine(FPaths::GetPath(RigExecPath), FPaths::GetBaseFilename(RigExecPath)) +
		                                       TEXT("_stack.json")));
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	const FString Name = FPackageName::GetShortName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();
	URigExecCharacter* Character = FindObject<URigExecCharacter>(Package, *Name);
	const bool bNew = Character == nullptr;
	if (bNew)
	{
		Character = NewObject<URigExecCharacter>(Package, *Name, RF_Public | RF_Standalone);
	}
	Character->Modify();
	Character->SourceRigExec = RigExecFile;
	Character->SourceStage = StageFile;
	Character->SourceControls = ControlsFile;
	Character->Program = MoveTemp(Bytes);
	Character->Controls = MoveTemp(Controls);
	Character->Picker = MoveTemp(Picker);
	Character->Stack = MoveTemp(Stack);
	Character->MeshSet = MoveTemp(Set);
	// Each surface image becomes a texture in this package, with its source,
	// so it cooks with the character and a packaged game needs no files.
	Character->Textures.Reset();
	{
		// One texture per image file (and colour space).
		TMap<FString, int32> Imported;
		auto Import = [&](const FString& File, bool bLinear, bool bNormal = false) -> int32
		{
			const FString Key = File + (bLinear ? TEXT("|linear") : TEXT("")) + (bNormal ? TEXT("|normal") : TEXT(""));
			if (const int32* Found = Imported.Find(Key))
			{
				return *Found;
			}
			FImage Image;
			if (!FImageUtils::LoadImage(*File, Image))
			{
				UE_LOG(LogTemp, Warning, TEXT("BakeCharacter: cannot read the image %s"), *File);
				Imported.Add(Key, INDEX_NONE);
				return INDEX_NONE;
			}
			FString TextureName = TEXT("T_") + FPaths::GetBaseFilename(File) + (bLinear ? TEXT("_Linear") : TEXT(""));
			for (TCHAR& Ch : TextureName)
			{
				if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
				{
					Ch = TEXT('_');
				}
			}
			UTexture2D* Texture = FindObject<UTexture2D>(Package, *TextureName);
			if (!Texture)
			{
				Texture = NewObject<UTexture2D>(Package, *TextureName, RF_Public);
			}
			if (bNormal)
			{
				Texture->Source.Init(Image);
				Texture->SRGB = false;
				Texture->CompressionSettings = TC_Normalmap;
			}
			else if (bLinear)
			{
				// Linear data, as a shader sampling it linearly expects: decode
				// the sRGB picture into half floats.
				FImage Linear;
				Image.CopyTo(Linear, ERawImageFormat::RGBA16F, EGammaSpace::Linear);
				Texture->Source.Init(Linear);
				Texture->SRGB = false;
				Texture->CompressionSettings = TC_HDR_Compressed;
			}
			else
			{
				Texture->Source.Init(Image);
				Texture->SRGB = true;
			}
			Texture->PostEditChange();
			const int32 Index = Character->Textures.Add(Texture);
			Imported.Add(Key, Index);
			return Index;
		};
		for (FRigExecMeshSource& Mesh : Character->MeshSet.Meshes)
		{
			for (FRigExecSurfaceLook& Look : Mesh.Slots)
			{
				Look.ColorTexture = Look.ColorTextureFile.IsEmpty() ? INDEX_NONE : Import(Look.ColorTextureFile, false);
				Look.ParamTextures.Reset();
				for (const TPair<FString, FString>& File : Look.ParamTextureFiles)
				{
					// The eye shader reads its iris as linear data.
					const bool bLinear = File.Key.EndsWith(TEXT("IrisMap"));
					const bool bNormal = File.Key.EndsWith(TEXT("NormalTexture"));
					Look.ParamTextures.Add(File.Key, Import(File.Value, bLinear, bNormal));
				}
			}
		}
	}
	Character->MeshCount = Character->MeshSet.Meshes.Num();
	Character->TouchRegionCount = Character->MeshSet.Regions.Num();
	Character->RigExecBytes = Character->Program.Num();
	Character->MarkPackageDirty();
	if (bNew)
	{
		FAssetRegistryModule::AssetCreated(Character);
	}
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString File = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Character, *File, Args))
	{
		UE_LOG(LogTemp, Error, TEXT("BakeCharacter: cannot save %s"), *File);
		return nullptr;
	}
	UE_LOG(LogTemp, Log, TEXT("BakeCharacter: %s: %d meshes, %d touch regions, %d rig bytes"), *AssetPath,
	       Character->MeshCount, Character->TouchRegionCount, Character->RigExecBytes);
	return Character;
#else
	return nullptr;
#endif
}
