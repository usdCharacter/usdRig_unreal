#include "RigExecComponent.h"

#include "RigExecShaders.h"
#include "RigExecSkinComponent.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

#include "Components/DynamicMeshComponent.h"
#include "ControlRig.h"
#include "ControlRigComponent.h"
#include "Dom/JsonObject.h"
#include "Rigs/RigHierarchy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "DynamicMesh/MeshNormals.h"
#include "HAL/PlatformTime.h"
#include "Tasks/Task.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UDynamicMesh.h"
#include "UnrealUSDWrapper.h"
#include "UsdWrappers/UsdStage.h"
#include "RigExecCharacter.h"

// Unreal builds USD into the editor only; a packaged game loads no stage.
#if USE_USD_SDK
#include "USDTypesConversion.h"

#include "USDIncludesStart.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdGeom/subset.h"
#include "pxr/usd/usdShade/material.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/usdShade/shader.h"
#include "USDIncludesEnd.h"
#endif

THIRD_PARTY_INCLUDES_START
#pragma push_macro("check")
#pragma push_macro("verify")
#undef check
#undef verify
#include "rigExecRuntime/runtime.h"
#pragma pop_macro("verify")
#pragma pop_macro("check")
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogRigExec, Log, All);

URigExecComponent::FOnRenderFallback URigExecComponent::OnRenderFallback;

using namespace UE::Geometry;

namespace
{
// A file property as a full path: a relative one is relative to the project
// directory, so a project carrying its rig data opens from any checkout.
FString
Resolve(const FFilePath& File)
{
	if (File.FilePath.IsEmpty() || !FPaths::IsRelative(File.FilePath))
	{
		return File.FilePath;
	}
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), File.FilePath);
}

// The six rotation orders, in avars:rotationOrder's spelling.
const TCHAR* const GOrders[] = {TEXT("XYZ"), TEXT("XZY"), TEXT("YXZ"), TEXT("YZX"), TEXT("ZXY"), TEXT("ZYX")};

struct FRot3
{
	double M[3][3];
};

// A row-vector rotation about axis `Axis` by `Radians`, as Gf builds it.
FRot3
AxisRotation(int32 Axis, double Radians)
{
	const double C = FMath::Cos(Radians), S = FMath::Sin(Radians);
	FRot3 R = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
	const int32 A = (Axis + 1) % 3, B = (Axis + 2) % 3;
	R.M[A][A] = C;
	R.M[A][B] = S;
	R.M[B][A] = -S;
	R.M[B][B] = C;
	return R;
}

FRot3
Multiply(const FRot3& L, const FRot3& R)
{
	FRot3 Out;
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Col = 0; Col < 3; ++Col)
		{
			Out.M[Row][Col] = L.M[Row][0] * R.M[0][Col] + L.M[Row][1] * R.M[1][Col] + L.M[Row][2] * R.M[2][Col];
		}
	}
	return Out;
}

FRot3
Transpose(const FRot3& R)
{
	FRot3 Out;
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Col = 0; Col < 3; ++Col)
		{
			Out.M[Row][Col] = R.M[Col][Row];
		}
	}
	return Out;
}

double
Unwrap(double Angle, double Hint)
{
	return Angle + 360.0 * FMath::RoundHalfFromZero((Hint - Angle) / 360.0);
}

// Euler angles in degrees whose rotations, applied in `Order` (row
// vectors, first axis first), reproduce `Rot`; of the two solutions the one
// nearest `Hint`, each angle unwrapped to the hint's turn. The derivation
// is gizmoMath.DecomposeEuler's.
void
DecomposeEuler(const FRot3& Rot, int32 Order, const double Hint[3], double Out[3])
{
	const TCHAR* Name = GOrders[Order];
	const int32 I = Name[0] - TEXT('X'), J = Name[1] - TEXT('X'), K = Name[2] - TEXT('X');
	const double Eps = (Order == 0 || Order == 3 || Order == 4) ? 1.0 : -1.0;
	auto M = [&Rot](int32 R, int32 C) { return Rot.M[C][R]; };

	const double CosBeta = FMath::Sqrt(M(K, J) * M(K, J) + M(K, K) * M(K, K));
	const double Beta = FMath::Atan2(-Eps * M(K, I), CosBeta);
	double Alpha, Gamma;
	if (CosBeta > 1e-9)
	{
		Alpha = FMath::Atan2(Eps * M(K, J), M(K, K));
		Gamma = FMath::Atan2(Eps * M(J, I), M(I, I));
	}
	else
	{
		// Gimbal lock: keep the hint's first angle, solve the last.
		Alpha = FMath::DegreesToRadians(Hint[I]);
		const FRot3 Residual = Multiply(Transpose(Multiply(AxisRotation(I, Alpha), AxisRotation(J, Beta))), Rot);
		Gamma = FMath::Atan2(Eps * Residual.M[I][J], Residual.M[I][I]);
	}
	double First[3], Second[3];
	First[I] = FMath::RadiansToDegrees(Alpha);
	First[J] = FMath::RadiansToDegrees(Beta);
	First[K] = FMath::RadiansToDegrees(Gamma);
	Second[I] = First[I] + 180.0;
	Second[J] = 180.0 - First[J];
	Second[K] = First[K] + 180.0;
	double Best = TNumericLimits<double>::Max();
	for (const double* Candidate : {First, Second})
	{
		double Unwrapped[3], Distance = 0.0;
		for (int32 A = 0; A < 3; ++A)
		{
			Unwrapped[A] = Unwrap(Candidate[A], Hint[A]);
			Distance += FMath::Abs(Unwrapped[A] - Hint[A]);
		}
		if (Distance < Best)
		{
			Best = Distance;
			FMemory::Memcpy(Out, Unwrapped, sizeof(Unwrapped));
		}
	}
}

#if USE_USD_SDK
// A material's look as a UsdPreviewSurface states it; anything else (a
// procedural shader) keeps the neutral default.
using FLook = FRigExecSurfaceLook;

FLook
ReadLook(const pxr::UsdShadeMaterial& Material)
{
	FLook Look;
	const pxr::UsdShadeShader Surface = Material ? Material.ComputeSurfaceSource() : pxr::UsdShadeShader();
	pxr::TfToken Id;
	if (!Surface)
	{
		return Look;
	}
	if (!Surface.GetShaderId(&Id) || Id != pxr::TfToken("UsdPreviewSurface"))
	{
		// Another shader: keep its image and number inputs as parameters of
		// the same names, for a material that knows them (the eye's).
		for (const pxr::UsdShadeInput& Input : Surface.GetInputs())
		{
			FString Name = UTF8_TO_TCHAR(Input.GetBaseName().GetString().c_str());
			if (Name.IsEmpty())
			{
				continue;
			}
			Name[0] = FChar::ToUpper(Name[0]);
			// An image fed by a UsdUVTexture node: its file.
			pxr::UsdShadeConnectableAPI InputSource;
			pxr::TfToken InputSourceName;
			pxr::UsdShadeAttributeType InputSourceType;
			if (Input.GetConnectedSource(&InputSource, &InputSourceName, &InputSourceType))
			{
				const pxr::UsdShadeShader Texture(InputSource.GetPrim());
				const pxr::UsdShadeInput FileInput = Texture ? Texture.GetInput(pxr::TfToken("file")) : pxr::UsdShadeInput();
				pxr::SdfAssetPath File;
				if (FileInput && FileInput.Get(&File))
				{
					const std::string& Resolved = File.GetResolvedPath();
					const std::string& Path = Resolved.empty() ? File.GetAssetPath() : Resolved;
					if (!Path.empty())
					{
						Look.ParamTextureFiles.Add(Name, UTF8_TO_TCHAR(Path.c_str()));
					}
				}
				continue;
			}
			pxr::VtValue Value;
			if (!Input.Get(&Value))
			{
				continue;
			}
			if (Value.IsHolding<pxr::SdfAssetPath>())
			{
				const pxr::SdfAssetPath& File = Value.UncheckedGet<pxr::SdfAssetPath>();
				const std::string& Resolved = File.GetResolvedPath();
				const std::string& Path = Resolved.empty() ? File.GetAssetPath() : Resolved;
				if (!Path.empty())
				{
					Look.ParamTextureFiles.Add(Name, UTF8_TO_TCHAR(Path.c_str()));
				}
			}
			else if (Value.IsHolding<float>())
			{
				Look.ParamScalars.Add(Name, Value.UncheckedGet<float>());
			}
			else if (Value.IsHolding<double>())
			{
				Look.ParamScalars.Add(Name, float(Value.UncheckedGet<double>()));
			}
			else if (Value.IsHolding<pxr::GfVec3f>())
			{
				const pxr::GfVec3f& C = Value.UncheckedGet<pxr::GfVec3f>();
				Look.ParamColors.Add(Name, FLinearColor(C[0], C[1], C[2]));
			}
		}
		return Look;
	}
	pxr::GfVec3f Color;
	float Value = 0.0f;
	const pxr::UsdShadeInput Diffuse = Surface.GetInput(pxr::TfToken("diffuseColor"));
	if (Diffuse && Diffuse.Get(&Color))
	{
		Look.Color = FLinearColor(Color[0], Color[1], Color[2]);
	}
	// An image on the colour: a UsdUVTexture's file, resolved on disk. The
	// texture carries the colour, so the tint is white.
	pxr::UsdShadeConnectableAPI Source;
	pxr::TfToken SourceName;
	pxr::UsdShadeAttributeType SourceType;
	if (Diffuse && Diffuse.GetConnectedSource(&Source, &SourceName, &SourceType))
	{
		const pxr::UsdShadeShader Texture(Source.GetPrim());
		pxr::TfToken TextureId;
		pxr::SdfAssetPath File;
		const pxr::UsdShadeInput FileInput = Texture ? Texture.GetInput(pxr::TfToken("file")) : pxr::UsdShadeInput();
		if (FileInput && Texture.GetShaderId(&TextureId) && TextureId == pxr::TfToken("UsdUVTexture") && FileInput.Get(&File))
		{
			const std::string& Resolved = File.GetResolvedPath();
			Look.ColorTextureFile = UTF8_TO_TCHAR((Resolved.empty() ? File.GetAssetPath() : Resolved).c_str());
			Look.Color = FLinearColor::White;
		}
	}
	// A normal map, kept as the surface material's NormalTexture.
	if (const pxr::UsdShadeInput Normal = Surface.GetInput(pxr::TfToken("normal")))
	{
		pxr::UsdShadeConnectableAPI NormalSource;
		pxr::TfToken NormalOut;
		pxr::UsdShadeAttributeType NormalType;
		if (Normal.GetConnectedSource(&NormalSource, &NormalOut, &NormalType))
		{
			const pxr::UsdShadeShader Map(NormalSource.GetPrim());
			const pxr::UsdShadeInput MapFile = Map ? Map.GetInput(pxr::TfToken("file")) : pxr::UsdShadeInput();
			pxr::SdfAssetPath File;
			if (MapFile && MapFile.Get(&File))
			{
				const std::string& Resolved = File.GetResolvedPath();
				Look.ParamTextureFiles.Add(TEXT("NormalTexture"),
				                           UTF8_TO_TCHAR((Resolved.empty() ? File.GetAssetPath() : Resolved).c_str()));
			}
		}
	}
	// An input the material does not author is invalid, and reading one throws.
	if (const pxr::UsdShadeInput Rough = Surface.GetInput(pxr::TfToken("roughness")); Rough && Rough.Get(&Value))
	{
		Look.Roughness = Value;
	}
	if (const pxr::UsdShadeInput Metal = Surface.GetInput(pxr::TfToken("metallic")); Metal && Metal.Get(&Value))
	{
		Look.Metallic = Value;
	}
	// A cutout: opacity under its threshold hides the faces outright.
	if (const pxr::UsdShadeInput Opacity = Surface.GetInput(pxr::TfToken("opacity")); Opacity && Opacity.Get(&Value))
	{
		float Threshold = 0.0f;
		const pxr::UsdShadeInput Cut = Surface.GetInput(pxr::TfToken("opacityThreshold"));
		if (Cut)
		{
			Cut.Get(&Threshold);
		}
		Look.Opacity = (Threshold > 0.0f && Value < Threshold) ? 0.0f : Value;
	}
	return Look;
}

// Every mesh's touch regions, as usdview's TouchPose opens them: for each
// mesh, the first layer (by layerOrder, then label, then path) of the
// RigExecTouchRegions scopes naming it. Typed rigExec:touch:* properties win
// and the older touchpose:* ones are the fallback, as in touchPoseModel.py.
void
ReadStageTouchRegions(const pxr::UsdStageRefPtr& UsdStage, FRigExecMeshSet& Out)
{
	const pxr::TfToken RegionsType("RigExecTouchRegions"), RegionType("RigExecTouchRegion");
	const pxr::TfToken MeshRel("rigExec:touch:mesh"), LayerName("rigExec:touch:layerName"),
		LayerOrder("rigExec:touch:layerOrder");
	const pxr::TfToken TypedFaces("rigExec:touch:faces"), CustomFaces("touchpose:faces");
	const pxr::TfToken TypedControl("rigExec:touch:control"), CustomControl("touchpose:control");
	const pxr::TfToken TypedPalette("rigExec:touch:palette"), CustomPalette("touchpose:palette");
	const pxr::TfToken Hilight("touchpose:hilight"), OwnColor("touchpose:color");
	const pxr::TfToken LeadColor("touchpose:leadColor"), SelectedColor("touchpose:selectedColor");

	auto IsRegion = [&](const pxr::UsdPrim& Child) {
		return Child.GetTypeName() == RegionType || Child.HasAttribute(CustomFaces);
	};
	auto Targets = [](const pxr::UsdPrim& Prim, const pxr::TfToken& Typed, const pxr::TfToken& Custom) {
		pxr::SdfPathVector Found;
		for (const pxr::TfToken& Name : {Typed, Custom})
		{
			if (const pxr::UsdRelationship Rel = Prim.GetRelationship(Name))
			{
				Rel.GetTargets(&Found);
				if (!Found.empty())
				{
					break;
				}
			}
		}
		return Found;
	};
	auto Color = [](const pxr::UsdPrim& Prim, const pxr::TfToken& Name) -> TOptional<FLinearColor> {
		pxr::GfVec3f Value;
		const pxr::UsdAttribute Attr = Prim.GetAttribute(Name);
		if (Attr && Attr.Get(&Value))
		{
			return FLinearColor(Value[0], Value[1], Value[2]);
		}
		return {};
	};

	struct FLayer
	{
		int32 Order;
		FString Label;
		FString Path;
		FString Mesh;
		pxr::UsdPrim Prim;
	};
	TArray<FLayer> Layers;
	for (const pxr::UsdPrim& Prim : UsdStage->Traverse())
	{
		if (Prim.GetTypeName() != RegionsType)
		{
			continue;
		}
		const pxr::SdfPathVector Mesh = Targets(Prim, MeshRel, pxr::TfToken("touchpose:mesh"));
		bool bHasRegions = false;
		for (const pxr::UsdPrim& Child : Prim.GetChildren())
		{
			bHasRegions |= IsRegion(Child);
		}
		if (Mesh.empty() || !bHasRegions)
		{
			continue;
		}
		FLayer& Layer = Layers.AddDefaulted_GetRef();
		int Order = 0;
		pxr::TfToken Name;
		if (const pxr::UsdAttribute Attr = Prim.GetAttribute(LayerOrder))
		{
			Attr.Get(&Order);
		}
		if (const pxr::UsdAttribute Attr = Prim.GetAttribute(LayerName))
		{
			Attr.Get(&Name);
		}
		Layer.Order = Order;
		Layer.Label = UTF8_TO_TCHAR((Name.IsEmpty() ? Prim.GetName() : Name).GetText());
		Layer.Path = UTF8_TO_TCHAR(Prim.GetPath().GetString().c_str());
		Layer.Mesh = UTF8_TO_TCHAR(Mesh.front().GetString().c_str());
		Layer.Prim = Prim;
	}
	Layers.Sort([](const FLayer& A, const FLayer& B) {
		return A.Order != B.Order ? A.Order < B.Order : A.Label != B.Label ? A.Label < B.Label : A.Path < B.Path;
	});

	for (int32 M = 0; M < Out.Meshes.Num(); ++M)
	{
		const FLayer* Layer = Layers.FindByPredicate([&](const FLayer& L) { return L.Mesh == Out.Meshes[M].Path; });
		if (!Layer)
		{
			continue;
		}
		pxr::VtVec3fArray Palette;
		for (const pxr::TfToken& Name : {TypedPalette, CustomPalette})
		{
			if (const pxr::UsdAttribute Attr = Layer->Prim.GetAttribute(Name))
			{
				if (Attr.Get(&Palette) && !Palette.empty())
				{
					break;
				}
			}
		}
		// Every mesh's scope carries the same two state colours; the last
		// one read stands.
		if (const TOptional<FLinearColor> Lead = Color(Layer->Prim, LeadColor))
		{
			Out.LeadColor = Lead;
		}
		if (const TOptional<FLinearColor> Selected = Color(Layer->Prim, SelectedColor))
		{
			Out.SelectedColor = Selected;
		}
		for (const pxr::UsdPrim& Child : Layer->Prim.GetChildren())
		{
			if (!IsRegion(Child))
			{
				continue;
			}
			const pxr::SdfPathVector Control = Targets(Child, TypedControl, CustomControl);
			FRigExecTouchRegionSource& Region = Out.Regions.AddDefaulted_GetRef();
			Region.Mesh = M;
			Region.Name = UTF8_TO_TCHAR(Child.GetName().GetText());
			Region.ControlPath = Control.empty() ? FString() : UTF8_TO_TCHAR(Control.front().GetString().c_str());
			pxr::VtIntArray Faces;
			for (const pxr::TfToken& Name : {TypedFaces, CustomFaces})
			{
				if (const pxr::UsdAttribute Attr = Child.GetAttribute(Name))
				{
					if (Attr.Get(&Faces) && !Faces.empty())
					{
						break;
					}
				}
			}
			Region.Faces.Append(Faces.cdata(), int32(Faces.size()));
			int Index = 0;
			if (const pxr::UsdAttribute Attr = Child.GetAttribute(Hilight))
			{
				Attr.Get(&Index);
			}
			if (Palette.empty())
			{
				Region.HoverColor = Color(Child, OwnColor).Get(FLinearColor(1.0f, 0.6f, 0.0f));
			}
			else
			{
				const pxr::GfVec3f& P = Palette[size_t(FMath::Max(Index, 0)) % Palette.size()];
				Region.HoverColor = FLinearColor(P[0], P[1], P[2]);
			}
		}
		UE_LOG(LogRigExec, Log, TEXT("touch layer %s on %s"), *Layer->Label, *Out.Meshes[M].Path);
	}
}
#endif

const TCHAR* const GTransformAvars[9] = {TEXT(".avars:tx"), TEXT(".avars:ty"), TEXT(".avars:tz"),
                                         TEXT(".avars:rx"), TEXT(".avars:ry"), TEXT(".avars:rz"),
                                         TEXT(".avars:sx"), TEXT(".avars:sy"), TEXT(".avars:sz")};

// Touch highlight states, in drawing priority: a region hovered and
// selected draws as hovered. Their colours and opacity are usdview
// TouchPose's (plugin/touchPose/touchPoseModel.py, touchPoseUI.py).
enum ETouchState
{
	TouchHover,
	TouchLead,
	TouchSelected,
	TouchStateCount
};
// The tint's strength: usdview's HIGHLIGHT_OPACITY, carried in the colour's
// alpha, which the surface material blends by.
constexpr float GTouchOpacity = 0.30f;
// An untinted face: the material ignores its colour.
const FVector4f GNoTint(1.0f, 1.0f, 1.0f, 0.0f);
const FLinearColor GDefaultLeadColor(0.054f, 0.420f, 0.187f);
const FLinearColor GDefaultSelectedColor(0.277f, 0.277f, 0.277f);

// A colour scaled so its brightest channel is `Value`: hue and saturation
// stay exactly as authored (touchPoseModel._ScaleTo).
FLinearColor
ScaleTo(const FLinearColor& Color, float Value)
{
	const float Peak = FMath::Max3(Color.R, Color.G, Color.B);
	if (Peak <= 1e-6f)
	{
		return FLinearColor(Value, Value, Value);
	}
	const float Factor = Value / Peak;
	return FLinearColor(FMath::Min(1.0f, Color.R * Factor), FMath::Min(1.0f, Color.G * Factor),
	                    FMath::Min(1.0f, Color.B * Factor));
}
} // namespace

struct URigExecComponent::FRigExecTouch
{
	struct FMesh
	{
		/** The USD mesh's path, which the regions' scope names. */
		FString Path;
		/** The USD face each triangle came from. */
		TArray<int32> TriangleFace;
		/** Each USD face's first triangle; a face's triangles are contiguous
		 * (one past the last face holds the triangle count). */
		TArray<int32> FaceFirstTriangle;
		/** Each USD face's region, -1 for unpainted skin; empty when the
		 * mesh has no regions. */
		TArray<int32> FaceRegion;
		TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> Tree;
		/** The posed points moved since the tree was built. */
		bool bTreeDirty = true;
	};
	struct FRegion
	{
		int32 Mesh = -1;
		FString Name;
		FString ControlPath;
		FName Element;
		FLinearColor HoverColor;
		/** The USD faces it owns, on its mesh. */
		TArray<int32> Faces;
	};
	TArray<FMesh> Meshes;
	TArray<FRegion> Regions;
	FLinearColor LeadColor = ScaleTo(GDefaultLeadColor, 0.95f);
	FLinearColor SelectedColor = ScaleTo(GDefaultSelectedColor, 0.62f);
	int32 Hover = -1;
	int32 Lead = -1;
	TArray<int32> Selected;
	bool bSuspended = false;
	/** Each region's state as last written into the colours
	 * (TouchStateCount: untinted). */
	TArray<uint8> Written;

	bool IsShown() const { return Hover >= 0 || Lead >= 0 || Selected.Num() > 0; }
};

URigExecComponent::URigExecComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	bTickInEditor = true;
}
namespace
{
/** Tasks a reader launched and has not joined yet. The reader may launch
 * from any task, so the list is locked. */
struct FRigExecDispatch
{
	FCriticalSection Lock;
	TArray<UE::Tasks::FTask> Tasks;
};

// Hands a reader Unreal's task system: its independent clusters (and the
// split halves of a large skin) then run in parallel. Results are
// bit-identical to the serial run.
void
UseTaskSystem(rigExec::RigExecRuntimeReader& Reader, const TSharedRef<FRigExecDispatch, ESPMode::ThreadSafe>& Dispatch)
{
	Reader.SetTaskDispatch(
		[Dispatch](std::function<void()> Work)
		{
			UE::Tasks::FTask Task = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Work = MoveTemp(Work)]() { Work(); });
			FScopeLock Lock(&Dispatch->Lock);
			Dispatch->Tasks.Add(MoveTemp(Task));
		},
		[Dispatch]()
		{
			// A task may launch more before it ends: join until none are left.
			for (;;)
			{
				TArray<UE::Tasks::FTask> Batch;
				{
					FScopeLock Lock(&Dispatch->Lock);
					Batch = MoveTemp(Dispatch->Tasks);
					Dispatch->Tasks.Reset();
				}
				if (Batch.IsEmpty())
				{
					return;
				}
				UE::Tasks::Wait(Batch);
			}
		});
}
} // namespace

/** The provider frames the last Execute published, by path, gathered on the
 * first AssetFrame after it; and the dispatcher that runs the reader's
 * clusters on Unreal's task system. */
struct URigExecComponent::FRigExecFrames
{
	std::vector<rigExec::RigExecRuntimeProviderFrames> Published;
	TMap<FString, int32> Index;
	bool bStale = true;
	/** The last published run's matrix primvars, read without the reader. */
	std::vector<rigExec::RigExecRuntimeMatrixPrimvar> Primvars;
	bool bHavePrimvars = false;
	/** A run in flight on the task system (bEvaluateAsync). */
	UE::Tasks::FTask Pending;
	bool bPending = false;
	bool bOk = true;
	std::string Error;
	float ExecuteMs = 0.0f;
	TSharedRef<FRigExecDispatch, ESPMode::ThreadSafe> Dispatch = MakeShared<FRigExecDispatch, ESPMode::ThreadSafe>();
};

URigExecComponent::~URigExecComponent()
{
	Settle();
	delete Reader;
	delete Touch;
	delete FrameCache;
}

bool
URigExecComponent::Load()
{
	// Never touch the reader while a run is in flight.
	Settle();
	Bytes.Reset();
	bPlaced = false;
	// The rig, its controls and its meshes: from the baked character, or
	// from the files and the stage they were baked from.
	FRigExecMeshSet Set;
	FString ControlsText;
	const bool bFromAsset = Character != nullptr;
	if (bFromAsset)
	{
		SourceName = Character->GetPathName();
		Bytes = Character->Program;
		ControlsText = Character->Controls;
		Set = Character->MeshSet;
		if (Bytes.IsEmpty())
		{
			UE_LOG(LogRigExec, Error, TEXT("%s holds no rig; bake it again"), *SourceName);
			return false;
		}
	}
	else
	{
		SourceName = RigExecFile.FilePath;
		if (!FFileHelper::LoadFileToArray(Bytes, *Resolve(RigExecFile)))
		{
			UE_LOG(LogRigExec, Error, TEXT("cannot read %s"), *RigExecFile.FilePath);
			return false;
		}
		if (!ControlsFile.FilePath.IsEmpty() && !FFileHelper::LoadFileToString(ControlsText, *Resolve(ControlsFile)))
		{
			UE_LOG(LogRigExec, Error, TEXT("cannot read %s"), *ControlsFile.FilePath);
		}
	}
	std::string Error;
	delete Reader;
	delete FrameCache;
	FrameCache = new FRigExecFrames;
	// The inputs open at their bake-time values: the rig as it was baked.
	Reader = rigExec::RigExecRuntimeReader::Open(Bytes.GetData(), size_t(Bytes.Num()), &Error).release();
	if (Reader)
	{
		UseTaskSystem(*Reader, FrameCache->Dispatch);
	}
	if (!Reader || !Reader->Execute(&Error))
	{
		UE_LOG(LogRigExec, Error, TEXT("cannot open the rig: %s"), UTF8_TO_TCHAR(Error.c_str()));
		delete Reader;
		Reader = nullptr;
		return false;
	}
	// A plugin mover (one usdRig registers from a USD-side library, such as
	// RigExecPatchMover) has no kernel here: its points pass through.
	for (const std::string& Type : Reader->GetMissingExternalKernels())
	{
		UE_LOG(LogRigExec, Warning, TEXT("%s: the rig uses %s, which the RigExec runtime in Unreal has no kernel for; its points do not move"),
		       *SourceName, UTF8_TO_TCHAR(Type.c_str()));
	}
	if (!bFromAsset)
	{
		TArray<FString> Properties;
		for (const auto& Points : Reader->GetPoints())
		{
			Properties.Add(UTF8_TO_TCHAR(Points.path.c_str()));
		}
		FString Why;
		if (!ReadStageMeshes(Resolve(StageFile), Properties, Set, Why))
		{
			UE_LOG(LogRigExec, Error, TEXT("%s: %s: %s"), *GetPathName(), *StageFile.FilePath, *Why);
			return false;
		}
	}
	bStageZUp = Set.bZUp;
	StageMetersPerUnit = Set.MetersPerUnit;
	CentimetresPerUnit = Set.MetersPerUnit * 100.0;

	BuildMeshes(Set);
	if (Meshes.Num() == 0)
	{
		UE_LOG(LogRigExec, Error, TEXT("no mesh in %s matches the rig's points"), *SourceName);
	}
	ControlMaps.Reset();
	ChannelMaps.Reset();
	LimbMaps.Reset();
	if (!ControlsText.IsEmpty())
	{
		LoadControls(ControlsText, bFromAsset ? SourceName : ControlsFile.FilePath);
	}
	LoadTouchRegions(Set);
	SetUpRenderPath();
	UE_LOG(LogRigExec, Log, TEXT("loaded %s: %d meshes, %d controls, %d channels, %d property chain values"),
	       *SourceName, Meshes.Num(), ControlMaps.Num(), ChannelMaps.Num(),
	       int32(Reader->GetPropertyValues().size()));
	return Meshes.Num() > 0;
}

bool
URigExecComponent::ReadStageMeshes(const FString& StagePath, const TArray<FString>& PointsProperties,
                                   FRigExecMeshSet& Out, FString& Error)
{
#if USE_USD_SDK
	const UE::FUsdStage Stage = UnrealUSDWrapper::OpenStage(*StagePath, EUsdInitialLoadSet::LoadAll);
	if (!Stage)
	{
		Error = TEXT("cannot open the stage");
		return false;
	}
	const pxr::UsdStageRefPtr UsdStage(Stage);
	const FUsdStageInfo StageInfo(UsdStage);
	Out = FRigExecMeshSet();
	Out.bZUp = StageInfo.UpAxis == EUsdUpAxis::ZAxis;
	Out.MetersPerUnit = StageInfo.MetersPerUnit;

	for (const FString& Property : PointsProperties)
	{
		if (!Property.EndsWith(TEXT(".points")))
		{
			continue;
		}
		const FString PrimPath = Property.LeftChop(7);
		const pxr::UsdGeomMesh UsdMesh(UsdStage->GetPrimAtPath(pxr::SdfPath(TCHAR_TO_UTF8(*PrimPath))));
		if (!UsdMesh)
		{
			continue;
		}
		FRigExecMeshSource& Mesh = Out.Meshes.AddDefaulted_GetRef();
		Mesh.Path = PrimPath;
		Mesh.Name = UTF8_TO_TCHAR(UsdMesh.GetPrim().GetName().GetText());
		Mesh.PointsProperty = Property;
		pxr::VtIntArray Counts, Indices;
		UsdMesh.GetFaceVertexCountsAttr().Get(&Counts);
		UsdMesh.GetFaceVertexIndicesAttr().Get(&Indices);
		Mesh.FaceCounts.Append(Counts.cdata(), int32(Counts.size()));
		Mesh.FaceIndices.Append(Indices.cdata(), int32(Indices.size()));

		// Material slots: the mesh's own binding, then one per bound face
		// subset, which overrides it for its faces.
		pxr::UsdShadeMaterialBindingAPI Binding(UsdMesh.GetPrim());
		Mesh.Slots.Add(ReadLook(Binding.ComputeBoundMaterial()));
		Mesh.FaceSlots.Init(0, int32(Counts.size()));
		for (const pxr::UsdGeomSubset& Subset : Binding.GetMaterialBindSubsets())
		{
			pxr::VtIntArray Faces;
			Subset.GetIndicesAttr().Get(&Faces);
			Mesh.Slots.Add(ReadLook(pxr::UsdShadeMaterialBindingAPI(Subset.GetPrim()).ComputeBoundMaterial()));
			for (int Face : Faces)
			{
				if (Face >= 0 && Face < Mesh.FaceSlots.Num())
				{
					Mesh.FaceSlots[Face] = Mesh.Slots.Num() - 1;
				}
			}
		}
		// primvars:st, one per face-vertex corner so faces keep their own UVs
		// across a seam; V flipped, as Unreal's UVs run top-down.
		const pxr::UsdGeomPrimvar St = pxr::UsdGeomPrimvarsAPI(UsdMesh.GetPrim()).GetPrimvar(pxr::TfToken("st"));
		pxr::VtVec2fArray Values;
		if (St && St.ComputeFlattened(&Values) && !Values.empty())
		{
			const pxr::TfToken Interpolation = St.GetInterpolation();
			Mesh.CornerUVs.SetNumUninitialized(int32(Indices.size()));
			int32 Corner = 0;
			for (int32 Face = 0; Face < int32(Counts.size()); ++Face)
			{
				for (int Side = 0; Side < Counts[Face]; ++Side, ++Corner)
				{
					size_t At = 0;
					if (Interpolation == pxr::UsdGeomTokens->faceVarying)
					{
						At = size_t(Corner);
					}
					else if (Interpolation == pxr::UsdGeomTokens->vertex || Interpolation == pxr::UsdGeomTokens->varying)
					{
						At = size_t(Indices[Corner]);
					}
					else if (Interpolation == pxr::UsdGeomTokens->uniform)
					{
						At = size_t(Face);
					}
					const pxr::GfVec2f UV = At < Values.size() ? Values[At] : pxr::GfVec2f(0.0f);
					Mesh.CornerUVs[Corner] = FVector2f(UV[0], 1.0f - UV[1]);
				}
			}
		}
	}
	ReadStageTouchRegions(UsdStage, Out);
	return true;
#else
	Error = TEXT("this build has no USD; bake a RigExec Character asset in the editor and load that");
	return false;
#endif
}

UTexture*
URigExecComponent::LookTexture(int32 Baked, const FString& File)
{
	if (Character && Character->Textures.IsValidIndex(Baked) && Character->Textures[Baked])
	{
		return Character->Textures[Baked];
	}
	if (File.IsEmpty())
	{
		return nullptr;
	}
	if (TObjectPtr<UTexture2D>* Cached = LiveTextures.Find(File))
	{
		return *Cached;
	}
	UTexture2D* Image = FPaths::FileExists(File) ? FImageUtils::ImportFileAsTexture2D(File) : nullptr;
	LiveTextures.Add(File, Image);
	return Image;
}

void
URigExecComponent::ApplyLookParameters(UMaterialInstanceDynamic* Instance, const FRigExecSurfaceLook& Look)
{
	if (!Instance)
	{
		return;
	}
	for (const TPair<FString, float>& Scalar : Look.ParamScalars)
	{
		Instance->SetScalarParameterValue(FName(*Scalar.Key), Scalar.Value);
	}
	for (const TPair<FString, FLinearColor>& Color : Look.ParamColors)
	{
		Instance->SetVectorParameterValue(FName(*Color.Key), Color.Value);
	}
	for (const TPair<FString, FString>& File : Look.ParamTextureFiles)
	{
		const int32* Baked = Look.ParamTextures.Find(File.Key);
		if (UTexture* Image = LookTexture(Baked ? *Baked : INDEX_NONE, File.Value))
		{
			Instance->SetTextureParameterValue(FName(*File.Key), Image);
		}
	}
}

void
URigExecComponent::ApplyMeshLook(int32 Mesh, int32 Slot, UMaterialInstanceDynamic* Instance)
{
	if (MeshLooks.IsValidIndex(Mesh) && MeshLooks[Mesh].IsValidIndex(Slot))
	{
		ApplyLookParameters(Instance, MeshLooks[Mesh][Slot]);
	}
}

UTexture*
URigExecComponent::SurfaceTexture(const FRigExecSurfaceLook& Look)
{
	// Baked into the character asset: cooks into a packaged game.
	if (Character && Character->Textures.IsValidIndex(Look.ColorTexture) && Character->Textures[Look.ColorTexture])
	{
		return Character->Textures[Look.ColorTexture];
	}
	if (Look.ColorTextureFile.IsEmpty())
	{
		return nullptr;
	}
	// Read live from a stage: load the image once, transiently.
	if (TObjectPtr<UTexture2D>* Cached = LiveTextures.Find(Look.ColorTextureFile))
	{
		return *Cached;
	}
	UTexture2D* Image = FPaths::FileExists(Look.ColorTextureFile) ? FImageUtils::ImportFileAsTexture2D(Look.ColorTextureFile) : nullptr;
	if (!Image)
	{
		UE_LOG(LogRigExec, Warning, TEXT("cannot read the surface image %s"), *Look.ColorTextureFile);
	}
	LiveTextures.Add(Look.ColorTextureFile, Image);
	return Image;
}

void
URigExecComponent::BuildMeshes(const FRigExecMeshSet& Set)
{
	for (UDynamicMeshComponent* Mesh : Meshes)
	{
		if (Mesh)
		{
			Mesh->DestroyComponent();
		}
	}
	for (URigExecSkinComponent* Skin : Skins)
	{
		if (Skin)
		{
			Skin->DestroyComponent();
		}
	}
	Skins.Reset();
	Meshes.Reset();
	PointSlots.Reset();
	MeshPaths.Reset();
	MeshLooks.Reset();
	MeshDrawn.Reset();
	++LoadCount;
	delete Touch;
	Touch = new FRigExecTouch;

	const auto& Points = Reader->GetPoints();
	TMap<FString, int32> SlotOf;
	for (int32 Slot = 0; Slot < int32(Points.size()); ++Slot)
	{
		SlotOf.Add(UTF8_TO_TCHAR(Points[size_t(Slot)].path.c_str()), Slot);
	}
	UMaterialInterface* Parent = SurfaceMaterial ? SurfaceMaterial.Get()
	                                             : LoadObject<UMaterialInterface>(nullptr, TEXT("/RigExec/M_RigExecSurface.M_RigExecSurface"));
	if (!Parent)
	{
		Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	}
	for (const FRigExecMeshSource& Source : Set.Meshes)
	{
		const int32* Slot = SlotOf.Find(Source.PointsProperty);
		if (!Slot)
		{
			UE_LOG(LogRigExec, Warning, TEXT("the rig does not pose %s"), *Source.PointsProperty);
			continue;
		}
		const auto& Rig = Points[size_t(*Slot)].points;
		const TArray<int32>& Counts = Source.FaceCounts;
		const TArray<int32>& Indices = Source.FaceIndices;

		FDynamicMesh3 Built;
		for (const auto& P : Rig)
		{
			Built.AppendVertex(Set.ToUnreal(P[0], P[1], P[2]));
		}
		// USD faces are counter-clockwise in a right-handed space. Swapping
		// Y and Z mirrors them into Unreal's left-handed space, where the
		// same index order is the front face, so the order is kept.
		TArray<int32> TriangleFace;
		TArray<FIndex3i> TriangleCorners;
		int32 Cursor = 0;
		for (int32 Face = 0; Face < Counts.Num(); ++Face)
		{
			const int32 Count = Counts[Face];
			// Faces of a cutout material (opacity zero) are not drawn at all.
			const int32 FaceSlot = Source.FaceSlots.IsValidIndex(Face) ? Source.FaceSlots[Face] : 0;
			if (Source.Slots.IsValidIndex(FaceSlot) && Source.Slots[FaceSlot].Opacity <= 0.0f)
			{
				Cursor += Count;
				continue;
			}
			for (int32 Corner = 1; Corner + 1 < Count && Cursor + Corner + 1 < Indices.Num(); ++Corner)
			{
				const int32 A = Indices[Cursor];
				const int32 B = Indices[Cursor + Corner];
				const int32 C = Indices[Cursor + Corner + 1];
				if (A < int32(Rig.size()) && B < int32(Rig.size()) && C < int32(Rig.size()))
				{
					if (Built.AppendTriangle(A, B, C) >= 0)
					{
						TriangleFace.Add(Face);
						TriangleCorners.Add(FIndex3i(Cursor, Cursor + Corner, Cursor + Corner + 1));
					}
				}
			}
			Cursor += Count;
		}
		Built.EnableAttributes();
		FMeshNormals::InitializeOverlayToPerVertexNormals(Built.Attributes()->PrimaryNormals(), false);
		Built.Attributes()->EnableMaterialID();
		for (int32 Triangle : Built.TriangleIndicesItr())
		{
			const int32 Face = TriangleFace[Triangle];
			Built.Attributes()->GetMaterialID()->SetValue(Triangle, Source.FaceSlots.IsValidIndex(Face) ? Source.FaceSlots[Face] : 0);
		}
		// Each USD face is a triangle group: a renderer that wants the real
		// polygons (a subdivision surface) gets the quads back from them.
		Built.EnableTriangleGroups();
		for (int32 Triangle : Built.TriangleIndicesItr())
		{
			Built.SetTriangleGroup(Triangle, TriangleFace[Triangle]);
		}
		if (Source.CornerUVs.Num() == Indices.Num() && Indices.Num() > 0)
		{
			FDynamicMeshUVOverlay* UVs = Built.Attributes()->PrimaryUV();
			TArray<int32> Elements;
			Elements.SetNumUninitialized(Indices.Num());
			for (int32 Corner = 0; Corner < Indices.Num(); ++Corner)
			{
				Elements[Corner] = UVs->AppendElement(Source.CornerUVs[Corner]);
			}
			for (int32 Triangle : Built.TriangleIndicesItr())
			{
				const FIndex3i& C = TriangleCorners[Triangle];
				UVs->SetTriangle(Triangle, FIndex3i(Elements[C.A], Elements[C.B], Elements[C.C]));
			}
		}
		// A colour per face corner, untinted: Touch Pose lights a region by
		// writing its faces' colours, which the surface material blends by
		// their alpha.
		{
			Built.Attributes()->EnablePrimaryColors();
			FDynamicMeshColorOverlay* Colors = Built.Attributes()->PrimaryColors();
			for (int32 Triangle : Built.TriangleIndicesItr())
			{
				Colors->SetTriangle(Triangle, FIndex3i(Colors->AppendElement(GNoTint), Colors->AppendElement(GNoTint),
				                                       Colors->AppendElement(GNoTint)));
			}
		}

		// Rebuilt on every load, so never saved.
		UDynamicMeshComponent* Component = NewObject<UDynamicMeshComponent>(
			GetOwner() ? static_cast<UObject*>(GetOwner()) : static_cast<UObject*>(this), *Source.Name, RF_Transient);
		Component->SetMesh(MoveTemp(Built));
		// Deformed every evaluation, so never part of a lighting build.
		Component->SetMobility(EComponentMobility::Movable);
		for (int32 Index = 0; Parent && Index < Source.Slots.Num(); ++Index)
		{
			const FRigExecSurfaceLook& Look = Source.Slots[Index];
			UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, Component);
			// The basic shape material calls its colour "Color"; a project
			// material may use the Unreal names.
			Instance->SetVectorParameterValue(TEXT("Color"), Look.Color);
			Instance->SetVectorParameterValue(TEXT("BaseColor"), Look.Color);
			Instance->SetScalarParameterValue(TEXT("Roughness"), Look.Roughness);
			Instance->SetScalarParameterValue(TEXT("Metallic"), Look.Metallic);
			if (UTexture* Image = SurfaceTexture(Look))
			{
				Instance->SetTextureParameterValue(TEXT("BaseColorTexture"), Image);
			}
			ApplyLookParameters(Instance, Look);
			Component->SetMaterial(Index, Instance);
		}
		if (GetOwner())
		{
			Component->SetupAttachment(this);
			Component->RegisterComponent();
		}
		Meshes.Add(Component);
		PointSlots.Add(*Slot);
		MeshPaths.Add(Source.Path);
		MeshLooks.Add(Source.Slots);
		MeshDrawn.Add(true);
		FRigExecTouch::FMesh& Touched = Touch->Meshes.AddDefaulted_GetRef();
		Touched.Path = Source.Path;
		Touched.FaceFirstTriangle.Init(TriangleFace.Num(), Counts.Num() + 1);
		for (int32 Triangle = TriangleFace.Num() - 1; Triangle >= 0; --Triangle)
		{
			Touched.FaceFirstTriangle[TriangleFace[Triangle]] = Triangle;
		}
		// A face that lost every triangle starts where the next one does.
		for (int32 Face = Counts.Num() - 1; Face >= 0; --Face)
		{
			Touched.FaceFirstTriangle[Face] = FMath::Min(Touched.FaceFirstTriangle[Face], Touched.FaceFirstTriangle[Face + 1]);
		}
		Touched.FaceRegion.Init(-1, Counts.Num());
		Touched.TriangleFace = MoveTemp(TriangleFace);
	}
}

bool
URigExecComponent::SetChannel(const FString& Channel, float Value)
{
	if (!Reader)
	{
		return false;
	}
	std::string Why;
	if (!WriteInput(TCHAR_TO_UTF8(*Channel), double(Value), &Why))
	{
		UE_LOG(LogRigExec, Warning, TEXT("%s: %s"), *Channel, UTF8_TO_TCHAR(Why.c_str()));
		return false;
	}
	// A described channel remembers what it was sent, so a reader of it
	// (an IK/FK switch with no Control Rig) sees the value.
	const int32 K = FindChannelIndex(Channel);
	if (K != INDEX_NONE)
	{
		ChannelMaps[K].Value = double(Value);
	}
	return true;
}

int32
URigExecComponent::FindChannelIndex(const FString& Path) const
{
	return ChannelMaps.IndexOfByPredicate([&Path](const FChannelMap& M) { return M.Path == Path; });
}

const FString&
URigExecComponent::GetChannelPath(int32 Channel) const
{
	static const FString None;
	return ChannelMaps.IsValidIndex(Channel) ? ChannelMaps[Channel].Path : None;
}

double
URigExecComponent::GetChannelValue(int32 Channel) const
{
	return ChannelMaps.IsValidIndex(Channel) ? ChannelMaps[Channel].Value : 0.0;
}

double
URigExecComponent::GetChannelDefault(int32 Channel) const
{
	return ChannelMaps.IsValidIndex(Channel) ? ChannelMaps[Channel].Default : 0.0;
}

const TArray<FString>&
URigExecComponent::GetChannelLabels(int32 Channel) const
{
	static const TArray<FString> None;
	return ChannelMaps.IsValidIndex(Channel) ? ChannelMaps[Channel].Labels : None;
}

bool
URigExecComponent::SetChannelValue(int32 Channel, double Value)
{
	if (!Reader || !ChannelMaps.IsValidIndex(Channel))
	{
		return false;
	}
	FChannelMap& Map = ChannelMaps[Channel];
	if (FMath::Abs(Map.Value - Value) <= 1e-7)
	{
		return true;
	}
	std::string Why;
	if (!WriteInput(TCHAR_TO_UTF8(*Map.Path), Value, &Why))
	{
		UE_LOG(LogRigExec, Warning, TEXT("%s: %s"), *Map.Path, UTF8_TO_TCHAR(Why.c_str()));
		return false;
	}
	Map.Value = Value;
	return true;
}

FName
URigExecComponent::GetControlElement(int32 Control) const
{
	return ControlMaps.IsValidIndex(Control) ? ControlMaps[Control].Element : NAME_None;
}

bool
URigExecComponent::GetControlAvars(int32 Control, double Out[9], bool bDefault) const
{
	if (!ControlMaps.IsValidIndex(Control) || ControlMaps[Control].bNull)
	{
		return false;
	}
	FMemory::Memcpy(Out, bDefault ? ControlMaps[Control].Defaults : ControlMaps[Control].Values, sizeof(double) * 9);
	return true;
}

uint16
URigExecComponent::GetControlAvarMask(int32 Control) const
{
	uint16 Mask = 0;
	for (int32 A = 0; ControlMaps.IsValidIndex(Control) && !ControlMaps[Control].bNull && A < 9; ++A)
	{
		Mask |= ControlMaps[Control].Channels[A] ? uint16(1u << A) : 0;
	}
	return Mask;
}

int32
URigExecComponent::SetControlAvars(int32 Control, const double In[9])
{
	if (!Reader || !ControlMaps.IsValidIndex(Control) || ControlMaps[Control].bNull)
	{
		return 0;
	}
	FControlMap& Map = ControlMaps[Control];
	int32 Changed = 0;
	for (int32 A = 0; A < 9; ++A)
	{
		if (!Map.Channels[A] || !FMath::IsFinite(In[A]) || FMath::Abs(In[A] - Map.Values[A]) <= 1e-7)
		{
			continue;
		}
		std::string Why;
		if (WriteInput(TCHAR_TO_UTF8(*(Map.Path + GTransformAvars[A])), In[A], &Why))
		{
			Map.Values[A] = In[A];
			++Changed;
		}
	}
	return Changed;
}

UControlRig*
URigExecComponent::SwitchRig() const
{
	return ControlRig ? ControlRig->GetControlRig() : nullptr;
}

double
URigExecComponent::ChannelNow(const FString& Path) const
{
	const UControlRig* Rig = SwitchRig();
	const FName Element = FindChannelElement(Path);
	const FRigElementKey Key(Element, ERigElementType::Control);
	if (Rig && !Element.IsNone() && Rig->GetHierarchy()->Contains(Key))
	{
		return Rig->GetHierarchy()->GetControlValue(Key).Get<float>();
	}
	return GetChannelValue(FindChannelIndex(Path));
}

void
URigExecComponent::WriteChannel(const FString& Path, double Value)
{
	UControlRig* Rig = SwitchRig();
	const FName Element = FindChannelElement(Path);
	if (Rig && !Element.IsNone() && Rig->GetHierarchy()->Contains(FRigElementKey(Element, ERigElementType::Control)))
	{
		Rig->SetControlValue<float>(Element, float(Value), true, FRigControlModifiedContext(EControlRigSetKey::DoNotCare), true);
		return;
	}
	SetChannelValue(FindChannelIndex(Path), Value);
}

void
URigExecComponent::ClearChannels()
{
	// Never touch the reader while a run is in flight.
	Settle();
	if (Reader)
	{
		Reader->ResetInputs();
	}
	// Back to the described defaults, so the next pull resends what moved.
	for (FControlMap& Map : ControlMaps)
	{
		FMemory::Memcpy(Map.Values, Map.Defaults, sizeof(Map.Values));
	}
	for (FChannelMap& Map : ChannelMaps)
	{
		Map.Value = Map.Default;
	}
}

float
URigExecComponent::Evaluate()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_Evaluate);
	if (!Reader || !FrameCache)
	{
		return -1.0f;
	}
	// A lower rate: skip this one, the inputs wait for the next.
	if (EvaluateEvery > 1 && (++SkippedEvaluations % EvaluateEvery) != 0)
	{
		return LastExecuteMs;
	}
	if (bEvaluateAsync && !bEvaluateNow)
	{
		// Off the game thread. A run still going keeps going: the inputs
		// written meanwhile wait for the next one, and the game does not wait
		// at all, so a heavy rig lowers its own rate, not the frame rate.
		// Readers use the last landed run.
		if (FrameCache->bPending && !FrameCache->Pending.IsCompleted())
		{
			return LastExecuteMs;
		}
		Settle();
		SnapshotLaunched();
		FRigExecFrames* Frames = FrameCache;
		rigExec::RigExecRuntimeReader* Run = Reader;
		Frames->bPending = true;
		Frames->Pending = UE::Tasks::Launch(UE_SOURCE_LOCATION,
		                                    [Frames, Run]()
		                                    {
			                                    TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_Execute);
			                                    const double Start = FPlatformTime::Seconds();
			                                    Frames->Error.clear();
			                                    Frames->bOk = Run->Execute(&Frames->Error);
			                                    Frames->ExecuteMs = float((FPlatformTime::Seconds() - Start) * 1000.0);
		                                    });
		return LastExecuteMs;
	}
	// On the game thread: a run still in flight lands first, and the inputs
	// queued behind it go in, so this run sees every write made so far.
	Settle();
	const double Start = FPlatformTime::Seconds();
	SnapshotLaunched();
	bool bExecuted;
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_Execute);
		FrameCache->Error.clear();
		bExecuted = Reader->Execute(&FrameCache->Error);
	}
	FrameCache->ExecuteMs = float((FPlatformTime::Seconds() - Start) * 1000.0);
	Publish(bExecuted);
	return float((FPlatformTime::Seconds() - Start) * 1000.0);
}

void
URigExecComponent::Settle()
{
	if (FrameCache && FrameCache->bPending)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_Settle);
		FrameCache->Pending.Wait();
		FrameCache->bPending = false;
		Publish(FrameCache->bOk);
	}
	FlushInputs();
}

bool
URigExecComponent::LandIfDone()
{
	if (FrameCache && FrameCache->bPending && FrameCache->Pending.IsCompleted())
	{
		Settle();
		return true;
	}
	return false;
}

bool
URigExecComponent::IsRunPending() const
{
	return FrameCache && FrameCache->bPending;
}

bool
URigExecComponent::WriteInput(const char* Name, double Value, std::string* Why)
{
	// A run is in flight: the write waits for the next one rather than the
	// game waiting for this one. The latest value per input wins.
	if (FrameCache && FrameCache->bPending)
	{
		QueuedInputs.Add(FString(UTF8_TO_TCHAR(Name)), Value);
		return true;
	}
	return Reader && Reader->SetInput(Name, Value, Why);
}

void
URigExecComponent::FlushInputs()
{
	if (!Reader || QueuedInputs.IsEmpty())
	{
		return;
	}
	for (const TPair<FString, double>& Input : QueuedInputs)
	{
		std::string Why;
		if (!Reader->SetInput(TCHAR_TO_UTF8(*Input.Key), Input.Value, &Why))
		{
			UE_LOG(LogRigExec, Warning, TEXT("%s: %s"), *Input.Key, UTF8_TO_TCHAR(Why.c_str()));
		}
	}
	QueuedInputs.Reset();
}

void
URigExecComponent::Publish(bool bExecuted)
{
	const double Start = FPlatformTime::Seconds();
	LastExecuteMs = FrameCache->ExecuteMs;
	if (!bExecuted)
	{
		UE_LOG(LogRigExec, Error, TEXT("evaluation failed: %s"), UTF8_TO_TCHAR(FrameCache->Error.c_str()));
		return;
	}
	for (FControlMap& Map : ControlMaps)
	{
		FMemory::Memcpy(Map.Landed, Map.Launched, sizeof(Map.Launched));
	}
	++PublishCount;
	// What readers ask for between runs, taken now so they never touch the
	// reader while another run is in flight.
	FrameCache->Primvars = Reader->GetMatrixPrimvars();
	FrameCache->bHavePrimvars = true;
	FrameCache->Published = Reader->GetProviderFramePublications();
	if (FrameCache->Index.Num() != int32(FrameCache->Published.size()))
	{
		FrameCache->Index.Reset();
		for (int32 I = 0; I < int32(FrameCache->Published.size()); ++I)
		{
			FrameCache->Index.Add(UTF8_TO_TCHAR(FrameCache->Published[size_t(I)].path.c_str()), I);
		}
	}
	FrameCache->bStale = false;
	const auto& Points = Reader->GetPoints();
	FRigExecMeshSet Units;
	Units.bZUp = bStageZUp;
	Units.MetersPerUnit = StageMetersPerUnit;
	for (int32 M = 0; M < Meshes.Num(); ++M)
	{
		const auto& Rig = Points[size_t(PointSlots[M])].points;
		// Three cases. The GPU skin draws it: the data mesh takes the points
		// quietly (picking and Touch Pose read them) and the skin uploads them.
		// Hidden, another renderer draws it (a subdivision surface, from these
		// points on the GPU): positions only, with the change events it
		// listens for. Otherwise the CPU fallback: positions and normals.
		TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_UpdateMesh);
		const bool bSkin = SkinDraws(M);
		const bool bDrawn = !bGpu && Meshes[M]->IsVisible();
		MeshDrawn[M] = bDrawn;
		Meshes[M]->GetDynamicMesh()->EditMesh(
			[&](FDynamicMesh3& Mesh)
			{
				const int32 Count = FMath::Min(Mesh.MaxVertexID(), int32(Rig.size()));
				for (int32 V = 0; V < Count; ++V)
				{
					const auto& P = Rig[size_t(V)];
					Mesh.SetVertex(V, Units.ToUnreal(P[0], P[1], P[2]));
				}
				// Normals from the deformed points every evaluation -- skin,
				// blend shapes and every other deformer included, as a
				// skeletal mesh's recompute-normals option does for morphs.
				if (bDrawn)
				{
					FMeshNormals::QuickRecomputeOverlayNormals(Mesh);
				}
			},
			// Positions AND normals changed, or the renderer keeps the rest
			// normals and the shading lags the shape.
			EDynamicMeshChangeType::DeformationEdit,
			bDrawn ? EDynamicMeshAttributeChangeFlags::VertexPositions | EDynamicMeshAttributeChangeFlags::NormalsTangents
			       : EDynamicMeshAttributeChangeFlags::VertexPositions,
			/*bDeferChangeEvents*/ bSkin);
		if (bSkin)
		{
			Skins[M]->UpdatePositions(*Meshes[M]->GetMesh());
		}
	}
	LastPoseTime = FPlatformTime::Seconds();
	LastPublishMs = float((FPlatformTime::Seconds() - Start) * 1000.0);
	// The skin moved: picks need a new tree. The highlight is in the
	// meshes' colours and moves with them.
	if (Touch)
	{
		for (FRigExecTouch::FMesh& Touched : Touch->Meshes)
		{
			Touched.bTreeDirty = true;
		}
	}
}

bool
URigExecComponent::SkinDraws(int32 M) const
{
	return bGpu && Skins.IsValidIndex(M) && Skins[M] && Meshes.IsValidIndex(M) && Meshes[M] && Meshes[M]->IsVisible();
}

void
URigExecComponent::SetUpRenderPath()
{
	bGpu = false;
	FallbackReason.Reset();
	if (!bDrawOnGPU)
	{
		FallbackReason = TEXT("Draw On GPU is off for this component");
	}
	else if (RigExecShaders::CanUseGpuSkin(&FallbackReason))
	{
		bGpu = GetOwner() != nullptr;
		if (!bGpu)
		{
			FallbackReason = TEXT("the component has no actor to put its GPU meshes on");
		}
	}
	int32 RenderVertices = 0;
	for (int32 M = 0; bGpu && M < Meshes.Num(); ++M)
	{
		UDynamicMeshComponent* Data = Meshes[M];
		URigExecSkinComponent* Skin = NewObject<URigExecSkinComponent>(GetOwner(), *(Data->GetName() + TEXT("_GPU")), RF_Transient);
		Skin->SetMobility(EComponentMobility::Movable);
		Skin->bMotionBlur = bMotionBlur;
		Skin->SetupAttachment(Data);
		TArray<UMaterialInterface*> Materials;
		for (int32 Slot = 0; Slot < Data->GetNumMaterials(); ++Slot)
		{
			Materials.Add(Data->GetMaterial(Slot));
		}
		Skin->Build(*Data->GetMesh(), Materials);
		Skin->RegisterComponent();
		Skin->SetVisibility(Data->IsVisible());
		RenderVertices += Skin->GetRenderVertexCount();
		Skins.Add(Skin);
		// The data mesh stays -- picking, Touch Pose and a subdivision surface
		// read it, and its visibility says whether anything else draws it --
		// but no longer draws itself.
		Data->SetRenderInMainPass(false);
		Data->SetRenderInDepthPass(false);
		Data->SetCastShadow(false);
		Data->SetVisibleInRayTracing(false);
	}
	if (bGpu)
	{
		RenderPath = FString::Printf(TEXT("GPU: %d meshes, %d vertices; each pose uploads the points once and the GPU computes normals%s"),
		                             Skins.Num(), RenderVertices, bMotionBlur ? TEXT(", with motion vectors") : TEXT(""));
		UE_LOG(LogRigExec, Log, TEXT("%s draws on the GPU (%d meshes, %d render vertices)"), *GetPathName(), Skins.Num(), RenderVertices);
		return;
	}
	RenderPath = TEXT("CPU fallback: ") + FallbackReason;
	if (bDrawOnGPU)
	{
		UE_LOG(LogRigExec, Warning, TEXT("%s draws on the CPU: %s"), *GetPathName(), *FallbackReason);
		OnRenderFallback.Broadcast(this, FallbackReason);
	}
	else
	{
		UE_LOG(LogRigExec, Log, TEXT("%s draws on the CPU: %s"), *GetPathName(), *FallbackReason);
	}
}

#if WITH_EDITOR
void
URigExecComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	// The render path is chosen when the meshes are built: rebuild them.
	const FName Changed = PropertyChangedEvent.GetMemberPropertyName();
	if ((Changed == GET_MEMBER_NAME_CHECKED(URigExecComponent, bDrawOnGPU) ||
	     Changed == GET_MEMBER_NAME_CHECKED(URigExecComponent, bMotionBlur)) && Reader)
	{
		Load();
	}
}
#endif

FVector
URigExecComponent::GetVertex(int32 Mesh, int32 Index) const
{
	if (!Meshes.IsValidIndex(Mesh) || !Meshes[Mesh])
	{
		return FVector::ZeroVector;
	}
	const FDynamicMesh3* Built = Meshes[Mesh]->GetMesh();
	return Built && Built->IsVertex(Index) ? FVector(Built->GetVertex(Index)) : FVector::ZeroVector;
}

bool
URigExecComponent::LoadControls(const FString& Text, const FString& Source)
{
	// Never touch the reader while a run is in flight.
	Settle();
	ControlMaps.Reset();
	ChannelMaps.Reset();
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root)
	{
		UE_LOG(LogRigExec, Error, TEXT("cannot read the controls of %s"), *Source);
		return false;
	}
	// The Control Rig is in USD space (build_control_rig.py): every control
	// keeps its USD offset and value as authored, and one root null converts
	// the whole asset into Unreal's space.
	FString Space, RootName;
	if (!Root->TryGetStringField(TEXT("space"), Space) || Space != TEXT("usd"))
	{
		UE_LOG(LogRigExec, Error,
		       TEXT("%s is not a USD-space controls file; rebuild the Control Rig with build_control_rig.py"),
		       *Source);
		return false;
	}
	UsdRoot = FName(Root->TryGetStringField(TEXT("root"), RootName) ? RootName : FString(TEXT("UsdSpace")));
	const TArray<TSharedPtr<FJsonValue>>* Placement = nullptr;
	if (Root->TryGetArrayField(TEXT("assetToWorld"), Placement) && Placement->Num() == 16)
	{
		for (int32 I = 0; I < 16; ++I)
		{
			AssetToWorld[I] = (*Placement)[I]->AsNumber();
		}
	}
	TArray<FString> Paths;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("controls")))
	{
		const TSharedPtr<FJsonObject>& Control = Value->AsObject();
		FControlMap& Map = ControlMaps.AddDefaulted_GetRef();
		Map.Element = FName(Control->GetStringField(TEXT("element")));
		Map.Path = Control->GetStringField(TEXT("path"));
		Map.Parent = int32(Control->GetNumberField(TEXT("parent")));
		FString Kind;
		Map.bNull = Control->TryGetStringField(TEXT("kind"), Kind) && Kind == TEXT("null");
		Paths.Add(Map.Path);
		const FString Order = Control->GetStringField(TEXT("rotationOrder"));
		for (int32 O = 0; O < 6; ++O)
		{
			if (Order == GOrders[O])
			{
				Map.Order = O;
			}
		}
		const auto& Sign = Control->GetArrayField(TEXT("rotationSign"));
		const auto& Defaults = Control->GetArrayField(TEXT("defaults"));
		const TCHAR* const Groups[3] = {TEXT("translate"), TEXT("rotate"), TEXT("scale")};
		for (int32 A = 0; A < 9; ++A)
		{
			Map.Channels[A] = Control->GetArrayField(Groups[A / 3])[A % 3]->AsBool();
			Map.Values[A] = Map.Defaults[A] = Defaults[A]->AsNumber();
		}
		for (int32 A = 0; A < 3; ++A)
		{
			Map.Sign[A] = Sign[A]->AsNumber();
		}
		Map.UnitScale = Control->GetNumberField(TEXT("unitScale"));
	}
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("channels")))
	{
		const TSharedPtr<FJsonObject>& Channel = Value->AsObject();
		FChannelMap& Map = ChannelMaps.AddDefaulted_GetRef();
		Map.Element = FName(Channel->GetStringField(TEXT("element")));
		Map.Path = Paths[int32(Channel->GetNumberField(TEXT("control")))] + TEXT(".") +
		           Channel->GetStringField(TEXT("attr"));
		Map.Value = Map.Default = Channel->GetNumberField(TEXT("default"));
		const TArray<TSharedPtr<FJsonValue>>* Labels = nullptr;
		if (Channel->TryGetArrayField(TEXT("labels"), Labels))
		{
			for (const TSharedPtr<FJsonValue>& Label : *Labels)
			{
				Map.Labels.Add(Label->AsString());
			}
		}
	}
	// The binary runtime takes transform avars and property chain inputs
	// only; a channel it refuses (a mode switch read at bake time) cannot
	// pose it and is dropped.
	if (Reader)
	{
		const int32 Described = ChannelMaps.Num();
		TArray<FString> Dropped;
		ChannelMaps.RemoveAll(
			[this, &Dropped](const FChannelMap& Map)
			{
				if (Reader->SetInput(TCHAR_TO_UTF8(*Map.Path), Map.Value, nullptr))
				{
					return false;
				}
				Dropped.Add(FPaths::GetExtension(Map.Path.Replace(TEXT(":"), TEXT("_"))) + TEXT(" on ") +
				            FPaths::GetBaseFilename(Map.Path.Left(Map.Path.Find(TEXT("."), ESearchCase::CaseSensitive,
				                                                            ESearchDir::FromEnd))));
				return true;
			});
		Reader->ResetInputs();
		if (Dropped.Num() > 0)
		{
			UE_LOG(LogRigExec, Log, TEXT("%d of %d channels are not live in %s: %s"), Dropped.Num(), Described,
			       *SourceName, *FString::Join(Dropped, TEXT(", ")));
		}
	}
	// Each control's fade, once the channels the runtime takes are known.
	{
		const TArray<TSharedPtr<FJsonValue>>& Controls = Root->GetArrayField(TEXT("controls"));
		for (int32 C = 0; C < Controls.Num() && C < ControlMaps.Num(); ++C)
		{
			const TSharedPtr<FJsonObject>* Fade = nullptr;
			if (!Controls[C]->AsObject()->TryGetObjectField(TEXT("fade"), Fade))
			{
				continue;
			}
			const FString FadeSource = (*Fade)->GetStringField(TEXT("source"));
			ControlMaps[C].bFadeInvert = (*Fade)->GetBoolField(TEXT("invert"));
			for (int32 K = 0; K < ChannelMaps.Num(); ++K)
			{
				if (ChannelMaps[K].Path == FadeSource)
				{
					ControlMaps[C].FadeChannel = K;
				}
			}
		}
	}
	// The IK/FK limbs, from their match constraints, for switching.
	LimbMaps.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Limbs = nullptr;
	if (Root->TryGetArrayField(TEXT("limbs"), Limbs))
	{
		auto Strings = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Field) {
			TArray<FString> Out;
			for (const TSharedPtr<FJsonValue>& V : Object->GetArrayField(Field))
			{
				Out.Add(V->AsString());
			}
			return Out;
		};
		for (const TSharedPtr<FJsonValue>& Value : *Limbs)
		{
			const TSharedPtr<FJsonObject> L = Value->AsObject();
			FLimbMap Limb;
			L->TryGetStringField(TEXT("match"), Limb.Match);
			Limb.Switch = L->GetStringField(TEXT("switch"));
			Limb.IkValue = L->GetNumberField(TEXT("ikValue"));
			Limb.FkControls = Strings(L, TEXT("fkControls"));
			Limb.IkControl = L->GetStringField(TEXT("ikControl"));
			Limb.Pole = L->GetStringField(TEXT("pole"));
			L->TryGetStringField(TEXT("upperScale"), Limb.UpperScale);
			L->TryGetStringField(TEXT("lowerScale"), Limb.LowerScale);
			L->TryGetStringField(TEXT("pin"), Limb.Pin);
			L->TryGetStringField(TEXT("twist"), Limb.Twist);
			const TArray<TSharedPtr<FJsonValue>>* Writable = nullptr;
			if (L->TryGetArrayField(TEXT("writable"), Writable))
			{
				for (const TSharedPtr<FJsonValue>& Path : *Writable)
				{
					Limb.Writable.Add(Path->AsString());
				}
			}
			if (!Limb.Match.IsEmpty() && Limb.FkControls.Num() == 3)
			{
				LimbMaps.Add(MoveTemp(Limb));
			}
		}
	}
	// The values the loaded pose was evaluated with.
	for (FControlMap& Map : ControlMaps)
	{
		FMemory::Memcpy(Map.Launched, Map.Values, sizeof(Map.Values));
		FMemory::Memcpy(Map.Landed, Map.Values, sizeof(Map.Values));
	}
	return true;
}

int32
URigExecComponent::PullControls()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_PullControls);
	if (!Reader)
	{
		return 0;
	}
	if (!ControlRig && GetOwner())
	{
		ControlRig = GetOwner()->FindComponentByClass<UControlRigComponent>();
	}
	UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	URigHierarchy* Hierarchy = Rig ? Rig->GetHierarchy() : nullptr;
	if (!Hierarchy)
	{
		return 0;
	}
	// Change test, in avar units: degrees and rig distance. A resync sends
	// everything whatever it held.
	constexpr double Tolerance = 1e-5;
	const bool bForce = bResync;
	bResync = false;
	if (bForce)
	{
		bPlaced = false;
		if (Touch)
		{
			// Unknown, so every region's colour is written again.
			Touch->Written.Init(uint8(TouchStateCount + 1), Touch->Regions.Num());
		}
	}
	int32 Changed = 0;
	auto Send = [&](const FString& Path, double& Held, double Value)
	{
		if (!bForce && FMath::Abs(Value - Held) <= Tolerance)
		{
			return;
		}
		std::string Why;
		if (WriteInput(TCHAR_TO_UTF8(*Path), Value, &Why))
		{
			Held = Value;
			++Changed;
		}
	};
	for (FControlMap& Map : ControlMaps)
	{
		const FRigElementKey Key(Map.Element, ERigElementType::Control);
		if (Map.bNull || !Hierarchy->Contains(Key))
		{
			continue;
		}
		// The control's value IS the avars matrix (USD space, centimetres):
		// read as a local.
		FMatrix Value = Hierarchy->GetLocalTransform(Key).ToMatrixWithScale();
		for (int32 K = 0; K < 3; ++K)
		{
			Value.M[3][K] /= CentimetresPerUnit;
		}
		Changed += SendControl(Map, Value, bForce);
	}
	for (FChannelMap& Map : ChannelMaps)
	{
		const FRigElementKey Key(Map.Element, ERigElementType::Control);
		if (Hierarchy->Contains(Key))
		{
			Send(Map.Path, Map.Value, double(Hierarchy->GetControlValue(Key).Get<float>()));
		}
	}
	UpdateFadedControls(Hierarchy);
	if (Changed > 0 || bForce)
	{
		Evaluate();
	}
	// A run that finished off the game thread lands now, without waiting:
	// the gizmos are placed from it below.
	if (FrameCache && FrameCache->bPending && FrameCache->Pending.IsCompleted())
	{
		Settle();
	}
	if (bForce)
	{
		RefreshTouchColors();
	}
	// The gizmos follow the rig for the animator; a game draws none. They
	// are placed whenever a value moved and again whenever a run lands: a
	// run off the game thread lands after the values it was launched with,
	// so the last one of a drag lands after the drag -- placing only on a
	// change left the gizmos one run behind until the next edit, which
	// then snapped them.
	const bool bGame = GetWorld() && GetWorld()->IsGameWorld();
	if ((Changed > 0 || !bPlaced || PlacedPublish != PublishCount) && !bGame)
	{
		PlaceControls();
		bPlaced = true;
		PlacedPublish = PublishCount;
	}
	return Changed;
}

void
URigExecComponent::UpdateFadedControls(URigHierarchy* Hierarchy)
{
	// A limb's controls fade with its IK/FK dial, as the viewport fade does
	// in usdview. The idle half is drawn on the limb (the rig publishes its
	// frames there), so it stays selectable, dimmed, and selecting one
	// switches the limb to it (FindPresentedLimbFor); with
	// bShowIdleLimbHalf off it is hidden instead.
	UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	if (ShapeColorsRig.Get() != Rig)
	{
		ShapeColors.Reset();
		ShapeColorsRig = Rig;
	}
	for (FControlMap& Map : ControlMaps)
	{
		if (Map.FadeChannel < 0 || Map.bNull || !ChannelMaps.IsValidIndex(Map.FadeChannel))
		{
			continue;
		}
		const double Weight = ChannelMaps[Map.FadeChannel].Value;
		const double Opacity = Map.bFadeInvert ? 1.0 - Weight : Weight;
		// 1 driving, 0 hidden, 2 idle and dimmed.
		const int8 Shown = Opacity > 1e-3 ? 1 : (bShowIdleLimbHalf ? 2 : 0);
		if (Shown == Map.Shown)
		{
			continue;
		}
		const FRigElementKey Key(Map.Element, ERigElementType::Control);
		FRigControlElement* Element = Hierarchy->Find<FRigControlElement>(Key);
		if (!Element)
		{
			continue;
		}
		Map.Shown = Shown;
		const FLinearColor Built = ShapeColors.FindOrAdd(Map.Element, Element->Settings.ShapeColor);
		const FLinearColor Color =
			Shown == 2 ? FLinearColor(Built.R * 0.35f, Built.G * 0.35f, Built.B * 0.35f, Built.A * 0.5f) : Built;
		if (Element->Settings.ShapeColor != Color)
		{
			FRigControlSettings Settings = Element->Settings;
			Settings.ShapeColor = Color;
			Hierarchy->SetControlSettings(Element, Settings, false, true, false);
		}
		Hierarchy->SetControlVisibility(Key, Shown != 0);
	}
}

int32
URigExecComponent::SendControl(FControlMap& Map, const FMatrix& Value, bool bForce)
{
	// Change test, in avar units: degrees and rig distance.
	constexpr double Tolerance = 1e-5;
	int32 Changed = 0;
	auto Send = [&](const FString& Path, double& Held, double V)
	{
		if (!bForce && FMath::Abs(V - Held) <= Tolerance)
		{
			return;
		}
		std::string Why;
		if (WriteInput(TCHAR_TO_UTF8(*Path), V, &Why))
		{
			Held = V;
			++Changed;
		}
	};
	FRot3 Rot;
	double Scale[3], Translate[3];
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Col = 0; Col < 3; ++Col)
		{
			Rot.M[Row][Col] = Value.M[Row][Col];
		}
		Scale[Row] = FMath::Sqrt(Rot.M[Row][0] * Rot.M[Row][0] + Rot.M[Row][1] * Rot.M[Row][1] +
		                         Rot.M[Row][2] * Rot.M[Row][2]);
		for (int32 Col = 0; Col < 3; ++Col)
		{
			Rot.M[Row][Col] /= FMath::Max(Scale[Row], 1e-12);
		}
		Translate[Row] = Value.M[3][Row];
	}
	// The sign multiplies the raw avar, so the hint is the effective
	// angle and the raw value is effective * sign.
	double Hint[3], Euler[3];
	for (int32 A = 0; A < 3; ++A)
	{
		Hint[A] = Map.Values[3 + A] * Map.Sign[A];
	}
	DecomposeEuler(Rot, Map.Order, Hint, Euler);
	for (int32 A = 0; A < 3; ++A)
	{
		if (Map.Channels[A])
		{
			Send(Map.Path + GTransformAvars[A], Map.Values[A], Translate[A] / Map.UnitScale);
		}
		if (Map.Channels[3 + A])
		{
			Send(Map.Path + GTransformAvars[3 + A], Map.Values[3 + A], Euler[A] * Map.Sign[A]);
		}
		if (Map.Channels[6 + A])
		{
			Send(Map.Path + GTransformAvars[6 + A], Map.Values[6 + A], Scale[A]);
		}
	}
	return Changed;
}

int32
URigExecComponent::FindControlIndex(const FString& NameOrPath) const
{
	const FName Name(*NameOrPath);
	for (int32 C = 0; C < ControlMaps.Num(); ++C)
	{
		if (ControlMaps[C].Path == NameOrPath || ControlMaps[C].Element == Name)
		{
			return C;
		}
	}
	// The last path component, for a description without element names.
	for (int32 C = 0; C < ControlMaps.Num(); ++C)
	{
		if (ControlMaps[C].Path.EndsWith(TEXT("/") + NameOrPath))
		{
			return C;
		}
	}
	return INDEX_NONE;
}

int32
URigExecComponent::GetControlParent(int32 Control) const
{
	return ControlMaps.IsValidIndex(Control) ? ControlMaps[Control].Parent : INDEX_NONE;
}

bool
URigExecComponent::IsControlNull(int32 Control) const
{
	return ControlMaps.IsValidIndex(Control) && ControlMaps[Control].bNull;
}

const FString&
URigExecComponent::GetControlPath(int32 Control) const
{
	static const FString None;
	return ControlMaps.IsValidIndex(Control) ? ControlMaps[Control].Path : None;
}

FMatrix
URigExecComponent::GetControlValueMatrix(int32 Control, bool bDefault) const
{
	if (!ControlMaps.IsValidIndex(Control) || ControlMaps[Control].bNull)
	{
		return FMatrix::Identity;
	}
	const FControlMap& Map = ControlMaps[Control];
	return ValueMatrix(Map, bDefault ? Map.Defaults : Map.Values);
}

FMatrix
URigExecComponent::GetLandedControlValueMatrix(int32 Control) const
{
	if (!ControlMaps.IsValidIndex(Control) || ControlMaps[Control].bNull)
	{
		return FMatrix::Identity;
	}
	return ValueMatrix(ControlMaps[Control], ControlMaps[Control].Landed);
}

void
URigExecComponent::SnapshotLaunched()
{
	for (FControlMap& Map : ControlMaps)
	{
		FMemory::Memcpy(Map.Launched, Map.Values, sizeof(Map.Values));
	}
}

FMatrix
URigExecComponent::ValueMatrix(const FControlMap& Map, const double* V) const
{
	// The inverse of SendControl: the rotations applied in the control's
	// order (row vectors, first axis first), each row scaled, then moved.
	const TCHAR* Name = GOrders[Map.Order];
	FRot3 Rot = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
	for (int32 Step = 0; Step < 3; ++Step)
	{
		const int32 Axis = Name[Step] - TEXT('X');
		Rot = Multiply(Rot, AxisRotation(Axis, FMath::DegreesToRadians(V[3 + Axis] * Map.Sign[Axis])));
	}
	FMatrix Out = FMatrix::Identity;
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Col = 0; Col < 3; ++Col)
		{
			Out.M[Row][Col] = Rot.M[Row][Col] * V[6 + Row];
		}
		Out.M[3][Row] = V[Row] * Map.UnitScale;
	}
	return Out;
}

bool
URigExecComponent::GetControlAssetFrame(int32 Control, FMatrix& Out) const
{
	return ControlMaps.IsValidIndex(Control) && AssetFrame(ControlMaps[Control].Path, Out);
}

int32
URigExecComponent::SetControlValueMatrix(int32 Control, const FMatrix& Value)
{
	if (!Reader || !ControlMaps.IsValidIndex(Control) || ControlMaps[Control].bNull)
	{
		return 0;
	}
	return SendControl(ControlMaps[Control], Value, false);
}

TArray<TPair<FString, double>>
URigExecComponent::GetLimbFkSwitches() const
{
	TArray<TPair<FString, double>> Out;
	for (const FLimbMap& Limb : LimbMaps)
	{
		// The switch's two ends are 0 and 1; IK is IkValue.
		Out.Emplace(Limb.Switch, Limb.IkValue >= 0.5 ? 0.0 : 1.0);
	}
	return Out;
}

bool
URigExecComponent::GetPickerText(FString& Out) const
{
	if (Character && !Character->Picker.IsEmpty())
	{
		Out = Character->Picker;
		return true;
	}
	const FString File = PickerFilePath();
	return !File.IsEmpty() && FFileHelper::LoadFileToString(Out, *File);
}

void
URigExecComponent::PlaceControls()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigExec_PlaceControls);
	UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	URigHierarchy* Hierarchy = Rig ? Rig->GetHierarchy() : nullptr;
	if (!Reader || !Hierarchy)
	{
		return;
	}
	// Each control's posed frame in Unreal's space, or none when the rig has
	// no slot for it (the gizmo then keeps its rest offset). The USD-space
	// root is read once, not once per control.
	const FMatrix RootGlobal = UsdRootGlobal();
	TArray<TOptional<FMatrix>> Frames;
	Frames.SetNum(ControlMaps.Num());
	for (int32 C = 0; C < ControlMaps.Num(); ++C)
	{
		FMatrix Asset;
		if (AssetFrame(ControlMaps[C].Path, Asset))
		{
			Frames[C] = ToUnreal(Asset, RootGlobal);
		}
	}
	// A parent's global once placed is its frame (parents are placed first):
	// take it from the frames rather than the hierarchy, which would
	// recompute it -- and its chain -- right after it was moved.
	auto ParentFrame = [&](int32 C) -> FMatrix
	{
		const int32 Parent = ControlMaps[C].Parent;
		if (Parent >= 0 && Frames[Parent].IsSet())
		{
			return Frames[Parent].GetValue();
		}
		return Parent >= 0 ? ParentGlobal(Hierarchy, C) : RootGlobal;
	};
	auto KeyOf = [this](int32 C)
	{
		return FRigElementKey(ControlMaps[C].Element,
		                      ControlMaps[C].bNull ? ERigElementType::Null : ERigElementType::Control);
	};
	for (int32 C = 0; C < ControlMaps.Num(); ++C)
	{
		const FControlMap& Map = ControlMaps[C];
		const FRigElementKey Key = KeyOf(C);
		if (!Frames[C].IsSet() || !Hierarchy->Contains(Key))
		{
			continue;
		}
		// Under the USD-space root every global is mirrored (the Y/Z swap);
		// locals are not. Everything is set as a local, so no FTransform ever
		// carries the mirror: a null's local is its frame in its parent's, a
		// control's offset its frame without its value.
		const FMatrix ParentInverse = ParentFrame(C).Inverse();
		if (Map.bNull)
		{
			Hierarchy->SetLocalTransform(Key, FTransform(Frames[C].GetValue() * ParentInverse), false, true);
			continue;
		}
		// The frame is the rig's for the values the last landed run saw. A
		// value sent since (a drag ahead of a run off the game thread) must
		// not be paired with it, or the gizmo is held on the old frame and
		// the drag fights it: the offset comes from the landed value, so the
		// gizmo shows the frame moved on by what changed since.
		FMatrix Value = Hierarchy->GetLocalTransform(Key).ToMatrixWithScale();
		if (FMemory::Memcmp(Map.Landed, Map.Values, sizeof(Map.Values)) != 0)
		{
			FMatrix Now = ValueMatrix(Map, Map.Values), Then = ValueMatrix(Map, Map.Landed);
			for (int32 K = 0; K < 3; ++K)
			{
				Now.M[3][K] *= CentimetresPerUnit;
				Then.M[3][K] *= CentimetresPerUnit;
			}
			Value = Value * Now.Inverse() * Then;
		}
		Hierarchy->SetControlOffsetTransform(Key, FTransform(Value.Inverse() * Frames[C].GetValue() * ParentInverse),
		                                     false, true);
	}
}

void
URigExecComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                 FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	// The GPU skins draw while their mesh is shown, and hide when another
	// renderer (a subdivision surface) hides it; shown again, a skin catches
	// up with the pose and colours it missed.
	for (int32 M = 0; bGpu && M < Skins.Num() && M < Meshes.Num(); ++M)
	{
		URigExecSkinComponent* Skin = Skins[M];
		const bool bShown = Meshes[M] && Meshes[M]->IsVisible();
		if (Skin && Skin->IsVisible() != bShown)
		{
			Skin->SetVisibility(bShown);
			if (bShown)
			{
				Skin->UpdatePositions(*Meshes[M]->GetMesh());
				Skin->UpdateColors(*Meshes[M]->GetMesh());
			}
		}
	}
	// A mesh another renderer drew is shown again: its normals were not kept.
	for (int32 M = 0; !bGpu && M < Meshes.Num() && M < MeshDrawn.Num(); ++M)
	{
		if (!MeshDrawn[M] && Meshes[M] && Meshes[M]->IsVisible())
		{
			MeshDrawn[M] = true;
			Meshes[M]->GetDynamicMesh()->EditMesh(
				[](FDynamicMesh3& Mesh) { FMeshNormals::QuickRecomputeOverlayNormals(Mesh); },
				EDynamicMeshChangeType::DeformationEdit,
				EDynamicMeshAttributeChangeFlags::VertexPositions | EDynamicMeshAttributeChangeFlags::NormalsTangents);
		}
	}
	if (bPullControlRig)
	{
		PullControls();
	}
}

void
URigExecComponent::OnRegister()
{
	Super::OnRegister();
#if WITH_EDITOR
	// An undo or redo can restore control values, meshes or colours behind
	// this component's back: resync after each one.
	if (!IsTemplate() && !UndoRedoHandle.IsValid())
	{
		UndoRedoHandle = FEditorDelegates::PostUndoRedo.AddUObject(this, &URigExecComponent::RequestResync);
	}
#endif
	if (!ControlRig && GetOwner())
	{
		ControlRig = GetOwner()->FindComponentByClass<UControlRigComponent>();
	}
	if (ControlRig)
	{
		PrimaryComponentTick.AddPrerequisite(ControlRig, ControlRig->PrimaryComponentTick);
	}
	// A saved level keeps the character or the file paths, not the meshes:
	// rebuild them.
	// A commandlet (a cook) only saves the level: nothing to draw.
	if (!IsTemplate() && !Reader && GetWorld() && !IsRunningCommandlet() &&
	    (Character || (!RigExecFile.FilePath.IsEmpty() && !StageFile.FilePath.IsEmpty())))
	{
		Load();
	}
}

void
URigExecComponent::RequestResync()
{
	// Now, for what the undo itself restored; and on the next tick, for what
	// Sequencer restores when it re-evaluates afterwards.
	bResync = true;
	PullControls();
	bResync = true;
}

void
URigExecComponent::OnUnregister()
{
	Settle();
#if WITH_EDITOR
	FEditorDelegates::PostUndoRedo.Remove(UndoRedoHandle);
	UndoRedoHandle.Reset();
#endif
	Super::OnUnregister();
}

FName
URigExecComponent::FindControlElement(const FString& Path) const
{
	for (const FControlMap& Map : ControlMaps)
	{
		if (Map.Path == Path)
		{
			return Map.Element;
		}
	}
	return NAME_None;
}

FName
URigExecComponent::FindChannelElement(const FString& AttrPath) const
{
	for (const FChannelMap& Map : ChannelMaps)
	{
		if (Map.Path == AttrPath)
		{
			return Map.Element;
		}
	}
	return NAME_None;
}

FString
URigExecComponent::PickerFilePath() const
{
	FString Controls = Resolve(ControlsFile);
	if (Controls.IsEmpty())
	{
		return FString();
	}
	// Biped_controls.json -> Biped_picker.json (anything after "_controls"
	// is ignored).
	FString Picker = Controls;
	const int32 At = Controls.Find(TEXT("_controls"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (At != INDEX_NONE && Controls.EndsWith(TEXT(".json")))
	{
		Picker = Controls.Left(At) + TEXT("_picker.json");
	}
	return Picker != Controls && FPaths::FileExists(Picker) ? Picker : FString();
}

bool
URigExecComponent::AssetFrame(const FString& Path, FMatrix& Out) const
{
	if (!Reader || !FrameCache)
	{
		return false;
	}
	// The last published run's frames; gathered here only before any run
	// has been published (Publish takes them otherwise).
	if (FrameCache->bStale && !FrameCache->bPending)
	{
		FrameCache->Published = Reader->GetProviderFramePublications();
		if (FrameCache->Index.Num() != int32(FrameCache->Published.size()))
		{
			FrameCache->Index.Reset();
			for (int32 I = 0; I < int32(FrameCache->Published.size()); ++I)
			{
				FrameCache->Index.Add(UTF8_TO_TCHAR(FrameCache->Published[size_t(I)].path.c_str()), I);
			}
		}
		FrameCache->bStale = false;
	}
	const int32* Slot = FrameCache->Index.Find(Path);
	if (!Slot || *Slot >= int32(FrameCache->Published.size()))
	{
		return false;
	}
	// The provider's final frame: origin and the three axis points. As a
	// row-vector matrix: the three handle vectors, then the origin.
	const rigExec::RrPointFrame& Frame = FrameCache->Published[size_t(*Slot)].final;
	const rigExec::RrVec3d& Origin = Frame.points[0];
	Out = FMatrix::Identity;
	for (int32 R = 0; R < 3; ++R)
	{
		for (int32 K = 0; K < 3; ++K)
		{
			Out.M[R][K] = Frame.points[size_t(R + 1)][K] - Origin[K];
		}
	}
	for (int32 K = 0; K < 3; ++K)
	{
		Out.M[3][K] = Origin[K];
	}
	return true;
}

TArray<FString>
URigExecComponent::GetMatrixPrimvarPaths() const
{
	TArray<FString> Out;
	if (Reader && FrameCache)
	{
		const auto& Primvars = FrameCache->bHavePrimvars ? FrameCache->Primvars : Reader->GetMatrixPrimvars();
		for (const rigExec::RigExecRuntimeMatrixPrimvar& Primvar : Primvars)
		{
			Out.Add(UTF8_TO_TCHAR(Primvar.path.c_str()));
		}
	}
	return Out;
}

bool
URigExecComponent::GetMatrixPrimvar(const FString& Path, FMatrix& Out) const
{
	if (!Reader)
	{
		return false;
	}
	const FTCHARToUTF8 Wanted(*Path);
	const auto& Primvars = FrameCache && FrameCache->bHavePrimvars ? FrameCache->Primvars : Reader->GetMatrixPrimvars();
	for (const rigExec::RigExecRuntimeMatrixPrimvar& Primvar : Primvars)
	{
		if (Primvar.path == Wanted.Get())
		{
			for (int32 R = 0; R < 4; ++R)
			{
				for (int32 C = 0; C < 4; ++C)
				{
					Out.M[R][C] = Primvar.matrix._mtx[R][C];
				}
			}
			return true;
		}
	}
	return false;
}

int32
URigExecComponent::FindMesh(const FString& PathOrName) const
{
	for (int32 M = 0; M < MeshPaths.Num(); ++M)
	{
		if (MeshPaths[M] == PathOrName || MeshPaths[M].EndsWith(TEXT("/") + PathOrName))
		{
			return M;
		}
	}
	return INDEX_NONE;
}

const FString&
URigExecComponent::GetMeshPath(int32 Mesh) const
{
	static const FString None;
	return MeshPaths.IsValidIndex(Mesh) ? MeshPaths[Mesh] : None;
}

TArray<UPrimitiveComponent*>
URigExecComponent::GetMeshDrawers(int32 Mesh) const
{
	TArray<UPrimitiveComponent*> Out;
	if (Meshes.IsValidIndex(Mesh) && Meshes[Mesh])
	{
		Out.Add(Meshes[Mesh]);
	}
	if (Skins.IsValidIndex(Mesh) && Skins[Mesh])
	{
		Out.Add(Skins[Mesh]);
	}
	return Out;
}

void
URigExecComponent::SetMeshMaterial(int32 Mesh, int32 Slot, UMaterialInterface* Material)
{
	for (UPrimitiveComponent* Drawer : GetMeshDrawers(Mesh))
	{
		Drawer->SetMaterial(Slot, Material);
	}
}

FMatrix
URigExecComponent::ToUnreal(const FMatrix& Asset) const
{
	return ToUnreal(Asset, UsdRootGlobal());
}

FMatrix
URigExecComponent::ToUnreal(const FMatrix& Asset, const FMatrix& RootGlobal) const
{
	double W[16];
	for (int32 R = 0; R < 4; ++R)
	{
		for (int32 K = 0; K < 4; ++K)
		{
			W[R * 4 + K] = Asset.M[R][0] * AssetToWorld[0 * 4 + K] + Asset.M[R][1] * AssetToWorld[1 * 4 + K] +
			               Asset.M[R][2] * AssetToWorld[2 * 4 + K] + Asset.M[R][3] * AssetToWorld[3 * 4 + K];
		}
	}
	// USD's own frame, in centimetres, then the root's conversion: the frame
	// the Control Rig shows under its USD-space root.
	FMatrix M = FMatrix::Identity;
	for (int32 R = 0; R < 3; ++R)
	{
		for (int32 K = 0; K < 3; ++K)
		{
			M.M[R][K] = W[R * 4 + K];
		}
		M.M[3][R] = W[12 + R] * CentimetresPerUnit;
	}
	return M * RootGlobal;
}

FMatrix
URigExecComponent::UsdRootGlobal() const
{
	const UControlRig* Rig = ControlRig ? ControlRig->GetControlRig() : nullptr;
	const URigHierarchy* Hierarchy = Rig ? Rig->GetHierarchy() : nullptr;
	const FRigElementKey Key(UsdRoot, ERigElementType::Null);
	if (Hierarchy && Hierarchy->Contains(Key))
	{
		return Hierarchy->GetGlobalTransform(Key).ToMatrixWithScale();
	}
	FMatrix Swapped = FMatrix::Identity;
	Swapped.M[1][1] = Swapped.M[2][2] = 0.0;
	Swapped.M[1][2] = Swapped.M[2][1] = 1.0;
	return Swapped;
}

FMatrix
URigExecComponent::ParentGlobal(const URigHierarchy* Hierarchy, int32 C) const
{
	const int32 Parent = ControlMaps[C].Parent;
	if (Parent >= 0)
	{
		const FRigElementKey Key(ControlMaps[Parent].Element,
		                         ControlMaps[Parent].bNull ? ERigElementType::Null : ERigElementType::Control);
		return Hierarchy->GetGlobalTransform(Key).ToMatrixWithScale();
	}
	return UsdRootGlobal();
}

FString
URigExecComponent::FindLimbSwitchFor(FName Element) const
{
	const FControlMap* Control =
		ControlMaps.FindByPredicate([Element](const FControlMap& M) { return !M.bNull && M.Element == Element; });
	if (!Control)
	{
		return FString();
	}
	const FString& Path = Control->Path;
	for (const FLimbMap& Limb : LimbMaps)
	{
		FString SwitchControl;
		Limb.Switch.Split(TEXT("."), &SwitchControl, nullptr);
		if (Path == SwitchControl || Limb.FkControls.Contains(Path) || Path == Limb.IkControl ||
		    (!Limb.Pole.IsEmpty() && Path == Limb.Pole) || Path.StartsWith(Limb.IkControl + TEXT("/")))
		{
			return Limb.Switch;
		}
	}
	return FString();
}

FString
URigExecComponent::FindPresentedLimbFor(FName Element) const
{
	const FControlMap* Control =
		ControlMaps.FindByPredicate([Element](const FControlMap& M) { return !M.bNull && M.Element == Element; });
	if (!Control)
	{
		return FString();
	}
	const FString& Path = Control->Path;
	auto Under = [&Path](const FString& Root)
	{ return !Root.IsEmpty() && (Path == Root || Path.StartsWith(Root + TEXT("/"))); };
	for (const FLimbMap& Limb : LimbMaps)
	{
		const bool bFk = Limb.FkControls.ContainsByPredicate(Under);
		const bool bIk = Under(Limb.IkControl) || Under(Limb.Pole);
		if ((!bFk && !bIk) || (SwitchRig() ? FindChannelElement(Limb.Switch).IsNone() : FindChannelIndex(Limb.Switch) == INDEX_NONE))
		{
			continue;
		}
		// Only at either end of the dial, as ikfkMatch.MatchHalf.Presented:
		// mid-blend both halves drive.
		const double Now = ChannelNow(Limb.Switch);
		const bool bInIk = FMath::Abs(Now - Limb.IkValue) <= 1e-4;
		const bool bInFk = FMath::Abs(Now - (1.0 - Limb.IkValue)) <= 1e-4;
		if ((bFk && bInIk) || (bIk && bInFk))
		{
			return Limb.Switch;
		}
	}
	return FString();
}

bool
URigExecComponent::IsLimbIk(const FString& SwitchPath) const
{
	const FLimbMap* Limb = LimbMaps.FindByPredicate([&](const FLimbMap& L) { return L.Switch == SwitchPath; });
	if (!Limb)
	{
		return false;
	}
	const double Now = ChannelNow(SwitchPath);
	return FMath::Abs(Now - Limb->IkValue) < FMath::Abs(Now - (1.0 - Limb->IkValue));
}

bool
URigExecComponent::FindSpaceChannel(FName Element, FName& OutChannel, TArray<FString>& OutLabels) const
{
	const FControlMap* Control =
		ControlMaps.FindByPredicate([Element](const FControlMap& M) { return !M.bNull && M.Element == Element; });
	if (!Control)
	{
		return false;
	}
	const FString Path = Control->Path + TEXT(".avars:space");
	const FChannelMap* Space = ChannelMaps.FindByPredicate([&Path](const FChannelMap& M) { return M.Path == Path; });
	if (!Space || Space->Labels.Num() < 2)
	{
		return false;
	}
	OutChannel = Space->Element;
	OutLabels = Space->Labels;
	return true;
}

bool
URigExecComponent::SwitchSpace(FName Element, int32 Index)
{
	FName Channel;
	TArray<FString> Labels;
	if (!Reader || !FindSpaceChannel(Element, Channel, Labels) || !Labels.IsValidIndex(Index))
	{
		return false;
	}
	const int32 C = FindControlIndex(Element.ToString());
	if (C == INDEX_NONE)
	{
		return false;
	}
	const FString SpacePath = ControlMaps[C].Path + TEXT(".avars:space");
	// The rig does the solving (usdview's spaceMatch.Plan): every run, each
	// space switch publishes the tx..rz that keep its control where it is in
	// every space it names. Read them for the pose on screen -- evaluated on
	// this thread, never a run still in flight -- and write the chosen
	// space's row with the space, as one edit.
	TGuardValue<bool> Now(bEvaluateNow, true);
	PullControls();
	Evaluate();
	const std::string Path(TCHAR_TO_UTF8(*ControlMaps[C].Path));
	const std::vector<double>* Row = nullptr;
	const std::vector<rigExec::RigExecRuntimeSpaceMatch> Matches = Reader->GetSpaceMatches();
	for (const rigExec::RigExecRuntimeSpaceMatch& M : Matches)
	{
		if (M.path == Path)
		{
			Row = &M.values;
			break;
		}
	}
	if (!Row || Row->size() < size_t(Index + 1) * 6)
	{
		UE_LOG(LogRigExec, Warning, TEXT("%s publishes no match for space %d; switching without matching"),
		       *ControlMaps[C].Path, Index);
		WriteChannel(SpacePath, double(Index));
		PullControls();
		if (!SwitchRig())
		{
			Evaluate();
		}
		return true;
	}
	// Scale is never written, and neither is a channel the control does not
	// expose (locked, or written by the rig).
	TMap<FString, double> Avars;
	for (int32 A = 0; A < 6; ++A)
	{
		const double V = (*Row)[size_t(Index * 6 + A)];
		if (ControlMaps[C].Channels[A] && FMath::IsFinite(V))
		{
			Avars.Add(GTransformAvars[A], V);
		}
	}
	SetControlAvars(ControlMaps[C].Path, Avars);
	WriteChannel(SpacePath, double(Index));
	PullControls();
	if (!SwitchRig())
	{
		Evaluate();
	}
	return true;
}

bool
URigExecComponent::SetControlAvars(const FString& Path, const TMap<FString, double>& Avars)
{
	UControlRig* Rig = SwitchRig();
	const int32 C = FindControlIndex(Path);
	if (C == INDEX_NONE || ControlMaps[C].bNull)
	{
		return false;
	}
	const FControlMap& Map = ControlMaps[C];
	double V[9];
	FMemory::Memcpy(V, Map.Values, sizeof(V));
	bool bAny = false;
	for (int32 A = 0; A < 6; ++A)
	{
		if (const double* Value = Avars.Find(GTransformAvars[A]))
		{
			V[A] = *Value;
			bAny = true;
		}
	}
	if (!bAny)
	{
		return true;
	}
	if (!Rig)
	{
		// No Control Rig (a game): straight to the rig.
		SetControlAvars(C, V);
		return true;
	}
	// The control's local IS its avars matrix (centimetres). Move it by the
	// change between the values held and the new ones, so whatever the
	// local carries outside the avars (a locked channel) stays.
	FMatrix Held = ValueMatrix(Map, Map.Values), Next = ValueMatrix(Map, V);
	for (int32 K = 0; K < 3; ++K)
	{
		Held.M[3][K] *= CentimetresPerUnit;
		Next.M[3][K] *= CentimetresPerUnit;
	}
	const FRigElementKey Key(Map.Element, ERigElementType::Control);
	const FMatrix Local = Rig->GetHierarchy()->GetLocalTransform(Key).ToMatrixWithScale() * Held.Inverse() * Next;
	Rig->SetControlLocalTransform(Map.Element, FTransform(Local), true,
	                              FRigControlModifiedContext(EControlRigSetKey::DoNotCare), true);
	return true;
}

bool
URigExecComponent::IsLimbSwitch(const FString& SwitchPath) const
{
	return LimbMaps.ContainsByPredicate([&](const FLimbMap& L) { return L.Switch == SwitchPath; });
}

bool
URigExecComponent::SwitchLimb(const FString& SwitchPath)
{
	// Never touch the reader while a run is in flight, and read the match
	// from a run of this pose, evaluated on this thread.
	TGuardValue<bool> Now(bEvaluateNow, true);
	Settle();
	const FLimbMap* Limb = LimbMaps.FindByPredicate([&](const FLimbMap& L) { return L.Switch == SwitchPath; });
	// Through the Control Rig in the editor; straight to the rig in a game.
	const bool bThroughRig = SwitchRig() != nullptr;
	if (!Limb || !Reader ||
	    (bThroughRig ? FindChannelElement(SwitchPath).IsNone() : FindChannelIndex(SwitchPath) == INDEX_NONE))
	{
		return false;
	}
	// Start from what the rig shows now, evaluated: the match outputs are
	// for the pose on screen.
	PullControls();
	Evaluate();
	const bool bToIk = !IsLimbIk(SwitchPath);
	const std::string MatchPath(TCHAR_TO_UTF8(*Limb->Match));
	const std::vector<rigExec::RigExecRuntimeIkFkMatch> Matches = Reader->GetIkFkMatches();
	const rigExec::RigExecRuntimeIkFkMatch* Match = nullptr;
	for (const rigExec::RigExecRuntimeIkFkMatch& M : Matches)
	{
		if (M.path == MatchPath)
		{
			Match = &M;
			break;
		}
	}
	const std::vector<double>* Values = Match ? (bToIk ? &Match->ikValues : &Match->fkValues) : nullptr;
	if (!Values || Values->size() != (bToIk ? 11u : 20u))
	{
		UE_LOG(LogRigExec, Warning, TEXT("%s published no %s values"), *Limb->Match, bToIk ? TEXT("IK") : TEXT("FK"));
		return false;
	}
	for (const double V : *Values)
	{
		if (!FMath::IsFinite(V))
		{
			return false;
		}
	}
	// {channel: value} for the incoming half, the writable channels only:
	// ikfkMatch.Match. IK: the IK control's translate and rotate, the
	// pole's translate, the two stretch scales. FK: each FK control's
	// translate and rotate, the two stretch scales, pin and twist cleared.
	TMap<FString, TMap<FString, double>> Transforms;
	TArray<TPair<FString, double>> Channels;
	auto Put = [&](const FString& Control, int32 First, int32 Count)
	{
		if (Control.IsEmpty())
		{
			return;
		}
		TMap<FString, double>& Avars = Transforms.FindOrAdd(Control);
		for (int32 I = 0; I < Count; ++I)
		{
			const FString Path = Control + GTransformAvars[I];
			if (Limb->Writable.Contains(Path))
			{
				Avars.Add(GTransformAvars[I], (*Values)[size_t(First + I)]);
			}
		}
	};
	auto Channel = [&](const FString& Path, double V)
	{
		if (!Path.IsEmpty() && Limb->Writable.Contains(Path))
		{
			Channels.Emplace(Path, V);
		}
	};
	if (bToIk)
	{
		Put(Limb->IkControl, 0, 6);
		Put(Limb->Pole, 6, 3);
		Channel(Limb->UpperScale, (*Values)[9]);
		Channel(Limb->LowerScale, (*Values)[10]);
	}
	else
	{
		for (int32 C = 0; C < 3; ++C)
		{
			Put(Limb->FkControls[C], C * 6, 6);
		}
		Channel(Limb->UpperScale, (*Values)[18]);
		Channel(Limb->LowerScale, (*Values)[19]);
		Channel(Limb->Pin, 0.0);
		Channel(Limb->Twist, 0.0);
	}
	// Straight onto the Control Rig's controls and channels, so they are
	// keyed and kept: each incoming control takes the match's channels, the
	// rest of it as it was.
	for (const TPair<FString, TMap<FString, double>>& Control : Transforms)
	{
		SetControlAvars(Control.Key, Control.Value);
	}
	for (const TPair<FString, double>& Write : Channels)
	{
		WriteChannel(Write.Key, Write.Value);
	}
	WriteChannel(SwitchPath, bToIk ? Limb->IkValue : 1.0 - Limb->IkValue);
	PullControls();
	if (!bThroughRig)
	{
		// No Control Rig to pull: the switched pose evaluates now.
		Evaluate();
	}
	return true;
}

FTransform
URigExecComponent::GetRigFrame(const FString& Path) const
{
	FMatrix Asset;
	return AssetFrame(Path, Asset) ? FTransform(ToUnreal(Asset)) : FTransform::Identity;
}

void
URigExecComponent::LoadTouchRegions(const FRigExecMeshSet& Set)
{
	if (!Touch)
	{
		return;
	}
	Touch->LeadColor = ScaleTo(Set.LeadColor.Get(GDefaultLeadColor), 0.95f);
	Touch->SelectedColor = ScaleTo(Set.SelectedColor.Get(GDefaultSelectedColor), 0.62f);
	TArray<FString> Missing;
	for (const FRigExecTouchRegionSource& Source : Set.Regions)
	{
		// Set.Meshes and Touch->Meshes line up unless a mesh the rig does not
		// pose was skipped: match by path.
		const int32 M = Touch->Meshes.IndexOfByPredicate(
			[&](const FRigExecTouch::FMesh& Mesh) { return Set.Meshes.IsValidIndex(Source.Mesh) && Mesh.Path == Set.Meshes[Source.Mesh].Path; });
		if (M == INDEX_NONE)
		{
			continue;
		}
		// Only an animator control can be selected; a rig helper is a null.
		// A region that selects nothing must not swallow the click.
		const FControlMap* Map = ControlMaps.FindByPredicate(
			[&Source](const FControlMap& C) { return !C.bNull && C.Path == Source.ControlPath; });
		if (!Map || Map->Element.IsNone())
		{
			if (!Source.ControlPath.IsEmpty())
			{
				Missing.Add(Source.Name);
			}
			continue;
		}
		FRigExecTouch::FMesh& Touched = Touch->Meshes[M];
		const int32 Region = Touch->Regions.Num();
		FRigExecTouch::FRegion& Out = Touch->Regions.AddDefaulted_GetRef();
		Out.Mesh = M;
		Out.Name = Source.Name;
		Out.ControlPath = Source.ControlPath;
		Out.Element = Map->Element;
		Out.HoverColor = ScaleTo(Source.HoverColor, 1.0f);
		for (int32 Face : Source.Faces)
		{
			if (Touched.FaceRegion.IsValidIndex(Face))
			{
				Touched.FaceRegion[Face] = Region;
				Out.Faces.Add(Face);
			}
		}
	}
	Touch->Written.Init(uint8(TouchStateCount), Touch->Regions.Num());
	UE_LOG(LogRigExec, Log, TEXT("%d touch regions"), Touch->Regions.Num());
	if (Missing.Num() > 0)
	{
		UE_LOG(LogRigExec, Log, TEXT("%d touch regions name no Control Rig control and are skipped: %s"), Missing.Num(),
		       *FString::Join(Missing, TEXT(", ")));
	}
}

int32
URigExecComponent::GetTouchRegionMesh(int32 Region) const
{
	return Touch && Touch->Regions.IsValidIndex(Region) ? Touch->Regions[Region].Mesh : -1;
}

int32
URigExecComponent::GetTouchRegionFaceCount(int32 Region) const
{
	return Touch && Touch->Regions.IsValidIndex(Region) ? Touch->Regions[Region].Faces.Num() : 0;
}

int32
URigExecComponent::GetTouchRegionCount() const
{
	return Touch ? Touch->Regions.Num() : 0;
}

FString
URigExecComponent::GetTouchRegionName(int32 Region) const
{
	return Touch && Touch->Regions.IsValidIndex(Region) ? Touch->Regions[Region].Name : FString();
}

FName
URigExecComponent::GetTouchRegionControl(int32 Region) const
{
	return Touch && Touch->Regions.IsValidIndex(Region) ? Touch->Regions[Region].Element : NAME_None;
}

TArray<int32>
URigExecComponent::GetTouchRegionsOf(const TArray<FName>& Controls) const
{
	TArray<int32> Out;
	for (int32 Region = 0; Touch && Region < Touch->Regions.Num(); ++Region)
	{
		if (Controls.Contains(Touch->Regions[Region].Element))
		{
			Out.Add(Region);
		}
	}
	return Out;
}

int32
URigExecComponent::PickTouchRegion(const FVector& RayOrigin, const FVector& RayDirection, double& OutDistance)
{
	OutDistance = 0.0;
	int32 Best = -1;
	double BestDistance = TNumericLimits<double>::Max();
	for (int32 M = 0; Touch && M < Touch->Meshes.Num() && M < Meshes.Num(); ++M)
	{
		FRigExecTouch::FMesh& Touched = Touch->Meshes[M];
		const FDynamicMesh3* Mesh = Meshes[M] ? Meshes[M]->GetMesh() : nullptr;
		if (Touched.FaceRegion.Num() == 0 || !Mesh)
		{
			continue;
		}
		// Built on the posed skin, and only again once the rig has moved it.
		if (!Touched.Tree || Touched.Tree->GetMesh() != Mesh)
		{
			Touched.Tree = MakeUnique<FDynamicMeshAABBTree3>(Mesh, false);
			Touched.bTreeDirty = true;
		}
		if (Touched.bTreeDirty)
		{
			Touched.Tree->Build();
			Touched.bTreeDirty = false;
		}
		const FTransform& ToWorld = Meshes[M]->GetComponentTransform();
		const FRay3d Ray(ToWorld.InverseTransformPosition(RayOrigin),
		                 ToWorld.InverseTransformVector(RayDirection).GetSafeNormal(), true);
		double T = 0.0;
		int Triangle = -1;
		if (!Touched.Tree->FindNearestHitTriangle(Ray, T, Triangle) || !Touched.TriangleFace.IsValidIndex(Triangle))
		{
			continue;
		}
		const double Distance = FVector::Dist(RayOrigin, ToWorld.TransformPosition(Ray.PointAt(T)));
		if (Distance < BestDistance)
		{
			// Unpainted skin still hides what is behind it.
			BestDistance = Distance;
			Best = Touched.FaceRegion[Touched.TriangleFace[Triangle]];
			OutDistance = Distance;
		}
	}
	return Best;
}

void
URigExecComponent::SetTouchHighlight(int32 Hover, int32 Lead, const TArray<int32>& Selected)
{
	if (!Touch)
	{
		return;
	}
	const bool bWasShown = Touch->IsShown();
	if (Touch->Hover == Hover && Touch->Lead == Lead && Touch->Selected == Selected)
	{
		return;
	}
	Touch->Hover = Hover;
	Touch->Lead = Lead;
	Touch->Selected = Selected;
	if (bWasShown || Touch->IsShown())
	{
		RefreshTouchColors();
	}
}

void
URigExecComponent::SetTouchSuspended(bool bSuspended)
{
	if (!Touch || Touch->bSuspended == bSuspended)
	{
		return;
	}
	Touch->bSuspended = bSuspended;
	RefreshTouchColors();
}

bool
URigExecComponent::IsTouchSuspended() const
{
	return Touch && Touch->bSuspended;
}

void
URigExecComponent::RefreshTouchColors()
{
	if (!Touch || Touch->Written.Num() != Touch->Regions.Num())
	{
		return;
	}
	// Each region's state now (suspended: none lit), by priority.
	TArray<uint8> State;
	State.Init(uint8(TouchStateCount), Touch->Regions.Num());
	if (!Touch->bSuspended)
	{
		for (int32 Region : Touch->Selected)
		{
			if (State.IsValidIndex(Region))
			{
				State[Region] = TouchSelected;
			}
		}
		if (State.IsValidIndex(Touch->Lead))
		{
			State[Touch->Lead] = TouchLead;
		}
		if (State.IsValidIndex(Touch->Hover))
		{
			State[Touch->Hover] = TouchHover;
		}
	}
	// Only the regions whose state changed are rewritten, mesh by mesh.
	TArray<TArray<int32>> Changed;
	Changed.SetNum(Meshes.Num());
	bool bAny = false;
	for (int32 Region = 0; Region < State.Num(); ++Region)
	{
		const int32 M = Touch->Regions[Region].Mesh;
		if (State[Region] != Touch->Written[Region] && Changed.IsValidIndex(M))
		{
			Changed[M].Add(Region);
			bAny = true;
		}
	}
	if (!bAny)
	{
		return;
	}
	for (int32 M = 0; M < Meshes.Num(); ++M)
	{
		if (Changed[M].Num() == 0 || !Meshes[M])
		{
			continue;
		}
		const FRigExecTouch::FMesh& Touched = Touch->Meshes[M];
		Meshes[M]->GetDynamicMesh()->EditMesh(
			[&](FDynamicMesh3& Mesh)
			{
				FDynamicMeshColorOverlay* Colors =
					Mesh.HasAttributes() && Mesh.Attributes()->HasPrimaryColors() ? Mesh.Attributes()->PrimaryColors() : nullptr;
				if (!Colors)
				{
					return;
				}
				for (int32 Region : Changed[M])
				{
					const FRigExecTouch::FRegion& R = Touch->Regions[Region];
					const FLinearColor C = State[Region] == TouchHover    ? R.HoverColor
					                       : State[Region] == TouchLead   ? Touch->LeadColor
					                       : State[Region] == TouchSelected ? Touch->SelectedColor
					                                                         : FLinearColor::White;
					const FVector4f Value =
						State[Region] == TouchStateCount ? GNoTint : FVector4f(C.R, C.G, C.B, GTouchOpacity);
					for (int32 Face : R.Faces)
					{
						for (int32 Triangle = Touched.FaceFirstTriangle[Face];
						     Triangle < Touched.FaceFirstTriangle[Face + 1]; ++Triangle)
						{
							const FIndex3i Elements = Colors->GetTriangle(Triangle);
							for (int32 Corner = 0; Corner < 3; ++Corner)
							{
								Colors->SetElement(Elements[Corner], Value);
							}
						}
					}
				}
			},
			EDynamicMeshChangeType::AttributeEdit, EDynamicMeshAttributeChangeFlags::VertexColors,
			/*bDeferChangeEvents*/ SkinDraws(M));
		if (SkinDraws(M))
		{
			Skins[M]->UpdateColors(*Meshes[M]->GetMesh());
		}
		for (int32 Region : Changed[M])
		{
			Touch->Written[Region] = State[Region];
		}
	}
}
