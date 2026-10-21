"""
Builds the example's game content inside the Unreal Editor: the baked
character asset and a level to run around in.

    RXC_Biped        the .rigexec, its controls and picker, and the meshes
                     read from the stage, in one asset (BakeCharacter): a
                     packaged game loads it with no USD
    L_RigExecGame    a floor, a few blocks and ramps, lights, a player start
                     and a glowing pad (ARigExecCinematicPad) that starts the
                     face close-up; its game mode spawns
                     ARigExecBipedCharacter, which retargets the Mannequin's
                     locomotion onto the rig
    M_RigExecGlow    the pad's additive glow: a ring and a rippling fill
    L_RigExecPlatformer
                     a side-scrolling course (ARigExecPlatformerLevel builds
                     it from its ASCII layout); the biped is the hero

Called by setup_example_project.py after the Control Rig is built (the
controls file then carries the Control Rig element names the asset keeps).
The Mannequin it animates from is copied into Content/Characters by
Setup_RigExecBiped.bat.
"""

import unreal

CHARACTER = "/Game/RigExec/RXC_Biped"
LEVEL = "/Game/RigExec/L_RigExecGame"
GAME_MODE = "/Script/RigExecBiped.RigExecBipedGameMode"
PLATFORMER = "/Game/RigExec/L_RigExecPlatformer"
PLATFORMER_MODE = "/Script/RigExecBiped.RigExecPlatformerGameMode"
GRID = "/Engine/EngineMaterials/WorldGridMaterial"
GLOW = "/Game/RigExec/M_RigExecGlow"
PAD = "/Script/RigExecBiped.RigExecCinematicPad"

# The glow, on a 0-1 plane: a bright ring near the edge and a soft fill
# rippling outward, nothing past the circle (additive, so black is clear).
GLOW_CODE = """
float d = length(UV - 0.5) * 2.0;
float ring = pow(saturate(1.0 - abs(d - 0.84) * 11.0), 2.0);
float fill = saturate(1.0 - d) * (0.22 + 0.12 * sin(d * 26.0 - T * 3.0));
return (ring + fill) * step(d, 1.0);
"""


def Bake(rigexec, stage, controls):
    character = unreal.RigExecRuntimeLibrary.bake_character(rigexec, stage, controls, CHARACTER)
    if character is None:
        raise RuntimeError("BakeCharacter failed; see the log")
    unreal.log("RigExec: baked %s (%d meshes, %d touch regions)" % (
        CHARACTER, character.get_editor_property("mesh_count"),
        character.get_editor_property("touch_region_count")))
    return character


def MakeGlowMaterial():
    """The pad's material: unlit, additive, Color x Glow x the ring shape."""
    if unreal.EditorAssetLibrary.does_asset_exist(GLOW):
        unreal.EditorAssetLibrary.delete_asset(GLOW)
    folder, name = GLOW.rsplit("/", 1)
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, folder, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    material.set_editor_property("two_sided", True)
    lib = unreal.MaterialEditingLibrary
    color = lib.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -900, 0)
    color.set_editor_property("parameter_name", "Color")
    color.set_editor_property("default_value", unreal.LinearColor(0.25, 0.75, 1.0, 1.0))
    glow = lib.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, 220)
    glow.set_editor_property("parameter_name", "Glow")
    glow.set_editor_property("default_value", 8.0)
    uv = lib.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -900, 400)
    time = lib.create_material_expression(material, unreal.MaterialExpressionTime, -900, 520)
    shape = lib.create_material_expression(material, unreal.MaterialExpressionCustom, -600, 400)
    shape.set_editor_property("code", GLOW_CODE)
    shape.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    inputs = []
    for input_name in ("UV", "T"):
        entry = unreal.CustomInput()
        entry.set_editor_property("input_name", input_name)
        inputs.append(entry)
    shape.set_editor_property("inputs", inputs)
    lib.connect_material_expressions(uv, "", shape, "UV")
    lib.connect_material_expressions(time, "", shape, "T")
    tint = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -600, 100)
    lib.connect_material_expressions(color, "", tint, "A")
    lib.connect_material_expressions(glow, "", tint, "B")
    out = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, -300, 200)
    lib.connect_material_expressions(tint, "", out, "A")
    lib.connect_material_expressions(shape, "", out, "B")
    lib.connect_material_property(out, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(GLOW)
    unreal.log("RigExec: made %s" % GLOW)
    return material


def _Spawn(cls, location, rotation=None, label=None):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = actors.spawn_actor_from_class(cls, unreal.Vector(*location), rotation or unreal.Rotator(0, 0, 0))
    if label:
        actor.set_actor_label(label)
    return actor


def _Block(mesh, location, scale, rotation=None, label=None):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = actors.spawn_actor_from_object(unreal.load_asset(mesh), unreal.Vector(*location),
                                           rotation or unreal.Rotator(0, 0, 0))
    actor.set_actor_scale3d(unreal.Vector(*scale))
    if label:
        actor.set_actor_label(label)
    grid = unreal.load_asset(GRID)
    for component in actor.get_components_by_class(unreal.StaticMeshComponent):
        component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
        if grid:
            component.set_material(0, grid)
    return actor


def BuildLevel():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    levels.load_level("/Engine/Maps/Entry")
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL):
        unreal.EditorAssetLibrary.delete_asset(LEVEL)
    if not levels.new_level(LEVEL):
        raise RuntimeError("could not create %s" % LEVEL)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    settings = world.get_world_settings()
    settings.set_editor_property("force_no_precomputed_lighting", True)
    mode = unreal.load_class(None, GAME_MODE)
    if mode is None:
        raise RuntimeError("no %s; build the project's C++ first" % GAME_MODE)
    settings.set_editor_property("default_game_mode", mode)

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for actor in actors.get_all_level_actors():
        if isinstance(actor, (unreal.DirectionalLight, unreal.SkyLight, unreal.SkyAtmosphere,
                              unreal.ExponentialHeightFog, unreal.VolumetricCloud, unreal.StaticMeshActor)):
            actors.destroy_actor(actor)

    # 60 m of floor, then a few things to run round and up.
    _Block("/Engine/BasicShapes/Cube", (0, 0, -50), (60, 60, 1), label="Floor")
    _Block("/Engine/BasicShapes/Cube", (900, 0, 50), (4, 4, 1), label="Step")
    _Block("/Engine/BasicShapes/Cube", (900, 600, 100), (4, 4, 2), label="Block")
    _Block("/Engine/BasicShapes/Cube", (300, -900, 80), (8, 3, 0.2), unreal.Rotator(roll=0, pitch=12, yaw=0), label="Ramp")
    for i in range(6):
        _Block("/Engine/BasicShapes/Cylinder", (-700 + 250 * i, 800, 100), (0.5, 0.5, 2), label="Pillar%d" % i)

    sun = _Spawn(unreal.DirectionalLight, (0, 0, 500), unreal.Rotator(roll=0, pitch=-40, yaw=-130), "Sun")
    sky = _Spawn(unreal.SkyLight, (0, 0, 500), label="SkyLight")
    for actor in (sun, sky):
        for component in actor.get_components_by_class(unreal.SceneComponent):
            component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    sky.get_component_by_class(unreal.SkyLightComponent).set_editor_property("real_time_capture", True)
    _Spawn(unreal.SkyAtmosphere, (0, 0, 0), label="SkyAtmosphere")
    _Spawn(unreal.ExponentialHeightFog, (0, 0, 0), label="Fog")
    _Spawn(unreal.PlayerStart, (0, 0, 100), unreal.Rotator(0, 0, 0), "PlayerStart")
    # Step on it for the face close-up.
    pad = unreal.load_class(None, PAD)
    if pad is None:
        raise RuntimeError("no %s; build the project's C++ first" % PAD)
    _Spawn(pad, (-500, -500, 0), label="CinematicPad")

    if not levels.save_current_level():
        raise RuntimeError("could not save %s" % LEVEL)
    unreal.log("RigExec: game level ready (%s)" % LEVEL)


def BuildPlatformer():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    levels.load_level("/Engine/Maps/Entry")
    if unreal.EditorAssetLibrary.does_asset_exist(PLATFORMER):
        unreal.EditorAssetLibrary.delete_asset(PLATFORMER)
    if not levels.new_level(PLATFORMER):
        raise RuntimeError("could not create %s" % PLATFORMER)
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    settings = world.get_world_settings()
    settings.set_editor_property("force_no_precomputed_lighting", True)
    mode = unreal.load_class(None, PLATFORMER_MODE)
    if mode is None:
        raise RuntimeError("no %s; build the project's C++ first" % PLATFORMER_MODE)
    settings.set_editor_property("default_game_mode", mode)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for actor in actors.get_all_level_actors():
        if isinstance(actor, (unreal.DirectionalLight, unreal.SkyLight, unreal.SkyAtmosphere,
                              unreal.ExponentialHeightFog, unreal.VolumetricCloud, unreal.StaticMeshActor)):
            actors.destroy_actor(actor)
    _Spawn(unreal.load_class(None, "/Script/RigExecBiped.RigExecPlatformerLevel"), (0, 0, 0), label="Course")
    # The sun from the camera's side (+Y) and the left, so the course faces it.
    sun = _Spawn(unreal.DirectionalLight, (0, 0, 800), unreal.Rotator(roll=0, pitch=-45, yaw=-70), "Sun")
    sky = _Spawn(unreal.SkyLight, (0, 0, 800), label="SkyLight")
    for actor in (sun, sky):
        for component in actor.get_components_by_class(unreal.SceneComponent):
            component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    sky.get_component_by_class(unreal.SkyLightComponent).set_editor_property("real_time_capture", True)
    _Spawn(unreal.SkyAtmosphere, (0, 0, 0), label="SkyAtmosphere")
    # The character moves itself to the course's start.
    _Spawn(unreal.PlayerStart, (300, 0, 200), label="PlayerStart")
    if not levels.save_current_level():
        raise RuntimeError("could not save %s" % PLATFORMER)
    unreal.log("RigExec: platformer level ready (%s)" % PLATFORMER)


def Setup(rigexec, stage, controls):
    Bake(rigexec, stage, controls)
    MakeGlowMaterial()
    BuildLevel()
    BuildPlatformer()
