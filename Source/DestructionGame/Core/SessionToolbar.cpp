// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/SessionToolbar.h"

#include "Core/BuildMode/SnapSolver.h"

namespace DestructionSession
{
	/*
	 * EVERY FILE-LOCAL NAME IN HERE CARRIES A SessionToolbar PREFIX, for the reason
	 * Core/PieceMenu.cpp and Core/Structure.cpp both set out: an anonymous namespace is private to a
	 * TRANSLATION UNIT rather than to a file, a unity build merges many files into one, and two
	 * file-local names that collide are a hard compile error between files that never refer to each
	 * other.
	 */
	namespace
	{
		/**
		 * THE BRICK COURSE PITCH — one brick plus one bed joint — READ FROM THE SNAP SETTINGS.
		 *
		 * DERIVED, NEVER WRITTEN DOWN AGAIN. A course is the brick's own coordinating dimension and
		 * the snap solver already owns it; a second hand-written 7.5 here would be a coordinating
		 * grid spelled in two places, free to disagree the day the brick or the joint is retuned.
		 * That is DESIGN §3's rule about conversion constants applied to a coordinating dimension.
		 */
		double SessionToolbarCoursePitchCm()
		{
			const BuildMode::FSnapSettings Settings;

			return Settings.BrickSizeCm.Z + Settings.JointThicknessCm;
		}

		/**
		 * The course a number NAMES, which for anything below the ground is the grounded one.
		 *
		 * ONE CLAMP FOR THE WHOLE COURSE VOCABULARY. A below-ground course reading "not grounded" on
		 * one call and getting a course-0 build plane on the next is two functions disagreeing about
		 * a state that is not supposed to exist, and that disagreement is what would let it survive.
		 */
		int32 SessionToolbarGroundedCourse(int32 Course)
		{
			return Course > 0 ? Course : 0;
		}

		/*
		 * THE STRIP'S PALETTE, IN LINEAR — which is the number this model hands out and the number
		 * Slate takes. SESSION_UI_DESIGN §e gives every colour twice, the linear triple and the sRGB
		 * hex the designer checks it against, and says in as many words that confusing the two is how
		 * a palette drifts.
		 *
		 * THE TWO ACCENTS ARE THE COLOURS THIS UI ALREADY USES, WRITTEN OUT HERE BECAUSE THERE IS
		 * NOWHERE SHAREABLE TO READ THEM FROM. The amber is the Caution band's gold and the red is the
		 * destructive row's, and both live today as file-static constants inside
		 * DestructionGamePlayerController.cpp — a widget file this Core namespace must not depend on,
		 * and one nothing else can include. So they are transcribed once, here, where the model that
		 * decides the look can reach them; the widget no longer keeps its own copy.
		 *
		 * AND THE TWO SWATCHES ARE THE SHED MATERIALS' OWN BASE COLOURS, from
		 * Scripts/Author-ShedMaterials.py — M_Shed_Brick is (0.35, 0.06, 0.04) and M_Shed_Timber is
		 * (0.45, 0.22, 0.09). The palette chip and the piece that lands are meant to be one DECISION;
		 * they are not one pixel, because the swatch is a flat Slate fill over a near-black bar and
		 * the brick is a lit surface, which is the same caveat RequiredContent.h's neighbour palette
		 * carries. Nobody may retune one of these to match a screenshot.
		 */
		const FLinearColor SessionToolbarBuildAccent(0.95f, 0.66f, 0.13f, 1.0f);
		const FLinearColor SessionToolbarDestroyAccent(0.72f, 0.16f, 0.14f, 1.0f);
		const FLinearColor SessionToolbarBrickSwatch(0.35f, 0.06f, 0.04f, 1.0f);
		const FLinearColor SessionToolbarTimberSwatch(0.45f, 0.22f, 0.09f, 1.0f);

		/*
		 * AND THE THREE VISUAL STATES' OWN COLOURS.
		 *
		 * THE IDLE FILL IS THE PANEL'S INACTIVE-TAB SLATE, which is dark enough to sit under a caption
		 * and solid enough not to read as greyed. The greyed fill is the same colour faded: the model
		 * has already refused the click, and what the fade stops is the strip telling the player the
		 * click was going to do something.
		 *
		 * DARK INK ON A LIT CHIP, because the lit chip is filled with an accent that is bright in
		 * display terms — amber especially — and a pale caption on it is unreadable at exactly the
		 * glance the lit chip exists for.
		 *
		 * AND THE DANGER CAPTION IS WARM RATHER THAN RED-FILLED. Clear build is the one irreversible
		 * control in the Build group, but a chip filled destroy-red sitting on an amber strip would
		 * read as the mode you are in; a caption tinted toward the red reads as a warning.
		 */
		const FLinearColor SessionToolbarIdleFill(0.16f, 0.18f, 0.24f, 0.75f);
		const FLinearColor SessionToolbarDisabledFill(0.16f, 0.18f, 0.24f, 0.30f);
		const FLinearColor SessionToolbarIdleCaption(0.82f, 0.86f, 0.92f, 1.0f);
		const FLinearColor SessionToolbarDisabledCaption(0.55f, 0.60f, 0.68f, 0.45f);
		const FLinearColor SessionToolbarDarkInkCaption(0.02f, 0.015f, 0.01f, 1.0f);
		const FLinearColor SessionToolbarDangerCaption(0.95f, 0.55f, 0.50f, 1.0f);

		/*
		 * THE DROP EDGE — a shadow under the chip on anything clickable, and nothing at all on a chip
		 * that is not. §e's disabled row asks for no edge; the GEOMETRY stays uniform (every chip is
		 * rounded the same and edged the same width, so the strip is one row of one shape) and it is
		 * the edge's own alpha that takes it away.
		 */
		const FLinearColor SessionToolbarChipEdge(0.0f, 0.0f, 0.0f, 0.35f);
		const FLinearColor SessionToolbarNoEdge(0.0f, 0.0f, 0.0f, 0.0f);

		/* §a principle 1's chunky rounded chip with its 2 px drop edge, on every chip of every strip. */
		constexpr float SessionToolbarChipCornerRadiusPx = 10.0f;
		constexpr float SessionToolbarChipOutlineWidthPx = 2.0f;

		/**
		 * Which region of the strip a button sits in.
		 *
		 * THE DEFAULT ARM IS Command, which is the fail-closed end for the reason the field's own
		 * default is: a button this build has never heard of draws past the last rule on its own,
		 * rather than joining the mode pair that may never move.
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
			case EToolbarButtonId::PlacementSnap:
			case EToolbarButtonId::PlacementFree:
			case EToolbarButtonId::CourseDown:
			case EToolbarButtonId::CourseUp:
				return EToolbarGroup::Settings;

			default:
				return EToolbarGroup::Command;
			}
		}

		/**
		 * Which piece a chip lays, if it lays one.
		 *
		 * BOTH BOARDS ARE Timber, because they are one material at two lengths — BuildPieceMaterial
		 * hands the same library row to the plate and the lintel, and a swatch that disagreed with it
		 * would be the chip promising a piece the placement does not lay.
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
		 * A BUTTON THIS BUILD DOES NOT KNOW GETS A WORD OF ITS OWN rather than sharing one, for the
		 * reason PresenterWordForJointRole's "no tier" exists: EToolbarButtonId is a uint8 and a
		 * cast is all it takes to make one, and a caption that reads like a real button is worse
		 * than one that is visibly not.
		 */
		FString SessionToolbarCaption(EToolbarButtonId Id)
		{
			switch (Id)
			{
			case EToolbarButtonId::ModeBuild:         return TEXT("Build");
			case EToolbarButtonId::ModeDestroy:       return TEXT("Destroy");
			case EToolbarButtonId::PieceBrick:        return TEXT("Brick");
			case EToolbarButtonId::PieceTimberPlate:  return TEXT("Timber plate");
			case EToolbarButtonId::PieceTimberLintel: return TEXT("Timber lintel");
			case EToolbarButtonId::PlacementSnap:     return TEXT("Snap");
			case EToolbarButtonId::PlacementFree:     return TEXT("Free");
			case EToolbarButtonId::CourseDown:        return TEXT("Course down");
			case EToolbarButtonId::CourseUp:          return TEXT("Course up");
			case EToolbarButtonId::ClearBuild:        return TEXT("Clear build");
			case EToolbarButtonId::RunStructure:      return TEXT("Run structure");
			}

			return TEXT("(no button)");
		}

		/**
		 * Whether a button is the one its group's setting names.
		 *
		 * A COMMAND IS NEVER LIT, which is the default arm rather than an omission: Clear and Run
		 * are things that HAPPEN, and a latched-looking command reads as a mode the player is stuck
		 * in.
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
			default:                                  return false;
			}
		}

		/**
		 * Whether the thing behind a button can actually happen.
		 *
		 * THE THREE PRECONDITIONS ARE THE WHOLE LIST, and everything else being live is a decision
		 * rather than an oversight: a mode button greyed by an over-eager precondition is a player
		 * who cannot get out of the mode they are in.
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

			default:
				return true;
			}
		}
	}

	TArray<FToolbarButton> SessionToolbarButtons(const FSessionToolbarState& State)
	{
		const bool bBuilding = State.Mode == ESessionMode::Build;

		/*
		 * THE MODE PAIR FIRST IN BOTH LISTS. Everything after it changes with the mode; the two
		 * buttons that switch modes may not move, or a strip whose first two slots shifted would put
		 * a different button under a cursor that has not moved.
		 */
		const TArray<EToolbarButtonId> Ids = bBuilding
			? TArray<EToolbarButtonId>{
				EToolbarButtonId::ModeBuild,
				EToolbarButtonId::ModeDestroy,
				EToolbarButtonId::PieceBrick,
				EToolbarButtonId::PieceTimberPlate,
				EToolbarButtonId::PieceTimberLintel,
				EToolbarButtonId::PlacementSnap,
				EToolbarButtonId::PlacementFree,
				EToolbarButtonId::CourseDown,
				EToolbarButtonId::CourseUp,
				EToolbarButtonId::ClearBuild }
			: TArray<EToolbarButtonId>{
				EToolbarButtonId::ModeBuild,
				EToolbarButtonId::ModeDestroy,
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
		 * THE STRIP IS ASKED RATHER THAN RE-DECIDED, and that is the whole reason this function is
		 * written this way round. A button the state does not draw, or draws greyed, is a bitwise
		 * no-op; deciding "can this happen" a second time here is precisely how a lit button that
		 * does nothing — or a greyed one that quietly acts — gets shipped.
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

		case EToolbarButtonId::CourseDown:
			/*
			 * NO CLAMP HERE, AND THAT IS THE POINT. The floor is the greying above, so the refused
			 * click leaves the state alone bit for bit rather than landing on a number that happens
			 * to be the same. The two are the same answer today and stop being the same answer the
			 * moment anything else on the state moves with a course change.
			 */
			After.Course = State.Course - 1;
			break;

		case EToolbarButtonId::CourseUp:
			After.Course = State.Course + 1;
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
		 * ANYTHING THAT IS NOT Build ANSWERS WITH THE DESTROY ACCENT, and that is written as a
		 * comparison rather than as a switch with a default arm on purpose: SessionToolbarButtons
		 * draws the Destroy strip for anything that is not Build (`bBuilding = Mode == Build`), so a
		 * mode this build has never heard of gets the Destroy strip AND the Destroy accent. Two
		 * answers derived the same way cannot come apart; a switch here with its own fallback is
		 * exactly where they would.
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
		 * None AND ANY KIND NOBODY DECLARED DRAW NOTHING. The swatch goes through one widget whatever
		 * it is, so "nothing to draw" has to be a colour — and a plausible block of colour on a chip
		 * that lays nothing would name a piece that chip cannot lay.
		 */
		return FLinearColor::Transparent;
	}

	FChipLook ChipLookFor(const FToolbarButton& Button, ESessionMode Mode)
	{
		FChipLook Look;

		/*
		 * THE GEOMETRY IS THE SAME ON EVERY CHIP OF EVERY STRIP, whatever state it is in. A strip
		 * whose chips changed shape with their state would read as several kinds of control; the
		 * state is said in colour and in weight, and the shape is what makes them all one row.
		 */
		Look.CornerRadiusPx = SessionToolbarChipCornerRadiusPx;
		Look.OutlineWidthPx = SessionToolbarChipOutlineWidthPx;

		if (!Button.bEnabled)
		{
			/*
			 * GREYED COMES FIRST, INCLUDING AHEAD OF THE "GO" CHIP BELOW. Run structure with nothing
			 * built is the one chip that is both filled-by-identity and refused, and it has to read as
			 * refused: a lit button that does nothing is the failure bEnabled exists for.
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
			 * THE MODE'S ACCENT, NOT THE BUTTON'S. Everything lit on a Build strip is amber and
			 * everything lit on a Destroy strip is red, so the colour of the strip is itself a reading
			 * of which mode the player is in — the fact they need from the corner of an eye while
			 * flying a camera.
			 */
			Look.Fill = ModeAccent(Mode);
			Look.Caption = SessionToolbarDarkInkCaption;
			Look.bBoldCaption = true;

			return Look;
		}

		if (Button.Id == EToolbarButtonId::RunStructure)
		{
			/*
			 * THE "GO" CHIP, AND IT IS WHY THIS FUNCTION TAKES A BUTTON. A command is never bActive —
			 * SessionToolbarIsActive's default arm guarantees it, because a latched command reads as a
			 * mode the player is stuck in — and §e asks for Run structure filled in the destroy accent
			 * anyway. Written as `bActive ? Accent : Idle` the two are irreconcilable; keyed on the
			 * button they are simply two different chips.
			 *
			 * THE DESTROY ACCENT BY NAME RATHER THAN THE MODE'S, which costs nothing today (Run is
			 * drawn in Destroy mode alone) and says the right thing: this chip is red because of what
			 * it does, not because of where it is.
			 */
			Look.Fill = SessionToolbarDestroyAccent;
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

	FVector BuildPieceHalfExtentCm(EBuildPieceKind Kind)
	{
		/*
		 * THE BRICK IS READ FROM THE SNAP SETTINGS AND THE TIMBER IS THE DEMO BUILDING'S OWN BOARD.
		 * The brick's dimensions already live in BuildMode::FSnapSettings, so halving them is what
		 * stops the toolbar becoming a third place they are written down; the plate is
		 * Core/BuildMode/DemoBuilding.cpp's (33.75, 5.125, 5.0) transcribed, and the lintel is that
		 * plate's 90 cm sibling, sharing its section so the two bear identically.
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
		 * THE FAIL-CLOSED ROW, AND Timber IS THE FAIL-CLOSED ANSWER. It is not
		 * compression-dominant, so BuildMode::JointForContact infers DryStone against it — a bearing
		 * that carries compression and friction and no tension, the weakest joint the inference can
		 * hand out. A piece nobody declared is credited with nothing it has not earned.
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
		 * THE PRINTED NUMBER COUNTS FROM ONE AND THE STORED ONE DOES NOT, WHICH IS THE WHOLE OF THIS
		 * LINE. FSessionToolbarState::Course is an array subscript and stays one — CoursePlaneZCm
		 * and IsCourseGrounded are arithmetic over it and are untouched — but a person counting
		 * courses of brick starts at one, and Core/PieceMenu.cpp has named the bricks that way since
		 * it was written ("BOTH NUMBERS COUNT FROM ONE"). Two surfaces naming one course had to
		 * agree, and this is the one that moved.
		 */
		return FString::Printf(TEXT("Course %d"), SessionToolbarGroundedCourse(Course) + 1);
	}
}
