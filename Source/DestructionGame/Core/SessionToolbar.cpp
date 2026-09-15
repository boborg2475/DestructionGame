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
