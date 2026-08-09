// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE LEANING-STACK ACCEPTANCE SET — the one self-weight fixture family that can demand the
 * missing stability check (DESIGN.md §7 gaps 1 and 6), and the red test for the interim
 * overturning guard (evolution step 2, disposable by design, deleted at step 4).
 *
 * WHAT IT IS. A single column of standard bricks, each course mortared to the one below and
 * offset a fixed distance sideways, so the stack races out over its own base. The offset is
 * constant and only the HEIGHT varies between rows: overturning demand at the bottom bed joint
 * grows as the SQUARE of the course count (m courses above it weigh m bricks and stand m/2
 * offsets further out), while the joint's restoring capacity — bond strength times a fixed
 * section — does not grow at all. Somewhere on that ladder a real stack stops being a lean and
 * becomes a fall, and the model today reads every rung identically.
 *
 * WHY THE APPROVED SHAPE MOVED, WITH THE ARITHMETIC (the review-queue item said 2 cm/course at
 * 5/10/15/20 courses, "stands while the resultant is inside the base"). Worked honestly per the
 * case-12 corollary (DESIGN.md §8, 2026-08-09), the 2 cm ladder does not discriminate:
 *
 *   - At 2 cm/course the bottom joint's overlap is 19.5 cm (W = 649.59 cm3, A = 199.875 cm2)
 *     and the first-crack tension demand is 410.6*m^2 - 1334.4*m pascals, which reaches the
 *     MEAN bond bracket (0.6-1.0 MPa, see below) only at m >= 40-51 — a 41-to-52-course stack.
 *     The approved 15- and 20-course rows sit past the RIGID-BLOCK tipping point (m >= 10) but
 *     the bonded bed rescues them by ~9.7x and ~4.9x (first-crack at m = 14 and 19 against the
 *     0.6 floor), exactly as it rescued case 12's pier: they honestly STAND, and there is no
 *     red in them.
 *   - Dry stone cannot be the fixture either, for the OPPOSITE reason: its tensile strength is
 *     an exact zero, so the model condemns any joint outside the kern (e > overlap/6), while a
 *     real dry stack rocks on its edge and stands to e = overlap/2. A 5-course, 2 cm/course dry
 *     stack — e = 4 cm against a 9.75 cm half-overlap, standing comfortably in reality — reads
 *     utilisation = Max() today. Every dry rung is wrong in the WRONG DIRECTION for this red
 *     (standing reads as falling); that is the missing no-tension rocking model, a different
 *     gap, noticed and left alone.
 *
 *   So the family is MORTARED, at 10 cm/course — a lean the bond genuinely cannot hold once the
 *   stack is tall enough — and the heights are chosen so every verdict clears the whole mean
 *   bracket with margin, rather than 5/10/15/20 whose middle rungs land inside it.
 *
 * THE STRENGTH BASIS THE VERDICTS ARE RULED AGAINST, because the coded profile cannot be it.
 * GeneralPurposeMortar carries f_xk1 = 0.10 MPa, a CHARACTERISTIC design value; DESIGN.md §3
 * records the user's decision that verdicts are ruled at MEAN strength, with the code re-anchor
 * deferred until after the LP oracle. UK NA Table NA.6 gives clay-unit characteristic bond of
 * 0.3-0.5 MPa by water absorption, and mean tested bond runs about twice characteristic, so the
 * honest bracket is roughly 0.6-1.0 MPa. Every verdict below clears BOTH ENDS of it:
 *
 *     row  courses  m   first-crack demand      verdict     margin
 *      1      5      4   0.0854 MPa             STANDS      7.0x under the 0.6 floor
 *      2      8      7   0.2734 MPa             STANDS      2.2x under the 0.6 floor
 *      3     30     29   4.899 MPa              FALLS       4.9x over the 1.0 ceiling
 *      4     40     39   8.890 MPa              FALLS       8.9x over the 1.0 ceiling
 *
 * demand(m) = (M/W - N/A) at the bottom bed joint: M = w*(d/2)*m^2 (w one brick's weight,
 * d the offset — the lever sum over m courses is d*(m^2)/2 exactly), W = t*(L-d)^2/6 the
 * overlap rectangle's modulus, N = m*w, A = t*(L-d). First-crack (uncracked elastic) is the
 * honest criterion for a brittle bond — DESIGN.md's own uncracked-section argument: the joint
 * reaches its flexural strength before a crack propagates, and once cracked it unzips. The
 * falling rows are ALSO checked against the most forgiving defensible reading, a rigid-plastic
 * stress block at the 1.0 ceiling (restoring = f*A*overlap/2 about the bearing edge): row 3
 * overturns that too at 1.59x (1077 N.m demand against 678), row 4 at 2.90x (1969 N.m). The
 * gap between 0.2734 and 4.899 MPa is the WINDOW the guard's effective bond figure must land
 * in, and it is 18x wide — any honest mean figure fits, and no tuning can satisfy the rows
 * without one.
 *
 * WHAT THE JOINT CHECKS READ, AND WHY THE FALLING ROWS READ SAFE BEFORE THE GUARD. Every
 * course is seated on
 * exactly one course, so the whole stack above any bed joint is one "corbelling body" and
 * CorbellingBodyDepthCm's floor credits the joint a composite section the FULL HEIGHT of the
 * stack: D = 7.5*m, W_c = t*D^2/6. That grows as m^2 — precisely cancelling the m^2 in the
 * demand — so the composite tension reading is the same number at EVERY height:
 *
 *     sigma_c = [w*(d/2)*m^2] / [t*(7.5m)^2/6] = 0.01388 MPa,  utilisation 0.1388 against
 *     the coded 0.10, for every joint with two or more courses above, at 8 courses and at 40.
 *
 * A height-independent reading of a fixture whose real risk grows quadratically is the whole
 * defect in one number. It is ALSO the sharpest fixture yet for the open composite-depth
 * "courses crossing the plane" question (DESIGN.md §5.5): any vertical plane through this
 * staircase is crossed by only ~L/d ≈ 2.15 courses, so the credited deep beam — 30 courses,
 * 225 cm — is masonry that is simply not there on the resisting plane. The two defects are one
 * failure here: credit only the ~16 cm of section that crosses the plane and the falling rows
 * read 3.6-45x. The fix that landed is the interim guard (FStructure::BreakOverturnedBodies,
 * DESIGN.md §7 evolution step 2); the plane rule remains open, and note it ALONE at today's
 * characteristic 0.10 would also condemn row 2 (0.2734/0.10 = 2.7) — which is why the guard
 * carries its own mean-basis bond term rather than reading the coded strength.
 *
 * AND THE GUARD'S BOND TERM MUST BE MEAN-BASIS OR IT BREAKS A STANDING RULING. The free-body
 * check of corbel A (four bricks, ~107 N, resultant ~17.4 cm past its bearing edge, ~18.5 N.m
 * of overturning about it) against its 179.48 cm3 root patch: at characteristic 0.10 MPa the
 * bond restores ~17.9 N.m and the guard would condemn the corbel the user ruled must stand; at
 * the 0.6 mean floor it restores ~108 N.m and the ruling survives 5.8x. The corbel family is
 * the guard's regression suite as much as this file is its driver.
 *
 * ASSERTIONS, per DESIGN.md §4. The STANDS rows assert both halves — nothing loses the earth
 * AND no cascade pass broke anything. The FALLS rows assert the OUTCOME: every course above the
 * grounded base loses its path to the earth and the base keeps its own, with zero pieces
 * Stranded so a routing limitation cannot wear the collapse's clothes. Displacement is never
 * read. No mechanism assertion names a specific joint's HasGiven, deliberately: the guard is a
 * free-body check and how it expresses "this body has no equilibrium" (severing the seat,
 * releasing the body) is implementation the outcome assertion must not dictate.
 *
 * THE FALLING ROWS ONCE PINNED WHAT THE MODEL DID INSTEAD (WallAcceptanceTest's DropsToday
 * convention): drops nothing, worst joint ~0.1388. Those pins were deleted in the same edit
 * that landed the guard and turned the rows green, per the convention. The FStackCase pin
 * fields and their checking block remain as unarmed machinery for the next known-wrong row.
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on (weight is mass x 980), everything is connected,
 * and the assertions are on solver state and outcome. Same footing as BeamAcceptanceTest.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 *
 * NOTHING IS IMPORTED FROM THE CODE UNDER TEST EXCEPT THE PRODUCER (Layout::MakeInterface,
 * whose emitted areas are checked against an interval intersection computed here) and the
 * mortar profile the stack is laid in (whose two figures the verdicts turn on are asserted as
 * fixture preconditions). The statics, the section moduli, the unit conversion and the mean
 * bond bracket are derived in this file, so a wrong production constant DISAGREES with this
 * file rather than agreeing with it.
 */
namespace LeaningStackTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * THE GEOMETRY. Every length is centimetres, at Unreal's default 1 uu = 1 cm.
	 * ================================================================================ */

	/** The standard brick every anchor in this project is derived from. */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	/** Fired clay, 1.9 g/cm3 — the same figure the wall fixtures use. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/**
	 * How far each course is laid past the one below, along the brick's length.
	 *
	 * 10 cm — just under half a brick per course, a lean nothing sane builds. CHOSEN SO THE
	 * BOND CANNOT RESCUE THE TALL ROWS: at the spec's original 2 cm the mean bond holds the
	 * stack to ~41-52 courses (see the file header), so the offset had to grow until the
	 * ladder's verdicts clear the whole mean bracket at fixture-sized heights. One offset for
	 * the family: the rows isolate HEIGHT and nothing else.
	 */
	constexpr double OffsetPerCourseCm = 10.0;

	/** A 1 cm mortar bed, so the course pitch is 7.5 cm — standard brickwork. */
	constexpr double BedJointThicknessCm = 1.0;
	constexpr double CoursePitchCm = BrickHeightCm + BedJointThicknessCm;

	/* ================================================================================
	 * UNITS AND THE STRENGTH BASIS. Every figure cited; none imported.
	 * ================================================================================ */

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. DELIBERATELY NOT
	 * DestructionForce::ForceUnitsPerMPaSqCm: this file has to fail if that constant is wrong
	 * rather than agree with it.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/**
	 * The MEAN bed-joint bond bracket the verdicts are ruled against, MPa.
	 *
	 * UK NA to BS EN 1996-1-1, Table NA.6: characteristic f_xk1 for clay units in M4/M6
	 * general-purpose mortar is 0.5 / 0.4 / 0.3 by water absorption (<7% / 7-12% / >12%), and
	 * mean tested bond strength runs about 2x characteristic (DESIGN.md §3's own bracket, the
	 * basis of the 2026-08-09 case-12 ruling). So 0.6 is the mean for the weakest-bonding clay
	 * and 1.0 for the strongest; a verdict is only written here if it holds at BOTH ends.
	 */
	constexpr double MeanBondFloorMPa = 0.6;
	constexpr double MeanBondCeilingMPa = 1.0;

	/* ================================================================================
	 * THE INDEPENDENT ORACLE: free-body statics of an offset column, from first principles.
	 * None of it mirrors production — the solver accumulates down a support graph; this is
	 * moments about one joint of one rigid body, which is where the verdicts come from.
	 * ================================================================================ */

	/** What every bed joint in the stack is: the overlap rectangle two offset courses share. */
	constexpr double OverlapCm = BrickLengthCm - OffsetPerCourseCm;
	constexpr double BedAreaSqCm = OverlapCm * BrickWidthCm;

	/** W = t * overlap^2 / 6 about the axis the stack leans over. */
	constexpr double BedModulusCm3 = BrickWidthCm * OverlapCm * OverlapCm / 6.0;

	/** Density-first multiplication order — the PieceMassKg contract; 2.72163125 kg. */
	constexpr double BrickMassKg =
		ClayDensityGramsPerCubicCm * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 2667.198625 uu. */
	constexpr double BrickWeightUu = BrickMassKg * GravityCmPerSecondSquared;

	/**
	 * The overturning moment about the bottom bed joint's own centroid, uu.cm, with m courses
	 * above it.
	 *
	 * Course i (counting the base as 0) is centred at i*d. The joint between courses 0 and 1
	 * is centred at d/2, so the lever of course i about it is i*d - d/2, and the sum over
	 * i = 1..m is d*(m^2)/2 EXACTLY — the m*(m-1)/2 pair sum plus m half-offsets. That
	 * quadratic is the whole fixture: weight grows with m and the centroid walks out with m.
	 */
	double BottomJointMomentUuCm(int32 CoursesAbove)
	{
		return BrickWeightUu * (OffsetPerCourseCm / 2.0)
			* double(CoursesAbove) * double(CoursesAbove);
	}

	/**
	 * The first-crack (uncracked elastic) bond tension demand at the bottom bed joint, MPa:
	 * M/W minus the mean compression N/A that closes it. The honest failure criterion for a
	 * brittle bond, and the number the verdicts compare against the mean bracket.
	 */
	double FirstCrackDemandMPa(int32 CoursesAbove)
	{
		const double BendingUuPerSqCm = BottomJointMomentUuCm(CoursesAbove) / BedModulusCm3;
		const double MeanCompressionUuPerSqCm =
			double(CoursesAbove) * BrickWeightUu / BedAreaSqCm;

		return (BendingUuPerSqCm - MeanCompressionUuPerSqCm) / ForceUnitsPerMPaSqCmHere;
	}

	/**
	 * The rigid-plastic cross-check for a FALLS verdict: overturning about the joint's leading
	 * BEARING EDGE against the most generous restoring a bonded joint can offer — the full
	 * bond stress block, f * A * (overlap/2). Returned as demand/capacity, so > 1 overturns.
	 *
	 * More forgiving than first-crack by roughly the W-to-A*l/2 ratio of 3, and NOT the honest
	 * criterion for a brittle bond — it exists so a falling row cannot be argued back up by
	 * the most charitable reading available. Only the two FALLS rows are required to clear it.
	 */
	double PlasticOverturningRatio(int32 CoursesAbove, double BondMPa)
	{
		/* The body's centroid, d*(m+1)/2, less the leading edge of the bearing at L/2. */
		const double EdgeLeverCm =
			OffsetPerCourseCm * double(CoursesAbove + 1) / 2.0 - BrickLengthCm / 2.0;

		const double OverturningUuCm =
			double(CoursesAbove) * BrickWeightUu * EdgeLeverCm;

		const double RestoringUuCm =
			BondMPa * ForceUnitsPerMPaSqCmHere * BedAreaSqCm * (OverlapCm / 2.0);

		return OverturningUuCm / RestoringUuCm;
	}

	/* ================================================================================
	 * THE TABLE.
	 * ================================================================================ */

	enum class EVerdict : uint8
	{
		/** The bond at the bottom joint cannot hold the lean: everything above the base goes. */
		Falls,

		/** The bond holds the lean with margin at the conservative end of the mean bracket. */
		Stands,
	};

	const TCHAR* VerdictName(EVerdict Verdict)
	{
		return Verdict == EVerdict::Falls ? TEXT("FALLS") : TEXT("STANDS");
	}

	struct FStackCase
	{
		int32 Number = 0;
		const TCHAR* Title = nullptr;

		/** What this row's matched pair varies. Printed on failure. */
		const TCHAR* Isolates = nullptr;

		int32 Courses = 0;
		EVerdict Verdict = EVerdict::Stands;

		/**
		 * HOW THE MODEL READS THIS ROW TODAY — a characterisation of a wrong answer, set on
		 * the FALLS rows only, INDEX_NONE / unset elsewhere. Not an expectation: it is what
		 * the solver does instead of the physics, measured off a run and written down so a
		 * regression inside the known failure fails loudly rather than hiding behind the
		 * expected red. WHEN THE ROW IS FIXED, DELETE THESE IN THE SAME EDIT.
		 */
		int32 DropsToday = INDEX_NONE;
		double WorstUtilisationToday = 0.0;
	};

	TArray<FStackCase> AllStackCases()
	{
		TArray<FStackCase> Cases;

		/*
		 * ROW 1 — THE SHALLOW CONTROL. Demand 0.0854 MPa: 7.0x inside the 0.6 mean floor, and
		 * 26.8x inside the plastic reading. Any guard loose enough to let real corbels stand
		 * must let this stand; a guard that condemns it is a rigid-block check with no bond
		 * term, which is the corbel-A regression described in the file header.
		 */
		Cases.Add({ 1, TEXT("5 courses"), TEXT("height (vs case 4)"),
			/*Courses*/ 5, EVerdict::Stands });

		/*
		 * ROW 2 — THE DEEPEST ROBUST STAND. Demand 0.2734 MPa: 2.2x inside the 0.6 mean
		 * floor. One more course reads 0.359 (1.67x) and the next 0.458 (1.31x), so 8 is
		 * where the STANDS ladder stops clearing the bracket with real margin. This row is
		 * the LOWER EDGE of the guard's window: its effective bond figure must exceed
		 * 0.2734 MPa or this row goes red in the wrong direction.
		 */
		Cases.Add({ 2, TEXT("8 courses"), TEXT("height (vs case 3)"),
			/*Courses*/ 8, EVerdict::Stands });

		/*
		 * ROW 3 — THE SHALLOWEST ROBUST FALL. Demand 4.899 MPa: 4.9x over the 1.0 mean
		 * ceiling at first crack, and 1.59x over even the rigid-plastic stress block at that
		 * ceiling (1077 N.m about the bearing edge against 678 of restoring). The model today
		 * reads its worst joint at ~0.1388 and drops nothing.
		 */
		Cases.Add({ 3, TEXT("30 courses"), TEXT("height (vs case 2)"),
			/*Courses*/ 30, EVerdict::Falls });

		/*
		 * ROW 4 — THE ANCHOR FALL. Demand 8.890 MPa: 8.9x at first crack, 2.90x under the
		 * plastic reading. The model reads the IDENTICAL ~0.1388 it read at 8 courses —
		 * the height-independence that is the defect in one number.
		 */
		Cases.Add({ 4, TEXT("40 courses"), TEXT("height (vs case 1)"),
			/*Courses*/ 40, EVerdict::Falls });

		return Cases;
	}

	/* ================================================================================
	 * THE FIXTURE.
	 * ================================================================================ */

	struct FStack
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;

		/** The joint between the grounded base and course 1 — the critical one, m = n-1. */
		int32 BottomJoint = INDEX_NONE;
	};

	/**
	 * Lay one row's stack: course i centred at (i*d, 0, 3.25 + i*7.5), course 0 grounded.
	 *
	 * EVERY PAIR IS OFFERED AND MakeInterface REFUSES THE NON-FACES. Consecutive courses are
	 * separated by exactly the 1 cm bed on Z and overlap 11.5 x 10.25 in plane; courses two
	 * apart are 8.5 cm of air apart and form nothing. Deciding which pairs touch is the
	 * producer's job; what this file checks, below, is the AREA of everything it accepted.
	 */
	void LayStack(int32 Courses, FStack& OutStack)
	{
		for (int32 Course = 0; Course < Courses; ++Course)
		{
			FPieceBox Box;
			Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				double(Course) * OffsetPerCourseCm,
				0.0,
				BrickHeightCm / 2.0 + double(Course) * CoursePitchCm);

			OutStack.Structure.AddPiece(BrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			OutStack.Boxes.Add(Box);
		}

		for (int32 First = 0; First < OutStack.Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < OutStack.Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(
						First, OutStack.Boxes[First], Second, OutStack.Boxes[Second],
						BedJointThicknessCm, GeneralPurposeMortar, Joint))
				{
					const int32 Handle = OutStack.Structure.AddConnection(Joint);

					if (First == 0 && Second == 1)
					{
						OutStack.BottomJoint = Handle;
					}
				}
			}
		}
	}

	/** Which live pieces have lost their path to the earth. Stranded counts as fallen. */
	TArray<int32> FallenPieces(const FStack& Stack)
	{
		TArray<int32> Fallen;

		for (int32 Piece = 0; Piece < Stack.Structure.NumPieces(); ++Piece)
		{
			if (Stack.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Stack.Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				Fallen.Add(Piece);
			}
		}

		return Fallen;
	}

	/** How many live pieces the solver could not route at all. A precondition, never a verdict. */
	int32 StrandedCount(const FStack& Stack)
	{
		int32 Stranded = 0;

		for (int32 Piece = 0; Piece < Stack.Structure.NumPieces(); ++Piece)
		{
			if (!Stack.Structure.IsPieceRemoved(Piece)
				&& Stack.Structure.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}

		return Stranded;
	}

	/** What one row's stack did, as built and then after the cascade. */
	struct FStackResult
	{
		bool bLaid = false;

		/** Read from the non-destructive solve, before anything may break. */
		double WorstUtilisation = 0.0;
		int32 WorstJoint = INDEX_NONE;
		double BottomUtilisation = 0.0;
		double BottomMomentUuCm = 0.0;
		double BottomCompositeDepthCm = 0.0;

		/** Passes that broke at least one joint. */
		int32 Passes = 0;

		TArray<int32> Fallen;
		int32 Stranded = 0;
	};

	/** Lay it, read it, then let the cascade run. Readings describe the stack AS BUILT. */
	void RunStackCase(
		FAutomationTestBase& Test, const FStackCase& Case, FStack& OutStack, FStackResult& OutResult)
	{
		LayStack(Case.Courses, OutStack);

		if (OutStack.Structure.NumConnections() != Case.Courses - 1
			|| OutStack.BottomJoint == INDEX_NONE)
		{
			Test.AddError(FString::Printf(
				TEXT("case %d (%s): FIXTURE the producer must emit exactly one bed joint per ")
				TEXT("course above the base — %d course(s) got %d joint(s)"),
				Case.Number, Case.Title, Case.Courses, OutStack.Structure.NumConnections()));

			return;
		}

		OutResult.bLaid = true;

		OutStack.Structure.SolveLoads();

		for (int32 Index = 0; Index < OutStack.Structure.NumConnections(); ++Index)
		{
			const double Utilisation = OutStack.Structure.GetConnectionUtilisation(Index);

			if (Utilisation > OutResult.WorstUtilisation)
			{
				OutResult.WorstUtilisation = Utilisation;
				OutResult.WorstJoint = Index;
			}
		}

		OutResult.BottomUtilisation =
			OutStack.Structure.GetConnectionUtilisation(OutStack.BottomJoint);
		OutResult.BottomMomentUuCm =
			OutStack.Structure.GetConnectionMoment(OutStack.BottomJoint).Size();
		OutResult.BottomCompositeDepthCm =
			OutStack.Structure.GetConnectionCompositeDepthCm(OutStack.BottomJoint);

		OutResult.Passes = OutStack.Structure.SolveAndBreak();
		OutResult.Fallen = FallenPieces(OutStack);
		OutResult.Stranded = StrandedCount(OutStack);
	}

	/** Everything the solver read, printed whether the row passes or not. */
	void ReportStackCase(
		FAutomationTestBase& Test, const FStackCase& Case, const FStackResult& Result)
	{
		if (!Result.bLaid)
		{
			return;
		}

		const int32 CoursesAbove = Case.Courses - 1;

		Test.AddInfo(FString::Printf(
			TEXT("case %d (%s): %s. DERIVED bottom-joint M %.10g uu.cm, first-crack demand ")
			TEXT("%.10g MPa against mean bond %.2g-%.2g, plastic edge ratio %.10g at the ")
			TEXT("ceiling. SOLVER bottom joint utilisation %.10g, |M| %.10g uu.cm, composite ")
			TEXT("depth %.10g cm; worst joint %d at %.10g; passes %d; fallen %d; stranded %d"),
			Case.Number, Case.Title, VerdictName(Case.Verdict),
			BottomJointMomentUuCm(CoursesAbove), FirstCrackDemandMPa(CoursesAbove),
			MeanBondFloorMPa, MeanBondCeilingMPa,
			PlasticOverturningRatio(CoursesAbove, MeanBondCeilingMPa),
			Result.BottomUtilisation, Result.BottomMomentUuCm, Result.BottomCompositeDepthCm,
			Result.WorstJoint, Result.WorstUtilisation, Result.Passes,
			Result.Fallen.Num(), Result.Stranded));
	}

	/**
	 * Everything a row must satisfy before its verdict means anything: the fixture is the
	 * shape it claims, the solver could route it, and the RULING is robust — each verdict
	 * clears the whole mean bracket, so nobody can later shift a height into the ambiguous
	 * band (9-19 courses at this offset) without this failing.
	 */
	void CheckFixture(
		FAutomationTestBase& Test, const FStackCase& Case, const FStack& Stack,
		const FStackResult& Result)
	{
		const FString Where = FString::Printf(TEXT("case %d (%s)"), Case.Number, Case.Title);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: FIXTURE every piece and joint must know where it is, or there is no ")
				TEXT("eccentricity anywhere and the whole ladder reads a centred load"),
				*Where),
			Stack.Structure.HasCompleteGeometry());

		for (int32 Index = 0; Index < Stack.Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Stack.Structure.GetConnection(Index);

			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: FIXTURE joint %d must be the overlap rectangle two offset ")
					TEXT("courses really share"),
					*Where, Index),
				Joint.InterfaceAreaSqCm, BedAreaSqCm);

			const int32 Upper = FMath::Max(Joint.PieceA, Joint.PieceB);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: FIXTURE joint %d must BEAR its upper course — a stack with a ")
					TEXT("joint in the shear tier is a different fixture"),
					*Where, Index),
				Stack.Structure.GetJointRole(Index, Upper) == EJointRole::BedBeneath);
		}

		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: no piece may be Stranded — a verdict decided by the solver declining ")
				TEXT("to route is not a verdict about a leaning stack"),
				*Where),
			Result.Stranded, 0);

		const int32 CoursesAbove = Case.Courses - 1;
		const double DemandMPa = FirstCrackDemandMPa(CoursesAbove);

		if (Case.Verdict == EVerdict::Stands)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: RULING a STANDS row must clear the conservative end of the mean ")
					TEXT("bond bracket with real margin; demand %.10g MPa against %.2g needs ")
					TEXT("2x and has %.10g"),
					*Where, DemandMPa, MeanBondFloorMPa, MeanBondFloorMPa / DemandMPa),
				DemandMPa * 2.0 <= MeanBondFloorMPa);
		}
		else
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: RULING a FALLS row must clear the generous end of the mean bond ")
					TEXT("bracket with real margin; demand %.10g MPa against %.2g needs 2x ")
					TEXT("and has %.10g"),
					*Where, DemandMPa, MeanBondCeilingMPa, DemandMPa / MeanBondCeilingMPa),
				DemandMPa >= MeanBondCeilingMPa * 2.0);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: RULING a FALLS row must overturn even under the most charitable ")
					TEXT("reading, a rigid-plastic bond block at the mean ceiling; the edge ")
					TEXT("ratio is %.10g and must exceed 1.5"),
					*Where, PlasticOverturningRatio(CoursesAbove, MeanBondCeilingMPa)),
				PlasticOverturningRatio(CoursesAbove, MeanBondCeilingMPa) > 1.5);
		}

		/*
		 * BOND TENSION HAS TO BE THE AXIS THAT DECIDES, or the ladder is measuring something
		 * else. The only competitor gravity offers this fixture is the squeezed edge — there
		 * is no shear demand at all on a stack of horizontal beds — and at mean strengths
		 * (bond 0.6-1.0 against mortar crushing of order 10) tension outruns it ~12x at every
		 * height. Worked at the extreme row: M/W + N/A = 8.98 MPa of edge compression against
		 * ~10 of crushing, while the tension edge is 8.9x over its own ceiling.
		 */
		const double EdgeCompressionMPa = DemandMPa
			+ 2.0 * double(CoursesAbove) * BrickWeightUu / BedAreaSqCm / ForceUnitsPerMPaSqCmHere;

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: RULING bond tension must govern the verdict by a clear margin over ")
				TEXT("mortar crushing (tension %.10g of its %.2g ceiling; compression %.10g ")
				TEXT("of ~10)"),
				*Where, DemandMPa / MeanBondCeilingMPa, MeanBondCeilingMPa,
				EdgeCompressionMPa / 10.0),
			DemandMPa / MeanBondCeilingMPa > 5.0 * (EdgeCompressionMPa / 10.0)
				|| Case.Verdict == EVerdict::Stands);
	}
}

/**
 * THE CATALOGUE: four heights of one lean, each with the verdict a real stack gives.
 *
 * TWO ROWS ARE GREEN ON ARRIVAL AND SAY SO. Cases 1 and 2 stand today and stand tomorrow;
 * they are the controls that stop the guard being writable as "condemn every offset stack",
 * and they are proven to bite by mutation (zeroing the composite depth in SolveLoads turns
 * case 2 into a 2.7x tension failure and drops its seven courses — recorded in the report
 * that landed this file). Cases 3 and 4 were the red that drove the interim guard: the
 * JOINT checks still read their worst joint at ~0.1388 — the same number they read for
 * case 2, because the composite section credits the whole stack as a deep beam over every
 * bed joint — but BreakOverturnedBodies now notices the body those joints belong to has
 * walked off its base, severs the bearing, and brings the rows down. The 0.1388 reading is
 * the reason the guard exists; the rows are green because it does.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLeaningStackCatalogueTest,
	"DestructionGame.Acceptance.LeaningStack.Catalogue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLeaningStackCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace LeaningStackTestSupport;

	/*
	 * THE PROFILE FIGURES THE VERDICTS TURN ON, CHECKED RATHER THAN TAKEN ON TRUST. The coded
	 * tensile strength is the CHARACTERISTIC 0.10 the mean bracket above is derived from
	 * (mean = 2 x the UK NA characteristic band whose floor is 3 x this Eurocode table value);
	 * a retune of it moves every margin in this file and must land here first, loudly.
	 */
	TestEqual(TEXT("FIXTURE: the mortar's coded characteristic bond is EN 1996-1-1's 0.10"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.1);

	TestEqual(TEXT("FIXTURE: the mortar's coded crushing strength is M10's 10 MPa"),
		GeneralPurposeMortar.CompressiveStrengthMPa, 10.0);

	/* The section every demand above divides by: 10.25 * 11.5^2 / 6 = 225.927 cm3. */
	TestEqual(TEXT("FIXTURE: the overlap rectangle's modulus is t * overlap^2 / 6"),
		BedModulusCm3, 10.25 * 11.5 * 11.5 / 6.0);

	const TArray<FStackCase> Cases = AllStackCases();

	TestEqual(TEXT("FIXTURE: the catalogue is four cases"), Cases.Num(), 4);

	for (const FStackCase& Case : Cases)
	{
		FStack Stack;
		FStackResult Result;

		RunStackCase(*this, Case, Stack, Result);
		ReportStackCase(*this, Case, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		CheckFixture(*this, Case, Stack, Result);

		const FString Where = FString::Printf(
			TEXT("case %d (%s) [isolates %s]"), Case.Number, Case.Title, Case.Isolates);

		if (Case.Verdict == EVerdict::Stands)
		{
			/*
			 * BOTH HALVES. "Nothing fell" alone passes for a stack that severed its bottom
			 * bed and settled in place; "no pass broke" alone passes for a stack whose load
			 * never reached anything.
			 */
			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means nothing lost the earth; %d piece(s) did"),
					*Where, Result.Fallen.Num()),
				Result.Fallen.Num(), 0);

			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means no joint gave; the cascade ran %d breaking pass(es)"),
					*Where, Result.Passes),
				Result.Passes, 0);

			continue;
		}

		/*
		 * THE RED. A stack whose bottom joint carries a bond tension demand of 4.9-8.9 MPa
		 * against a mean bond of at most 1.0 has no equilibrium to find: everything above the
		 * grounded base comes down. Today the model reads the same joint at ~0.1388 of its
		 * coded strength — the composite deep beam credits the whole stack — and drops
		 * nothing, which is DESIGN.md §7 gaps 1 and 6 in one fixture.
		 */
		TArray<int32> WronglyStanding;

		for (int32 Course = 1; Course < Case.Courses; ++Course)
		{
			if (!Result.Fallen.Contains(Course))
			{
				WronglyStanding.Add(Course);
			}
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s: a lean the bond cannot hold comes down — every course above the ")
				TEXT("base must lose the earth, and %d of %d kept it; the bottom joint's ")
				TEXT("first-crack demand is %.10g MPa against a mean bond of at most %.2g, ")
				TEXT("and the model instead read the worst joint (%d) at %.10g of coded ")
				TEXT("capacity by crediting a %.10g cm composite section"),
				*Where, WronglyStanding.Num(), Case.Courses - 1,
				FirstCrackDemandMPa(Case.Courses - 1), MeanBondCeilingMPa,
				Result.WorstJoint, Result.WorstUtilisation, Result.BottomCompositeDepthCm),
			WronglyStanding.Num(), 0);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the grounded base is not what failed; it must keep the earth"),
				*Where),
			Stack.Structure.GetPieceSupport(0) == EPieceSupport::Grounded);

		/*
		 * THE PINS: what the model does INSTEAD, so a regression inside the known failure
		 * fails loudly. Delete both in the same edit that makes this row pass.
		 */
		if (Case.DropsToday != INDEX_NONE)
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: KNOWN-RED PIN — the model drops %d piece(s) here today. If you ")
					TEXT("just fixed this row, delete its DropsToday and WorstUtilisationToday; ")
					TEXT("if nothing here was meant to change, a regression moved a wrong ")
					TEXT("answer to a different wrong answer"),
					*Where, Case.DropsToday),
				Result.Fallen.Num(), Case.DropsToday);

			TestTrue(
				*FString::Printf(
					TEXT("%s: KNOWN-RED PIN — the model reads the worst joint at %.17g today ")
					TEXT("(height-independent: the composite m^2 cancels the demand's m^2); ")
					TEXT("it read %.17g"),
					*Where, Case.WorstUtilisationToday, Result.WorstUtilisation),
				FMath::Abs(Result.WorstUtilisation - Case.WorstUtilisationToday)
					<= Case.WorstUtilisationToday * 1.0e-9);
		}
	}

	return true;
}

/**
 * HEIGHT ALONE HAS TO DECIDE THE OUTCOME.
 *
 * Cases 2 and 4 are the same brick, the same mortar, the same 10 cm lean per course; the only
 * thing that differs is how many courses there are, and the overturning demand grows with the
 * square of that number while nothing on the restoring side grows at all. Eight courses at
 * 0.2734 MPa of bond demand must stand and forty at 8.890 must not.
 *
 * WHY IT IS WORTH A TEST BESIDE THE CATALOGUE: the catalogue could in principle be satisfied
 * by a model that condemns every offset stack (rows 1-2 would catch it, but as two separate
 * row failures). This is the discrimination stated as ONE relation — the answer must CHANGE
 * between two fixtures whose every per-course number is identical — which is the form a model
 * with a height-independent reading cannot fake. Today both rows answer identically: zero
 * pieces fall in either, and the worst-joint readings agree to nine decimal places.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLeaningStackHeightTest,
	"DestructionGame.Acceptance.LeaningStack.TheHeightAloneDecidesTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLeaningStackHeightTest::RunTest(const FString& Parameters)
{
	using namespace LeaningStackTestSupport;

	const TArray<FStackCase> Cases = AllStackCases();

	const FStackCase& Shorter = Cases[1];
	const FStackCase& Taller = Cases[3];

	TestTrue(TEXT("FIXTURE: the pair really is two heights of one lean"),
		Shorter.Courses < Taller.Courses);

	FStack ShorterStack;
	FStackResult ShorterResult;
	RunStackCase(*this, Shorter, ShorterStack, ShorterResult);
	ReportStackCase(*this, Shorter, ShorterResult);

	FStack TallerStack;
	FStackResult TallerResult;
	RunStackCase(*this, Taller, TallerStack, TallerResult);
	ReportStackCase(*this, Taller, TallerResult);

	if (!ShorterResult.bLaid || !TallerResult.bLaid)
	{
		return false;
	}

	TestTrue(
		*FString::Printf(
			TEXT("the same lean at two heights must separate: %d courses at %.10g MPa of ")
			TEXT("bond demand stand while %d at %.10g fall; the model dropped %d piece(s) ")
			TEXT("from the short stack and %d from the tall one"),
			Shorter.Courses, FirstCrackDemandMPa(Shorter.Courses - 1),
			Taller.Courses, FirstCrackDemandMPa(Taller.Courses - 1),
			ShorterResult.Fallen.Num(), TallerResult.Fallen.Num()),
		TallerResult.Fallen.Num() > ShorterResult.Fallen.Num());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
