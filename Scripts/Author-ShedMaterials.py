"""Author the shed's structural-material colour assets.

Regenerates /Game/Materials/M_Shed_Brick and /Game/Materials/M_Shed_Timber --
the constant-colour Default-Lit materials the brick actors wear per structural
material (ClayBrick -> brick-red, Timber -> timber-tan). Needs the PythonScriptPlugin
(enabled in DestructionGame.uproject). Run headless from a git-bash / cmd shell:

  "C:\\Program Files\\Epic Games\\UE_5.8\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe" \\
    "<project>\\DestructionGame.uproject" \\
    -ExecutePythonScript="<repo>\\Scripts\\Author-ShedMaterials.py" \\
    -unattended -nopause -nosplash -NoSound

The assets are committed, so this only needs re-running to change a colour or add
a material. Content.RequiredAssetsResolve fails if either asset goes missing.
"""

import unreal

# Author simple constant-colour, Default-Lit materials for the shed's structural
# materials. Same shape as the project's existing hand-authored M_* constant-colour
# assets: one Constant3Vector driving BaseColor. Colours are LINEAR (roughly the
# sRGB values a clay brick and bare timber read as under a light).

def make_material(name, base_color):
    pkg_path = "/Game/Materials"
    tools = unreal.AssetToolsHelpers.get_asset_tools()

    existing = "%s/%s" % (pkg_path, name)
    if unreal.EditorAssetLibrary.does_asset_exist(existing):
        unreal.EditorAssetLibrary.delete_asset(existing)

    mat = tools.create_asset(name, pkg_path, unreal.Material, unreal.MaterialFactoryNew())
    if mat is None:
        unreal.log_error("FAILED to create %s" % name)
        return

    c3v = unreal.MaterialEditingLibrary.create_material_expression(
        mat, unreal.MaterialExpressionConstant3Vector, -350, 0)
    c3v.set_editor_property("constant", base_color)

    unreal.MaterialEditingLibrary.connect_material_property(
        c3v, "", unreal.MaterialProperty.MP_BASE_COLOR)

    unreal.MaterialEditingLibrary.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    unreal.log("CREATED %s" % mat.get_path_name())


make_material("M_Shed_Brick", unreal.LinearColor(0.35, 0.06, 0.04, 1.0))
make_material("M_Shed_Timber", unreal.LinearColor(0.45, 0.22, 0.09, 1.0))

unreal.log("SHED MATERIAL SCRIPT DONE")
