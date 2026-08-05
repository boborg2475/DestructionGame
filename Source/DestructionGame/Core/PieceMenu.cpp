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
	 * Why a brick is or is not being held up, in words.
	 *
	 * "NOBODY HAS SOLVED YET" IS ASKED FIRST AND IS ITS OWN SENTENCE, and that ordering
	 * is the whole point of the function. EPieceSupport::Falling is both a real collapse
	 * and an absent answer — deliberately, because enumerator zero has to promise least
	 * — so a readout that went straight to the enumerator would draw a freshly built,
	 * never-solved wall as a column of falling bricks: a catastrophe reported that has
	 * not happened.
	 */
	FString PresenterWordForSupport(const FPieceInspection& Inspection)
	{
		if (!Inspection.bHasSupportAnswer)
		{
			return FString(TEXT("not solved yet"));
		}

		switch (Inspection.Support)
		{
		case EPieceSupport::Grounded:  return FString(TEXT("grounded"));
		case EPieceSupport::Supported: return FString(TEXT("supported"));
		case EPieceSupport::Stranded:  return FString(TEXT("stranded"));
		case EPieceSupport::Falling:   return FString(TEXT("falling"));
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
	 * The bar's decade ticks, low to high.
	 *
	 * A LOG AXIS WITH NO TICKS IS UNREADABLE BY CONSTRUCTION — the same visible fill means
	 * 1000× on one panel and 3× on another, and nothing on screen says which. So they are
	 * not decoration, and they are placed by the SAME curve the fill is, rather than by four
	 * hand-written fractions: a caption promising a scale the arithmetic does not follow is
	 * a plausible-looking picture over a wrong number.
	 */
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
	const FPieceRef& InspectedRef)
{
	FPieceMenuInspector Inspector;

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
		Entry.bIsLivePiece = InspectPiece(Binding, Ref).bIsPiece;
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

	if (!Inspector.bHasInspectedPiece)
	{
		return Inspector;
	}

	Inspector.Pieces[InspectedEntry].bIsInspected = true;
	Inspector.InspectedRef = InspectedRef;
	Inspector.SupportText = PresenterWordForSupport(Inspection);

	/*
	 * THE JOINT LIST IS SUMMED UP BEFORE IT IS BROKEN OUT, AND IT IS A FACT ABOUT THE GRAPH
	 * RATHER THAN ABOUT THE SOLVE. A brick nobody has solved for still has exactly the joints
	 * it was built with, so this sentence must not go quiet with the support word — and a
	 * brick with none still gets one, because "no joints" is the truth about an isolated pad
	 * and a widget left to notice an empty array would be holding the branch.
	 */
	Inspector.JointsText = PresenterWordForJointCount(Inspection.Joints.Num());

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

	for (const FJointInspection& Joint : Inspection.Joints)
	{
		FInspectorJointRow& Row = Inspector.Joints.AddDefaulted_GetRef();

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
		Row.UtilisationPercent = Joint.Utilisation * 100.0;

		/*
		 * AND THE MARGIN AND THE BAR ARE TRANSFORMS OF THE TWO NUMBERS ABOVE, NOT A THIRD
		 * TRIP TO THE GRAPH. The reciprocal of a utilisation already on the row cannot
		 * disagree with the per cent beside it, which is what keeps the exact-equality
		 * sweep the readout test runs against InspectPiece holding unchanged.
		 */
		Row.MarginText = PresenterMarginText(Row.UtilisationPercent, Row.bHasGiven);
		Row.HeadroomFraction = PresenterHeadroomFraction(Row.UtilisationPercent, Row.bHasGiven);

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
		 */
		Row.Text = Row.bHasGiven
			? FString::Printf(
				TEXT("#%d  %s  %s  broken (went with a removed piece)"),
				Row.ConnectionIndex, *OtherPieceText,
				PresenterWordForJointRole(Row.Role))
			: FString::Printf(
				TEXT("#%d  %s  %s  %s  %.3f %%  %s"),
				Row.ConnectionIndex, *OtherPieceText,
				PresenterWordForJointRole(Row.Role),
				*PresenterForceText(Row.ForceN), Row.UtilisationPercent, *Row.MarginText);
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
