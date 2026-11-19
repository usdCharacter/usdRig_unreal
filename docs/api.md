# API reference

Everything below is in the `RigExecRuntime` module (`RIGEXECRUNTIME_API`)
unless noted otherwise. Blueprint names are as shown. Python uses
snake_case (`rig.set_channel(...)`).

Channel and control paths are **rig prim paths**, exactly as usdview shows
them. A control is `/Biped/Rig/.../L_Arm`, and a channel is that path plus
`.avars:<name>`, for example `/Biped/Rig/.../L_Arm.avars:ikfk`.

## `ARigExecActor`

`Public/RigExecActor.h`: a character ready for Sequencer.

| Member | |
| --- | --- |
| `URigExecComponent* Rig` | The rig and its meshes |
| `UControlRigComponent* Controls` | The Control Rig that poses it. `Rig->ControlRig` points here |
| `void SetControlRigAsset(UObject* Asset)` | Points `Controls` at a Control Rig Blueprint, or at its generated class |

Both components are attached to a plain `Root`. The actor itself does not
tick.

## `URigExecComponent`

`Public/RigExecComponent.h`, a `USceneComponent`. It ticks in
`TG_PostUpdateWork`, after the Control Rig component, and ticks in the
editor too.

### Properties (category *RigExec*)

| Property | Type | |
| --- | --- | --- |
| `Character` | `URigExecCharacter*` | The baked character: the rig, its controls and picker, and its meshes in one asset, with no USD. When set, the file paths are ignored. What a packaged game loads (see [game.md](game.md)) |
| `RigExecFile` | `FFilePath` | The `.rigexec` (`rigExecBake`). Relative paths are relative to the project directory |
| `StageFile` | `FFilePath` | The USD stage the rig was baked from, read for mesh topology and materials |
| `SurfaceMaterial` | `UMaterialInterface*` | Parent of each mesh material. It receives `Color`/`BaseColor`, `Roughness` and `Metallic` parameters. Empty means `/RigExec/M_RigExecSurface`, which also blends the vertex colour in by its alpha (the Touch Pose tint, see [Touch Pose](editor-workflow.md#touch-pose)) |
| `ControlsFile` | `FFilePath` | The controls description (after `build_control_rig.py`). Optional: without it, nothing maps the Control Rig onto the rig |
| `ControlRig` | `UControlRigComponent*` | The Control Rig that poses the rig. Empty means the owner's first one |
| `bPullControlRig` | `bool` | Pull the Control Rig's controls into the rig each tick (default on). Off when something else drives the controls, such as the retargeter |

### Functions

| Function | Blueprint | |
| --- | --- | --- |
| `bool Load()` | ✓ | Opens the rig, runs its first frame, builds the meshes (from `Character`, else from the stage) and reads the controls. Returns false, with the reason logged to `LogRigExec`, on failure. Called from `OnRegister` when `Character` or both file paths are set, except in a commandlet |
| `bool SetChannel(const FString& Channel, float Value)` | ✓ | Sets one avar by path. It takes effect at the next `Evaluate`. Returns false (with a warning) when the runtime refuses it |
| `void ClearChannels()` | ✓ | Drops every avar that was set, back to the baked pose, and resets the cached control values so the next pull resends them |
| `float Evaluate()` | ✓ | Runs the rig and moves the meshes. Returns milliseconds, or negative on failure |
| `int32 PullControls()` | ✓ | Reads the Control Rig into avars and evaluates if anything moved, then re-places the control offsets. Returns the number of avars that changed. Called every tick |
| `TArray<UDynamicMeshComponent*> GetMeshes() const` | ✓ | The meshes `Load` built, in the rig's mesh order |
| `FVector GetVertex(int32 Mesh, int32 Index) const` | ✓ | A vertex as last placed, in Unreal space (for tests) |
| `bool SwitchLimb(const FString& SwitchPath)` | ✓ | Hands the limb switched by `SwitchPath` (`….avars:ikfk`) to its other half without moving it, then flips the switch. With a Control Rig it writes through it with undo, so wrap it in a transaction; with none (a game) it writes straight to the rig and evaluates. Returns false if there is no such limb |
| `FString FindLimbSwitchFor(FName Element) const` | C++ | The switch (`….avars:ikfk`) of the limb a Control Rig control belongs to: the switch's control, an FK or IK control, the pole, or anything under the IK control. Empty when it is on no limb |
| `bool IsLimbIk(const FString& SwitchPath) const` | C++ | Whether that limb is on its IK half now |
| `bool FindSpaceChannel(FName Element, FName& OutChannel, TArray<FString>& OutLabels) const` | C++ | The control's space channel and its space names, from the controls file. False when it has fewer than two spaces |
| `bool SwitchSpace(FName Element, int32 Index)` | ✓ | Moves the control to that space without moving it, using the values the rig publishes for each space (`<control>.spaces:matchValues`). With a Control Rig it writes through it with undo, so wrap it in a transaction; with none it writes straight to the rig |
| `FString FindPresentedLimbFor(FName Element) const` | ✓ | The switch of the limb `Element` belongs to when it is on the half the limb is not driving with (what selecting it switches, matched). Empty otherwise |
| `FTransform GetRigFrame(const FString& Path) const` | ✓ | The posed frame of a control or joint, in the Control Rig's space. Identity if the path is unknown |
| `FName FindControlElement(const FString& Path) const` | C++ | The Control Rig control for a rig control |
| `FName FindChannelElement(const FString& AttrPath) const` | C++ | The Control Rig control or channel for a rig attribute |
| `FString PickerFilePath() const` | C++ | `<Name>_picker.json` beside the controls file, or empty |
| `bool IsLimbSwitch(const FString& SwitchPath) const` | C++ | Whether `SwitchPath` is a limb switch this rig can match |
| `bool GetPickerText(FString& Out) const` | C++ | The picker layout: the character's, else the file's |
| `static bool ReadStageMeshes(const FString& Stage, const TArray<FString>& PointsProperties, FRigExecMeshSet& Out, FString& Error)` | C++ | Reads each posed mesh's topology, UVs, looks and touch regions off a stage. Builds with the USD SDK (the editor) only |

**Driving controls directly** (C++), for code that bypasses Control Rig,
such as `URigExecRetargetComponent`. Controls are indexed in the controls
description's order, parents first. Matrices are USD row-vector, asset
space, rig units.

| Function | |
| --- | --- |
| `int32 GetControlCount() const`, `int32 FindControlIndex(const FString& NameOrPath) const` | The controls, and one by element name, path or last path component (-1 if none) |
| `int32 GetControlParent(int32)`, `bool IsControlNull(int32)`, `const FString& GetControlPath(int32)` | Its parent, whether it is a helper null, its path |
| `FMatrix GetControlValueMatrix(int32 Control, bool bDefault = false) const` | Its value as last sent, or its rest value |
| `bool GetControlAssetFrame(int32 Control, FMatrix& Out) const` | Its posed frame from the last evaluation |
| `int32 SetControlValueMatrix(int32 Control, const FMatrix& Value)` | Sends a value as the control's avars, decomposed in its rotation order. Returns how many avars changed; `Evaluate` to see them |
| `TArray<TPair<FString, double>> GetLimbFkSwitches() const` | Each IK/FK limb's switch and its FK value |
| `FName GetControlElement(int32)` | Its Control Rig element name (what touch regions and picker buttons name) |
| `bool GetControlAvars(int32 Control, double Out[9], bool bDefault = false) const`, `uint16 GetControlAvarMask(int32) const` | Its nine transform avars (tx ty tz rx ry rz sx sy sz, raw values) as last sent or at rest, and which of them it exposes (bit A for avar A) |
| `int32 SetControlAvars(int32 Control, const double In[9])` | Sends raw avar values; ones the control does not expose are ignored. Returns how many changed |
| `int32 GetChannelCount()`, `int32 FindChannelIndex(const FString& Path)`, `GetChannelPath`, `GetChannelValue`, `GetChannelDefault`, `GetChannelLabels` | The non-transform channels the controls file describes (IK/FK dials, spaces, switches) and their values as last sent |
| `bool SetChannelValue(int32 Channel, double Value)` | Sends one; `SetChannel` by path also updates the value these report |
| `FMatrix GetLandedControlValueMatrix(int32) const` | The value the last landed run evaluated with: what pairs with `GetControlAssetFrame` |

Picking and posing with none of Control Rig, as the example's animation
studio does ([game.md](game.md#the-animation-studio)): `IsLimbIk`,
`FindPresentedLimbFor`, `SwitchLimb` and `SwitchSpace` read and write these
channels when the component has no Control Rig.
| `double GetCentimetresPerUnit() const` | Rig units to centimetres |

## `URigExecCharacter`

`Public/RigExecCharacter.h`: a baked character asset (see [game.md](game.md)).
It shows `SourceRigExec`, `SourceStage` and `SourceControls` (what it was baked
from), and `MeshCount`, `TouchRegionCount` and `RigExecBytes`. Its payload is
`Program` (the `.rigexec` bytes), `Controls`, `Picker` and `MeshSet` (an
`FRigExecMeshSet`). It is serialized with its own version number, and an asset
from another version is refused at load, so bake it again.

## `URigExecRetargetComponent`

`Public/RigExecRetargetComponent.h`: drives a `URigExecComponent` from an
animated `USkeletalMeshComponent` (see [game.md](game.md#the-retargeter)).

| Property | |
| --- | --- |
| `Source` | The animated mesh. Empty means the owner's first skeletal mesh |
| `Target` | The rig. Empty means the owner's |
| `Bones` | `FRigExecRetargetBone` entries, parents first: `SourceBone`, `TargetControl`, `SourceAim`/`TargetAim` (a child of each, to line the rest poses up), `bTranslate` (carry the movement). Defaults to `MannequinToBiped()` |
| `LeftControl`, `RightControl`, `LowControl`, `HighControl` | The entries whose targets set the facing |
| `bForceFk`, `bEnabled` | Hand the limbs to FK; switch it off |
| `Status`, `LastMilliseconds`, `LastErrorDegrees` | What it is doing, a frame's cost, and how closely the rig followed |

`Rebind()` binds again on the next tick. `DriveOnce()` binds if it has not,
then carries the source's current pose onto the rig once and evaluates,
enabled or not: a bake poses the source at each sample time and calls it
(the animation studio bakes its default clips this way).

### Example: posing without Control Rig

```cpp
URigExecComponent* Rig = Actor->Rig;
Rig->ClearChannels();
Rig->SetChannel(TEXT("/Biped/Rig/.../M_Jaw.avars:rx"), 15.f);
const float Ms = Rig->Evaluate();
```

```python
rig = actor.get_editor_property("rig")
rig.set_channel("/Biped/Rig/.../M_Jaw.avars:rx", 15.0)
print(rig.evaluate(), "ms")
```

Each tick also calls `PullControls()`, which re-sends any Control Rig value
that differs from what it last sent. To drive a character purely from
script, either leave `ControlsFile` empty or set `ControlRig` to none.

### Render path (category *RigExec|Render*)

| Member | Blueprint | |
| --- | --- | --- |
| `bDrawOnGPU` | property | Draw the meshes with the GPU skin (default). Off, or where it cannot run, the CPU fallback draws them. Changing it in Details rebuilds the meshes |
| `bMotionBlur` | property | On the GPU, write motion vectors from the pose's movement for TSR and motion blur (default on). Changing it rebuilds the meshes |
| `RenderPath` | read-only property | How the meshes draw now: `GPU: …`, or `CPU fallback: <reason>` |
| `bool IsDrawingOnGPU() const` | ✓ | Whether the GPU skin draws right now |
| `static FOnRenderFallback OnRenderFallback` | C++ | Broadcast `(Component, Reason)` when a component that wanted the GPU falls back. The editor turns it into a toast |

`URigExecSkinComponent` is the GPU skin: one per mesh, created and fed by the Rig component, not meant to be
added by hand.

### Touch regions (category *RigExec|Touch*)

| Member | Blueprint | |
| --- | --- | --- |
| `int32 GetTouchRegionCount() const` | ✓ | Regions read at load, numbered across the meshes |
| `FString GetTouchRegionName(int32 Region) const` | ✓ | The region prim's name, for example `R_LoArm_touch` |
| `FName GetTouchRegionControl(int32 Region) const` | ✓ | The Control Rig control the region selects |
| `int32 GetTouchRegionMesh(int32 Region) const`, `int32 GetTouchRegionFaceCount(int32 Region) const` | C++ | The drawn mesh the region paints, and how many faces |
| `int32 PickTouchRegion(FVector RayOrigin, FVector RayDirection, double& OutDistance)` | ✓ | The region a world-space ray first hits on the *posed* skin, or -1 if it misses or hits unpainted skin. The bounding-volume tree it uses is rebuilt lazily after the rig moves |
| `TArray<int32> GetTouchRegionsOf(const TArray<FName>& Controls) const` | C++ | Regions whose control is in the list |
| `void SetTouchHighlight(int32 Hover, int32 Lead, const TArray<int32>& Selected)` | ✓ | Lights the hover, lead and selected regions (−1/empty for none) by writing their faces' vertex colours, and only for regions whose state changed. The tint rides the skin as it poses, and reaches a subdivision surface that draws the meshes |
| `void SetTouchSuspended(bool bSuspended)` / `bool IsTouchSuspended() const` | ✓ | Clears the tint without forgetting the highlight. Resuming writes it back |
| `double GetLastPoseTime() const` | ✓ | `FPlatformTime::Seconds()` of the last `Evaluate`, or 0 |

```python
rig = actor.get_editor_property("rig")
region, distance = rig.pick_touch_region(unreal.Vector(0, -400, 120), unreal.Vector(0, 1, 0))
print(rig.get_touch_region_name(region), rig.get_touch_region_control(region))
```

## `URigExecRuntimeLibrary`

`Public/RigExecRuntimeLibrary.h`: static Blueprint and Python helpers.

| Function | |
| --- | --- |
| `FString RigExecSelfTest(const FString& RigExecFile, const FString& Channel, const TArray<float>& Values)` | Opens a `.rigexec` and poses `Channel` once per value. Reports whether property chains are live, each pose's time, and the largest point move from the first pose. Needs no stage and no Control Rig |
| `bool ShowControlRigInViewport(UControlRig* ControlRig)` | *Editor only.* Enters the Control Rig animation mode, hands it this rig and shows its controls, which is the state the Animation Outliner's eye toggles. It loads the rig's shape libraries first, or the gizmos have no mesh |
| `bool SetControlsAsOverlay(bool bOverlay)` | *Editor only.* Sets the Animation mode's "Controls As Overlay" (`ControlRigEditModeSettings.bShowControlsAsOverlay`) and saves it. Set it before opening a sequence |
| `FString RigExecInspect(const FString& RigExecFile)` | Opens a `.rigexec` with the plugin's runtime and reports whether it plays here: its inputs and outputs, any plugin mover with no kernel, and which number inputs move it (each nudged by 0.5). Starts with `OK` when it opens, evaluates, responds and resets |
| `URigExecCharacter* BakeCharacter(const FString& RigExecFile, const FString& StageFile, const FString& ControlsFile, const FString& AssetPath)` | *Editor only.* Bakes a character asset at `AssetPath` and saves it. Relative files are relative to the project |

## Editor module (`RigExecEditor`)

| Entry point | |
| --- | --- |
| Tab `RigExecPicker` | "RigExec Picker", a nomad tab in the Level Editor's Window menu |
| **Tools → RigExec → Picker** | Opens the tab |
| Console `RigExec.OpenPicker` | Opens the tab |
| **Tools → RigExec → Hover Picker** | Toggles the hover picker over the level viewports |
| Console `RigExec.HoverPicker [0\|1]` | Shows or hides the hover picker; toggles with no argument |
| **P** over a level viewport | Toggles the hover picker (only with a RigExec character in the level) |
| Right-click over a control | Opens the marking menu (always on; see editor-workflow.md) |
| **Tools → RigExec → Touch Pose** | Toggles Touch Pose in the level viewports |
| Console `RigExec.TouchPose [0\|1]` | Turns Touch Pose on or off; toggles with no argument |

`SRigExecPicker`, `FRigExecPickerModel` and `FRigExecHoverPicker` are
private to the module. The hover picker's layout is saved per user in
`Saved/RigExec/HoverPicker.json`.

## Log category

`LogRigExec` (in `RigExecComponent.cpp`). On a successful load it logs:

```
loaded <file>: <n> meshes, <n> controls, <n> channels, <n> property chain values
<n> of <m> channels are not live in <file>: <attr> on <control>, ...
```

## The runtime (`RigExecLib`)

The vendored `rigExec::RigExecRuntimeReader`
(`ThirdParty/RigExecLib/include/rigExecRuntime/runtime.h`) is usdRig's API.
The plugin uses this subset of it:

| Call | Use |
| --- | --- |
| `static Open(bytes, size, &error)` | Opens a `.rigexec` image held in memory; the inputs start at their bake-time values |
| `SetInput(path, value, &error)` / `ResetInputs()` | Live inputs (`<control>.avars:tx`, `<control>.avars:ikfk`, …) |
| `SetTaskDispatch(dispatch, wait)` | Runs the rig's clusters in parallel (the component passes `UE::Tasks`) |
| `Execute(&error)` | Evaluates |
| `GetPoints()` | Per-mesh deformed points (`path` is `<mesh>.points`), shared buffers |
| `GetProviderFramePublications()` | Every control and joint's posed frame, asset space |
| `GetIkFkMatches()` | Each limb's IK/FK match values, for `SwitchLimb` |
| `GetPropertyValues()` | The property chains' published values |

It also offers `GetSpaceMatches`, `SetExternalKernel` for plugin movers, joint
matrices, diagnostics and counters. See the header comments.
