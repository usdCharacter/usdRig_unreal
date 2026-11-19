#include "RigExecStackModel.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RigExecActor.h"
#include "RigExecCharacter.h"
#include "RigExecComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
int64
Int(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	double Value = 0.0;
	Object->TryGetNumberField(Field, Value);
	return int64(Value);
}

FString
Str(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	FString Value;
	Object->TryGetStringField(Field, Value);
	return Value;
}

const TArray<TSharedPtr<FJsonValue>>&
Arr(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	static const TArray<TSharedPtr<FJsonValue>> None;
	const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
	return Object->TryGetArrayField(Field, Value) ? *Value : None;
}

ERigExecMoverFamily
FamilyOf(const FString& Type, const TArray<FString>& Targets)
{
	for (const FString& Target : Targets)
	{
		if (Target.EndsWith(TEXT(".points")))
		{
			return ERigExecMoverFamily::Deformer;
		}
	}
	if (Type.Contains(TEXT("Constraint")))
	{
		return ERigExecMoverFamily::Constraint;
	}
	if (Type.Contains(TEXT("Math")))
	{
		return ERigExecMoverFamily::Math;
	}
	return ERigExecMoverFamily::Other;
}

/** One parse per source and payload: the panel asks on every selection. */
struct FCached
{
	TWeakObjectPtr<const UObject> Source;
	uint32 Hash = 0;
	TSharedPtr<FRigExecStack> Stack;
	FString Reason;
};
TArray<FCached> GCache;
} // namespace

TSharedPtr<FRigExecStack>
FRigExecStack::Parse(const FString& Json, FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("the stack is not valid JSON");
		return nullptr;
	}
	TSharedPtr<FRigExecStack> Out = MakeShared<FRigExecStack>();
	Out->Stage = Str(Root, TEXT("stage"));
	Out->RigRoot = Str(Root, TEXT("rigRoot"));
	Out->Exported = Str(Root, TEXT("exported"));

	TArray<int32> Ancestors;
	for (const TSharedPtr<FJsonValue>& Value : Arr(Root, TEXT("layers")))
	{
		const TSharedPtr<FJsonObject> L = Value->AsObject();
		if (!L.IsValid())
		{
			continue;
		}
		FRigExecStackLayer Layer;
		Layer.Name = Str(L, TEXT("name"));
		Layer.File = Str(L, TEXT("file"));
		Layer.Depth = int32(Int(L, TEXT("depth")));
		L->TryGetBoolField(TEXT("missing"), Layer.bMissing);
		Layer.MTime = Int(L, TEXT("mtime"));
		Layer.Size = Int(L, TEXT("size"));
		Layer.PrimSpecs = int32(Int(L, TEXT("primSpecs")));
		Layer.Defines = int32(Int(L, TEXT("defines")));
		Layer.Overrides = int32(Int(L, TEXT("overrides")));
		// Pre-order with depths: the parent is the last layer one level up.
		Ancestors.SetNum(FMath::Min(Ancestors.Num(), Layer.Depth));
		Layer.Parent = Ancestors.Num() > 0 ? Ancestors.Last() : INDEX_NONE;
		const int32 Index = Out->Layers.Add(MoveTemp(Layer));
		if (Out->Layers[Index].Parent != INDEX_NONE)
		{
			Out->Layers[Out->Layers[Index].Parent].Children.Add(Index);
		}
		Ancestors.Add(Index);
	}

	TMap<int32, int32> ByOrdinal;
	for (const TSharedPtr<FJsonValue>& Value : Arr(Root, TEXT("movers")))
	{
		const TSharedPtr<FJsonObject> M = Value->AsObject();
		if (!M.IsValid())
		{
			continue;
		}
		FRigExecStackMover Mover;
		Mover.Ordinal = int32(Int(M, TEXT("o")));
		Mover.Path = Str(M, TEXT("path"));
		Mover.Type = Str(M, TEXT("type"));
		for (const TSharedPtr<FJsonValue>& T : Arr(M, TEXT("targets")))
		{
			Mover.Targets.Add(T->AsString());
		}
		Mover.Layer = int32(Int(M, TEXT("layer")));
		for (const TSharedPtr<FJsonValue>& O : Arr(M, TEXT("over")))
		{
			Mover.Over.Add(int32(O->AsNumber()));
		}
		M->TryGetBoolField(TEXT("enabled"), Mover.bEnabled);
		int32 Slash = INDEX_NONE;
		Mover.Path.FindLastChar(TEXT('/'), Slash);
		Mover.Name = Slash == INDEX_NONE ? Mover.Path : Mover.Path.Mid(Slash + 1);
		const FString Parent = Slash == INDEX_NONE ? FString() : Mover.Path.Left(Slash);
		int32 ParentSlash = INDEX_NONE;
		Parent.FindLastChar(TEXT('/'), ParentSlash);
		Mover.Group = ParentSlash == INDEX_NONE ? Parent : Parent.Mid(ParentSlash + 1);
		Mover.Family = FamilyOf(Mover.Type, Mover.Targets);
		switch (Mover.Family)
		{
		case ERigExecMoverFamily::Deformer: ++Out->DeformerCount; break;
		case ERigExecMoverFamily::Constraint: ++Out->ConstraintCount; break;
		case ERigExecMoverFamily::Math: ++Out->MathCount; break;
		default: break;
		}
		ByOrdinal.Add(Mover.Ordinal, Out->Movers.Num());
		Out->Movers.Add(MoveTemp(Mover));
	}

	for (const TSharedPtr<FJsonValue>& Value : Arr(Root, TEXT("meshes")))
	{
		const TSharedPtr<FJsonObject> M = Value->AsObject();
		if (!M.IsValid())
		{
			continue;
		}
		FRigExecStackMesh Mesh;
		Mesh.Path = Str(M, TEXT("path"));
		Mesh.Name = Str(M, TEXT("name"));
		for (const TSharedPtr<FJsonValue>& O : Arr(M, TEXT("movers")))
		{
			if (const int32* Index = ByOrdinal.Find(int32(O->AsNumber())))
			{
				Mesh.Movers.Add(*Index);
			}
		}
		Out->Meshes.Add(MoveTemp(Mesh));
	}

	for (const TSharedPtr<FJsonValue>& Value : Arr(Root, TEXT("solvers")))
	{
		const TSharedPtr<FJsonObject> S = Value->AsObject();
		if (S.IsValid())
		{
			Out->Solvers.Add({Str(S, TEXT("type")), Str(S, TEXT("path")), int32(Int(S, TEXT("joints")))});
		}
	}
	for (const TSharedPtr<FJsonValue>& Value : Arr(Root, TEXT("skipped")))
	{
		const TSharedPtr<FJsonObject> S = Value->AsObject();
		if (S.IsValid())
		{
			Out->Skipped.Add({Str(S, TEXT("path")), Str(S, TEXT("reason"))});
		}
	}
	if (Out->Layers.Num() == 0 && Out->Movers.Num() == 0)
	{
		OutError = TEXT("the stack lists no layers and no movers");
		return nullptr;
	}
	Out->RefreshChanged();
	return Out;
}

const UObject*
FRigExecStack::Resolve(const UObject* Source)
{
	if (const AActor* Actor = Cast<AActor>(Source))
	{
		return Actor->FindComponentByClass<URigExecComponent>();
	}
	return Source;
}

FString
FRigExecStack::DisplayName(const UObject* Source)
{
	const UObject* Resolved = Resolve(Source);
	if (const URigExecComponent* Component = Cast<URigExecComponent>(Resolved))
	{
		const AActor* Owner = Component->GetOwner();
		const FString Actor = Owner ? Owner->GetActorLabel() : Component->GetName();
		if (Component->Character)
		{
			return FString::Printf(TEXT("%s  (%s)"), *Actor, *Component->Character->GetName());
		}
		return Actor;
	}
	return Resolved ? Resolved->GetName() : FString();
}

TSharedPtr<FRigExecStack>
FRigExecStack::For(const UObject* Source, FString& OutReason)
{
	const UObject* Resolved = Resolve(Source);
	if (!Resolved)
	{
		OutReason = TEXT("no RigExec rig on this actor");
		return nullptr;
	}
	FString Json;
	if (const URigExecComponent* Component = Cast<URigExecComponent>(Resolved))
	{
		if (Component->Character)
		{
			Resolved = Component->Character;
		}
		else if (!Component->RigExecFile.FilePath.IsEmpty())
		{
			// A component playing files: <rig>_stack.json beside its .rigexec.
			FString File = Component->RigExecFile.FilePath;
			if (FPaths::IsRelative(File))
			{
				File = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), File);
			}
			const FString StackFile = FPaths::Combine(FPaths::GetPath(File), FPaths::GetBaseFilename(File)) + TEXT("_stack.json");
			if (!FFileHelper::LoadFileToString(Json, *StackFile))
			{
				OutReason = FString::Printf(TEXT("no %s beside the rig (run export_stack.py)"), *FPaths::GetCleanFilename(StackFile));
				return nullptr;
			}
		}
		else
		{
			OutReason = TEXT("the Rig component has no character or rig file set");
			return nullptr;
		}
	}
	if (const URigExecCharacter* Character = Cast<URigExecCharacter>(Resolved))
	{
		if (Character->Stack.IsEmpty())
		{
			OutReason = TEXT("this character was baked without its stack. Run the prepare script (it now writes "
			                 "<Name>_stack.json) and bake the character again");
			return nullptr;
		}
		Json = Character->Stack;
	}
	if (Json.IsEmpty())
	{
		OutReason = TEXT("not a RigExec character");
		return nullptr;
	}

	const uint32 Hash = FCrc::StrCrc32(*Json) ^ uint32(Json.Len());
	GCache.RemoveAll([](const FCached& C) { return !C.Source.IsValid(); });
	for (FCached& Cached : GCache)
	{
		if (Cached.Source.Get() == Resolved && Cached.Hash == Hash)
		{
			OutReason = Cached.Reason;
			return Cached.Stack;
		}
	}
	FCached& Entry = GCache.AddDefaulted_GetRef();
	Entry.Source = Resolved;
	Entry.Hash = Hash;
	Entry.Stack = Parse(Json, Entry.Reason);
	OutReason = Entry.Reason;
	return Entry.Stack;
}

void
FRigExecStack::RefreshChanged()
{
	IFileManager& Files = IFileManager::Get();
	for (FRigExecStackLayer& Layer : Layers)
	{
		Layer.bChanged = false;
		if (Layer.bMissing || Layer.File.IsEmpty() || Layer.MTime == 0)
		{
			continue;
		}
		const FDateTime Stamp = Files.GetTimeStamp(*Layer.File);
		if (Stamp == FDateTime::MinValue())
		{
			// Not on this machine (another workstation baked it): unknown, not changed.
			continue;
		}
		const int64 Now = Stamp.ToUnixTimestamp();
		Layer.bChanged = FMath::Abs(Now - Layer.MTime) > 2 || Files.FileSize(*Layer.File) != Layer.Size;
	}
}

int32
FRigExecStack::ChangedLayerCount() const
{
	int32 Count = 0;
	for (const FRigExecStackLayer& Layer : Layers)
	{
		Count += Layer.bChanged ? 1 : 0;
	}
	return Count;
}

FLinearColor
FRigExecStack::FamilyColor(ERigExecMoverFamily Family)
{
	switch (Family)
	{
	case ERigExecMoverFamily::Deformer: return FLinearColor(FColor(86, 156, 214));
	case ERigExecMoverFamily::Constraint: return FLinearColor(FColor(220, 180, 90));
	case ERigExecMoverFamily::Math: return FLinearColor(FColor(140, 140, 140));
	default: return FLinearColor(FColor(170, 130, 200));
	}
}

FLinearColor
FRigExecStack::TypeColor(const FRigExecStackMover& Mover)
{
	if (Mover.Family != ERigExecMoverFamily::Deformer)
	{
		return FamilyColor(Mover.Family);
	}
	const FString& T = Mover.Type;
	if (T.Contains(TEXT("Skin"))) return FLinearColor(FColor(86, 156, 214));
	if (T.Contains(TEXT("BlendShape"))) return FLinearColor(FColor(230, 140, 70));
	if (T.Contains(TEXT("Matrix"))) return FLinearColor(FColor(78, 190, 170));
	if (T.Contains(TEXT("Curve"))) return FLinearColor(FColor(180, 120, 220));
	if (T.Contains(TEXT("Smooth")) || T.Contains(TEXT("DeltaMush"))) return FLinearColor(FColor(120, 200, 100));
	if (T.Contains(TEXT("Lattice")) || T.Contains(TEXT("Surface")) || T.Contains(TEXT("Patch")))
		return FLinearColor(FColor(220, 120, 160));
	return FLinearColor(FColor(150, 170, 200));
}
