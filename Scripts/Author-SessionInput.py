"""Author the build/destroy session's input assets.

Regenerates the nine session input actions under /Game/Input/Actions, the
/Game/Input/IMC_Session mapping context that reaches them, and the Chorded Action
trigger that puts IMC_MouseLook's free-look behind a held right mouse button
(SESSION_UI_DESIGN.md (d) "Cursor and camera", slice S6). Needs the
PythonScriptPlugin (enabled in DestructionGame.uproject). Close the editor, then
run headless from a git-bash / cmd / PowerShell shell:

  "C:\\Program Files\\Epic Games\\UE_5.8\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe" \\
    "<project>\\DestructionGame.uproject" \\
    -ExecutePythonScript="<repo>\\Scripts\\Author-SessionInput.py" \\
    -unattended -nopause -nosplash -NoSound

The assets are committed, so this only needs re-running to add a shortcut or to
change a key. What keeps them honest afterwards is the test suite, not this file:
Content.SessionInput.SessionContextMapsTheShortcuts pins every action, its value
type and its key; Content.SessionInput.SessionKeysAreFree pins that none of those
keys is one IMC_Default already uses; Content.SessionInput.LookNeedsTheModifierHeld
pins the chord and that IA_HoverPiece did NOT catch one; and
Content.RequiredAssetsResolve pins that the controller and the required-content
table name exactly these paths.

DELETE-THEN-CREATE FOR THE TEN NEW ASSETS, MODIFY-IN-PLACE FOR IMC_MouseLook.
Scripts/Author-ShedMaterials.py established the first half: re-authoring an asset
from scratch is what makes a second run mean the same thing as the first.
IMC_MouseLook is not ours to re-author -- it is hand-authored content that predates
this slice and carries one mapping we only want to add a trigger to -- so it is
loaded, edited and saved. A consequence worth knowing on a RE-RUN: deleting
IA_LookModifier while IMC_MouseLook's chord still points at it leaves that
reference null for the length of the run, and the re-point at the end is what
repairs it. Running this script halfway is therefore not a safe state; run it to
completion, and the LookNeedsTheModifierHeld test is what says it got there.
"""

import unreal


ACTIONS_PATH = "/Game/Input/Actions"
INPUT_PATH = "/Game/Input"

# One row per shortcut SESSION_UI_DESIGN (b) draws on a chip, plus the look modifier
# that is not a shortcut at all -- it exists only to be the action IMC_MouseLook's
# chord watches. The key names are FKey names, which is what unreal.Key wants.
SESSION_ACTIONS = [
    ("IA_LookModifier", "RightMouseButton", "hold to look around"),
    ("IA_SessionToggleMode", "Tab", "toggle Build/Destroy"),
    ("IA_SessionPieceBrick", "One", "palette: brick"),
    ("IA_SessionPiecePlate", "Two", "palette: timber plate"),
    ("IA_SessionPieceLintel", "Three", "palette: timber lintel"),
    ("IA_SessionSnapToggle", "G", "toggle Snap/Free"),
    ("IA_SessionCourseUp", "RightBracket", "course up"),
    ("IA_SessionCourseDown", "LeftBracket", "course down"),
    ("IA_SessionRun", "Enter", "run the structure"),
]

WRITTEN = []


def factory_for(class_name, asset_class):
    """The engine's own factory for this asset class, or a DataAssetFactory standing in.

    UInputActionFactory and UInputMappingContextFactory live in an editor-only
    Enhanced Input module and are not guaranteed to be reflected into Python. Both
    asset classes derive from UDataAsset, so unreal.DataAssetFactory with its
    data_asset_class set produces the same asset by a route that is always there.
    """
    factory_class = getattr(unreal, class_name, None)

    if factory_class is not None:
        return factory_class()

    unreal.log("unreal.%s is not exposed; falling back to DataAssetFactory" % class_name)

    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", asset_class)

    return factory


def make_asset(name, package_path, asset_class, factory_class_name):
    """Delete whatever is there and author it again, so a re-run means what a first run does.

    WITH A RE-AUTHOR-IN-PLACE FALLBACK, BECAUSE THE DELETE DOES NOT ALWAYS LAND IN TIME.
    EditorAssetLibrary.delete_asset force-deletes the object, but the package can still
    be known to the asset registry on the very next call -- at which point CanCreateAsset
    refuses ("already exists in package") and, running unattended, cannot ask. Every
    property these assets carry is written wholesale below, so editing the survivor is
    the same asset either way; what is NOT the same is failing, so the fallback is taken
    and said out loud rather than left as a silent difference between two runs.
    """
    full_path = "%s/%s" % (package_path, name)

    if unreal.EditorAssetLibrary.does_asset_exist(full_path):
        unreal.EditorAssetLibrary.delete_asset(full_path)

        unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(
            [package_path], True)

    tools = unreal.AssetToolsHelpers.get_asset_tools()

    if not unreal.EditorAssetLibrary.does_asset_exist(full_path):
        asset = tools.create_asset(
            name, package_path, asset_class, factory_for(factory_class_name, asset_class))

        if asset is not None:
            return asset

    asset = unreal.EditorAssetLibrary.load_asset(full_path)

    if asset is None:
        unreal.log_error("FAILED to create or load %s" % full_path)

        return None

    unreal.log("RE-AUTHORED IN PLACE (the delete had not reached the registry): %s" % full_path)

    return asset


def make_input_action(name):
    action = make_asset(name, ACTIONS_PATH, unreal.InputAction, "InputActionFactory")

    if action is None:
        return None

    # EVERY ONE OF THESE IS A PRESS, so the value type is Boolean and is set rather
    # than left to a default. An axis action actuates on a VALUE, which would fire the
    # controller's handler on frames the player asked for nothing -- and a chord asks
    # "is that action triggering", which is a yes/no question.
    action.set_editor_property("value_type", unreal.InputActionValueType.BOOLEAN)

    unreal.EditorAssetLibrary.save_loaded_asset(action)

    WRITTEN.append(action.get_path_name())

    return action


def mapping(action, key_name):
    # unreal.Key TAKES NO CONSTRUCTOR ARGUMENTS in this build -- neither positional nor
    # keyword -- so the FName is set through the property rather than passed in.
    key = unreal.Key()
    key.set_editor_property("key_name", key_name)

    entry = unreal.EnhancedActionKeyMapping()

    entry.set_editor_property("action", action)
    entry.set_editor_property("key", key)

    return entry


def context_mappings(context):
    """The mappings C++ will actually read: DefaultKeyMappings.Mappings, not the old array.

    UE 5.7 DEPRECATED UInputMappingContext::Mappings AND MOVED THE LIVE DATA into the
    DefaultKeyMappings struct, and UInputMappingContext::GetMappings() -- what every
    assertion in Tests/SessionInputAssetsTest.cpp reads -- returns that struct's array.
    Writing the deprecated one instead is the quiet failure this helper exists to
    prevent: the asset saves, the editor shows the mappings, and the game maps no keys.
    """
    return context.get_editor_property("default_key_mappings").get_editor_property("mappings")


def set_context_mappings(context, entries):
    data = unreal.InputMappingContextMappingData()
    data.set_editor_property("mappings", entries)

    context.set_editor_property("default_key_mappings", data)


def describe(context):
    """Every mapping in a context with its triggers, printed, so the run is checkable."""
    lines = []

    for entry in context_mappings(context):
        action = entry.get_editor_property("action")
        key = entry.get_editor_property("key")

        triggers = []

        for trigger in entry.get_editor_property("triggers"):
            chord = getattr(trigger, "chord_action", None)

            triggers.append(
                "%s%s" % (
                    trigger.get_class().get_name(),
                    "(chord=%s)" % chord.get_name() if chord is not None else ""))

        lines.append(
            "  %s = %s%s" % (
                action.get_name() if action is not None else "<none>",
                key.get_editor_property("key_name"),
                " [%s]" % ", ".join(triggers) if triggers else ""))

    return "\n".join(lines) if lines else "  <no mappings>"


actions = {}

for name, _key, _what in SESSION_ACTIONS:
    actions[name] = make_input_action(name)

if any(action is None for action in actions.values()):
    unreal.log_error("SESSION INPUT SCRIPT ABORTED: an input action failed to author")
else:
    # THE SESSION'S OWN CONTEXT, AUTHORED AS ONE ARRAY RATHER THAN NINE map_key CALLS.
    # UInputMappingContext::MapKey is not reflected, and a Python-side struct handed to
    # a setter is a copy either way -- so the mappings are built as structs and the whole
    # array is set, then read back off the saved asset below to prove what landed.
    session = make_asset(
        "IMC_Session", INPUT_PATH, unreal.InputMappingContext, "InputMappingContextFactory")

    if session is None:
        unreal.log_error("SESSION INPUT SCRIPT ABORTED: IMC_Session failed to author")
    else:
        set_context_mappings(
            session, [mapping(actions[name], key) for name, key, _what in SESSION_ACTIONS])

        unreal.EditorAssetLibrary.save_loaded_asset(session)

        WRITTEN.append(session.get_path_name())

    # AND THE CHORD, WHICH IS THE WHOLE OF S6. IMC_MouseLook binds the raw Mouse2D axis
    # with no held button, so the camera follows the mouse all the time and there is no
    # pointer; the only cursor this game has ever had came from REMOVING that context
    # while a menu was up, which a permanent toolbar cannot live with. Gating look on a
    # held RMB leaves nothing for a panel to take away.
    look_context = unreal.EditorAssetLibrary.load_asset(
        "%s/IMC_MouseLook.IMC_MouseLook" % INPUT_PATH)

    if look_context is None:
        unreal.log_error("FAILED to load IMC_MouseLook; the chord was not authored")
    else:
        edited = []

        for entry in context_mappings(look_context):
            # THE TRIGGER IS OUTERED TO THE CONTEXT. Triggers are an Instanced array, so an
            # object created with any other outer is not part of this package and simply is
            # not there the next time the asset is loaded.
            chord = unreal.new_object(unreal.InputTriggerChordAction, outer=look_context)
            chord.set_editor_property("chord_action", actions["IA_LookModifier"])

            entry.set_editor_property("triggers", [chord])

            edited.append(entry)

        set_context_mappings(look_context, edited)

        unreal.EditorAssetLibrary.save_loaded_asset(look_context)

        WRITTEN.append(look_context.get_path_name())

# WHAT LANDED, READ BACK OFF DISK RATHER THAN OFF THE OBJECTS JUST EDITED. A struct set
# through Python is a copy of a copy in several places, so the only honest report is the
# reloaded asset's own mappings.
for path in ("%s/IMC_Session.IMC_Session" % INPUT_PATH,
             "%s/IMC_MouseLook.IMC_MouseLook" % INPUT_PATH):
    reloaded = unreal.EditorAssetLibrary.load_asset(path)

    if reloaded is None:
        unreal.log_error("RELOAD FAILED for %s" % path)
    else:
        unreal.log("%s maps:\n%s" % (path, describe(reloaded)))

for path in WRITTEN:
    unreal.log("WROTE %s" % path)

unreal.log("SESSION INPUT SCRIPT DONE (%d assets)" % len(WRITTEN))
