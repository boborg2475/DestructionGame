// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/SessionToolbar.h"

#include "Core/BuildMode/SnapSolver.h"
#include "Core/Profiles/ConnectionProfiles.h"

namespace DestructionSession
{
	/*
	 * Every file-local name here carries a SessionToolbar prefix, for the reason Core/PieceMenu.cpp
	 * and Core/Structure.cpp both set out: an anonymous namespace is private to a translation unit,
	 * not a file, and a unity build merges many files into one — so two colliding file-local names
	 * are a hard compile error between files that never refer to each other.
	 */
	namespace
	{
		/**
		 * The brick course pitch — one brick plus one bed joint — read from the snap settings.
		 *
		 * Derived, never written down again: a second hand-written 7.5 here would be a coordinating
		 * grid spelled in two places, free to disagree the day the brick or joint is retuned. DESIGN
		 * §3's rule about conversion constants, applied to a coordinating dimension.
		 */
		double SessionToolbarCoursePitchCm()
		{
			const BuildMode::FSnapSettings Settings;

			return Settings.BrickSizeCm.Z + Settings.JointThicknessCm;
		}

		/**
		 * The course a number NAMES, which for anything below the ground is the grounded one.
		 *
		 * One clamp for the whole course vocabulary — the alternative is two functions disagreeing
		 * about a state that should not exist, which is what would let it survive.
		 */
		int32 SessionToolbarGroundedCourse(int32 Course)
		{
			return Course > 0 ? Course : 0;
		}

		/*
		 * The strip's palette, in linear — the number this model hands out and Slate takes.
		 * SESSION_UI_DESIGN §e gives every colour twice, linear and sRGB hex; confusing the two is
		 * how a palette drifts.
		 *
		 * The two accents are the colours this UI already uses, transcribed here because there is
		 * nowhere shareable to read them from: amber is the Caution band's gold, red is the
		 * destructive row's, both file-static constants inside DestructionGamePlayerController.cpp, a
		 * widget file this Core namespace must not depend on.
		 *
		 * The two swatches are the shed materials' own base colours, from
		 * Scripts/Author-ShedMaterials.py — M_Shed_Brick (0.35, 0.06, 0.04) and M_Shed_Timber
		 * (0.45, 0.22, 0.09), not pixel-identical to the brick's lit surface since the swatch is a
		 * flat Slate fill over a near-black bar. Nobody may retune one to match a screenshot.
		 */
		const FLinearColor SessionToolbarBuildAccent(0.95f, 0.66f, 0.13f, 1.0f);
		const FLinearColor SessionToolbarDestroyAccent(0.72f, 0.16f, 0.14f, 1.0f);
		const FLinearColor SessionToolbarBrickSwatch(0.35f, 0.06f, 0.04f, 1.0f);
		const FLinearColor SessionToolbarTimberSwatch(0.45f, 0.22f, 0.09f, 1.0f);

		/*
		 * The three visual states' own colours.
		 *
		 * The idle fill is the panel's inactive-tab slate — dark enough to sit under a caption, solid
		 * enough not to read as greyed. The greyed fill is the same colour faded, so the fade is what
		 * stops the strip telling the player a refused click was going to do something. Dark ink sits
		 * on a lit chip because the fill is a bright accent — amber especially — and a pale caption on
		 * it is unreadable at exactly the glance it exists for.
		 *
		 * The danger caption is warm rather than red-filled: Clear build is the one irreversible
		 * control in the Build group, but a chip filled destroy-red on an amber strip would read as
		 * the mode you are in, so a caption tinted toward red reads as a warning instead.
		 */
		const FLinearColor SessionToolbarIdleFill(0.16f, 0.18f, 0.24f, 0.75f);
		const FLinearColor SessionToolbarDisabledFill(0.16f, 0.18f, 0.24f, 0.30f);
		const FLinearColor SessionToolbarIdleCaption(0.82f, 0.86f, 0.92f, 1.0f);
		const FLinearColor SessionToolbarDisabledCaption(0.55f, 0.60f, 0.68f, 0.45f);
		const FLinearColor SessionToolbarDarkInkCaption(0.02f, 0.015f, 0.01f, 1.0f);
		const FLinearColor SessionToolbarDangerCaption(0.95f, 0.55f, 0.50f, 1.0f);

		/*
		 * The drop edge — a shadow under the chip on anything clickable, nothing on a chip that is
		 * not. §e's disabled row asks for no edge; the geometry stays uniform and it is the edge's
		 * own alpha that takes it away.
		 */
		const FLinearColor SessionToolbarChipEdge(0.0f, 0.0f, 0.0f, 0.35f);
		const FLinearColor SessionToolbarNoEdge(0.0f, 0.0f, 0.0f, 0.0f);

		/*
		 * The one chip that is ringed rather than edged: the "go" command. A latched tab and an
		 * irreversible verb may not be the same chip, and on a Destroy strip they nearly are — the
		 * lit `Destroy` tab and `Run structure` are both filled the same red two slots apart. The
		 * difference has to be in the outline because the fill is spoken for (§e asks for Run filled
		 * in the destroy accent), so a bright rim is the ordinary way a UI says "this is the button
		 * that does the thing".
		 */
		const FLinearColor SessionToolbarGoChipRing(1.0f, 0.95f, 0.92f, 0.90f);

		/* §a principle 1's chunky rounded chip with its 2 px drop edge, on every chip of every strip. */
		constexpr float SessionToolbarChipCornerRadiusPx = 10.0f;
		constexpr float SessionToolbarChipOutlineWidthPx = 2.0f;

		/**
		 * Which region of the strip a button sits in.
		 *
		 * The default arm is Command, the fail-closed end: a button this build has never heard of
		 * draws past the last rule on its own, rather than joining the mode pair that may never move.
		 */
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

			/*
			 * And so is Rotate: which way the next piece lies is a property of the placement rather
			 * than something that happens, so it belongs with the palette and ahead of the commands.
			 */
			case EToolbarButtonId::RotatePiece:

			case EToolbarButtonId::PlacementSnap:
			case EToolbarButtonId::PlacementFree:

			/*
			 * The six joint chips are settings for the same reason the placement pair is: the choice
			 * is a property of the next placement, so it latches, lights, and sits ahead of the rule
			 * the commands are past.
			 */
			case EToolbarButtonId::JointAuto:
			case EToolbarButtonId::JointMortar:
			case EToolbarButtonId::JointDry:
			case EToolbarButtonId::JointNail:
			case EToolbarButtonId::JointScrew:
			case EToolbarButtonId::JointBolt:

			case EToolbarButtonId::CourseDown:
			case EToolbarButtonId::CourseUp:

			/*
			 * The load overlay is a setting: it changes how the session looks at the structure rather
			 * than doing anything to it, so it sits ahead of the rule, away from the click that
			 * settles the wall.
			 */
			case EToolbarButtonId::ToggleLoadOverlay:
				return EToolbarGroup::Settings;

			default:
				return EToolbarGroup::Command;
			}
		}

		/**
		 * Which piece a chip lays, if it lays one.
		 *
		 * Both boards are Timber: one material at two lengths, and a swatch that disagreed with
		 * BuildPieceMaterial would be a chip promising a piece the placement does not lay.
		 */
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

		/**
		 * What a button reads.
		 *
		 * A button this build does not know gets a word of its own, for the reason
		 * PresenterWordForJointRole's "no tier" exists: a caption that reads like a real button is
		 * worse than one that is visibly not.
		 */
		FString SessionToolbarCaption(EToolbarButtonId Id)
		{
			switch (Id)
			{
			case EToolbarButtonId::ModeBuild:         return TEXT("Build");
			case EToolbarButtonId::ModeDestroy:       return TEXT("Destroy");
			case EToolbarButtonId::PieceBrick:        return TEXT("Brick");

			/*
			 * The piece, not its material — the swatch already says "Timber". Naming it again in the
			 * caption was the same fact drawn twice, at eleven characters a time on a strip that must
			 * fit 1280 px without scrolling (§b).
			 */
			case EToolbarButtonId::PieceTimberPlate:  return TEXT("Plate");
			case EToolbarButtonId::PieceTimberLintel: return TEXT("Lintel");
			/*
			 * The verb, and only the verb: "Rotate 90" or "Rotate piece" say the same thing at more of
			 * the 1280 px this strip may not scroll past.
			 */
			case EToolbarButtonId::RotatePiece:       return TEXT("Rotate");

			case EToolbarButtonId::PlacementSnap:     return TEXT("Snap");
			case EToolbarButtonId::PlacementFree:     return TEXT("Free");

			/*
			 * The fastener's own word, "Dry" rather than "DryStone": the library row is DryStone, but
			 * the chip is one slot of a six-slot control on a 48 px bar and both spellings read as
			 * "no bond at all".
			 */
			case EToolbarButtonId::JointAuto:         return TEXT("Auto");
			case EToolbarButtonId::JointMortar:       return TEXT("Mortar");
			case EToolbarButtonId::JointDry:          return TEXT("Dry");
			case EToolbarButtonId::JointNail:         return TEXT("Nail");
			case EToolbarButtonId::JointScrew:        return TEXT("Screw");
			case EToolbarButtonId::JointBolt:         return TEXT("Bolt");
			/*
			 * The stepper's arrows say nothing the readout between them does not — `CourseLabel` sits
			 * between these two chips, so "Course down/up" would spell "Course" twice more.
			 *
			 * ASCII, deliberately. §b writes the pair as the typographic minus and plus, but a TEXT()
			 * literal is the one place a non-ASCII character is at the mercy of the source encoding;
			 * the hyphen reads identically at 11 px and cannot be mangled into a question mark.
			 */
			case EToolbarButtonId::CourseDown:        return TEXT("-");
			case EToolbarButtonId::CourseUp:          return TEXT("+");
			case EToolbarButtonId::ToggleLoadOverlay: return TEXT("Load overlay");

			/*
			 * One word, because the seventeenth chip took the room: `Clear build` was the widest chip
			 * on the Build strip, ending one pixel past a 1280 px viewport where nothing says why a
			 * control cannot be clicked. The strip only ever shows the Build strip, so "build" was the
			 * fact the chip was drawing twice.
			 *
			 * The verb is what survives, the same rule the stepper's `-`/`+` and the palette's
			 * `Plate`/`Lintel` were shortened under. `Clear` keeps its warm danger caption.
			 */
			case EToolbarButtonId::ClearBuild:        return TEXT("Clear");
			case EToolbarButtonId::RunStructure:      return TEXT("Run structure");
			}

			return TEXT("(no button)");
		}

		/**
		 * Whether a button is the one its group's setting names.
		 *
		 * A command is never lit — the default arm rather than an omission: Clear and Run are things
		 * that happen, and a latched-looking command reads as a mode the player is stuck in.
		 */
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
			 * Auto is lit when nothing is overridden, derived once rather than twice: `State.Joint ==
			 * Auto` reads the same today but comes apart on a choice this build has never heard of,
			 * which JointOverrideFor hands back to the inference — the strip would show no chip lit
			 * over a session that is, in fact, on Auto.
			 */
			case EToolbarButtonId::JointAuto:         return JointOverrideFor(State.Joint) == nullptr;

			case EToolbarButtonId::JointMortar:       return State.Joint == EJointChoice::Mortar;
			case EToolbarButtonId::JointDry:          return State.Joint == EJointChoice::Dry;
			case EToolbarButtonId::JointNail:         return State.Joint == EJointChoice::Nail;
			case EToolbarButtonId::JointScrew:        return State.Joint == EJointChoice::Screw;
			case EToolbarButtonId::JointBolt:         return State.Joint == EJointChoice::Bolt;

			/*
			 * Rotate latches, the whole difference between it and a verb: unlit while every ghost
			 * lands turned would leave the player with a rotated wall and nothing on screen admitting
			 * to it.
			 */
			case EToolbarButtonId::RotatePiece:       return State.bRotated;

			/*
			 * A setting latches, the whole difference between this chip and Run structure two slots
			 * away: a toggle drawn unlit over a wall it has tinted leaves the player with no control
			 * that admits to having done it.
			 */
			case EToolbarButtonId::ToggleLoadOverlay: return State.bLoadOverlay;

			default:                                  return false;
			}
		}

		/**
		 * Whether the thing behind a button can actually happen.
		 *
		 * The three preconditions are the whole list; everything else being live is a decision, not
		 * an oversight — a mode button greyed by an over-eager precondition traps the player in it.
		 */
		bool SessionToolbarIsEnabled(const FSessionToolbarState& State, EToolbarButtonId Id)
		{
			switch (Id)
			{
			case EToolbarButtonId::CourseDown:
				/* There is no course below the one with the earth under it. */
				return State.Course >= 1;

			case EToolbarButtonId::ClearBuild:
			case EToolbarButtonId::RunStructure:
				/* Both act on a live structure, and a silent no-op reads as a missed click. */
				return State.bHasStructure;

			case EToolbarButtonId::ToggleLoadOverlay:
				/*
				 * The same precondition, not tidiness: with nothing built there is nothing to solve
				 * or tint, so a live chip would latch on and colour zero bricks.
				 */
				return State.bHasStructure;

			default:
				return true;
			}
		}
	}

	TArray<FToolbarButton> SessionToolbarButtons(const FSessionToolbarState& State)
	{
		const bool bBuilding = State.Mode == ESessionMode::Build;

		/*
		 * The mode pair first in both lists — the two buttons that switch modes may not move, or a
		 * strip whose first slots shifted would put a different button under a cursor that has not
		 * moved.
		 */
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
		/*
		 * The strip is asked rather than re-decided: a button the state does not draw, or draws
		 * greyed, is a bitwise no-op. Deciding "can this happen" a second time is how a lit button
		 * that does nothing gets shipped.
		 */
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

		/*
		 * One chip of a segmented control replaces the choice, never adds to it, and Auto is a
		 * choice like the other five — a player who screwed a plate down and wants an ordinary
		 * bedded brick has no other way to say so.
		 */
		case EToolbarButtonId::JointAuto:         After.Joint = EJointChoice::Auto; break;
		case EToolbarButtonId::JointMortar:       After.Joint = EJointChoice::Mortar; break;
		case EToolbarButtonId::JointDry:          After.Joint = EJointChoice::Dry; break;
		case EToolbarButtonId::JointNail:         After.Joint = EJointChoice::Nail; break;
		case EToolbarButtonId::JointScrew:        After.Joint = EJointChoice::Screw; break;
		case EToolbarButtonId::JointBolt:         After.Joint = EJointChoice::Bolt; break;

		case EToolbarButtonId::RotatePiece:
			/*
			 * A toggle, like the overlay and for the same reason: a rotation a player cannot undo is
			 * a player reopening the level. Only the flag moves.
			 */
			After.bRotated = !State.bRotated;
			break;

		case EToolbarButtonId::CourseDown:
			/*
			 * No clamp here, deliberately: the floor is the greying above, so a refused click leaves
			 * the state alone bit for bit rather than landing on a number that happens to match.
			 */
			After.Course = State.Course - 1;
			break;

		case EToolbarButtonId::CourseUp:
			After.Course = State.Course + 1;
			break;

		case EToolbarButtonId::ToggleLoadOverlay:
			/*
			 * A toggle, not a latch: the same chip turns it off, and only the flag moves, so the
			 * overlay survives every trip through Build mode where the chip is not drawn.
			 */
			After.bLoadOverlay = !State.bLoadOverlay;
			break;

		case EToolbarButtonId::ClearBuild:
		case EToolbarButtonId::RunStructure:
			/* Commands. The controller runs them; the toolbar's own state is untouched by either. */
			break;
		}

		return After;
	}

	FLinearColor ModeAccent(ESessionMode Mode)
	{
		/*
		 * Anything that is not Build answers with the destroy accent, written as a comparison rather
		 * than a switch on purpose: SessionToolbarButtons draws the Destroy strip the same way
		 * (`bBuilding = Mode == Build`), so the two answers derived alike cannot come apart.
		 */
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

		/*
		 * None and any kind nobody declared draw nothing: the swatch goes through one widget whatever
		 * it is, so "nothing to draw" has to be a colour rather than a chip naming a piece it lays.
		 */
		return FLinearColor::Transparent;
	}

	FChipLook ChipLookFor(const FToolbarButton& Button, ESessionMode Mode)
	{
		FChipLook Look;

		/*
		 * The geometry is the same on every chip of every strip, whatever state it is in: state is
		 * said in colour and weight, and shape is what makes them all one row.
		 */
		Look.CornerRadiusPx = SessionToolbarChipCornerRadiusPx;
		Look.OutlineWidthPx = SessionToolbarChipOutlineWidthPx;

		if (!Button.bEnabled)
		{
			/*
			 * Greyed comes first, ahead of the "go" chip below: Run structure with nothing built is
			 * both filled-by-identity and refused, and it has to read as refused.
			 */
			Look.Fill = SessionToolbarDisabledFill;
			Look.Outline = SessionToolbarNoEdge;
			Look.Caption = SessionToolbarDisabledCaption;
			Look.bBoldCaption = false;

			return Look;
		}

		Look.Outline = SessionToolbarChipEdge;

		if (Button.bActive)
		{
			/*
			 * The mode's accent, not the button's: everything lit on a Build strip is amber and on a
			 * Destroy strip is red, so the strip's colour reads the mode from the corner of an eye.
			 */
			Look.Fill = ModeAccent(Mode);
			Look.Caption = SessionToolbarDarkInkCaption;
			Look.bBoldCaption = true;

			return Look;
		}

		if (Button.Id == EToolbarButtonId::RunStructure)
		{
			/*
			 * The "go" chip, why this function takes a button: a command is never bActive, yet §e
			 * asks for Run structure filled in the destroy accent anyway — irreconcilable as
			 * `bActive ? Accent : Idle`, but keyed on the button they are simply two different chips.
			 *
			 * The destroy accent by name rather than the mode's: this chip is red because of what it
			 * does, not where it is. The ring keeps it apart from the lit `Destroy` tab, which carries
			 * the same fill for a different reason — see SessionToolbarGoChipRing.
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
		/*
		 * The bed bond, not the perpend: Mortar is the player asking for a full bond wherever the
		 * piece lands, head joints included. The weak perpend is what the inference picks for a
		 * vertical face, so there is no chip for it.
		 */
		case EJointChoice::Mortar: return &DestructionProfiles::GeneralPurposeMortar;

		case EJointChoice::Dry:    return &DestructionProfiles::DryStone;
		case EJointChoice::Nail:   return &DestructionProfiles::Nail;
		case EJointChoice::Screw:  return &DestructionProfiles::Screw;
		case EJointChoice::Bolt:   return &DestructionProfiles::Bolt;

		case EJointChoice::Auto:   break;
		}

		/*
		 * Auto and an unknown choice both override nothing — see the header. The unknown arm is
		 * fail-closed: handing back the inference credits the joint with what it had before the chip
		 * existed, where a plausible row would fasten it with a profile nobody picked.
		 */
		return nullptr;
	}

	FVector BuildPieceHalfExtentCm(EBuildPieceKind Kind)
	{
		/*
		 * The brick is read from the snap settings, the timber from the demo building's own board:
		 * halving BuildMode::FSnapSettings stops the toolbar becoming a third place brick dimensions
		 * are written down, and the lintel shares the plate's section so the two bear identically.
		 */
		const BuildMode::FSnapSettings Settings;

		switch (Kind)
		{
		case EBuildPieceKind::Brick:        return 0.5 * Settings.BrickSizeCm;
		case EBuildPieceKind::TimberPlate:  return FVector(33.75, 5.125, 5.0);
		case EBuildPieceKind::TimberLintel: return FVector(45.0, 5.125, 5.0);
		}

		/* A kind this build has never heard of gets no size at all — see the header. */
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

		/*
		 * The fail-closed row: Timber is not compression-dominant, so BuildMode::JointForContact
		 * infers DryStone against it — compression and friction, no tension, the weakest joint the
		 * inference can hand out.
		 */
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
		/*
		 * The printed number counts from one, the stored one does not: FSessionToolbarState::Course
		 * stays an array subscript, but Core/PieceMenu.cpp has named brick courses from one since it
		 * was written ("both numbers count from one"), so this is the surface that moved to agree.
		 */
		return FString::Printf(TEXT("Course %d"), SessionToolbarGroundedCourse(Course) + 1);
	}
}
