# In a game: baking and retargeting

A `.rigexec` has no USD in it. The only thing the editor reads from the
stage is each mesh's topology, UVs, material looks and touch regions, once,
at load. A **RigExec Character** asset holds all of that next to the rig, so a
packaged game loads the character with no USD and no files on disk.

The example project uses this to make the biped playable: a third-person
character whose hidden UE5 Mannequin runs Epic's locomotion animation, and a
retargeter carries that animation onto the rig every frame.

## Playing the example

```bat
Examples\RigExecBiped\Setup_RigExecBiped.bat     & rem once: copies the Mannequin, builds, bakes, makes the level
```

Open `/Game/RigExec/L_RigExecGame` and press Play, or run it standalone:

```bat
"%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" Examples\RigExecBiped\RigExecBiped.uproject /Game/RigExec/L_RigExecGame -game
```

| Input | Does |
| --- | --- |
| WASD / left stick | Move |
| Mouse / right stick | Look |
| Space / gamepad A | Jump |
| M | Show the Mannequin a metre to the side, for comparing |
| `-` / `=`, `0`–`2` | Subdivision level, live (with Squarebit Subdivs) |
| Esc / gamepad B | Leave the close-up; otherwise open the pause menu (in a menu: back) |
| P / gamepad Start | Pause menu |

### The close-up

A glowing circle on the floor (`ARigExecCinematicPad`) starts a close-up
when the biped steps on it:

1. A hard cut to a camera on the face, with letterbox bars and its own key
   and fill lights. The biped stops; the Mannequin's idle keeps playing.
2. The subdivision level goes from 0 to 1 after a moment on the cage.
3. A blink (`L_UpLid`/`R_UpLid` translated 2.735 down), then the jaw opens
   (`M_Jaw` turned 12° about X) and settles just parted. As it settles, a
   smile grows in: the mouth corners (`L_Mouth`/`R_Mouth`) move out and up,
   the upper lip (`M_UpLip`, `L_UpLip`, `R_UpLip`) lifts to show the teeth,
   and the inner brows (`L_InBrow`/`R_InBrow`) rise for a slightly
   surprised look.
4. An idle that runs until Esc, looking mostly down at something in front
   of him. The eyes turn in the head (`L_Eye`/`R_Eye`, with `M_Look`'s
   `lookAt` switched off) and reach each new spot in a few frames. The head
   and neck set off after them about 0.2 s later, the eyes ease back as it
   arrives, and the upper lids follow the eyes down. Blinks come every 2–5 s,
   sometimes doubled and often with a big look.

The head turns ride on top of the retargeted animation through
`URigExecRetargetComponent::SetExtraTurn`. Esc cuts back to the follow
camera, sets the level to 0 and rests the face. The pad arms again once the
biped has stepped off it.

The HUD always shows **SUBD LEVEL**. Keys change it at any time, and in the
close-up the cursor shows, so its − / + buttons can be clicked. The
Subdivs component is made by class name, so the example builds without the
plugin and the HUD then says so. The level is capped at 2: on the biped's
143k render vertices, level 3 takes many seconds to build.

In the editor, Esc stops Play In Editor, so leave the close-up with gamepad
B and open the menu with P, or play standalone or packaged.

The glow material, `M_RigExecGlow`, is made by `Scripts/setup_game.py`
(`MakeGlowMaterial`). It is an additive ring and a rippling fill.

### Packaging

```bat
package_game.bat                 & rem Build\Game\Windows\RigExecBiped.exe
```

`CONFIG=Shipping` packages a shipping build. With the editor open, set
`RIGEXEC_UAT_ARGS=-nocompileeditor`: the editor's Live Coding blocks
rebuilding the editor modules, and the game build does not need them.
The stats in the menu need a Development build. The package carries no USD
libraries and no `.usda` files: the character is the cooked `RXC_Biped`.

### Pause menu and settings

Esc (outside the close-up), P or gamepad Start pauses the game and opens
**PAUSED**: Resume, Settings, Quit Game. Quit closes the game (in the
editor it ends Play In Editor). The menu works with the mouse, the arrow
keys or W/S with Enter or Space, and the gamepad's D-pad with A and B.

The FPS counter in the bottom-left corner shows frames per second, the
frame time and the worst frame of the last second, in real time whatever
the pause or time scale. **Settings** holds the testing switches:

| Setting | Does |
| --- | --- |
| FPS counter | The counter on or off |
| Debug stats | A panel over it: game, render and GPU time, resolution, speed, subdivision level, close-up or play, the retarget's cost and error, its status |
| Engine stats | The engine's `stat unit` |
| Subdivision panel | The SUBD LEVEL panel on or off |
| Subdivision level | 0, 1 or 2 (n/a without Squarebit Subdivs) |
| Show Mannequin | The Mannequin a metre to the side |
| Time scale | ×1, ×0.5, ×0.25, ×0.1, for watching motion closely |
| VSync | `r.VSync` |
| Frame cap | Off, 30, 60, 120 (`t.MaxFPS`) |
| Screen percentage | 100, 75, 50 (`r.ScreenPercentage`), to tell a GPU limit from a CPU one |

The display settings are saved in the game's `GameUserSettings.ini`. Time
scale, the Mannequin and the level start fresh each time. Both levels have
the menu: their HUDs share `ARigExecMenuHUD`, which draws over each level's
own HUD.

### Splash and loading screen

The packaged game opens on a splash and a loading screen:

1. `Content/Splash/Splash.bmp` while the engine starts (Windows draws it
   before the game window exists);
2. the **RigExecBipedLoadingScreen** module's screen, from the moment the
   window opens until the level and the biped have loaded: the picture,
   `Content/Splash/LoadingScreen.png`, with a throbber and a pulsing
   LOADING. The movie player draws it on its own thread, so it keeps moving
   while the game thread loads. It shows again for any level opened later.

The module loads before the engine starts (`PreLoadingScreen`), so it uses
no assets: it decodes the PNG itself and hands Slate the pixels. Both files
ship as loose files (`DirectoriesToAlwaysStageAsNonUFS` in
`DefaultGame.ini`). It does nothing in the editor.

The picture is a render of the biped's own face. `-RigExecPortrait` poses
and lights it (subdivision level 2, a smile, eyes on the lens) and saves a
few framings to `Saved/RigExecPortrait`. `Scripts/make_splash.py` lays one
out with the title:

```bat
Build\Game\Windows\RigExecBiped.exe /Game/RigExec/L_RigExecGame -RigExecPortrait -UseFixedTimeStep -FPS=30 -windowed -resx=1280 -resy=720
python Scripts\make_splash.py Build\Game\Windows\RigExecBiped\Saved\RigExecPortrait\portrait_0.png Examples\RigExecBiped\Content\Splash
```

### Test switches

The character reads these from the command line, in the editor or the
packaged game:

| Switch | Does |
| --- | --- |
| `-RigExecDemo` | Runs a circle on its own (walk, then run), jumps every 7 s, swings the camera round, and logs the retarget's cost and error each second |
| `-RigExecShots` | With `-RigExecDemo`: a screenshot every 2.5 s into `Saved/RigExecShots` |
| `-RigExecQuitAfter=<s>` | With `-RigExecDemo`: quits after that many seconds (logs `RIGEXEC_DEMO_DONE`) |
| `-RigExecShowSource` | Starts with the Mannequin shown |
| `-RigExecFrames` | Saves every frame to `Saved/RigExecFrames`, for a video (with `-UseFixedTimeStep -FPS=30`) |
| `-RigExecAutoplay` | On the platformer level: the autopilot plays the course |
| `-RigExecPadDemo` | On `L_RigExecGame`: runs a lap onto the glowing pad, watches the close-up, steps the subdivision level 1 → 2 → 0 → 1, presses Esc, runs off and quits |
| `-RigExecPortrait` | On `L_RigExecGame`: poses and lights the face, saves four high-resolution framings to `Saved/RigExecPortrait`, and quits |

## The platformer

`/Game/RigExec/L_RigExecPlatformer` is a side-scrolling course with the biped
as the hero. `ARigExecPlatformerLevel` builds it at load from an ASCII layout,
which `Scripts/make_platformer_layout.py` generates. The course has ground
with pits, bricks, `?` blocks that pay out a coin when headbutted from below,
pipes, staircases, coin arcs, a flagpole and a castle. Its pieces are
instanced basic shapes in flat colours, with hills, bushes and clouds behind.
`ARigExecPlatformerCharacter` is the third-person character with:

- a side camera that leads along the course and follows the height of the
  ground stood on, not every jump;
- movement locked to the course's plane;
- a high jump that goes higher the longer Space is held.

A/D or the left stick runs, Space jumps. The HUD shows coins, the world and
the time, and **COURSE CLEAR!** past the flag.

`-RigExecAutoplay` plays it with no one at the controls. The autopilot runs
right and reads the course with traces: it jumps at a wall or step ahead,
at a pit ahead, and under a `?` block it hasn't hit yet. Past the flag it
walks to the castle door and, when recording, quits. To record it from the
packaged game:

```bat
Build\Game\Windows\RigExecBiped.exe /Game/RigExec/L_RigExecPlatformer -RigExecAutoplay -RigExecFrames -UseFixedTimeStep -FPS=30 -windowed -resx=1280 -resy=720
ffmpeg -framerate 30 -i Build\Game\Windows\RigExecBiped\Saved\RigExecFrames\frame_%05d.png -c:v libx264 -pix_fmt yuv420p -movflags +faststart platformer.mp4
```

`-RigExecFrames` saves every frame; with the fixed 30 fps step, each frame is
1/30 s of play.

## Squarebit Eye

The biped's eyes have surface projectors: each eye mesh gets the frame its
iris is projected from (`primvars:eyeProjector`) and the eye control's
pupil and iris channels packed into a matrix (`primvars:eyeDials`). The
runtime publishes both (`URigExecComponent::GetMatrixPrimvar`).
`URigExecEyeComponent` (**RigExec Squarebit Eye**) puts the Squarebit Eye
materials on every mesh with a projector, the left and right variants by
the mesh's name. Every frame after the rig evaluates, it:

- writes the projector into the meshes' Custom Primitive Data, in the Eye's
  packed layout (origin and right scale, aim and pupil radius, up), on the
  Dynamic Mesh, the GPU skin and any Squarebit Subdivs surface;
- sends the dials to the material: sizes multiply by `1 + dial` and offsets
  add, as the rig's own eye shader does.

It refers to the Eye only by asset path, so the plugin builds and runs
without it, and its **Status** then says the materials are missing.
Install it into the example with:

```bat
install_eye.bat                  & rem copies ..\SquarebitEye\unreal\SquarebitEye into the example's Plugins
```

The example's build then compiles it for this engine, and the cook takes
`/SquarebitEye`. The character binds its eyes before it adds the
subdivision surface, so the surface picks the Eye materials up.

## The character asset

`URigExecCharacter` (Content Browser: **RigExec Character**) holds:

- the `.rigexec` bytes;
- the controls description, with the Control Rig element names, and the
  picker layout;
- for every mesh the rig poses: face counts and indices, one UV per face
  corner, each face's material slot and each slot's preview-surface colour,
  roughness and metallic;
- the touch regions and their colours.

Bake one in the editor:

```python
unreal.RigExecRuntimeLibrary.bake_character(
    "RigExecData/Biped.rigexec", "../../../usdRig/examples/biped/Biped_stack.usda",
    "RigExecData/Biped_controls.json", "/Game/RigExec/RXC_Biped")
```

Relative files are relative to the project. `Setup_RigExecBiped.bat` bakes
`RXC_Biped` after it rebuilds the Control Rig, so the controls carry their
element names. Bake again whenever the `.rigexec` or the controls change
(`prepare_biped.bat`).

Set **Character** on a `URigExecComponent` and it loads from the asset and
ignores the file paths. The editor works the same with either: the pickers,
Touch Pose and Sequencer all run from the asset.

## The retargeter

`URigExecRetargetComponent` drives a `URigExecComponent` from an animated
`USkeletalMeshComponent`. The source can be hidden; it is made to keep
animating. Each frame, after the source's animation:

1. **Facing.** At bind, the source is turned to face the way the rig does,
   from a pair of bones across the body (`L_UpLeg`/`R_UpLeg`) and a pair
   along it (`M_Body`/`M_Head`).
2. **Rest poses lined up.** Walking down the map, each bone with an aim
   child is turned so it points the way its control does. This takes the
   Mannequin's A-pose onto the biped's T-pose, as an IK Retargeter's
   retarget pose does.
3. **Rotations.** Each bone's world rotation relative to its lined-up rest
   is applied to its control's rest frame.
4. **Movement.** The pelvis's movement is scaled by the two pelvis heights
   and carried onto `M_Body`.
5. **Controls.** Walking the controls root first, each driven control's value
   is solved from the frame it should have and its parent's. The offset
   between them is whatever the rig last put there. That is exact for a
   plain hierarchy, and it keeps up with a constraint that moves it. The rig
   then evaluates once.

It hands each IK/FK limb to FK first (`bForceFk`), and turns the component's
Control Rig pull off, since the retargeter owns the controls.

`SetExtraTurn(Control, Turn)` layers a turn, in the rig's asset space, on a
driven control after the animation. The close-up uses it to turn the head
and neck while the idle plays.

`MannequinToBiped()` is the default map: pelvis, spine, neck and head, and
clavicle, arm, hand, thigh, calf, foot and ball on both sides, 22 controls.
Fingers are left out. The biped's finger controls hang off nulls that follow
the hand through constraints, and locomotion barely moves them.

**Status** says what it is driving, **LastMilliseconds** what a frame cost, and
**LastErrorDegrees** the worst gap between a driven control's frame as the rig
computed it and as asked: under 1° walking, up to about 5° at a run.

### Other animation

Anything on the UE5 Mannequin skeleton plays as is: the example's Animation
Blueprint, a montage, or Epic's Game Animation Sample. To use Fab content,
add it to the project and set the character's **Source Animation** to its
Animation Blueprint. For another skeleton, change **Bones** (bone, control,
the aim child of each, and which one carries the movement).

## Cost

On the biped, a frame takes about 15–20 ms on the game thread. Most of it is
the rig evaluation and the gathering of frame publications. The retarget
math is a fraction of a millisecond. Gizmo placement, about 14 ms a pose in
the editor, is skipped in game worlds, because a game draws no gizmos.

## Limits

- **Plugin movers.** A mover that usdRig plays through a USD-side library
  has no kernel in the Unreal runtime, and its points pass through. Today
  that is `RigExecPatchMover` (the patch curve and mouth examples). `Load()`
  warns when a rig has one, and `URigExecRuntimeLibrary::RigExecInspect`
  reports it for any file. It needs the kernel moved into a USD-free usdRig
  library, which the plugin would then register with `SetExternalKernel`.
- **FK only.** Legs are driven by rotation, so feet can slide a little where
  the two characters' proportions differ. The Mannequin's foot IK runs on
  the source, before the retarget.
- **Squarebit Subdivs** builds into the game when it is installed. On
  `L_RigExecGame` the character adds its Subdivs component at play (level 0
  until the close-up or a key changes it); elsewhere the game draws the
  rig's own meshes.
- **Eyes.** Without Squarebit Eye, the baked biped's eyes draw plain white:
  the rig's own eye shader is a glslfx for usdview.
