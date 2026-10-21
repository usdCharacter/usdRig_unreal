#include "RigExecEyeComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Paths.h"
#include "RigExecComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogRigExecEye, Log, All);

namespace
{
// The rig's dial slots (the projector's shaderDialSources order) and the
// Eye material parameter each drives. Sizes multiply (1 + dial), offsets add
// -- the rig's own eye shader's rule, so zero is neutral.
struct FDial
{
	const TCHAR* Parameter;
	bool bScale;
};
const FDial GDials[8] = {
	{TEXT("PupilWidth"), true},   {TEXT("PupilHeight"), true},  {TEXT("IrisWidth"), true},
	{TEXT("IrisHeight"), true},   {TEXT("PupilOffsetX"), false}, {TEXT("PupilOffsetY"), false},
	{nullptr, true} /* pupilRadius: Custom Primitive Data */, {TEXT("LimbusRadius"), true},
};
const int32 GPupilSlot = 6;
const int32 GIrisSlot = 7;

int32
SideOf(const FString& Path)
{
	const FString Name = FPaths::GetBaseFilename(Path).ToLower();
	if (Name.StartsWith(TEXT("l_")) || Name.Contains(TEXT("_l_")) || Name.EndsWith(TEXT("_l")) || Name.Contains(TEXT("left")))
	{
		return 0;
	}
	if (Name.StartsWith(TEXT("r_")) || Name.Contains(TEXT("_r_")) || Name.EndsWith(TEXT("_r")) || Name.Contains(TEXT("right")))
	{
		return 1;
	}
	return -1;
}
} // namespace

URigExecEyeComponent::URigExecEyeComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// After the rig has evaluated, wherever that happened this frame.
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	bTickInEditor = true;
	LeftMaterial = TSoftObjectPtr<UMaterialInterface>(
	    FSoftObjectPath(TEXT("/SquarebitEye/Materials/M_SquarebitEye_L.M_SquarebitEye_L")));
	RightMaterial = TSoftObjectPtr<UMaterialInterface>(
	    FSoftObjectPath(TEXT("/SquarebitEye/Materials/M_SquarebitEye_R.M_SquarebitEye_R")));
}

bool
URigExecEyeComponent::Bind()
{
	Eyes.Reset();
	Held.Reset();
	if (!Rig && GetOwner())
	{
		Rig = GetOwner()->FindComponentByClass<URigExecComponent>();
	}
	if (!Rig || Rig->GetMeshes().Num() == 0)
	{
		Status = TEXT("no loaded RigExec component");
		return false;
	}
	BoundLoad = Rig->GetLoadCount();
	UMaterialInterface* Masters[2] = {LeftMaterial.LoadSynchronous(), RightMaterial.LoadSynchronous()};
	if (!Masters[0] || !Masters[1])
	{
		Status = TEXT("the Squarebit Eye materials are not in the project (install the SquarebitEye plugin)");
		UE_LOG(LogRigExecEye, Warning, TEXT("%s: %s"), *GetPathName(), *Status);
		return false;
	}
	// Every mesh a projector publishes onto is an eye.
	const FString Suffix = TEXT(".primvars:") + ProjectorPrimvar;
	for (const FString& Primvar : Rig->GetMatrixPrimvarPaths())
	{
		if (!Primvar.EndsWith(Suffix))
		{
			continue;
		}
		const FString Prim = Primvar.LeftChop(Suffix.Len());
		const int32 Mesh = Rig->FindMesh(Prim);
		if (Mesh == INDEX_NONE)
		{
			UE_LOG(LogRigExecEye, Warning, TEXT("%s: a projector on %s, which the rig does not draw"), *GetPathName(), *Prim);
			continue;
		}
		FEye& Eye = Eyes.AddDefaulted_GetRef();
		Eye.Mesh = Mesh;
		Eye.Projector = Primvar;
		Eye.Dials = Prim + TEXT(".primvars:") + DialPrimvar;
		const int32 Side = SideOf(Prim);
		Eye.Index = Side >= 0 ? Side : (Eyes.Num() - 1) % 2;
	}
	if (Eyes.Num() == 0)
	{
		Status = FString::Printf(TEXT("the rig publishes no %s"), *ProjectorPrimvar);
		return false;
	}
	for (FEye& Eye : Eyes)
	{
		const TArray<UPrimitiveComponent*> Drawers = Rig->GetMeshDrawers(Eye.Mesh);
		const int32 Slots = Drawers.Num() > 0 ? Drawers[0]->GetNumMaterials() : 0;
		for (int32 Slot = 0; Slot < FMath::Max(Slots, 1); ++Slot)
		{
			UMaterialInstanceDynamic* Look = UMaterialInstanceDynamic::Create(Masters[Eye.Index], this);
			Look->SetScalarParameterValue(TEXT("RefractionMode"), float(RefractionMode));
			Rig->SetMeshMaterial(Eye.Mesh, Slot, Look);
			Eye.Looks.Add(Look);
			Held.Add(Look);
		}
	}
	Status = FString::Printf(TEXT("drawing %d eyes with Squarebit Eye"), Eyes.Num());
	UE_LOG(LogRigExecEye, Log, TEXT("%s: %s"), *GetPathName(), *Status);
	Update();
	return true;
}

void
URigExecEyeComponent::Update()
{
	if (!Rig || Eyes.Num() == 0)
	{
		return;
	}
	// A subdivision surface draws the eyes too and needs the same data.
	TArray<UPrimitiveComponent*> Subdivisions;
	if (AActor* Owner = GetOwner())
	{
		TArray<UPrimitiveComponent*> Primitives;
		Owner->GetComponents(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (Primitive->GetClass()->GetName().Contains(TEXT("SquarebitSubdiv")))
			{
				Subdivisions.Add(Primitive);
			}
		}
	}
	const FTransform ToWorld = Rig->GetComponentTransform();
	const double Cm = Rig->GetCentimetresPerUnit();
	for (FEye& Eye : Eyes)
	{
		FMatrix Asset;
		if (!Rig->GetMatrixPrimvar(Eye.Projector, Asset))
		{
			continue;
		}
		// The projector's rows: right, up and aim axes scaled by the eye's
		// radius, then its origin -- into the world.
		const FMatrix Local = Rig->AssetToComponent(Asset);
		auto Row = [&Local](int32 R) { return FVector(Local.M[R][0], Local.M[R][1], Local.M[R][2]); };
		const FVector Origin = ToWorld.TransformPosition(Row(3));
		const FVector Right = ToWorld.TransformVector(Row(0) * Cm);
		FVector Up = ToWorld.TransformVector(Row(1) * Cm);
		FVector Aim = ToWorld.TransformVector(Row(2) * Cm);
		const double ScaleX = Right.Size(), ScaleY = Up.Size(), ScaleZ = Aim.Size();
		if (!FMath::IsFinite(ScaleX + ScaleY + ScaleZ) || FMath::Min3(ScaleX, ScaleY, ScaleZ) < 1e-4 || Origin.ContainsNaN())
		{
			continue;
		}
		Aim /= ScaleZ;
		Up = Up - Aim * FVector::DotProduct(Up, Aim);
		if (!Up.Normalize(1e-6))
		{
			continue;
		}

		float Dial[8] = {0, 0, 0, 0, 0, 0, 0, 0};
		FMatrix Dials;
		if (Rig->GetMatrixPrimvar(Eye.Dials, Dials))
		{
			for (int32 Slot = 0; Slot < 8; ++Slot)
			{
				Dial[Slot] = float(Dials.M[Slot / 4][Slot % 4]);
			}
		}
		const float Pupil = PupilRadius * (1.0f + Dial[GPupilSlot]);

		// The Eye's packed layout: origin + right scale, aim x its scale +
		// pupil radius, up x its scale + LOD blend; eye N at float N*12.
		const FVector4 A(Origin.X, Origin.Y, Origin.Z, ScaleX);
		const FVector4 B(Aim.X * ScaleZ, Aim.Y * ScaleZ, Aim.Z * ScaleZ, Pupil);
		const FVector4 C(Up.X * ScaleY, Up.Y * ScaleY, Up.Z * ScaleY, 0.0);
		TArray<UPrimitiveComponent*> Targets = Rig->GetMeshDrawers(Eye.Mesh);
		Targets.Append(Subdivisions);
		const int32 Base = Eye.Index * 12;
		for (UPrimitiveComponent* Target : Targets)
		{
			Target->SetCustomPrimitiveDataVector4(Base + 0, A);
			Target->SetCustomPrimitiveDataVector4(Base + 4, B);
			Target->SetCustomPrimitiveDataVector4(Base + 8, C);
		}

		// The rest of the dials are material parameters.
		for (int32 Slot = 0; Slot < 8; ++Slot)
		{
			if (!GDials[Slot].Parameter || Eye.Sent[Slot] == Dial[Slot])
			{
				continue;
			}
			Eye.Sent[Slot] = Dial[Slot];
			const float Base1 = Slot == GIrisSlot ? IrisRadius : 1.0f;
			const float Value = GDials[Slot].bScale ? Base1 * (1.0f + Dial[Slot]) : Dial[Slot];
			for (UMaterialInstanceDynamic* Look : Eye.Looks)
			{
				Look->SetScalarParameterValue(GDials[Slot].Parameter, Value);
			}
		}
	}
}

void
URigExecEyeComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!Rig && GetOwner())
	{
		Rig = GetOwner()->FindComponentByClass<URigExecComponent>();
	}
	// The rig reloaded: its meshes are new components.
	if (Rig && Rig->GetMeshes().Num() > 0 && Rig->GetLoadCount() != BoundLoad)
	{
		Bind();
		return;
	}
	Update();
}
