#include "RigExecStudioAnimation.h"

#include "Animation/AnimSequenceBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "RigExecBipedCharacter.h"
#include "RigExecComponent.h"
#include "RigExecRetargetComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogRigExecStudio, Log, All);

namespace RigExecStudio
{
namespace
{
/** Values closer than this are the same (degrees, rig units, scale). */
constexpr double GSame = 1e-4;
/** The longest a default clip is baked (frames). */
constexpr int32 GMaxBakeFrames = 240;

/** A key's slope (value per frame) between its neighbours: their average,
 * flat where the curve turns, and never so steep the segment overshoots. */
double Slope(const FTrack& T, int32 Key, int32 D)
{
	if (Key <= 0 || Key >= T.Num() - 1)
	{
		return 0.0;
	}
	const double V0 = T.KeyValues(Key - 1)[D], V1 = T.KeyValues(Key)[D], V2 = T.KeyValues(Key + 1)[D];
	const double S0 = (V1 - V0) / double(T.Frames[Key] - T.Frames[Key - 1]);
	const double S1 = (V2 - V1) / double(T.Frames[Key + 1] - T.Frames[Key]);
	if (S0 * S1 <= 0.0)
	{
		return 0.0;
	}
	const double M = 0.5 * (S0 + S1);
	const double Limit = 3.0 * FMath::Min(FMath::Abs(S0), FMath::Abs(S1));
	return FMath::Clamp(M, -Limit, Limit);
}

TSharedPtr<FJsonObject> TrackToJson(const FTrack& Track)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Frames, Values;
	for (const int32 Frame : Track.Frames)
	{
		Frames.Add(MakeShared<FJsonValueNumber>(Frame));
	}
	for (const double V : Track.Values)
	{
		// Six places: far finer than a degree or a tenth of a millimetre.
		Values.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V * 1e6) / 1e6));
	}
	Object->SetArrayField(TEXT("frames"), Frames);
	Object->SetArrayField(TEXT("values"), Values);
	if (Track.bStepped)
	{
		Object->SetBoolField(TEXT("stepped"), true);
	}
	return Object;
}

bool TrackFromJson(const TSharedPtr<FJsonObject>& Object, int32 Dim, FTrack& Out)
{
	Out = FTrack();
	Out.Dim = Dim;
	Object->TryGetBoolField(TEXT("stepped"), Out.bStepped);
	const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("frames"), Frames) || !Object->TryGetArrayField(TEXT("values"), Values) ||
	    Values->Num() != Frames->Num() * Dim)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Frame : *Frames)
	{
		Out.Frames.Add(int32(Frame->AsNumber()));
	}
	for (const TSharedPtr<FJsonValue>& V : *Values)
	{
		Out.Values.Add(V->AsNumber());
	}
	return true;
}

FString Write(const TSharedPtr<FJsonObject>& Root)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Text;
}

TSharedPtr<FJsonObject> Read(const FString& Text, const TCHAR* Kind)
{
	TSharedPtr<FJsonObject> Root;
	FString Found;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid() ||
	    !Root->TryGetStringField(TEXT("kind"), Found) || Found != Kind)
	{
		return nullptr;
	}
	return Root;
}

bool Keeps(const TSet<FString>* Only, const FString& Path)
{
	return !Only || Only->Contains(Path);
}
} // namespace

void FTrack::SetKey(int32 Frame, const double* V)
{
	int32 At = 0;
	while (At < Frames.Num() && Frames[At] < Frame)
	{
		++At;
	}
	if (At < Frames.Num() && Frames[At] == Frame)
	{
		FMemory::Memcpy(Values.GetData() + At * Dim, V, sizeof(double) * Dim);
		return;
	}
	Frames.Insert(Frame, At);
	Values.InsertUninitialized(At * Dim, Dim);
	FMemory::Memcpy(Values.GetData() + At * Dim, V, sizeof(double) * Dim);
}

bool FTrack::RemoveKey(int32 Frame)
{
	const int32 At = FindKey(Frame);
	if (At == INDEX_NONE)
	{
		return false;
	}
	Frames.RemoveAt(At);
	Values.RemoveAt(At * Dim, Dim);
	return true;
}

void FTrack::Sample(float Frame, double* Out) const
{
	if (Frames.Num() == 0)
	{
		return;
	}
	if (Frames.Num() == 1 || Frame <= Frames[0])
	{
		FMemory::Memcpy(Out, KeyValues(0), sizeof(double) * Dim);
		return;
	}
	if (Frame >= Frames.Last())
	{
		FMemory::Memcpy(Out, KeyValues(Frames.Num() - 1), sizeof(double) * Dim);
		return;
	}
	int32 K = 0;
	while (K + 1 < Frames.Num() && Frames[K + 1] <= Frame)
	{
		++K;
	}
	if (bStepped)
	{
		FMemory::Memcpy(Out, KeyValues(K), sizeof(double) * Dim);
		return;
	}
	// A cubic Hermite segment between keys K and K + 1.
	const double H = double(Frames[K + 1] - Frames[K]);
	const double T = (double(Frame) - Frames[K]) / H;
	const double T2 = T * T, T3 = T2 * T;
	const double H00 = 2 * T3 - 3 * T2 + 1, H10 = T3 - 2 * T2 + T, H01 = -2 * T3 + 3 * T2, H11 = T3 - T2;
	for (int32 D = 0; D < Dim; ++D)
	{
		const double V0 = KeyValues(K)[D], V1 = KeyValues(K + 1)[D];
		Out[D] = H00 * V0 + H10 * H * Slope(*this, K, D) + H01 * V1 + H11 * H * Slope(*this, K + 1, D);
	}
}

TArray<int32> FClip::KeyFrames(const TSet<FString>* Only) const
{
	TSet<int32> Set;
	for (const TMap<FString, FTrack>* Tracks : {&Controls, &Channels})
	{
		for (const TPair<FString, FTrack>& Track : *Tracks)
		{
			if (Keeps(Only, Track.Key))
			{
				Set.Append(Track.Value.Frames);
			}
		}
	}
	TArray<int32> Out = Set.Array();
	Out.Sort();
	return Out;
}

void FClip::MoveKeys(int32 From, int32 To, const TSet<FString>* Only)
{
	if (From == To)
	{
		return;
	}
	for (TMap<FString, FTrack>* Tracks : {&Controls, &Channels})
	{
		for (TPair<FString, FTrack>& Track : *Tracks)
		{
			const int32 Key = Track.Value.FindKey(From);
			if (!Keeps(Only, Track.Key) || Key == INDEX_NONE)
			{
				continue;
			}
			TArray<double> V(Track.Value.KeyValues(Key), Track.Value.Dim);
			Track.Value.RemoveKey(From);
			Track.Value.SetKey(To, V.GetData());
		}
	}
	Length = FMath::Max(Length, To);
}

int32 FClip::RemoveKeys(int32 Frame, const TSet<FString>* Only)
{
	int32 Removed = 0;
	for (TMap<FString, FTrack>* Tracks : {&Controls, &Channels})
	{
		for (auto It = Tracks->CreateIterator(); It; ++It)
		{
			if (Keeps(Only, It.Key()) && It.Value().RemoveKey(Frame))
			{
				++Removed;
				if (It.Value().Num() == 0)
				{
					It.RemoveCurrent();
				}
			}
		}
	}
	return Removed;
}

void FClip::Merge(const FClip& Other, int32 Offset)
{
	auto MergeTracks = [Offset](const TMap<FString, FTrack>& From, TMap<FString, FTrack>& Into) {
		for (const TPair<FString, FTrack>& Track : From)
		{
			FTrack& Target = Into.FindOrAdd(Track.Key);
			if (Target.Num() == 0)
			{
				Target.Dim = Track.Value.Dim;
				Target.bStepped = Track.Value.bStepped;
			}
			for (int32 K = 0; K < Track.Value.Num(); ++K)
			{
				Target.SetKey(Track.Value.Frames[K] + Offset, Track.Value.KeyValues(K));
			}
		}
	};
	MergeTracks(Other.Controls, Controls);
	MergeTracks(Other.Channels, Channels);
	Length = FMath::Max(Length, Other.Length + Offset);
}

void FBinding::Bind(URigExecComponent* InRig)
{
	Rig = InRig;
	ControlOf.Reset();
	ChannelOf.Reset();
	Choices.Reset();
	if (!InRig)
	{
		return;
	}
	for (int32 C = 0; C < InRig->GetControlCount(); ++C)
	{
		if (!InRig->IsControlNull(C))
		{
			ControlOf.Add(InRig->GetControlPath(C), C);
		}
	}
	for (int32 K = 0; K < InRig->GetChannelCount(); ++K)
	{
		ChannelOf.Add(InRig->GetChannelPath(K), K);
		if (InRig->GetChannelLabels(K).Num() > 0)
		{
			Choices.Add(InRig->GetChannelPath(K));
		}
	}
}

void ApplyClip(const FClip& Clip, float Frame, const FBinding& Binding)
{
	URigExecComponent* Rig = Binding.Rig.Get();
	if (!Rig)
	{
		return;
	}
	// Channels first: a switch keyed with the controls it hands over to.
	for (const TPair<FString, FTrack>& Track : Clip.Channels)
	{
		const int32* K = Binding.ChannelOf.Find(Track.Key);
		if (K && Track.Value.Num() > 0)
		{
			double V = 0.0;
			if (Track.Value.bStepped || Binding.Choices.Contains(Track.Key))
			{
				FTrack Stepped = Track.Value;
				Stepped.bStepped = true;
				Stepped.Sample(Frame, &V);
			}
			else
			{
				Track.Value.Sample(Frame, &V);
			}
			Rig->SetChannelValue(*K, V);
		}
	}
	for (const TPair<FString, FTrack>& Track : Clip.Controls)
	{
		const int32* C = Binding.ControlOf.Find(Track.Key);
		if (C && Track.Value.Num() > 0 && Track.Value.Dim == 9)
		{
			double V[9];
			Track.Value.Sample(Frame, V);
			Rig->SetControlAvars(*C, V);
		}
	}
}

FPose CapturePose(const FBinding& Binding, const FString& Name)
{
	FPose Pose;
	Pose.Name = Name;
	const URigExecComponent* Rig = Binding.Rig.Get();
	if (!Rig)
	{
		return Pose;
	}
	for (const TPair<FString, int32>& Control : Binding.ControlOf)
	{
		TArray<double> V;
		V.SetNumUninitialized(9);
		if (Rig->GetControlAvars(Control.Value, V.GetData()))
		{
			Pose.Controls.Add(Control.Key, MoveTemp(V));
		}
	}
	for (const TPair<FString, int32>& Channel : Binding.ChannelOf)
	{
		Pose.Channels.Add(Channel.Key, Rig->GetChannelValue(Channel.Value));
	}
	return Pose;
}

void ApplyPose(const FPose& Pose, float Amount, const FBinding& Binding)
{
	URigExecComponent* Rig = Binding.Rig.Get();
	if (!Rig)
	{
		return;
	}
	const double A = FMath::Clamp(double(Amount), 0.0, 1.0);
	for (const TPair<FString, double>& Channel : Pose.Channels)
	{
		const int32* K = Binding.ChannelOf.Find(Channel.Key);
		if (!K)
		{
			continue;
		}
		const double Now = Rig->GetChannelValue(*K);
		const bool bChoice = Binding.Choices.Contains(Channel.Key);
		Rig->SetChannelValue(*K, bChoice ? (A >= 0.5 ? Channel.Value : Now) : FMath::Lerp(Now, Channel.Value, A));
	}
	for (const TPair<FString, TArray<double>>& Control : Pose.Controls)
	{
		const int32* C = Binding.ControlOf.Find(Control.Key);
		double Now[9];
		if (!C || Control.Value.Num() != 9 || !Rig->GetControlAvars(*C, Now))
		{
			continue;
		}
		for (int32 D = 0; D < 9; ++D)
		{
			Now[D] = FMath::Lerp(Now[D], Control.Value[D], A);
		}
		Rig->SetControlAvars(*C, Now);
	}
}

void KeyPose(FClip& Clip, int32 Frame, const FPose& Pose, const FBinding& Binding, const TSet<FString>* Only)
{
	for (const TPair<FString, TArray<double>>& Control : Pose.Controls)
	{
		if (Keeps(Only, Control.Key) && Control.Value.Num() == 9)
		{
			FTrack& Track = Clip.Controls.FindOrAdd(Control.Key);
			Track.Dim = 9;
			Track.SetKey(Frame, Control.Value.GetData());
		}
	}
	for (const TPair<FString, double>& Channel : Pose.Channels)
	{
		if (Keeps(Only, Channel.Key))
		{
			FTrack& Track = Clip.Channels.FindOrAdd(Channel.Key);
			Track.Dim = 1;
			Track.bStepped = Binding.Choices.Contains(Channel.Key);
			Track.SetKey(Frame, &Channel.Value);
		}
	}
	Clip.Length = FMath::Max(Clip.Length, Frame);
}

TSet<FString> Changed(const FPose& Before, const FPose& After)
{
	TSet<FString> Out;
	for (const TPair<FString, TArray<double>>& Control : After.Controls)
	{
		const TArray<double>* Was = Before.Controls.Find(Control.Key);
		for (int32 D = 0; D < Control.Value.Num(); ++D)
		{
			if (!Was || !Was->IsValidIndex(D) || FMath::Abs((*Was)[D] - Control.Value[D]) > GSame)
			{
				Out.Add(Control.Key);
				break;
			}
		}
	}
	for (const TPair<FString, double>& Channel : After.Channels)
	{
		const double* Was = Before.Channels.Find(Channel.Key);
		if (!Was || FMath::Abs(*Was - Channel.Value) > GSame)
		{
			Out.Add(Channel.Key);
		}
	}
	return Out;
}

FString ClipToJson(const FClip& Clip)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("kind"), TEXT("rigExecClip"));
	Root->SetNumberField(TEXT("version"), FBaker::Version);
	Root->SetStringField(TEXT("name"), Clip.Name);
	Root->SetBoolField(TEXT("builtIn"), Clip.bBuiltIn);
	Root->SetNumberField(TEXT("length"), Clip.Length);
	Root->SetNumberField(TEXT("fps"), GFps);
	TSharedPtr<FJsonObject> Controls = MakeShared<FJsonObject>(), Channels = MakeShared<FJsonObject>();
	for (const TPair<FString, FTrack>& Track : Clip.Controls)
	{
		Controls->SetObjectField(Track.Key, TrackToJson(Track.Value));
	}
	for (const TPair<FString, FTrack>& Track : Clip.Channels)
	{
		Channels->SetObjectField(Track.Key, TrackToJson(Track.Value));
	}
	Root->SetObjectField(TEXT("controls"), Controls);
	Root->SetObjectField(TEXT("channels"), Channels);
	return Write(Root);
}

bool ClipFromJson(const FString& Text, FClip& Out)
{
	const TSharedPtr<FJsonObject> Root = Read(Text, TEXT("rigExecClip"));
	if (!Root)
	{
		return false;
	}
	Out = FClip();
	Root->TryGetStringField(TEXT("name"), Out.Name);
	Root->TryGetBoolField(TEXT("builtIn"), Out.bBuiltIn);
	Out.Length = int32(Root->GetNumberField(TEXT("length")));
	const TSharedPtr<FJsonObject>* Controls = nullptr;
	const TSharedPtr<FJsonObject>* Channels = nullptr;
	if (Root->TryGetObjectField(TEXT("controls"), Controls))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Controls)->Values)
		{
			FTrack Track;
			if (TrackFromJson(Field.Value->AsObject(), 9, Track))
			{
				Out.Controls.Add(Field.Key, MoveTemp(Track));
			}
		}
	}
	if (Root->TryGetObjectField(TEXT("channels"), Channels))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Channels)->Values)
		{
			FTrack Track;
			if (TrackFromJson(Field.Value->AsObject(), 1, Track))
			{
				Out.Channels.Add(Field.Key, MoveTemp(Track));
			}
		}
	}
	return true;
}

FString PoseToJson(const FPose& Pose)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("kind"), TEXT("rigExecPose"));
	Root->SetNumberField(TEXT("version"), FBaker::Version);
	Root->SetStringField(TEXT("name"), Pose.Name);
	Root->SetBoolField(TEXT("builtIn"), Pose.bBuiltIn);
	TSharedPtr<FJsonObject> Controls = MakeShared<FJsonObject>(), Channels = MakeShared<FJsonObject>();
	for (const TPair<FString, TArray<double>>& Control : Pose.Controls)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const double V : Control.Value)
		{
			Values.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(V * 1e6) / 1e6));
		}
		Controls->SetArrayField(Control.Key, Values);
	}
	for (const TPair<FString, double>& Channel : Pose.Channels)
	{
		Channels->SetNumberField(Channel.Key, Channel.Value);
	}
	Root->SetObjectField(TEXT("controls"), Controls);
	Root->SetObjectField(TEXT("channels"), Channels);
	return Write(Root);
}

bool PoseFromJson(const FString& Text, FPose& Out)
{
	const TSharedPtr<FJsonObject> Root = Read(Text, TEXT("rigExecPose"));
	if (!Root)
	{
		return false;
	}
	Out = FPose();
	Root->TryGetStringField(TEXT("name"), Out.Name);
	Root->TryGetBoolField(TEXT("builtIn"), Out.bBuiltIn);
	const TSharedPtr<FJsonObject>* Controls = nullptr;
	const TSharedPtr<FJsonObject>* Channels = nullptr;
	if (Root->TryGetObjectField(TEXT("controls"), Controls))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Controls)->Values)
		{
			TArray<double> V;
			for (const TSharedPtr<FJsonValue>& Each : Field.Value->AsArray())
			{
				V.Add(Each->AsNumber());
			}
			if (V.Num() == 9)
			{
				Out.Controls.Add(Field.Key, MoveTemp(V));
			}
		}
	}
	if (Root->TryGetObjectField(TEXT("channels"), Channels))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Channels)->Values)
		{
			Out.Channels.Add(Field.Key, Field.Value->AsNumber());
		}
	}
	return true;
}

FString LibraryDir(const FString& Character)
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("RigExecStudio"), FileNameFor(Character));
}

FString FileNameFor(const FString& Name)
{
	FString Out;
	for (const TCHAR C : Name)
	{
		Out.AppendChar(FChar::IsAlnum(C) || C == TEXT('-') || C == TEXT('_') ? C : TEXT('_'));
	}
	return Out.IsEmpty() ? FString(TEXT("Untitled")) : Out;
}

TArray<FBakeJob> FBaker::DefaultJobs()
{
	const FString Unarmed = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/");
	auto Path = [](const FString& Folder, const TCHAR* Name) { return Folder + Name + TEXT(".") + Name; };
	TArray<FBakeJob> Jobs;
	Jobs.Add({TEXT("Idle"), Path(Unarmed, TEXT("MM_Idle")), {{TEXT("Idle"), 0.0f}}});
	Jobs.Add({TEXT("Walk"), Path(Unarmed + TEXT("Walk/"), TEXT("MF_Unarmed_Walk_Fwd")),
	          {{TEXT("Walk - Contact"), 0.0f}, {TEXT("Walk - Passing"), 0.25f}}});
	Jobs.Add({TEXT("Jog"), Path(Unarmed + TEXT("Jog/"), TEXT("MF_Unarmed_Jog_Fwd")), {{TEXT("Jog - Stride"), 0.0f}}});
	Jobs.Add({TEXT("Jump"), Path(Unarmed + TEXT("Jump/"), TEXT("MM_Jump")),
	          {{TEXT("Jump - Crouch"), 0.15f}, {TEXT("Jump - Air"), 0.6f}}});
	Jobs.Add({TEXT("Punch"), Path(Unarmed + TEXT("Attack/"), TEXT("MM_Attack_01")), {{TEXT("Punch"), 0.4f}}});
	// (Not the hit reacts: they are additive, which a single animation cannot play.)
	Jobs.Add({TEXT("Dash"), Path(Unarmed + TEXT("Jump/"), TEXT("MM_Dash")), {{TEXT("Dash"), 0.35f}}});
	Jobs.Add({TEXT("Land"), Path(Unarmed + TEXT("Jump/"), TEXT("MM_Land")), {{TEXT("Land - Impact"), 0.2f}}});
	Jobs.Add({TEXT("Fall Down"), Path(TEXT("/Game/Characters/Mannequins/Anims/Death/"), TEXT("MM_Death_Front_01")),
	          {{TEXT("Down"), 1.0f}}});
	return Jobs;
}

void FBaker::Start(ARigExecBipedCharacter* InHero, const FBinding& InBinding)
{
	Hero = InHero;
	Binding = InBinding;
	Jobs = DefaultJobs();
	Job = 0;
	Sample = 0;
	Clips.Reset();
	Poses.Reset();
	Samples.Reset();
	Error.Reset();
	URigExecComponent* Rig = InHero ? InHero->Rig.Get() : nullptr;
	if (!Rig || !InHero->Retarget || !InHero->GetMesh())
	{
		Error = TEXT("no rig to bake onto");
		Jobs.Reset();
		return;
	}
	// On this thread, every sample: each one reads the run before it.
	bWasAsync = Rig->bEvaluateAsync;
	Rig->bEvaluateAsync = false;
	Rig->EvaluateEvery = 1;
	Rig->Settle();
	// Rest, in FK (the retargeter drives the FK controls): the first pose.
	Rig->ClearChannels();
	for (const TPair<FString, double>& Switch : Rig->GetLimbFkSwitches())
	{
		Rig->SetChannel(Switch.Key, float(Switch.Value));
	}
	Rig->Evaluate();
	Rest = CapturePose(Binding, TEXT("Rest"));
	Rest.bBuiltIn = true;
	Poses.Add(Rest);

	USkeletalMeshComponent* Mesh = InHero->GetMesh();
	Mesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	InHero->Retarget->Source = Mesh;
	InHero->Retarget->Target = Rig;
	InHero->Retarget->bEnabled = false;
	InHero->Retarget->ClearExtraTurns();
	InHero->Retarget->Rebind();
	TotalSamples = DoneSamples = 0;
	for (const FBakeJob& Each : Jobs)
	{
		if (const UAnimSequenceBase* Anim = LoadObject<UAnimSequenceBase>(nullptr, *Each.Animation))
		{
			TotalSamples += FMath::Min(FMath::RoundToInt(Anim->GetPlayLength() * GFps), GMaxBakeFrames) + 1;
		}
	}
	UE_LOG(LogRigExecStudio, Log, TEXT("baking %d default clips (%d samples) for %s"), Jobs.Num(), TotalSamples,
	       *InHero->GetCharacterAsset().ToString());
}

bool FBaker::BeginJob()
{
	UAnimSequenceBase* Anim = LoadObject<UAnimSequenceBase>(nullptr, *Jobs[Job].Animation);
	if (!Anim)
	{
		UE_LOG(LogRigExecStudio, Warning, TEXT("no animation at %s; %s is not baked"), *Jobs[Job].Animation, *Jobs[Job].Name);
		return false;
	}
	if (Anim->IsValidAdditive())
	{
		UE_LOG(LogRigExecStudio, Warning, TEXT("%s is additive; %s is not baked"), *Jobs[Job].Animation, *Jobs[Job].Name);
		return false;
	}
	Sequence = Anim;
	SampleCount = FMath::Min(FMath::RoundToInt(Anim->GetPlayLength() * GFps), GMaxBakeFrames);
	Samples.Reset();
	USkeletalMeshComponent* Mesh = Hero->GetMesh();
	Mesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	Mesh->SetAnimation(Anim);
	Mesh->Stop();
	return true;
}

bool FBaker::Step(int32 Budget)
{
	ARigExecBipedCharacter* Who = Hero.Get();
	if (!Who || Job >= Jobs.Num())
	{
		return true;
	}
	URigExecComponent* Rig = Who->Rig;
	USkeletalMeshComponent* Mesh = Who->GetMesh();
	Rig->bEvaluateAsync = false;
	Rig->EvaluateEvery = 1;
	const double Until = FPlatformTime::Seconds() + Budget / 1000.0;
	while (Job < Jobs.Num() && FPlatformTime::Seconds() < Until)
	{
		if (!Sequence.IsValid() && !BeginJob())
		{
			++Job;
			continue;
		}
		// The source posed at this sample's time, then carried onto the rig.
		Mesh->SetPosition(float(Sample) / GFps, false);
		Mesh->TickAnimation(0.0f, false);
		Mesh->RefreshBoneTransforms();
		if (!Who->Retarget->DriveOnce())
		{
			Error = Who->Retarget->Status;
			Jobs.Reset();
			break;
		}
		if (Sample == 0)
		{
			// A clip's first sample twice: the offsets settle on the new pose.
			Who->Retarget->DriveOnce();
		}
		Samples.Add(CapturePose(Binding, FString()));
		++DoneSamples;
		if (++Sample > SampleCount)
		{
			FinishJob();
			++Job;
			Sample = 0;
			Sequence.Reset();
		}
	}
	if (Job >= Jobs.Num())
	{
		Restore();
		return true;
	}
	return false;
}

void FBaker::FinishJob()
{
	const FBakeJob& Each = Jobs[Job];
	FClip Clip;
	Clip.Name = Each.Name;
	Clip.bBuiltIn = true;
	Clip.Length = SampleCount;
	if (Samples.Num() == 0)
	{
		return;
	}
	const FPose& First = Samples[0];
	// What moves gets a key every KeyEvery frames (and on the last); what
	// holds still off its rest gets one key; what stays at rest gets none,
	// so whatever the animator posed there stays.
	for (const TPair<FString, TArray<double>>& Control : First.Controls)
	{
		bool bMoves = false;
		for (const FPose& Taken : Samples)
		{
			const TArray<double>& V = Taken.Controls[Control.Key];
			for (int32 D = 0; D < 9 && !bMoves; ++D)
			{
				bMoves = FMath::Abs(V[D] - Control.Value[D]) > GSame;
			}
			if (bMoves)
			{
				break;
			}
		}
		const TArray<double>* AtRest = Rest.Controls.Find(Control.Key);
		bool bOffRest = false;
		for (int32 D = 0; AtRest && D < 9 && !bOffRest; ++D)
		{
			bOffRest = FMath::Abs((*AtRest)[D] - Control.Value[D]) > GSame;
		}
		if (!bMoves && !bOffRest)
		{
			continue;
		}
		FTrack& Track = Clip.Controls.Add(Control.Key);
		Track.Dim = 9;
		for (int32 S = 0; S < Samples.Num(); S += bMoves ? 1 : Samples.Num())
		{
			if (S % KeyEvery == 0 || S == Samples.Num() - 1)
			{
				Track.SetKey(S, Samples[S].Controls[Control.Key].GetData());
			}
		}
	}
	for (const TPair<FString, double>& Channel : First.Channels)
	{
		// The IK/FK switches are keyed whatever they hold, so the clip plays
		// in FK (what the retargeter drove) whatever the limbs were in.
		const double* AtRest = Rest.Channels.Find(Channel.Key);
		if ((Binding.Rig.IsValid() && Binding.Rig->IsLimbSwitch(Channel.Key)) || (AtRest && FMath::Abs(*AtRest - Channel.Value) > GSame))
		{
			FTrack& Track = Clip.Channels.Add(Channel.Key);
			Track.Dim = 1;
			Track.bStepped = Binding.Choices.Contains(Channel.Key);
			Track.SetKey(0, &Channel.Value);
		}
	}
	for (const TPair<FString, float>& Pose : Each.Poses)
	{
		const int32 S = FMath::Clamp(FMath::RoundToInt(Pose.Value * (Samples.Num() - 1)), 0, Samples.Num() - 1);
		FPose Taken = Samples[S];
		Taken.Name = Pose.Key;
		Taken.bBuiltIn = true;
		Poses.Add(MoveTemp(Taken));
	}
	UE_LOG(LogRigExecStudio, Log, TEXT("baked %s: %d frames, %d control tracks"), *Clip.Name, Clip.Length, Clip.Controls.Num());
	Clips.Add(MoveTemp(Clip));
	Samples.Reset();
}

void FBaker::Restore()
{
	ARigExecBipedCharacter* Who = Hero.Get();
	if (!Who)
	{
		return;
	}
	URigExecComponent* Rig = Who->Rig;
	Rig->bEvaluateAsync = bWasAsync;
	// The Mannequin back on its animation blueprint (a preview in the lobby
	// idles with it), the rig back at rest.
	USkeletalMeshComponent* Mesh = Who->GetMesh();
	Mesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	if (UClass* Anim = Who->SourceAnimation.LoadSynchronous())
	{
		Mesh->SetAnimInstanceClass(Anim);
	}
	ApplyPose(Rest, 1.0f, Binding);
	Rig->Evaluate();
}

void FBaker::Cancel()
{
	if (IsRunning())
	{
		Restore();
	}
	Jobs.Reset();
	Hero.Reset();
}

float FBaker::Progress() const
{
	return TotalSamples > 0 ? float(DoneSamples) / float(TotalSamples) : 1.0f;
}

FString FBaker::Describe() const
{
	return Jobs.IsValidIndex(Job) ? Jobs[Job].Name : FString();
}
} // namespace RigExecStudio
