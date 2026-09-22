// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceMenu.h"

#include "Core/Connection.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

/*
 * Presenter prefix on every name: an anonymous namespace is private to a translation unit, and
 * a unity build merges files, so two file-local names can collide (see Structure.cpp).
 */
namespace
{
	/**
	 * A joint's tier, in player-facing words. None gets its own word, not a shared one: no row
	 * can carry None today, but reading it as one of the three real tiers is the fail-open
	 * direction GetJointRole guards against.
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
	 * What fastens a joint, named by which library row it is — and "custom" for one that is none.
	 *
	 * The role is not enough, which is the whole reason this exists: "bed below" is the same
	 * sentence for a screwed plate and a dry-bedded one, and those two structures behave
	 * nothing alike the moment either is run — the choice changes committed physics and
	 * nothing on screen moves when it does, so this is the only place a player can find out
	 * what they built.
	 *
	 * The lookup is by value because the identity is already gone: an FConnection stores a
	 * copy of the profile it was made with, so there is no address left to compare by the
	 * time a joint is in a wall. FindConnectionProfileRow matches the five fields back to the
	 * shipped row — a lookup into data rather than a branch per profile, which is what keeps
	 * connection types data. Lower case because it sits mid-sentence: the library spells its
	 * rows GeneralPurposeMortar, and a capitalised name in the middle of "#2  course 1 · #3
	 * head ..." reads as the start of a new clause.
	 *
	 * A strength this library never shipped reads "custom" rather than going quiet or
	 * guessing. Nothing in the game builds one today, but a hand-made profile in a fixture or
	 * a future authored joint would land here, and naming the nearest row instead would be a
	 * plausible lie — this library is siblings by construction.
	 */
	FString PresenterWordForJointProfile(const FStructure& Structure, int32 ConnectionIndex)
	{
		if (ConnectionIndex >= 0 && ConnectionIndex < Structure.NumConnections())
		{
			const DestructionProfiles::FNamedConnectionProfile* const Row =
				DestructionProfiles::FindConnectionProfileRow(
					Structure.GetConnection(ConnectionIndex).Strength);

			if (Row != nullptr && Row->Name != nullptr)
			{
				return FString(Row->Name).ToLower();
			}
		}

		return FString(TEXT("custom"));
	}

	/**
	 * What the panel calls itself, in every state there is.
	 *
	 * A constant string is still a string the widget may not spell, for the reason every
	 * other word on this struct lives here: choosing it is a decision, and Slate is where
	 * nothing can read it. The branch count is zero today; the day it gains one, the
	 * decision is already on the testable side of the seam.
	 */
	const TCHAR* const PresenterPanelHeader = TEXT("Selection");

	/**
	 * What the readout region says when there is no brick to break out.
	 *
	 * A fixed panel reserves the readout's space whether or not it has one, so the
	 * alternative is a hole in the panel that reads as a readout that failed rather than
	 * one waiting to be asked — the same rule "No bricks selected" and "No joints" follow.
	 */
	const TCHAR* const PresenterInspectedHint = TEXT("Hover a brick in the list to see its joints");

	/**
	 * Why a brick is or is not being held up, as the BUCKET everything else about it is read off.
	 *
	 * "This ref names no brick" is asked first and "nobody has solved yet" second, and both
	 * orderings are the whole point of the function. A brick a removal took, a ref naming
	 * another wall and a ref missing a half all come back bIsPiece false — and the bucket
	 * that falls out of the guard below for one of those is NotSolved, which promises the
	 * brick is there and indistinguishable from every live row on a freshly built wall. So a
	 * ref naming nothing says exactly that, before anything is read off the enumerators.
	 *
	 * EPieceSupport::Falling is then both a real collapse and an absent answer, deliberately,
	 * because enumerator zero has to promise least — so a readout going straight to the
	 * enumerator would draw a freshly built, never-solved wall as a column of falling bricks.
	 *
	 * One function for the entry row and the readout under it, which is why the first guard
	 * lives here rather than at the one call site that can reach it: two inches apart on one
	 * panel, one brick may not read "supported" in the list and something else over its joints.
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
	 * That bucket in the words a player reads — and the bucket is the only thing it asks.
	 *
	 * The word is derived from the bucket rather than beside it, which is what makes the two
	 * unable to disagree: a model that decided each from the inspection separately would be
	 * two derivations of one question sitting a few pixels apart — a row reading "grounded"
	 * next to the colour this game uses for a brick that is coming down (Core/PieceMenu.h
	 * says at length why the bucket exists at all).
	 *
	 * One sentence per bucket, including the two that are not physical states: two buckets
	 * sharing a sentence would make them indistinguishable to a reader while still being two
	 * colours, the worst of both.
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
	 * How many joints a brick has, in words — including when it has none.
	 *
	 * An empty list gets a sentence, exactly as CountText's "No bricks selected" does: an
	 * isolated grounded pad is a real brick with nothing joined to it, a fact about the
	 * brick rather than an absence of data. Singular and plural are decided here too —
	 * "1 joints" is the same branch wearing a smaller coat.
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
	 * Singular and plural are decided here for the reason CountText's are: "1 bricks" is a
	 * branch, and a branch in Slate is untested by construction. There is no wording for
	 * nothing at all — a row that exists always carries at least one ref, since a menu
	 * with no target is a button with nothing behind it.
	 */
	FString PresenterWordForTargetCount(int32 BrickCount)
	{
		return BrickCount == 1
			? FString(TEXT("1 brick"))
			: FString::Printf(TEXT("%d bricks"), BrickCount);
	}

	/**
	 * What a piece is made of, named by which library row it is and never by its numbers.
	 *
	 * Identity, not equality: "what shipped profile is this piece built from" is a question
	 * about which row, and a profile field-for-field equal to ClayBrick is still a different
	 * row the moment one of the two is retuned. The address is what BuildMode::PlacePiece
	 * stores — BuildPieceMaterial hands out a reference to the shipped constant precisely so
	 * a retune reaches every brick — so the address is what this asks.
	 *
	 * The library is walked rather than the three externs compared, so a material stays
	 * data: a chain of `== &DestructionProfiles::ClayBrick` tests would be a branch per
	 * material in a presenter, and its failure mode is quiet — a fourth profile would read
	 * "Unknown material" with nothing to say why.
	 *
	 * A piece nobody said what it is made of is the common case rather than the exotic one:
	 * DestructionLayout::RunningBond lays every wall in this game off a bare density and sets
	 * no material at all, so the null arm is most of the pieces in the project. It reads as
	 * an undescribed material rather than a blank, since the size and mass beside it are
	 * still known.
	 */
	FString PresenterWordForMaterial(const DestructionProfiles::FMaterialProfile* Material)
	{
		if (Material != nullptr)
		{
			for (const DestructionProfiles::FNamedMaterialProfile& Row :
				DestructionProfiles::AllMaterialProfiles())
			{
				if (&Row.Profile == Material && Row.Name != nullptr)
				{
					return FString(Row.Name);
				}
			}
		}

		return FString(TEXT("Unknown material"));
	}

	/**
	 * A length in centimetres, to at most two decimals and with no trailing zeros.
	 *
	 * The precision is a decision and so is the trimming: a bare "%.2f" reads "67.50 ×
	 * 10.25 × 10.00" for a wall plate, three numbers written to a precision nothing in this
	 * game measures to, while "%g" is right for these dimensions and wrong the first time
	 * one needs three decimals, dropping to scientific notation instead. Two decimals is
	 * the coordinating grid's own resolution (the brick is 10.25 cm deep).
	 *
	 * The trim stops at the point, which is why it needs no guard against eating a number's
	 * own zeros: "1000.00" loses two zeros and then meets the '.', not a '0', so the loop
	 * ends and the point is chopped once, leaving "1000" rather than "1". The same property
	 * lets a non-finite length through untouched — "%.2f" of a NaN has no trailing zeros and
	 * no point, so it stays visibly a word instead of being chopped into a plausible number.
	 */
	FString PresenterCentimetreText(double LengthCm)
	{
		FString Text = FString::Printf(TEXT("%.2f"), LengthCm);

		while (Text.EndsWith(TEXT("0"), ESearchCase::CaseSensitive))
		{
			Text.LeftChopInline(1);
		}

		if (Text.EndsWith(TEXT("."), ESearchCase::CaseSensitive))
		{
			Text.LeftChopInline(1);
		}

		return Text;
	}

	/**
	 * What the inspected brick is: its material, its size and its mass, as one composed line.
	 *
	 * The size is the box's full dimensions, twice what the binding stores:
	 * FPieceBox::ExtentCm is a half size, matching FBox::GetExtent, so a readout printing it
	 * raw would present a standard brick as 10.75 × 5.125 × 3.25 — half the size of every
	 * brick in the game.
	 *
	 * The mass is the piece's own rather than a second derivation from that box:
	 * FStructurePiece::MassKg is the number the solver routes as load, and multiplying the
	 * dimensions by a density again would be a third copy of DestructionLayout::PieceMassKg,
	 * free to disagree with the physics it describes — the same discipline
	 * Core/PieceInspection.h states about utilisation, one layer further out.
	 *
	 * One decimal on the kilograms, a hundredth of the lightest piece this game lays and the
	 * precision anybody would weigh a brick to. No conversion boundary here: density is
	 * g/cm3 and length is cm, both Unreal's own units, and no force is computed here at all.
	 */
	FString PresenterIdentityLine(const FStructureBinding& Binding, int32 PieceIndex)
	{
		const FVector FullSizeCm = 2.0 * Binding.GetBinding(PieceIndex).Box.ExtentCm;
		const FStructurePiece& Piece = Binding.GetStructure().GetPiece(PieceIndex);

		return FString::Printf(
			TEXT("%s · %s × %s × %s cm · %.1f kg"),
			*PresenterWordForMaterial(Piece.Material),
			*PresenterCentimetreText(FullSizeCm.X),
			*PresenterCentimetreText(FullSizeCm.Y),
			*PresenterCentimetreText(FullSizeCm.Z),
			Piece.MassKg);
	}

	/**
	 * How far apart two centres may sit in Z and still be one course.
	 *
	 * Half a centimetre, against a course pitch of 7.5 cm — the 6.5 cm brick this game lays
	 * plus one mortar joint (DestructionLayout::RunningBond). A fifteenth of the smallest
	 * real gap between two courses: room for float noise and a brick a couple of millimetres
	 * off nominal, nowhere near enough to swallow a course.
	 *
	 * The two failure directions are not symmetric, which is why this sits far closer to the
	 * tight end than the middle: too tight and a wall reads as eighty courses of one brick
	 * each, obviously useless; too loose and two real courses merge, so two DIFFERENT bricks
	 * compete for one position number while the readout goes on looking ordinary.
	 *
	 * Known scale assumption, recorded rather than solved: it is an absolute distance, so a
	 * structure built of pieces under about a centimetre tall would band its courses
	 * together. Nothing in the game builds one, and the adaptive alternative — a fraction of
	 * each piece's own height — makes "same course as" non-transitive across mixed sizes.
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
	 * A NaN does not merely get the wrong course: every comparison against one is false, so
	 * it breaks the strict weak ordering the sort below is entitled to assume — undefined
	 * behaviour rather than a bad label. An infinity orders fine and then bands into a course
	 * of its own whose number depends on where the sort put it, unstable as well as
	 * meaningless. Both are excluded, so a piece nobody can place disturbs nobody else's.
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
	 * X alone is not enough, and the case that breaks it is ordinary masonry rather than a
	 * pathological input: a wall two leaves thick puts two bricks of one course at the same
	 * X, differing only in depth, so they would share a position number. Y settles those, and
	 * the handle settles two pieces at exactly the same point — a total order, so no two
	 * pieces of a course can share an ordinal.
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
	 * A position, because an array subscript is not a place: "brick 4:282" says which slot
	 * of which array a brick is in, which a person standing in front of a wall cannot check,
	 * while a course and a place along it is how the wall was built. Derivable here and only
	 * here — FStructure is position-free on purpose, and the binding is the layer holding
	 * the boxes.
	 *
	 * A handle this binding cannot place gets an empty string, and the caller falls back to
	 * the old ref-shaped label rather than inventing a position.
	 */
	TArray<FString> PresenterPositionLabels(const FStructureBinding& Binding)
	{
		TArray<FString> Labels;
		Labels.SetNum(Binding.NumPieces());

		TArray<FPresenterPlacedPiece> Placed;
		Placed.Reserve(Binding.NumPieces());

		/*
		 * A removed piece is still placed — FPieceBinding keeps its box for exactly this. A
		 * course that renumbered itself when a brick was pulled out would rename every brick
		 * to its right at the very moment a player is looking at them.
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
		 * Banded against the course's own floor, never against the piece before it.
		 * Comparing each piece to its predecessor is the obvious loop and wrong in a way
		 * nothing would notice: a run of pieces each a few millimetres above the last CHAINS,
		 * so forty of them merge into one "course" spanning sixteen centimetres. Anchoring to
		 * the floor bounds a course's total spread at the tolerance however many pieces arrive.
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
		 * Both numbers count from one, as a person counting courses of brick always does. A
		 * zero-based course is the array subscript this label exists to stop printing,
		 * wearing a different name.
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
	 * Not a conversion boundary, deliberately not named like one: turning Unreal force units
	 * into newtons is the boundary (DestructionPresenter::ForceUnitsPerNewton) and has
	 * already happened by the time anything here runs. A kilonewton is a thousand newtons by
	 * definition of the prefix, so this is a choice of how to print a number.
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
	 * nothing at all on a joint nothing is levering.
	 *
	 * The absence is the half that bites: a settled wall bends nowhere, since a brick on two
	 * symmetric bed patches has its centre of mass at the area-weighted centroid of its
	 * supports, so the eccentricity is zero exactly rather than nearly. Almost every joint a
	 * player looks at has nothing to say here, so a clause appended anyway would make the
	 * common case worse to read for the sake of the rare one.
	 *
	 * The test is against exact zero, a statement rather than a tolerance (see
	 * FStructure::GetConnectionMoment) — a centred load, an unplaced piece and an unmeasured
	 * rectangle all produce it exactly. The polarity keeps a fault visible: a moment that is
	 * not a number is not equal to zero, so it prints its clause rather than going quiet, the
	 * same direction the force and per cent beside it already take.
	 *
	 * N·cm, and no new conversion boundary: the number arrives already converted by
	 * DestructionPresenter::ForceUnitsPerNewton, and a moment is uu.cm with length already in
	 * centimetres, so the centimetre rides through untouched. Metres would collapse the
	 * interesting range — 313.6 N·cm is 3.136 N·m, where one printed digit is a whole 10 N·cm.
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
	 * Why margin at all, when the percentage is already on the row: "0.049 %" means
	 * something only to a reader who already knows 100 % is failure and that masonry in
	 * compression sits three or four orders of magnitude under it, while "2041× margin" says
	 * the whole thing in a phrase. It is the reciprocal of a number already on the row rather
	 * than a second derivation, so the exact-equality sweep the readout test holds ForceN and
	 * UtilisationPercent to keeps holding.
	 *
	 * The three readings that are not a number are the point, since each is a state where
	 * the plain reciprocal produces something plausible and wrong:
	 *
	 *   - A joint that has given carries nothing, so the arithmetic files it under "no
	 *     load" — an intact unloaded joint and a hole in the wall reading identically, the
	 *     defect FJointInspection::bHasGiven exists to prevent, reappearing one layer out.
	 *   - A joint at or past its limit divides into something no bigger than one, so a
	 *     joint at twice capacity would read "0.5× margin" — the word MARGIN beside a joint
	 *     that has none. At and past the limit are one sentence deliberately.
	 *   - An unloaded joint divides by zero. Infinite margin is true and useless.
	 *
	 * The order of the guards is the fail-closed one and is not free: every comparison
	 * against a NaN is false, so `!(Percent < Full)` is the branch a NaN falls into, reading
	 * as a joint with nothing left rather than a healthy one — cheap to be wrong in.
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
	 * A linear bar is empty forever, the whole reason the scale is logarithmic: a settled
	 * brick wall sits near 0.0005 of capacity, so a bar drawn on utilisation directly is a
	 * flat zero at every joint of every structure the game builds today. Full is 1000×
	 * margin, empty is the joint giving, and each decade is a third of the bar — so most
	 * joints peg it full, honestly, since they genuinely are that far from failing.
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
	 * Unloaded is full and given is empty — the bar's copy of the rule bHasGiven exists for:
	 * both read 0 N at 0 %, and one is a hole in the wall, the single worst thing this panel
	 * could draw as full.
	 *
	 * The guards run in the same order as PresenterMarginText's and for the same reason: a
	 * NaN utilisation falls into `!(Percent < Full)` and empties the bar. Asking "is it
	 * unloaded" first would fill it instead, since every comparison against a NaN is false.
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
	 * Where a bar stops being comfortable, and where it becomes critical — as utilisations,
	 * because that is the number already on the row.
	 *
	 * Ten per cent of capacity is 10x margin and fifty per cent is 2x, stated in per cent
	 * rather than in multiples so the guards below compare against UtilisationPercent
	 * directly — a reciprocal taken first would divide by zero for the unloaded joint that
	 * is the most comfortable one there is.
	 */
	constexpr double PresenterCautionAtPercent = 10.0;
	constexpr double PresenterCriticalAtPercent = 50.0;

	/**
	 * Which band a joint's bar is drawn in.
	 *
	 * A bucket rather than a colour: which side of 10x a joint sits on is a decision about
	 * what this game calls dangerous, and a widget comparing a fraction against two constants
	 * would hold that decision where nothing can read it. The hue stays the widget's.
	 *
	 * At an edge the joint takes the worse band, which is why the guards run worst first and
	 * are written negated — the same shape as PresenterMarginText's "no margin left" at
	 * exactly 1.0, since over-promising is the expensive direction on a panel whose job is to
	 * say what is about to fall down. Every comparison against a NaN is false, so a
	 * degenerate utilisation falls into the first guard and is drawn critical.
	 *
	 * A joint that has given is critical whatever its number says: it carries nothing, so the
	 * arithmetic alone would file a hole in the wall under the most comfortable state there
	 * is — the defect bHasGiven exists to prevent, one field further out.
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
	 * How many joint rows can carry a colour at all.
	 *
	 * Six, the wall's number rather than a round one: a brick inside a running bond is
	 * spanned by two above, rests on two below and has a head joint either side. A palette
	 * that ran out before then would leave the ordinary case half-coloured.
	 */
	constexpr int32 PresenterColourSlots = 6;

	/**
	 * Which colour slot a joint row takes, or nothing once the palette has run out.
	 *
	 * Per slot, not per brick: keyed on the far-end brick is what a reader assumes, but a
	 * wall is over a thousand bricks against a handful of legible hues, so it must collide —
	 * two rows in one colour is a lie about the single thing a swatch says. Keyed on the row
	 * it never collides, at the price that one brick is the first colour in one readout and
	 * the second in another.
	 *
	 * Past the end, nothing rather than a wrap: wrapping reintroduces the collision on the
	 * brick with the most joints, the one being read hardest. An absent swatch is an absence;
	 * a repeated one is a wrong answer.
	 */
	int32 PresenterColourSlotFor(int32 RowIndex)
	{
		return RowIndex < PresenterColourSlots ? RowIndex : INDEX_NONE;
	}

	/**
	 * The bar's decade ticks, low to high.
	 *
	 * A log axis with no ticks is unreadable by construction — the same visible fill means
	 * 1000× on one panel and 3× on another. They are placed by the same curve the fill is,
	 * rather than four hand-written fractions, so a caption cannot promise a scale the
	 * arithmetic does not follow.
	 */
	/**
	 * Whether both halves of a pixel measurement are numbers at all.
	 *
	 * Written so a NaN answers false rather than slipping through. FMath::IsFinite rejects
	 * both a NaN and an infinity — every comparison against a NaN is false, so FMath::Max
	 * discards one and FMath::Min replaces it, either way turning a fault into a plausible
	 * offset a hundred pixels in.
	 */
	bool PresenterPanelPixelsAreFinite(const FVector2D& ValuePx)
	{
		return FMath::IsFinite(ValuePx.X) && FMath::IsFinite(ValuePx.Y);
	}

	/**
	 * Whether a size is a size — and a negative one is not a small panel.
	 *
	 * The permitted range is Viewport - Panel, so a negative subtrahend WIDENS it: a
	 * viewport of -1080 px would let the corner be dragged further out than any real screen
	 * allows, the fail-open direction reached by arithmetic that reads reasonably on its own.
	 *
	 * Stated as `>= 0.0` rather than `!(< 0.0)`, the same rule the negated guards elsewhere
	 * in this file follow: this answers a question whose false is the fault, so the
	 * comparison has to be the one a NaN fails. Zero is usable — a viewport of no size and a
	 * panel exactly as big as its screen both leave a range of zero, pinning the corner to
	 * the origin rather than a degenerate one.
	 */
	bool PresenterPanelSizeIsUsable(const FVector2D& SizePx)
	{
		return SizePx.X >= 0.0 && SizePx.Y >= 0.0;
	}

	/**
	 * Whether a margin is a distance the panel can be stood off an edge by.
	 *
	 * A negative margin is not a smaller margin, the whole reason this is separate from "is
	 * it finite": it pushes the panel PAST the edge it was measured from — a 640 px panel on
	 * a 1920 px screen at -24 px opens at 1304, hanging 24 px off the right — and
	 * ClampPanelOffset would then quietly pull it back, so the fault would be invisible
	 * rather than absent.
	 *
	 * Zero is usable: the panel's right edge flush against the viewport's is a real
	 * placement. Stated as `>= 0.0` rather than `!(< 0.0)`, for the same reason
	 * PresenterPanelSizeIsUsable is.
	 */
	bool PresenterPanelMarginIsUsable(double MarginPx)
	{
		return MarginPx >= 0.0;
	}

	/**
	 * One axis of the panel's corner, pinned inside the room that axis has.
	 *
	 * The nesting is the whole function, and it is Max(Min(...)) because the other order
	 * hands back a negative offset: a panel wider than its screen makes LargestPx negative,
	 * and Min(Max(X, 0), Largest) would return Largest — the panel's own heading off the
	 * left edge of the screen — while this order returns 0. The two read identically at a
	 * glance, which is why the case is pinned by Presenter.PanelOffsetClamp.
	 *
	 * Spelled out rather than deferred to FMath::Clamp, which happens to be this order
	 * today — luck rather than a promise this function needs.
	 */
	double PresenterPanelAxisPinned(double DesiredPx, double LargestPx)
	{
		return FMath::Max(FMath::Min(DesiredPx, LargestPx), 0.0);
	}

	/*
	 * How much screen the full panel takes, and neither figure is picked.
	 *
	 * The width is a measured floor plus a stated clearance. The longest line this readout
	 * can compose is the bending clause on the ragged corbel wall (the only shape that bends
	 * at all): "#4  course 3 · #1  bed above generalpurposemortar  40.0 N  0.650 %  154×
	 * margin  150.0 N·cm bending", which overran the old 640 px panel by 101 px, measured by
	 * World.Menu.TheReadoutFitsInsideThePanel.
	 *
	 * The floor is sized for the longest *library row*, not the fixture's:
	 * `generalpurposemortarperpend` is seven characters (about 39 px) longer than the
	 * measured line's `generalpurposemortar` and is in every bonded wall in the game, so the
	 * floor takes those seven too — otherwise the panel would clip every wall but the one the
	 * test happens to lay. 640 + 101 + 39 = 780 is the floor; 800 px leaves a little real
	 * clearance on top.
	 *
	 * The height is a fit rather than a measurement: 560 px is about half a 1080 viewport, so
	 * the brick list, readout and action rows are all on screen at once — it did not move,
	 * since naming the profile makes lines longer, never more numerous.
	 *
	 * Pinned to the pixel by Presenter.PieceMenuPanelSize, as characterisation rather than a
	 * claim it can check: a Full arm that quietly drifted would take the measured floor with it.
	 */
	constexpr double PresenterFullPanelWidthPx = 800.0;
	constexpr double PresenterFullPanelHeightPx = 560.0;

	/*
	 * How much the compact panel takes, a floor of its own rather than a fraction of the full
	 * one — it was 0.7 of a 640 px panel, but the full width has since moved to fit a line
	 * COMPACT does not draw, so following it would spend screen on the complaint this mode
	 * exists to answer ("it takes up so much of the screen"). Compact drops the joint table
	 * and the headroom scale, leaving the brick list, whose widest row is a position label and
	 * a support word in a fixed 150 px column anchored to the panel's right edge.
	 *
	 * The binding constraint is not line containment but two lines on one row printing over
	 * each other: the support word's own slack is a constant 71 px however narrow the panel
	 * gets, and what runs out is the gap between "course 12 · #3" and the column walking left
	 * toward it. Swept in World.Menu.TheReadoutFitsInsideThePanel, that gap closes at about
	 * 231 px and is still 74 px clear at 380 px — 448 px is comfortably outside it.
	 *
	 * The height is 0.7 of the full panel's, so the panel keeps a sensible shape rather than
	 * a band across the screen; 448 x 392 against 800 x 560 leaves 39 % of the area, well
	 * inside the two thirds the mode promises.
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
	 * A menu for one brick is a menu for a selection of one, spelled that way rather than
	 * duplicated, so a second copy of the fail-closed rule below cannot drift from it.
	 */
	return BuildPieceMenuRows(Actions, TArrayView<const FPieceRef>(&Ref, 1));
}

TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	TArrayView<const FPieceRef> Refs)
{
	TArray<FPieceMenuRow> Rows;

	/*
	 * No selection builds no rows. A menu with no target is a button with nothing behind it,
	 * and a row that exists must always carry at least one piece — which also lets
	 * everything below take Refs.Last() without a second guard.
	 */
	if (Refs.Num() == 0)
	{
		return Rows;
	}

	/*
	 * A ref missing either half builds nothing, however good the rest of the menu was. A
	 * row is a COMMAND rather than a readout, and a default FPieceRef is both what a click
	 * on the floor arrives as and what "we have no answer" looks like; offering the full
	 * menu against one would put a Delete button on screen with nothing behind it. (Zero is
	 * a real structure and piece — the first the game mode builds — so the sentinel is
	 * tested by name rather than truthiness.)
	 *
	 * The whole list goes rather than the bad entry — dropping one would put a button on
	 * screen that acts on fewer bricks than the player picked, and nothing about that reads
	 * as a bug.
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
	 * One row per action, in the order offered, and CanRun is not consulted: PieceActionsFor
	 * already decided which actions a piece may offer, and asking again would be a second,
	 * quieter copy of that policy.
	 */
	for (const FPieceAction* const Action : Actions)
	{
		/*
		 * A malformed row is skipped rather than dereferenced. AllPieceActions() cannot hold
		 * one, but this takes an array from anywhere, and dereferencing a null row would
		 * abort the whole run rather than dropping one entry.
		 */
		if (Action == nullptr || Action->Label == nullptr)
		{
			continue;
		}

		FPieceMenuRow& Row = Rows.AddDefaulted_GetRef();

		/*
		 * The action is carried by pointer and the label taken from it, never spelled here:
		 * "which entry did they choose" is a pointer comparison against the shipped table,
		 * which a copied row would break while keeping every label right.
		 */
		Row.Label = FString(Action->Label);
		Row.Action = Action;
		Row.Refs.Append(Refs.GetData(), Refs.Num());

		/*
		 * Whether choosing it destroys something is the action's own flag, copied like Label
		 * is and never re-decided — a widget styling a button by reading its caption would
		 * be a policy no test can reach.
		 */
		Row.bIsDestructive = Action->bIsDestructive;

		/*
		 * Taken from Refs rather than the selection the caller remembers, so a button
		 * cannot promise to act on a different number of bricks than it will.
		 */
		Row.TargetText = PresenterWordForTargetCount(Row.Refs.Num());

		/*
		 * The anchor is derived from the set, never stored beside it — a field naming a
		 * piece outside the selection would be worse than no field, since it is what a
		 * per-brick readout hangs off.
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
	 * The heading is set before any of the fail-closed routes below can take, since it is
	 * the one line that may not go quiet: a fixed panel is on screen while the selection is
	 * empty, while a ref names another wall and while nothing has been solved.
	 */
	Inspector.HeaderText = FString(PresenterPanelHeader);

	/*
	 * The count is the selection's own and never shrinks to what resolves. A ref naming
	 * another wall, one whose piece a cascade took, one missing a half — all still count,
	 * since the highlights on screen are drawn off the same set and a count that quietly
	 * disagreed with them would be the presenter contradicting itself.
	 */
	Inspector.SelectedCount = Selected.Num();

	/*
	 * Singular and plural are decided here, since "1 brick" against "1 bricks" is a branch
	 * and a branch in the widget is untested by construction.
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
	 * One entry per selected brick, in pick order, never reordered and never deduplicated.
	 * A projection of a list rather than a second implementation of set semantics:
	 * FPieceSelection guarantees the set, and a presenter that sorted or de-duped would look
	 * tidy while ceasing to agree with the order the batched commit runs in.
	 */
	Inspector.Pieces.Reserve(Selected.Num());

	/*
	 * Every label names where the brick is — "course 2 · #1" — when this binding can place
	 * it. The table is built once for the whole binding rather than per entry, since a
	 * position is a fact about a piece's neighbours: naming one brick needs every brick.
	 */
	const TArray<FString> PositionLabels = PresenterPositionLabels(Binding);

	/*
	 * Where it cannot, the label names both halves of the ref — "brick 9:1". A piece index
	 * alone is not an identity: two walls both have a brick 1, and a selection can hold refs
	 * from more than one, so an unqualified label would present two different bricks as one
	 * string.
	 *
	 * The fallback is the old rule kept word for word, justified entirely by totality — two
	 * selected bricks must never present as the same string — kept for refs with no position
	 * here at all: one naming another wall, one missing a half, one whose box is a NaN.
	 * "brick 4:-1" prints an absent half as what it is, not a piece index any wall can have,
	 * so no positioned brick can collide with it.
	 *
	 * The structure is checked before the handle, for the reason
	 * FStructureBinding::ResolvePiece checks it first: an unidentified binding must match
	 * nothing. This deliberately does not go through ResolvePiece, which fails closed on a
	 * REMOVED piece — a hole in a wall is still somewhere, and its box still says where.
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
		 * Whether it still names a brick is InspectPiece's question, asked of every entry
		 * rather than only the singled-out one — a ResolvePiece call spelled out here would
		 * be a second, quieter copy of the rule the breakout below already asks. A RELEASED
		 * brick reads LIVE — still a piece in the graph — and what the menu may do about it
		 * is PieceActionsFor's intersection, already said by the rows going empty.
		 */
		const FPieceInspection EntryInspection = InspectPiece(Binding, Ref);

		Entry.bIsLivePiece = EntryInspection.bIsPiece;

		/*
		 * Why that brick is or is not standing up, on every row rather than only the one
		 * singled out — off the same inspection, the point of taking it as a local. Eleven
		 * picked bricks are eleven identical rows without it, and a second InspectPiece call
		 * would be a second scan of the whole connection array for a question already
		 * answered. The dot beside the word is the same answer rather than a second one, so
		 * a row cannot say "grounded" in text and draw the colour of a brick that is falling.
		 */
		Entry.SupportBand = PresenterSupportBand(EntryInspection);
		Entry.SupportText = PresenterWordForSupportBand(Entry.SupportBand);
	}

	/*
	 * The inspected brick must be a member of the selection, found as an index rather than
	 * a bool: marking by index marks exactly one entry however many times the same brick
	 * was picked, so "at most one entry reads as inspected" holds by construction. An
	 * anchor outside the set it anchors is a readout of somebody else's brick.
	 */
	const int32 InspectedEntry = Selected.IndexOfByKey(InspectedRef);

	/*
	 * And it must be a live piece, InspectPiece's question and not a second one. A ref
	 * naming another structure, one missing a half and a piece a removal took all come back
	 * bIsPiece false, so the default-constructed inspection below IS the fail-closed answer.
	 */
	FPieceInspection Inspection;

	if (InspectedEntry != INDEX_NONE)
	{
		Inspection = InspectPiece(Binding, InspectedRef);
	}

	Inspector.bHasInspectedPiece = Inspection.bIsPiece;

	/*
	 * The hint belongs to exactly one of the three states a panel can be in. Bricks picked
	 * and none pointed at is the state that needs it; a brick pointed at must NOT carry it
	 * (the stale-field defect this struct is shaped against), and neither must an empty
	 * selection, where CountText already speaks for the state.
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
	 * The readout names its own subject, in the entry list's own words rather than a second
	 * derivation: the list scrolls, so the entry a breakout belongs to can be out of sight
	 * while the breakout stays. Copied off the marked entry, so the two halves cannot drift.
	 */
	Inspector.InspectedLabel = Inspector.Pieces[InspectedEntry].Label;

	/*
	 * What that brick is, beside where it is. The label above never says what it is made
	 * of, how big it is or what it weighs, so two pieces of one wall differing by a whole
	 * material and a factor of four in weight present identically.
	 *
	 * Composed here for the reason every other string on this struct is: choosing a unit, a
	 * precision and a separator is logic, and the menu widget was landed under a recorded
	 * exception to the TDD gate on the condition it holds none.
	 *
	 * Reached only past the bHasInspectedPiece return above, so empty exactly when no brick
	 * is singled out, and the handle it reads is InspectPiece's own resolved answer, never
	 * the ref.
	 */
	Inspector.IdentityText = PresenterIdentityLine(Binding, Inspection.PieceIndex);

	/*
	 * The readout's own dot and word, off one bucket for the reason every entry row's are:
	 * a panel with nothing singled out keeps the field's default — NotAPiece — rather than
	 * a colour left over from the brick the cursor has left.
	 */
	Inspector.SupportBand = PresenterSupportBand(Inspection);
	Inspector.SupportText = PresenterWordForSupportBand(Inspector.SupportBand);

	/*
	 * The joint list is summed up before it is broken out, and it is a fact about the graph
	 * rather than the solve: a brick nobody has solved for still has exactly the joints it
	 * was built with, and a brick with none still gets a sentence, since "no joints" is the
	 * truth about an isolated pad.
	 */
	Inspector.JointsText = PresenterWordForJointCount(Inspection.Joints.Num());

	/*
	 * A compact readout stops here: everything below is exactly the per-joint table and the
	 * scale that gives its bars meaning, so the cut is a placement rather than a filter.
	 *
	 * It is after JointsText deliberately: "3 joints" is a sentence about the BRICK rather
	 * than a row of the table, counted off InspectPiece's list rather than Inspector.Joints
	 * — so a compact readout says the brick has three neighbours while showing none of them,
	 * rather than reporting a trimmed list's count as a fact.
	 *
	 * Returning before the loop is what makes it a saving: a mode that built every row and
	 * then hid it would pay the whole price of the table for a panel that never draws it.
	 * The caption and ticks need no guard of their own, since both are set only inside the
	 * "there is a bar to label" block below.
	 */
	if (Detail == EPieceMenuDetail::Compact)
	{
		return Inspector;
	}

	/*
	 * The breakout is InspectPiece's answer converted and worded, row for row, in its
	 * order. Nothing here re-derives a number: the tier, force, ratio and the two break
	 * fields are read straight off the model. (The one thing read off the graph is the
	 * joint's PROFILE, which the inspection does not carry.) That includes the adjacency —
	 * a joint that has given is dropped from the solver's support lists before the tier is
	 * even decided, and it is exactly the row a player who just pulled a brick is looking for.
	 */
	Inspector.Joints.Reserve(Inspection.Joints.Num());

	for (int32 JointIndex = 0; JointIndex < Inspection.Joints.Num(); ++JointIndex)
	{
		const FJointInspection& Joint = Inspection.Joints[JointIndex];

		FInspectorJointRow& Row = Inspector.Joints.AddDefaulted_GetRef();

		/*
		 * The swatch is the row's own number, taken from the loop rather than from anything
		 * about the joint: a colour keyed on the connection or the far brick runs out at the
		 * first wall; keyed on the row it never can.
		 */
		Row.ColourSlot = PresenterColourSlotFor(JointIndex);

		Row.ConnectionIndex = Joint.ConnectionIndex;
		Row.OtherPieceIndex = Joint.OtherPieceIndex;
		Row.Role = Joint.Role;
		Row.bHasGiven = Joint.bHasGiven;
		Row.BreakPass = Joint.BreakPass;

		/*
		 * The one unit change, and the only place entitled to it: 1 N = 100 uu, named once as
		 * DestructionPresenter::ForceUnitsPerNewton — not ForceUnitsPerMPaSqCm, which is
		 * 10,000 (this factor times cm2-to-mm2), a clean 100x a tuned-looking readout would
		 * hide perfectly. Divided rather than multiplied by 0.01, since 0.01 is not
		 * representable and the quotient is what a decimal reading of the model gives.
		 */
		Row.ForceN = Joint.ForceUu.Size() / DestructionPresenter::ForceUnitsPerNewton;

		/*
		 * The bend through the same constant, not a second boundary: a moment is uu.cm and
		 * length is already centimetres, so this is the identical unit change the line above
		 * makes. The magnitude, for the reason ForceN takes one — which way a joint is being
		 * levered open is not a thing a line of text says.
		 */
		Row.MomentNCm = Joint.MomentUuCm.Size() / DestructionPresenter::ForceUnitsPerNewton;

		Row.UtilisationPercent = Joint.Utilisation * 100.0;

		/*
		 * The margin and the bar are transforms of the two numbers above, not a third trip
		 * to the graph: the reciprocal of a utilisation already on the row cannot disagree
		 * with the per cent beside it, which keeps the exact-equality sweep the readout test
		 * runs against InspectPiece holding.
		 */
		Row.MarginText = PresenterMarginText(Row.UtilisationPercent, Row.bHasGiven);
		Row.HeadroomFraction = PresenterHeadroomFraction(Row.UtilisationPercent, Row.bHasGiven);
		Row.MarginBand = PresenterMarginBand(Row.UtilisationPercent, Row.bHasGiven);

		/*
		 * The far end is named where it is, out of the table the entry list was named from
		 * rather than a second derivation. The row exists so a player can find the brick on
		 * the OTHER side of the joint, and printing one two inches under "course 2 · #1" by
		 * a different scheme would name bricks in one wall two ways; PositionLabels is a
		 * lookup, so the two halves of the panel cannot disagree.
		 *
		 * A removed far end still names a place: FPieceBinding keeps the box of a piece
		 * that has gone on purpose, so a severed joint says where the hole is.
		 *
		 * The fallback is the entry label's, word for word, down to the structure id: a
		 * player reading "brick 21:13" in a joint row and in the list above has to be able
		 * to tell it is one brick. That id is the BINDING'S, since a far end is a bare
		 * handle in the inspected brick's own structure and can never name another wall.
		 */
		const bool bFarEndIsPlaced = PositionLabels.IsValidIndex(Row.OtherPieceIndex)
			&& !PositionLabels[Row.OtherPieceIndex].IsEmpty();

		const FString OtherPieceText = bFarEndIsPlaced
			? PositionLabels[Row.OtherPieceIndex]
			: FString::Printf(TEXT("brick %d:%d"), Binding.StructureId, Row.OtherPieceIndex);

		/*
		 * A joint that has given is a different sentence, not a different number: it
		 * carries nothing, so it reads 0 N at 0 %, identical to an intact unloaded joint.
		 * Deciding that here keeps the widget free of the branch.
		 *
		 * The bending clause trails its number like every other clause on the line —
		 * "78.4 N", "49.000 %", "2.0× margin" are all <number> <word> — and is empty on the
		 * joints not being levered open, almost all of them. Without it a bent joint prints
		 * a force and a percentage with no arithmetic between them.
		 */
		/*
		 * What holds it, beside where it is: the role says which face the joint is on, this
		 * says what is IN it, the fact that decides whether the joint gives.
		 *
		 * Read off the graph rather than off the inspection, the one thing in this loop
		 * that is: a profile is not a reading of the solve — a brick nobody has solved for
		 * is fastened with exactly what it was built with — so there is no second
		 * derivation to drift from. FJointInspection does not carry it.
		 */
		const FString ProfileWord =
			PresenterWordForJointProfile(Binding.GetStructure(), Row.ConnectionIndex);

		Row.Text = Row.bHasGiven
			? FString::Printf(
				TEXT("#%d  %s  %s  broken (went with a removed piece)"),
				Row.ConnectionIndex, *OtherPieceText,
				PresenterWordForJointRole(Row.Role))
			: FString::Printf(
				TEXT("#%d  %s  %s  %s  %s  %.3f %%  %s%s"),
				Row.ConnectionIndex, *OtherPieceText,
				PresenterWordForJointRole(Row.Role), *ProfileWord,
				*PresenterForceText(Row.ForceN), Row.UtilisationPercent, *Row.MarginText,
				*PresenterBendingText(Row.MomentNCm));
	}

	/*
	 * The bar is labelled exactly when there is a bar to label: a brick with no joints
	 * draws none, so a caption beside it would be a label on nothing. The caption quotes
	 * the top of the scale rather than spelling it again, so a bar redrawn over a different
	 * number of decades cannot go on promising three.
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
	 * Compact is the only arm that withholds anything, and every other answer is Full,
	 * including an enumerator nobody declared: EPieceMenuDetail is a uint8 and a cast is all
	 * it takes to produce one. Answering an unknown value with the compact size would
	 * suppress numbers somebody asked for (the reason Full is enumerator zero); the full
	 * size only spends screen. Full is the fall-through rather than a case, and the switch
	 * is left exhaustive so a mode added later is a compiler warning here.
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
	 * Anything that is not a number fails the whole vector to the origin, rather than only
	 * the axis it arrived on — the same rule ClampPanelOffset states, and the same fault: a
	 * screen whose edges are not known is one where half an answer looks deliberate.
	 * FMath::Max discards a NaN and FMath::Min replaces it, so an unguarded subtraction
	 * below would hand back a plausible corner.
	 */
	if (!PresenterPanelPixelsAreFinite(PanelSizePx)
		|| !PresenterPanelPixelsAreFinite(ViewportSizePx)
		|| !FMath::IsFinite(MarginPx))
	{
		return FVector2D::ZeroVector;
	}

	/*
	 * A negative size is not a small panel, nor a negative margin a small margin — both
	 * WIDEN the room the subtraction thinks it has, the fail-open direction reached by
	 * arithmetic that reads reasonably on its own line.
	 */
	if (!PresenterPanelSizeIsUsable(PanelSizePx)
		|| !PresenterPanelSizeIsUsable(ViewportSizePx)
		|| !PresenterPanelMarginIsUsable(MarginPx))
	{
		return FVector2D::ZeroVector;
	}

	/*
	 * The home itself: the panel's right edge stands MarginPx in from the viewport's, and
	 * its height is centred down the screen. The margin is horizontal only, since "centred"
	 * already answers the vertical question.
	 *
	 * Pinned at zero on each axis independently: a viewport narrower than the panel, or a
	 * margin wider than the room left over, makes the subtraction NEGATIVE — the panel's
	 * heading off the top-left of the screen, the one corner that has to stay grabbable.
	 *
	 * Needs no upper pin and so composes with ClampPanelOffset rather than restating it:
	 * the margin is non-negative, so the X answer is never past Viewport - Panel, and half
	 * of a non-negative gap is never past the whole of it.
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
	 * Anything that is not a number fails the whole vector to the origin, rather than only
	 * the axis it arrived on: a viewport whose height is not a number is a screen whose
	 * edges are not known, and half an answer against it looks deliberate. Zero is a corner
	 * that is always on screen and always grabbable.
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
	 * The two axes are pinned independently: a drag past the right edge must not also
	 * reset the vertical position, or a player dragging along one edge would see the panel
	 * jump on the other.
	 */
	return FVector2D(
		PresenterPanelAxisPinned(DesiredOffsetPx.X, ViewportSizePx.X - PanelSizePx.X),
		PresenterPanelAxisPinned(DesiredOffsetPx.Y, ViewportSizePx.Y - PanelSizePx.Y));
}

EJointMarginBand WorstJointBandForPiece(const FStructure& Structure, int32 PieceIndex)
{
	/*
	 * One question closes every handle that names nothing, exactly as InspectPiece's does:
	 * IsPieceRemoved already answers true for a negative handle, INDEX_NONE, a handle past
	 * the end and a piece the player pulled out.
	 */
	if (Structure.IsPieceRemoved(PieceIndex))
	{
		return EJointMarginBand::Critical;
	}

	/*
	 * A structure nothing has solved is not a comfortable one: GetConnectionUtilisation
	 * answers ZERO before any load has been routed, so reading it straight would paint an
	 * untouched wall green end to end, drawing "no data" as headroom. HasSupportAnswer is
	 * the one accessor that can tell the two apart — the support array is sized by
	 * SolveLoads and nothing else, so its extent IS the set of handles the last solve
	 * answered for.
	 */
	if (!Structure.HasSupportAnswer(PieceIndex))
	{
		return EJointMarginBand::Critical;
	}

	/*
	 * The solve is asked whether anything is holding this piece up before its joints are
	 * asked anything at all. A piece with no path to the earth is Critical whatever its
	 * joints read — the ordinary shape of a wall coming down, where what's left hanging off
	 * the piece is an unloaded joint reading a fraction of a per cent: a brick in mid-air
	 * drawn as the safest thing on the wall.
	 */
	if (!Structure.IsPieceSupported(PieceIndex))
	{
		return EJointMarginBand::Critical;
	}

	/*
	 * The worst of the piece's own joints, bucketed one at a time by the function the rows
	 * use. Written this way round so that PresenterMarginBand is the only thing that ever
	 * compares a number against the two edges: an FMath::Max taken over utilisations
	 * instead would discard a NaN (DESIGN §4), letting a degenerate joint vanish rather
	 * than reading Critical through PresenterMarginBand.
	 *
	 * A joint that has given is skipped, the one place this rule differs from the row's: a
	 * row says "this joint is gone" and Critical is right for it, but carried into a PIECE
	 * aggregate the same rule would paint every neighbour of every deleted brick red.
	 */
	EJointMarginBand Worst = EJointMarginBand::Comfortable;
	bool bAnyLiveJoint = false;

	for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
	{
		const FConnection& Connection = Structure.GetConnection(Index);

		if (Connection.PieceA != PieceIndex && Connection.PieceB != PieceIndex)
		{
			continue;
		}

		if (Connection.HasGiven())
		{
			continue;
		}

		bAnyLiveJoint = true;

		// bHasGiven is false because the given ones are already gone (skipped above).
		const EJointMarginBand Band = PresenterMarginBand(
			100.0 * Structure.GetConnectionUtilisation(Index), /*bHasGiven*/ false);

		if (static_cast<uint8>(Band) < static_cast<uint8>(Worst))
		{
			Worst = Band;
		}
	}

	/*
	 * A supported piece with nothing left to read is not a piece in trouble: a brick with
	 * no joints at all — the first one a player lays on the earth — and one whose every
	 * joint has given both arrive here, with the guard above already establishing something
	 * holds this one up. Critical would paint the single brick on an empty plot red the
	 * moment it landed.
	 */
	return bAnyLiveJoint ? Worst : EJointMarginBand::Comfortable;
}
