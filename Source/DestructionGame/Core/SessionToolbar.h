// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * Presenter model for the session toolbar: which buttons the build/destroy strip offers, which
 * is lit or greyed, and what a click does. Kept out of Slate (like Core/PieceMenu.h) so it is
 * testable headless; the widget only draws.
 */
namespace DestructionSession
{
	/** Session mode. Build is zero so a default session cannot destroy anything. */
	enum class ESessionMode : uint8
	{
		/** Placing pieces. */
		Build,

		/** Removing pieces. Build settings are kept for the way back. */
		Destroy,
	};

	/** Which piece the ghost is. A kind, so dimensions live only in the palette tables below. */
	enum class EBuildPieceKind : uint8
	{
		/** Standard 21.5 x 10.25 x 6.5 cm clay brick. */
		Brick,

		/** The demo building's 67.5 cm timber wall plate. */
		TimberPlate,

		/** 90 cm timber, for spanning an opening. */
		TimberLintel,
	};

	/** Whether a placement snaps onto the bond or goes exactly where the cursor is. */
	enum class EPlacementMode : uint8
	{
		/** The snap solver's best candidate wins. */
		Snap,

		/** The requested pose is used verbatim, overlaps and all. */
		Free,
	};

	/**
	 * What fastens the next piece. Auto (zero) leaves every joint to BuildMode::JointForContact;
	 * the others map to library rows via JointOverrideFor. No perpend chip: the inference picks it.
	 */
	enum class EJointChoice : uint8
	{
		/** The inference decides per joint. */
		Auto,

		/** General-purpose mortar in every joint the piece forms. */
		Mortar,

		/** No bond at all: compression and friction only. */
		Dry,

		Nail,
		Screw,
		Bolt,
	};

	/** Everything the toolbar knows about the session, and the only thing a click changes. */
	struct FSessionToolbarState
	{
		ESessionMode Mode = ESessionMode::Build;

		EBuildPieceKind Piece = EBuildPieceKind::Brick;

		EPlacementMode Placement = EPlacementMode::Snap;

		/** What fastens every joint the next placement forms. Survives a trip through Destroy mode. */
		EJointChoice Joint = EJointChoice::Auto;

		/**
		 * Zero-based build-plane course; course 0 sits on the earth. Never negative:
		 * ApplyToolbarButton refuses to go below 0 and the course functions treat negatives as 0.
		 */
		int32 Course = 0;

		/**
		 * Whether the next piece is turned a quarter turn about Z. Makes quoin corners reachable.
		 * Survives a trip through Destroy mode.
		 */
		bool bRotated = false;

		/** Whether there is a live structure; Clear and Run are greyed without one. */
		bool bHasStructure = false;

		/**
		 * Whether the Destroy-mode load overlay is on (each piece tinted by its worst joint's margin
		 * band). A latching setting, off by default because it costs a solve per toggle and mutation.
		 * Survives a trip through Build mode.
		 */
		bool bLoadOverlay = false;
	};

	/** Every button the strip can carry. Clicks report the id, never the caption. */
	enum class EToolbarButtonId : uint8
	{
		ModeBuild,
		ModeDestroy,
		PieceBrick,
		PieceTimberPlate,
		PieceTimberLintel,

		/** Latch: turn the next piece a quarter turn about Z. Drawn after the pieces, with no swatch. */
		RotatePiece,

		PlacementSnap,
		PlacementFree,

		/** The six joint chips, in EJointChoice order, between the placement pair and the course stepper. */
		JointAuto,
		JointMortar,
		JointDry,
		JointNail,
		JointScrew,
		JointBolt,

		CourseDown,
		CourseUp,

		/** Destroy-mode setting: tint every piece by its worst joint's margin band. */
		ToggleLoadOverlay,

		ClearBuild,
		RunStructure,
	};

	/**
	 * Strip region (SESSION_UI_DESIGN §b): mode tabs, settings, command, each contiguous and in
	 * this order. The widget draws a 1 px rule where the group changes, so a destructive click is
	 * never next to a setting.
	 */
	enum class EToolbarGroup : uint8
	{
		/** The two mode tabs. Always first; they never move. */
		Mode,

		/** Everything that changes with the mode. */
		Settings,

		/** The mode's one command. */
		Command,
	};

	/** The colour swatch a chip carries (a kind; SwatchColour maps it to a colour). */
	enum class EToolbarSwatch : uint8
	{
		/** Nothing to draw (non-piece chips). */
		None,

		Brick,

		/** Plate and lintel: one material at two lengths. */
		Timber,
	};

	/** One button on the strip. */
	struct FToolbarButton
	{
		EToolbarButtonId Id = EToolbarButtonId::ModeBuild;

		/** Caption, kept on the row so tests can read it. */
		FString Label;

		/** Currently chosen. One per setting group; commands never are. Distinct from bEnabled. */
		bool bActive = false;

		/**
		 * Whether a click can take effect. ApplyToolbarButton reads this rather than re-deciding.
		 * False by default so a half-built row cannot claim a click will land.
		 */
		bool bEnabled = false;

		/** Strip region. Command by default, so an unfilled row cannot pose as a mode tab. */
		EToolbarGroup Group = EToolbarGroup::Command;

		/** Which piece the chip lays, if any. */
		EToolbarSwatch Swatch = EToolbarSwatch::None;
	};

	/**
	 * How one chip is drawn (SESSION_UI_DESIGN §e). Decided here so tests can tell lit from greyed;
	 * plain colours, which the widget turns into brushes.
	 */
	struct FChipLook
	{
		/** Fill; the mode's accent when chosen. */
		FLinearColor Fill = FLinearColor::Transparent;

		/**
		 * The 2 px drop edge. Also distinguishes the Run chip from the lit Destroy tab, which share
		 * the same red fill: the Run chip has a bright rim, everything else a shadow.
		 */
		FLinearColor Outline = FLinearColor::Transparent;

		/** Caption colour: dark on a lit chip, grey on an idle one. */
		FLinearColor Caption = FLinearColor::White;

		/** Corner radius, px. Same on every chip. */
		float CornerRadiusPx = 0.0f;

		/** Drop-edge width, px. Same on every chip. */
		float OutlineWidthPx = 0.0f;

		/** Bold caption; bActive is shown by both fill and weight. */
		bool bBoldCaption = false;
	};

	/**
	 * The strip for a state: one fixed list per mode, mode pair first. The button set depends on
	 * the mode alone, so the strip never rearranges under the cursor.
	 */
	TArray<FToolbarButton> SessionToolbarButtons(const FSessionToolbarState& State);

	/**
	 * One click as a pure function. A button the strip does not draw, or draws greyed, is a
	 * no-op (including the course floor, refused rather than clamped). Fields the transition does
	 * not name survive it.
	 */
	FSessionToolbarState ApplyToolbarButton(const FSessionToolbarState& State, EToolbarButtonId Id);

	/**
	 * Mode accent: build amber, destroy red, applied to every lit chip on that mode's strip. An
	 * unknown mode gets the destroy accent, matching SessionToolbarButtons.
	 */
	FLinearColor ModeAccent(ESessionMode Mode);

	/**
	 * Swatch colour: the shed materials' base colours, so the chip matches the piece laid. None
	 * and unknown kinds are transparent.
	 */
	FLinearColor SwatchColour(EToolbarSwatch Swatch);

	/**
	 * How one chip is drawn. Precedence: !bEnabled, then bActive, then the Run ("go") chip, then
	 * idle, so a greyed Run still reads greyed. Clear build warns with a warm caption rather than a
	 * red fill, which would read as Destroy mode.
	 */
	FChipLook ChipLookFor(const FToolbarButton& Button, ESessionMode Mode);

	/**
	 * The profile a joint choice forces on every formed joint, or nullptr for Auto (let the
	 * inference decide). Returns the library row's address, never a copy, so retunes propagate.
	 * An unknown choice returns nullptr (fail closed).
	 */
	const FConnectionStrength* JointOverrideFor(EJointChoice Joint);

	/**
	 * A piece kind's half extent, cm, matching what the demo building lays (the plate is
	 * DemoBuilding.cpp's (33.75, 5.125, 5.0)). An unknown kind returns zero, an obvious refusal.
	 */
	FVector BuildPieceHalfExtentCm(EBuildPieceKind Kind);

	/**
	 * A piece kind's material, by reference to the library row (never a copy, so retunes
	 * propagate). An unknown kind returns Timber, which makes JointForContact infer DryStone, the
	 * weakest joint it hands out.
	 */
	const DestructionProfiles::FMaterialProfile& BuildPieceMaterial(EBuildPieceKind Kind);

	/**
	 * Centre Z, cm, of a piece laid on a course. Course 0 rests on the ground, so it returns the
	 * piece's half-height (ruling 2026-09-15). The pitch is always the brick course pitch from
	 * FSnapSettings, whatever the piece, so a timber plate on course 2 bears on two brick courses.
	 * Negative courses are course 0.
	 */
	double CoursePlaneZCm(int32 Course, double PieceHalfHeightCm);

	/**
	 * Whether the toolbar intends the build plane to be on the earth (course 0 only). Only drives
	 * the readout: a snap can lift a piece onto a course above, so the committed piece's grounded
	 * flag must come from the snapped pose, never from this. Negative courses are course 0.
	 */
	bool IsCourseGrounded(int32 Course);

	/**
	 * Course readout, counting from one to match the piece menu (ruling 2026-09-15):
	 * CourseLabel(0) reads "Course 1". FSessionToolbarState::Course stays zero-based.
	 */
	FString CourseLabel(int32 Course);
}
