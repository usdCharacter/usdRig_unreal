// The animation studio's data: keyframed clips and poses on a RigExec rig's
// own avars, how a clip is sampled between its keys, saving and loading them
// as JSON, and baking clips from Mannequin animation through the retargeter.
//
// Everything is keyed by rig path (a control's prim path, a channel's
// property path), so a clip or pose is plain data a person can save and
// apply to the character again later. A control key holds its nine
// transform avars (tx ty tz rx ry rz sx sy sz, raw values); a channel key one
// value. Keys sit on whole frames at 30 fps.
#pragma once

#include "CoreMinimal.h"

class ARigExecBipedCharacter;
class URigExecComponent;

namespace RigExecStudio
{
/** Frames per second every clip is keyed and played at. */
constexpr float GFps = 30.0f;

/** One control's or channel's keys, sorted by frame. */
struct FTrack
{
	/** Values per key: 9 for a control, 1 for a channel. */
	int32 Dim = 9;
	/** Holds each key until the next (a space or a switch). */
	bool bStepped = false;
	TArray<int32> Frames;
	/** Dim values per key, in key order. */
	TArray<double> Values;

	int32 Num() const { return Frames.Num(); }
	const double* KeyValues(int32 Key) const { return Values.GetData() + Key * Dim; }
	int32 FindKey(int32 Frame) const { return Frames.IndexOfByKey(Frame); }
	/** Adds or replaces the key on `Frame`. */
	void SetKey(int32 Frame, const double* V);
	bool RemoveKey(int32 Frame);
	/** The value at a (fractional) frame: a smooth curve through the keys
	 * that never overshoots them, flat before the first and after the last. */
	void Sample(float Frame, double* Out) const;
};

struct FClip
{
	FString Name;
	/** A default the studio baked: it can be used, not overwritten. */
	bool bBuiltIn = false;
	/** The last frame (the first is 0). */
	int32 Length = 72;
	TMap<FString, FTrack> Controls;
	TMap<FString, FTrack> Channels;

	bool IsEmpty() const { return Controls.Num() == 0 && Channels.Num() == 0; }
	/** Every frame any key is on (of only `Only`'s tracks when given), sorted. */
	TArray<int32> KeyFrames(const TSet<FString>* Only = nullptr) const;
	/** Moves the keys on `From` to `To` (of only `Only`'s tracks when given). */
	void MoveKeys(int32 From, int32 To, const TSet<FString>* Only = nullptr);
	/** Removes the keys on `Frame`; returns how many went. */
	int32 RemoveKeys(int32 Frame, const TSet<FString>* Only = nullptr);
	/** Copies `Other`'s keys in, `Offset` frames later, over any on the same frames. */
	void Merge(const FClip& Other, int32 Offset);
};

struct FPose
{
	FString Name;
	bool bBuiltIn = false;
	/** Nine avars per control path. */
	TMap<FString, TArray<double>> Controls;
	TMap<FString, double> Channels;
};

/** A character's controls and channels by path, looked up once. */
struct FBinding
{
	TWeakObjectPtr<URigExecComponent> Rig;
	TMap<FString, int32> ControlOf;
	TMap<FString, int32> ChannelOf;
	/** Channels holding a choice (a space, a switch): stepped, never blended. */
	TSet<FString> Choices;

	void Bind(URigExecComponent* InRig);
	bool IsBound(const URigExecComponent* InRig) const { return Rig.Get() == InRig && InRig != nullptr; }
};

/** Poses the rig with the clip at `Frame`; only keyed controls and channels move. */
void ApplyClip(const FClip& Clip, float Frame, const FBinding& Binding);
/** Every control's avars and every channel's value now. */
FPose CapturePose(const FBinding& Binding, const FString& Name);
/** Poses the rig toward `Pose` by `Amount` (1: all the way); choices
 * switch at half way. */
void ApplyPose(const FPose& Pose, float Amount, const FBinding& Binding);
/** Keys every control and channel in `Pose` on `Frame`. */
void KeyPose(FClip& Clip, int32 Frame, const FPose& Pose, const FBinding& Binding, const TSet<FString>* Only = nullptr);
/** The controls and channels whose values differ between two poses. */
TSet<FString> Changed(const FPose& Before, const FPose& After);

FString ClipToJson(const FClip& Clip);
bool ClipFromJson(const FString& Text, FClip& Out);
FString PoseToJson(const FPose& Pose);
bool PoseFromJson(const FString& Text, FPose& Out);

/** Where a character's library lives: Saved/RigExecStudio/<Character>/. */
FString LibraryDir(const FString& Character);
/** A name safe as a file name. */
FString FileNameFor(const FString& Name);

/** One default clip to bake: a Mannequin animation and the poses taken from it. */
struct FBakeJob
{
	FString Name;
	FString Animation;
	/** Poses captured on the way: their name and where (0-1 through the clip). */
	TArray<TPair<FString, float>> Poses;
};

/** The default clips, baked from the Mannequin's animation through the
 * character's retargeter a few samples a frame, so the game keeps drawing
 * while it works. */
class FBaker
{
public:
	/** The clips every character gets, and the version that names the bake
	 * (a change to the jobs or the sampling bakes again). */
	static TArray<FBakeJob> DefaultJobs();
	static constexpr int32 Version = 2;
	/** Keep every Nth sample as a key: the clip still plays like the
	 * animation, and its keys are few enough to read on the timeline. */
	static constexpr int32 KeyEvery = 3;

	void Start(ARigExecBipedCharacter* Hero, const FBinding& Binding);
	/** Bakes up to `Samples` samples; true once done (or failed). */
	bool Step(int32 Samples);
	bool IsRunning() const { return Hero.IsValid() && Job < Jobs.Num(); }
	/** Stops a bake part way, the character put back as it was. */
	void Cancel();
	float Progress() const;
	FString Describe() const;

	TArray<FClip> Clips;
	TArray<FPose> Poses;
	FString Error;

private:
	bool BeginJob();
	void FinishJob();
	void Restore();

	TWeakObjectPtr<ARigExecBipedCharacter> Hero;
	FBinding Binding;
	TArray<FBakeJob> Jobs;
	int32 Job = 0;
	int32 Sample = 0;
	int32 SampleCount = 0;
	TWeakObjectPtr<class UAnimSequenceBase> Sequence;
	/** Every sample's pose for the job in hand. */
	TArray<FPose> Samples;
	FPose Rest;
	bool bWasAsync = true;
	int32 TotalSamples = 0, DoneSamples = 0;
};
} // namespace RigExecStudio
