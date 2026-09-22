// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Math/Color.h"

/**
 * Every content path this module hard-references from C++. FObjectFinder resolves at runtime, so
 * a renamed asset silently becomes null; RequiredContentTest sweeps this table and also fails on
 * a CDO reference missing from it. Constructors use these constants, never their own literals.
 */
namespace DestructionContent
{
	// The four input actions ADestructionGameFlyingPawn binds.
	inline constexpr const TCHAR* MoveActionPath = TEXT("/Game/Input/Actions/IA_Move.IA_Move");
	inline constexpr const TCHAR* LookActionPath = TEXT("/Game/Input/Actions/IA_Look.IA_Look");
	inline constexpr const TCHAR* MouseLookActionPath = TEXT("/Game/Input/Actions/IA_MouseLook.IA_MouseLook");
	inline constexpr const TCHAR* AscendActionPath = TEXT("/Game/Input/Actions/IA_Jump.IA_Jump");

	/** Opens the piece context menu. Bound on the controller, which owns the cursor and contexts. */
	inline constexpr const TCHAR* InspectPieceActionPath =
		TEXT("/Game/Input/Actions/IA_InspectPiece.IA_InspectPiece");

	/**
	 * Keeps the hover highlight on the cursor. Separate from IA_MouseLook (same Mouse2D axis)
	 * because look is chorded to IA_LookModifier and hover must fire with no button held
	 * (Content.SessionInput.LookNeedsTheModifierHeld).
	 */
	inline constexpr const TCHAR* HoverPieceActionPath =
		TEXT("/Game/Input/Actions/IA_HoverPiece.IA_HoverPiece");

	/**
	 * Unbound on purpose: IMC_MouseLook's Chorded Action trigger watches it so look needs the right
	 * mouse button held. No CDO resolves it, so only the table sweep would catch it going missing.
	 */
	inline constexpr const TCHAR* LookModifierActionPath =
		TEXT("/Game/Input/Actions/IA_LookModifier.IA_LookModifier");

	/**
	 * The eight session shortcuts. Each is a toolbar click via OnToolbarButton, so the model's
	 * refusals apply; Tab and G toggle a pair of buttons via ToggleSessionMode and
	 * ToggleSessionPlacement. Pinned by Content.SessionInput.SessionContextMapsTheShortcuts and
	 * SessionKeysAreFree.
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
	 * Highlight overlays, one per non-None state, so hovered, selected and inspected look distinct.
	 * Inspected is strongest: it marks the brick whose joint forces are shown.
	 */
	inline constexpr const TCHAR* BrickHoverMaterialPath =
		TEXT("/Game/Materials/M_BrickHover.M_BrickHover");
	inline constexpr const TCHAR* BrickSelectedMaterialPath =
		TEXT("/Game/Materials/M_BrickSelected.M_BrickSelected");
	inline constexpr const TCHAR* BrickInspectedMaterialPath =
		TEXT("/Game/Materials/M_BrickInspected.M_BrickInspected");

	/**
	 * Load overlay materials, one per EJointMarginBand (SESSION_UI_DESIGN §a principle 6). Overlays,
	 * not replacements, since they cover every piece at once. Colours are in the array below.
	 */
	inline constexpr const TCHAR* BrickLoadComfortableMaterialPath =
		TEXT("/Game/Materials/M_BrickLoadComfortable.M_BrickLoadComfortable");
	inline constexpr const TCHAR* BrickLoadCautionMaterialPath =
		TEXT("/Game/Materials/M_BrickLoadCaution.M_BrickLoadCaution");
	inline constexpr const TCHAR* BrickLoadCriticalMaterialPath =
		TEXT("/Game/Materials/M_BrickLoadCritical.M_BrickLoadCritical");

	/**
	 * Band colours: the materials' emissive, the headroom bar fill and the falling-brick dot.
	 * Indexed by EJointMarginBand (Critical 0, Caution 1, Comfortable 2). See
	 * BrickNeighbourSwatchColours on why equal values do not look equal on screen.
	 */
	inline constexpr FLinearColor BrickLoadSwatchColours[] = {
		FLinearColor(0.95f, 0.24f, 0.20f, 1.0f),
		FLinearColor(0.95f, 0.66f, 0.13f, 1.0f),
		FLinearColor(0.18f, 0.76f, 0.55f, 1.0f)
	};

	/**
	 * One material per joint-readout colour slot, so a row and its brick match. Six because a
	 * running-bond brick has six joints (FInspectorJointRow::ColourSlot).
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
	 * The panel swatch colour per slot: the emissive of the matching material above. A mismatch
	 * points the player at the wrong brick. Kept beside the paths after a separate palette drifted.
	 *
	 * Duplicated because a Constant3Vector is editor-only; a named VectorParameter would remove
	 * this array. Until then Content.NeighbourSwatchesMatchTheirMaterials asserts agreement.
	 * Do not tune these to a screenshot: a flat Slate fill and an additive overlay on a lit
	 * surface display the same linear value differently.
	 */
	inline constexpr FLinearColor BrickNeighbourSwatchColours[] = {
		FLinearColor(0.00f, 0.80f, 0.00f, 1.0f),
		FLinearColor(0.15f, 0.25f, 1.00f, 1.0f),
		FLinearColor(0.45f, 0.10f, 0.00f, 1.0f),
		FLinearColor(1.00f, 0.00f, 0.12f, 1.0f),
		FLinearColor(0.72f, 0.55f, 1.00f, 1.0f),
		FLinearColor(0.20f, 0.45f, 0.05f, 1.0f)
	};

	/** Base materials (element 0) per shed material; any other material keeps the mesh's grey. */
	inline constexpr const TCHAR* ShedBrickMaterialPath =
		TEXT("/Game/Materials/M_Shed_Brick.M_Shed_Brick");
	inline constexpr const TCHAR* ShedTimberMaterialPath =
		TEXT("/Game/Materials/M_Shed_Timber.M_Shed_Timber");

	// Mapping contexts ADestructionGamePlayerController adds for a local player.
	inline constexpr const TCHAR* DefaultMappingContextPath = TEXT("/Game/Input/IMC_Default.IMC_Default");
	inline constexpr const TCHAR* MouseLookMappingContextPath = TEXT("/Game/Input/IMC_MouseLook.IMC_MouseLook");

	/**
	 * The session's keys, separate so SessionKeysAreFree can diff them against the pawn's. A
	 * collision would be consumed (bConsumeInput) and the pawn would silently stop answering.
	 */
	inline constexpr const TCHAR* SessionMappingContextPath = TEXT("/Game/Input/IMC_Session.IMC_Session");

	/** ABrickActor's placeholder mesh. Not a UPROPERTY, so RequiredContentTest checks it by hand. */
	inline constexpr const TCHAR* BrickPlaceholderMeshPath = TEXT("/Game/LevelPrototyping/Meshes/SM_Cube.SM_Cube");

	TArrayView<const TCHAR* const> RequiredContentPaths();
}
