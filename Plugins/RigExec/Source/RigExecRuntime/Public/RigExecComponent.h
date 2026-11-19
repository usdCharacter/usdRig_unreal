// A RigExec rig in an Unreal scene: the binary runtime posing the rig, and
// one dynamic mesh per deformed mesh drawing what it computed.
#pragma once

#include "Components/SceneComponent.h"
#include "CoreMinimal.h"
#include <string>
#include "Engine/EngineTypes.h"

#include "RigExecCharacter.h"
#include "RigExecComponent.generated.h"

class UControlRigComponent;
class UMaterialInterface;
class UDynamicMeshComponent;
class URigExecSkinComponent;
class URigExecCharacter;
struct FRigExecMeshSet;

namespace rigExec
{
class RigExecRuntimeReader;
}


UCLASS(ClassGroup = (Animation), meta = (BlueprintSpawnableComponent))
class RIGEXECRUNTIME_API URigExecComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	/** A component fell back to drawing on the CPU, and why (the editor shows
	 * a toast). Not fired when Draw On GPU is simply off. */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnRenderFallback, URigExecComponent*, const FString&);
	static FOnRenderFallback OnRenderFallback;

	URigExecComponent();
	virtual ~URigExecComponent() override;

	/** The baked character (URigExecRuntimeLibrary::BakeCharacter): the rig,
	 * its controls and its meshes in one asset, with no USD and no files on
	 * disk -- what a packaged game loads. When set, the file paths below are
	 * ignored. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	TObjectPtr<URigExecCharacter> Character;

	/** The rig: a .rigexec (rigExecBake), every input settable. Relative paths
	 * here and below are relative to the project directory. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FFilePath RigExecFile;

	/** The USD stage the rig was baked from, read once for each mesh's
	 * topology so vertex i is the rig's point i. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FFilePath StageFile;

	/** The parent of each mesh material: every USD material becomes an
	 * instance with its preview-surface colour ("Color" or "BaseColor"),
	 * "Roughness" and "Metallic". The plugin's M_RigExecSurface when empty,
	 * which also blends the vertex colour in by its alpha -- how Touch Pose
	 * tints the skin, drawn directly or through a subdivision surface. A
	 * material of your own shows Touch Pose only if it does the same. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	TObjectPtr<UMaterialInterface> SurfaceMaterial;

	/** export_controls.py's description, with the element names
	 * build_control_rig.py wrote back: how each Control Rig control and
	 * channel maps onto the rig's avars. Optional. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	FFilePath ControlsFile;

	/** The Control Rig whose controls pose the rig each tick. Found on the
	 * owning actor when left empty. */
	UPROPERTY(EditAnywhere, Category = "RigExec")
	TObjectPtr<UControlRigComponent> ControlRig;


	/** Loads the rig and builds the meshes. False with the reason logged. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	bool Load();

	/** Sets one channel by property path ("/.../M_Jaw.avars:rx"); takes
	 * effect at the next Evaluate. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	bool SetChannel(const FString& Channel, float Value);

	/** Drops every channel set, back to the rig's baked pose. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	void ClearChannels();

	/** Runs the rig and moves the meshes. Returns the evaluation time in
	 * milliseconds, negative on failure. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	float Evaluate();

	/** The meshes built by Load, in the rig's mesh order. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	TArray<UDynamicMeshComponent*> GetMeshes() const { return Meshes; }

	/** Vertex `Index` of mesh `Mesh` as the rig last placed it, in Unreal
	 * space -- for tests. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	FVector GetVertex(int32 Mesh, int32 Index) const;

	/** Reads the Control Rig's controls into avars and, when any moved,
	 * evaluates. Returns the number of avars that changed. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	int32 PullControls();

	/** The Control Rig control standing for the rig control at `Path`
	 * ("/Biped/Rig/.../L_Arm"), or None. */
	FName FindControlElement(const FString& Path) const;
	/** The Control Rig control or channel standing for the rig attribute
	 * at `AttrPath` ("/.../L_Arm.avars:ikfk"), or None. */
	FName FindChannelElement(const FString& AttrPath) const;
	/** The picker layout export_picker.py wrote beside the controls file
	 * (Biped_controls.json -> Biped_picker.json), resolved; empty if none. */
	FString PickerFilePath() const;
	/** The picker layout's text: the character asset's, else the file's.
	 * False when there is none. */
	bool GetPickerText(FString& Out) const;

	/** Reads every mesh the rig poses (`PointsProperties`, "<mesh>.points")
	 * off a USD stage: topology, UVs, material looks and touch regions.
	 * Editor (or any build with the USD SDK) only; false with `Error` set
	 * otherwise. */
	static bool ReadStageMeshes(const FString& StagePath, const TArray<FString>& PointsProperties,
	                            FRigExecMeshSet& Out, FString& Error);

	/** Pull the Control Rig's controls into the rig each tick. Off when
	 * something else drives the controls directly (a retargeter, through
	 * SetControlValueMatrix). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	bool bPullControlRig = true;

	/** Keep a limb's idle IK/FK half selectable: its controls stay drawn,
	 * dimmed, on the limb, and selecting one switches the limb to it,
	 * matched. Off hides the idle half. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec")
	bool bShowIdleLimbHalf = true;

	/** Direct control access, for drivers that bypass Control Rig. Controls
	 * are indexed in the controls description's order, parents first;
	 * matrices are USD row-vector, in asset space and rig units. */
	int32 GetControlCount() const { return ControlMaps.Num(); }
	/** The control whose path or Control Rig element name is `NameOrPath`, or -1. */
	int32 FindControlIndex(const FString& NameOrPath) const;
	int32 GetControlParent(int32 Control) const;
	bool IsControlNull(int32 Control) const;
	const FString& GetControlPath(int32 Control) const;
	/** The control's value as last sent (`bDefault`: its rest value). */
	FMatrix GetControlValueMatrix(int32 Control, bool bDefault = false) const;
	/** The value the last landed run evaluated with: what pairs with the
	 * frames GetControlAssetFrame gives (a run off the game thread lands
	 * after newer values were sent). */
	FMatrix GetLandedControlValueMatrix(int32 Control) const;
	/** The control's posed frame from the last evaluation; false when the
	 * rig publishes none for it. */
	bool GetControlAssetFrame(int32 Control, FMatrix& Out) const;
	/** Sends a control's value: its avars, decomposed in its rotation order.
	 * Returns how many avars changed; Evaluate to see them. */
	int32 SetControlValueMatrix(int32 Control, const FMatrix& Value);
	/** Rig units to centimetres. */
	double GetCentimetresPerUnit() const { return CentimetresPerUnit; }
	/** The control's Control Rig element name (its touch regions and picker
	 * buttons name it). */
	FName GetControlElement(int32 Control) const;
	/** The control's nine transform avars (tx ty tz rx ry rz sx sy sz, raw
	 * avar values) as last sent, or its rest values; false for a null. */
	bool GetControlAvars(int32 Control, double Out[9], bool bDefault = false) const;
	/** Which of the nine the control exposes (bit A for avar A). */
	uint16 GetControlAvarMask(int32 Control) const;
	/** Sends raw avar values; the ones the control does not expose are
	 * ignored. Returns how many changed; Evaluate to see them. For drivers
	 * with no Control Rig (a game). */
	int32 SetControlAvars(int32 Control, const double In[9]);

	/** The rig's channels: every non-transform input the controls file
	 * describes (IK/FK dials, spaces, switches), with their values as last
	 * sent. For drivers with no Control Rig. */
	int32 GetChannelCount() const { return ChannelMaps.Num(); }
	int32 FindChannelIndex(const FString& Path) const;
	const FString& GetChannelPath(int32 Channel) const;
	double GetChannelValue(int32 Channel) const;
	double GetChannelDefault(int32 Channel) const;
	/** A dial's value names (a space switch's spaces), or empty. */
	const TArray<FString>& GetChannelLabels(int32 Channel) const;
	bool SetChannelValue(int32 Channel, double Value);

	/** The matrix primvars the last evaluation published -- a surface
	 * projector's frame and its packed shader dials -- as
	 * "<prim>.primvars:<name>" paths. */
	TArray<FString> GetMatrixPrimvarPaths() const;
	/** One of them: USD row-vector, asset space, rig units. False when the
	 * rig publishes no such primvar. */
	bool GetMatrixPrimvar(const FString& Path, FMatrix& Out) const;
	/** An asset-space frame (row vectors) in this component's space and in
	 * centimetres: where the meshes' points are put. */
	FMatrix AssetToComponent(const FMatrix& Asset) const { return ToUnreal(Asset); }
	/** The mesh drawn for the stage prim `PathOrName` ("/Biped/Geom/l_eye_geo"
	 * or "l_eye_geo"), or -1. */
	int32 FindMesh(const FString& PathOrName) const;
	const FString& GetMeshPath(int32 Mesh) const;
	/** Everything drawing mesh `Mesh`: its Dynamic Mesh and, on the GPU path,
	 * its skin. (A subdivision surface draws them too; it is its own
	 * component.) */
	TArray<UPrimitiveComponent*> GetMeshDrawers(int32 Mesh) const;
	/** Puts `Material` on slot `Slot` of mesh `Mesh`, on everything drawing it. */
	void SetMeshMaterial(int32 Mesh, int32 Slot, UMaterialInterface* Material);
	/** Counts Loads: the meshes are new components after each. */
	int32 GetLoadCount() const { return LoadCount; }
	/** The limbs' IK/FK switches and the value each takes for FK. */
	TArray<TPair<FString, double>> GetLimbFkSwitches() const;

	/** Whether `SwitchPath` ("/.../L_Arm.avars:ikfk") is a limb's IK/FK
	 * switch this rig can match. */
	bool IsLimbSwitch(const FString& SwitchPath) const;
	/** Hands the limb switched by `SwitchPath` to its other half without
	 * moving it: the incoming half's channels take the values the rig's
	 * IK/FK match published for the limb's current pose, the Control Rig's
	 * controls follow, then the switch flips -- usdview's ikfkMatch.Plan. Writes through the Control Rig with undo, so
	 * call it inside a transaction. False when there is no such limb. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	bool SwitchLimb(const FString& SwitchPath);
	/** The switch ("/.../L_Arm.avars:ikfk") of the limb the Control Rig
	 * control `Element` belongs to -- the switch's own control, an FK or IK
	 * control, the pole, or anything under the IK control (a foot's roll) --
	 * as usdview's ikfkMatch.LimbFor. Empty when it is on no limb. */
	FString FindLimbSwitchFor(FName Element) const;
	/** Whether the limb switched by `SwitchPath` is on its IK half now. */
	bool IsLimbIk(const FString& SwitchPath) const;
	/** The switch of the limb `Element` belongs to when `Element` is on the
	 * half that limb is NOT driving with (an FK control while it is in IK,
	 * the IK control, pole or foot roll while in FK) -- drawn on the limb,
	 * driving nothing. Selecting one switches the limb to it, matched, as
	 * usdview's _SwitchPresentedLimbs. Empty otherwise, and mid-blend. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	FString FindPresentedLimbFor(FName Element) const;
	/** The space channel on the Control Rig control `Element` (its
	 * avars:space) and the label of each space, or false when it has none
	 * or only one. */
	bool FindSpaceChannel(FName Element, FName& OutChannel, TArray<FString>& OutLabels) const;
	/** Moves `Element` to space `Index` without moving it: the space flips
	 * and the control takes the values the rig published for that space
	 * (`<control>.spaces:matchValues`), as usdview's spaceMatch.Plan.
	 * Writes through the Control Rig with undo, so call it inside a
	 * transaction. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	bool SwitchSpace(FName Element, int32 Index);

	/** The posed frame of the rig control or joint at `Path`, in the
	 * Control Rig's space; identity when the rig has no such slot. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	FTransform GetRigFrame(const FString& Path) const;


	/** The touch regions read from the stage at load: for each mesh, the
	 * first layer of its RigExecTouchRegions, as usdview's TouchPose opens
	 * it. Regions are numbered across the meshes. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	int32 GetTouchRegionCount() const;

	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	FString GetTouchRegionName(int32 Region) const;

	/** The drawn mesh `Region` is painted on, or -1. */
	int32 GetTouchRegionMesh(int32 Region) const;
	/** How many faces `Region` paints. */
	int32 GetTouchRegionFaceCount(int32 Region) const;

	/** The Control Rig control a click in `Region` selects, or None. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	FName GetTouchRegionControl(int32 Region) const;

	/** The touch region a world-space ray first hits on the posed skin, or
	 * -1 when it misses or lands on unpainted skin. `OutDistance` is the
	 * hit's distance along the ray. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	int32 PickTouchRegion(const FVector& RayOrigin, const FVector& RayDirection, double& OutDistance);

	/** The regions whose control is one of `Controls`. */
	TArray<int32> GetTouchRegionsOf(const TArray<FName>& Controls) const;

	/** Lights touch regions on the skin: `Hover` in its palette colour,
	 * `Lead` in the lead colour and `Selected` in the selected colour (-1 /
	 * empty for none), as usdview's TouchPose does. Written into the drawn
	 * meshes' vertex colours (only the regions that changed), so it rides
	 * the skin as the rig poses and reaches any renderer that draws those
	 * meshes -- a subdivision surface included. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	void SetTouchHighlight(int32 Hover, int32 Lead, const TArray<int32>& Selected);

	/** Hides the touch highlight without forgetting it while the rig is
	 * being worked (a drag, playback, an attribute edit); it returns as it
	 * was when resumed. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	void SetTouchSuspended(bool bSuspended);

	UFUNCTION(BlueprintCallable, Category = "RigExec|Touch")
	bool IsTouchSuspended() const;

	/** When the rig last posed (FPlatformTime::Seconds), or 0. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	double GetLastPoseTime() const { return LastPoseTime; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

	/** Sends every control and channel to the rig again, re-poses, re-places
	 * the gizmos and rewrites the Touch Pose colours -- what scrubbing
	 * Sequencer does -- now and again on the next tick. Called after every
	 * editor undo or redo, which can restore state this component never saw
	 * change, before or after Sequencer re-evaluates. */
	UFUNCTION(BlueprintCallable, Category = "RigExec")
	void RequestResync();
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Draw the meshes on the GPU: each pose uploads the posed points once
	 * and the GPU computes the normals and tangents. Where that cannot run
	 * (no compute shaders) or with this off, the meshes draw on the CPU, which
	 * recomputes the normals and re-uploads the mesh every pose. */
	UPROPERTY(EditAnywhere, Category = "RigExec|Render")
	bool bDrawOnGPU = true;

	/** Run the rig on the task system, off the game thread: each run lands
	 * a frame later, when the next input write settles it, so the game
	 * thread does not wait for it. Readers see the last landed run. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec|Performance")
	bool bEvaluateAsync = true;

	/** Evaluate only every Nth request (1 = every frame); the inputs that
	 * arrive in between wait for the next run. A lower rate for a character
	 * seen from afar. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RigExec|Performance", meta = (ClampMin = "1", ClampMax = "8"))
	int32 EvaluateEvery = 1;

	/** The last run's time in the runtime, and in publishing its points. */
	UPROPERTY(VisibleInstanceOnly, Transient, Category = "RigExec|Performance")
	float LastExecuteMs = 0.0f;
	UPROPERTY(VisibleInstanceOnly, Transient, Category = "RigExec|Performance")
	float LastPublishMs = 0.0f;

	/** Waits for a run in flight and publishes it. */
	void Settle();
	/** Publishes a run off the game thread that has already finished, without
	 * waiting; true when one landed. A driver that evaluates only when it
	 * changed something calls this each frame, so its last change shows. */
	bool LandIfDone();
	/** A run is in flight off the game thread. */
	bool IsRunPending() const;

	/** On the GPU, write motion vectors from the pose's movement, so TSR and
	 * motion blur see the character deform (the CPU fallback draws without
	 * them). */
	UPROPERTY(EditAnywhere, Category = "RigExec|Render", meta = (EditCondition = "bDrawOnGPU"))
	bool bMotionBlur = true;

	/** How the meshes are drawn now, and why when it is the CPU fallback. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "RigExec|Render")
	FString RenderPath;

	/** Whether the meshes draw on the GPU right now. */
	UFUNCTION(BlueprintCallable, Category = "RigExec|Render")
	bool IsDrawingOnGPU() const { return bGpu; }
	/** Why the meshes draw on the CPU; empty on the GPU. */
	const FString& GetFallbackReason() const { return FallbackReason; }

private:
	/** Owned; a raw pointer because the generated code cannot see the
	 * reader's definition. */
	rigExec::RigExecRuntimeReader* Reader = nullptr;
	TArray<uint8> Bytes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UDynamicMeshComponent>> Meshes;
	/** Each mesh's position in the reader's point list. */
	TArray<int32> PointSlots;
	/** Each mesh's stage prim. */
	TArray<FString> MeshPaths;
	int32 LoadCount = 0;
	/** The stage's up axis and units, for converting the rig's points. */
	bool bStageZUp = false;
	double StageMetersPerUnit = 0.01;

	struct FControlMap
	{
		FName Element;
		FString Path;
		int32 Parent = -1;
		/** A rig helper drawn as a null: placed, never read. */
		bool bNull = false;
		/** The channel fading this control (a limb's IK/FK dial): the
		 * control is shown only while the fade is above zero. -1: none. */
		int32 FadeChannel = -1;
		bool bFadeInvert = false;
		int8 Shown = -1;
		int32 Order = 0;
		double Sign[3] = {1.0, 1.0, 1.0};
		double UnitScale = 1.0;
		bool Channels[9] = {};
		/** tx ty tz rx ry rz sx sy sz as last sent: the decomposition's
		 * hint and the change test. */
		double Values[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
		double Defaults[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
		/** Values as the run in flight, and the last landed one, saw them. */
		double Launched[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
		double Landed[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
	};
	/** Values -> Launched for every control, as a run starts. */
	void SnapshotLaunched();
	FMatrix ValueMatrix(const FControlMap& Map, const double* V) const;
	struct FChannelMap
	{
		FName Element;
		FString Path;
		double Value = 0.0;
		double Default = 0.0;
		/** A dial's value names (a space switch's spaces), or empty. */
		TArray<FString> Labels;
	};
	TArray<FControlMap> ControlMaps;
	TArray<FChannelMap> ChannelMaps;
	double CentimetresPerUnit = 1.0;
	/** The asset's placement (USD row-vector), from the description. */
	double AssetToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	/** The Control Rig's root null (the description's "root"): every control
	 * keeps its USD offset and value as authored, and this one null carries
	 * the asset from USD's Y-up right-handed space into Unreal's. */
	FName UsdRoot;
	/** The USD-space root's global matrix in the Control Rig (the Y/Z swap
	 * when the hierarchy is not at hand). */
	FMatrix UsdRootGlobal() const;
	/** The global matrix of the parent of control C: its parent control or
	 * null, or the USD-space root. */
	FMatrix ParentGlobal(const class URigHierarchy* Hierarchy, int32 C) const;
	bool LoadControls(const FString& Text, const FString& Source);
	/** Sends one control's value matrix (rig units) as avars. */
	int32 SendControl(FControlMap& Map, const FMatrix& Value, bool bForce);
	/** Builds the dynamic meshes from the stage's data and the rig's points. */
	void BuildMeshes(const FRigExecMeshSet& Set);
	/** A finished run: snapshot what readers ask for, move the meshes. */
	void Publish(bool bExecuted);
	/** An input write: to the reader, or queued while a run is in flight. */
	bool WriteInput(const char* Name, double Value, std::string* Why);
	void FlushInputs();
	TMap<FString, double> QueuedInputs;
	int32 SkippedEvaluations = 0;
	/** A look's colour image: the character's baked texture, else the file
	 * loaded once (a stage read live in the editor). */
	class UTexture* SurfaceTexture(const struct FRigExecSurfaceLook& Look);
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<class UTexture2D>> LiveTextures;
	/** A baked image (an index into the character's textures), else the file. */
	class UTexture* LookTexture(int32 Baked, const FString& File);
	/** Each drawn mesh's looks, by slot, as loaded. */
	TArray<TArray<FRigExecSurfaceLook>> MeshLooks;

public:
	/** Sets a look's shader parameters (images and numbers) on a material. */
	void ApplyLookParameters(class UMaterialInstanceDynamic* Instance, const FRigExecSurfaceLook& Look);
	/** The same for mesh `Mesh`'s slot `Slot`, as the stage bound it: what a
	 * component replacing that slot's material (the eyes) keeps. */
	void ApplyMeshLook(int32 Mesh, int32 Slot, class UMaterialInstanceDynamic* Instance);
	/** Mesh `Mesh`'s slot `Slot` look as loaded, or null. */
	const FRigExecSurfaceLook* GetMeshLook(int32 Mesh, int32 Slot) const
	{
		return MeshLooks.IsValidIndex(Mesh) && MeshLooks[Mesh].IsValidIndex(Slot) ? &MeshLooks[Mesh][Slot] : nullptr;
	}

private:
	/** Where the rig was loaded from, for the log. */
	FString SourceName;
	/** Moves every control's offset so its gizmo sits on the frame the rig
	 * computed: offset = value^-1 * frame * parentFrame^-1. */
	void PlaceControls();
	/** A control's or joint's posed frame, asset space (row vectors). */
	bool AssetFrame(const FString& Path, FMatrix& Out) const;
	/** An asset-space frame as a Control Rig global transform. */
	FMatrix ToUnreal(const FMatrix& Asset) const;
	/** The same, with the USD-space root's global already in hand (read once
	 * per placement rather than once per control). */
	FMatrix ToUnreal(const FMatrix& Asset, const FMatrix& RootGlobal) const;
	/** Sets the transform avars `Avars` names (".avars:tx" .. ":rz" -> raw
	 * avar value) on the Control Rig control at `Path`, every other channel
	 * as it is, with undo. False when it has no control. */
	bool SetControlAvars(const FString& Path, const TMap<FString, double>& Avars);
	/** A channel's value now: the Control Rig's when there is one, else as
	 * last sent. */
	double ChannelNow(const FString& Path) const;
	/** Writes a channel: through the Control Rig with undo when there is
	 * one, else straight to the rig. */
	void WriteChannel(const FString& Path, double Value);
	/** Whether switching writes through a Control Rig (the editor) or
	 * straight to the rig (a game, which has none). */
	class UControlRig* SwitchRig() const;
	/** One IK/FK limb, as its RigExecIkFkMatchConstraint describes it. The
	 * rig computes the match (the runtime's GetIkFkMatches); a switch writes
	 * those values to the incoming half's writable channels. */
	struct FLimbMap
	{
		/** The match constraint's prim path, as the runtime publishes it. */
		FString Match;
		FString Switch;
		double IkValue = 1.0;
		TArray<FString> FkControls;
		FString IkControl;
		FString Pole;
		/** The channels the match's stretch, pin and twist inputs read, or
		 * empty. */
		FString UpperScale, LowerScale, Pin, Twist;
		/** The channel paths a switch may write: the rig writes or drives
		 * none of them. */
		TSet<FString> Writable;
	};
	TArray<FLimbMap> LimbMaps;
	bool bPlaced = false;
	/** Evaluate on the game thread whatever bEvaluateAsync says: a switch
	 * reads what its own writes computed. */
	bool bEvaluateNow = false;
	/** Runs published, and the one the gizmos were last placed from: a run
	 * landing after the drag stopped still re-places them. */
	int32 PublishCount = 0;
	int32 PlacedPublish = -1;
	/** Each faded control's shape colour as the Control Rig built it, kept
	 * across Loads (the dimmed one is all the hierarchy holds meanwhile). */
	TMap<FName, FLinearColor> ShapeColors;
	TWeakObjectPtr<class UControlRig> ShapeColorsRig;
	/** Shows or dims the faded controls (a limb's idle half). */
	void UpdateFadedControls(class URigHierarchy* Hierarchy);
	/** A full resync is due (RequestResync). */
	bool bResync = false;
#if WITH_EDITOR
	FDelegateHandle UndoRedoHandle;
#endif

	/** The touch regions and what picking and drawing them needs. Owned;
	 * a raw pointer for the same reason as Reader. */
	struct FRigExecTouch;
	FRigExecTouch* Touch = nullptr;
	/** The runtime's published control and joint frames, gathered at most
	 * once per evaluation (AssetFrame), and the parallel dispatcher the
	 * reader runs its clusters through. Raw for the same reason as Reader. */
	struct FRigExecFrames;
	FRigExecFrames* FrameCache = nullptr;
	double LastPoseTime = 0.0;
	/** Takes the regions whose control is known; after the meshes and controls. */
	void LoadTouchRegions(const FRigExecMeshSet& Set);
	/** Writes the regions whose highlight changed into the meshes' colours. */
	void RefreshTouchColors();
	/** Whether each mesh was drawn (visible) at its last update: a hidden
	 * mesh -- another renderer draws it -- is kept to positions only, and
	 * gets its normals back when it is shown again. */
	TArray<bool> MeshDrawn;

	/** The GPU skins drawing Meshes, parallel to it; empty on the CPU path. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<URigExecSkinComponent>> Skins;
	bool bGpu = false;
	FString FallbackReason;
	/** Chooses GPU or CPU drawing for the meshes Load built. */
	void SetUpRenderPath();
	/** Whether mesh `M` is drawn by its GPU skin right now (the skin exists
	 * and nothing else -- a subdivision surface -- has hidden the mesh). */
	bool SkinDraws(int32 M) const;
};
