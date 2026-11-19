#include "RigExecStudioGameMode.h"
#include "RigExecVR.h"

#include "Camera/PlayerCameraManager.h"
#include "CanvasItem.h"
#include "Dom/JsonObject.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/FileManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "RHI.h"
#include "RenderTimer.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RigExecBipedCharacter.h"
#include "RigExecComponent.h"
#include "RigExecRetargetComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TextureResource.h"

DEFINE_LOG_CATEGORY_STATIC(LogRigExecStudioHud, Log, All);

using namespace RigExecStudio;

namespace
{
const FLinearColor GPanel(0.03f, 0.05f, 0.08f, 0.84f);
const FLinearColor GAccent(0.15f, 0.55f, 0.85f, 0.95f);
const FLinearColor GCyan(0.45f, 0.85f, 1.0f);
const FLinearColor GText(0.88f, 0.93f, 1.0f);
const FLinearColor GDim(0.55f, 0.6f, 0.68f);
const FLinearColor GKey(1.0f, 0.78f, 0.2f);
const FLinearColor GHead(1.0f, 0.32f, 0.3f);
/** Undo steps kept. */
constexpr int32 GUndoDepth = 60;
/** Pixels a press moves before it is a drag. */
constexpr float GDragStart = 3.0f;

const TCHAR* const GAxisNames[9] = {TEXT("tx"), TEXT("ty"), TEXT("tz"), TEXT("rx"), TEXT("ry"), TEXT("rz"),
                                    TEXT("sx"), TEXT("sy"), TEXT("sz")};

FString ShortName(const FString& Path)
{
	FString Name = Path;
	int32 Slash = INDEX_NONE;
	if (Name.FindLastChar(TEXT('/'), Slash))
	{
		Name = Name.Mid(Slash + 1);
	}
	return Name;
}

UFont* UiFont()
{
	return GEngine ? GEngine->GetMediumFont() : nullptr;
}
} // namespace

ARigExecStudioGameMode::ARigExecStudioGameMode()
{
	DefaultPawnClass = ARigExecBipedCharacter::StaticClass();
	HUDClass = ARigExecStudioHUD::StaticClass();
}

ARigExecStudioHUD::ARigExecStudioHUD()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ARigExecStudioHUD::BeginPlay()
{
	Super::BeginPlay();
	// Choosing who to animate is the studio's first step, every visit.
	FString Named;
	const TCHAR* Cmd = FCommandLine::Get();
	const bool bAutomated = FParse::Value(Cmd, TEXT("RigExecCharacter="), Named) || FParse::Param(Cmd, TEXT("RigExecStudioTest"));
	if (!IsMenuOpen() && !bAutomated && ARigExecBipedCharacter::FindCharacters().Num() > 1)
	{
		OpenChooser();
	}
	bTest = FParse::Param(Cmd, TEXT("RigExecStudioTest"));
	bProfile = FParse::Param(Cmd, TEXT("RigExecStudioProfile"));
}

void ARigExecStudioHUD::EndPlay(const EEndPlayReason::Type Reason)
{
	Unbind();
	Super::EndPlay(Reason);
}

// --- The character ----------------------------------------------------------

URigExecComponent* ARigExecStudioHUD::Rig() const
{
	return Bound.IsValid() ? Bound->Rig.Get() : nullptr;
}

FString ARigExecStudioHUD::CharacterName() const
{
	FString Name = Bound.IsValid() ? Bound->GetCharacterAsset().GetAssetName() : FString();
	Name.RemoveFromStart(TEXT("RXC_"));
	return Name.IsEmpty() ? FString(TEXT("Character")) : Name;
}

void ARigExecStudioHUD::Bind(ARigExecBipedCharacter* Hero)
{
	Bound = Hero;
	Hero->bStudio = true;
	// The studio sets the controls; nothing else may.
	Hero->Retarget->bEnabled = false;
	Hero->Retarget->ClearExtraTurns();
	Hero->GetCharacterMovement()->StopMovementImmediately();
	Hero->GetCharacterMovement()->DisableMovement();
	URigExecComponent* R = Hero->Rig;
	R->bEvaluateAsync = false;
	R->EvaluateEvery = 1;
	R->Settle();
	Binding.Bind(R);
	Picker.Load(R);
	// The controls TouchPose cannot reach through the skin get a dot: no
	// region names them on a mesh that is drawn.
	Untouched.Reset();
	TMap<int32, int32> RegionsOnMesh;
	auto Drawn = [R](int32 Mesh) {
		for (const UPrimitiveComponent* Drawer : R->GetMeshDrawers(Mesh))
		{
			if (Drawer && Drawer->IsVisible())
			{
				return true;
			}
		}
		return false;
	};
	for (int32 Region = 0; Region < R->GetTouchRegionCount(); ++Region)
	{
		RegionsOnMesh.FindOrAdd(R->GetTouchRegionMesh(Region))++;
	}
	for (const TPair<int32, int32>& Mesh : RegionsOnMesh)
	{
		UE_LOG(LogRigExecStudioHud, Log, TEXT("touch regions: %d on %s (%s)"), Mesh.Value, *R->GetMeshPath(Mesh.Key),
		       Drawn(Mesh.Key) ? TEXT("drawn") : TEXT("hidden"));
	}
	for (const TPair<FString, int32>& Control : Binding.ControlOf)
	{
		if (R->GetControlAvarMask(Control.Value) == 0)
		{
			continue;
		}
		bool bReached = false;
		for (const int32 Region : R->GetTouchRegionsOf({R->GetControlElement(Control.Value)}))
		{
			bReached |= R->GetTouchRegionFaceCount(Region) > 0 && Drawn(R->GetTouchRegionMesh(Region));
		}
		if (!bReached)
		{
			Untouched.Add(Control.Value);
		}
	}
	PickerPanel = 0;
	Selected.Reset();
	UndoStack.Reset();
	RedoStack.Reset();
	Edit = FClip();
	Edit.Name = TEXT("Untitled");
	bEditChanged = false;
	Frame = 0.0f;
	AppliedFrame = -1.0f;
	bPlaying = false;
	Drag = FDrag();
	if (USpringArmComponent* Boom = Hero->CameraBoom)
	{
		Boom->bDoCollisionTest = false;
		Boom->bEnableCameraLag = false;
		Boom->bEnableCameraRotationLag = false;
		Boom->bUsePawnControlRotation = true;
	}
	HomeCamera();
	CamFocus = CamFocusWant;
	CamDist = CamDistWant;
	UE_LOG(LogRigExecStudioHud, Log, TEXT("studio: %s, %d controls, %d channels, %d touch regions, %d picker panels"),
	       *CharacterName(), Binding.ControlOf.Num(), Binding.ChannelOf.Num(), R->GetTouchRegionCount(), Picker.Panels.Num());
	LoadLibrary();
}

void ARigExecStudioHUD::Unbind()
{
	if (bBaking)
	{
		Baker.Cancel();
		bBaking = false;
	}
	if (ARigExecBipedCharacter* Old = Bound.Get())
	{
		// Back to a character the game drives (a lobby preview idles).
		Old->bStudio = false;
		Old->Retarget->bEnabled = true;
		Old->Retarget->Rebind();
		Old->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		if (Old->Rig)
		{
			Old->Rig->SetTouchHighlight(-1, -1, {});
			Old->Rig->SetTouchSuspended(false);
		}
	}
	Bound.Reset();
	Binding.Bind(nullptr);
	Selected.Reset();
	Untouched.Reset();
	HoverRegion = -1;
	HoverControl = INDEX_NONE;
}

int32 ARigExecStudioHUD::ControlUnder(const FVector2D& At, int32& OutRegion)
{
	OutRegion = -1;
	URigExecComponent* R = Rig();
	if (!R || !PlayerOwner)
	{
		return INDEX_NONE;
	}
	FVector Origin = FVector::ZeroVector, Direction = FVector::ForwardVector;
	double SkinDistance = 0.0;
	if (FromScreen(At, Origin, Direction))
	{
		OutRegion = R->PickTouchRegion(Origin, Direction, SkinDistance);
		if (OutRegion >= 0)
		{
			const int32 C = R->FindControlIndex(R->GetTouchRegionControl(OutRegion).ToString());
			if (C != INDEX_NONE)
			{
				return C;
			}
		}
	}
	// Unpainted skin or none: the nearest dot of a control no region names;
	// on unpainted skin, failing that, the nearest control of any kind (a
	// character whose regions do not fit its skin can still be posed).
	int32 Best = INDEX_NONE;
	float Nearest = 16.0f * S;
	TArray<int32> Candidates = Untouched.Array();
	if (OutRegion < 0 && SkinDistance > 0.0)
	{
		// The ray hit unpainted skin.
		Nearest = 28.0f * S;
		for (const TPair<FString, int32>& Control : Binding.ControlOf)
		{
			if (!Untouched.Contains(Control.Value) && R->GetControlAvarMask(Control.Value) != 0)
			{
				Candidates.Add(Control.Value);
			}
		}
	}
	for (const int32 C : Candidates)
	{
		FVector2D OnScreen;
		if (ToScreen(
		        R->GetComponentTransform().TransformPosition(R->GetRigFrame(R->GetControlPath(C)).GetLocation()), OnScreen))
		{
			const float D = float(FVector2D::Distance(OnScreen, At));
			if (D < Nearest)
			{
				Nearest = D;
				Best = C;
			}
		}
	}
	return Best;
}

void ARigExecStudioHUD::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!PlayerOwner || IsMenuOpen())
	{
		return;
	}
	// The cursor is the studio's tool: always shown, never captured.
	if (!PlayerOwner->bShowMouseCursor)
	{
		PlayerOwner->bShowMouseCursor = true;
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		PlayerOwner->SetInputMode(Mode);
	}
	ARigExecBipedCharacter* Hero = this->Hero();
	if (Bound.IsValid() && Hero != Bound.Get())
	{
		Unbind();
	}
	if (!Bound.IsValid() && Hero && Hero->Rig && Hero->Rig->GetControlCount() > 0)
	{
		Bind(Hero);
	}
	URigExecComponent* R = Rig();
	if (!R)
	{
		return;
	}
	if (bTest && !bBaking)
	{
		RunTest(DeltaSeconds);
	}
	if (bBaking)
	{
		// A slice of the bake a frame: the game keeps drawing meanwhile.
		if (Baker.Step(16))
		{
			bBaking = false;
			if (!Baker.Error.IsEmpty())
			{
				Toast(FString::Printf(TEXT("Could not bake the defaults: %s"), *Baker.Error));
			}
			else
			{
				Clips.Insert(Baker.Clips, 0);
				Poses.Insert(Baker.Poses, 0);
				SaveDefaults();
				Toast(FString::Printf(TEXT("Baked %d default animations and %d poses"), Baker.Clips.Num(), Baker.Poses.Num()));
			}
			AppliedFrame = -1.0f;
			bDirty = true;
		}
		ApplyCamera(DeltaSeconds);
		return;
	}
	if (bPlaying && Drag.Kind != EDrag::Scrub)
	{
		Frame += DeltaSeconds * GFps;
		if (Frame > Edit.Length)
		{
			if (bLoop && Edit.Length > 0)
			{
				Frame = FMath::Fmod(Frame, float(Edit.Length));
			}
			else
			{
				Frame = float(Edit.Length);
				bPlaying = false;
			}
		}
	}
	const double RigStart = FPlatformTime::Seconds();
	// The rig runs on worker threads, as in the game, so the game thread
	// never waits for it; playing, every other frame (the clip is 30 fps).
	// The self-test reads each pose straight after setting it: on this thread.
	R->bEvaluateAsync = !bTest && ARigExecBipedCharacter::bRigAsync;
	R->EvaluateEvery = bPlaying ? FMath::Clamp(ARigExecBipedCharacter::RigRateFar, 1, 2) : 1;
	if (bWasPlaying && !bPlaying)
	{
		// Stopped: the frame it stopped on, in full.
		bDirty = true;
		AppliedFrame = -1.0f;
	}
	bWasPlaying = bPlaying;
	ApplyEdit();
	if (bDirty)
	{
		// A run still going takes this change on the next frame: one
		// evaluation launched now would leave it waiting for another call.
		if (!R->IsRunPending() || R->LandIfDone())
		{
			R->Evaluate();
			bDirty = false;
		}
	}
	// A run that finished lands now, so the last change always shows.
	R->LandIfDone();
	ProfileSpent[0] += FPlatformTime::Seconds() - RigStart;
	if (bProfile)
	{
		ProfileTick(DeltaSeconds);
	}
	R->SetTouchSuspended(bPlaying);
	ApplyCamera(DeltaSeconds);
}

// --- The library ----------------------------------------------------------------

void ARigExecStudioHUD::LoadLibrary()
{
	Clips.Reset();
	Poses.Reset();
	const FString Dir = LibraryDir(CharacterName());
	// The defaults, as last baked for this character: baked again when missing
	// or made by another version of the bake.
	bool bDefaults = false;
	FString Text;
	TSharedPtr<FJsonObject> Manifest;
	if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, TEXT("Defaults"), TEXT("manifest.json"))) &&
	    FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Manifest) && Manifest.IsValid() &&
	    int32(Manifest->GetNumberField(TEXT("version"))) == FBaker::Version)
	{
		bDefaults = true;
		for (const TCHAR* Kind : {TEXT("clips"), TEXT("poses")})
		{
			const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
			if (!Manifest->TryGetArrayField(Kind, Files))
			{
				bDefaults = false;
				continue;
			}
			for (const TSharedPtr<FJsonValue>& File : *Files)
			{
				FString Body;
				const bool bRead = FFileHelper::LoadFileToString(Body, *FPaths::Combine(Dir, TEXT("Defaults"), File->AsString()));
				if (FCString::Strcmp(Kind, TEXT("clips")) == 0)
				{
					FClip Clip;
					bDefaults &= bRead && ClipFromJson(Body, Clip);
					Clip.bBuiltIn = true;
					Clips.Add(MoveTemp(Clip));
				}
				else
				{
					FPose Pose;
					bDefaults &= bRead && PoseFromJson(Body, Pose);
					Pose.bBuiltIn = true;
					Poses.Add(MoveTemp(Pose));
				}
			}
		}
	}
	if (!bDefaults)
	{
		Clips.Reset();
		Poses.Reset();
	}
	// The person's own, after the defaults.
	auto LoadUser = [&](const TCHAR* Folder, bool bPose) {
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, Folder, TEXT("*.json")), true, false);
		Files.Sort();
		for (const FString& File : Files)
		{
			FString Body;
			if (!FFileHelper::LoadFileToString(Body, *FPaths::Combine(Dir, Folder, File)))
			{
				continue;
			}
			if (bPose)
			{
				FPose Pose;
				if (PoseFromJson(Body, Pose))
				{
					Pose.bBuiltIn = false;
					Poses.Add(MoveTemp(Pose));
				}
			}
			else
			{
				FClip Clip;
				if (ClipFromJson(Body, Clip))
				{
					Clip.bBuiltIn = false;
					Clips.Add(MoveTemp(Clip));
				}
			}
		}
	};
	LoadUser(TEXT("Animations"), false);
	LoadUser(TEXT("Poses"), true);
	if (!bDefaults)
	{
		StartBake();
	}
}

void ARigExecStudioHUD::StartBake()
{
	ARigExecBipedCharacter* Hero = Bound.Get();
	if (!Hero)
	{
		return;
	}
	Clips.RemoveAll([](const FClip& C) { return C.bBuiltIn; });
	Poses.RemoveAll([](const FPose& P) { return P.bBuiltIn; });
	bPlaying = false;
	Baker.Start(Hero, Binding);
	bBaking = Baker.Error.IsEmpty();
	if (!bBaking)
	{
		Toast(FString::Printf(TEXT("Could not bake the defaults: %s"), *Baker.Error));
	}
}

void ARigExecStudioHUD::SaveDefaults() const
{
	const FString Dir = FPaths::Combine(LibraryDir(CharacterName()), TEXT("Defaults"));
	TArray<TSharedPtr<FJsonValue>> ClipFiles, PoseFiles;
	for (const FClip& Clip : Clips)
	{
		if (Clip.bBuiltIn)
		{
			const FString File = FileNameFor(Clip.Name) + TEXT(".json");
			FFileHelper::SaveStringToFile(ClipToJson(Clip), *FPaths::Combine(Dir, File));
			ClipFiles.Add(MakeShared<FJsonValueString>(File));
		}
	}
	for (const FPose& Pose : Poses)
	{
		if (Pose.bBuiltIn)
		{
			const FString File = TEXT("pose_") + FileNameFor(Pose.Name) + TEXT(".json");
			FFileHelper::SaveStringToFile(PoseToJson(Pose), *FPaths::Combine(Dir, File));
			PoseFiles.Add(MakeShared<FJsonValueString>(File));
		}
	}
	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetNumberField(TEXT("version"), FBaker::Version);
	Manifest->SetArrayField(TEXT("clips"), ClipFiles);
	Manifest->SetArrayField(TEXT("poses"), PoseFiles);
	FString Text;
	FJsonSerializer::Serialize(Manifest.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
	FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Dir, TEXT("manifest.json")));
}

void ARigExecStudioHUD::SaveClip(const FString& Name)
{
	Edit.Name = Name;
	FClip Copy = Edit;
	Copy.bBuiltIn = false;
	const FString File = FPaths::Combine(LibraryDir(CharacterName()), TEXT("Animations"), FileNameFor(Name) + TEXT(".json"));
	if (!FFileHelper::SaveStringToFile(ClipToJson(Copy), *File))
	{
		Toast(TEXT("Could not save the animation"));
		return;
	}
	const int32 Mine = Clips.IndexOfByPredicate([&Name](const FClip& C) { return !C.bBuiltIn && C.Name == Name; });
	Mine != INDEX_NONE ? (void)(Clips[Mine] = Copy) : (void)Clips.Add(Copy);
	bEditChanged = false;
	Toast(FString::Printf(TEXT("Saved animation \"%s\""), *Name));
}

void ARigExecStudioHUD::SavePose(const FString& Name)
{
	FPose Pose = CapturePose(Binding, Name);
	const FString File = FPaths::Combine(LibraryDir(CharacterName()), TEXT("Poses"), FileNameFor(Name) + TEXT(".json"));
	if (!FFileHelper::SaveStringToFile(PoseToJson(Pose), *File))
	{
		Toast(TEXT("Could not save the pose"));
		return;
	}
	const int32 Mine = Poses.IndexOfByPredicate([&Name](const FPose& P) { return !P.bBuiltIn && P.Name == Name; });
	Mine != INDEX_NONE ? (void)(Poses[Mine] = Pose) : (void)Poses.Add(Pose);
	Toast(FString::Printf(TEXT("Saved pose \"%s\""), *Name));
}

void ARigExecStudioHUD::DeleteItem(bool bPose, int32 Index)
{
	const FString Dir = LibraryDir(CharacterName());
	if (bPose && Poses.IsValidIndex(Index) && !Poses[Index].bBuiltIn)
	{
		IFileManager::Get().Delete(*FPaths::Combine(Dir, TEXT("Poses"), FileNameFor(Poses[Index].Name) + TEXT(".json")));
		Toast(FString::Printf(TEXT("Deleted pose \"%s\""), *Poses[Index].Name));
		Poses.RemoveAt(Index);
	}
	else if (!bPose && Clips.IsValidIndex(Index) && !Clips[Index].bBuiltIn)
	{
		IFileManager::Get().Delete(*FPaths::Combine(Dir, TEXT("Animations"), FileNameFor(Clips[Index].Name) + TEXT(".json")));
		Toast(FString::Printf(TEXT("Deleted animation \"%s\""), *Clips[Index].Name));
		Clips.RemoveAt(Index);
	}
}

void ARigExecStudioHUD::UseClip(int32 Index, bool bInsert)
{
	if (!Clips.IsValidIndex(Index))
	{
		return;
	}
	PushUndo();
	const FClip& Clip = Clips[Index];
	if (bInsert && !Edit.IsEmpty())
	{
		const int32 At = FMath::RoundToInt(Frame);
		Edit.Merge(Clip, At);
		bEditChanged = true;
		Toast(FString::Printf(TEXT("Added \"%s\" at frame %d"), *Clip.Name, At));
	}
	else
	{
		Edit = Clip;
		Edit.bBuiltIn = false;
		bEditChanged = false;
		Frame = 0.0f;
		Toast(FString::Printf(TEXT("\"%s\" is on the timeline: %d frames, %d keyed controls"), *Clip.Name, Clip.Length,
		                      Clip.Controls.Num()));
	}
	ApplyEdit(true);
}

void ARigExecStudioHUD::UsePose(int32 Index)
{
	if (!Poses.IsValidIndex(Index))
	{
		return;
	}
	PushUndo();
	const FPose Before = CapturePose(Binding, FString());
	ApplyPose(Poses[Index], PoseMix, Binding);
	bDirty = true;
	AutoKey(Before);
	Toast(FString::Printf(TEXT("Pose \"%s\"%s%s"), *Poses[Index].Name,
	                      PoseMix < 0.999f ? *FString::Printf(TEXT(" at %d%%"), FMath::RoundToInt(PoseMix * 100.0f)) : TEXT(""),
	                      bAutoKey ? *FString::Printf(TEXT(", keyed on frame %d"), FMath::RoundToInt(Frame)) : TEXT("")));
}

// --- The timeline -----------------------------------------------------------------

void ARigExecStudioHUD::SetFrame(float NewFrame)
{
	Frame = FMath::Clamp(NewFrame, 0.0f, float(FMath::Max(Edit.Length, 0)));
}

void ARigExecStudioHUD::ApplyEdit(bool bForce)
{
	if (Edit.IsEmpty())
	{
		AppliedFrame = Frame;
		return;
	}
	if (bForce || Frame != AppliedFrame)
	{
		ApplyClip(Edit, Frame, Binding);
		AppliedFrame = Frame;
		bDirty = true;
	}
}

TSet<FString> ARigExecStudioHUD::SelectedPaths(bool bWithChannels) const
{
	TSet<FString> Out;
	const URigExecComponent* R = Rig();
	if (!R)
	{
		return Out;
	}
	for (const int32 C : Selected)
	{
		const FString& Path = R->GetControlPath(C);
		Out.Add(Path);
		if (!bWithChannels)
		{
			continue;
		}
		// Its own channels, and the switch of the limb it is on.
		for (const TPair<FString, int32>& Channel : Binding.ChannelOf)
		{
			if (Channel.Key.StartsWith(Path + TEXT(".")))
			{
				Out.Add(Channel.Key);
			}
		}
		const FString Switch = R->FindLimbSwitchFor(R->GetControlElement(C));
		if (!Switch.IsEmpty())
		{
			Out.Add(Switch);
		}
	}
	return Out;
}

void ARigExecStudioHUD::KeySelection(bool bAll)
{
	if (!Rig())
	{
		return;
	}
	PushUndo();
	const int32 At = FMath::RoundToInt(Frame);
	const FPose Now = CapturePose(Binding, FString());
	const bool bEverything = bAll || Selected.Num() == 0;
	const TSet<FString> Only = SelectedPaths(true);
	KeyPose(Edit, At, Now, Binding, bEverything ? nullptr : &Only);
	bEditChanged = true;
	AppliedFrame = Frame;
	Toast(bEverything ? FString::Printf(TEXT("Keyed every control on frame %d"), At)
	                  : FString::Printf(TEXT("Keyed %d control%s on frame %d"), Selected.Num(), Selected.Num() == 1 ? TEXT("") : TEXT("s"), At));
}

void ARigExecStudioHUD::DeleteKeys()
{
	PushUndo();
	const int32 At = FMath::RoundToInt(Frame);
	const TSet<FString> Only = SelectedPaths(true);
	const int32 Removed = Edit.RemoveKeys(At, Selected.Num() > 0 ? &Only : nullptr);
	if (Removed > 0)
	{
		bEditChanged = true;
		ApplyEdit(true);
	}
	Toast(Removed > 0 ? FString::Printf(TEXT("Deleted %d key%s on frame %d"), Removed, Removed == 1 ? TEXT("") : TEXT("s"), At)
	                  : FString::Printf(TEXT("No keys on frame %d"), At));
}

void ARigExecStudioHUD::AutoKey(const FPose& Before)
{
	if (!bAutoKey || !Rig())
	{
		return;
	}
	const FPose After = CapturePose(Binding, FString());
	const TSet<FString> Moved = Changed(Before, After);
	if (Moved.Num() == 0)
	{
		return;
	}
	KeyPose(Edit, FMath::RoundToInt(Frame), After, Binding, &Moved);
	bEditChanged = true;
	AppliedFrame = Frame;
}

int32 ARigExecStudioHUD::KeyFrameNear(int32 From, int32 Step) const
{
	const TSet<FString> Only = SelectedPaths(true);
	const TArray<int32> Frames = Edit.KeyFrames(Selected.Num() > 0 ? &Only : nullptr);
	if (Step > 0)
	{
		for (const int32 F : Frames)
		{
			if (F > From)
			{
				return F;
			}
		}
	}
	else
	{
		for (int32 K = Frames.Num() - 1; K >= 0; --K)
		{
			if (Frames[K] < From)
			{
				return Frames[K];
			}
		}
	}
	return From;
}

// --- Selection and switching ------------------------------------------------------

void ARigExecStudioHUD::Select(const TArray<int32>& Controls, int32 Mode)
{
	TArray<int32> Added;
	if (Mode == 0)
	{
		for (const int32 C : Controls)
		{
			if (!Selected.Contains(C))
			{
				Added.Add(C);
			}
		}
		Selected = Controls;
	}
	for (const int32 C : Controls)
	{
		if (Mode == 1 || Mode == 3)
		{
			if (Selected.Contains(C) && Mode == 1)
			{
				Selected.Remove(C);
			}
			else if (!Selected.Contains(C))
			{
				Selected.Add(C);
				Added.Add(C);
			}
		}
		else if (Mode == 2)
		{
			Selected.Remove(C);
		}
	}
	SwitchPresented(Added);
}

void ARigExecStudioHUD::SwitchPresented(const TArray<int32>& Picked)
{
	URigExecComponent* R = Rig();
	if (!R)
	{
		return;
	}
	TArray<FString> Switches;
	for (const int32 C : Picked)
	{
		const FString Switch = R->FindPresentedLimbFor(R->GetControlElement(C));
		if (!Switch.IsEmpty())
		{
			Switches.AddUnique(Switch);
		}
	}
	for (const FString& Switch : Switches)
	{
		SwitchLimbMatched(Switch);
	}
}

bool ARigExecStudioHUD::SwitchLimbMatched(const FString& SwitchPath)
{
	URigExecComponent* R = Rig();
	if (!R)
	{
		return false;
	}
	PushUndo();
	const FPose Before = CapturePose(Binding, FString());
	const bool bToIk = !R->IsLimbIk(SwitchPath);
	FString Limb;
	SwitchPath.Split(TEXT("."), &Limb, nullptr);
	Limb = ShortName(Limb);
	if (!R->SwitchLimb(SwitchPath))
	{
		Toast(FString::Printf(TEXT("%s: the IK/FK match failed"), *Limb));
		return false;
	}
	AutoKey(Before);
	Toast(FString::Printf(TEXT("%s to %s, matched"), *Limb, bToIk ? TEXT("IK") : TEXT("FK")));
	return true;
}

bool ARigExecStudioHUD::SwitchSpaceMatched(int32 Control, int32 Value)
{
	URigExecComponent* R = Rig();
	if (!R)
	{
		return false;
	}
	PushUndo();
	const FPose Before = CapturePose(Binding, FString());
	const FName Element = R->GetControlElement(Control);
	FName Channel;
	TArray<FString> Labels;
	R->FindSpaceChannel(Element, Channel, Labels);
	if (!R->SwitchSpace(Element, Value))
	{
		Toast(TEXT("That space could not be switched"));
		return false;
	}
	AutoKey(Before);
	Toast(FString::Printf(TEXT("%s in %s, matched"), *ShortName(R->GetControlPath(Control)),
	                      Labels.IsValidIndex(Value) ? *Labels[Value] : TEXT("?")));
	return true;
}

void ARigExecStudioHUD::ZeroControls()
{
	URigExecComponent* R = Rig();
	if (!R)
	{
		return;
	}
	PushUndo();
	const FPose Before = CapturePose(Binding, FString());
	TArray<int32> Which = Selected;
	if (Which.Num() == 0)
	{
		Binding.ControlOf.GenerateValueArray(Which);
	}
	for (const int32 C : Which)
	{
		double Rest[9];
		if (R->GetControlAvars(C, Rest, true))
		{
			R->SetControlAvars(C, Rest);
		}
		const FString Prefix = R->GetControlPath(C) + TEXT(".");
		for (const TPair<FString, int32>& Channel : Binding.ChannelOf)
		{
			if (Selected.Num() == 0 || Channel.Key.StartsWith(Prefix))
			{
				R->SetChannelValue(Channel.Value, R->GetChannelDefault(Channel.Value));
			}
		}
	}
	bDirty = true;
	AutoKey(Before);
	Toast(Selected.Num() > 0 ? TEXT("Zeroed the selection") : TEXT("Zeroed every control"));
}

// --- Undo ------------------------------------------------------------------------

ARigExecStudioHUD::FSnapshot ARigExecStudioHUD::Snapshot() const
{
	return {Edit, CapturePose(Binding, FString()), Selected};
}

void ARigExecStudioHUD::Restore(const FSnapshot& State)
{
	Edit = State.Edit;
	ApplyPose(State.Pose, 1.0f, Binding);
	Selected = State.Selected;
	AppliedFrame = Frame;
	bDirty = true;
}

void ARigExecStudioHUD::PushUndo()
{
	UndoStack.Add(Snapshot());
	if (UndoStack.Num() > GUndoDepth)
	{
		UndoStack.RemoveAt(0);
	}
	RedoStack.Reset();
}

void ARigExecStudioHUD::Undo(bool bRedo)
{
	TArray<FSnapshot>& From = bRedo ? RedoStack : UndoStack;
	TArray<FSnapshot>& To = bRedo ? UndoStack : RedoStack;
	if (From.Num() == 0)
	{
		Toast(bRedo ? TEXT("Nothing to redo") : TEXT("Nothing to undo"));
		return;
	}
	To.Add(Snapshot());
	const FSnapshot State = From.Pop();
	Restore(State);
	Toast(bRedo ? TEXT("Redo") : TEXT("Undo"));
}

// --- The camera ---------------------------------------------------------------------

void ARigExecStudioHUD::HomeCamera()
{
	ARigExecBipedCharacter* Hero = Bound.Get();
	URigExecComponent* R = Rig();
	if (!Hero || !R)
	{
		return;
	}
	// The whole character, from in front and a little to one side.
	const FVector Feet = R->GetComponentLocation();
	float Height = 180.0f;
	const int32 Head = R->FindControlIndex(TEXT("M_Head"));
	if (Head != INDEX_NONE)
	{
		const FVector At = R->GetComponentTransform().TransformPosition(R->GetRigFrame(R->GetControlPath(Head)).GetLocation());
		Height = FMath::Clamp(float(At.Z - Feet.Z) + 25.0f, 60.0f, 600.0f);
	}
	CamFocusWant = Feet + FVector(0.0, 0.0, Height * 0.5f);
	CamDistWant = Height * 2.0f;
	CamYaw = Hero->GetActorRotation().Yaw + 180.0f - 25.0f;
	CamPitch = -6.0f;
}

void ARigExecStudioHUD::FrameSelection()
{
	URigExecComponent* R = Rig();
	if (!R || Selected.Num() == 0)
	{
		HomeCamera();
		return;
	}
	FBox Box(ForceInit);
	for (const int32 C : Selected)
	{
		Box += R->GetComponentTransform().TransformPosition(R->GetRigFrame(R->GetControlPath(C)).GetLocation());
	}
	CamFocusWant = Box.GetCenter();
	CamDistWant = FMath::Clamp(float(Box.GetExtent().Size()) * 4.0f + 70.0f, 50.0f, 600.0f);
}

void ARigExecStudioHUD::ApplyCamera(float DeltaSeconds)
{
	ARigExecBipedCharacter* Hero = Bound.Get();
	if (!Hero || !Hero->CameraBoom || !PlayerOwner)
	{
		return;
	}
	if (IsVR())
	{
		// In a headset the camera is the player: moved at once, never glided
		// (a glide you did not make is what makes VR sickening).
		CamFocus = CamFocusWant;
		CamDist = CamDistWant;
	}
	else
	{
		CamFocus = FMath::VInterpTo(CamFocus, CamFocusWant, DeltaSeconds, 10.0f);
		CamDist = FMath::FInterpTo(CamDist, CamDistWant, DeltaSeconds, 10.0f);
	}
	USpringArmComponent* Boom = Hero->CameraBoom;
	Boom->TargetArmLength = CamDist;
	Boom->TargetOffset = CamFocus - Boom->GetComponentLocation();
	PlayerOwner->SetControlRotation(FRotator(CamPitch, CamYaw, 0.0f));
}

// --- Input -------------------------------------------------------------------------

bool ARigExecStudioHUD::Pressed(const FKey& Key) const
{
	return (PlayerOwner && PlayerOwner->WasInputKeyJustPressed(Key)) || (bDemo && DemoKeys.Contains(Key));
}

void ARigExecStudioHUD::ReadInput()
{
	MouseBefore = Mouse;
	float X = 0.0f, Y = 0.0f;
	if (PlayerOwner && PlayerOwner->GetMousePosition(X, Y))
	{
		Mouse = FVector2D(X, Y);
	}
	bPress = Pressed(EKeys::LeftMouseButton);
	bRelease = PlayerOwner && PlayerOwner->WasInputKeyJustReleased(EKeys::LeftMouseButton);
	bDown = PlayerOwner && PlayerOwner->IsInputKeyDown(EKeys::LeftMouseButton);
	bRightPress = Pressed(EKeys::RightMouseButton);
	bRightDown = PlayerOwner && PlayerOwner->IsInputKeyDown(EKeys::RightMouseButton);
	bMiddlePress = Pressed(EKeys::MiddleMouseButton);
	bMiddleDown = PlayerOwner && PlayerOwner->IsInputKeyDown(EKeys::MiddleMouseButton);
	auto Held = [this](const FKey& A, const FKey& B) {
		return PlayerOwner && (PlayerOwner->IsInputKeyDown(A) || PlayerOwner->IsInputKeyDown(B));
	};
	bShift = Held(EKeys::LeftShift, EKeys::RightShift);
	bCtrl = Held(EKeys::LeftControl, EKeys::RightControl);
	bAlt = Held(EKeys::LeftAlt, EKeys::RightAlt);
	Wheel = (Pressed(EKeys::MouseScrollUp) ? 1.0f : 0.0f) - (Pressed(EKeys::MouseScrollDown) ? 1.0f : 0.0f);
	bTaken = false;
	if (bDemo)
	{
		// The tour's cursor is the mouse.
		Mouse = DemoMouse;
		bPress = bDemoPress;
		bRelease = bDemoRelease;
		bDown = bDemoDown;
		bRightPress = bRightDown = bMiddlePress = bMiddleDown = false;
		bShift = bDemoShift;
		bCtrl = bAlt = false;
		Wheel = 0.0f;
	}
	if (IsVR())
	{
		// The laser on the panel is the mouse, its trigger the left button.
		if (VRIn.bOnPanel)
		{
			Mouse = VRIn.Pointer;
		}
		else if (Drag.Kind == EDrag::None)
		{
			Mouse = FVector2D(-10000.0, -10000.0);
		}
		bPress = VRIn.bTriggerPress && VRIn.bOnPanel;
		bRelease = VRIn.bTriggerRelease;
		bDown = VRIn.bTrigger;
		bRightPress = bRightDown = bMiddlePress = bMiddleDown = false;
		Wheel = 0.0f;
	}
}

bool ARigExecStudioHUD::HandleBack()
{
	if (bNaming)
	{
		bNaming = false;
		return true;
	}
	if (bSpaceMenu)
	{
		bSpaceMenu = false;
		return true;
	}
	if (Selected.Num() > 0)
	{
		Selected.Reset();
		return true;
	}
	return false;
}

void ARigExecStudioHUD::Shortcuts()
{
	if (Pressed(EKeys::SpaceBar))
	{
		bPlaying = !bPlaying;
		if (bPlaying && Frame >= Edit.Length)
		{
			Frame = 0.0f;
		}
	}
	if (Pressed(EKeys::Left) || Pressed(EKeys::Right))
	{
		bPlaying = false;
		SetFrame(float(FMath::RoundToInt(Frame) + (Pressed(EKeys::Left) ? -1 : 1)));
	}
	if (Pressed(EKeys::Comma) || Pressed(EKeys::Period))
	{
		bPlaying = false;
		SetFrame(float(KeyFrameNear(FMath::RoundToInt(Frame), Pressed(EKeys::Comma) ? -1 : 1)));
	}
	if (Pressed(EKeys::Home))
	{
		SetFrame(0.0f);
	}
	if (Pressed(EKeys::End))
	{
		SetFrame(float(Edit.Length));
	}
	if (Pressed(EKeys::S) && !bCtrl)
	{
		KeySelection(bShift);
	}
	if (Pressed(EKeys::Delete) || Pressed(EKeys::X))
	{
		DeleteKeys();
	}
	if (Pressed(EKeys::W))
	{
		PoseMode = EPoseMode::Move;
	}
	if (Pressed(EKeys::E))
	{
		PoseMode = EPoseMode::Rotate;
	}
	if (Pressed(EKeys::Q))
	{
		PoseMode = EPoseMode::Auto;
	}
	if (Pressed(EKeys::R))
	{
		PoseMode = EPoseMode::Scale;
	}
	if (Pressed(EKeys::K))
	{
		bAutoKey = !bAutoKey;
		Toast(bAutoKey ? TEXT("Auto Key on: every change is keyed") : TEXT("Auto Key off: press S to key"));
	}
	if (Pressed(EKeys::F))
	{
		FrameSelection();
	}
	if (Pressed(EKeys::H))
	{
		HomeCamera();
	}
	if (bCtrl && Pressed(EKeys::Z))
	{
		Undo(bShift);
	}
	if (bCtrl && Pressed(EKeys::Y))
	{
		Undo(true);
	}
	if (Picker.Panels.Num() > 0 && (Pressed(EKeys::LeftBracket) || Pressed(EKeys::RightBracket)))
	{
		PickerPanel = (PickerPanel + (Pressed(EKeys::LeftBracket) ? -1 : 1) + Picker.Panels.Num()) % Picker.Panels.Num();
	}
}

// --- Drags ---------------------------------------------------------------------

void ARigExecStudioHUD::ViewportPress()
{
	URigExecComponent* R = Rig();
	if (!R || !PlayerOwner)
	{
		return;
	}
	if (bRightPress || (bPress && bAlt))
	{
		Drag = FDrag();
		Drag.Kind = EDrag::Orbit;
		Drag.Start = Mouse;
		return;
	}
	if (bMiddlePress)
	{
		Drag = FDrag();
		Drag.Kind = EDrag::Pan;
		Drag.Start = Mouse;
		return;
	}
	if (!bPress)
	{
		return;
	}
	// A manipulator handle first: it is drawn over the skin.
	const int32 Handle = GizmoHandleAt(Mouse);
	if (Handle >= 0)
	{
		Drag = FDrag();
		Drag.Kind = EDrag::Gizmo;
		Drag.Start = Mouse;
		Drag.Index = Handle;
		return;
	}
	// TouchPose: the control under the cursor on the skin (or its dot).
	int32 Region = -1;
	const int32 Control = ControlUnder(Mouse, Region);
	Drag = FDrag();
	Drag.Start = Mouse;
	if (Control == INDEX_NONE || R->IsControlNull(Control))
	{
		// The empty view: a drag orbits, a click clears the selection.
		Drag.Kind = EDrag::Orbit;
		Drag.Region = -2;
		return;
	}
	if (bShift)
	{
		Select({Control}, 1);
		return;
	}
	if (bCtrl)
	{
		Select({Control}, 2);
		return;
	}
	if (!Selected.Contains(Control))
	{
		Select({Control}, 0);
	}
	else
	{
		// Already selected: it leads, and the selection drags together.
		Selected.Remove(Control);
		Selected.Add(Control);
	}
	Drag.Kind = EDrag::Pose;
	Drag.Control = Control;
}

void ARigExecStudioHUD::BeginPoseDrag()
{
	URigExecComponent* R = Rig();
	if (!R || Selected.Num() == 0)
	{
		Drag = FDrag();
		return;
	}
	R->Settle();
	Drag.Before = CapturePose(Binding, FString());
	PushUndo();
	bPlaying = false;
	// Rotate or move, by the lead control's channels and the mode.
	const int32 Lead = Selected.Last();
	const uint16 Mask = R->GetControlAvarMask(Lead);
	const bool bRotates = (Mask & 0x38) != 0, bMoves = (Mask & 0x07) != 0;
	Drag.Index = PoseMode == EPoseMode::Scale && (Mask & 0x1C0) != 0 ? 11 : -1;
	bool bTranslate = PoseMode == EPoseMode::Move || (PoseMode == EPoseMode::Auto && !bRotates);
	if (bCtrl)
	{
		bTranslate = !bTranslate;
	}
	if (bTranslate && !bMoves)
	{
		bTranslate = false;
	}
	if (!bTranslate && !bRotates)
	{
		bTranslate = true;
	}
	Drag.bTranslate = bTranslate;
	// Each selected control with no selected control above it, about its own pivot.
	for (const int32 C : Selected)
	{
		bool bUnder = false;
		for (int32 P = R->GetControlParent(C); P != INDEX_NONE && !bUnder; P = R->GetControlParent(P))
		{
			bUnder = Selected.Contains(P);
		}
		FPoseStart Start;
		Start.Control = C;
		Start.Value = R->GetLandedControlValueMatrix(C);
		if (bUnder || !R->GetControlAssetFrame(C, Start.Frame))
		{
			continue;
		}
		Start.Pivot = R->GetComponentTransform().TransformPosition(R->GetRigFrame(R->GetControlPath(C)).GetLocation());
		R->GetControlAvars(C, Start.Avars);
		Drag.Targets.Add(Start);
	}
}

void ARigExecStudioHUD::ApplyWorldEdit(const FQuat& Turn, const FVector& Move, double ScaleBy, int32 ScaleAxis)
{
	URigExecComponent* R = Rig();
	if (!R)
	{
		return;
	}
	const FTransform Component = R->GetComponentTransform();
	// The asset's axes in the component's (its linear part: the Y-up to Z-up turn).
	FMatrix L = R->AssetToComponent(FMatrix::Identity);
	L.M[3][0] = L.M[3][1] = L.M[3][2] = 0.0;
	const FMatrix LInverse = L.Inverse();
	const double Cm = R->GetCentimetresPerUnit();
	const FQuat Local = Component.GetRotation().Inverse() * Turn * Component.GetRotation();
	const FMatrix TurnInAsset = L * FQuatRotationMatrix(Local) * LInverse;
	const FVector MoveInAsset = LInverse.TransformVector(Component.InverseTransformVector(Move)) / Cm;
	for (const FPoseStart& Target : Drag.Targets)
	{
		if (ScaleBy != 1.0)
		{
			// Scale is the control's own: its sx sy sz.
			double V[9];
			FMemory::Memcpy(V, Target.Avars, sizeof(V));
			for (int32 A = 0; A < 3; ++A)
			{
				if (ScaleAxis < 0 || ScaleAxis == A)
				{
					V[6 + A] = FMath::Max(Target.Avars[6 + A] * ScaleBy, 0.001);
				}
			}
			R->SetControlAvars(Target.Control, V);
			continue;
		}
		FMatrix Moved = Target.Frame;
		FMatrix Linear = Target.Frame;
		Linear.M[3][0] = Linear.M[3][1] = Linear.M[3][2] = 0.0;
		Moved = Linear * TurnInAsset;
		Moved.M[3][0] = Target.Frame.M[3][0] + MoveInAsset.X;
		Moved.M[3][1] = Target.Frame.M[3][1] + MoveInAsset.Y;
		Moved.M[3][2] = Target.Frame.M[3][2] + MoveInAsset.Z;
		// The value that puts the control on that frame: what it carries
		// between its value and its parent is unchanged by its own move.
		R->SetControlValueMatrix(Target.Control, Moved * Target.Frame.Inverse() * Target.Value);
	}
	bDirty = true;
}

void ARigExecStudioHUD::UpdatePoseDrag()
{
	URigExecComponent* R = Rig();
	if (!R || !PlayerOwner || Drag.Targets.Num() == 0)
	{
		return;
	}
	const FVector2D Total = Mouse - Drag.Start;
	FVector Eye, Forward, Right, Up;
	GetView(Eye, Forward, Right, Up);
	const double Slow = bShift ? 0.25 : 1.0;
	if (Drag.Index == 11)
	{
		// Scale on the skin: across, evenly.
		ApplyWorldEdit(FQuat::Identity, FVector::ZeroVector, FMath::Exp(Total.X * 0.006 * Slow), -1);
		return;
	}
	if (Drag.bTranslate)
	{
		// The pivot follows the cursor across the view.
		const double PerPixel = PixelSizeAt(Drag.Targets.Last().Pivot);
		ApplyWorldEdit(FQuat::Identity, (Right * Total.X - Up * Total.Y) * PerPixel * Slow, 1.0, -1);
		return;
	}
	// A trackball: the surface under the cursor goes where the cursor goes.
	// Across turns about the view's up, up and down about its side; Alt
	// twists about the view.
	const double Degrees = 0.45 * Slow;
	const FQuat Turn = bAlt ? FQuat(Forward, FMath::DegreesToRadians(Total.X * Degrees))
	                        : FQuat(Up, FMath::DegreesToRadians(-Total.X * Degrees)) *
	                              FQuat(Right, FMath::DegreesToRadians(-Total.Y * Degrees));
	ApplyWorldEdit(Turn, FVector::ZeroVector, 1.0, -1);
}

void ARigExecStudioHUD::EndDrag()
{
	URigExecComponent* R = Rig();
	switch (Drag.Kind)
	{
	case EDrag::Pose:
	case EDrag::Gizmo:
	case EDrag::Grab:
	case EDrag::Avar:
	case EDrag::Channel:
		if (Drag.bMoved && R)
		{
			AutoKey(Drag.Before);
		}
		break;
	case EDrag::Orbit:
		if (!Drag.bMoved && Drag.Region == -2 && !bShift)
		{
			Selected.Reset();
		}
		break;
	case EDrag::Keys:
		if (Drag.bMoved && Drag.KeyAt != Drag.KeyFrom)
		{
			PushUndo();
			const TSet<FString> Only = SelectedPaths(true);
			Edit.MoveKeys(Drag.KeyFrom, Drag.KeyAt, Drag.bKeysOfSelection ? &Only : nullptr);
			bEditChanged = true;
			SetFrame(float(Drag.KeyAt));
			ApplyEdit(true);
		}
		break;
	default:
		break;
	}
	Drag = FDrag();
}

// --- Drawing helpers ------------------------------------------------------------

bool ARigExecStudioHUD::Inside(float X, float Y, float W, float H) const
{
	return Mouse.X >= X && Mouse.X <= X + W && Mouse.Y >= Y && Mouse.Y <= Y + H;
}

bool ARigExecStudioHUD::OverPanel(const FVector2D& At) const
{
	auto In = [&At](const FBox2D& Box) { return Box.bIsValid && Box.IsInside(At); };
	return In(TopBox) || In(TimelineBox) || (bShowLibrary && In(LibraryBox)) || (bShowPicker && In(SideBox)) || (Canvas && At.Y > Canvas->ClipY - 28.0f * S);
}

float ARigExecStudioHUD::LabelWidth(const FString& Text, float Size) const
{
	UFont* Font = UiFont();
	if (!Font || !Canvas)
	{
		return 0.0f;
	}
	float W = 0.0f, H = 0.0f;
	Canvas->TextSize(Font, Text, W, H);
	return W * Size * S / FMath::Max(float(Font->GetMaxCharHeight()), 1.0f);
}

void ARigExecStudioHUD::Label(const FString& Text, float X, float Y, float Size, const FLinearColor& Color, int32 Align)
{
	UFont* Font = UiFont();
	if (!Font || !Canvas || Text.IsEmpty())
	{
		return;
	}
	const float Scale = Size * S / FMath::Max(float(Font->GetMaxCharHeight()), 1.0f);
	float W = 0.0f, H = 0.0f;
	Canvas->TextSize(Font, Text, W, H);
	W *= Scale;
	H *= Scale;
	const float Left = Align < 0 ? X : Align > 0 ? X - W : X - W * 0.5f;
	FCanvasTextItem Item(FVector2D(Left, Y - H * 0.5f), FText::FromString(Text), Font, Color);
	Item.BlendMode = ARigExecVRRig::UiTextBlend();
	Item.Scale = FVector2D(Scale, Scale);
	Item.bOutlined = true;
	Item.OutlineColor = FLinearColor(0, 0, 0, 0.6f);
	Canvas->DrawItem(Item);
}

bool ARigExecStudioHUD::UiButton(const FString& Text, float X, float Y, float W, float H, bool bOn, bool bEnabled)
{
	DemoTargets.Add(Text, FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + H)));
	const bool bHover = bEnabled && Inside(X, Y, W, H);
	const FLinearColor Fill = !bEnabled ? FLinearColor(0.1f, 0.11f, 0.13f, 0.8f)
	                          : bOn     ? GAccent
	                          : bHover  ? FLinearColor(0.2f, 0.3f, 0.42f, 0.95f)
	                                    : FLinearColor(0.1f, 0.15f, 0.22f, 0.92f);
	DrawRect(Fill, X, Y, W, H);
	Label(Text, X + W * 0.5f, Y + H * 0.5f, 12.0f, bEnabled ? FLinearColor::White : GDim, 0);
	if (bHover && bPress && !bTaken)
	{
		bTaken = true;
		return true;
	}
	return false;
}

void ARigExecStudioHUD::Panel(const FBox2D& Box, const FString& Title)
{
	const FVector2D Size = Box.GetSize();
	DrawRect(GPanel, Box.Min.X, Box.Min.Y, Size.X, Size.Y);
	DrawRect(GCyan, Box.Min.X, Box.Min.Y, Size.X, 2.0f * S);
	if (!Title.IsEmpty())
	{
		Label(Title, Box.Min.X + 10.0f * S, Box.Min.Y + 14.0f * S, 13.0f, GCyan);
	}
}

void ARigExecStudioHUD::Toast(const FString& Text)
{
	ToastText = Text;
	ToastUntil = FPlatformTime::Seconds() + 2.8;
	UE_LOG(LogRigExecStudioHud, Log, TEXT("studio: %s"), *Text);
}

static void DrawDiamond(UCanvas* Canvas, float X, float Y, float R, const FLinearColor& Color)
{
	TArray<FCanvasUVTri> Tris;
	FCanvasUVTri A, B;
	A.V0_Pos = FVector2D(X, Y - R);
	A.V1_Pos = FVector2D(X + R, Y);
	A.V2_Pos = FVector2D(X - R, Y);
	B.V0_Pos = FVector2D(X - R, Y);
	B.V1_Pos = FVector2D(X + R, Y);
	B.V2_Pos = FVector2D(X, Y + R);
	A.V0_Color = A.V1_Color = A.V2_Color = B.V0_Color = B.V1_Color = B.V2_Color = Color;
	Tris.Add(A);
	Tris.Add(B);
	FCanvasTriangleItem Item(Tris, GWhiteTexture);
	Item.BlendMode = ARigExecVRRig::UiBlend();
	Canvas->DrawItem(Item);
}

// --- The frame -------------------------------------------------------------------

void ARigExecStudioHUD::DrawGame()
{
	if (!Canvas || !PlayerOwner || IsMenuOpen())
	{
		return;
	}
	const float W = Canvas->ClipX, H = Canvas->ClipY;
	S = H / 720.0f;
	ReadInput();
	URigExecComponent* R = Rig();
	if (!R)
	{
		Label(TEXT("Loading the character..."), W * 0.5f, H * 0.5f, 18.0f, GText, 0);
		return;
	}
	TopBox = FBox2D(FVector2D(0, 0), FVector2D(W, 36.0f * S));
	TimelineBox = FBox2D(FVector2D(0, H - 150.0f * S), FVector2D(W, H - 28.0f * S));
	LibraryBox = FBox2D(FVector2D(10.0f * S, 46.0f * S), FVector2D(250.0f * S, H - 160.0f * S));
	SideBox = FBox2D(FVector2D(W - 380.0f * S, 46.0f * S), FVector2D(W - 10.0f * S, H - 160.0f * S));
	const bool bModal = bNaming || bBaking;
	if (bModal)
	{
		bTaken = true;
	}
	if (bSpaceMenu)
	{
		DrawSpaceMenu(false);
	}

	// The view first, the panels over it.
	double Clock = FPlatformTime::Seconds();
	auto Lap = [&Clock](double& Into) {
		const double Now = FPlatformTime::Seconds();
		Into += Now - Clock;
		Clock = Now;
	};
	DrawViewportMarks();
	Lap(ProfileSpent[2]);
	DrawTop(W, H);
	if (bShowLibrary)
	{
		DrawLibrary();
	}
	Lap(ProfileSpent[4]);
	if (bShowPicker)
	{
		DrawSide();
	}
	Lap(ProfileSpent[1]);
	DrawTimeline();
	Lap(ProfileSpent[3]);
	Label(IsVR() ? TEXT("trigger  click / drag   grip  hold a control   A play   B undo   X key   Y frame   "
	                    "right stick  turn / nearer   left stick  frames / up-down   stick click  home   menu button  menu")
	             : TEXT("Space play   </>  frame   ,/.  key   S key   Del delete   K auto key   W/E/R/Q move/rotate/scale/auto   "
	                    "RMB orbit   MMB pan   wheel zoom   F frame   H home   Ctrl+Z undo"),
	      W - 12.0f * S, H - 14.0f * S, 10.5f, GDim, 1);
	// VR: the grip holds a control in the hand.
	if (IsVR() && !bModal)
	{
		UpdateGrab();
	}

	// What the panels left: the view's press and the drag in hand.
	const bool bPanelPress = (bPress || bRightPress || bMiddlePress) && OverPanel(Mouse);
	if (!bTaken && !bPanelPress && Drag.Kind == EDrag::None && (bPress || bRightPress || bMiddlePress))
	{
		ViewportPress();
	}
	if (!bModal && !bTaken && Wheel != 0.0f && !OverPanel(Mouse))
	{
		CamDistWant = FMath::Clamp(CamDistWant * FMath::Pow(0.88f, Wheel), 25.0f, 2000.0f);
	}
	if (Drag.Kind != EDrag::None && Drag.Kind != EDrag::Grab)
	{
		const bool bHeld = Drag.Kind == EDrag::Orbit && !Drag.bMoved && bRightDown ? true
		                   : Drag.Kind == EDrag::Orbit ? (bDown || bRightDown)
		                   : Drag.Kind == EDrag::Pan   ? bMiddleDown
		                                               : bDown;
		// A laser shakes more than a mouse: it must go further to be a drag.
		if (!Drag.bMoved && FVector2D::Distance(Mouse, Drag.Start) > (IsVR() ? 6.0f * S : GDragStart))
		{
			Drag.bMoved = true;
			if (Drag.Kind == EDrag::Pose || Drag.Kind == EDrag::Gizmo)
			{
				const int32 Handle = Drag.Index;
				BeginPoseDrag();
				if (Drag.Kind == EDrag::Gizmo)
				{
					Drag.Index = Handle;
				}
			}
		}
		const FVector2D Step = Mouse - MouseBefore;
		switch (Drag.Kind)
		{
		case EDrag::Orbit:
			// In VR the world never turns under you: the right stick turns you.
			if (Drag.bMoved && !IsVR())
			{
				CamYaw += float(Step.X) * 0.3f;
				CamPitch = FMath::Clamp(CamPitch - float(Step.Y) * 0.3f, -85.0f, 85.0f);
			}
			break;
		case EDrag::Pan:
		{
			const FRotationMatrix View(FRotator(CamPitch, CamYaw, 0.0f));
			const float PerPixel = CamDist * 1.6f / FMath::Max(W, 1.0f);
			const FVector Move = (-View.GetUnitAxis(EAxis::Y) * Step.X + View.GetUnitAxis(EAxis::Z) * Step.Y) * PerPixel;
			CamFocus += Move;
			CamFocusWant += Move;
			break;
		}
		case EDrag::Pose:
			if (Drag.bMoved && Drag.Targets.Num() > 0)
			{
				UpdatePoseDrag();
			}
			break;
		case EDrag::Gizmo:
			if (Drag.bMoved && Drag.Targets.Num() > 0)
			{
				UpdateGizmoDrag();
			}
			break;
		case EDrag::Avar:
		case EDrag::Channel:
			if (Drag.bMoved)
			{
				const double Dx = (Mouse.X - Drag.Start.X) / S * (bShift ? 0.1 : 1.0);
				if (Drag.Kind == EDrag::Avar)
				{
					double V[9];
					if (R->GetControlAvars(Drag.Control, V))
					{
						const double PerPixel = Drag.Index < 3 ? 0.05 : Drag.Index < 6 ? 0.5 : 0.005;
						V[Drag.Index] = Drag.StartValue + Dx * PerPixel;
						R->SetControlAvars(Drag.Control, V);
					}
				}
				else
				{
					R->SetChannelValue(Drag.Index, Drag.StartValue + Dx * 0.01);
				}
				bDirty = true;
			}
			break;
		case EDrag::Mix:
			PoseMix = FMath::Clamp(float((Mouse.X - Drag.Start.X) / FMath::Max(Drag.StartValue, 1.0)), 0.0f, 1.0f);
			break;
		default:
			break;
		}
		if (!bHeld)
		{
			EndDrag();
		}
	}

	GizmoHover = Drag.Kind == EDrag::Gizmo ? Drag.Index : (Drag.Kind == EDrag::None && !OverPanel(Mouse) ? GizmoHandleAt(Mouse) : -1);
	const double PickStart = FPlatformTime::Seconds();
	// TouchPose's hover: the region under a still cursor in the view.
	if (!bModal && !bPlaying && (Drag.Kind == EDrag::None) && !OverPanel(Mouse) && GizmoHover < 0)
	{
		if (!Mouse.Equals(LastPickMouse, 0.5) || bDirty)
		{
			LastPickMouse = Mouse;
			HoverControl = ControlUnder(Mouse, HoverRegion);
		}
	}
	else if (OverPanel(Mouse) || bPlaying)
	{
		HoverRegion = -1;
		HoverControl = INDEX_NONE;
	}
	ProfileSpent[5] += FPlatformTime::Seconds() - PickStart;
	TArray<FName> Elements;
	for (const int32 C : Selected)
	{
		Elements.Add(R->GetControlElement(C));
	}
	const TArray<int32> Regions = R->GetTouchRegionsOf(Elements);
	const TArray<int32> Lead = Selected.Num() > 0 ? R->GetTouchRegionsOf({R->GetControlElement(Selected.Last())}) : TArray<int32>();
	R->SetTouchHighlight(FMath::Max(HoverRegion, -1), Lead.Num() > 0 ? Lead[0] : -1, Regions);

	if (bSpaceMenu)
	{
		DrawSpaceMenu(true);
	}
	if (bBaking)
	{
		DrawBaking(W, H);
	}
	if (bNaming)
	{
		TypeName();
		DrawNaming(W, H);
	}
	else if (!bModal)
	{
		Shortcuts();
		if (IsVR())
		{
			VRControls();
		}
	}
	if (FPlatformTime::Seconds() < ToastUntil)
	{
		const float TW = LabelWidth(ToastText, 14.0f) + 40.0f * S;
		DrawRect(FLinearColor(0.02f, 0.04f, 0.07f, 0.88f), W * 0.5f - TW * 0.5f, 46.0f * S, TW, 30.0f * S);
		DrawRect(GAccent, W * 0.5f - TW * 0.5f, 46.0f * S, 3.0f * S, 30.0f * S);
		Label(ToastText, W * 0.5f, 61.0f * S, 14.0f, FLinearColor::White, 0);
	}
}

bool ARigExecStudioHUD::GetVRStage(FVector& LookAt, float& Yaw, float& Distance, float& FloorZ) const
{
	const URigExecComponent* R = Rig();
	if (IsChoosing() || !R)
	{
		return Super::GetVRStage(LookAt, Yaw, Distance, FloorZ);
	}
	// Where the orbit camera would be, on the floor the character stands on
	// (never tilted: the head does that).
	LookAt = CamFocus;
	Yaw = CamYaw;
	Distance = CamDist;
	FloorZ = float(R->GetComponentLocation().Z);
	return true;
}

void ARigExecStudioHUD::VRControls()
{
	const float Dt = FMath::Min(float(FApp::GetDeltaTime()), 0.1f);
	if (VRPressed(EKeys::OculusTouch_Right_A_Click))
	{
		bPlaying = !bPlaying;
		if (bPlaying && Frame >= Edit.Length)
		{
			Frame = 0.0f;
		}
	}
	if (VRPressed(EKeys::OculusTouch_Right_B_Click))
	{
		Undo(false);
	}
	if (VRPressed(EKeys::OculusTouch_Left_X_Click))
	{
		KeySelection(false);
	}
	if (VRPressed(EKeys::OculusTouch_Left_Y_Click))
	{
		FrameSelection();
	}
	if (VRPressed(EKeys::OculusTouch_Right_Thumbstick_Click) || VRPressed(EKeys::OculusTouch_Left_Thumbstick_Click))
	{
		HomeCamera();
	}
	// The right stick: a snap turn about the character, and nearer or further.
	if (VRIn.Flick[1].X != 0)
	{
		CamYaw += 30.0f * VRIn.Flick[1].X;
	}
	if (FMath::Abs(VRIn.Stick[1].Y) > 0.2)
	{
		CamDistWant = FMath::Clamp(CamDistWant * FMath::Pow(0.35f, float(VRIn.Stick[1].Y) * Dt), 40.0f, 1500.0f);
	}
	// The left stick: a frame at a time across; up and down raises what you look at.
	if (VRIn.Flick[0].X != 0)
	{
		bPlaying = false;
		SetFrame(float(FMath::RoundToInt(Frame) + VRIn.Flick[0].X));
	}
	if (FMath::Abs(VRIn.Stick[0].Y) > 0.2)
	{
		const float Rise = float(VRIn.Stick[0].Y) * Dt * 60.0f;
		CamFocus.Z += Rise;
		CamFocusWant.Z += Rise;
	}
}

void ARigExecStudioHUD::UpdateGrab()
{
	URigExecComponent* R = Rig();
	if (!R)
	{
		return;
	}
	if (Drag.Kind == EDrag::Grab)
	{
		if (!VRIn.bGrip || Drag.Targets.Num() == 0)
		{
			EndDrag();
			return;
		}
		const FPoseStart& Lead = Drag.Targets.Last();
		const uint16 Mask = R->GetControlAvarMask(Lead.Control);
		const bool bMoves = (Mask & 0x07) != 0, bRotates = (Mask & 0x38) != 0;
		const FQuat Delta = (VRIn.GripTurn * GrabTurn.Inverse()).GetNormalized();
		FQuat Turn = FQuat::Identity;
		FVector Move = FVector::ZeroVector;
		if (bMoves)
		{
			// It goes where the hand goes, and turns with it if it can.
			Move = VRIn.GripAt - GrabFrom;
			Turn = bRotates ? Delta : FQuat::Identity;
		}
		else if (bRotates)
		{
			const FVector From = GrabFrom - Lead.Pivot, To = VRIn.GripAt - Lead.Pivot;
			if (From.Size() < 60.0 && From.Size() > 3.0 && To.Size() > 3.0)
			{
				// Held near: it swings to point where the hand went, and
				// twists as the wrist does.
				const FVector Axis = From.GetSafeNormal();
				const FVector Along = Axis * FVector::DotProduct(FVector(Delta.X, Delta.Y, Delta.Z), Axis);
				FQuat Twist(Along.X, Along.Y, Along.Z, Delta.W);
				Twist = Twist.SizeSquared() > 1e-8 ? Twist.GetNormalized() : FQuat::Identity;
				Turn = FQuat::FindBetweenVectors(From, To) * Twist;
			}
			else
			{
				// Held from afar: it turns as the hand turns.
				Turn = Delta;
			}
		}
		ApplyWorldEdit(Turn, Move, 1.0, -1);
		return;
	}
	if (!VRIn.bGripPress || Drag.Kind != EDrag::None)
	{
		return;
	}
	// What the laser is on (or, on nothing, the selection) is taken in hand.
	int32 Region = -1;
	const int32 Control = VRIn.bOnPanel && !OverPanel(Mouse) ? ControlUnder(Mouse, Region) : INDEX_NONE;
	if (Control != INDEX_NONE && !R->IsControlNull(Control))
	{
		if (!Selected.Contains(Control))
		{
			Select({Control}, 0);
		}
		else
		{
			Selected.Remove(Control);
			Selected.Add(Control);
		}
	}
	if (Selected.Num() == 0)
	{
		return;
	}
	Drag = FDrag();
	Drag.Kind = EDrag::Grab;
	Drag.Start = Mouse;
	Drag.bMoved = true;
	BeginPoseDrag();
	if (Drag.Targets.Num() == 0)
	{
		Drag = FDrag();
		return;
	}
	GrabFrom = VRIn.GripAt;
	GrabTurn = VRIn.GripTurn;
}

bool ARigExecStudioHUD::CounterAnchor(float W, float H, float InS, float& X, float& Y) const
{
	// The bottom left of the view, just above the timeline.
	X = (bShowLibrary ? 260.0f : 10.0f) * InS;
	Y = H - 158.0f * InS;
	return false;
}

void ARigExecStudioHUD::DrawTop(float W, float H)
{
	DrawRect(FLinearColor(0.01f, 0.02f, 0.04f, 0.9f), 0.0f, 0.0f, W, TopBox.Max.Y);
	DrawRect(GCyan, 0.0f, TopBox.Max.Y - 2.0f * S, W, 2.0f * S);
	Label(TEXT("ANIMATION STUDIO"), 14.0f * S, 18.0f * S, 16.0f, FLinearColor::White);
	Label(CharacterName().ToUpper(), 14.0f * S + LabelWidth(TEXT("ANIMATION STUDIO"), 16.0f) + 12.0f * S, 18.0f * S, 14.0f, GKey);
	const float Y = 5.0f * S, BH = 26.0f * S;
	float X = 300.0f * S;
	auto Next = [&](const FString& Text, float Width, bool bOn, bool bEnabled = true) {
		const bool bHit = UiButton(Text, X, Y, Width * S, BH, bOn, bEnabled);
		X += Width * S + 4.0f * S;
		return bHit;
	};
	if (Next(TEXT("Auto (Q)"), 72.0f, PoseMode == EPoseMode::Auto))
	{
		PoseMode = EPoseMode::Auto;
	}
	if (Next(TEXT("Rotate (E)"), 82.0f, PoseMode == EPoseMode::Rotate))
	{
		PoseMode = EPoseMode::Rotate;
	}
	if (Next(TEXT("Move (W)"), 76.0f, PoseMode == EPoseMode::Move))
	{
		PoseMode = EPoseMode::Move;
	}
	if (Next(TEXT("Scale (R)"), 76.0f, PoseMode == EPoseMode::Scale))
	{
		PoseMode = EPoseMode::Scale;
	}
	X += 10.0f * S;
	if (Next(TEXT("Library"), 66.0f, bShowLibrary))
	{
		bShowLibrary = !bShowLibrary;
	}
	if (Next(TEXT("Picker"), 60.0f, bShowPicker))
	{
		bShowPicker = !bShowPicker;
	}
	X += 10.0f * S;
	if (Next(TEXT("Zero"), 50.0f, false))
	{
		ZeroControls();
	}
	if (Next(TEXT("Undo"), 50.0f, false, UndoStack.Num() > 0))
	{
		Undo(false);
	}
	if (Next(TEXT("Redo"), 50.0f, false, RedoStack.Num() > 0))
	{
		Undo(true);
	}
	if (Next(TEXT("Frame (F)"), 66.0f, false))
	{
		FrameSelection();
	}
	if (Next(TEXT("Home (H)"), 64.0f, false))
	{
		HomeCamera();
	}
	X = W - 200.0f * S;
	if (Next(TEXT("Character"), 92.0f, false, ARigExecBipedCharacter::FindCharacters().Num() > 1))
	{
		ReopenChooser();
	}
	if (Next(TEXT("Menu"), 92.0f, false))
	{
		OpenMenu();
	}
}

void ARigExecStudioHUD::DrawLibrary()
{
	Panel(LibraryBox, TEXT("LIBRARY"));
	const float X = LibraryBox.Min.X + 8.0f * S, W = LibraryBox.GetSize().X - 16.0f * S;
	const bool bModal = bNaming || bBaking;
	const float Top = LibraryBox.Min.Y + 30.0f * S, Bottom = LibraryBox.Max.Y - 72.0f * S;
	const float Row = 22.0f * S;
	const float Half = (Bottom - Top) * 0.5f;
	auto List = [&](bool bPose, float Y0, float Height, int32& Scroll) {
		const int32 Count = bPose ? Poses.Num() : Clips.Num();
		Label(bPose ? TEXT("POSES") : TEXT("ANIMATIONS"), X, Y0 + 10.0f * S, 12.0f, GText);
		if (UiButton(bPose ? TEXT("+ Save Pose") : TEXT("+ Save Anim"), X + W - 96.0f * S, Y0, 96.0f * S, 20.0f * S, false,
		             !bModal && (bPose || !Edit.IsEmpty())))
		{
			bNaming = true;
			bNamingPose = bPose;
			int32 Mine = 1;
			for (; Mine < 1000; ++Mine)
			{
				const FString Try = FString::Printf(TEXT("My %s %d"), bPose ? TEXT("Pose") : TEXT("Animation"), Mine);
				const bool bUsed = bPose ? Poses.ContainsByPredicate([&](const FPose& P) { return P.Name == Try; })
				                         : Clips.ContainsByPredicate([&](const FClip& C) { return C.Name == Try; });
				if (!bUsed)
				{
					break;
				}
			}
			NameText = !bPose && !Edit.Name.IsEmpty() && Edit.Name != TEXT("Untitled") &&
			                   !Clips.ContainsByPredicate([&](const FClip& C) { return C.bBuiltIn && C.Name == Edit.Name; })
			               ? Edit.Name
			               : FString::Printf(TEXT("My %s %d"), bPose ? TEXT("Pose") : TEXT("Animation"), Mine);
		}
		const float ListTop = Y0 + 26.0f * S;
		const int32 Fits = FMath::Max(1, FMath::FloorToInt((Height - 28.0f * S) / Row));
		if (Inside(X, ListTop, W, Fits * Row) && Wheel != 0.0f)
		{
			Scroll -= int32(Wheel);
			Wheel = 0.0f;
		}
		Scroll = FMath::Clamp(Scroll, 0, FMath::Max(0, Count - Fits));
		for (int32 Shown = 0; Shown < Fits && Scroll + Shown < Count; ++Shown)
		{
			const int32 I = Scroll + Shown;
			const FString& Name = bPose ? Poses[I].Name : Clips[I].Name;
			const bool bBuilt = bPose ? Poses[I].bBuiltIn : Clips[I].bBuiltIn;
			const float Y = ListTop + Shown * Row;
			DemoTargets.Add((bPose ? TEXT("pose:") : TEXT("clip:")) + Name, FBox2D(FVector2D(X, Y), FVector2D(X + W * 0.6f, Y + Row - 2.0f * S)));
			const bool bHover = Inside(X, Y, W, Row - 2.0f * S);
			const bool bCurrent = !bPose && Edit.Name == Name && !bEditChanged;
			DrawRect(bCurrent ? FLinearColor(0.12f, 0.3f, 0.46f, 0.95f) : bHover ? FLinearColor(0.16f, 0.24f, 0.34f, 0.95f)
			                                                                    : FLinearColor(0.07f, 0.1f, 0.15f, 0.9f),
			         X, Y, W, Row - 2.0f * S);
			Label(Name, X + 8.0f * S, Y + Row * 0.5f - 1.0f * S, 12.0f, FLinearColor::White);
			if (bBuilt)
			{
				Label(TEXT("default"), X + W - 8.0f * S, Y + Row * 0.5f - 1.0f * S, 10.0f, GDim, 1);
			}
			else if (bHover)
			{
				if (UiButton(TEXT("x"), X + W - 22.0f * S, Y + 2.0f * S, 18.0f * S, Row - 6.0f * S))
				{
					DeleteItem(bPose, I);
					return;
				}
			}
			if (bHover && bPress && !bTaken)
			{
				bTaken = true;
				bPose ? UsePose(I) : UseClip(I, bShift);
			}
		}
		if (Count == 0)
		{
			Label(bBaking ? TEXT("baking...") : TEXT("none yet"), X + 8.0f * S, ListTop + Row * 0.5f, 11.0f, GDim);
		}
	};
	List(false, Top, Half, ClipScroll);
	List(true, Top + Half, Half, PoseScroll);

	// How much of a pose to take: 100% is the pose, less blends toward it.
	const float MixY = Bottom + 6.0f * S, MixW = W - 70.0f * S;
	Label(FString::Printf(TEXT("Pose mix %d%%"), FMath::RoundToInt(PoseMix * 100.0f)), X, MixY + 8.0f * S, 11.0f, GText);
	const float BarX = X + 100.0f * S, BarW = W - 100.0f * S;
	DrawRect(FLinearColor(0.1f, 0.12f, 0.16f, 1.0f), BarX, MixY + 4.0f * S, BarW, 8.0f * S);
	DrawRect(GAccent, BarX, MixY + 4.0f * S, BarW * PoseMix, 8.0f * S);
	if (Inside(BarX, MixY - 2.0f * S, BarW, 20.0f * S) && bPress && !bTaken)
	{
		bTaken = true;
		Drag = FDrag();
		Drag.Kind = EDrag::Mix;
		Drag.Start = FVector2D(BarX, MixY);
		Drag.StartValue = BarW;
		Drag.bMoved = true;
		PoseMix = FMath::Clamp(float((Mouse.X - BarX) / BarW), 0.0f, 1.0f);
	}
	(void)MixW;
	Label(TEXT("Click to apply   Shift+click: add at the frame"), X, MixY + 26.0f * S, 9.5f, GDim);
	if (UiButton(TEXT("Rebake defaults"), X, LibraryBox.Max.Y - 28.0f * S, W, 22.0f * S, false, !bBaking))
	{
		StartBake();
	}
}

void ARigExecStudioHUD::DrawSide()
{
	Panel(SideBox, Picker.Panels.Num() > 0 ? FString() : TEXT("PICKER"));
	const float X = SideBox.Min.X + 8.0f * S, W = SideBox.GetSize().X - 16.0f * S;
	const float ChannelsH = 210.0f * S;
	float Y = SideBox.Min.Y + 6.0f * S;
	if (Picker.Panels.Num() == 0)
	{
		Label(TEXT("This character has no picker."), X, Y + 40.0f * S, 11.0f, GDim);
	}
	else
	{
		// The tabs: one per picker panel.
		float TabX = X;
		for (int32 P = 0; P < Picker.Panels.Num(); ++P)
		{
			const float TabW = LabelWidth(Picker.Panels[P].Label, 12.0f) + 18.0f * S;
			if (UiButton(Picker.Panels[P].Label, TabX, Y, TabW, 22.0f * S, P == PickerPanel))
			{
				PickerPanel = P;
			}
			TabX += TabW + 3.0f * S;
		}
		PickerPanel = FMath::Clamp(PickerPanel, 0, Picker.Panels.Num() - 1);
		const FPickerPanel& Shown = Picker.Panels[PickerPanel];
		const float AreaY = Y + 28.0f * S, AreaH = SideBox.Max.Y - ChannelsH - AreaY - 8.0f * S;
		const float Scale = FMath::Min(W / FMath::Max(Shown.Size.X, 1.0f), AreaH / FMath::Max(Shown.Size.Y, 1.0f));
		const FVector2f Offset(X + (W - Shown.Size.X * Scale) * 0.5f, AreaY + (AreaH - Shown.Size.Y * Scale) * 0.5f);
		int32 Hovered = INDEX_NONE;
		if (Inside(X, AreaY, W, AreaH))
		{
			const FVector2f At = (FVector2f(Mouse) - Offset) / Scale;
			Hovered = Picker.HitAt(PickerPanel, At);
			if (bPress && !bTaken)
			{
				bTaken = true;
				if (Hovered != INDEX_NONE)
				{
					PickButton(PickerPanel, Hovered);
				}
			}
		}
		Picker.Draw(Canvas, PickerPanel, Offset, Scale, Hovered, Selected, 1.0f);
		for (const FPickerButton& Button : Shown.Buttons)
		{
			if (Button.Targets.Num() == 1 && Picker.Shown(Button) == 1)
			{
				DemoTargets.Add(TEXT("pick:") + ShortName(Rig()->GetControlPath(Button.Targets[0])),
				                FBox2D(FVector2D(Offset + Button.Box.Min * Scale), FVector2D(Offset + Button.Box.Max * Scale)));
			}
		}
		if (Hovered != INDEX_NONE)
		{
			const FPickerButton& Button = Shown.Buttons[Hovered];
			FString Tip = Button.ChannelPath.IsEmpty() ? FString() : ShortName(Button.ChannelPath);
			for (const int32 T : Button.Targets)
			{
				Tip += (Tip.IsEmpty() ? TEXT("") : TEXT(", ")) + ShortName(Rig()->GetControlPath(T));
			}
			if (Picker.Shown(Button) == 2)
			{
				Tip += TEXT("  (idle half: picking it switches the limb, matched)");
			}
			Label(Tip, X, SideBox.Max.Y - ChannelsH - 6.0f * S, 10.5f, GKey);
		}
	}
	DrawChannels(X, SideBox.Max.Y - ChannelsH, W, ChannelsH - 6.0f * S);
}

void ARigExecStudioHUD::PickButton(int32 PanelIndex, int32 ButtonIndex)
{
	URigExecComponent* R = Rig();
	const FPickerButton& Button = Picker.Panels[PanelIndex].Buttons[ButtonIndex];
	if (!R)
	{
		return;
	}
	if (Button.Command == TEXT("zero_ctrls"))
	{
		ZeroControls();
		return;
	}
	if (!Button.ChannelPath.IsEmpty())
	{
		const int32 Count = Button.Labels.Num();
		auto ValueOf = [&Button, Count](int32 Index) { return Button.bInvert ? Count - 1 - Index : Index; };
		const int32 Here = FMath::Max(Button.Labels.IndexOfByKey(Picker.ValueLabel(Button)), 0);
		if (R->IsLimbSwitch(Button.ChannelPath))
		{
			SwitchLimbMatched(Button.ChannelPath);
		}
		else if (Count > 2 && Button.SwitchedControl != INDEX_NONE)
		{
			OpenSpaceMenu(Button.SwitchedControl, Button.ChannelPath, Button.Labels, Button.bInvert);
		}
		else if (Count > 0 && Button.SwitchedControl != INDEX_NONE)
		{
			SwitchSpaceMatched(Button.SwitchedControl, ValueOf((Here + 1) % Count));
		}
		else
		{
			// A plain dial or toggle: its next value.
			PushUndo();
			const FPose Before = CapturePose(Binding, FString());
			const int32 K = R->FindChannelIndex(Button.ChannelPath);
			R->SetChannelValue(K, Count > 0 ? double(ValueOf((Here + 1) % Count)) : (R->GetChannelValue(K) > 0.5 ? 0.0 : 1.0));
			bDirty = true;
			AutoKey(Before);
		}
		return;
	}
	TArray<int32> Wanted = Button.Targets;
	if (bAlt && !Button.Mirror.IsEmpty())
	{
		for (const FPickerButton& Other : Picker.Panels[PanelIndex].Buttons)
		{
			if (Other.Id == Button.Mirror && Other.bLive)
			{
				for (const int32 T : Other.Targets)
				{
					Wanted.AddUnique(T);
				}
			}
		}
	}
	Select(Wanted, bShift ? 1 : bCtrl ? 2 : 0);
}

void ARigExecStudioHUD::DrawChannels(float X, float Y, float W, float H)
{
	URigExecComponent* R = Rig();
	DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.7f), X - 4.0f * S, Y, W + 8.0f * S, H);
	const int32 Lead = Selected.Num() > 0 ? Selected.Last() : INDEX_NONE;
	if (!R || Lead == INDEX_NONE)
	{
		Label(TEXT("CHANNELS"), X, Y + 12.0f * S, 12.0f, GCyan);
		Label(TEXT("Pick a control: hover the skin and click,"), X, Y + 40.0f * S, 11.0f, GDim);
		Label(TEXT("or click a button in the picker above."), X, Y + 58.0f * S, 11.0f, GDim);
		Label(TEXT("Drag on the skin to pose it."), X, Y + 76.0f * S, 11.0f, GDim);
		return;
	}
	const FString& Path = R->GetControlPath(Lead);
	const int32 At = FMath::RoundToInt(Frame);
	const FTrack* Track = Edit.Controls.Find(Path);
	const bool bKeyed = Track && Track->FindKey(At) != INDEX_NONE;
	Label(ShortName(Path) + (Selected.Num() > 1 ? FString::Printf(TEXT("  (+%d)"), Selected.Num() - 1) : FString()), X,
	      Y + 12.0f * S, 13.0f, FLinearColor::White);
	DrawDiamond(Canvas, X + W - 70.0f * S, Y + 12.0f * S, 5.0f * S, bKeyed ? GKey : Track ? GDim : FLinearColor(0.3f, 0.3f, 0.3f, 0.6f));
	Label(bKeyed ? TEXT("keyed") : Track ? TEXT("animated") : TEXT("no keys"), X + W - 60.0f * S, Y + 12.0f * S, 10.0f,
	      bKeyed ? GKey : GDim);

	double V[9];
	R->GetControlAvars(Lead, V);
	const uint16 Mask = R->GetControlAvarMask(Lead);
	float RowY = Y + 28.0f * S;
	const float RowH = 21.0f * S, CellW = (W - 30.0f * S) / 3.0f;
	const TCHAR* Groups[3] = {TEXT("T"), TEXT("R"), TEXT("S")};
	for (int32 G = 0; G < 3; ++G)
	{
		if ((Mask & (0x7 << (G * 3))) == 0)
		{
			continue;
		}
		Label(Groups[G], X, RowY + RowH * 0.5f, 12.0f, GCyan);
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 A = G * 3 + K;
			const float CX = X + 24.0f * S + K * CellW;
			const bool bOn = (Mask & (1 << A)) != 0;
			DemoTargets.Add(FString(TEXT("avar:")) + GAxisNames[A], FBox2D(FVector2D(CX, RowY), FVector2D(CX + CellW - 4.0f * S, RowY + RowH - 3.0f * S)));
			const bool bHover = bOn && Inside(CX, RowY, CellW - 4.0f * S, RowH - 3.0f * S);
			const bool bDragging = Drag.Kind == EDrag::Avar && Drag.Control == Lead && Drag.Index == A;
			DrawRect(bDragging ? GAccent : bHover ? FLinearColor(0.18f, 0.26f, 0.36f, 1.0f) : FLinearColor(0.08f, 0.1f, 0.14f, 1.0f),
			         CX, RowY, CellW - 4.0f * S, RowH - 3.0f * S);
			Label(GAxisNames[A], CX + 5.0f * S, RowY + RowH * 0.5f - 1.0f * S, 9.5f, GDim);
			Label(bOn ? (G == 1 ? FString::Printf(TEXT("%.1f"), V[A]) : FString::Printf(TEXT("%.2f"), V[A])) : FString(TEXT("-")), CX + CellW - 9.0f * S,
			      RowY + RowH * 0.5f - 1.0f * S, 11.0f, bOn ? FLinearColor::White : GDim, 1);
			if (bHover && bPress && !bTaken)
			{
				// Drag across to change it; Shift for fine steps.
				bTaken = true;
				Drag = FDrag();
				Drag.Kind = EDrag::Avar;
				Drag.Start = Mouse;
				Drag.Control = Lead;
				Drag.Index = A;
				Drag.StartValue = V[A];
				Drag.Before = CapturePose(Binding, FString());
				PushUndo();
			}
			if (bHover && bRightPress && !bTaken)
			{
				// Right-click: back to its rest value.
				bTaken = true;
				PushUndo();
				const FPose Before = CapturePose(Binding, FString());
				double Rest[9];
				R->GetControlAvars(Lead, Rest, true);
				V[A] = Rest[A];
				R->SetControlAvars(Lead, V);
				bDirty = true;
				AutoKey(Before);
			}
		}
		RowY += RowH;
	}
	// The limb it is on: which half drives, and a matched switch.
	const FName Element = R->GetControlElement(Lead);
	const FString Switch = R->FindLimbSwitchFor(Element);
	if (!Switch.IsEmpty())
	{
		const bool bIk = R->IsLimbIk(Switch);
		Label(FString::Printf(TEXT("Limb  %s"), bIk ? TEXT("IK") : TEXT("FK")), X, RowY + RowH * 0.5f, 12.0f, GText);
		if (UiButton(bIk ? TEXT("Switch to FK (matched)") : TEXT("Switch to IK (matched)"), X + 90.0f * S, RowY, W - 90.0f * S,
		             RowH - 3.0f * S))
		{
			SwitchLimbMatched(Switch);
		}
		RowY += RowH;
	}
	// Its space, if it has a choice of them.
	FName SpaceChannel;
	TArray<FString> Labels;
	if (R->FindSpaceChannel(Element, SpaceChannel, Labels))
	{
		const FString SpaceAt = Path + TEXT(".avars:space");
		const int32 K = R->FindChannelIndex(SpaceAt);
		const int32 Now = FMath::RoundToInt(R->GetChannelValue(K));
		Label(TEXT("Space"), X, RowY + RowH * 0.5f, 12.0f, GText);
		DemoTargets.Add(TEXT("space"), FBox2D(FVector2D(X + 90.0f * S, RowY), FVector2D(X + W, RowY + RowH - 3.0f * S)));
		if (UiButton(FString::Printf(TEXT("%s  v"), Labels.IsValidIndex(Now) ? *Labels[Now] : TEXT("?")), X + 90.0f * S, RowY,
		             W - 90.0f * S, RowH - 3.0f * S))
		{
			OpenSpaceMenu(Lead, SpaceAt, Labels, false);
		}
		RowY += RowH;
	}
	// Its other channels: drag a number, click a choice.
	for (const TPair<FString, int32>& Channel : Binding.ChannelOf)
	{
		if (RowY + RowH > Y + H)
		{
			break;
		}
		if (!Channel.Key.StartsWith(Path + TEXT(".")) || Channel.Key.EndsWith(TEXT("avars:space")) || Channel.Key == Switch)
		{
			continue;
		}
		FString Name = Channel.Key.Mid(Path.Len() + 1);
		Name.RemoveFromStart(TEXT("avars:"));
		const double Value = R->GetChannelValue(Channel.Value);
		const TArray<FString>& Choice = R->GetChannelLabels(Channel.Value);
		Label(Name, X, RowY + RowH * 0.5f, 11.0f, GText);
		const float CX = X + 110.0f * S, CW = W - 110.0f * S;
		const bool bHover = Inside(CX, RowY, CW, RowH - 3.0f * S);
		DrawRect(bHover ? FLinearColor(0.18f, 0.26f, 0.36f, 1.0f) : FLinearColor(0.08f, 0.1f, 0.14f, 1.0f), CX, RowY, CW,
		         RowH - 3.0f * S);
		const int32 Index = FMath::RoundToInt(Value);
		Label(Choice.Num() > 0 ? (Choice.IsValidIndex(Index) ? Choice[Index] : FString(TEXT("?"))) : FString::Printf(TEXT("%.3f"), Value),
		      CX + CW - 8.0f * S, RowY + RowH * 0.5f - 1.0f * S, 11.0f, FLinearColor::White, 1);
		if (bHover && bPress && !bTaken)
		{
			bTaken = true;
			if (Choice.Num() > 0)
			{
				PushUndo();
				const FPose Before = CapturePose(Binding, FString());
				R->SetChannelValue(Channel.Value, double((Index + 1) % Choice.Num()));
				bDirty = true;
				AutoKey(Before);
			}
			else
			{
				Drag = FDrag();
				Drag.Kind = EDrag::Channel;
				Drag.Start = Mouse;
				Drag.Index = Channel.Value;
				Drag.StartValue = Value;
				Drag.Before = CapturePose(Binding, FString());
				PushUndo();
			}
		}
		RowY += RowH;
	}
	if (Mask == 0 && Switch.IsEmpty())
	{
		Label(TEXT("This control has no channels to pose."), X, RowY + RowH * 0.5f, 11.0f, GDim);
	}
}

void ARigExecStudioHUD::DrawTimeline()
{
	Panel(TimelineBox, FString());
	const float Left = 12.0f * S, Top = TimelineBox.Min.Y + 6.0f * S, BH = 24.0f * S;
	float X = Left;
	auto Next = [&](const FString& Text, float Width, bool bOn = false, bool bEnabled = true) {
		const bool bHit = UiButton(Text, X, Top, Width * S, BH, bOn, bEnabled);
		X += Width * S + 4.0f * S;
		return bHit;
	};
	if (Next(TEXT("|<"), 32.0f))
	{
		bPlaying = false;
		SetFrame(0.0f);
	}
	if (Next(TEXT("<"), 30.0f))
	{
		bPlaying = false;
		SetFrame(float(FMath::RoundToInt(Frame) - 1));
	}
	if (Next(bPlaying ? TEXT("Pause") : TEXT("Play"), 58.0f, bPlaying))
	{
		bPlaying = !bPlaying;
		if (bPlaying && Frame >= Edit.Length)
		{
			Frame = 0.0f;
		}
	}
	if (Next(TEXT(">"), 30.0f))
	{
		bPlaying = false;
		SetFrame(float(FMath::RoundToInt(Frame) + 1));
	}
	if (Next(TEXT(">|"), 32.0f))
	{
		bPlaying = false;
		SetFrame(float(Edit.Length));
	}
	Label(FString::Printf(TEXT("%3d / %d"), FMath::RoundToInt(Frame), Edit.Length), X + 6.0f * S, Top + BH * 0.5f, 14.0f, FLinearColor::White);
	X += 84.0f * S;
	if (Next(TEXT("Loop"), 48.0f, bLoop))
	{
		bLoop = !bLoop;
	}
	X += 12.0f * S;
	if (Next(TEXT("Key (S)"), 64.0f))
	{
		KeySelection(false);
	}
	if (Next(TEXT("Key All"), 64.0f))
	{
		KeySelection(true);
	}
	if (Next(TEXT("Delete Key"), 84.0f))
	{
		DeleteKeys();
	}
	if (Next(TEXT("Auto Key (K)"), 96.0f, bAutoKey))
	{
		bAutoKey = !bAutoKey;
	}
	X += 12.0f * S;
	Label(TEXT("Length"), X, Top + BH * 0.5f, 12.0f, GText);
	X += 48.0f * S;
	if (Next(TEXT("-"), 24.0f, false, Edit.Length > 6))
	{
		PushUndo();
		Edit.Length = FMath::Max(6, Edit.Length - 6);
		SetFrame(Frame);
	}
	Label(FString::FromInt(Edit.Length), X + 14.0f * S, Top + BH * 0.5f, 12.0f, FLinearColor::White, 0);
	X += 30.0f * S;
	if (Next(TEXT("+"), 24.0f))
	{
		PushUndo();
		Edit.Length += 6;
	}
	X += 12.0f * S;
	if (Next(TEXT("New"), 46.0f))
	{
		PushUndo();
		Edit = FClip();
		Edit.Name = TEXT("Untitled");
		bEditChanged = false;
		SetFrame(0.0f);
		Toast(TEXT("A new, empty animation: pose, then key (S)"));
	}
	Label(FString::Printf(TEXT("%s%s   %d keyed"), *Edit.Name, bEditChanged ? TEXT(" *") : TEXT(""), Edit.Controls.Num()), X + 6.0f * S,
	      Top + BH * 0.5f, 12.0f, GKey);

	// The tracks: a ruler, every key, and the selection's keys.
	const float X0 = 100.0f * S, X1 = TimelineBox.Max.X - 20.0f * S;
	const float RulerY = Top + BH + 8.0f * S, RulerH = 20.0f * S, RowH = 20.0f * S;
	const float AllY = RulerY + RulerH + 2.0f * S, SelY = AllY + RowH + 2.0f * S;
	const int32 Length = FMath::Max(Edit.Length, 1);
	auto XOf = [&](float F) { return X0 + (X1 - X0) * F / float(Length); };
	auto FrameAt = [&](float At) { return FMath::Clamp(FMath::RoundToInt((At - X0) / (X1 - X0) * Length), 0, Length); };
	DrawRect(FLinearColor(0.06f, 0.08f, 0.11f, 1.0f), X0, RulerY, X1 - X0, RulerH);
	DemoTargets.Add(TEXT("ruler"), FBox2D(FVector2D(X0, RulerY), FVector2D(X1, RulerY + RulerH)));
	DrawRect(FLinearColor(0.04f, 0.05f, 0.08f, 1.0f), X0, AllY, X1 - X0, RowH);
	DrawRect(FLinearColor(0.04f, 0.05f, 0.08f, 1.0f), X0, SelY, X1 - X0, RowH);
	Label(TEXT("frames"), Left, RulerY + RulerH * 0.5f, 11.0f, GDim);
	Label(TEXT("all keys"), Left, AllY + RowH * 0.5f, 11.0f, GDim);
	Label(Selected.Num() > 0 ? TEXT("selected") : TEXT("(select)"), Left, SelY + RowH * 0.5f, 11.0f, GDim);
	const float Spacing = (X1 - X0) / float(Length);
	const int32 LabelEvery = Spacing * 10.0f >= 34.0f * S ? 10 : Spacing * 20.0f >= 34.0f * S ? 20 : 30;
	for (int32 F = 0; F <= Length; ++F)
	{
		const bool bMajor = F % LabelEvery == 0;
		if (!bMajor && Spacing < 4.0f * S && F % 5 != 0)
		{
			continue;
		}
		const float TX = XOf(float(F));
		DrawRect(bMajor ? GDim : FLinearColor(0.3f, 0.34f, 0.4f, 1.0f), TX, RulerY + (bMajor ? 0.0f : RulerH * 0.55f), 1.0f,
		         bMajor ? RulerH : RulerH * 0.45f);
		if (bMajor)
		{
			Label(FString::FromInt(F), TX + 3.0f * S, RulerY + 7.0f * S, 10.0f, GText);
		}
	}
	const TSet<FString> Mine = SelectedPaths(true);
	const TArray<int32> AllKeys = Edit.KeyFrames();
	const TArray<int32> SelKeys = Selected.Num() > 0 ? Edit.KeyFrames(&Mine) : TArray<int32>();
	bool bOnKey = false;
	auto Keys = [&](const TArray<int32>& Frames, float RowY, bool bSelection) {
		for (const int32 F : Frames)
		{
			const float KX = XOf(float(F));
			const bool bHover = Inside(KX - 6.0f * S, RowY, 12.0f * S, RowH);
			const bool bMoving = Drag.Kind == EDrag::Keys && Drag.KeyFrom == F && Drag.bKeysOfSelection == bSelection;
			DrawDiamond(Canvas, bMoving ? XOf(float(Drag.KeyAt)) : KX, RowY + RowH * 0.5f, (bHover ? 6.5f : 5.0f) * S,
			            bSelection ? GKey : FLinearColor(0.8f, 0.85f, 0.92f, 0.9f));
			if (bHover && bPress && !bTaken)
			{
				// A key: jump to it, and drag it to move it.
				bTaken = true;
				bOnKey = true;
				bPlaying = false;
				SetFrame(float(F));
				Drag = FDrag();
				Drag.Kind = EDrag::Keys;
				Drag.Start = Mouse;
				Drag.KeyFrom = Drag.KeyAt = F;
				Drag.bKeysOfSelection = bSelection;
			}
		}
	};
	Keys(AllKeys, AllY, false);
	Keys(SelKeys, SelY, true);
	if (Drag.Kind == EDrag::Keys)
	{
		Drag.KeyAt = FrameAt(float(Mouse.X));
		if (Drag.KeyAt != Drag.KeyFrom)
		{
			Drag.bMoved = true;
		}
		Label(FString::Printf(TEXT("move to %d"), Drag.KeyAt), XOf(float(Drag.KeyAt)) + 8.0f * S, AllY - 2.0f * S, 10.0f, GKey);
	}
	// Scrubbing: press anywhere on the tracks.
	if (!bOnKey && Inside(X0, RulerY, X1 - X0, SelY + RowH - RulerY) && bPress && !bTaken)
	{
		bTaken = true;
		bPlaying = false;
		Drag = FDrag();
		Drag.Kind = EDrag::Scrub;
		Drag.Start = Mouse;
		Drag.bMoved = true;
	}
	if (Drag.Kind == EDrag::Scrub)
	{
		SetFrame(float(FrameAt(float(Mouse.X))));
	}
	// The playhead.
	const float HX = XOf(Frame);
	DrawRect(GHead, HX - 1.0f * S, RulerY, 2.0f * S, SelY + RowH - RulerY);
	DrawRect(GHead, HX - 14.0f * S, RulerY - 14.0f * S, 28.0f * S, 13.0f * S);
	Label(FString::FromInt(FMath::RoundToInt(Frame)), HX, RulerY - 7.5f * S, 10.0f, FLinearColor::White, 0);
}

void ARigExecStudioHUD::DrawViewportMarks()
{
	URigExecComponent* R = Rig();
	if (!R || !PlayerOwner)
	{
		return;
	}
	const FTransform Component = R->GetComponentTransform();
	// The controls no touch region names: a dot on each to click.
	if (!bPlaying)
	{
		for (const int32 C : Untouched)
		{
			FVector2D At;
			if (ToScreen(Component.TransformPosition(R->GetRigFrame(R->GetControlPath(C)).GetLocation()), At))
			{
				const bool bHover = HoverControl == C && HoverRegion < 0;
				const float Size = (bHover ? 7.0f : 3.5f) * S;
				DrawRect(bHover ? GKey : FLinearColor(0.5f, 0.8f, 1.0f, 0.4f), At.X - Size * 0.5f, At.Y - Size * 0.5f, Size, Size);
			}
		}
	}
	// The selection's pivots; the lead's axes.
	for (int32 K = 0; K < Selected.Num(); ++K)
	{
		const int32 C = Selected[K];
		const bool bLead = K == Selected.Num() - 1;
		const FTransform Posed = R->GetRigFrame(R->GetControlPath(C));
		const FVector Pivot = Component.TransformPosition(Posed.GetLocation());
		FVector2D At;
		if (!ToScreen(Pivot, At))
		{
			continue;
		}
		if (bLead && !bPlaying && PoseMode != EPoseMode::Auto)
		{
			DrawGizmo();
		}
		else if (bLead)
		{
			// Auto (TouchPose): the control's axes, small, out of the way.
			const float Length = CamDist * 0.05f;
			const FLinearColor Axes[3] = {FLinearColor(1.0f, 0.25f, 0.2f), FLinearColor(0.3f, 1.0f, 0.3f), FLinearColor(0.3f, 0.5f, 1.0f)};
			for (int32 A = 0; A < 3; ++A)
			{
				const FVector Tip = Pivot + Component.TransformVectorNoScale(Posed.GetUnitAxis(EAxis::Type(A + 1))) * Length;
				FVector2D TipAt;
				if (ToScreen(Tip, TipAt))
				{
					DrawLine(At.X, At.Y, TipAt.X, TipAt.Y, Axes[A], 2.0f * S);
				}
			}
		}
		const float Size = (bLead ? 7.0f : 5.0f) * S;
		DrawRect(bLead ? GKey : GCyan, At.X - Size * 0.5f, At.Y - Size * 0.5f, Size, Size);
	}
	// Which control the cursor is on, and what a drag does.
	if (Drag.Kind == EDrag::Pose && Drag.bMoved)
	{
		Label(Drag.Index == 11 ? TEXT("scale") : Drag.bTranslate ? TEXT("move") : bAlt ? TEXT("twist") : TEXT("rotate"), Mouse.X + 16.0f * S,
		      Mouse.Y + 14.0f * S, 12.0f, GKey);
	}
	else if (Drag.Kind == EDrag::Grab)
	{
		Label(TEXT("holding"), Mouse.X + 16.0f * S, Mouse.Y + 14.0f * S, 12.0f, GKey);
	}
	else if (Drag.Kind == EDrag::Gizmo && Drag.bMoved)
	{
		const TCHAR* What = Drag.Index <= 3 ? TEXT("move") : Drag.Index <= 7 ? TEXT("rotate") : TEXT("scale");
		Label(What, Mouse.X + 16.0f * S, Mouse.Y + 14.0f * S, 12.0f, GKey);
	}
	else if (HoverControl != INDEX_NONE)
	{
		Label(ShortName(R->GetControlPath(HoverControl)), Mouse.X + 16.0f * S, Mouse.Y + 14.0f * S, 12.0f, FLinearColor::White);
	}
}

void ARigExecStudioHUD::DrawBaking(float W, float H)
{
	const float BW = 460.0f * S, BH = 110.0f * S, X = (W - BW) * 0.5f, Y = H * 0.36f;
	DrawRect(FLinearColor(0.02f, 0.04f, 0.07f, 0.94f), X, Y, BW, BH);
	DrawRect(GCyan, X, Y, BW, 2.0f * S);
	Label(TEXT("Baking the default animations"), X + BW * 0.5f, Y + 22.0f * S, 16.0f, FLinearColor::White, 0);
	Label(FString::Printf(TEXT("the Mannequin's %s, retargeted onto %s"), *Baker.Describe(), *CharacterName()), X + BW * 0.5f,
	      Y + 46.0f * S, 12.0f, GDim, 0);
	DrawRect(FLinearColor(0.1f, 0.12f, 0.16f, 1.0f), X + 20.0f * S, Y + 66.0f * S, BW - 40.0f * S, 12.0f * S);
	DrawRect(GAccent, X + 20.0f * S, Y + 66.0f * S, (BW - 40.0f * S) * Baker.Progress(), 12.0f * S);
	Label(TEXT("once per character: they are kept for next time"), X + BW * 0.5f, Y + 94.0f * S, 10.5f, GDim, 0);
}

void ARigExecStudioHUD::TypeName()
{
	static const TCHAR* Digits[10] = {TEXT("Zero"), TEXT("One"), TEXT("Two"), TEXT("Three"), TEXT("Four"),
	                                  TEXT("Five"), TEXT("Six"), TEXT("Seven"), TEXT("Eight"), TEXT("Nine")};
	for (TCHAR C = TEXT('A'); C <= TEXT('Z'); ++C)
	{
		if (Pressed(FKey(FName(*FString::Chr(C)))))
		{
			NameText.AppendChar(bShift ? C : FChar::ToLower(C));
		}
	}
	for (int32 D = 0; D < 10; ++D)
	{
		if (Pressed(FKey(FName(Digits[D]))) || Pressed(FKey(FName(*FString::Printf(TEXT("NumPad%s"), Digits[D])))))
		{
			NameText.AppendChar(TCHAR(TEXT('0') + D));
		}
	}
	if (Pressed(EKeys::SpaceBar))
	{
		NameText.AppendChar(TEXT(' '));
	}
	if (Pressed(EKeys::Hyphen) || Pressed(EKeys::Subtract))
	{
		NameText.AppendChar(bShift ? TEXT('_') : TEXT('-'));
	}
	if (Pressed(EKeys::BackSpace) && NameText.Len() > 0)
	{
		NameText.LeftChopInline(1);
	}
	NameText = NameText.Left(40);
	if (Pressed(EKeys::Enter) || Pressed(EKeys::Gamepad_FaceButton_Bottom))
	{
		const FString Name = NameText.TrimStartAndEnd();
		if (!Name.IsEmpty())
		{
			bNaming = false;
			bNamingPose ? SavePose(Name) : SaveClip(Name);
		}
	}
}

void ARigExecStudioHUD::DrawNaming(float W, float H)
{
	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.45f), 0.0f, 0.0f, W, H);
	const float BW = 440.0f * S, BH = 150.0f * S, X = (W - BW) * 0.5f, Y = H * 0.34f;
	DrawRect(FLinearColor(0.02f, 0.04f, 0.07f, 0.97f), X, Y, BW, BH);
	DrawRect(GCyan, X, Y, BW, 2.0f * S);
	Label(bNamingPose ? TEXT("Save this pose as") : TEXT("Save this animation as"), X + 20.0f * S, Y + 22.0f * S, 15.0f,
	      FLinearColor::White);
	DrawRect(FLinearColor(0.08f, 0.1f, 0.14f, 1.0f), X + 20.0f * S, Y + 42.0f * S, BW - 40.0f * S, 34.0f * S);
	const bool bCaret = FMath::Fmod(FPlatformTime::Seconds(), 1.0) < 0.55;
	Label(NameText + (bCaret ? TEXT("|") : TEXT(" ")), X + 30.0f * S, Y + 59.0f * S, 15.0f, GKey);
	Label(TEXT("Type a name.   Enter saves, Esc cancels."), X + 20.0f * S, Y + 92.0f * S, 11.0f, GDim);
	// The buttons take clicks even though the panels behind do not.
	bTaken = false;
	if (UiButton(TEXT("Save"), X + BW - 200.0f * S, Y + BH - 38.0f * S, 86.0f * S, 26.0f * S, true, !NameText.TrimStartAndEnd().IsEmpty()))
	{
		bNaming = false;
		bNamingPose ? SavePose(NameText.TrimStartAndEnd()) : SaveClip(NameText.TrimStartAndEnd());
	}
	if (UiButton(TEXT("Cancel"), X + BW - 106.0f * S, Y + BH - 38.0f * S, 86.0f * S, 26.0f * S))
	{
		bNaming = false;
	}
	bTaken = true;
}

void ARigExecStudioHUD::OpenSpaceMenu(int32 Control, const FString& Path, const TArray<FString>& Labels, bool bInvert)
{
	bSpaceMenu = true;
	SpaceMenuAt = Mouse;
	SpaceControl = Control;
	SpacePath = Path;
	SpaceLabels = Labels;
	bSpaceInvert = bInvert;
}

void ARigExecStudioHUD::DrawSpaceMenu(bool bDraw)
{
	URigExecComponent* R = Rig();
	if (!R || !Canvas)
	{
		bSpaceMenu = false;
		return;
	}
	const float RowH = 24.0f * S, MW = 200.0f * S;
	const float MH = RowH * (SpaceLabels.Num() + 1);
	const float X = FMath::Min(float(SpaceMenuAt.X), Canvas->ClipX - MW - 4.0f), Y = FMath::Min(float(SpaceMenuAt.Y), Canvas->ClipY - MH - 4.0f);
	const int32 Count = SpaceLabels.Num();
	const int32 Raw = FMath::RoundToInt(R->GetChannelValue(R->FindChannelIndex(SpacePath)));
	const int32 Now = bSpaceInvert ? Count - 1 - Raw : Raw;
	// Twice a frame: first for the press (before anything under it), then drawn on top.
	if (!bDraw)
	{
		if (bPress && !bTaken)
		{
			bTaken = true;
			for (int32 I = 0; I < Count; ++I)
			{
				if (Inside(X, Y + RowH * (I + 1), MW, RowH))
				{
					bSpaceMenu = false;
					SwitchSpaceMatched(SpaceControl, bSpaceInvert ? Count - 1 - I : I);
					return;
				}
			}
			bSpaceMenu = false;
		}
		return;
	}
	DrawRect(FLinearColor(0.02f, 0.04f, 0.07f, 0.97f), X, Y, MW, MH);
	DrawRect(GCyan, X, Y, MW, 2.0f * S);
	Label(TEXT("Space (keeps it in place)"), X + 8.0f * S, Y + RowH * 0.5f, 11.0f, GCyan);
	for (int32 I = 0; I < Count; ++I)
	{
		const float RY = Y + RowH * (I + 1);
		DemoTargets.Add(TEXT("spaceitem:") + SpaceLabels[I], FBox2D(FVector2D(X, RY), FVector2D(X + MW, RY + RowH)));
		if (Inside(X, RY, MW, RowH) || I == Now)
		{
			DrawRect(I == Now ? GAccent : FLinearColor(0.2f, 0.3f, 0.42f, 0.95f), X, RY, MW, RowH);
		}
		Label(SpaceLabels[I], X + 12.0f * S, RY + RowH * 0.5f, 12.0f, FLinearColor::White);
	}
}

void ARigExecStudioHUD::ProfileTick(float DeltaSeconds)
{
	// Idle 4 s, then the Walk clip playing 4 s, then quit; a line a second.
	ProfileTime += DeltaSeconds;
	++ProfileFrames;
	static float NextLine = 1.0f;
	if (ProfileTime > 4.0f && !bPlaying && Edit.IsEmpty())
	{
		const int32 Walk = Clips.IndexOfByPredicate([](const FClip& C) { return C.Name == TEXT("Walk"); });
		UseClip(Walk, false);
		bPlaying = true;
	}
	if (ProfileTime < NextLine)
	{
		return;
	}
	NextLine += 1.0f;
	const double Per = 1000.0 / FMath::Max(ProfileFrames, 1);
	URigExecComponent* R = Rig();
	UE_LOG(LogRigExecStudioHud, Log,
	       TEXT("profile %s %s: %d fps | game %.1f render %.1f gpu %.1f ms | rig %.2f (execute %.2f publish %.2f) picker %.2f marks %.2f timeline %.2f panels %.2f hover %.2f ms/frame"),
	       *CharacterName(), bPlaying ? TEXT("playing") : TEXT("idle"), ProfileFrames, FPlatformTime::ToMilliseconds(GGameThreadTime),
	       FPlatformTime::ToMilliseconds(GRenderThreadTime), FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)), ProfileSpent[0] * Per,
	       R ? R->LastExecuteMs : 0.0f, R ? R->LastPublishMs : 0.0f, ProfileSpent[1] * Per, ProfileSpent[2] * Per, ProfileSpent[3] * Per,
	       ProfileSpent[4] * Per, ProfileSpent[5] * Per);
	ProfileFrames = 0;
	FMemory::Memzero(ProfileSpent, sizeof(ProfileSpent));
	if (ProfileTime > 9.0f)
	{
		UKismetSystemLibrary::QuitGame(this, PlayerOwner, EQuitPreference::Quit, false);
	}
}
