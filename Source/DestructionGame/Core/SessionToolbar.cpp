// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/SessionToolbar.h"

#include "Core/BuildMode/SnapSolver.h"
#include "Core/Profiles/ConnectionProfiles.h"

namespace DestructionSession
{
	// File-local names carry a SessionToolbar prefix so they cannot collide in a unity build.
	namespace
	{
		/** Course pitch (brick plus bed joint), derived from the snap settings rather than restated. */
		double SessionToolbarCoursePitchCm()
		{
			const BuildMode::FSnapSettings Settings;

			return Settings.BrickSizeCm.Z + Settings.JointThicknessCm;
		}

		/** Clamps a course to 0 or above. The single clamp for all course functions. */
		int32 SessionToolbarGroundedCourse(int32 Course)
		{
			return Course > 0 ? Course : 0;
		}

		/*
		 * Palette, in linear (SESSION_UI_DESIGN §e lists linear and sRGB; do not mix them). Accents
		 * are copied from DestructionGamePlayerController.cpp, which Core must not depend on. Swatches
		 * are the shed materials' base colours (Scripts/Author-ShedMaterials.py); do not retune them to
		 * match a screenshot.
		 */
		const FLinearColor SessionToolbarBuildAccent(0.95f, 0.66f, 0.13f, 1.0f);
		const FLinearColor SessionToolbarDestroyAccent(0.72f, 0.16f, 0.14f, 1.0f);
		const FLinearColor SessionToolbarBrickSwatch(0.35f, 0.06f, 0.04f, 1.0f);
		const FLinearColor SessionToolbarTimberSwatch(0.45f, 0.22f, 0.09f, 1.0f);

		/*
		 * Chip state colours. Disabled is the idle fill faded. Lit chips use dark ink, since a pale
		 * caption is unreadable on amber. Clear's danger cue is a warm caption, not a red fill, which
		 * would read as the Destroy mode.
		 */
		const FLinearColor SessionToolbarIdleFill(0.16f, 0.18f, 0.24f, 0.75f);
		const FLinearColor SessionToolbarDisabledFill(0.16f, 0.18f, 0.24f, 0.30f);
		const FLinearColor SessionToolbarIdleCaption(0.82f, 0.86f, 0.92f, 1.0f);
		const FLinearColor SessionToolbarDisabledCaption(0.55f, 0.60f, 0.68f, 0.45f);
		const FLinearColor SessionToolbarDarkInkCaption(0.02f, 0.015f, 0.01f, 1.0f);
		const FLinearColor SessionToolbarDangerCaption(0.95f, 0.55f, 0.50f, 1.0f);

		// Drop edge on clickable chips; disabled chips get a zero-alpha edge.
		const FLinearColor SessionToolbarChipEdge(0.0f, 0.0f, 0.0f, 0.35f);
		const FLinearColor SessionToolbarNoEdge(0.0f, 0.0f, 0.0f, 0.0f);

		/*
		 * Ring for Run structure, which shares the lit Destroy tab's red fill (§e); the ring tells
		 * the command apart from the tab.
		 */
		const FLinearColor SessionToolbarGoChipRing(1.0f, 0.95f, 0.92f, 0.90f);

		// §a principle 1: rounded chip with a 2 px edge, on every chip.
		constexpr float SessionToolbarChipCornerRadiusPx = 10.0f;
		constexpr float SessionToolbarChipOutlineWidthPx = 2.0f;

		/** Strip region for a button. Unknown buttons default to Command, never the fixed mode pair. */
		EToolbarGroup SessionToolbarGroup(EToolbarButtonId Id)
		{
			switch (Id)
			{
			case EToolbarButtonId::ModeBuild:
			case EToolbarButtonId::ModeDestroy:
				return EToolbarGroup::Mode;

			case EToolbarButtonId::PieceBrick:
			case EToolbarButtonId::PieceTimberPlate:
			case EToolbarButtonId::PieceTimberLintel:

			// Rotate and the joint chips configure the next placement, so they are settings.
			case EToolbarButtonId::RotatePiece:

			case EToolbarButtonId::PlacementSnap:
			case EToolbarButtonId::PlacementFree:

			case EToolbarButtonId::JointAuto:
			case EToolbarButtonId::JointMortar:
			case EToolbarButtonId::JointDry:
			case EToolbarButtonId::JointNail:
			case EToolbarButtonId::JointScrew:
			case EToolbarButtonId::JointBolt:

			case EToolbarButtonId::CourseDown:
			case EToolbarButtonId::CourseUp:

			// The load overlay changes only the view, so it is a setting too.
			case EToolbarButtonId::ToggleLoadOverlay:
				return EToolbarGroup::Settings;

			default:
				return EToolbarGroup::Command;
			}
		}

		/** Swatch for the piece a chip lays. Must agree with BuildPieceMaterial; both boards are Timber. */
		EToolbarSwatch SessionToolbarSwatch(EToolbarButtonId Id)
		{
			switch (Id)
			{
			case EToolbarButtonId::PieceBrick:        return EToolbarSwatch::Brick;
			case EToolbarButtonId::PieceTimberPlate:
			case EToolbarButtonId::PieceTimberLintel: return EToolbarSwatch::Timber;
			default:                                  return EToolbarSwatch::None;
			}
		}

		/** Button caption. Unknown buttons get a visibly-wrong caption rather than a plausible one. */
		FString SessionToolbarCaption(EToolbarButtonId Id)
		{
			switch (Id)
			{
			case EToolbarButtonId::ModeBuild:         return TEXT("Build");
			case EToolbarButtonId::ModeDestroy:       return TEXT("Destroy");
			case EToolbarButtonId::PieceBrick:        return TEXT("Brick");

			/*
			 * Captions are kept short so the strip fits 1280 px (§b). The swatch already says
			 * "Timber".
			 */
			case EToolbarButtonId::PieceTimberPlate:  return TEXT("Plate");
			case EToolbarButtonId::PieceTimberLintel: return TEXT("Lintel");
			case EToolbarButtonId::RotatePiece:       return TEXT("Rotate");

			case EToolbarButtonId::PlacementSnap:     return TEXT("Snap");
			case EToolbarButtonId::PlacementFree:     return TEXT("Free");

			// "Dry" is the DryStone profile, shortened.
			case EToolbarButtonId::JointAuto:         return TEXT("Auto");
			case EToolbarButtonId::JointMortar:       return TEXT("Mortar");
			case EToolbarButtonId::JointDry:          return TEXT("Dry");
			case EToolbarButtonId::JointNail:         return TEXT("Nail");
			case EToolbarButtonId::JointScrew:        return TEXT("Screw");
			case EToolbarButtonId::JointBolt:         return TEXT("Bolt");
			/*
			 * CourseLabel sits between these. ASCII rather than §b's typographic minus, so the source
			 * encoding cannot mangle it.
			 */
			case EToolbarButtonId::CourseDown:        return TEXT("-");
			case EToolbarButtonId::CourseUp:          return TEXT("+");
			case EToolbarButtonId::ToggleLoadOverlay: return TEXT("Load overlay");

			// "Clear build" overflowed 1280 px by one pixel; it only appears on the Build strip.
			case EToolbarButtonId::ClearBuild:        return TEXT("Clear");
			case EToolbarButtonId::RunStructure:      return TEXT("Run structure");
			}

			return TEXT("(no button)");
		}

		/** Whether a button is lit. Commands never are; a lit command reads as a stuck mode. */
		bool SessionToolbarIsActive(const FSessionToolbarState& State, EToolbarButtonId Id)
		{
			switch (Id)
			{
			case EToolbarButtonId::ModeBuild:         return State.Mode == ESessionMode::Build;
			case EToolbarButtonId::ModeDestroy:       return State.Mode != ESessionMode::Build;
			case EToolbarButtonId::PieceBrick:        return State.Piece == EBuildPieceKind::Brick;
			case EToolbarButtonId::PieceTimberPlate:  return State.Piece == EBuildPieceKind::TimberPlate;
			case EToolbarButtonId::PieceTimberLintel: return State.Piece == EBuildPieceKind::TimberLintel;
			case EToolbarButtonId::PlacementSnap:     return State.Placement == EPlacementMode::Snap;
			case EToolbarButtonId::PlacementFree:     return State.Placement != EPlacementMode::Snap;

			/*
			 * Lit when nothing is overridden. Not `== Auto`: an unknown choice also falls back to
			 * inference, and must light Auto.
			 */
			case EToolbarButtonId::JointAuto:         return JointOverrideFor(State.Joint) == nullptr;

			case EToolbarButtonId::JointMortar:       return State.Joint == EJointChoice::Mortar;
			case EToolbarButtonId::JointDry:          return State.Joint == EJointChoice::Dry;
			case EToolbarButtonId::JointNail:         return State.Joint == EJointChoice::Nail;
			case EToolbarButtonId::JointScrew:        return State.Joint == EJointChoice::Screw;
			case EToolbarButtonId::JointBolt:         return State.Joint == EJointChoice::Bolt;

			// Toggles latch, so the screen shows they are on.
			case EToolbarButtonId::RotatePiece:       return State.bRotated;

			case EToolbarButtonId::ToggleLoadOverlay: return State.bLoadOverlay;

			default:                                  return false;
			}
		}

		/** Whether a button can act. Everything else stays enabled so no mode can trap the player. */
		bool SessionToolbarIsEnabled(const FSessionToolbarState& State, EToolbarButtonId Id)
		{
			switch (Id)
			{
			case EToolbarButtonId::CourseDown:
				return State.Course >= 1;

			case EToolbarButtonId::ClearBuild:
			case EToolbarButtonId::RunStructure:
				// A silent no-op would read as a missed click.
				return State.bHasStructure;

			case EToolbarButtonId::ToggleLoadOverlay:
				// Nothing to tint without a structure.
				return State.bHasStructure;

			default:
				return true;
			}
		}
	}

	TArray<FToolbarButton> SessionToolbarButtons(const FSessionToolbarState& State)
	{
		const bool bBuilding = State.Mode == ESessionMode::Build;

		// Mode pair first in both lists, so it never moves under the cursor.
		const TArray<EToolbarButtonId> Ids = bBuilding
			? TArray<EToolbarButtonId>{
				EToolbarButtonId::ModeBuild,
				EToolbarButtonId::ModeDestroy,
				EToolbarButtonId::PieceBrick,
				EToolbarButtonId::PieceTimberPlate,
				EToolbarButtonId::PieceTimberLintel,
				EToolbarButtonId::RotatePiece,
				EToolbarButtonId::PlacementSnap,
				EToolbarButtonId::PlacementFree,
				EToolbarButtonId::JointAuto,
				EToolbarButtonId::JointMortar,
				EToolbarButtonId::JointDry,
				EToolbarButtonId::JointNail,
				EToolbarButtonId::JointScrew,
				EToolbarButtonId::JointBolt,
				EToolbarButtonId::CourseDown,
				EToolbarButtonId::CourseUp,
				EToolbarButtonId::ClearBuild }
			: TArray<EToolbarButtonId>{
				EToolbarButtonId::ModeBuild,
				EToolbarButtonId::ModeDestroy,
				EToolbarButtonId::ToggleLoadOverlay,
				EToolbarButtonId::RunStructure };

		TArray<FToolbarButton> Buttons;
		Buttons.Reserve(Ids.Num());

		for (EToolbarButtonId Id : Ids)
		{
			FToolbarButton Button;
			Button.Id = Id;
			Button.Label = SessionToolbarCaption(Id);
			Button.bActive = SessionToolbarIsActive(State, Id);
			Button.bEnabled = SessionToolbarIsEnabled(State, Id);
			Button.Group = SessionToolbarGroup(Id);
			Button.Swatch = SessionToolbarSwatch(Id);

			Buttons.Add(MoveTemp(Button));
		}

		return Buttons;
	}

	FSessionToolbarState ApplyToolbarButton(const FSessionToolbarState& State, EToolbarButtonId Id)
	{
		// Reuse the strip's own answer: an undrawn or greyed button is a no-op.
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		const FToolbarButton* Button = Buttons.FindByPredicate(
			[Id](const FToolbarButton& Candidate) { return Candidate.Id == Id; });

		if (Button == nullptr || !Button->bEnabled)
		{
			return State;
		}

		FSessionToolbarState After = State;

		switch (Id)
		{
		case EToolbarButtonId::ModeBuild:         After.Mode = ESessionMode::Build; break;
		case EToolbarButtonId::ModeDestroy:       After.Mode = ESessionMode::Destroy; break;
		case EToolbarButtonId::PieceBrick:        After.Piece = EBuildPieceKind::Brick; break;
		case EToolbarButtonId::PieceTimberPlate:  After.Piece = EBuildPieceKind::TimberPlate; break;
		case EToolbarButtonId::PieceTimberLintel: After.Piece = EBuildPieceKind::TimberLintel; break;
		case EToolbarButtonId::PlacementSnap:     After.Placement = EPlacementMode::Snap; break;
		case EToolbarButtonId::PlacementFree:     After.Placement = EPlacementMode::Free; break;

		// Segmented control: each chip, Auto included, replaces the choice.
		case EToolbarButtonId::JointAuto:         After.Joint = EJointChoice::Auto; break;
		case EToolbarButtonId::JointMortar:       After.Joint = EJointChoice::Mortar; break;
		case EToolbarButtonId::JointDry:          After.Joint = EJointChoice::Dry; break;
		case EToolbarButtonId::JointNail:         After.Joint = EJointChoice::Nail; break;
		case EToolbarButtonId::JointScrew:        After.Joint = EJointChoice::Screw; break;
		case EToolbarButtonId::JointBolt:         After.Joint = EJointChoice::Bolt; break;

		case EToolbarButtonId::RotatePiece:
			After.bRotated = !State.bRotated;
			break;

		case EToolbarButtonId::CourseDown:
			// No clamp: the disabled check above is the floor.
			After.Course = State.Course - 1;
			break;

		case EToolbarButtonId::CourseUp:
			After.Course = State.Course + 1;
			break;

		case EToolbarButtonId::ToggleLoadOverlay:
			// Persists through Build mode, where the chip is not drawn.
			After.bLoadOverlay = !State.bLoadOverlay;
			break;

		case EToolbarButtonId::ClearBuild:
		case EToolbarButtonId::RunStructure:
			// Commands: the controller runs them; toolbar state is unchanged.
			break;
		}

		return After;
	}

	FLinearColor ModeAccent(ESessionMode Mode)
	{
		// Same comparison SessionToolbarButtons uses, so the two cannot disagree.
		return Mode == ESessionMode::Build ? SessionToolbarBuildAccent : SessionToolbarDestroyAccent;
	}

	FLinearColor SwatchColour(EToolbarSwatch Swatch)
	{
		switch (Swatch)
		{
		case EToolbarSwatch::Brick:  return SessionToolbarBrickSwatch;
		case EToolbarSwatch::Timber: return SessionToolbarTimberSwatch;
		default:                     break;
		}

		// None or unknown: transparent, since the swatch widget is always drawn.
		return FLinearColor::Transparent;
	}

	FChipLook ChipLookFor(const FToolbarButton& Button, ESessionMode Mode)
	{
		FChipLook Look;

		// Geometry is uniform; state is shown by colour and weight.
		Look.CornerRadiusPx = SessionToolbarChipCornerRadiusPx;
		Look.OutlineWidthPx = SessionToolbarChipOutlineWidthPx;

		if (!Button.bEnabled)
		{
			// Before the "go" chip check: a disabled Run structure must read as disabled.
			Look.Fill = SessionToolbarDisabledFill;
			Look.Outline = SessionToolbarNoEdge;
			Look.Caption = SessionToolbarDisabledCaption;
			Look.bBoldCaption = false;

			return Look;
		}

		Look.Outline = SessionToolbarChipEdge;

		if (Button.bActive)
		{
			// The mode's accent, so the strip colour shows the mode.
			Look.Fill = ModeAccent(Mode);
			Look.Caption = SessionToolbarDarkInkCaption;
			Look.bBoldCaption = true;

			return Look;
		}

		if (Button.Id == EToolbarButtonId::RunStructure)
		{
			/*
			 * The "go" chip: never active, but §e fills it with the destroy accent regardless of mode.
			 * The ring distinguishes it from the lit Destroy tab.
			 */
			Look.Fill = SessionToolbarDestroyAccent;
			Look.Outline = SessionToolbarGoChipRing;
			Look.Caption = SessionToolbarDarkInkCaption;
			Look.bBoldCaption = true;

			return Look;
		}

		Look.Fill = SessionToolbarIdleFill;
		Look.Caption = Button.Id == EToolbarButtonId::ClearBuild
			? SessionToolbarDangerCaption
			: SessionToolbarIdleCaption;
		Look.bBoldCaption = false;

		return Look;
	}

	const FConnectionStrength* JointOverrideFor(EJointChoice Joint)
	{
		switch (Joint)
		{
		// Full bed bond on every face; the weaker perpend is left to inference.
		case EJointChoice::Mortar: return &DestructionProfiles::GeneralPurposeMortar;

		case EJointChoice::Dry:    return &DestructionProfiles::DryStone;
		case EJointChoice::Nail:   return &DestructionProfiles::Nail;
		case EJointChoice::Screw:  return &DestructionProfiles::Screw;
		case EJointChoice::Bolt:   return &DestructionProfiles::Bolt;

		case EJointChoice::Auto:   break;
		}

		// Auto and unknown choices override nothing; unknown falls back to inference (fail closed).
		return nullptr;
	}

	FVector BuildPieceHalfExtentCm(EBuildPieceKind Kind)
	{
		// Brick from the snap settings; timber from the demo building's board. Lintel shares the plate's section.
		const BuildMode::FSnapSettings Settings;

		switch (Kind)
		{
		case EBuildPieceKind::Brick:        return 0.5 * Settings.BrickSizeCm;
		case EBuildPieceKind::TimberPlate:  return FVector(33.75, 5.125, 5.0);
		case EBuildPieceKind::TimberLintel: return FVector(45.0, 5.125, 5.0);
		}

		// Unknown kind: zero size.
		return FVector::ZeroVector;
	}

	const DestructionProfiles::FMaterialProfile& BuildPieceMaterial(EBuildPieceKind Kind)
	{
		switch (Kind)
		{
		case EBuildPieceKind::Brick:
			return DestructionProfiles::ClayBrick;

		case EBuildPieceKind::TimberPlate:
		case EBuildPieceKind::TimberLintel:
			return DestructionProfiles::Timber;
		}

		// Fail closed: Timber makes JointForContact infer DryStone, the weakest joint.
		return DestructionProfiles::Timber;
	}

	double CoursePlaneZCm(int32 Course, double PieceHalfHeightCm)
	{
		return SessionToolbarGroundedCourse(Course) * SessionToolbarCoursePitchCm() + PieceHalfHeightCm;
	}

	bool IsCourseGrounded(int32 Course)
	{
		return SessionToolbarGroundedCourse(Course) == 0;
	}

	FString CourseLabel(int32 Course)
	{
		// Displayed from one, stored from zero, matching Core/PieceMenu.cpp.
		return FString::Printf(TEXT("Course %d"), SessionToolbarGroundedCourse(Course) + 1);
	}
}
