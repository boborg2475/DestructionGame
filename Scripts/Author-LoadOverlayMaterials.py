"""Author the load overlay's three band materials.

Regenerates /Game/Materials/M_BrickLoadComfortable, M_BrickLoadCaution and
M_BrickLoadCritical -- the translucent unlit overlays an ABrickActor wears when the
Destroy strip's "Load overlay" chip is on, one per band of EJointMarginBand. Needs the
PythonScriptPlugin (enabled in DestructionGame.uproject). Run headless from PowerShell:

  & "C:\\Program Files\\Epic Games\\UE_5.8\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe" `
    "<project>\\DestructionGame.uproject" `
    -ExecutePythonScript="<repo>\\Scripts\\Author-LoadOverlayMaterials.py" `
    -unattended -nopause -nosplash -NoSound

THE SAME SHAPE AS THE HIGHLIGHT FAMILY, MEASURED RATHER THAN GUESSED. M_BrickHover,
M_BrickSelected, M_BrickInspected and M_BrickNeighbour0..5 are each a TRANSLUCENT, UNLIT
material with one Constant3Vector into Emissive Colour and one scalar Constant into
Opacity, and these three are built the same way -- an overlay has to sit ON a lit brick
rather than replace its surface, and Nanite is off on the brick mesh for exactly this.

THE OPACITY IS THE HOVER'S 0.35 AND NOT THE OTHERS' 0.95, AND THAT IS THE ONE DELIBERATE
DIFFERENCE. Hover, selection and the neighbour hues call out ONE brick or six; the load
overlay covers EVERY live piece at once, so at 0.95 the bond disappears under a sheet of
flat colour and the player can no longer see the wall they are reading.

THE COLOURS ARE NOT PICKED HERE. They are DestructionContent::BrickLoadSwatchColours in
Source/DestructionGame/RequiredContent.h, which is also what the details window paints its
headroom bars with -- a brick tinted one green beside a bar drawn another is two answers to
one question. Transcribed rather than imported because Python cannot read a C++ header;
change them THERE first, then re-run this. A Content.* agreement test in the shape of
Content.NeighbourSwatchesMatchTheirMaterials is owed and is logged in CURRENT_STATE.md.

The assets are committed, so this only needs re-running to change a colour.
Content.RequiredAssetsResolve fails if any of the three goes missing.
"""

import unreal

# The three bands' emissive colours, LINEAR, index for index with RequiredContent.h's
# BrickLoadSwatchColours (which is ordered by EJointMarginBand: Critical, Caution,
# Comfortable). Named here rather than indexed so the asset and its colour read as one row.
BAND_MATERIALS = [
    ("M_BrickLoadComfortable", unreal.LinearColor(0.18, 0.76, 0.55, 1.0)),
    ("M_BrickLoadCaution", unreal.LinearColor(0.95, 0.66, 0.13, 1.0)),
    ("M_BrickLoadCritical", unreal.LinearColor(0.95, 0.24, 0.20, 1.0)),
]

# How strongly the tint sits over the brick. See the module docstring: the load overlay is
# on every piece at once, so it is the hover's light 0.35 rather than the selection's 0.95.
OVERLAY_OPACITY = 0.35


def make_overlay_material(name, emissive_color):
    pkg_path = "/Game/Materials"
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    existing = "%s/%s" % (pkg_path, name)
    if unreal.EditorAssetLibrary.does_asset_exist(existing):
        unreal.EditorAssetLibrary.delete_asset(existing)

    mat = tools.create_asset(name, pkg_path, unreal.Material, unreal.MaterialFactoryNew())
    if mat is None:
        unreal.log_error("FAILED to create %s" % name)
        return

    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)

    c3v = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionConstant3Vector, -350, 0)
    c3v.set_editor_property("constant", emissive_color)

    unreal.MaterialEditingLibrary.connect_material_property(
        c3v, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    opacity = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionConstant, -350, 150)
    opacity.set_editor_property("r", OVERLAY_OPACITY)

    unreal.MaterialEditingLibrary.connect_material_property(
        opacity, "", unreal.MaterialProperty.MP_OPACITY)

    unreal.MaterialEditingLibrary.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    unreal.log("CREATED %s" % mat.get_path_name())


for material_name, colour in BAND_MATERIALS:
    make_overlay_material(material_name, colour)

unreal.log("LOAD OVERLAY MATERIAL SCRIPT DONE")
