// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Math/Color.h"

/**
 * Every content path this module hard-references from C++.
 *
 * WHY A TABLE AT ALL. ConstructorHelpers::FObjectFinder resolves a path AT CONSTRUCTION,
 * not at compile time, so deleting or renaming an asset leaves the reference null and
 * nothing says so — the pawn simply stops responding to input, or a brick spawns with no
 * mesh. CURRENT_STATE.md records that hazard against ADestructionGameFlyingPawn,
 * ADestructionGamePlayerController and ABrickActor. A table turns "content deletion
 * silently breaks the game" into "a test goes red", which is the whole point of it.
 *
 * ADDING A HARD REFERENCE IS ADDING A ROW, the same shape as the profile libraries in
 * Core/Profiles: the sweep in Tests/RequiredContentTest.cpp picks a new row up for free,
 * and cross-checks in the other direction too, so a reference resolved on a CDO that is
 * NOT in this table fails as well.
 *
 * THE PATHS ARE NAMED CONSTANTS AND THE CONSTRUCTORS USE THESE, NOT LITERALS OF THEIR
 * OWN. A table of paths that all load is worth nothing if it is not the list the game
 * actually resolves — the test's cross-check is what catches that drift after the fact,
 * and one spelling of each path is what stops it happening. There is no second copy of
 * these strings anywhere in the module.
 */
namespace DestructionContent
{
	/* The four input actions ADestructionGameFlyingPawn binds. */
	inline constexpr const TCHAR* MoveActionPath = TEXT("/Game/Input/Actions/IA_Move.IA_Move");
	inline constexpr const TCHAR* LookActionPath = TEXT("/Game/Input/Actions/IA_Look.IA_Look");
	inline constexpr const TCHAR* MouseLookActionPath = TEXT("/Game/Input/Actions/IA_MouseLook.IA_MouseLook");
	inline constexpr const TCHAR* AscendActionPath = TEXT("/Game/Input/Actions/IA_Jump.IA_Jump");

	/**
	 * The input action that opens the piece context menu, bound on the PLAYER CONTROLLER.
	 *
	 * ON THE CONTROLLER RATHER THAN THE PAWN, deliberately: the controller already owns the
	 * mapping contexts, it outlives any pawn, and it is what carries the cursor and the
	 * deprojection this action needs. A pawn-side binding would go away with the pawn.
	 */
	inline constexpr const TCHAR* InspectPieceActionPath =
		TEXT("/Game/Input/Actions/IA_InspectPiece.IA_InspectPiece");

	/**
	 * The input action that keeps the highlight following the cursor, bound on the CONTROLLER
	 * beside IA_InspectPiece.
	 *
	 * A SECOND ACTION RATHER THAN IA_MouseLook, EVEN THOUGH BOTH READ THE SAME Mouse2D AXIS.
	 * IA_MouseLook lived in a context the piece menu REMOVED for as long as it was up, so hover
	 * hung off it would have stopped updating at exactly the moment the cursor appeared and the
	 * player started moving it over bricks. Nothing removes a context any more — IMC_MouseLook's
	 * mapping is chorded to IA_LookModifier below, which is what made the removal unnecessary —
	 * so what keeps the two actions apart now is the CHORD: hover must fire while no button is
	 * held, and IA_MouseLook by definition does not. Folding them back together is logged in
	 * CURRENT_STATE as a later content change, and Content.SessionInput.LookNeedsTheModifierHeld
	 * is what insists the chord has not been copied onto this one in the meantime.
	 */
	inline constexpr const TCHAR* HoverPieceActionPath =
		TEXT("/Game/Input/Actions/IA_HoverPiece.IA_HoverPiece");

	/**
	 * THE ONE ACTION NOTHING BINDS, AND THE REASON THE CAMERA CAN BE POINTED AT ALL.
	 *
	 * IA_LookModifier has no handler and must not get one. It exists to be the action
	 * IMC_MouseLook's Chorded Action trigger WATCHES: free-look used to read the raw Mouse2D
	 * axis with no held button, so the camera followed the mouse all the time and the only
	 * cursor this game had came from REMOVING that whole context while a piece menu was up —
	 * which a toolbar that is on screen for the whole session cannot live with. Gating look on
	 * a held right mouse button leaves a panel nothing to take away.
	 *
	 * IT IS RESOLVED ONTO NO CDO, WHICH IS WHY THE TABLE ROW MATTERS MORE HERE THAN ANYWHERE.
	 * Every other path below is also held by a constructor, so a missing asset shows up twice.
	 * This one is referenced only by the IMC_Session mapping and by IMC_MouseLook's chord, so
	 * the table sweep is the only thing that would say it had gone.
	 */
	inline constexpr const TCHAR* LookModifierActionPath =
		TEXT("/Game/Input/Actions/IA_LookModifier.IA_LookModifier");

	/**
	 * THE EIGHT SESSION SHORTCUTS, ONE ACTION PER CONTROL THE TOOLBAR DRAWS.
	 *
	 * ONE ACTION PER CHIP RATHER THAN ONE PER KEY, because a keyboard shortcut in this design
	 * IS a toolbar click: every one of these is bound to a single OnToolbarButton dispatch, so
	 * the model's refusals apply to the keyboard exactly as they apply to the strip. Two of them
	 * stand for a PAIR of buttons — Tab toggles the mode tabs and G toggles Snap/Free — which is
	 * a read of the current state rather than a second id, and is why those two go through
	 * ToggleSessionMode and ToggleSessionPlacement instead of straight at the door.
	 *
	 * The keys themselves live in IMC_Session and are pinned by
	 * Content.SessionInput.SessionContextMapsTheShortcuts; that none of them is a key IMC_Default
	 * already flies the pawn with is the separate claim of Content.SessionInput.SessionKeysAreFree.
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
	 * THREE ASSETS RATHER THAN ONE, because the four highlight states have to be four
	 * DISTINGUISHABLE looks or the enum is decoration: the one thing a player must be able to
	 * check before pressing Delete is which bricks are going, and a hovered brick that draws
	 * like a selected one takes that away. They are overlays, so a brick keeps its own material
	 * underneath and nothing has to remember what it replaced.
	 *
	 * AND THE INSPECTED ONE IS THE STRONGEST OF THE THREE, deliberately. It marks the single
	 * brick whose joint forces are on screen, so it has to be tellable apart from the other
	 * bricks the player also picked — a breakout drawn beside five bricks that look identical
	 * to its subject is ambiguous about which brick it is the breakout OF.
	 */
	inline constexpr const TCHAR* BrickHoverMaterialPath =
		TEXT("/Game/Materials/M_BrickHover.M_BrickHover");
	inline constexpr const TCHAR* BrickSelectedMaterialPath =
		TEXT("/Game/Materials/M_BrickSelected.M_BrickSelected");
	inline constexpr const TCHAR* BrickInspectedMaterialPath =
		TEXT("/Game/Materials/M_BrickInspected.M_BrickInspected");

	/**
	 * ONE MATERIAL PER COLOUR SLOT OF THE JOINT READOUT, so a row of numbers and a brick in the
	 * world are the same colour.
	 *
	 * SIX, WHICH IS THE MODEL'S NUMBER RATHER THAN A ROUND ONE: a brick inside a running bond has
	 * six joints, and FInspectorJointRow::ColourSlot hands out slot i to row i until it runs out.
	 * The emissive constants are the same six the panel paints its swatches in, so the pairing is
	 * by construction rather than by somebody picking amber twice.
	 *
	 * AN ARRAY RATHER THAN SIX NAMED CONSTANTS, and it is the one place that shape is right: these
	 * are indexed BY A SLOT NUMBER the model computed, so a name per slot would need a switch at
	 * every call site to turn the number back into the name.
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
	 * AND WHAT THE PANEL PAINTS SLOT i'S SWATCH IN — BESIDE THE PATH RATHER THAN INSIDE THE WIDGET.
	 *
	 * THESE ARE THE EMISSIVE CONSTANTS OF THE SIX MATERIALS DIRECTLY ABOVE, INDEX FOR INDEX, and
	 * that agreement is the entire feature: a joint row draws this colour, the brick on the far end
	 * of that joint wears the material on the same row, and the player finds one from the other. A
	 * swatch that disagrees with its material is not a cosmetic fault — it points the player at the
	 * wrong brick in a wall of 1,220 identical ones, which is worse than drawing no swatch at all.
	 *
	 * BESIDE THE PATHS BECAUSE THE TWO ARRAYS ARE ONE ROW PER SLOT. They lived in
	 * DestructionGamePlayerController.cpp as a file-static palette whose comment claimed to be
	 * "exactly" these emissives, and a palette repick changed three of the assets without it —
	 * amber, chartreuse and teal against a green, a clay and a sage, drawn side by side, with the
	 * whole suite green because nothing held them together. Adjacency is not a proof, but it is what
	 * makes a seventh slot arriving with a path and no colour visible to the person adding it.
	 *
	 * THIS DUPLICATION CANNOT BE DELETED FROM C++ ALONE, AND SAYING SO IS THE HONEST THING TO DO.
	 * The shader needs the colour in the asset and the swatch needs it at runtime; a Constant3Vector
	 * is editor-only data, so nothing can read it in a cooked build. The design that would make the
	 * drift UNSAYABLE is re-authoring each emissive as a named VectorParameter, at which point the
	 * swatch reads GetVectorParameterValue and this array goes away entirely. Until then the
	 * agreement is asserted rather than structural — Content.NeighbourSwatchesMatchTheirMaterials.
	 *
	 * EQUALITY HERE IS NOT EQUALITY ON SCREEN, and nobody may "fix" one of these to match a
	 * screenshot. The swatch is a Slate fill drawn at full strength over the panel's near-black
	 * background; the brick is an ADDITIVE overlay composited over a lit surface, so the same linear
	 * triple lands at a different display value on the brick than it does in the panel. The two are
	 * meant to be the same DECISION, not the same pixel, and matching them by eye would put a third
	 * number in the system rather than removing one.
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
	 * WHAT A BRICK WEARS AS ITS BASE COLOUR, ONE ASSET PER STRUCTURAL MATERIAL THE SHED IS MADE OF.
	 *
	 * THE BASE MATERIAL (element 0), NOT AN OVERLAY. The highlight assets above sit on top of a
	 * brick so it keeps its own look underneath; these ARE that look underneath, so a wall reads
	 * brick-red and a roof timber-tan before a cursor ever crosses either. A brick whose material
	 * maps to neither keeps the mesh's grey default, which is why only the two the shed uses are
	 * named here.
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
	 * A THIRD CONTEXT RATHER THAN NINE MORE MAPPINGS IN IMC_Default, because these are the
	 * SESSION's keys rather than the pawn's: what they do is decided by the toolbar model, they
	 * arrive and change together, and keeping them in one asset is what lets
	 * Content.SessionInput.SessionKeysAreFree diff them against the flying pawn's keys and say
	 * that none of them has been stolen. bConsumeInput defaults to true, so a collision would
	 * not double up — it would WITHHOLD the key and the pawn would silently stop answering.
	 */
	inline constexpr const TCHAR* SessionMappingContextPath = TEXT("/Game/Input/IMC_Session.IMC_Session");

	/**
	 * ABrickActor's placeholder mesh, and the one reference reflection cannot see: it is
	 * set onto a mesh component's own asset pointer rather than being a UPROPERTY of the
	 * actor, so Tests/RequiredContentTest.cpp reads it back by hand.
	 */
	inline constexpr const TCHAR* BrickPlaceholderMeshPath = TEXT("/Game/LevelPrototyping/Meshes/SM_Cube.SM_Cube");

	TArrayView<const TCHAR* const> RequiredContentPaths();
}
