// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Math/Color.h"

/**
 * Every content path this module hard-references from C++.
 *
 * Why a table at all: ConstructorHelpers::FObjectFinder resolves a path at construction, not
 * compile time, so a deleted or renamed asset leaves the reference null with nothing saying
 * so — the pawn stops responding, or a brick spawns with no mesh. A table turns that silent
 * break into a red test (CURRENT_STATE.md has the history).
 *
 * Adding a hard reference is adding a row, the same shape as the profile libraries in
 * Core/Profiles: Tests/RequiredContentTest.cpp's sweep picks it up for free, and cross-checks
 * the other direction too, so a reference resolved on a CDO not in this table fails as well.
 *
 * The constructors use these named constants, not literals of their own, so there is no
 * second copy of a path anywhere in the module for the cross-check to disagree with.
 */
namespace DestructionContent
{
	/* The four input actions ADestructionGameFlyingPawn binds. */
	inline constexpr const TCHAR* MoveActionPath = TEXT("/Game/Input/Actions/IA_Move.IA_Move");
	inline constexpr const TCHAR* LookActionPath = TEXT("/Game/Input/Actions/IA_Look.IA_Look");
	inline constexpr const TCHAR* MouseLookActionPath = TEXT("/Game/Input/Actions/IA_MouseLook.IA_MouseLook");
	inline constexpr const TCHAR* AscendActionPath = TEXT("/Game/Input/Actions/IA_Jump.IA_Jump");

	/**
	 * The input action that opens the piece context menu, bound on the player controller.
	 *
	 * On the controller, not the pawn: it already owns the mapping contexts, outlives any pawn,
	 * and carries the cursor and deprojection this action needs.
	 */
	inline constexpr const TCHAR* InspectPieceActionPath =
		TEXT("/Game/Input/Actions/IA_InspectPiece.IA_InspectPiece");

	/**
	 * The input action that keeps the highlight following the cursor, bound beside IA_InspectPiece.
	 *
	 * A second action rather than IA_MouseLook, though both read the same Mouse2D axis: IMC_MouseLook
	 * is now chorded to IA_LookModifier below, so what keeps the two apart is the chord — hover
	 * must fire while no button is held, and IA_MouseLook by definition does not.
	 * Content.SessionInput.LookNeedsTheModifierHeld pins that the chord stays off this one.
	 */
	inline constexpr const TCHAR* HoverPieceActionPath =
		TEXT("/Game/Input/Actions/IA_HoverPiece.IA_HoverPiece");

	/**
	 * The one action nothing binds, and the reason the camera can be pointed at all.
	 *
	 * IA_LookModifier has no handler and must not get one: it exists only for IMC_MouseLook's
	 * Chorded Action trigger to watch, gating look on a held right mouse button so the cursor
	 * has something to hold off — free-look used to read the raw Mouse2D axis unconditionally.
	 *
	 * Resolved onto no CDO, so the table row matters more here than anywhere: every other path
	 * is also held by a constructor and a missing asset shows up twice, but this one is
	 * referenced only by IMC_Session and IMC_MouseLook's chord — only the sweep would notice it gone.
	 */
	inline constexpr const TCHAR* LookModifierActionPath =
		TEXT("/Game/Input/Actions/IA_LookModifier.IA_LookModifier");

	/**
	 * The eight session shortcuts, one action per control the toolbar draws.
	 *
	 * One action per chip rather than per key: a keyboard shortcut in this design IS a toolbar
	 * click, each bound to a single OnToolbarButton dispatch, so the model's refusals apply to
	 * the keyboard exactly as to the strip. Two stand for a pair of buttons — Tab toggles the
	 * mode tabs, G toggles Snap/Free — which is why those two go through ToggleSessionMode and
	 * ToggleSessionPlacement instead of straight at the door.
	 *
	 * Pinned by Content.SessionInput.SessionContextMapsTheShortcuts; that none collides with a
	 * key IMC_Default already flies the pawn with is Content.SessionInput.SessionKeysAreFree.
	 */
	inline constexpr const TCHAR* SessionToggleModeActionPath =
		TEXT("/Game/Input/Actions/IA_SessionToggleMode.IA_SessionToggleMode");
	inline constexpr const TCHAR* SessionPieceBrickActionPath =
		TEXT("/Game/Input/Actions/IA_SessionPieceBrick.IA_SessionPieceBrick");
	inline constexpr const TCHAR* SessionPiecePlateActionPath =
		TEXT("/Game/Input/Actions/IA_SessionPiecePlate.IA_SessionPiecePlate");
	inline constexpr const TCHAR* SessionPieceLintelActionPath =
		TEXT("/Game/Input/Actions/IA_SessionPieceLintel.IA_SessionPieceLintel");
	inline constexpr const TCHAR* SessionSnapToggleActionPath =
		TEXT("/Game/Input/Actions/IA_SessionSnapToggle.IA_SessionSnapToggle");
	inline constexpr const TCHAR* SessionCourseUpActionPath =
		TEXT("/Game/Input/Actions/IA_SessionCourseUp.IA_SessionCourseUp");
	inline constexpr const TCHAR* SessionCourseDownActionPath =
		TEXT("/Game/Input/Actions/IA_SessionCourseDown.IA_SessionCourseDown");
	inline constexpr const TCHAR* SessionRunActionPath =
		TEXT("/Game/Input/Actions/IA_SessionRun.IA_SessionRun");

	/**
	 * What a called-out brick wears, one asset per state that is not None.
	 *
	 * Three assets rather than one: the four highlight states must be four distinguishable
	 * looks or the enum is decoration — a player must be able to check which bricks are going
	 * before pressing Delete, and a hovered brick drawn like a selected one hides that. They
	 * are overlays, so a brick keeps its own material underneath.
	 *
	 * The inspected one is the strongest, deliberately: it marks the single brick whose joint
	 * forces are on screen, so it must be tellable apart from other picked bricks.
	 */
	inline constexpr const TCHAR* BrickHoverMaterialPath =
		TEXT("/Game/Materials/M_BrickHover.M_BrickHover");
	inline constexpr const TCHAR* BrickSelectedMaterialPath =
		TEXT("/Game/Materials/M_BrickSelected.M_BrickSelected");
	inline constexpr const TCHAR* BrickInspectedMaterialPath =
		TEXT("/Game/Materials/M_BrickInspected.M_BrickInspected");

	/**
	 * What a piece wears under the load overlay, one asset per band of EJointMarginBand.
	 *
	 * Three assets because there are three bands, and the bands are the model's: the presenter
	 * already decides which side of 10x and 2x margin a joint sits on (SESSION_UI_DESIGN §a
	 * principle 6, "the colour of a thing is the model's decision; the hue is the widget's").
	 *
	 * The same kind of overlay as M_BrickHover, which matters more here: it covers every live
	 * piece at once, so anything opaque or replacing the brick's own material would hide the
	 * bond or repaint the whole wall.
	 *
	 * Green, amber, red — the headroom bar's own three, so the overlay and the panel beside it
	 * never disagree. The colours are the array directly below, which the panel now reads too.
	 */
	inline constexpr const TCHAR* BrickLoadComfortableMaterialPath =
		TEXT("/Game/Materials/M_BrickLoadComfortable.M_BrickLoadComfortable");
	inline constexpr const TCHAR* BrickLoadCautionMaterialPath =
		TEXT("/Game/Materials/M_BrickLoadCaution.M_BrickLoadCaution");
	inline constexpr const TCHAR* BrickLoadCriticalMaterialPath =
		TEXT("/Game/Materials/M_BrickLoadCritical.M_BrickLoadCritical");

	/**
	 * And what each band is painted in — the emissive constant of the three materials above, the
	 * fill of the details window's headroom bar, and the dot beside a falling brick, all one home.
	 * Beside the paths so a fourth band arriving with a path and no colour is visible to the
	 * person adding it (see BrickNeighbourSwatchColours below for why that matters in practice).
	 *
	 * Indexed by EJointMarginBand's own enumerators — Critical zero, Caution one, Comfortable two
	 * — so a call site needs no switch to turn the model's number back into a colour. The enum
	 * lives in Core/PieceMenu.h and this header deliberately does not include it: this is a
	 * table of content, not presentation logic.
	 *
	 * Equality here is not equality on screen — see BrickNeighbourSwatchColours.
	 */
	inline constexpr FLinearColor BrickLoadSwatchColours[] = {
		FLinearColor(0.95f, 0.24f, 0.20f, 1.0f),
		FLinearColor(0.95f, 0.66f, 0.13f, 1.0f),
		FLinearColor(0.18f, 0.76f, 0.55f, 1.0f)
	};

	/**
	 * One material per colour slot of the joint readout, so a row of numbers and a brick in the
	 * world are the same colour.
	 *
	 * Six, the model's number rather than a round one: a brick inside a running bond has six
	 * joints, and FInspectorJointRow::ColourSlot hands out slot i to row i until it runs out.
	 * An array rather than six named constants, since these are indexed by a slot number the
	 * model computed, and a name per slot would need a switch at every call site.
	 */
	inline constexpr const TCHAR* BrickNeighbourMaterialPaths[] = {
		TEXT("/Game/Materials/M_BrickNeighbour0.M_BrickNeighbour0"),
		TEXT("/Game/Materials/M_BrickNeighbour1.M_BrickNeighbour1"),
		TEXT("/Game/Materials/M_BrickNeighbour2.M_BrickNeighbour2"),
		TEXT("/Game/Materials/M_BrickNeighbour3.M_BrickNeighbour3"),
		TEXT("/Game/Materials/M_BrickNeighbour4.M_BrickNeighbour4"),
		TEXT("/Game/Materials/M_BrickNeighbour5.M_BrickNeighbour5")
	};

	/**
	 * And what the panel paints slot i's swatch in — beside the path rather than inside the widget.
	 *
	 * These are the emissive constants of the six materials above, index for index: a joint row
	 * draws this colour, the brick on the far end wears the material on the same row, and the
	 * player finds one from the other. A swatch that disagrees with its material points the
	 * player at the wrong brick in a wall of 1,220 identical ones.
	 *
	 * Beside the paths because the two arrays are one row per slot — they used to live as a
	 * file-static palette in DestructionGamePlayerController.cpp whose comment claimed to be
	 * "exactly" these emissives, and a palette repick changed three of the assets without it.
	 * Adjacency is not a proof, but it makes a seventh slot arriving with a path and no colour
	 * visible to whoever adds it.
	 *
	 * This duplication cannot be deleted from C++ alone: the shader needs the colour in the asset
	 * and the swatch needs it at runtime, and a Constant3Vector is editor-only data, unreadable
	 * in a cooked build. Re-authoring each emissive as a named VectorParameter would let the
	 * swatch read GetVectorParameterValue and remove this array entirely; until then the
	 * agreement is asserted rather than structural — Content.NeighbourSwatchesMatchTheirMaterials.
	 *
	 * Equality here is not equality on screen, and nobody may "fix" one of these to match a
	 * screenshot: the swatch is a flat Slate fill, the brick an additive overlay over a lit
	 * surface, so the same linear triple lands at a different display value on each. The two are
	 * meant to be the same decision, not the same pixel.
	 */
	inline constexpr FLinearColor BrickNeighbourSwatchColours[] = {
		FLinearColor(0.00f, 0.80f, 0.00f, 1.0f),
		FLinearColor(0.15f, 0.25f, 1.00f, 1.0f),
		FLinearColor(0.45f, 0.10f, 0.00f, 1.0f),
		FLinearColor(1.00f, 0.00f, 0.12f, 1.0f),
		FLinearColor(0.72f, 0.55f, 1.00f, 1.0f),
		FLinearColor(0.20f, 0.45f, 0.05f, 1.0f)
	};

	/**
	 * What a brick wears as its base colour, one asset per structural material the shed is made of.
	 *
	 * The base material (element 0), not an overlay: the highlight assets above sit on top of a
	 * brick so it keeps its own look underneath, and these ARE that look. A brick whose material
	 * maps to neither keeps the mesh's grey default, hence only these two are named.
	 */
	inline constexpr const TCHAR* ShedBrickMaterialPath =
		TEXT("/Game/Materials/M_Shed_Brick.M_Shed_Brick");
	inline constexpr const TCHAR* ShedTimberMaterialPath =
		TEXT("/Game/Materials/M_Shed_Timber.M_Shed_Timber");

	/* The three mapping contexts ADestructionGamePlayerController adds for a local player. */
	inline constexpr const TCHAR* DefaultMappingContextPath = TEXT("/Game/Input/IMC_Default.IMC_Default");
	inline constexpr const TCHAR* MouseLookMappingContextPath = TEXT("/Game/Input/IMC_MouseLook.IMC_MouseLook");

	/**
	 * The session's own keyboard, applied alongside the other two for the whole session.
	 *
	 * A third context rather than nine more mappings in IMC_Default: these are the session's
	 * keys, decided by the toolbar model, and keeping them in one asset lets
	 * Content.SessionInput.SessionKeysAreFree diff them against the flying pawn's. bConsumeInput
	 * defaults to true, so a collision would withhold the key rather than double it up — the
	 * pawn would silently stop answering.
	 */
	inline constexpr const TCHAR* SessionMappingContextPath = TEXT("/Game/Input/IMC_Session.IMC_Session");

	/**
	 * ABrickActor's placeholder mesh, and the one reference reflection cannot see: it is
	 * set onto a mesh component's own asset pointer rather than a UPROPERTY of the actor, so
	 * Tests/RequiredContentTest.cpp reads it back by hand.
	 */
	inline constexpr const TCHAR* BrickPlaceholderMeshPath = TEXT("/Game/LevelPrototyping/Meshes/SM_Cube.SM_Cube");

	TArrayView<const TCHAR* const> RequiredContentPaths();
}
