"""
Bakes another RigExec character into the example project, beside the biped:

    RXC_<Name>   the .rigexec, its controls and picker, and the meshes,
                 materials and their images read from the stage

and makes sure the plugin's surface material takes a colour image
(BaseColorTexture, white by default, so untextured looks are unchanged).

Run in the editor (or as a startup script, see setup_character.bat):

    RIGEXEC_CHARACTER=<Name>            RigExecData/<Name>.rigexec, <Name>_controls.json
    RIGEXEC_CHARACTER_STAGE=<stage>     the USD stage the meshes come from
    RIGEXEC_CHARACTER_FOLDER=/Game/...  where the asset goes (default /Game/RigExec/<Name>)

The game finds every baked character by itself and lets the player choose.
"""

import os

import unreal

SURFACE = "/RigExec/M_RigExecSurface"
WHITE = "/Engine/EngineResources/WhiteSquareTexture"


def EnsureSurfaceTexture():
    material = unreal.load_asset(SURFACE)
    lib = unreal.MaterialEditingLibrary
    for node in lib.get_material_expressions(material) if hasattr(lib, "get_material_expressions") else []:
        if isinstance(node, unreal.MaterialExpressionTextureSampleParameter2D) and \
                str(node.get_editor_property("parameter_name")) == "BaseColorTexture":
            unreal.log("RigExec: %s already takes BaseColorTexture" % SURFACE)
            return
    source = lib.get_material_property_input_node(material, unreal.MaterialProperty.MP_BASE_COLOR)
    if source is None:
        raise RuntimeError("%s has nothing on Base Color" % SURFACE)
    output = lib.get_material_property_input_node_output_name(material, unreal.MaterialProperty.MP_BASE_COLOR) \
        if hasattr(lib, "get_material_property_input_node_output_name") else ""
    pos = source.get_editor_property("material_expression_editor_x"), \
        source.get_editor_property("material_expression_editor_y")
    image = lib.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D,
                                           pos[0] - 320, pos[1] + 260)
    image.set_editor_property("parameter_name", "BaseColorTexture")
    image.set_editor_property("texture", unreal.load_asset(WHITE))
    image.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    tint = lib.create_material_expression(material, unreal.MaterialExpressionMultiply, pos[0] + 200, pos[1])
    lib.connect_material_expressions(source, output or "", tint, "A")
    lib.connect_material_expressions(image, "RGB", tint, "B")
    lib.connect_material_property(tint, "", unreal.MaterialProperty.MP_BASE_COLOR)
    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("RigExec: %s now takes BaseColorTexture" % SURFACE)


FLAT = "/Engine/EngineMaterials/DefaultNormal"


def RemoveSurfaceNormal():
    """No normal input on the surface material: the deformed meshes carry no
    tangents, so any tangent-space normal (even a flat one) shades as noise
    along the UVs. Normal maps wait for tangents on the deformed meshes."""
    material = unreal.load_asset(SURFACE)
    lib = unreal.MaterialEditingLibrary
    node = lib.get_material_property_input_node(material, unreal.MaterialProperty.MP_NORMAL)
    if node is None:
        return
    lib.delete_material_expression(material, node)
    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("RigExec: %s takes no normal map (needs tangents)" % SURFACE)


def EnsureSurfaceNormal():
    """NormalTexture on the surface material: a flat normal by default."""
    material = unreal.load_asset(SURFACE)
    lib = unreal.MaterialEditingLibrary
    if "NormalTexture" in [str(n) for n in lib.get_texture_parameter_names(material)]:
        unreal.log("RigExec: %s already takes NormalTexture" % SURFACE)
        return
    image = lib.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -700, 500)
    image.set_editor_property("parameter_name", "NormalTexture")
    image.set_editor_property("texture", unreal.load_asset(FLAT))
    image.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    lib.connect_material_property(image, "RGB", unreal.MaterialProperty.MP_NORMAL)
    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("RigExec: %s now takes NormalTexture" % SURFACE)


def Bake(name, stage, folder):
    # Relative to the project, as BakeCharacter reads them.
    rigexec = "RigExecData/%s.rigexec" % name
    controls = "RigExecData/%s_controls.json" % name
    asset = "%s/RXC_%s" % (folder, name)
    character = unreal.RigExecRuntimeLibrary.bake_character(rigexec, stage, controls, asset)
    if character is None:
        raise RuntimeError("BakeCharacter failed for %s; see the log" % name)
    unreal.log("RigExec: baked %s (%d meshes, %d touch regions, %d images)" % (
        asset, character.get_editor_property("mesh_count"),
        character.get_editor_property("touch_region_count"),
        len(character.get_editor_property("textures"))))


def Main():
    name = os.environ.get("RIGEXEC_CHARACTER")
    stage = os.environ.get("RIGEXEC_CHARACTER_STAGE")
    if not name or not stage:
        raise RuntimeError("set RIGEXEC_CHARACTER and RIGEXEC_CHARACTER_STAGE")
    folder = os.environ.get("RIGEXEC_CHARACTER_FOLDER", "/Game/RigExec/" + name)
    EnsureSurfaceTexture()
    RemoveSurfaceNormal()
    Bake(name, stage, folder)


try:
    Main()
    unreal.log("RIGEXEC_SETUP_OK")
except Exception as error:
    unreal.log_error("RIGEXEC_SETUP_FAILED: %s" % error)
    raise
finally:
    if os.environ.get("RIGEXEC_QUIT"):
        unreal.SystemLibrary.quit_editor()
