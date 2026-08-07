// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceMenu.h"

/*
 * EVERY NAME IN HERE CARRIES A Presenter PREFIX, for the reason Structure.cpp's header
 * comment sets out at length: an anonymous namespace is private to a TRANSLATION UNIT
 * rather than to a file, a unity build merges many files into one, and two file-local
 * names that collide are then a hard compile error between files that never refer to
 * each other.
 */
namespace
{
	/**
	 * A joint's tier, in the words a player reads.
	 *
	 * THE WORDING LIVES HERE BECAUSE THE WIDGET MAY NOT HAVE IT. Choosing a word for a
	 * tier is a decision, and the menu widget was landed under a recorded exception to
	 * the TDD gate on the condition that it holds no decisions at all.
	 *
	 * None has a word of its own rather than sharing one. InspectPiece only ever lists
	 * real joints on a real piece and AddConnection refuses a normal that will not
	 * normalise, so no row can carry it today — but a tier that reads as one of the
	 * three real ones would be the fail-open direction the whole GetJointRole contract
	 * is written against.
	 */
	const TCHAR* PresenterWordForJointRole(EJointRole Role)
	{
		switch (Role)
		{
		case EJointRole::BedBeneath: return TEXT("bed below");
		case EJointRole::BedAbove:   return TEXT("bed above");
		case EJointRole::Head:       return TEXT("head");
		case EJointRole::None:       return TEXT("no tier");
		}

		return TEXT("no tier");
	}

	/**
	 * WHAT THE PANEL CALLS ITSELF, IN EVERY STATE THERE IS.
	 *
	 * A CONSTANT STRING IS STILL A STRING THE WIDGET MAY NOT SPELL, for the reason every other
	 * word on this struct lives here: choosing it is a decision, and a decision taken in Slate
	 * is taken where nothing can read it. The branch count happens to be zero today; the day it
	 * gains one — a word that changes with the mode, a count folded into it — the decision is
	 * already on the testable side of the seam rather than needing to be moved there.
	 */
	const TCHAR* const PresenterPanelHeader = TEXT("Selection");

	/**
	 * WHAT THE READOUT REGION SAYS WHEN THERE IS NO BRICK TO BREAK OUT.
	 *
	 * A FIXED PANEL RESERVES THE READOUT'S SPACE WHETHER OR NOT IT HAS ONE, so the alternative
	 * is a hole in the middle of the panel that reads as a readout that failed rather than as
	 * one waiting to be asked. It is the same rule "No bricks selected" and "No joints" already
	 * follow: an absence is a fact to be stated.
	 */
	const TCHAR* const PresenterInspectedHint = TEXT("Hover a brick in the list to see its joints");

	/**
	 * Why a brick is or is not being held up, as the BUCKET everything else about it is read off.
	 *
	 * "THIS REF NAMES NO BRICK" IS ASKED FIRST AND "NOBODY HAS SOLVED YET" SECOND, AND BOTH
	 * ORDERINGS ARE THE WHOLE POINT OF THE FUNCTION.
	 *
	 * A brick a removal took, a ref naming another wall and a ref missing a half all come
	 * back bIsPiece false with a default support answer — and the bucket that falls out of
	 * the guard below for one of those is NotSolved, which promises the brick is there and
	 * that a solve is all it is waiting for. On a freshly built wall it is also
	 * indistinguishable from every live row on the panel. So a ref that names nothing says
	 * exactly that, and says it before anything is read off the enumerators.
	 *
	 * EPieceSupport::Falling is then both a real collapse and an absent answer —
	 * deliberately, because enumerator zero has to promise least — so a readout that went
	 * straight to the enumerator would draw a freshly built, never-solved wall as a column
	 * of falling bricks: a catastrophe reported that has not happened.
	 *
	 * ONE FUNCTION FOR THE ENTRY ROW AND FOR THE READOUT UNDER IT, WHICH IS WHY THE FIRST
	 * GUARD LIVES HERE RATHER THAN AT THE ONE CALL SITE THAT CAN REACH IT. Two inches apart
	 * on one panel, one brick may not read "supported" in the list and something else over
	 * its joints, and two derivations of one question are how that happens.
	 */
	EPieceSupportBand PresenterSupportBand(const FPieceInspection& Inspection)
	{
		if (!Inspection.bIsPiece)
		{
			return EPieceSupportBand::NotAPiece;
		}

		if (!Inspection.bHasSupportAnswer)
		{
			return EPieceSupportBand::NotSolved;
		}

		switch (Inspection.Support)
		{
		case EPieceSupport::Grounded:  return EPieceSupportBand::Grounded;
		case EPieceSupport::Supported: return EPieceSupportBand::Supported;
		case EPieceSupport::Stranded:  return EPieceSupportBand::Stranded;
		case EPieceSupport::Falling:   return EPieceSupportBand::Falling;
		}

		return EPieceSupportBand::Falling;
	}

	/**
	 * That bucket in the words a player reads — AND THE BUCKET IS THE ONLY THING IT ASKS.
	 *
	 * THE WORD IS DERIVED FROM THE BUCKET RATHER THAN BESIDE IT, WHICH IS WHAT MAKES THE TWO
	 * UNABLE TO DISAGREE. The dot on a row and the word beside it answer one question, and a
	 * model that decided each from the inspection separately would be two derivations of that
	 * question sitting a few pixels apart — a row reading "grounded" next to the colour this
	 * game uses for a brick that is coming down. Core/PieceMenu.h says at length why the bucket
	 * exists at all; this is the half that keeps it honest.
	 *
	 * ONE SENTENCE PER BUCKET, INCLUDING THE TWO THAT ARE NOT PHYSICAL STATES. Two buckets
	 * sharing a sentence would make them indistinguishable to a reader while still being two
	 * colours, which is the worst of both.
	 */
	FString PresenterWordForSupportBand(EPieceSupportBand Band)
	{
		switch (Band)
		{
		case EPieceSupportBand::NotAPiece: return FString(TEXT("not in this wall"));
		case EPieceSupportBand::NotSolved: return FString(TEXT("not solved yet"));
		case EPieceSupportBand::Grounded:  return FString(TEXT("grounded"));
		case EPieceSupportBand::Supported: return FString(TEXT("supported"));
		case EPieceSupportBand::Stranded:  return FString(TEXT("stranded"));
		case EPieceSupportBand::Falling:   return FString(TEXT("falling"));
		}

		return FString(TEXT("falling"));
	}

	/**
	 * How many joints a brick has, in words — INCLUDING WHEN IT HAS NONE.
	 *
	 * AN EMPTY LIST GETS A SENTENCE, exactly as CountText's "No bricks selected" does, and
	 * for the same reason: a widget left to notice Joints.Num() == 0 for itself would hold
	 * a decision in the one place no test can reach. An isolated grounded pad is a real
	 * brick with nothing joined to it, and that is a fact about the brick rather than an
	 * absence of data. Singular and plural are decided here too — "1 joints" is the same
	 * branch wearing a smaller coat.
	 */
	FString PresenterWordForJointCount(int32 JointCount)
	{
		if (JointCount == 0)
		{
			return FString(TEXT("No joints"));
		}

		if (JointCount == 1)
		{
			return FString(TEXT("1 joint"));
		}

		return FString::Printf(TEXT("%d joints"), JointCount);
	}

	/**
	 * How many bricks a row will act on, as a noun phrase — "1 brick", "11 bricks".
	 *
	 * SINGULAR AND PLURAL ARE DECIDED HERE FOR THE REASON CountText'S ARE: "1 bricks" is a
	 * branch, and a branch in Slate is untested by construction. There is no wording for
	 * nothing at all, and that is not an omission — a row that exists always carries at
	 * least one ref, because a menu with no target is a button with nothing behind it.
	 */
	FString PresenterWordForTargetCount(int32 BrickCount)
	{
		return BrickCount == 1
			? FString(TEXT("1 brick"))
			: FString::Printf(TEXT("%d bricks"), BrickCount);
	}

	/**
	 * HOW FAR APART TWO CENTRES MAY SIT IN Z AND STILL BE ONE COURSE.
	 *
	 * Half a centimetre, against a course pitch of 7.5 cm — the 6.5 cm brick this game lays
	 * plus one mortar joint, which DestructionLayout::RunningBond computes. The band is
	 * therefore a fifteenth of the smallest real gap between two courses: room enough for
	 * float noise and a brick modelled a couple of millimetres off nominal, nowhere near
	 * enough to swallow a course.
	 *
	 * THE TWO FAILURE DIRECTIONS ARE NOT SYMMETRIC, which is why this is a band rather than
	 * an equality and why it sits far closer to the tight end than the middle. Too tight and
	 * a wall reads as eighty courses of one brick each — useless, and obviously useless. Too
	 * loose and two real courses merge, at which point two DIFFERENT bricks compete for one
	 * position number: the readout goes on looking perfectly ordinary while naming the wrong
	 * brick, which is the whole failure the label rule exists to prevent.
	 *
	 * KNOWN SCALE ASSUMPTION, RECORDED RATHER THAN SOLVED: it is an absolute distance, so a
	 * structure built of pieces under about a centimetre tall would band its courses
	 * together. Nothing in the game builds one, and the adaptive alternative — a fraction of
	 * each piece's own height — makes "same course as" non-transitive across mixed sizes,
	 * which would leave the banding depending on visiting order.
	 */
	constexpr double PresenterCourseToleranceCm = 0.5;

	/** One piece on its way to a position: where it is, and which course it landed in. */
	struct FPresenterPlacedPiece
	{
		int32 Handle = INDEX_NONE;
		FVector CentreCm = FVector::ZeroVector;
		int32 Course = INDEX_NONE;
	};

	/**
	 * Whether a centre can be banded and ordered at all.
	 *
	 * A NaN DOES NOT MERELY GET THE WRONG COURSE. Every comparison against one is false, so
	 * it breaks the strict weak ordering the sort below is entitled to assume — undefined
	 * behaviour rather than a bad label. An infinity orders perfectly well and then bands
	 * into a course of its own whose NUMBER depends on where the sort happened to put it,
	 * which is unstable as well as meaningless. Both are excluded here, so a piece nobody
	 * can place neither takes a position nor disturbs anybody else's.
	 */
	bool PresenterHasUsableCentre(const FVector& CentreCm)
	{
		return FMath::IsFinite(CentreCm.X)
			&& FMath::IsFinite(CentreCm.Y)
			&& FMath::IsFinite(CentreCm.Z);
	}

	/**
	 * Along one course: X, then Y, then the piece handle.
	 *
	 * X ALONE IS NOT ENOUGH, and the case that breaks it is ordinary masonry rather than a
	 * pathological input: a wall two leaves thick puts two bricks of one course at the same
	 * X, differing only in depth, and they would then share a position number — two
	 * different bricks presenting as one string. Y settles those, and the handle settles two
	 * pieces at exactly the same point, which no wall produces but a caller can hand in. The
	 * result is a TOTAL order over a course's pieces, so no two of them can share an ordinal.
	 */
	bool PresenterIsEarlierAlongCourse(
		const FPresenterPlacedPiece& A,
		const FPresenterPlacedPiece& B)
	{
		if (A.CentreCm.X != B.CentreCm.X)
		{
			return A.CentreCm.X < B.CentreCm.X;
		}

		if (A.CentreCm.Y != B.CentreCm.Y)
		{
			return A.CentreCm.Y < B.CentreCm.Y;
		}

		return A.Handle < B.Handle;
	}

	/**
	 * Every piece of a binding named by WHERE IT IS — "course 2 · #1" — in handle order.
	 *
	 * A POSITION, BECAUSE AN ARRAY SUBSCRIPT IS NOT A PLACE. "brick 4:282" says which slot
	 * of which array a brick is in, which is exactly the thing a person standing in front of
	 * a wall does not have and cannot check; a course and a place along it is how the wall
	 * was built and how anybody would point at it. It is derivable HERE and only here —
	 * FStructure is position-free on purpose, and the binding is the layer holding the boxes.
	 *
	 * A HANDLE THIS BINDING CANNOT PLACE GETS AN EMPTY STRING, and the caller falls back to
	 * the old ref-shaped label rather than inventing a position.
	 */
	TArray<FString> PresenterPositionLabels(const FStructureBinding& Binding)
	{
		TArray<FString> Labels;
		Labels.SetNum(Binding.NumPieces());

		TArray<FPresenterPlacedPiece> Placed;
		Placed.Reserve(Binding.NumPieces());

		/*
		 * A REMOVED PIECE IS STILL PLACED, and FPieceBinding keeping the box of a piece that
		 * has gone is exactly what that is for. A course that renumbered itself when a brick
		 * was pulled out of it would change the name of every brick to that brick's right,
		 * at the very moment a player is looking at them — a label may never depend on what
		 * has been done to a neighbour.
		 */
		for (int32 Handle = 0; Handle < Binding.NumPieces(); ++Handle)
		{
			const FVector& CentreCm = Binding.GetBinding(Handle).Box.CentreCm;

			if (!PresenterHasUsableCentre(CentreCm))
			{
				continue;
			}

			FPresenterPlacedPiece& Piece = Placed.AddDefaulted_GetRef();
			Piece.Handle = Handle;
			Piece.CentreCm = CentreCm;
		}

		Placed.Sort([](const FPresenterPlacedPiece& A, const FPresenterPlacedPiece& B)
			{
				return A.CentreCm.Z != B.CentreCm.Z
					? A.CentreCm.Z < B.CentreCm.Z
					: PresenterIsEarlierAlongCourse(A, B);
			});

		/*
		 * BANDED AGAINST THE COURSE'S OWN FLOOR, NEVER AGAINST THE PIECE BEFORE IT.
		 *
		 * Comparing each piece to its predecessor is the obvious loop and it is wrong in a
		 * way nothing would notice: a run of pieces each a few millimetres above the last
		 * CHAINS, so forty of them merge into one "course" spanning sixteen centimetres —
		 * two real courses of a wall presented as one, with their position numbers
		 * interleaved. Anchoring to the floor bounds a course's total spread at the
		 * tolerance however many pieces arrive.
		 */
		int32 Course = 0;
		double CourseFloorZCm = 0.0;

		for (FPresenterPlacedPiece& Piece : Placed)
		{
			const bool bJoinsCourse = Course > 0
				&& (Piece.CentreCm.Z - CourseFloorZCm) <= PresenterCourseToleranceCm;

			if (!bJoinsCourse)
			{
				++Course;
				CourseFloorZCm = Piece.CentreCm.Z;
			}

			Piece.Course = Course;
		}

		Placed.Sort([](const FPresenterPlacedPiece& A, const FPresenterPlacedPiece& B)
			{
				return A.Course != B.Course
					? A.Course < B.Course
					: PresenterIsEarlierAlongCourse(A, B);
			});

		/*
		 * BOTH NUMBERS COUNT FROM ONE, because a person counting courses of brick starts at
		 * one and always has. A zero-based course is the array subscript this label exists
		 * to stop printing, wearing a different name.
		 */
		int32 PositionInCourse = 0;
		int32 CurrentCourse = INDEX_NONE;

		for (const FPresenterPlacedPiece& Piece : Placed)
		{
			PositionInCourse = Piece.Course == CurrentCourse ? PositionInCourse + 1 : 1;
			CurrentCourse = Piece.Course;

			Labels[Piece.Handle] =
				FString::Printf(TEXT("course %d · #%d"), Piece.Course, PositionInCourse);
		}

		return Labels;
	}

	/**
	 * How many newtons make one kilonewton.
	 *
	 * NOT A CONVERSION BOUNDARY, AND DELIBERATELY NOT NAMED LIKE ONE. Turning Unreal force
	 * units into newtons is the boundary, it is DestructionPresenter::ForceUnitsPerNewton,
	 * and it has already happened by the time anything here runs. A kilonewton is a thousand
	 * newtons by definition of the prefix, so this is a choice of how to PRINT a number
	 * rather than a change of what it means — which is why it lives beside the formatting.
	 */
	constexpr double PresenterNewtonsPerKilonewton = 1000.0;

	/**
	 * What a joint carries, in whichever of the two units reads better.
	 *
	 * 91200.0 N is a number a reader has to count the digits of and 91.2 kN is not. The
	 * switch is AT a thousand rather than above it: at exactly the boundary "1.0 kN" is what
	 * a reader wants and "1000.0 N" is the digit counting the switch exists to stop.
	 */
	FString PresenterForceText(double ForceN)
	{
		return ForceN >= PresenterNewtonsPerKilonewton
			? FString::Printf(TEXT("%.1f kN"), ForceN / PresenterNewtonsPerKilonewton)
			: FString::Printf(TEXT("%.1f N"), ForceN);
	}

	/**
	 * What is levering this joint open, as a clause to hang off the end of its line — and
	 * NOTHING AT ALL on a joint nothing is levering.
	 *
	 * THE ABSENCE IS THE HALF THAT BITES. A settled wall bends nowhere: a brick on two
	 * symmetric bed patches has its centre of mass at the area-weighted centroid of its
	 * supports, so the eccentricity is zero EXACTLY rather than nearly. Almost every joint a
	 * player ever looks at therefore has nothing to say here, and a clause appended anyway
	 * would make the common case worse to read for the sake of the rare one.
	 *
	 * SO THE TEST IS AGAINST EXACT ZERO, WHICH IS A STATEMENT RATHER THAN A TOLERANCE — see
	 * FStructure::GetConnectionMoment. A centred load, a piece nobody placed and a joint whose
	 * rectangle nobody measured all produce it exactly. And the polarity is the one that keeps
	 * a fault visible: a moment that is not a number is not equal to zero, so it prints its
	 * clause rather than going quiet, which is the same direction the force and the per cent
	 * beside it already take.
	 *
	 * N·cm, AND NO NEW CONVERSION BOUNDARY. The number arrives already converted by
	 * DestructionPresenter::ForceUnitsPerNewton, applied once where ForceN is; a moment is
	 * uu.cm and this game's length unit is the centimetre, so the centimetre rides through
	 * untouched. Metres would collapse the interesting range — 313.6 N·cm is 3.136 N·m, where
	 * one step of the last printed digit is a whole 10 N·cm.
	 */
	FString PresenterBendingText(double MomentNCm)
	{
		return MomentNCm == 0.0
			? FString()
			: FString::Printf(TEXT("  %.1f N·cm bending"), MomentNCm);
	}

	/** What a joint's utilisation reads when it is carrying exactly all it can. */
	constexpr double PresenterFullLoadPercent = 100.0;

	/** At or above this many times its load, a joint's margin loses its decimal. */
	constexpr double PresenterWholeMarginAtTimes = 100.0;

	/**
	 * How many times its load this joint could take, in words.
	 *
	 * WHY MARGIN AT ALL, WHEN THE PERCENTAGE IS ALREADY ON THE ROW. "0.049 %" means
	 * something only to a reader who already knows that 100 % is failure and that masonry in
	 * compression sits three or four orders of magnitude under it, and nothing on the panel
	 * says either. "2041× margin" says the whole thing in a phrase. It is the RECIPROCAL of
	 * a number already on the row rather than a second derivation, so the exact-equality
	 * sweep the readout test holds ForceN and UtilisationPercent to keeps holding untouched.
	 *
	 * THE THREE READINGS THAT ARE NOT A NUMBER ARE THE POINT, because each is a state where
	 * the plain reciprocal produces something plausible and wrong:
	 *
	 *   - A JOINT THAT HAS GIVEN carries nothing, so the arithmetic files it under "no
	 *     load" — an intact unloaded joint and a hole in the wall reading identically, which
	 *     is the exact defect FJointInspection::bHasGiven exists to prevent, reappearing one
	 *     layer out in a new field.
	 *   - A JOINT AT OR PAST ITS LIMIT divides into something no bigger than one, so the
	 *     formula goes on answering: a joint at twice capacity reads "0.5× margin", which is
	 *     the word MARGIN beside a joint that has none. At and past the limit are one
	 *     sentence deliberately — both have nothing left, and a second wording would be a
	 *     branch whose only purpose is decoration.
	 *   - AN UNLOADED JOINT divides by zero. Infinite margin is true and useless.
	 *
	 * THE ORDER OF THE GUARDS IS THE FAIL-CLOSED ONE AND IS NOT FREE. Every comparison
	 * against a NaN is false, so `!(Percent < Full)` is the branch a NaN falls into: a
	 * degenerate utilisation reads as a joint with nothing left rather than as a healthy
	 * one, which is the direction that is cheap to be wrong in.
	 */
	FString PresenterMarginText(double UtilisationPercent, bool bHasGiven)
	{
		if (bHasGiven)
		{
			return FString(TEXT("gone"));
		}

		if (!(UtilisationPercent < PresenterFullLoadPercent))
		{
			return FString(TEXT("no margin left"));
		}

		if (!(UtilisationPercent > 0.0))
		{
			return FString(TEXT("no load"));
		}

		const double MarginTimes = PresenterFullLoadPercent / UtilisationPercent;

		/*
		 * A tenth of a multiple that large is noise, so it is dropped — but only above the
		 * boundary. The two rungs either side of it are what make the rule falsifiable.
		 */
		return MarginTimes >= PresenterWholeMarginAtTimes
			? FString::Printf(TEXT("%.0f× margin"), MarginTimes)
			: FString::Printf(TEXT("%.1f× margin"), MarginTimes);
	}

	/**
	 * How many decades of margin the headroom bar spans.
	 *
	 * A LINEAR BAR IS EMPTY FOREVER, which is the whole reason the scale is logarithmic. A
	 * settled brick wall sits near 0.0005 of capacity, so a bar drawn on utilisation
	 * directly is a flat zero at every joint of every structure the game currently builds.
	 * Full is 1000× margin, empty is the joint giving, and each decade is a third of the
	 * bar. The consequence is that most joints peg it full, and that is honest: they
	 * genuinely are three orders of magnitude from failing.
	 */
	constexpr int32 PresenterHeadroomDecades = 3;

	/** The bar's fill for a margin, and the same curve the scale's ticks are placed by. */
	double PresenterHeadroomForMargin(double MarginTimes)
	{
		return FMath::Clamp(
			FMath::LogX(10.0, MarginTimes) / static_cast<double>(PresenterHeadroomDecades),
			0.0,
			1.0);
	}

	/**
	 * How full one joint's headroom bar is, 0 to 1.
	 *
	 * UNLOADED IS FULL AND GIVEN IS EMPTY, which is the bar's copy of the rule bHasGiven
	 * exists for: both read 0 N at 0 %, and one of them is a hole in the wall. Drawing a
	 * full bar beside a hole is the single worst thing this panel could do, so the two ends
	 * of the bar are exactly where the two states land.
	 *
	 * THE GUARDS RUN IN THE SAME ORDER AS PresenterMarginText'S, and for the same reason: a
	 * NaN utilisation falls into `!(Percent < Full)` and empties the bar rather than filling
	 * it. Asking "is it unloaded" first would fill it instead, because every comparison
	 * against a NaN is false.
	 */
	double PresenterHeadroomFraction(double UtilisationPercent, bool bHasGiven)
	{
		if (bHasGiven)
		{
			return 0.0;
		}

		if (!(UtilisationPercent < PresenterFullLoadPercent))
		{
			return 0.0;
		}

		if (!(UtilisationPercent > 0.0))
		{
			return 1.0;
		}

		return PresenterHeadroomForMargin(PresenterFullLoadPercent / UtilisationPercent);
	}

	/**
	 * WHERE A BAR STOPS BEING COMFORTABLE, AND WHERE IT BECOMES CRITICAL — as utilisations,
	 * because that is the number already on the row.
	 *
	 * Ten per cent of capacity is 10x margin and fifty per cent is 2x. They are stated in per
	 * cent rather than in multiples so the guards below compare against UtilisationPercent
	 * directly: a reciprocal taken first would divide by zero for the unloaded joint that is
	 * the most comfortable one there is, and would then need a guard of its own to say so.
	 */
	constexpr double PresenterCautionAtPercent = 10.0;
	constexpr double PresenterCriticalAtPercent = 50.0;

	/**
	 * Which band a joint's bar is drawn in.
	 *
	 * A BUCKET RATHER THAN A COLOUR. Which side of 10x a joint sits on is a decision about
	 * what this game calls dangerous, and a widget comparing a fraction against two constants
	 * would be holding that decision where nothing can read it. The hue stays the widget's.
	 *
	 * AT AN EDGE THE JOINT TAKES THE WORSE BAND, WHICH IS WHY THE GUARDS RUN WORST FIRST AND
	 * ARE WRITTEN NEGATED. It is the same shape as PresenterMarginText's "no margin left" at
	 * exactly 1.0: over-promising is the expensive direction on a panel whose whole job is to
	 * say what is about to fall down. And every comparison against a NaN is false, so a
	 * degenerate utilisation falls into the FIRST guard and is drawn as critical rather than
	 * as a joint three orders of magnitude from failing.
	 *
	 * A JOINT THAT HAS GIVEN IS CRITICAL WHATEVER ITS NUMBER SAYS. It carries nothing, so the
	 * arithmetic alone files a hole in the wall under "the most comfortable state there is" —
	 * which is exactly the defect bHasGiven exists to prevent, one field further out.
	 */
	EJointMarginBand PresenterMarginBand(double UtilisationPercent, bool bHasGiven)
	{
		if (bHasGiven)
		{
			return EJointMarginBand::Critical;
		}

		if (!(UtilisationPercent < PresenterCriticalAtPercent))
		{
			return EJointMarginBand::Critical;
		}

		if (!(UtilisationPercent < PresenterCautionAtPercent))
		{
			return EJointMarginBand::Caution;
		}

		return EJointMarginBand::Comfortable;
	}

	/**
	 * HOW MANY JOINT ROWS CAN CARRY A COLOUR AT ALL.
	 *
	 * SIX, AND IT IS THE WALL'S NUMBER RATHER THAN A ROUND ONE: a brick inside a running bond
	 * is spanned by two above, rests on two below and has a head joint either side. A palette
	 * that ran out before then would leave the ordinary case half-coloured.
	 */
	constexpr int32 PresenterColourSlots = 6;

	/**
	 * Which colour slot a joint row takes, or nothing once the palette has run out.
	 *
	 * PER SLOT, NOT PER BRICK. Keyed on the far-end brick is what a reader assumes, and it
	 * cannot be built: a wall is over a thousand bricks and a palette is a handful of legible
	 * hues, so it must collide — and two rows in one colour is a lie about the single thing a
	 * swatch says. Keyed on the ROW it never collides, at the price that one brick is the
	 * first colour in one readout and the second in another.
	 *
	 * AND PAST THE END, NOTHING RATHER THAN A WRAP. Wrapping is the tidy answer and it
	 * reintroduces the collision this was chosen to avoid, on the brick with the most joints —
	 * which is the brick being read hardest. An absent swatch is an absence; a repeated one is
	 * a wrong answer.
	 */
	int32 PresenterColourSlotFor(int32 RowIndex)
	{
		return RowIndex < PresenterColourSlots ? RowIndex : INDEX_NONE;
	}

	/**
	 * The bar's decade ticks, low to high.
	 *
	 * A LOG AXIS WITH NO TICKS IS UNREADABLE BY CONSTRUCTION — the same visible fill means
	 * 1000× on one panel and 3× on another, and nothing on screen says which. So they are
	 * not decoration, and they are placed by the SAME curve the fill is, rather than by four
	 * hand-written fractions: a caption promising a scale the arithmetic does not follow is
	 * a plausible-looking picture over a wrong number.
	 */
	/**
	 * Whether both halves of a pixel measurement are numbers at all.
	 *
	 * WRITTEN SO A NaN ANSWERS false RATHER THAN SLIPPING THROUGH. FMath::IsFinite already rejects
	 * both a NaN and an infinity, which is the pair that has to be caught before any comparison
	 * runs: every comparison against a NaN is false, so FMath::Max DISCARDS one and FMath::Min
	 * REPLACES it — either way a fault becomes a plausible offset a hundred pixels in, and nothing
	 * on screen says the screen's own size was never known.
	 */
	bool PresenterPanelPixelsAreFinite(const FVector2D& ValuePx)
	{
		return FMath::IsFinite(ValuePx.X) && FMath::IsFinite(ValuePx.Y);
	}

	/**
	 * Whether a size is a size — AND A NEGATIVE ONE IS NOT A SMALL PANEL.
	 *
	 * The permitted range is Viewport - Panel, so a negative subtrahend WIDENS it: a viewport of
	 * -1080 px would let the corner be dragged further out than any real screen allows, which is
	 * the fail-OPEN direction reached by arithmetic that reads perfectly reasonably on its own line.
	 *
	 * STATED AS `>= 0.0` RATHER THAN AS `!(< 0.0)`, WHICH IS THE SAME RULE THE NEGATED GUARDS
	 * ELSEWHERE IN THIS FILE FOLLOW AND NOT AN EXCEPTION TO IT. Both spellings exist to put a NaN
	 * on the FAULT side; this one answers a question whose false is the fault, so the comparison is
	 * the one a NaN fails. `!(X < 0.0)` would call a NaN a usable size.
	 *
	 * ZERO IS USABLE. A viewport of no size at all and a panel exactly as big as its screen both
	 * leave a range of zero, which pins the corner to the origin — a real answer rather than a
	 * degenerate one.
	 */
	bool PresenterPanelSizeIsUsable(const FVector2D& SizePx)
	{
		return SizePx.X >= 0.0 && SizePx.Y >= 0.0;
	}

	/**
	 * Whether a margin is a distance the panel can be stood off an edge by.
	 *
	 * A NEGATIVE MARGIN IS NOT A SMALLER MARGIN, WHICH IS THE WHOLE OF WHY THIS IS SEPARATE FROM
	 * "is it finite". It is a distance that pushes the panel PAST the edge it was measured from —
	 * a 640 px panel on a 1920 px screen at -24 px opens at 1304, twenty-four pixels of it hanging
	 * off the right — and ClampPanelOffset would then quietly pull it back to 1280, so the fault
	 * would be INVISIBLE rather than absent. It arrives from exactly the arithmetic a negative
	 * viewport does, and it is refused for the same reason.
	 *
	 * ZERO IS USABLE: the panel's right edge flush against the viewport's is a real placement, and
	 * it is the boundary the clamp already calls inside its own range.
	 *
	 * STATED AS `>= 0.0` RATHER THAN AS `!(< 0.0)`, exactly as PresenterPanelSizeIsUsable is and
	 * for the same reason: this answers a question whose false is the fault, so the comparison has
	 * to be the one a NaN fails.
	 */
	bool PresenterPanelMarginIsUsable(double MarginPx)
	{
		return MarginPx >= 0.0;
	}

	/**
	 * One axis of the panel's corner, pinned inside the room that axis has.
	 *
	 * THE NESTING IS THE WHOLE FUNCTION, AND IT IS Max(Min(...)) BECAUSE THE OTHER ORDER HANDS BACK
	 * A NEGATIVE OFFSET. A panel wider than its screen makes LargestPx negative, and an inverted
	 * range does not fail loudly: Min(Max(X, 0), Largest) returns Largest — the panel's own heading
	 * off the left edge of the screen, the one corner that has to stay grabbable — while this order
	 * returns 0. The two read identically at a glance, which is why the case is pinned by
	 * Presenter.PanelOffsetClamp rather than left to whichever spelling somebody reaches for.
	 *
	 * SPELLED OUT RATHER THAN DEFERRED TO FMath::Clamp, WHICH HAPPENS TO BE THIS ORDER TODAY. That
	 * is luck rather than a promise, and it is a promise this function needs.
	 */
	double PresenterPanelAxisPinned(double DesiredPx, double LargestPx)
	{
		return FMath::Max(FMath::Min(DesiredPx, LargestPx), 0.0);
	}

	/*
	 * HOW MUCH SCREEN THE FULL PANEL TAKES, AND NEITHER FIGURE IS PICKED.
	 *
	 * THE WIDTH IS A MEASURED FLOOR PLUS A STATED CLEARANCE. 617.5 px is the longest line this
	 * readout can compose — "#4  course 3 · #1  bed above  40.0 N  4.551 %  22.0× margin  150.0
	 * N·cm bending", laid out on the ragged corbel wall, which is the only wall shape that bends at
	 * all and so the only one from which this number is visible. World.Menu.TheReadoutFitsInside-
	 * ThePanel measures it. 640 px clears it by 22 px, about four characters at the readout's font,
	 * which is the room a course number in the hundreds still wants in the game's own 1,220-brick
	 * wall.
	 *
	 * THE HEIGHT IS A FIT RATHER THAN A MEASUREMENT: 560 px is about half a 1080 viewport, so the
	 * brick list, the readout and the action rows are all on screen at once.
	 *
	 * PINNED TO THE PIXEL BY Presenter.PieceMenuPanelSize, as characterisation rather than as a
	 * claim it can check: a Full arm that quietly drifted would take the measured floor with it,
	 * and the wall that can see that floor is expensive to build.
	 */
	constexpr double PresenterFullPanelWidthPx = 640.0;
	constexpr double PresenterFullPanelHeightPx = 560.0;

	/*
	 * AND HOW MUCH THE COMPACT PANEL TAKES, WHICH IS 0.7 OF EACH AXIS — HALF THE AREA.
	 *
	 * THE PLAYER'S COMPLAINT, VERBATIM: "it takes up so much of the screen." Compact already drops
	 * the joint table and the headroom scale, so the sentences that set the full width are gone;
	 * what remains is the brick list, whose widest row is a position label and a support word set in
	 * a fixed 150 px column anchored to the panel's right edge.
	 *
	 * THE BINDING CONSTRAINT IS NOT LINE CONTAINMENT — IT IS TWO LINES ON ONE ROW PRINTING OVER EACH
	 * OTHER, and that is the measurement this figure comes from. The support word's own slack is a
	 * constant 71 px however narrow the panel gets, because it is measured against a right edge the
	 * word is pinned to; what runs out is the gap between "course 12 · #3" and the column walking
	 * left towards it. Swept in World.Menu.TheReadoutFitsInsideThePanel, that gap closes at about
	 * 231 px of panel and is still 74 px clear at 380 px. 448 px is comfortably outside it.
	 *
	 * THE HEIGHT FOLLOWS THE SAME FRACTION so the panel keeps its proportions rather than becoming a
	 * band across the screen — EPieceMenuDetail::Compact has to be strictly smaller on BOTH axes,
	 * and 0.7 x 0.7 leaves 49 % of the area, inside the two thirds the mode's own description of
	 * itself promises.
	 */
	constexpr double PresenterCompactPanelWidthPx = 448.0;
	constexpr double PresenterCompactPanelHeightPx = 392.0;

	TArray<FHeadroomScaleTick> PresenterHeadroomScale()
	{
		TArray<FHeadroomScaleTick> Scale;
		Scale.Reserve(PresenterHeadroomDecades + 1);

		double MarginTimes = 1.0;

		for (int32 Decade = 0; Decade <= PresenterHeadroomDecades; ++Decade)
		{
			FHeadroomScaleTick& Tick = Scale.AddDefaulted_GetRef();
			Tick.Label = FString::Printf(TEXT("%.0f×"), MarginTimes);
			Tick.Fraction = PresenterHeadroomForMargin(MarginTimes);

			MarginTimes *= 10.0;
		}

		return Scale;
	}
}

TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	const FPieceRef& Ref)
{
	/*
	 * A MENU FOR ONE BRICK IS A MENU FOR A SELECTION OF ONE, spelled that way rather than
	 * duplicated: a menu for one brick is not a different kind of menu, and a second copy
	 * of the fail-closed rule below is a second policy free to drift from it.
	 */
	return BuildPieceMenuRows(Actions, TArrayView<const FPieceRef>(&Ref, 1));
}

TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	TArrayView<const FPieceRef> Refs)
{
	TArray<FPieceMenuRow> Rows;

	/*
	 * NO SELECTION BUILDS NO ROWS. A menu with no target is a button with nothing behind
	 * it, and a row that exists must always carry at least one piece to commit against —
	 * which is also what lets everything below take Refs.Last() without a second guard.
	 */
	if (Refs.Num() == 0)
	{
		return Rows;
	}

	/*
	 * AND A REF MISSING EITHER HALF BUILDS NOTHING, however good the menu handed in was,
	 * and however many of the other refs were perfect.
	 *
	 * A row is a COMMAND waiting to be chosen rather than a readout, and a default FPieceRef
	 * is both what a click on the floor arrives as and what "we have no answer" looks like.
	 * Offering the full menu against one would put a Delete button on screen with nothing
	 * behind it. Zero is a real structure and a real piece — the first one the game mode
	 * builds — so the sentinel is tested for by name rather than by truthiness.
	 *
	 * The whole list goes rather than the bad entry, for the reason PieceActionsFor gives:
	 * dropping one would put a button on screen that acts on fewer bricks than the player
	 * picked, and nothing about that reads as a bug.
	 */
	for (const FPieceRef& Ref : Refs)
	{
		if (Ref.StructureId == INDEX_NONE || Ref.PieceIndex == INDEX_NONE)
		{
			return Rows;
		}
	}

	Rows.Reserve(Actions.Num());

	/*
	 * ONE ROW PER ACTION, IN THE ORDER THEY WERE OFFERED, AND CanRun IS NOT CONSULTED.
	 * PieceActionsFor is what decides which actions a piece may offer and it has already
	 * run; asking again here would be a second, quieter copy of that policy.
	 */
	for (const FPieceAction* const Action : Actions)
	{
		/*
		 * A MALFORMED ROW IS SKIPPED RATHER THAN DEREFERENCED. AllPieceActions() cannot hold
		 * one, but this takes an array from anywhere, and dereferencing a null row would
		 * abort the whole run rather than dropping one entry.
		 */
		if (Action == nullptr || Action->Label == nullptr)
		{
			continue;
		}

		FPieceMenuRow& Row = Rows.AddDefaulted_GetRef();

		/*
		 * THE ACTION IS CARRIED BY POINTER AND THE LABEL IS TAKEN FROM IT, never spelled
		 * here: "which entry did they choose" is a pointer comparison against the shipped
		 * table, which a copied row would break while keeping every label right.
		 */
		Row.Label = FString(Action->Label);
		Row.Action = Action;
		Row.Refs.Append(Refs.GetData(), Refs.Num());

		/*
		 * AND WHETHER CHOOSING IT DESTROYS SOMETHING IS THE ACTION'S OWN FLAG, COPIED LIKE
		 * Label IS AND NEVER RE-DECIDED. A widget styling a button by reading its caption
		 * would be a policy written in string literals where no test can reach it, and it
		 * would stop styling the day the caption is retuned.
		 */
		Row.bIsDestructive = Action->bIsDestructive;

		/*
		 * AND WHAT IT WILL ACT ON, IN WORDS, DERIVED FROM THE SET IT COMMITS AGAINST. Taken
		 * from Refs rather than from the selection the caller happens to remember, so a
		 * button cannot promise to act on a different number of bricks than it will.
		 */
		Row.TargetText = PresenterWordForTargetCount(Row.Refs.Num());

		/*
		 * THE ANCHOR IS DERIVED FROM THE SET, NEVER STORED BESIDE IT — the same shape as
		 * Label, which is a copy of the action's own text. A field naming a piece that is
		 * not in the selection would be worse than no field, because it is what a per-brick
		 * readout hangs off.
		 */
		Row.Ref = Row.Refs.Last();
	}

	return Rows;
}

FPieceMenuInspector BuildPieceMenuInspector(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Selected,
	const FPieceRef& InspectedRef,
	EPieceMenuDetail Detail)
{
	FPieceMenuInspector Inspector;

	/*
	 * THE HEADING IS SET BEFORE ANY OF THE FAIL-CLOSED ROUTES BELOW CAN TAKE, because it is the
	 * one line that may not go quiet. A fixed panel is on screen while the selection is empty,
	 * while a ref names another wall and while nothing has been solved, and a bare count over a
	 * blank box reads as a readout that broke rather than as one with nothing to say.
	 */
	Inspector.HeaderText = FString(PresenterPanelHeader);

	/*
	 * THE COUNT IS THE SELECTION'S OWN AND IT NEVER SHRINKS TO WHAT RESOLVES. A ref
	 * naming another wall, one whose piece a cascade took and one missing a half all
	 * still count: the player picked that many bricks and the highlights on screen are
	 * drawn off the same set, so a count that quietly disagreed with them would be the
	 * presenter contradicting itself.
	 */
	Inspector.SelectedCount = Selected.Num();

	/*
	 * SINGULAR AND PLURAL ARE DECIDED HERE, because "1 brick" against "1 bricks" is a
	 * branch and a branch in the widget is untested by construction. Nothing picked is
	 * its own sentence rather than "0 bricks selected", which reads as a fault.
	 */
	if (Inspector.SelectedCount == 0)
	{
		Inspector.CountText = FString(TEXT("No bricks selected"));
	}
	else if (Inspector.SelectedCount == 1)
	{
		Inspector.CountText = FString(TEXT("1 brick selected"));
	}
	else
	{
		Inspector.CountText =
			FString::Printf(TEXT("%d bricks selected"), Inspector.SelectedCount);
	}

	/*
	 * ONE ENTRY PER SELECTED BRICK, IN PICK ORDER, NEVER REORDERED AND NEVER
	 * DEDUPLICATED. This is a PROJECTION of a list rather than a second implementation
	 * of set semantics: FPieceSelection is what guarantees the set, and a presenter that
	 * sorted or de-duped would look perfectly tidy while ceasing to agree with the order
	 * the batched commit runs in.
	 */
	Inspector.Pieces.Reserve(Selected.Num());

	/*
	 * AND EVERY LABEL NAMES WHERE THE BRICK IS — "course 2 · #1" — WHEN THIS BINDING CAN
	 * PLACE IT.
	 *
	 * The table is built ONCE for the whole binding rather than per entry, because a
	 * position is a fact about a piece's neighbours: naming one brick needs every brick.
	 */
	const TArray<FString> PositionLabels = PresenterPositionLabels(Binding);

	/*
	 * AND WHERE IT CANNOT, THE LABEL NAMES BOTH HALVES OF THE REF — "brick 9:1". A piece
	 * index on its own is not an identity: two walls both have a brick 1, and a selection
	 * can hold refs from more than one of them, so an unqualified label presents two
	 * different bricks as one string — one of which singles out nothing when it is clicked.
	 *
	 * THE FALLBACK IS THE OLD RULE KEPT WORD FOR WORD, AND THAT IS THE POINT RATHER THAN A
	 * LEFTOVER. The rule it replaced was justified entirely by TOTALITY — two selected
	 * bricks must never present as the same string — so the replacement has to keep that
	 * promise for the refs that have no position here at all: one naming another wall, one
	 * missing a half, and one whose box is a NaN. "brick 4:-1" prints an absent half as what
	 * it is, which is not a piece index any wall can have, and no positioned brick can
	 * collide with either shape. It is a debugger; unambiguous beats friendly.
	 *
	 * THE STRUCTURE IS CHECKED BEFORE THE HANDLE, for the reason FStructureBinding::Resolve-
	 * Piece checks it first: an unidentified binding must match nothing, or a ref that never
	 * learned who it belonged to would find it and be handed somebody's position. This
	 * deliberately does NOT go through ResolvePiece, which fails closed on a REMOVED piece —
	 * a hole in a wall is still somewhere, and its box is still there to say where.
	 */
	const bool bLabelsAreThisStructure = Binding.StructureId != INDEX_NONE;

	for (const FPieceRef& Ref : Selected)
	{
		FInspectorPieceEntry& Entry = Inspector.Pieces.AddDefaulted_GetRef();
		Entry.Ref = Ref;

		const bool bIsPlaced = bLabelsAreThisStructure
			&& Ref.StructureId == Binding.StructureId
			&& PositionLabels.IsValidIndex(Ref.PieceIndex)
			&& !PositionLabels[Ref.PieceIndex].IsEmpty();

		Entry.Label = bIsPlaced
			? PositionLabels[Ref.PieceIndex]
			: FString::Printf(TEXT("brick %d:%d"), Ref.StructureId, Ref.PieceIndex);

		/*
		 * AND WHETHER IT STILL NAMES A BRICK IS InspectPiece'S QUESTION, ASKED OF EVERY
		 * ENTRY RATHER THAN ONLY OF THE SINGLED-OUT ONE. It is deliberately not a
		 * ResolvePiece call spelled out here: that would be a second, quieter copy of the
		 * rule the breakout below already asks, and the two would eventually disagree about
		 * which bricks exist. A RELEASED brick reads LIVE — it is still a piece in the graph
		 * with a support state and a joint list, and what the MENU may do about it is
		 * PieceActionsFor's intersection, already said by the rows going empty.
		 */
		const FPieceInspection EntryInspection = InspectPiece(Binding, Ref);

		Entry.bIsLivePiece = EntryInspection.bIsPiece;

		/*
		 * AND WHY THAT BRICK IS OR IS NOT STANDING UP, ON EVERY ROW RATHER THAN ONLY ON THE
		 * ONE SINGLED OUT — OFF THE SAME INSPECTION, WHICH IS THE POINT OF TAKING IT AS A
		 * LOCAL.
		 *
		 * Eleven picked bricks are eleven identical rows without it, and finding the falling
		 * one means hovering each in turn while the model has the answer for all of them
		 * already. A second InspectPiece call for the word would be a second scan of the
		 * whole connection array per row for a question this one has answered.
		 *
		 * AND THE DOT BESIDE THE WORD IS THE SAME ANSWER RATHER THAN A SECOND ONE. The bucket is
		 * asked once and the word is read off the bucket, so a row cannot say "grounded" in text
		 * and draw the colour of a brick that is falling.
		 */
		Entry.SupportBand = PresenterSupportBand(EntryInspection);
		Entry.SupportText = PresenterWordForSupportBand(Entry.SupportBand);
	}

	/*
	 * THE INSPECTED BRICK MUST BE A MEMBER OF THE SELECTION, AND THE MEMBERSHIP IS FOUND
	 * AS AN INDEX RATHER THAN AS A BOOL. Marking by index marks exactly one entry however
	 * many times the same brick was picked, so "at most one entry reads as inspected"
	 * holds by construction instead of by a guard somebody has to remember. An anchor
	 * outside the set it anchors is a readout of somebody else's brick — Core/PieceMenu.h
	 * says why on FPieceMenuRow::Ref, and it does not weaken here.
	 */
	const int32 InspectedEntry = Selected.IndexOfByKey(InspectedRef);

	/*
	 * AND IT MUST BE A LIVE PIECE, WHICH IS InspectPiece'S QUESTION AND NOT A SECOND ONE.
	 * A ref naming another structure, a ref missing a half and a piece a removal took all
	 * come back bIsPiece false, so the default-constructed inspection below IS the
	 * fail-closed answer: nothing singled out, nothing broken out, and no support word.
	 */
	FPieceInspection Inspection;

	if (InspectedEntry != INDEX_NONE)
	{
		Inspection = InspectPiece(Binding, InspectedRef);
	}

	Inspector.bHasInspectedPiece = Inspection.bIsPiece;

	/*
	 * AND THE HINT BELONGS TO EXACTLY ONE OF THE THREE STATES A PANEL CAN BE IN, WHICH IS WHY
	 * IT IS DECIDED HERE RATHER THAN BY A WIDGET NOTICING AN EMPTY BREAKOUT.
	 *
	 * Bricks picked and none pointed at is the state that needs it. A brick pointed at must NOT
	 * carry it — a hint standing beside a live breakout is the stale-field defect this whole
	 * struct is shaped against — and neither must an empty selection, where a player would be
	 * sent to hunt a list with nothing in it and CountText already speaks for the state.
	 */
	if (Inspector.SelectedCount > 0 && !Inspector.bHasInspectedPiece)
	{
		Inspector.InspectedHintText = FString(PresenterInspectedHint);
	}

	if (!Inspector.bHasInspectedPiece)
	{
		return Inspector;
	}

	Inspector.Pieces[InspectedEntry].bIsInspected = true;
	Inspector.InspectedRef = InspectedRef;

	/*
	 * AND THE READOUT NAMES ITS OWN SUBJECT, IN THE ENTRY LIST'S OWN WORDS RATHER THAN BY A
	 * SECOND DERIVATION OF THE SAME QUESTION. Two inches apart on one panel, one brick may not
	 * be "course 2 · #1" in the list and something else over its joints — and the list scrolls,
	 * so the entry a breakout belongs to can be out of sight entirely while the breakout stays.
	 * Copied off the marked entry, so the two halves of the panel cannot drift apart.
	 */
	Inspector.InspectedLabel = Inspector.Pieces[InspectedEntry].Label;

	/*
	 * AND THE READOUT'S OWN DOT AND WORD, OFF ONE BUCKET FOR THE REASON EVERY ENTRY ROW'S ARE.
	 * This is reached only past the bHasInspectedPiece return above, so a panel with nothing
	 * singled out keeps the field's default — NotAPiece, the one value claiming nothing — beside
	 * the empty word, rather than a colour left over from the brick the cursor has left.
	 */
	Inspector.SupportBand = PresenterSupportBand(Inspection);
	Inspector.SupportText = PresenterWordForSupportBand(Inspector.SupportBand);

	/*
	 * THE JOINT LIST IS SUMMED UP BEFORE IT IS BROKEN OUT, AND IT IS A FACT ABOUT THE GRAPH
	 * RATHER THAN ABOUT THE SOLVE. A brick nobody has solved for still has exactly the joints
	 * it was built with, so this sentence must not go quiet with the support word — and a
	 * brick with none still gets one, because "no joints" is the truth about an isolated pad
	 * and a widget left to notice an empty array would be holding the branch.
	 */
	Inspector.JointsText = PresenterWordForJointCount(Inspection.Joints.Num());

	/*
	 * AND A COMPACT READOUT STOPS HERE, WHICH IS EVERYTHING BELOW AND NOTHING ABOVE IT.
	 *
	 * WHAT IS LEFT UNDER THIS LINE IS EXACTLY THE PER-JOINT TABLE AND THE SCALE THAT GIVES ITS BARS
	 * A MEANING, so the cut is a placement rather than a filter — there is no second list to keep in
	 * step with the first, and no field that could be trimmed here and left standing there.
	 *
	 * IT IS AFTER JointsText DELIBERATELY. "3 joints" is a sentence about the BRICK rather than a
	 * row of the table, it is the one line that says the table exists to be opened, and it is
	 * counted off InspectPiece's list rather than off Inspector.Joints — so a compact readout says
	 * the brick has three neighbours while showing none of them. Counting the trimmed list instead
	 * would report a brick with three neighbours as having none, which reads as a fact rather than
	 * as an absence.
	 *
	 * AND RETURNING BEFORE THE LOOP IS WHAT MAKES IT A SAVING. A mode that built every row and then
	 * hid it would pay the whole price of the table for a panel that never draws it, which is the
	 * argument for the mode being on the model at all rather than a collapsed slot in the widget.
	 *
	 * THE CAPTION AND THE TICKS NEED NO GUARD OF THEIR OWN: both are set only inside the "there is a
	 * bar to label" block below, so a table that was never built cannot be labelled.
	 */
	if (Detail == EPieceMenuDetail::Compact)
	{
		return Inspector;
	}

	/*
	 * THE BREAKOUT IS InspectPiece'S ANSWER CONVERTED AND WORDED, ROW FOR ROW, IN ITS
	 * ORDER. Nothing here consults FConnection, a normal or a strength profile: the tier,
	 * the force, the ratio and the two break fields are read straight off the model,
	 * because a second derivation agrees to nine decimal places forever and still differs
	 * in the last bit. That includes the ADJACENCY — a joint that has given is dropped
	 * from the solver's support lists before the tier is even decided, and it is exactly
	 * the row a player who just pulled a brick is looking for.
	 */
	Inspector.Joints.Reserve(Inspection.Joints.Num());

	for (int32 JointIndex = 0; JointIndex < Inspection.Joints.Num(); ++JointIndex)
	{
		const FJointInspection& Joint = Inspection.Joints[JointIndex];

		FInspectorJointRow& Row = Inspector.Joints.AddDefaulted_GetRef();

		/*
		 * THE SWATCH IS THE ROW'S OWN NUMBER, WHICH IS WHY IT IS TAKEN FROM THE LOOP RATHER
		 * THAN FROM ANYTHING ABOUT THE JOINT. A colour keyed on the connection or on the brick
		 * at the far end runs out at the first wall; keyed on the row it never can.
		 */
		Row.ColourSlot = PresenterColourSlotFor(JointIndex);

		Row.ConnectionIndex = Joint.ConnectionIndex;
		Row.OtherPieceIndex = Joint.OtherPieceIndex;
		Row.Role = Joint.Role;
		Row.bHasGiven = Joint.bHasGiven;
		Row.BreakPass = Joint.BreakPass;

		/*
		 * THE ONE UNIT CHANGE, AND THE ONLY PLACE ENTITLED TO IT. 1 N = 100 uu, named
		 * once as DestructionPresenter::ForceUnitsPerNewton — NOT ForceUnitsPerMPaSqCm,
		 * which is 10,000, this factor times cm2-to-mm2, and reaching for it would be a
		 * clean 100x that a tuned-looking readout hides perfectly. Divided rather than
		 * multiplied by 0.01, because 0.01 is not representable and the quotient is what
		 * a decimal reading of the model's own number gives.
		 */
		Row.ForceN = Joint.ForceUu.Size() / DestructionPresenter::ForceUnitsPerNewton;

		/*
		 * AND THE BEND THROUGH THE SAME CONSTANT, WHICH IS NOT A SECOND BOUNDARY. A moment is
		 * uu.cm and length is already centimetres, so turning it into newton-centimetres is the
		 * identical unit change the line above makes — "moments" merely sounds like it should
		 * introduce one. The MAGNITUDE, for the reason ForceN takes one: which way a joint is
		 * being levered open is not a thing a line of text says, and the worst corner is the
		 * worst corner either way.
		 */
		Row.MomentNCm = Joint.MomentUuCm.Size() / DestructionPresenter::ForceUnitsPerNewton;

		Row.UtilisationPercent = Joint.Utilisation * 100.0;

		/*
		 * AND THE MARGIN AND THE BAR ARE TRANSFORMS OF THE TWO NUMBERS ABOVE, NOT A THIRD
		 * TRIP TO THE GRAPH. The reciprocal of a utilisation already on the row cannot
		 * disagree with the per cent beside it, which is what keeps the exact-equality
		 * sweep the readout test runs against InspectPiece holding unchanged.
		 */
		Row.MarginText = PresenterMarginText(Row.UtilisationPercent, Row.bHasGiven);
		Row.HeadroomFraction = PresenterHeadroomFraction(Row.UtilisationPercent, Row.bHasGiven);
		Row.MarginBand = PresenterMarginBand(Row.UtilisationPercent, Row.bHasGiven);

		/*
		 * AND THE FAR END IS NAMED WHERE IT IS, OUT OF THE TABLE THE ENTRY LIST WAS NAMED
		 * FROM RATHER THAN OFF A SECOND DERIVATION OF THE SAME QUESTION.
		 *
		 * The row exists so a player can find the brick on the OTHER side of the joint,
		 * which is the one thing an index into FStructure's piece array cannot help anybody
		 * do — and printing one two inches under "course 2 · #1" names bricks in a single
		 * wall by two incompatible schemes. Working the course out again here would agree
		 * with the entry list for as long as nobody touched either, which is the drift this
		 * project has already paid for; PositionLabels is a lookup, so the two halves of the
		 * panel cannot disagree.
		 *
		 * A REMOVED FAR END STILL NAMES A PLACE. FPieceBinding keeps the box of a piece that
		 * has gone on purpose, so a severed joint says where the hole is instead of falling
		 * back — which is the single most useful thing to say about a hole.
		 *
		 * AND THE FALLBACK IS THE ENTRY LABEL'S, WORD FOR WORD, down to the structure id: a
		 * player reading "brick 21:13" in a joint row and in the list above has to be able to
		 * tell it is one brick. That id is the BINDING'S because a far end is a bare handle in
		 * the inspected brick's own structure — a joint row can never name another wall, and
		 * can never be missing a half, so a box that says nowhere is the only route here at
		 * all. The binding is identified by construction: an unidentified one resolves no ref,
		 * so nothing would have been inspected and this loop would not be running.
		 */
		const bool bFarEndIsPlaced = PositionLabels.IsValidIndex(Row.OtherPieceIndex)
			&& !PositionLabels[Row.OtherPieceIndex].IsEmpty();

		const FString OtherPieceText = bFarEndIsPlaced
			? PositionLabels[Row.OtherPieceIndex]
			: FString::Printf(TEXT("brick %d:%d"), Binding.StructureId, Row.OtherPieceIndex);

		/*
		 * AND A JOINT THAT HAS GIVEN IS A DIFFERENT SENTENCE, NOT A DIFFERENT NUMBER. It
		 * carries nothing, so it reads 0 N at 0 % — identical to an intact joint with
		 * nothing on it, and one of those is a hole in the wall. Deciding that here is
		 * what keeps the widget free of the branch.
		 *
		 * AND THE BENDING CLAUSE TRAILS ITS NUMBER LIKE EVERY OTHER CLAUSE ON THE LINE —
		 * "78.4 N", "49.000 %", "2.0× margin" are all <number> <word> — and it is EMPTY on
		 * the joints that are not being levered open, which is almost all of them. Without it
		 * a bent joint prints a force and a percentage with no arithmetic between them: 78.4 N
		 * at 49 % of capacity beside 490.0 N at 1 %, and the term that reconciles them named
		 * nowhere on the line.
		 */
		Row.Text = Row.bHasGiven
			? FString::Printf(
				TEXT("#%d  %s  %s  broken (went with a removed piece)"),
				Row.ConnectionIndex, *OtherPieceText,
				PresenterWordForJointRole(Row.Role))
			: FString::Printf(
				TEXT("#%d  %s  %s  %s  %.3f %%  %s%s"),
				Row.ConnectionIndex, *OtherPieceText,
				PresenterWordForJointRole(Row.Role),
				*PresenterForceText(Row.ForceN), Row.UtilisationPercent, *Row.MarginText,
				*PresenterBendingText(Row.MomentNCm));
	}

	/*
	 * AND THE BAR IS LABELLED EXACTLY WHEN THERE IS A BAR TO LABEL. A brick with no joints
	 * draws none, so a caption beside it is a label on nothing — the same stale-field defect
	 * as a joint breakout left over from the brick the player just deselected, wearing the
	 * tidiest possible face.
	 *
	 * THE CAPTION QUOTES THE TOP OF THE SCALE RATHER THAN SPELLING IT AGAIN, so a bar
	 * redrawn over a different number of decades cannot go on promising three.
	 */
	if (Inspector.Joints.Num() > 0)
	{
		Inspector.HeadroomScale = PresenterHeadroomScale();

		Inspector.HeadroomCaption = FString::Printf(
			TEXT("headroom — full is %s margin, empty is the joint giving"),
			*Inspector.HeadroomScale.Last().Label);
	}

	return Inspector;
}

FVector2D PieceMenuPanelSizePx(EPieceMenuDetail Detail)
{
	/*
	 * COMPACT IS THE ONLY ARM THAT WITHHOLDS ANYTHING, AND EVERY OTHER ANSWER IS Full — INCLUDING
	 * AN ENUMERATOR NOBODY DECLARED.
	 *
	 * EPieceMenuDetail is a uint8 and a cast is all it takes to produce one, so which arm an
	 * unknown value falls into is a decision rather than an impossibility. Answering it with the
	 * compact size would SUPPRESS numbers somebody asked for, which is the failure the enum's own
	 * comment names as the reason Full is enumerator zero; answering it with the full size only
	 * spends screen. So Full is the fall-through rather than a case, and the switch is left
	 * exhaustive so that a mode added later is a compiler warning here instead of a silent Full.
	 */
	switch (Detail)
	{
	case EPieceMenuDetail::Compact:
		return FVector2D(PresenterCompactPanelWidthPx, PresenterCompactPanelHeightPx);

	case EPieceMenuDetail::Full:
		break;
	}

	return FVector2D(PresenterFullPanelWidthPx, PresenterFullPanelHeightPx);
}

FVector2D PieceMenuHomeOffset(
	FVector2D PanelSizePx,
	FVector2D ViewportSizePx,
	double MarginPx)
{
	/*
	 * ANYTHING THAT IS NOT A NUMBER FAILS THE WHOLE VECTOR TO THE ORIGIN, RATHER THAN ONLY THE AXIS
	 * IT ARRIVED ON — the same rule ClampPanelOffset states, and stated the same way because it is
	 * the same fault: a screen whose edges are not known is one where half an answer looks
	 * deliberate. FMath::Max DISCARDS a NaN and FMath::Min REPLACES it, so an unguarded version of
	 * the subtraction below hands back a perfectly plausible corner.
	 */
	if (!PresenterPanelPixelsAreFinite(PanelSizePx)
		|| !PresenterPanelPixelsAreFinite(ViewportSizePx)
		|| !FMath::IsFinite(MarginPx))
	{
		return FVector2D::ZeroVector;
	}

	/*
	 * AND A NEGATIVE SIZE IS NOT A SMALL PANEL, NOR A NEGATIVE MARGIN A SMALL MARGIN. Both WIDEN
	 * the room the subtraction thinks it has — the fail-OPEN direction, reached by arithmetic that
	 * reads perfectly reasonably on the line it is written.
	 */
	if (!PresenterPanelSizeIsUsable(PanelSizePx)
		|| !PresenterPanelSizeIsUsable(ViewportSizePx)
		|| !PresenterPanelMarginIsUsable(MarginPx))
	{
		return FVector2D::ZeroVector;
	}

	/*
	 * THE HOME ITSELF: the panel's right edge stands MarginPx in from the viewport's, and its
	 * height is centred down the screen. The margin is horizontal only, because "centred" already
	 * answers the vertical question and a margin applied to a centred axis does nothing.
	 *
	 * PINNED AT ZERO ON EACH AXIS INDEPENDENTLY, WHICH IS WHY THIS IS NOT MERELY A SUBTRACTION. A
	 * viewport narrower than the panel, or a margin wider than the room left over, makes the
	 * subtraction NEGATIVE — the panel's own heading off the top-left of the screen, which is the
	 * one corner that has to stay grabbable. Per axis rather than per vector because a panel too
	 * wide for its screen is still centred down it.
	 *
	 * IT NEEDS NO UPPER PIN AND SO COMPOSES WITH ClampPanelOffset RATHER THAN RESTATING IT: the
	 * margin is non-negative, so the X answer is never past Viewport - Panel, and half of a
	 * non-negative gap is never past the whole of it. Clamping this again changes nothing, which is
	 * what stops the panel jumping the first time it is picked up.
	 */
	return FVector2D(
		FMath::Max(ViewportSizePx.X - PanelSizePx.X - MarginPx, 0.0),
		FMath::Max((ViewportSizePx.Y - PanelSizePx.Y) * 0.5, 0.0));
}

FVector2D ClampPanelOffset(
	FVector2D DesiredOffsetPx,
	FVector2D PanelSizePx,
	FVector2D ViewportSizePx)
{
	/*
	 * ANYTHING THAT IS NOT A NUMBER FAILS THE WHOLE VECTOR TO THE ORIGIN, RATHER THAN ONLY THE AXIS
	 * IT ARRIVED ON.
	 *
	 * A viewport whose height is not a number is a screen whose edges are not known, and half an
	 * answer against a screen of unknown size is worse than the default position — it looks
	 * deliberate. Zero is a corner that is always on screen and is always grabbable, which is the
	 * only property the fallback has to have.
	 */
	if (!PresenterPanelPixelsAreFinite(DesiredOffsetPx)
		|| !PresenterPanelPixelsAreFinite(PanelSizePx)
		|| !PresenterPanelPixelsAreFinite(ViewportSizePx))
	{
		return FVector2D::ZeroVector;
	}

	if (!PresenterPanelSizeIsUsable(PanelSizePx) || !PresenterPanelSizeIsUsable(ViewportSizePx))
	{
		return FVector2D::ZeroVector;
	}

	/*
	 * AND THE TWO AXES ARE PINNED INDEPENDENTLY. A drag past the right edge must not also reset the
	 * vertical position, or a player dragging along one edge would see the panel jump on the other.
	 */
	return FVector2D(
		PresenterPanelAxisPinned(DesiredOffsetPx.X, ViewportSizePx.X - PanelSizePx.X),
		PresenterPanelAxisPinned(DesiredOffsetPx.Y, ViewportSizePx.Y - PanelSizePx.Y));
}
