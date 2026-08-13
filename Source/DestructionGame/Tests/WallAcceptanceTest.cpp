// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE WALL ACCEPTANCE SET — twenty configurations with an expected outcome for each, drawn
 * from how real masonry behaves rather than from what the solver computes.
 *
 * The catalogue is claude_plans/WALL_CASES.html and the user agreed it on 2026-08-06. This file
 * is that catalogue as a parameterised table: ADDING A CASE IS ADDING NUMBERS, NOT CODE. Case 20
 * is written; case 20's own uncertainty is recorded beside it.
 *
 * WHY A SPREAD RATHER THAN ONE FIXTURE. The arching argument cannot be settled by one collapsing
 * wall. FIVE MATCHED PAIRS differ by exactly one variable each — 7 vs 8 is depth of cover, 7 vs 9
 * is span, 7 vs 10 is the abutment, 13 vs 14 is corbel projection, 15 vs 16 is whether
 * superimposed compression suppresses bending tension — so a disagreement between the two halves
 * of a pair points at ONE TERM in the force calculation, which a single wall never can. A solver
 * that answers both halves of a pair the same way has no such term at all.
 *
 * NONE OF THE FIVE IS AN OUTCOME PAIR ANY MORE, AND `Acceptance.Wall.MatchedPairs` RETIRED WITH
 * THE LAST OF THEM ON 2026-08-12. That test compared how many bricks each half dropped, and it
 * needed the catalogue to rule the two halves differently; five re-rulings, none of them a solver
 * fix, have now made every half of every pair STAND:
 *
 *   13 vs 14   2026-08-07   a bonded corbel resists with its full depth
 *   15 vs 16   2026-08-07   a bonded header with nothing on it reads a sixth of f_xk1
 *   11 vs 12   2026-08-09   the one-cell pier takes the thrust (not one of the five, but the
 *                           same event: an outcome pair losing its separation)
 *    7 vs  8   2026-08-11   the coverless course jams into its abutments as a flat arch
 *    7 vs  9   2026-08-12   the ten-cell deep beam spans, so span stops separating on outcome
 *    7 vs 10   2026-08-12   the free-end panel cantilevers, so the abutment stops separating too
 *
 * An outcome pair whose halves answer identically separates nothing, and — this is what forced the
 * retirement rather than one more deleted row — 7 vs 10 had reached the state where it could only
 * PASS WHILE PRODUCTION WAS WRONG: the catalogue now rules both halves STANDS, so the honest
 * answer is 0 against 0 and the assertion is unsatisfiable, while the model's 12 unrouted bricks
 * made it read as a discrimination. A test that goes green on a defect is worse than no test.
 *
 * WHERE EACH DISCRIMINATION WENT, because deleting an assertion without saying is how a suite
 * quietly stops measuring something:
 *
 *   13 vs 14  Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome — doubling the step
 *             per course takes the worst joint from 0.070 to 0.195, a factor of 2.8.
 *   15 vs 16  Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome — the SAME joint of
 *             the SAME geometry reads 0.0018 with six courses on the header's tail and 0.0582 with
 *             nothing on it, a factor of 32.
 *    7 vs  9  Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome — NEW on 2026-08-12, written in
 *             the same slice that cost the outcome pair its separation. The jamb beside the
 *             opening reads 0.269 under a four-cell span and 0.985 under a ten-cell one, 3.66x,
 *             and the header of that test argues at length why this is NOT the 7-vs-8 trap.
 *   11 vs 12  the LP oracle: 128.12 on three cells of bearing against 89.12 on one, pinned in
 *             OracleSlowSweep.RigidBlock.WallsAndLadders.
 *    7 vs 10  the LP oracle likewise, and DELIBERATELY NOT relocated onto a production reading —
 *             see the ABUTMENT paragraph in the span test's header. wall-07 prices at 296.22 and
 *             wall-10 at 35.82, 8.27x in the physically right direction.
 *    7 vs  8  NOWHERE, AND THE SUITE IS POORER FOR IT: the readings run the wrong way (0.269 with
 *             eight courses of cover against 0.219 with one), because a downward-routing solver
 *             reads cover as load rather than as arch capacity. The recorded cost is in the CASE 8
 *             block of section B — after that ruling NO CASE IN THIS SET REFUSES ARCHING FOR LACK
 *             OF COVER.
 *
 * IF AN OUTCOME PAIR EVER COMES BACK — the wanted list has two candidates, a discriminator that
 * starves the ABUTMENT rather than the cover, and a lintel over case 7's opening — the shape to
 * rebuild is small and is recorded here so it need not be rediscovered: a table of
 * {variable, lesser case, greater case}, run both halves, and assert the greater loses strictly
 * more while the lesser loses NOTHING WHENEVER ITS OWN CATALOGUE VERDICT SAYS SO (read off
 * `EVerdict`, never listed in the test — that is what let case 11's two rulings in one day relax
 * and re-tighten the claim in the same edit instead of leaving a stale private copy behind).
 *
 * THREE VERDICTS, THREE ASSERTION SHAPES, per DESIGN.md §4's outcome-not-mechanism rule:
 *
 *   STANDS      nothing left the structure AND no joint anywhere gave. Both halves, because
 *               "no piece fell" alone passes for a wall that severed half its joints and stayed
 *               leaning together.
 *   LOCAL LOSS  the set of pieces that lost the ground is EXACTLY the named set. Identity, not
 *               a count: a test that only counted would be satisfied by the wrong bricks falling,
 *               and "the course over the doorway drops but the wall is fine" is the whole point
 *               of having a middle verdict at all.
 *   COLLAPSE    every piece of a named region lost the ground, and every piece of a named
 *               SURVIVOR region kept it. Two-sided, because a wall that comes down because
 *               everything comes down is not evidence that the span term works.
 *
 * AND AS OF 2026-08-12 THE CATALOGUE CONTAINS NO `Collapse` ROW AT ALL — NINETEEN `Stands` AND ONE
 * `LocalLoss` — WHICH MAKES THE THIRD ASSERTION SHAPE ABOVE PRESENTLY DEAD. Cases 9, 10 and 19
 * were the last three, all re-ruled to STANDS on the same day, so `Acceptance.Wall.Catalogue`'s
 * collapse arm and the matching branch of `ModelAgreesWithVerdict` are UNEXERCISED: neither could
 * fail today whatever it said, and a reader must not take their presence as coverage. Both are
 * kept rather than deleted because the wanted case 2b (CURRENT_STATE, "new acceptance cases
 * wanted") is a deliberately FALLING counter-case to case 9 and revives them; the day it lands,
 * the first thing to check is that the collapse arm still bites.
 *
 * DISPLACEMENT IS NOT USED AS A BREAK ASSERTION ANYWHERE HERE, and it could not be: DESIGN.md §4
 * is explicit that two pieces can sever and stay resting exactly in place. What is read instead is
 * whether a piece still has a path to the earth after the cascade — which is what the binding
 * pushes to physics, so it is the outcome and not the mechanism.
 *
 * NEEDS A TICKING WORLD: NO, DELIBERATELY. Everything DESIGN.md §4 asks of an integration test is
 * here — gravity is on (weight is mass x 980 and there is no way to switch it off), everything is
 * connected, and the assertion is on outcome — and the one thing a world would add is the WIRE
 * from the solver's answer to Chaos, which Tests/StructureIntegrationTest.cpp already covers three
 * times over and which is identical for all twenty rows. Twenty worlds of up to 375 brick
 * actors would cost minutes to say nothing new. If a row ever needs to be watched falling, promote
 * that ONE row into the integration file rather than moving this table.
 *
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. See CURRENT_STATE.md; the `using namespace` lives inside each RunTest body for
 * the same reason.
 *
 * NOTHING HERE IS IMPORTED FROM THE CODE UNDER TEST EXCEPT THE PRODUCER ITSELF. The grid, the
 * brick weight, the newton-to-Unreal conversion and every strength are re-derived below, so a
 * wrong constant in production makes this file DISAGREE with it rather than agree with it. The one
 * deliberate exception is Layout::MakeInterface, which decides whether two boxes share a face:
 * re-implementing that would be re-implementing the thing under test, and
 * Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays pins the fixture's own bricklaying against
 * Layout::RunningBond so this file cannot quietly become a second, drifting wall producer.
 */
namespace WallAcceptanceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * THE COORDINATING GRID, RE-DERIVED HERE. UK metric standard brick with a 1 cm joint, so a
	 * brick plus a joint is one CELL along the wall (22.5 cm) and one COURSE up (7.5 cm), and
	 * running bond offsets alternate courses by half a cell. Every position in the table below is
	 * quoted in CELLS — a piece's cell index is its centre X divided by 22.5 — because that is the
	 * unit the bond is built on and the only one in which "one brick out, mid-wall" is a number
	 * somebody can check by eye.
	 */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickDepthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double JointCm = 1.0;

	constexpr double CellPitchCm = BrickLengthCm + JointCm;
	constexpr double CoursePitchCm = BrickHeightCm + JointCm;
	constexpr double HalfCellCm = CellPitchCm * 0.5;

	/** What is left of a brick when a joint is taken out of it and the remainder halved. */
	constexpr double HalfBatLengthCm = (BrickLengthCm - JointCm) * 0.5;

	/** Every wall in this file has its left face here, so cell 0 is the first full brick. */
	constexpr double LeftFaceCm = -BrickLengthCm * 0.5;

	/**
	 * The shortest closing piece the fixture will lay at a course's left end.
	 *
	 * A course laid right-to-left rarely divides exactly, and what is left at the left end is a
	 * cut brick. Below this it is dropped instead, because a two-centimetre sliver is a piece with
	 * a plausible mass and an implausible joint, and it would sit at the FAR end from everything
	 * under test while being the first thing to break. 4 cm is comfortably under the 10.25 cm half
	 * bat a flush wall really closes with and comfortably over anything that would be laid by
	 * accident.
	 */
	constexpr double MinClosingPieceCm = 4.0;

	/** g/cm3. ClayBrick's published density, spelled out so a changed profile shows up here. */
	constexpr double ClayBrickDensityGramsPerCubicCm = 1.9;

	/**
	 * WEIGHT FROM MASS, DERIVED RATHER THAN IMPORTED.
	 *
	 * Unreal's gravity is 980 cm/s2 and mass is in kilograms, so kg x 980 IS the weight in Unreal
	 * force units — DESIGN.md §3's 1 N = 100 uu is already inside that number and applying it again
	 * is the 100x error the units section exists to prevent. Density first, then dimensions, which
	 * is the one association that lands exactly on 2.72163125 kg.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double FullBrickWeightUu =
		ClayBrickDensityGramsPerCubicCm * BrickLengthCm * BrickDepthCm * BrickHeightCm / 1000.0
		* GravityCmPerSecondSquared;

	/**
	 * Unreal force units per MPa per cm2, SPELLED OUT RATHER THAN IMPORTED.
	 *
	 * Production has one named boundary for this, Core/ConnectionStrength.h's
	 * ForceUnitsPerMPaSqCm. A test that read it would agree with a wrong one, so this writes the
	 * number itself and disagrees instead.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 10000.0;

	/** EN 1996-1-1 Table 3.4's f_vk0 for general purpose mortar, asserted against the profile. */
	constexpr double MortarShearCohesionMPa = 0.2;

	/** EN 1996-1-1 Table 3.2's f_xk1 for general purpose mortar, asserted against the profile. */
	constexpr double MortarFlexuralBondMPa = 0.1;

	/** A head joint: the end face of a brick, 10.25 cm through the wall by 6.5 cm high. */
	constexpr double HeadJointAreaSqCm = BrickDepthCm * BrickHeightCm;

	enum class EBond : uint8
	{
		/** Alternate courses offset half a cell, half bats filling the ends flush. */
		Running,

		/** Every course identical, so every head joint lines up through the whole wall. */
		Stack,
	};

	enum class EVerdict : uint8
	{
		Stands,
		LocalLoss,
		Collapse,
	};

	const TCHAR* VerdictName(EVerdict Verdict)
	{
		switch (Verdict)
		{
		case EVerdict::Stands:    return TEXT("STANDS");
		case EVerdict::LocalLoss: return TEXT("LOCAL LOSS");
		default:                  return TEXT("COLLAPSE");
		}
	}

	/**
	 * A rectangle in (course, cell) space — the one vocabulary the whole table speaks.
	 *
	 * A piece is in the region when its course is within the inclusive course range and its cell
	 * index is STRICTLY between the two cell bounds. Strict on purpose: every brick centre in
	 * every wall here is an exact multiple of a quarter cell, so a bound placed on a quarter-cell
	 * boundary would be a tolerance question, and every bound written below is at least an eighth
	 * of a cell (2.8 cm) clear of any brick centre.
	 */
	struct FWallRegion
	{
		int32 CourseLo = 0;
		int32 CourseHi = 0;
		double CellLo = 0.0;
		double CellHi = 0.0;
	};

	struct FWallCase
	{
		int32 Number = 0;
		const TCHAR* Title = nullptr;
		EVerdict Verdict = EVerdict::Stands;

		/** What this row's matched pair varies, printed on failure. Null for an unpaired row. */
		const TCHAR* Isolates = nullptr;

		int32 Courses = 0;

		/** Full bricks in an even course. */
		int32 Cells = 0;

		EBond Bond = EBond::Running;

		/** First course that steps out, or INDEX_NONE. Every course from it steps out again. */
		int32 CorbelFromCourse = INDEX_NONE;
		double CorbelStepCm = 0.0;

		/**
		 * A course whose end brick is pushed half a cell past the face of the wall, or INDEX_NONE.
		 *
		 * This is the projecting-header fixture. A flush odd course normally closes with a half
		 * bat; pushing the face out half a cell and closing with a FULL brick instead puts a whole
		 * brick where the bat was, half of it bearing on the course below and half of it over air.
		 */
		int32 ProjectingCourse = INDEX_NONE;

		/** Everything the player deletes, applied AFTER the intact wall has been checked. */
		TArrayView<const FWallRegion> Cuts;

		/**
		 * What must lose its path to the earth.
		 *
		 * EXACT for a local loss — the fallen set must be precisely the pieces named here, so the
		 * wrong bricks falling is a failure. A LOWER BOUND for a collapse, paired with MustStand
		 * as the upper one, because the exact boundary of a collapse is a model detail and
		 * over-claiming it would make the row fail for a reason nobody asked about.
		 */
		TArrayView<const FWallRegion> MustFall;

		/** What must keep it. Read only for a collapse; a local loss gets exactness instead. */
		TArrayView<const FWallRegion> MustStand;

		/**
		 * HOW MANY PIECES THE MODEL DROPS HERE TODAY — a CHARACTERISATION OF A WRONG ANSWER, set on
		 * the three known-red rows and INDEX_NONE everywhere else.
		 *
		 * IT IS NOT AN EXPECTATION AND IT ENDORSES NOTHING. `MustFall` and `MustStand` above are
		 * what a real wall does; this is what the solver does instead, measured off a run and
		 * written down. It exists because a row that is ALREADY RED absorbs a regression silently:
		 * case 20 is supposed to drop two bricks, drops nine, and would go on failing in exactly
		 * the same words if a change made it drop ninety. Pinning the count makes the known failure
		 * a fixed point rather than a hole in the net.
		 *
		 * AND SINCE 2026-08-12 TWO OF THE THREE SIT ON A ROW WHOSE VERDICT IS `Stands`, WHICH IS A
		 * NEW SHAPE FOR THIS FIELD AND WORTH READING TWICE. Until then every red row asked for
		 * MORE loss than the model produced, so a `DropsToday` was always a count the catalogue
		 * would have liked to be bigger. Cases 10 and 19 are the other way round: the user re-ruled
		 * both to STANDS on the same day, the model still drops 12 and 34, and the pin is now
		 * pointing at pieces that should never have left the wall. The direction does not change
		 * what the field means — it is still "what the solver does, never what it should do" — but
		 * a reader who assumes a `DropsToday` implies an expected collapse will misread those two.
		 *
		 * WHEN THE ROW IS FIXED, THIS ANCHOR MUST BE DELETED IN THE SAME EDIT — it will fail, and
		 * that failure is the reminder. Never "update" it to a new wrong number without saying in
		 * the change why the model's answer moved. Case 9's pin went exactly that way on
		 * 2026-08-12: its re-ruling to STANDS handed the row to the model, so `DropsToday = 0` had
		 * nothing left to characterise and was deleted rather than kept as a zero.
		 */
		int32 DropsToday = INDEX_NONE;

		/**
		 * HOW MANY LIVE PIECES THE SOLVER CANNOT ROUTE HERE TODAY. Zero is the claim; anything else
		 * is a CHARACTERISED DEFECT, written down on the row that has it.
		 *
		 * DESIGN.md §4 requires a collapse test to assert that nothing is `Stranded` at the moment
		 * it goes, so that a solver limitation cannot wear a collapse's clothes — and this file had
		 * never asserted it. Writing it down for the first time on 2026-08-09 found that three of
		 * the then-six red rows did strand: cases 10, 12 and 19 routed part of what they drop
		 * nowhere at all, so part of those verdicts was the solver declining to divide load round a
		 * loop rather than masonry failing. That is a finding about the SOLVER and it is recorded
		 * here rather than hidden by relaxing the assertion off those rows. Case 12's 11-strand pin
		 * died with its 2026-08-09 rewrite (the ten-cell cut that stranded is no longer laid), so
		 * TWO exceptions remain — cases 10 and 19 — and eighteen of the twenty make the plain zero
		 * claim. Neither exception can grow by one piece without failing.
		 *
		 * THE 2026-08-12 RULINGS SHARPENED WHAT THOSE TWO NUMBERS MEAN RATHER THAN MOVING THEM.
		 * Both rows are now ruled STANDS, and the LP-oracle sweep measured that production reaches
		 * its 12 and its 34 in ZERO cascade passes at worst readings of 0.300 and 0.318 — no joint
		 * anywhere came near capacity. So on those two rows the stranding is not a footnote to a
		 * collapse; it is the same finding as the drop count, said twice: nothing broke, the router
		 * simply had nowhere to send the load. That is precisely why the catalogue stopped calling
		 * either one a collapse — an absent mechanism is not a strength verdict.
		 *
		 * WHAT WOULD RETIRE IT: the loop-division rule DESIGN.md §5.1 records as still absent. When
		 * it lands, these two go to zero and the exceptions are deleted.
		 */
		int32 StrandsToday = 0;
	};

	/* ================================================================================
	 * THE FIXTURE'S OWN BRICKLAYER.
	 * ================================================================================
	 *
	 * WHY NOT Layout::RunningBond. Six of the twenty cases are not running-bond rectangles:
	 * two corbel out, two carry a projecting header, and two are stack bond. RunningBond lays one
	 * shape, so either this file gets a second producer or six cases get dropped — and the six
	 * dropped ones include two of the five matched pairs, which are the whole reason the set
	 * exists.
	 *
	 * THE BRICKLAYER IS ONE RULE, NOT SIX SPECIAL CASES. Every course is described by two numbers
	 * — where its right face is, and how long its rightmost piece is — and then laid RIGHT TO
	 * LEFT on the 22.5 cm pitch, closing with whatever is left at the wall's left face. Running
	 * bond, stack bond, a corbel and a projecting header are all values of those two numbers:
	 *
	 *      running bond    right face fixed; odd courses close with a half bat at the right
	 *      stack bond      right face fixed; every course closes with a full brick
	 *      corbel          right face steps out once per course from CorbelFromCourse
	 *      header          one course's right face is half a cell further out
	 *
	 * Laying right-to-left rather than left-to-right is what keeps the cut piece at the LEFT end,
	 * far from every corbel and header under test, instead of in the middle of what is being
	 * measured.
	 *
	 * AND IT IS CHECKED AGAINST THE REAL PRODUCER. A flush running-bond wall laid here must be the
	 * same wall Layout::RunningBond lays, brick for brick — see
	 * Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays. Without that, this file is a second
	 * definition of what a wall is and every number below is measured against the wrong one.
	 */

	/** The right face of a flush wall of this many cells. */
	double FlushRightFaceCm(int32 Cells)
	{
		return (Cells - 1) * CellPitchCm + BrickLengthCm * 0.5;
	}

	/** Where a course's right face is and how long its rightmost piece is. */
	void CourseGeometry(const FWallCase& Case, int32 Course, double& OutRightCm, double& OutFirstLenCm)
	{
		OutRightCm = FlushRightFaceCm(Case.Cells);

		OutFirstLenCm = (Case.Bond == EBond::Stack || (Course % 2) == 0)
			? BrickLengthCm
			: HalfBatLengthCm;

		if (Case.ProjectingCourse == Course)
		{
			OutRightCm += HalfCellCm;
			OutFirstLenCm = BrickLengthCm;
		}

		if (Case.CorbelFromCourse != INDEX_NONE && Course >= Case.CorbelFromCourse)
		{
			OutRightCm += (Course - Case.CorbelFromCourse + 1) * Case.CorbelStepCm;
			OutFirstLenCm = BrickLengthCm;
		}
	}

	/**
	 * MASS FROM GEOMETRY, DERIVED HERE RATHER THAN IMPORTED.
	 *
	 * Density is g/cm3 and dimensions are cm, so cm3 x g/cm3 is grams and grams / 1000 is
	 * kilograms. No force conversion belongs here: 1 N = 100 uu is a property of forces and mass
	 * goes into Unreal unconverted. Density first for the same reason production does it — that is
	 * the association that lands exactly on 2.72163125 for a full brick.
	 */
	double PieceMassKgHere(const FPieceBox& Box)
	{
		return ClayBrickDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/** A wall, plus where every piece of it sits in (course, cell) terms. */
	struct FWall
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
		TArray<int32> CourseOf;
		TArray<double> CellOf;

		int32 NumPieces() const { return Boxes.Num(); }
	};

	bool RegionContains(const FWallRegion& Region, int32 Course, double Cell)
	{
		return Course >= Region.CourseLo
			&& Course <= Region.CourseHi
			&& Cell > Region.CellLo
			&& Cell < Region.CellHi;
	}

	bool AnyRegionContains(TArrayView<const FWallRegion> Regions, int32 Course, double Cell)
	{
		for (const FWallRegion& Region : Regions)
		{
			if (RegionContains(Region, Course, Cell))
			{
				return true;
			}
		}

		return false;
	}

	/** Lay the wall the case describes, before anything is cut out of it. */
	void LayWall(const FWallCase& Case, FWall& OutWall)
	{
		TArray<TArray<int32>> HandlesInCourse;

		for (int32 Course = 0; Course < Case.Courses; ++Course)
		{
			const double CentreZCm = BrickHeightCm * 0.5 + Course * CoursePitchCm;

			double RightCm = 0.0;
			double LenCm = 0.0;
			CourseGeometry(Case, Course, RightCm, LenCm);

			TArray<int32> Handles;

			while (RightCm - LeftFaceCm >= MinClosingPieceCm)
			{
				double LeftCm = RightCm - LenCm;

				if (LeftCm < LeftFaceCm)
				{
					LeftCm = LeftFaceCm;
					LenCm = RightCm - LeftCm;

					if (LenCm < MinClosingPieceCm)
					{
						break;
					}
				}

				FPieceBox Box;
				Box.CentreCm = FVector((LeftCm + RightCm) * 0.5, 0.0, CentreZCm);
				Box.ExtentCm = FVector(LenCm, BrickDepthCm, BrickHeightCm) * 0.5;

				/*
				 * THE BOX'S CENTRE IS THE CENTRE OF MASS, because a brick is a homogeneous solid
				 * and its mass came off that same box. Without it the wall has no eccentricity at
				 * all and every corbel in it reads as though its weight acted through the middle
				 * of its support — which is exactly the state HasCompleteGeometry exists to make
				 * askable, and it is asserted as a fixture precondition below.
				 */
				const int32 Handle = OutWall.Structure.AddPiece(
					PieceMassKgHere(Box), Course == 0, Box.CentreCm);

				OutWall.Boxes.Add(Box);
				OutWall.CourseOf.Add(Course);
				OutWall.CellOf.Add(Box.CentreCm.X / CellPitchCm);
				Handles.Add(Handle);

				RightCm = LeftCm - JointCm;
				LenCm = BrickLengthCm;
			}

			HandlesInCourse.Add(MoveTemp(Handles));
		}

		/*
		 * THE PAIRS: neighbours along a course, and every piece of the course below. Offering the
		 * whole course below rather than working out which pieces something spans is what keeps
		 * the mixed-size and corbelled cases honest — MakeInterface refuses the pairs that turn out
		 * to be diagonals, and that decision belongs to it and to nothing written here. Courses
		 * two apart are never offered because a 6.5 cm brick on a 7.5 cm pitch cannot reach.
		 */
		for (int32 Course = 0; Course < HandlesInCourse.Num(); ++Course)
		{
			const TArray<int32>& Row = HandlesInCourse[Course];

			for (int32 Index = 0; Index < Row.Num(); ++Index)
			{
				for (int32 Other = Index + 1; Other < Row.Num(); ++Other)
				{
					FConnection Joint;

					if (MakeInterface(
							Row[Index], OutWall.Boxes[Row[Index]],
							Row[Other], OutWall.Boxes[Row[Other]],
							JointCm, GeneralPurposeMortar, Joint))
					{
						OutWall.Structure.AddConnection(Joint);
					}
				}

				if (Course == 0)
				{
					continue;
				}

				for (const int32 Below : HandlesInCourse[Course - 1])
				{
					FConnection Joint;

					if (MakeInterface(
							Below, OutWall.Boxes[Below],
							Row[Index], OutWall.Boxes[Row[Index]],
							JointCm, GeneralPurposeMortar, Joint))
					{
						OutWall.Structure.AddConnection(Joint);
					}
				}
			}
		}
	}

	/** Every piece the case cuts away, in handle order. */
	TArray<int32> CutPieces(const FWallCase& Case, const FWall& Wall)
	{
		TArray<int32> Cut;

		for (int32 Piece = 0; Piece < Wall.NumPieces(); ++Piece)
		{
			if (AnyRegionContains(Case.Cuts, Wall.CourseOf[Piece], Wall.CellOf[Piece]))
			{
				Cut.Add(Piece);
			}
		}

		return Cut;
	}

	/**
	 * Which live pieces have lost their path to the earth.
	 *
	 * THIS IS THE OUTCOME, NOT THE MECHANISM. A joint severing is a step; what the player sees is
	 * which bricks come down, and a piece comes down exactly when the solve says nothing is
	 * holding it any more. Stranded counts as fallen for the same reason Falling does: the piece
	 * is not being carried, and whether the solver could not route it or genuinely has nothing to
	 * route it through is a different question — asked in the report line below, never folded into
	 * the verdict.
	 */
	TArray<int32> FallenPieces(const FWall& Wall)
	{
		TArray<int32> Fallen;

		for (int32 Piece = 0; Piece < Wall.Structure.NumPieces(); ++Piece)
		{
			if (Wall.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Wall.Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				Fallen.Add(Piece);
			}
		}

		return Fallen;
	}

	/**
	 * How many live pieces the solver could not route at all. A PRECONDITION, NEVER A VERDICT.
	 *
	 * Stranded means the solver declined to divide load round a loop, so it counts as fallen in
	 * FallenPieces above for the reason stated there — the piece is not being carried. But a row
	 * whose verdict was decided by that is a row about the SOLVER'S LIMIT rather than about the
	 * wall, and reading it as physics is how a solver limitation comes to wear a collapse's
	 * clothes (DESIGN.md §4). So it is counted separately, printed on every row, and asserted to
	 * be zero before any verdict is read.
	 */
	int32 StrandedCount(const FWall& Wall)
	{
		int32 Stranded = 0;

		for (int32 Piece = 0; Piece < Wall.Structure.NumPieces(); ++Piece)
		{
			if (!Wall.Structure.IsPieceRemoved(Piece)
				&& Wall.Structure.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}

		return Stranded;
	}

	/** The worst utilisation over the joints still in the structure, and which pair carries it. */
	double WorstUtilisation(const FWall& Wall, int32& OutPieceA, int32& OutPieceB)
	{
		double Worst = 0.0;
		OutPieceA = INDEX_NONE;
		OutPieceB = INDEX_NONE;

		for (int32 Index = 0; Index < Wall.Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Wall.Structure.GetConnection(Index);

			if (Joint.HasGiven())
			{
				continue;
			}

			const double Utilisation = Wall.Structure.GetConnectionUtilisation(Index);

			if (Utilisation > Worst)
			{
				Worst = Utilisation;
				OutPieceA = Joint.PieceA;
				OutPieceB = Joint.PieceB;
			}
		}

		return Worst;
	}

	/** A piece list as (course, cell), capped so a total collapse does not fill the log. */
	FString DescribePieces(const FWall& Wall, const TArray<int32>& Pieces)
	{
		if (Pieces.Num() == 0)
		{
			return TEXT("{}");
		}

		constexpr int32 MaxShown = 24;

		FString Line = TEXT("{");

		for (int32 Index = 0; Index < Pieces.Num() && Index < MaxShown; ++Index)
		{
			Line += FString::Printf(
				TEXT("%sc%d/%g"),
				Index == 0 ? TEXT("") : TEXT(", "),
				Wall.CourseOf[Pieces[Index]],
				Wall.CellOf[Pieces[Index]]);
		}

		if (Pieces.Num() > MaxShown)
		{
			Line += FString::Printf(TEXT(", +%d more"), Pieces.Num() - MaxShown);
		}

		return Line + TEXT("}");
	}

	/** What one case's wall did, once it has been laid, cut and solved to a standstill. */
	struct FWallResult
	{
		bool bLaid = false;

		int32 PiecesLaid = 0;
		int32 PiecesCut = 0;

		/** Passes that broke at least one joint, as built and then after the cut. */
		int32 IntactPasses = 0;
		int32 CutPasses = 0;

		/** Whether the intact wall stood — only meaningful for a case that cuts something. */
		bool bIntactStood = false;

		/** How many pieces the wall had already dropped BEFORE the player cut anything. */
		int32 IntactFallen = 0;

		/**
		 * Read AS BUILT, before anything is removed or broken, because the question is weaker
		 * afterwards: HasCompleteGeometry is a conjunction over what is still in the structure, so
		 * a removed piece with no centre of mass and a joint that has given both stop counting.
		 * Asking after the cut would let exactly the fixture defect this exists to catch through.
		 */
		bool bCompleteGeometryAsBuilt = false;

		TArray<int32> Fallen;

		/** Of those, how many the solver could not route rather than could not hold up. */
		int32 Stranded = 0;

		double Worst = 0.0;
		int32 WorstPieceA = INDEX_NONE;
		int32 WorstPieceB = INDEX_NONE;
	};

	/**
	 * Lay it, cut it, let the cascade run, and record what came down. NO ASSERTIONS AT ALL.
	 *
	 * THE INTACT WALL IS SOLVED FIRST AND THAT IS NOT DECORATION. A case whose wall was already
	 * falling apart before the player touched it measures nothing, and every "stands" row would
	 * fail for a reason that has nothing to do with the case. What is done with that reading is
	 * CheckWallFixture's business: a cutting row must have stood before the cut, and a row that
	 * cuts NOTHING — the corbels, the header and the intact walls — is a case whose as-built state
	 * IS the thing under test, so the catalogue reads it as a verdict rather than as a precondition.
	 *
	 * PURE, SO THAT IT CAN BE CACHED. Six tests in this file ask for the same twenty walls, and the
	 * walls were being laid and cascaded roughly sixty times a run to answer them. What may be
	 * shared between two tests is a wall's ANSWER, which is a function of the case and of nothing
	 * else; what may NEVER be shared is the assertions about it, because a fixture failure that
	 * fired only for whichever test happened to ask first would be a failure that moves when tests
	 * are reordered. Hence the split: this half is cached, and the checking half below is re-run,
	 * against the cached answer, exactly as often as it was before.
	 */
	void SolveWallCase(const FWallCase& Case, FWall& OutWall, FWallResult& OutResult)
	{
		LayWall(Case, OutWall);

		OutResult.PiecesLaid = OutWall.NumPieces();

		if (OutResult.PiecesLaid == 0)
		{
			return;
		}

		OutResult.bCompleteGeometryAsBuilt = OutWall.Structure.HasCompleteGeometry();

		OutResult.IntactPasses = OutWall.Structure.SolveAndBreak();
		OutResult.IntactFallen = FallenPieces(OutWall).Num();
		OutResult.bIntactStood = OutResult.IntactPasses == 0 && OutResult.IntactFallen == 0;

		if (Case.Cuts.Num() > 0)
		{
			const TArray<int32> Cut = CutPieces(Case, OutWall);

			OutResult.PiecesCut = Cut.Num();

			if (Cut.Num() == 0)
			{
				return;
			}

			for (const int32 Piece : Cut)
			{
				OutWall.Structure.RemovePiece(Piece);
			}

			OutResult.CutPasses = OutWall.Structure.SolveAndBreak();
		}

		OutResult.Fallen = FallenPieces(OutWall);
		OutResult.Stranded = StrandedCount(OutWall);
		OutResult.Worst = WorstUtilisation(OutWall, OutResult.WorstPieceA, OutResult.WorstPieceB);
		OutResult.bLaid = true;
	}

	/**
	 * Everything a row must satisfy before its verdict means anything, asserted once per CALLER.
	 *
	 * A wall the fixture laid without geometry, or one that fell down before the player touched it,
	 * produces a verdict that is about the fixture rather than about the physics — and a red for
	 * that reason sends whoever reads it chasing a bug that is not there.
	 */
	void CheckWallFixture(
		FAutomationTestBase& Test,
		const FWallCase& Case,
		const FWall& Wall,
		const FWallResult& Result)
	{
		if (Result.PiecesLaid == 0)
		{
			Test.AddError(FString::Printf(
				TEXT("case %d (%s): FIXTURE laid no bricks at all"), Case.Number, Case.Title));

			return;
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("case %d (%s): FIXTURE every brick and every joint must know where it is, or ")
				TEXT("there are no moments and every corbel reads as centred"),
				Case.Number, Case.Title),
			Result.bCompleteGeometryAsBuilt);

		if (Case.Cuts.Num() > 0)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("case %d (%s): FIXTURE the wall must stand before the player cuts it; it ")
					TEXT("broke joints in %d pass(es) and dropped %d piece(s) as built"),
					Case.Number, Case.Title,
					Result.IntactPasses, Result.IntactFallen),
				Result.bIntactStood);

			if (Result.PiecesCut == 0)
			{
				Test.AddError(FString::Printf(
					TEXT("case %d (%s): FIXTURE the cut regions named no brick at all"),
					Case.Number, Case.Title));

				return;
			}
		}

		/*
		 * AND NOTHING MAY BE Stranded, WHICH IS THE PRECONDITION THAT MAKES A VERDICT HONEST.
		 *
		 * The same claim `Core.Structure.AStaircaseVoidCondemnsTheCorbel` and the collapse rows of
		 * `Tests/StructureIntegrationTest.cpp` make, and `Acceptance.Beam.Catalogue` makes row by
		 * row: a Stranded piece is one the solver DECLINED to route round a loop, not one the wall
		 * failed to hold up. `FallenPieces` folds the two together on purpose — a piece nothing is
		 * carrying comes down either way — so without this the two are indistinguishable inside a
		 * verdict, and a row could name exactly the right bricks for entirely the wrong reason.
		 *
		 * WRITTEN AGAINST THE ROW'S OWN FIGURE RATHER THAN AGAINST A BARE ZERO, because two rows
		 * are not zero: see FWallCase::StrandsToday for what that means and why it is recorded on
		 * the rows instead of being relaxed away. Eighteen of the twenty make the plain claim.
		 */
		Test.TestEqual(
			*FString::Printf(
				TEXT("case %d (%s): FIXTURE %s — a verdict decided by the solver declining to divide ")
				TEXT("load round a loop is a statement about the solver rather than about the wall, ")
				TEXT("and %d of the %d piece(s) that came down here got there that way"),
				Case.Number, Case.Title,
				Case.StrandsToday == 0
					? TEXT("no live piece may be Stranded")
					: *FString::Printf(
						TEXT("this row is CHARACTERISED as stranding %d live piece(s) — a known ")
						TEXT("solver limitation, not an expectation"),
						Case.StrandsToday),
				Result.Stranded, Result.Fallen.Num()),
			Result.Stranded, Case.StrandsToday);
	}

	/** One wall and what it did — the unit the cache hands out and every caller reads. */
	struct FSolvedWall
	{
		FWall Wall;
		FWallResult Result;
	};

	/**
	 * EVERYTHING THE SOLVE READS OFF A CASE, AND NOTHING ELSE — which is what makes it a key.
	 *
	 * `LayWall` reads the course count, the cell count and (through `CourseGeometry`) the bond, the
	 * corbel and the projecting course; `CutPieces` reads the cut regions. It reads no other field,
	 * so two cases agreeing on these are the same wall cut the same way and CANNOT differ in their
	 * answer. The number is deliberately NOT part of the key: the height ladder in
	 * `StackBondColumnShearIsHeightIndependent` builds its own case 18 at ten courses, which is the
	 * catalogue's case 18 brick for brick, and its sixteen-course sibling differs here in the first
	 * field. Verdicts, titles and the named fall regions are NOT part of it either, because the
	 * solver never sees them — they are what the assertions compare the answer against.
	 */
	FString SolveKeyOf(const FWallCase& Case)
	{
		FString Key = FString::Printf(
			TEXT("%d|%d|%d|%d|%.17g|%d"),
			Case.Courses, Case.Cells, static_cast<int32>(Case.Bond),
			Case.CorbelFromCourse, Case.CorbelStepCm, Case.ProjectingCourse);

		for (const FWallRegion& Region : Case.Cuts)
		{
			Key += FString::Printf(
				TEXT("|%d,%d,%.17g,%.17g"),
				Region.CourseLo, Region.CourseHi, Region.CellLo, Region.CellHi);
		}

		return Key;
	}

	/**
	 * The answer for this wall, laid and cascaded ONCE however many tests ask for it.
	 *
	 * HELD BY POINTER RATHER THAN BY VALUE because a TMap moves its values when it grows, and every
	 * caller here holds a reference across the rest of its own test. It outlives the run rather than
	 * the test, which costs twenty-one walls of memory and is safe for the same reason the cache is
	 * sound at all: the answer is a pure function of the key, so a second run in the same process
	 * recomputes nothing and reads exactly what the first one would have computed.
	 */
	const FSolvedWall& SolvedWallCase(const FWallCase& Case)
	{
		static TMap<FString, TUniquePtr<FSolvedWall>> Cache;

		const FString Key = SolveKeyOf(Case);

		if (TUniquePtr<FSolvedWall>* Found = Cache.Find(Key))
		{
			return *Found->Get();
		}

		TUniquePtr<FSolvedWall> Solved = MakeUnique<FSolvedWall>();

		SolveWallCase(Case, Solved->Wall, Solved->Result);

		return *Cache.Add(Key, MoveTemp(Solved)).Get();
	}

	/** The cached answer, with this caller's own copy of the fixture preconditions run over it. */
	const FSolvedWall& RunWallCase(FAutomationTestBase& Test, const FWallCase& Case)
	{
		const FSolvedWall& Solved = SolvedWallCase(Case);

		CheckWallFixture(Test, Case, Solved.Wall, Solved.Result);

		return Solved;
	}

	/** One line per case, whether it passed or not, so the whole set reads off the log. */
	void ReportWallCase(
		FAutomationTestBase& Test,
		const FWallCase& Case,
		const FWall& Wall,
		const FWallResult& Result)
	{
		Test.AddInfo(FString::Printf(
			TEXT("case %02d %-44s expected %-10s | laid %d, cut %d, passes %d(+%d), fell %d (%d ")
			TEXT("stranded) %s, worst %.6g%s"),
			Case.Number, Case.Title, VerdictName(Case.Verdict),
			Result.PiecesLaid, Result.PiecesCut, Result.IntactPasses, Result.CutPasses,
			Result.Fallen.Num(), Result.Stranded, *DescribePieces(Wall, Result.Fallen),
			Result.Worst,
			Result.WorstPieceA == INDEX_NONE
				? TEXT("")
				: *FString::Printf(
					TEXT(" at c%d/%g-c%d/%g"),
					Wall.CourseOf[Result.WorstPieceA], Wall.CellOf[Result.WorstPieceA],
					Wall.CourseOf[Result.WorstPieceB], Wall.CellOf[Result.WorstPieceB])));
	}

	/* ================================================================================
	 * THE CATALOGUE.
	 * ================================================================================ */

	/* --- A: one brick out. ---------------------------------------------------------
	 *
	 * THIRTY COURSES, NOT THE TEN THE DRAWING SHOWS, AND THE HEIGHT IS THE WHOLE POINT.
	 * ARCHING_DESIGN.md works the half-seated joint out at 0.058203838 of capacity PER BRICK
	 * WEIGHT it carries, so a brick with nine courses over it reads 0.52 and a wall ten courses
	 * tall says "stands" whatever the model does — the row would assert nothing and pass forever.
	 * The joint reaches 1.0 at 17.18 brick weights, i.e. at eighteen courses of cover, so thirty
	 * courses with the cut in course 1 puts it firmly past the line (about 28 brick weights, which
	 * is the 1.62971 that design records). The physical claim — a real wall does not notice one
	 * brick — is height-independent; the fixture has to be tall enough to be able to disagree.
	 */
	constexpr int32 TallCourses = 30;
	constexpr int32 StandardCells = 12;

	const FWallRegion Case2Cuts[] = { { 1, 1, 5.25, 5.75 } };
	const FWallRegion Case3Cuts[] = { { 1, 1, 11.00, 11.50 } };
	const FWallRegion Case4Cuts[] = { { 0, 0, 4.75, 5.25 } };

	const FWallRegion Case5Cuts[] =
	{
		{ 1, 1, 1.25, 1.75 },
		{ 1, 1, 3.25, 3.75 },
		{ 1, 1, 5.25, 5.75 },
		{ 1, 1, 7.25, 7.75 },
		{ 1, 1, 9.25, 9.75 },
	};

	/* --- B: openings and depth of cover. ------------------------------------------- */

	constexpr int32 CoveredCourses = 12;

	const FWallRegion TwoCellOpening[] = { { 1, 3, 4.75, 6.25 } };
	const FWallRegion FourCellOpening[] = { { 1, 3, 3.75, 7.25 } };

	/* ================================================================================
	 * CASE 8, RE-RULED 2026-08-11: THE ONE COURSE OVER THE HOLE STANDS, AND THE CATALOGUE LOSES ITS
	 * LAST "NO ROOM TO ARCH" ROW WITH IT.
	 * ================================================================================
	 *
	 * THE GEOMETRY IS UNCHANGED AND IS STILL WALKED OFF THE BRICKLAYER ABOVE, the same way case 20's
	 * teeth are. Five courses, twelve cells, case 7's own four-cell cut through courses 1..3, so ONE
	 * course of cover. The opening cuts cells 4.5, 5.5 and 6.5 out of odd course 3, and an
	 * even-course brick at cell k sits on the odd course below at cells k - 0.5 and k + 0.5, so of
	 * the four bricks of course 4 standing over the hole
	 *
	 *     cell 4   over cut 4.5 and INTACT 3.5   one bed patch — a corbel
	 *     cell 5   over cut 4.5 and cut 5.5      NO bed patch at all
	 *     cell 6   over cut 5.5 and cut 6.5      NO bed patch at all
	 *     cell 7   over cut 6.5 and INTACT 7.5   one bed patch — a corbel
	 *
	 * WHAT THE ROW USED TO CLAIM. LOCAL LOSS of the two seatless middle bricks — c4/5 and c4/6, the
	 * region { 4, 4, 4.75, 6.25 } this block used to declare — on the catalogue's reading that "a
	 * single course cannot arch: it is a beam in flexure over four bricks, and mortar has 0.1 MPa to
	 * offer". (An earlier draft named all four and was corrected on case 20's reading: the two over
	 * the jambs keep one patch each, and ONE PATCH IS A CORBEL RATHER THAN A TOOTH. That correction
	 * is not what moved now.) The row was red for its whole life because THE MODEL DROPS NOTHING
	 * HERE: slice 2 took it from 2 dropped to 0 by re-seating the seatless pair through the head
	 * joints of the bricks either side.
	 *
	 * THE RULING, AND WHY IT IS THE CATALOGUE THAT MOVED. As of the 2026-08-11 LP-oracle sweep this
	 * row was the outlier of THREE independently derived methods, not of one:
	 *
	 *     catalogue   LOCAL LOSS — two bricks drop — on flexure of a single spanning course
	 *     production  drops nothing, breaks nothing; worst joint 0.218869 at c2/8-c3/7.5, which is
	 *                 in the jamb and not over the hole at all
	 *     LP oracle   STANDS at lambda* = 324.732096 (52 blocks / 101 joints, 1,289 pivots, 4.7 s
	 *                 — RigidBlockOracleSweepTest's opt-in wall sweep, measured 2026-08-11)
	 *
	 * The user ruled STANDS on 2026-08-11, per the standing instruction of DESIGN §8 (cases 11 and
	 * 12): a published gate and a prior ruling are both evidence, neither is gospel, and the physics
	 * is what decides. Worked through:
	 *
	 *     the mechanism   flexure is not the only path across a four-cell hole. The limit theorem
	 *                     needs only ONE admissible equilibrium, and the one it finds puts the flat
	 *                     course into its abutments in HEAD-JOINT COMPRESSION — a flat arch. That
	 *                     needs abutments, not cover, which is exactly the term the "one course of
	 *                     cover" reading assumed away.
	 *     the margin      lambda* = 324.73 says the fixture is nowhere near strength-governed: this
	 *                     profile's compressive capacity is 100x its tensile one (10.0 / 0.1 MPa)
	 *                     and four bricks of self weight is a rounding error against it.
	 *     the charity     even discounted by BOTH of the sweep header's slants at once — /3 for
	 *                     plastic-vs-first-crack and /6 as a characteristic-vs-mean budget — the
	 *                     verdict reads 18.0 and still stands. The /6 is a budget rather than a
	 *                     correction, and in the honest direction: the oracle ran at the CODED
	 *                     characteristic 0.10 while every §8 ruling is argued at mean 0.4-0.8, so
	 *                     reading it against the ruling basis would MULTIPLY lambda*, not divide it.
	 *     the abutment    "the jamb spreads and the flat arch lets go" is the one reading that could
	 *                     still condemn it, and the LP was asked: its joint rows are no-tension plus
	 *                     Mohr-Coulomb friction, so an abutment that could not receive the thrust
	 *                     without sliding would have shown up as a low lambda*, not a high one.
	 *
	 * WHAT THE MODEL'S AGREEMENT IS AND IS NOT WORTH. It reaches STANDS by re-seating the spanned
	 * group through its head joints, which is the same jamming action in a cruder form — the right
	 * family of mechanism, arrived at by a different route. Do NOT read it as confirmation: at this
	 * wall's height the springing carries almost no pre-compression, and as sigma_n goes to zero the
	 * Mohr-Coulomb cohesion term dominates and the thrust check goes toothless however large H/V
	 * becomes (measured 2026-08-07, TestResults/2026-08-07_0230/RESULTS.md §6.2). The weight of this
	 * verdict is carried by the LP, not by production agreeing with it.
	 *
	 * THE COST, RECORDED BECAUSE IT MUST STAY VISIBLE. Case 8 was THE CATALOGUE'S LAST "NO ROOM TO
	 * ARCH" DISCRIMINATOR — DESIGN §8's case-11 entry names it as exactly that, twice, in the
	 * sentence that let case 11 be re-ruled without losing the idea. After this ruling THERE IS NO
	 * CASE ANYWHERE IN THE SET WHERE ARCHING IS REFUSED FOR LACK OF COVER, and no row can fail if a
	 * future solver grants cover-free arching everywhere. Two consequences to hand on:
	 *
	 *     - the 7-vs-8 pair stops separating on outcome and left Acceptance.Wall.MatchedPairs; that
	 *       test has since retired entirely (2026-08-12, when the last of its pairs went the same
	 *       way) and the FILE HEADER now records where every pair's discrimination went
	 *     - a replacement discriminator, if the idea is wanted back, has to be a fixture where the
	 *       ABUTMENT cannot receive the thrust — case 10's shape, not case 8's — since it is the
	 *       abutment and not the cover that this ruling says a flat arch needs. NOTE, after
	 *       2026-08-12: case 10 itself has since been ruled to STAND, so its shape is the model for
	 *       such a fixture and not a fixture that does the job today
	 *
	 * AND IT SUPERSEDES THE STANDING DOUBT, which is the other reason this went to the user:
	 * CURRENT_STATE recorded that a bricklayer looking at a coverless course over 1.2 m would expect
	 * the WHOLE COURSE to come down rather than two named bricks. That doubt was about which bricks
	 * the LOCAL LOSS names; the ruling removes the local loss, so the doubt goes with it rather than
	 * being answered. If physical evidence ever contradicts this row it will contradict the whole
	 * verdict, not the naming — and it should come back through DESIGN §8 as a fourth ruling, not as
	 * a quiet edit to the region this block just deleted.
	 */

	/* ================================================================================
	 * CASE 9, RE-RULED 2026-08-12: THE TEN-CELL OPENING SPANS AS A DEEP BEAM, AND THE THIRD
	 * ARCHING-GATE VERDICT RETIRES WITH IT.
	 * ================================================================================
	 *
	 * THE GEOMETRY IS UNCHANGED, walked off the bricklayer above in centimetres:
	 *
	 *     the cut  { 1, 3, 1.75, 11.25 } takes cells 2..11 out of even course 2 and cells
	 *              2.5..10.5 out of odd courses 1 and 3 — 28 of the 174 laid
	 *     jambs    TOOTHED, so two faces per side: the even courses stop at 33.25 cm and resume
	 *              at 259.25, the odd courses stop at 44.50 and resume at 248.00
	 *     span     203.50 cm at its narrowest reveal and 226.00 at its widest — call it 9.5 cells
	 *     head     the top of course 3 is 29.00 cm up; eight courses of cover stand over it and
	 *              the wall's top is 89.00 cm, so 60.00 cm of masonry spans the hole
	 *
	 * WHAT THE ROW USED TO CLAIM. COLLAPSE — the region { 4, 11, 1.75, 11.25 } this block used to
	 * declare comes down, and the two jamb regions { 0, 3, -1.0, 1.75 } and { 0, 3, 11.25, 15.0 }
	 * survive — on the published arching gate: BS 5977 wants 300 mm of masonry above the APEX of a
	 * 45 degree isosceles triangle raised on the clear span, and a 2.1 m span needs 1.07 m of rise
	 * before the cover is even counted. This fixture has 0.60 m in total. The row was red for its
	 * whole life because THE MODEL DROPS NOTHING HERE.
	 *
	 * THE RULING, AND WHY IT IS THE CATALOGUE THAT MOVED. As of the 2026-08-12 LP-oracle sweep this
	 * row was the outlier of THREE independently derived methods, exactly as case 8 was:
	 *
	 *     catalogue   COLLAPSE, on the arching gate
	 *     production  STANDS: drops nothing, breaks nothing, zero cascade passes; worst joint
	 *                 0.98502040901419818 at c3/11.5-c4/11, the bed joint where the cover lands on
	 *                 the right jamb
	 *     LP oracle   STANDS at lambda* = 36.5639285 (146 blocks / 350 joints — OracleSlowSweep.
	 *                 RigidBlock.WallsAndLadders. Its 4,885 pivots in 50.4 s were MEASURED
	 *                 PRE-PARTIAL-PRICING on 2026-08-12 and the in-flight pricing slice has since
	 *                 moved them; lambda* and the block/joint counts are the measurement, the cost
	 *                 figures are history)
	 *
	 * The user ruled STANDS on 2026-08-12, per the standing instruction of DESIGN §8 (cases 11, 12
	 * and 8): a published design threshold is not a collapse predictor, and the physics decides.
	 * Worked through, and the hand check is INDEPENDENT of both other methods:
	 *
	 *     the mechanism   60 cm of bonded masonry over a 213.75 cm span is a DEEP BEAM at span over
	 *                     depth 3.6, not a triangle of loose bricks looking for somewhere to arch.
	 *                     The gate assumes the masonry above the span is dead weight until it can
	 *                     form an arch; a bonded section carries it in flexure whether or not there
	 *                     is room for the triangle.
	 *     the arithmetic  eight courses over 9.5 cells is 76 brick weights = 2027 N; simply
	 *                     supported, W*L/8 = 542 N.m; the section is t*D^2/6 = 10.25 * 60^2/6 =
	 *                     6150 cm^3; so the extreme fibre reads 0.088 MPa. That is 0.88x the
	 *                     CONSERVATIVE characteristic f_xk1 (0.10) and about 0.15x the mean basis
	 *                     (0.4-0.8) every §8 ruling is argued on. Under capacity on the strict
	 *                     reading and comfortably under it on the honest one.
	 *     the margin      lambda* = 36.56 survives both of the sweep header's charitable discounts
	 *                     applied at once — /3 for plastic-vs-first-crack and /6 as a
	 *                     characteristic-vs-mean budget — at 2.03, and the /6 runs the honest way
	 *                     (the oracle ran at the coded characteristic 0.10).
	 *     the agreement   production's 0.985 and the hand figure's 0.88 are the same answer to
	 *                     within the routing. That is the cross-check that matters: two methods
	 *                     that do not share an implementation both put this fixture just under
	 *                     capacity, and the third stands it outright.
	 *
	 * THE COST, RECORDED BECAUSE IT MUST STAY VISIBLE. This is the THIRD verdict to leave the set
	 * on the retirement of the published arching gate — case 11 on 2026-08-08, case 8 on
	 * 2026-08-11, case 9 now — and after it THE SET HAS NO CASE THAT REFUSES A SPAN FOR WANT OF
	 * RISE. Nothing here can fail if a future solver grants deep-beam action over any span at any
	 * cover. The wanted-list discriminator that would restore the idea is already recorded
	 * (CURRENT_STATE, "new acceptance cases wanted"): it has to starve the ABUTMENT, since after
	 * three rulings it is the abutment and not the cover or the rise that a spanning course needs.
	 *
	 * AND THE ROW GOES GREEN, WHICH IS WHY ITS PINS ARE DELETED RATHER THAN MOVED. The model
	 * already stands this wall, so `DropsToday` has nothing to characterise; case 9 leaves the
	 * caption test's known-disagreement list in the same edit. Unlike cases 10 and 19 below, this
	 * ruling hands the row to the model outright.
	 *
	 * ONE THING TO WATCH, RECORDED IN CURRENT_STATE AND REPEATED HERE. Production's 0.98502040901
	 * is ONE RETUNE FROM 1.0. Nothing physical separates this wall from a wall that drops its whole
	 * head, and the day that reading crosses over, this row and the sweep's `AgreeStands` relation
	 * both flip with no change in the physics. The absolute reading is pinned by
	 * Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome below so that the crossing fails loudly
	 * rather than quietly reclassifying the case.
	 */
	const FWallRegion Case9Cuts[] = { { 1, 3, 1.75, 11.25 } };

	/* ================================================================================
	 * CASE 10, RE-RULED 2026-08-12: THE FREE-END PANEL CANTILEVERS, AND PRODUCTION'S "COLLAPSE" IS
	 * AN ABSENT MECHANISM RATHER THAN A STRENGTH VERDICT.
	 * ================================================================================
	 *
	 * THE GEOMETRY. Case 7's cut, extended through to the free right end:
	 * { 1, 3, 7.75, 11.50 } takes cells 8..11 out of even course 2 and cells 8.5..11.25 (the
	 * closing half bat included) out of odd courses 1 and 3, twelve of the 150 laid. So there is a
	 * jamb on the LEFT and nothing at all on the right — the panel of cover over the opening has
	 * one support, not two, and 3.75 cells (84.4 cm) of it hangs past that support.
	 *
	 * WHAT THE ROW USED TO CLAIM. COLLAPSE of { 4, 11, 8.00, 11.50 } with { 0, 3, -1.0, 7.75 }
	 * standing — the unsupported end of the wall comes down and the jambed half survives — on the
	 * reading that an opening with no abutment cannot arch, which is true and is not the question.
	 * THE FALL REGION DIED WITH THE RULING; THE SURVIVOR REGION DID NOT, and the paragraph below
	 * headed WHAT SURVIVES THE RULING says why it is still asserted.
	 *
	 * THE RULING. As of the 2026-08-12 sweep:
	 *
	 *     catalogue   COLLAPSE — the overhanging panel comes down
	 *     production  drops 12, THREE OF THEM STRANDED, in ZERO cascade passes at a worst reading
	 *                 of 0.299975 (at c2/7-c3/7.5, in the jamb). No joint anywhere is near
	 *                 capacity; nothing broke
	 *     LP oracle   STANDS at lambda* = 35.8172298 (138 blocks / 349 joints; 2,640 pivots in
	 *                 11.5 s, MEASURED PRE-PARTIAL-PRICING 2026-08-12 — the in-flight pricing slice
	 *                 has already moved both, so treat the cost figures as history and lambda* and
	 *                 the block/joint counts as the measurement)
	 *
	 * The user ruled STANDS on 2026-08-12. What decided it is the ZERO in "zero cascade passes":
	 *
	 *     the finding     production is not saying this panel is too weak. It is saying its router
	 *                     could not find a path for those twelve pieces, because load only ever
	 *                     goes DOWN and there is nothing under them. Every one of the twelve is
	 *                     unrouted, not overloaded — the sharpest statement of DESIGN §7's missing
	 *                     mechanism anywhere in the suite, and NOT evidence about masonry.
	 *     the mechanism   84.4 cm of bonded panel 60 cm deep off one jamb is a CANTILEVER, and a
	 *                     bonded section cantilevers. This is the same composite action the
	 *                     2026-08-06 free-end ruling credited when it ruled that one brick out at a
	 *                     free end must not bring a wall down (case 3) — at 3.75 cells instead of
	 *                     one, which is the only thing that is new.
	 *     the arithmetic  8 courses x 3.75 cells is 30 brick weights = 800 N, acting at a 42.2 cm
	 *                     lever, so M = 337 N.m; over the same t*D^2/6 = 6150 cm^3 that is
	 *                     0.055 MPa — 0.55x the characteristic f_xk1 (0.10) and 0.14x of f_xk2
	 *                     (0.40). And it is charitable to the COLLAPSE reading twice over: the
	 *                     crack plane it assumes is a straight vertical cut, where a real one is a
	 *                     toothed staircase picking up bed-joint cohesion the plane cannot see.
	 *     the margin      35.82 discounted by /3 and /6 together is 1.99, still standing.
	 *
	 * WHAT THIS ROW BECOMES, AND IT IS A NEW SHAPE FOR THE CATALOGUE: a STANDS row THE MODEL STILL
	 * DISAGREES WITH. Every previous STANDS verdict here was one the model produced. This one is
	 * expected to stand and measured to drop twelve, so it stays on the known-red list, stays on
	 * the caption's disagreement marker, and keeps `DropsToday = 12` / `StrandsToday = 3` — the
	 * pins are what stop the failure drifting inside its own red. The row goes green at DESIGN §7's
	 * evolution step 4, when equilibrium becomes the cascade's authority and load can route
	 * sideways.
	 *
	 * WHAT SURVIVES THE RULING: THE SURVIVOR REGION, AND IT IS RESTORED VERBATIM. A STANDS verdict
	 * says nothing comes down anywhere, so { 0, 3, -1.0, 7.75 } — the jambed left half below the
	 * head, which the old COLLAPSE row named as the masonry that must keep its footing — is still a
	 * TRUE claim and a strictly stronger one than "twelve fell". The three counts this row pins are
	 * blind to WHICH twelve: a routing change that dropped different pieces while dropping twelve
	 * of them would satisfy `DropsToday`, `StrandsToday` and the STANDS failure line all three,
	 * and only the region names the identity. It costs nothing to keep and it is asserted in the
	 * INVERTED shape — no piece inside it may be among the fallen — so it characterises WHERE
	 * today's wrongness is NOT. Verified against the measured drop set: all twelve sit in courses
	 * 4..9 at cells 9.0 and above, so the region is clear of every one of them and clear of the cut.
	 *
	 * THE COST. The 7-vs-10 abutment pair stops separating on outcome and leaves with the retired
	 * `Acceptance.Wall.MatchedPairs` (the file header lists all five pairs and where each went).
	 * It was DELIBERATELY NOT relocated onto a production reading — case 10's 0.300 against case
	 * 7's 0.269 is a real number measured on a wall the router could not route, and pinning it
	 * would claim an abutment term the model demonstrably does not have. Its home is the LP:
	 * 296.22 against 35.82, 8.27x, in the physically right direction.
	 */
	const FWallRegion Case10Cuts[] = { { 1, 3, 7.75, 11.50 } };
	const FWallRegion Case10Stands[] = { { 0, 3, -1.0, 7.75 } };

	/* --- C: spanning between supports. --------------------------------------------- */

	/* ================================================================================
	 * CASE 11, RULED TWICE ON 2026-08-08: A PUBLISHED DESIGN GATE SAID LOCAL LOSS, THE PHYSICS SAYS
	 * STANDS, AND THE PHYSICS IS THE KEEPER.
	 * ================================================================================
	 *
	 * THE ROW WAS DRAFTED "STANDS", RULED "LOCAL LOSS", AND THEN RE-RULED "STANDS" THE SAME DAY, and
	 * both rulings are recorded because which one is right is the whole content of this row.
	 *
	 * THE FIRST RULING RESTED ON THE BS 5977 ARCHING GATE — an opening arches if it has an
	 * overlapping bond AND at least 300 mm of masonry above the APEX of a 45 degree isosceles
	 * triangle raised on the clear span — and claude_plans/PROJECT_REVIEW.md §3 checked all twenty
	 * rows against it. This fixture fails it by its whole height. THE SECOND RULING IS THAT A
	 * PUBLISHED DESIGN THRESHOLD IS NOT A COLLAPSE PREDICTOR AND MUST NOT BE TREATED AS ONE: the
	 * 300 mm rule is a never-even-crack serviceability line carrying design safety factors, and the
	 * question this catalogue asks is what a real wall DOES, which has to be answered by working the
	 * physics rather than by quoting a code. Worked honestly, the wall holds — see below.
	 *
	 * THE GEOMETRY IS KEPT BECAUSE IT IS THE INPUT TO BOTH READINGS, WALKED OFF THE BRICKLAYER ABOVE
	 * RATHER THAN OFF THE DRAWING, in centimetres:
	 *
	 *     the cut  { 0, 3, 2.75, 8.25 } takes cells 3..8 out of the even courses 0 and 2 and cells
	 *              3.5..7.5 out of the odd courses 1 and 3 — six bricks and five bricks a course,
	 *              22 of the 150 laid
	 *     jambs    the reveal is TOOTHED, so it is two faces and not one: the even courses stop at
	 *              55.75 cm and resume at 191.75, the odd courses stop at 67.00 and resume at 180.50
	 *     span     113.50 cm at its narrowest and 136.00 cm at its widest — a 1.2-1.4 m opening
	 *     head     the top of course 3 is 29.00 cm up; eight courses of cover stand over it and the
	 *              wall's top is 89.00 cm, so there is 60.00 cm of masonry above the opening
	 *     apex     half the span above the head: 56.75 cm at the narrowest reveal, 68.00 at the
	 *              widest, i.e. 85.75 cm to 97.00 cm above the ground
	 *     piers    three cells of bearing each — 66.5 cm of solid masonry carrying eight courses
	 *
	 * So there is between 32 mm and nothing at all above that apex, against the 300 mm the gate
	 * wants, and at the wide reveal the apex stands 8 cm clear above the top of the wall. THAT IS
	 * WHAT THE FIRST RULING READ, AND IT IS TRUE AS FAR AS IT GOES: no thrust line fits above the
	 * triangle, so this panel is not held up by an ARCH.
	 *
	 * IT IS HELD UP BY BEING A DEEP BEAM, WHICH IS THE READING THE GATE NEVER ASKS ABOUT. The 60 cm
	 * of bonded masonry standing over the opening spans 136 cm at worst, so span/depth is 2.3 —
	 * squarely deep-beam territory — and the load it carries is its own weight, the same 44 bricks
	 * the discarded fall region named:
	 *
	 *     W   44 x 2.72163125 kg = 119.75 kg, x 980 = 1.1735e5 Unreal force units
	 *     M   W L / 8 = 1.1735e5 x 136 / 8 = 1.995e6 uu.cm at midspan
	 *     Z   t D^2 / 6 = 10.25 x 60^2 / 6 = 6150 cm3
	 *     f   M / Z = 324 uu/cm2, i.e. 0.0324 MPa — call it 0.03 to 0.04 depending on which reveal
	 *         governs and on how much surcharge beyond the span window is counted in
	 *
	 * AGAINST WHAT. PROJECT_REVIEW.md §1 records the user's decision to move the profiles to MEAN
	 * bond strength, 0.4 to 0.8 MPa, where 0.03-0.04 is FIVE TO TEN PER CENT of capacity. Even
	 * against the conservative characteristic 0.10 MPa the profiles carry today it is under a third,
	 * and the model's own reading agrees: its worst joint in this wall is c3/8.5-c4/8 at 0.362193 of
	 * f_xk1 — the toothed corner of the reveal, not the midspan — which is the same "comfortable"
	 * answer from a completely different route. Real half-brick walls bridge 1.2 m of missing
	 * masonry routinely; a rule that forbids it is a rule about cracking, not about falling.
	 *
	 * HENCE STANDS, WITH NO FALL REGION AND NO SURVIVOR REGION, because nothing comes down. Paired
	 * against case 12 this row isolates PIER WIDTH — three cells of bearing against one — and on
	 * 2026-08-09 the same honest-physics rule reached case 12 itself: the one-cell pier ALSO holds
	 * (the arithmetic is above Case12Cuts), so the pair separates in the MARGIN rather than the
	 * outcome, exactly as 13/14 and 15/16 came to.
	 *
	 * AND THE CLAUSE THAT USED TO END THIS BLOCK IS NOW FALSE, WHICH IS WORTH SAYING RATHER THAN
	 * DELETING. It read: "the genuine 'no room to arch' discriminator in this set remains CASE 8,
	 * one course of cover over a four-cell hole, which stays red and stays local loss." That was the
	 * consolation for re-ruling this row, and DESIGN §8 recorded it in the same words. On 2026-08-11
	 * the same standing instruction reached case 8 too — the LP oracle stands the coverless course
	 * at lambda* = 324.73 through head-joint compression into its abutments — and the user re-ruled
	 * it STANDS. SO THE SET NOW HAS NO CASE AT ALL WHERE ARCHING IS REFUSED FOR LACK OF COVER, and
	 * the honest reading of these three rulings together is that COVER WAS NEVER THE GATE: what a
	 * flat or deep arch needs is an ABUTMENT that can receive the thrust, which is case 10's
	 * variable and the one row of the family still red.
	 */
	const FWallRegion Case11Cuts[] = { { 0, 3, 2.75, 8.25 } };

	/* ================================================================================
	 * CASE 12, REWRITTEN 2026-08-09: THE SAME SPAN ON A ONE-CELL PIER — AND, WORKED HONESTLY, THE
	 * PIER HOLDS.
	 * ================================================================================
	 *
	 * WHAT THE OLD ROW WAS AND WHY IT WENT. As encoded until today, case 12 cut ten cells out of
	 * twelve — { 0, 3, 0.75, 10.75 } — which varied SPAN AND PIER AT ONCE against case 11 and
	 * near-duplicated case 9's ten-cell collapse. The comment that stood here claimed the row
	 * "passes today", which had stopped being true: it was one of the six known reds, pinned at 76
	 * dropped and 11 stranded, and it named no survivors, so a model that always answered "falls"
	 * would have passed it. The 2026-08-08 review approved rewriting it as case 11's own span on a
	 * narrow pier, so that pier width is the one variable between the halves of the pair.
	 *
	 * THE NEW GEOMETRY, WALKED OFF THE BRICKLAYER ABOVE RATHER THAN OFF A DRAWING. The wall is
	 * case 11's wall — twelve courses, twelve cells — and the cut is case 11's cut shifted to the
	 * wall's left end:
	 *
	 *     the cut  { 0, 3, 0.75, 6.25 } takes cells 1..6 out of even courses 0 and 2 and cells
	 *              1.5..5.5 out of odd courses 1 and 3 — six and five bricks a course, 22 of the
	 *              150 laid, exactly case 11's count
	 *     pier     what is left at the left end: the cell-0 brick on even courses, the 9.25 cm
	 *              closing piece and the cell-0.5 brick on odd — a bonded running-bond column
	 *              21.5 to 32.75 cm along the wall, four courses (29 cm) up to the springing
	 *     abutment the right side keeps five cells, 111.5 cm of solid masonry — case 11's pier
	 *              grown wider, so any failure here is attributable to the narrow side alone
	 *     span     the reveals are the same toothed 113.5 to 136.0 cm as case 11, under the same
	 *              60 cm of bonded cover
	 *
	 * WHY ONE NARROW PIER RATHER THAN TWO. The approved sketch said "one-cell piers, with a
	 * survivor region", and those two wishes cannot both be had symmetrically: two one-cell piers
	 * bound an eight-cell wall, in which a pier failure leaves NOTHING standing to name — old case
	 * 12's exact weakness. Narrowing ONE pier keeps the wall, the span and the cover bit-identical
	 * to case 11 and leaves the wide side able to survive anything the narrow side does.
	 *
	 * THE PANEL IS NOT THE QUESTION — case 11 settled it on identical numbers: span/depth 2.3, a
	 * ~44-brick panel (119.75 kg, 1.174e5 uu) at M/Z ≈ 0.033 MPa of bending, a twentieth of mean
	 * bond strength. THE PIER IS THE QUESTION. The recorded expectation (DESIGN §8, made in
	 * passing inside case 11's second ruling) was that the thrust shoves it over; worked honestly,
	 * per the standing rule that neither a published gate nor the model is gospel:
	 *
	 *     H       the panel places its thrust line where it likes inside its own 60 cm depth, so
	 *             the honest demand on an abutment is the MINIMUM thrust, at r ≈ 50 cm of rise:
	 *             H = W·L/(8r) = 1174 N x 1.36 m / (8 x 0.50 m) ≈ 400 N — a third of the panel's
	 *             own weight. (The solver's kern-limited r = d_e/3 = 20 cm reads H = 998 N; that
	 *             is an uncracked-serviceability construction, deliberately 2.5x harsher than a
	 *             collapse prediction — DESIGN §5.4 says so itself.)
	 *     hinge   course 0 is ground, so the candidate hinge is the course-0/1 bed joint, ~22 cm
	 *             below the springing: overturning demand ≈ 400 N x 0.22 m = 88 N·m (kern: 220).
	 *     rigid   restoring is N x b/2. The pier's share of the ~95-brick upper wall plus its own
	 *             courses is 550 to 1,050 N by where the abutment reaction lands, over b/2 =
	 *             10.75 cm: 59 to 113 N·m. A RIGID stack is at the line — 0.7 to 1.3.
	 *     bonded  the joint's own modulus is 10.25 x 21.5^2 / 6 = 790 cm3, so mean flexural bond
	 *             — 0.4 to 0.8 MPa, the basis adopted 2026-08-08 — adds 316 to 632 N·m of
	 *             capacity: FOUR TO EIGHT TIMES the honest demand, and still 1.7 to 3.4x against
	 *             even the kern-limited thrust. Sliding never governs: cohesion alone is 0.2 MPa
	 *             over ~220 cm2 = 4,400 N, ten times H.
	 *
	 * SO THE ONLY READINGS THAT CONDEMN THIS PIER ARE THE RIGID BLOCK AND THE
	 * CHARACTERISTIC-STRENGTH-AGAINST-KERN-THRUST STACK — the never-even-crack design stack. The
	 * rigid-block reading is the one this catalogue has already rejected three times (cases 14 and
	 * 16 and the free-end ruling): an uncracked bonded section carries what a rigid block cannot.
	 * A realistic scenario agrees — a 1.2-1.4 m garden-wall opening on a single 215 mm jamb under
	 * 60 cm of bonded brickwork is common construction and it stands, cracked at worst. VERDICT:
	 * STANDS, with no fall region and no survivor region, because nothing comes down. The
	 * "survivor region" of the approved sketch presupposed a collapse the arithmetic does not
	 * support; what it was for — a collapse row that cannot be passed by a model that always says
	 * "falls" — is moot on a row that expects nothing to fall.
	 *
	 * THE MODEL AGREES, AND FOR A REASON WORTH PINNING IN PROSE: measured 2026-08-09, it drops 0,
	 * breaks 0, strands 0, and its worst joint reads 0.362067 at the pier-side springing
	 * c3/0.5-c4/1 — within 0.03% of case 11's 0.362193, because both are the same half-seat
	 * eccentricity at a reveal corner. The solver carries the thrust as springing SHEAR only and
	 * never walks it down the pier as a moment (DESIGN §7 item 6), so it reads NO pier-width term
	 * at all: the margin separation between these two rows exists in the arithmetic above and in
	 * no readout, which is why the pair has no reading test to move onto and the discrimination
	 * waits on the leaning stack and the LP oracle.
	 *
	 * WHAT THE SET GIVES UP, SAID PLAINLY: at self-weight there is no honest fixture in this
	 * family where a pier fails while its span survives — the thrust scales with the panel's own
	 * weight while the pier's capacity scales with the wall standing on it. The pier-overturning
	 * discriminator DESIGN §7 items 1 and 6 want needs either a surcharge (no external-force
	 * channel exists yet) or no thrust at all — the leaning-stack case, queue item 2, which is the
	 * overturning guard's red test. The pair against case 11 accordingly separates IN THE MARGIN —
	 * the one-cell pier runs a few times from failure on the mean basis where every member of case
	 * 11 sits an order of magnitude clear — rather than in the outcome, and the MatchedPairs row
	 * that asserted the outcome half was removed with the same dating. The LP oracle has since paid
	 * that debt: 128.12 on three cells of bearing against 89.12 on one, 1.44x in the physically
	 * right direction, pinned in OracleSlowSweep.RigidBlock.WallsAndLadders.
	 */
	const FWallRegion Case12Cuts[] = { { 0, 3, 0.75, 6.25 } };

	/* --- D: corbelling and the projecting header. ----------------------------------- */

	constexpr int32 CorbelCells = 8;
	constexpr int32 CorbelFirstCourse = 6;
	constexpr double QuarterBrickStepCm = HalfCellCm * 0.5;
	constexpr double HalfBrickStepCm = HalfCellCm;

	/* ================================================================================
	 * CASE 14, RE-DERIVED: A BONDED FOUR-STEP CORBEL STANDS AT 0.19516 AND NOTHING COMES DOWN.
	 * ================================================================================
	 *
	 * THE VERDICT ON THIS ROW WAS COLLAPSE UNTIL 2026-08-07 AND THE USER RULED IT WRONG. The
	 * catalogue's reasoning was a RIGID-BODY OVERTURNING reading — "the resultant walks outside the
	 * bed below" — and that reading is geometrically true here and stated below because it is worth
	 * knowing which claim was abandoned. What overrules it is that this project models an UNCRACKED
	 * BONDED SECTION, and the same user had already ruled, on 2026-08-06, that a brick deleted at a
	 * free end must not bring a wall down. Any rule local enough to honour that also honours this
	 * corbel: `Core.Structure.ACorbelResistsWithItsWholeDepth` asserts a FIVE-step raking corbel
	 * standing at 0.219, and a five-step corbel and a four-step one are the same fixture with a
	 * different provenance — one cut that way, one laid that way. A threshold sited between them
	 * would be a number tuned to make two contradictory statements both come out true.
	 *
	 * WHAT THE FIXTURE ACTUALLY PRESENTS, WALKED OFF THE BRICKLAYER ABOVE RATHER THAN ASSUMED.
	 * CourseGeometry pushes each course from 6 upward one more half cell (11.25 cm) out and closes
	 * it with a FULL brick, so the end brick of course c spans
	 *
	 *     course 6   158.00 .. 179.50      course 8   180.50 .. 202.00
	 *     course 7   169.25 .. 190.75      course 9   191.75 .. 213.25
	 *
	 * and the end brick of the flush course 5 below is the odd course's HALF BAT at 158.00..168.25.
	 * Every one of the four therefore overlaps exactly ONE piece below it, over exactly 10.25 cm —
	 * the same square half seat the raking staircase corbel presents — with its centre of mass
	 * 5.625 cm outboard of that patch's centroid. THAT is what makes this the staircase's fixture:
	 * four steps of it instead of eleven. (It is also the overturning claim: the mass acts 5.625 cm
	 * out on a patch only 5.125 cm wide, so the resultant IS outside the bearing. Bond carries it.)
	 *
	 * THE LADDER, AND IT IS THE STAIRCASE LADDER BECAUSE IT IS THE STAIRCASE TOPOLOGY. Each step
	 * takes its own weight, ALL of the step above (which has nowhere else to go) and half of the
	 * next brick along, whose own share grows the same way; the moment carries across the 11.25 cm
	 * the corbel has stepped out. Written for s steps below the top and asserted, not imported.
	 *
	 * AND THE SECTION IS THE DEPTH OF BONDED MASONRY STANDING OVER THE JOINT, W = t*D^2/6, taken at
	 * the LESSER of that and the bed patch's own 179.4817708 cm3 — the top step has one course over
	 * it, D = 7.5 cm, W = 96.09 cm3, SHALLOWER than the patch, so the patch governs there and the
	 * step keeps the 0.058203838191552663 that `AdoptedWallLoadsItsWaistEccentrically` pins:
	 *
	 *     course   s   F (weights)   M (weight.cm)   courses over   reads
	 *        9     0        1             5.625            1        0.058203838   (patch governs)
	 *        8     1        2.5          22.5              2        0.156128700
	 *        7     2        4.5          56.25             3        0.173476333
	 *        6     3        7           112.5              4        0.195160875   <- the worst
	 *
	 * 0.195 OF f_xk1 IS A CORBEL STANDING AT A FIFTH OF WHAT HOLDS IT, so the verdict follows the
	 * number rather than the other way round: STANDS, with no fall region and no survivor region to
	 * name, because nothing comes down. The arithmetic is asserted in its own test below rather
	 * than left as a comment, since a verdict of "stands" is satisfied by 0.195 and by 0.0001 alike
	 * and only one of them is this fixture.
	 *
	 * CROSS-CHECKED AGAINST TWO FIGURES THIS FILE DID NOT PRODUCE. The same four lines of
	 * arithmetic give 0.21858 for a five-step corbel and 0.36903147272727271 for the eleven-step
	 * staircase — ARCHING_DESIGN.md's published 0.219 and the anchor
	 * `Core.Structure.AStaircaseVoidCondemnsTheCorbel` pins to seventeen digits. Both are asserted
	 * below as loose cross-checks, so a derivation that drifted fails HERE rather than agreeing
	 * with itself.
	 */

	/** What one step of a raking corbel carries, in brick weights, s steps below the top. */
	constexpr double CorbelLadderForceBrickWeights(int32 StepsBelowTop)
	{
		return 1.0 + StepsBelowTop + StepsBelowTop * (StepsBelowTop + 1) / 4.0;
	}

	/** What bends it, in brick-weight-centimetres: its own 5.625 cm arm plus everything above. */
	constexpr double CorbelLadderMomentBrickWeightCm(int32 StepsBelowTop)
	{
		const double S = StepsBelowTop;

		const double SumOfForces = S + S * (S - 1.0) / 2.0 + (S - 1.0) * S * (S + 1.0) / 12.0;

		return (HalfCellCm * 0.5) * (S + 1.0) + HalfCellCm * SumOfForces;
	}

	/** The square half seat a corbelled end brick keeps, and its own section modulus, cm2 and cm3. */
	constexpr double HalfSeatAreaSqCm = BrickDepthCm * BrickDepthCm;

	constexpr double HalfSeatModulusCm3 =
		(4.0 / 3.0) * (BrickDepthCm * 0.5) * (BrickDepthCm * 0.5) * (BrickDepthCm * 0.5);

	/** Bending on that patch less the compression closing it; never negative. */
	double HalfSeatTensionMPa(double MomentBrickWeightCm, double ForceBrickWeights)
	{
		const double BendingMPa = MomentBrickWeightCm * FullBrickWeightUu
			/ (HalfSeatModulusCm3 * ForceUnitsPerMPaSqCmHere);

		const double NormalMPa = ForceBrickWeights * FullBrickWeightUu
			/ (HalfSeatAreaSqCm * ForceUnitsPerMPaSqCmHere);

		return FMath::Max(0.0, BendingMPa - NormalMPa);
	}

	/** Pure bending on the deep-beam section: W = t * D^2 / 6 through the masonry standing over it. */
	double CompositeTensionMPa(double MomentBrickWeightCm, int32 CoursesOfDepth)
	{
		const double DepthCm = CoursesOfDepth * CoursePitchCm;

		const double ModulusCm3 = BrickDepthCm * DepthCm * DepthCm / 6.0;

		return MomentBrickWeightCm * FullBrickWeightUu / (ModulusCm3 * ForceUnitsPerMPaSqCmHere);
	}

	/**
	 * What the bottom rung of a k-step bonded corbel reads, as a fraction of f_xk1.
	 *
	 * The bottom rung is k - 1 steps below the top and has k courses of masonry over its bed joint,
	 * so k is the only argument. The `min` is what keeps the model nested: composite action is an
	 * ALTERNATIVE way of carrying the moment rather than an extra one, so it may only ever help.
	 */
	double CorbelBottomRungUtilisation(int32 Steps)
	{
		const double MomentBrickWeightCm = CorbelLadderMomentBrickWeightCm(Steps - 1);
		const double ForceBrickWeights = CorbelLadderForceBrickWeights(Steps - 1);

		return FMath::Min(
			HalfSeatTensionMPa(MomentBrickWeightCm, ForceBrickWeights),
			CompositeTensionMPa(MomentBrickWeightCm, Steps)) / MortarFlexuralBondMPa;
	}

	/* ================================================================================
	 * CASE 16, RE-DERIVED: A BONDED HEADER WITH NOTHING ON IT STANDS AT 0.058204.
	 * ================================================================================
	 *
	 * THE VERDICT ON THIS ROW WAS LOCAL LOSS UNTIL 2026-08-07 AND IT WAS REVISED IN THE CATALOGUE
	 * WITHOUT THIS FILE FOLLOWING. That is the drift the catalogue exists to prevent, and it is
	 * corrected here by re-deriving the number rather than by flipping the word.
	 *
	 * The header projects half a cell (11.25 cm) past the face, so of its 21.5 cm it keeps
	 * 21.5 - 11.25 = 10.25 cm of bearing on the brick below, and its centre of mass sits half the
	 * unseated length — 5.625 cm — outboard of that patch's centroid. On a 10.25 x 10.25 cm patch,
	 * whose section modulus about the bending axis is 10.25 x 10.25^2 / 6 = 179.4817708 cm3:
	 *
	 *     2667.198625 x 5.625 / 179.4817708 = 0.0083591 MPa   bending, opening the outer edge
	 *     2667.198625 / 105.0625            = 0.0025387 MPa   its own weight, closing it
	 *     tension 0.0058204 / 0.1 (f_xk1)   = 0.058204        of flexural bond capacity
	 *
	 * — a brick standing at six percent of what holds it, not a brick falling off. The abandoned
	 * "local loss" was a RIGID-BODY OVERTURNING reading (the resultant is 5.625 cm out on a patch
	 * only 5.125 cm wide, so it does lie outside the bearing) and it is the same error case 14 was
	 * corrected for on the same day: this project models an UNCRACKED BONDED SECTION, where cured
	 * mortar carries exactly that. The overturning claim is written down here because it is worth
	 * knowing which reading was abandoned, not because it is still live.
	 *
	 * SO THERE IS NO FALL REGION TO NAME, and the row carries no MustFall and no MustStand.
	 *
	 * AND THE PAIR HAD TO MOVE WITH IT. 15 and 16 differ ONLY in what sits on the header's tail,
	 * and both now stand — so as an OUTCOME pair they discriminate nothing, exactly as 13 and 14
	 * stopped doing. WHAT STILL SEPARATES THEM IS THE READING ON ONE JOINT: the model reads
	 * 0.00184 for case 15 against 0.05820 for case 16, a factor of 32 from superimposed load alone,
	 * with case 15's tension driven to zero and compression left governing. That is asserted in
	 * Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome below, and the row was removed
	 * from the outcome-pair test rather than left there unsatisfiable. That test —
	 * Acceptance.Wall.MatchedPairs — has since retired entirely, on 2026-08-12, when the last two of
	 * its five pairs lost their separation the same way; see the file header.
	 */

	/* --- E: bond pattern and head-joint shear. --------------------------------------- */

	const FWallRegion Case18Cuts[] = { { 5, 5, 4.75, 5.25 } };

	/* --- F: losing the base, and the staircase void. --------------------------------- */

	/* ================================================================================
	 * CASE 19, RE-RULED 2026-08-12: THE UNDERPINNED HALF CANTILEVERS. THE CLOSEST CALL OF THE
	 * THREE, AND RULED STANDS KNOWINGLY.
	 * ================================================================================
	 *
	 * THE GEOMETRY, AND IT IS SIX WHOLE CELLS RATHER THAN THE 5.75 AN EARLIER DRAFT OF THIS BLOCK
	 * CLAIMED. { 0, 0, -0.50, 5.25 } cuts course 0, which is an EVEN course of twelve WHOLE bricks
	 * — no half bat, since the fixture lays right to left and the short closing piece falls on the
	 * ODD courses — so the region's open bounds take cells 0, 1, 2, 3, 4 and 5, six whole bricks,
	 * and the run's own report prints `cut 6`. That is 6.0 x 22.5 = 135 cm of footing gone from
	 * under a ten-course, twelve-cell wall, with nine courses standing over the void. The old
	 * "5.75 cells (129 cm)" mixed a piece COUNT into a cell SPAN and read 4% light on the lever as
	 * well as on the load; every figure below is re-derived from 6.0.
	 *
	 * WHAT THE ROW USED TO CLAIM. COLLAPSE of { 1, 9, -1.00, 4.60 } with { 0, 9, 7.75, 13.0 }
	 * standing. The survivor region reached two and a half cells clear of the cut ON PURPOSE: a
	 * failure there would have been the 33.69 degree spreading front walking further across the
	 * wall than the missing support can account for, which is a defect and not a boundary quibble.
	 * THE FALL REGION WENT WITH THE RULING AND THE SURVIVOR REGION DID NOT — see WHAT SURVIVES THE
	 * RULING below, which is where that reasoning is now cashed rather than merely remembered.
	 *
	 * THE RULING. As of the 2026-08-12 sweep:
	 *
	 *     catalogue   COLLAPSE — the masonry over the void has no path to the earth
	 *     production  drops 34, SIX OF THEM STRANDED, in ZERO cascade passes at a worst reading of
	 *                 0.31804 (at c0/6-c1/5.5). As with case 10: nothing broke
	 *     LP oracle   STANDS at lambda* = 12.3824832 — the LOWEST of the fifteen walls measured,
	 *                 and still above 1 (119 blocks / 308 joints; 2,970 pivots in 16.1 s, MEASURED
	 *                 PRE-PARTIAL-PRICING 2026-08-12 and since moved by the in-flight pricing
	 *                 slice — lambda* and the block/joint counts are the measurement, the cost
	 *                 figures are history)
	 *
	 * THE USER RULED STANDS ON 2026-08-12 AND THE RULING IS RECORDED AS THE CLOSEST CALL IN THE
	 * SET, because the hand check STRADDLES the verdict rather than settling it:
	 *
	 *     the arithmetic  nine courses over 6.0 cells is 9 x 6 = 54 brick weights = 1440 N at a
	 *                     67.5 cm lever (half of the 135 cm of missing footing), so M = 972 N.m;
	 *                     the section is t*D^2/6 = 10.25 * 67.5^2/6 = 7784 cm^3, giving 0.125 MPa.
	 *     and it lands    1.25x the characteristic f_xk1 (0.10) — OVER capacity
	 *                     0.31x of f_xk2 (0.40) — the strength a toothed vertical crack path
	 *                     actually costs, and comfortably UNDER it
	 *                     about 0.21x the mean basis (0.4-0.8) the §8 rulings are argued on
	 *                     So one published number condemns it and two acquit it. This is a
	 *                     judgement, not an arithmetic result, and it is written down as one. THE
	 *                     CORRECTION FROM 5.75 CELLS TO 6.0 MOVED EVERY FIGURE AND NO CONCLUSION:
	 *                     0.115 became 0.125 MPa and the straddle is 9% wider on the condemning
	 *                     side, still one published number against two.
	 *     the caveat      IT IS AN END CANTILEVER, not a span between two supports. A cantilever
	 *                     has no second support to redistribute to, so of the three fixtures ruled
	 *                     on 2026-08-12 this is the one where being wrong is most plausible.
	 *     the anchor      real practice underpins in bays of roughly a metre and expects the wall
	 *                     above to bridge them. 135 cm is longer than that and the ruling knows it;
	 *                     what it rests on is that the wall is BONDED over nine courses, which a
	 *                     bay of underpinning also relies on.
	 *     the finding     and the same ZERO that decided case 10 applies here: production reaches
	 *                     34 dropped without breaking a single joint. Its verdict is unroutability,
	 *                     not strength, so it is not a third opinion — it is the same missing
	 *                     mechanism reported twice.
	 *
	 * IF PHYSICAL EVIDENCE EVER CONTRADICTS THIS ROW it will contradict the whole verdict, and it
	 * should come back through DESIGN §8 as a further ruling rather than as a quiet edit restoring
	 * the fall region this block deleted.
	 *
	 * WHAT SURVIVES THE RULING: THE SURVIVOR REGION, RESTORED VERBATIM AS { 0, 9, 7.75, 13.0 }, AND
	 * ON THIS ROW IT IS THE SHARPEST ASSERTION THERE IS. A STANDS verdict says nothing comes down
	 * anywhere, so naming the still-footed RIGHT half is a true claim and a strictly stronger one
	 * than "thirty-four fell". The reason it matters here more than anywhere else is the shape of
	 * the wrong answer: production's 34 are the spreading front walking left-to-right out of the
	 * missing footing, and A ROUTING CHANGE THAT SHIFTED THAT FRONT THREE CELLS RIGHT — dropping
	 * pieces in the still-footed half while sparing an equal number at the far left — WOULD KEEP
	 * `DropsToday = 34` AND `StrandsToday = 6` AND EVERY OTHER PIN ON THIS ROW. Counts cannot see
	 * a translation; the region can. It is asserted in the INVERTED shape — no piece inside it may
	 * be among the fallen — so what it records is where today's wrongness is NOT, which is exactly
	 * the old two-and-a-half-cells-clear reasoning surviving its verdict. Verified against the
	 * measured drop set: all 34 sit at cell 4.5 and below, so the region is clear of every one of
	 * them, and it is clear of the cut as well.
	 *
	 * THE ROW STAYS RED, IN THE INVERTED DIRECTION — expected STANDS, measured 34 dropped. Like
	 * case 10 it keeps its pins (`DropsToday = 34`, `StrandsToday = 6`) and its caption marker, and
	 * it goes green at evolution step 4.
	 */
	const FWallRegion Case19Cuts[] = { { 0, 0, -0.50, 5.25 } };
	const FWallRegion Case19Stands[] = { { 0, 9, 7.75, 13.0 } };

	/*
	 * CASE 20 — THE STAIRCASE VOID, and it is the one case in the set that was drafted with a
	 * question mark. The user ruled on 2026-08-06: LOCAL LOSS, the loose toothed bricks at the cut
	 * edge drop and the mass of the wall stands.
	 *
	 * THE RAKING CUT, one region per course, reading up. Each course above the last is cut one
	 * cell less far to the right, so the surviving masonry to the RIGHT of the void steps left
	 * over the hole as it rises — that is the overhang in the screenshot.
	 *
	 * WHICH BRICKS ARE THE TEETH, WORKED OUT FROM THE GEOMETRY RATHER THAN FROM THE DRAWING. A
	 * brick in an even course at cell k sits on the odd course below at cells k - 0.5 and k + 0.5;
	 * an odd-course brick at cell k + 0.5 sits on cells k and k + 1. Walking the surviving pieces
	 * against the cuts, exactly TWO of them are left with NO bed patch at all:
	 *
	 *     course 3, cell 4.5   sits over cut cells 4 and 5
	 *     course 5, cell 2.5   sits over cut cells 2 and 3
	 *
	 * Everything else along the cut face keeps at least one patch — course 2 cell 6, course 4
	 * cell 4 and course 6 cell 2 are all half seated, which is a corbel and not a tooth. Those two
	 * are the named set, and naming them by IDENTITY rather than by count is the point: a rule
	 * that dropped the corbelled half-seats instead would fall the same NUMBER of bricks and be
	 * completely wrong.
	 *
	 * THE FALL SET DRAWN IN WALL_CASES.html IS NOT USABLE and this deliberately does not
	 * transcribe it: its region overlaps its own cut regions almost entirely, so what it renders
	 * is one brick in course 7 that nothing in the prose claims. The prose is what was agreed.
	 *
	 * RE-EXAMINED AND CONFIRMED 2026-08-12 — A RULING THAT MOVED NOTHING, which is worth recording
	 * precisely because the other three rows examined beside it all moved. Case 20 went to the user
	 * with cases 9, 10 and 19 after the LP-oracle sweep, and it came back UNCHANGED: verdict, named
	 * teeth, `DropsToday = 9` and the standing doubt all stay exactly as they are. What the
	 * measurement said, and why none of it disturbed the row:
	 *
	 *     production  UNLIKE cases 10 and 19 this one IS a strength verdict: one cascade pass and a
	 *                 worst reading of 42.71, a joint genuinely forty-two times over capacity. The
	 *                 raking cut leaves teeth hanging where those two rows merely leave pieces
	 *                 unrouted, so nothing here is an absent-mechanism artefact.
	 *                 42.71 IS THE PRE-CASCADE PEAK and this row's own report prints something
	 *                 else, so the two must not be reconciled by guesswork: the sweep reads every
	 *                 joint after `SolveLoads` and BEFORE `SolveAndBreak`, which is where a joint
	 *                 42x over capacity is still in the graph; `ReportWallCase` below reads
	 *                 `Result.Worst` AFTER the cascade, by which time that joint has given and been
	 *                 removed, and the survivors' worst is 0.296506. One structure, two instants —
	 *                 not two measurements of the same thing
	 *     LP oracle   lambda* = 82.629597 — but a GLOBAL load factor has no local vocabulary at
	 *                 all. It says every block including both teeth has SOME admissible
	 *                 equilibrium; it cannot say whether nine bricks or two come down, which is the
	 *                 entire content of a LOCAL LOSS verdict. By hand a tooth is cheap to hang:
	 *                 0.1 MPa over two head joints (2 x 66.625 cm^2) and the two bed patches above
	 *                 it (2 x 105.0625 cm^2) is 3434 N against a brick's 26.67 N, about 129x — so
	 *                 the named teeth are demonstrably not what 82.63 is measuring
	 *
	 * THE STANDING DOUBT SURVIVES THE MEASUREMENT UNTOUCHED: the true count is more than the two
	 * named and fewer than the model's nine, and the LP could not narrow it because it does not
	 * speak that language. It waits for equilibrium promotion (DESIGN §7 step 4), not for another
	 * ruling.
	 */
	const FWallRegion Case20Cuts[] =
	{
		{ 1, 1, 0.5, 6.5 },
		{ 2, 2, 0.5, 5.5 },
		{ 3, 3, 0.5, 4.5 },
		{ 4, 4, 0.5, 3.5 },
		{ 5, 5, 0.5, 2.5 },
		{ 6, 6, 0.5, 1.5 },
	};

	const FWallRegion Case20Falls[] =
	{
		{ 3, 3, 4.40, 4.60 },
		{ 5, 5, 2.40, 2.60 },
	};

	/**
	 * The catalogue, built rather than aggregate-initialised so every field is named at its value.
	 *
	 * TWENTY CASES, NINETEEN OF THEM RECTANGLES WITH HOLES IN. Nothing here is a shape somebody
	 * invented to break the solver: cases 1-5 are the deletions that fire on every click, 6-10 are
	 * doorways, 11-12 are a wall on piers, 13-16 are corbelling, 17-18 are the bond, and 19-20 are
	 * where collapse is the right answer.
	 */
	TArray<FWallCase> AllWallCases()
	{
		TArray<FWallCase> Cases;

		auto Add = [&Cases](
			int32 Number,
			const TCHAR* Title,
			EVerdict Verdict,
			int32 Courses,
			int32 Cells,
			TArrayView<const FWallRegion> Cuts,
			TArrayView<const FWallRegion> MustFall,
			TArrayView<const FWallRegion> MustStand,
			const TCHAR* Isolates) -> FWallCase&
		{
			FWallCase Case;
			Case.Number = Number;
			Case.Title = Title;
			Case.Verdict = Verdict;
			Case.Courses = Courses;
			Case.Cells = Cells;
			Case.Cuts = Cuts;
			Case.MustFall = MustFall;
			Case.MustStand = MustStand;
			Case.Isolates = Isolates;

			return Cases.Add_GetRef(Case);
		};

		/* A — one brick out. Nothing here should do more than settle. */

		Add(1, TEXT("Intact wall"), EVerdict::Stands,
			TallCourses, StandardCells, {}, {}, {}, nullptr);

		Add(2, TEXT("One brick out, mid-wall"), EVerdict::Stands,
			TallCourses, StandardCells, Case2Cuts, {}, {}, TEXT("bond, against case 18"));

		/*
		 * CASE 3 IS THE ONE A PLAYER REPORTED, and the user ruled on 2026-08-06 that a brick
		 * deleted at a free end must not bring the wall down. ARCHING_DESIGN.md's slices 1-4 do
		 * not reach it — the surviving brick overhangs OUTWARD with nothing beyond it to abut
		 * against, so the arch is correctly refused and the cantilever ladder starts — so this row
		 * stays red until composite vertical action lands. It is not a defect in the row.
		 */
		Add(3, TEXT("One brick out at the free end"), EVerdict::Stands,
			TallCourses, StandardCells, Case3Cuts, {}, {}, nullptr);

		Add(4, TEXT("One brick out of the bottom course"), EVerdict::Stands,
			TallCourses, StandardCells, Case4Cuts, {}, {}, nullptr);

		Add(5, TEXT("Alternate bricks out of one course"), EVerdict::Stands,
			TallCourses, StandardCells, Case5Cuts, {}, {}, nullptr);

		/* B — openings and depth of cover. */

		Add(6, TEXT("Two-brick opening, deep cover"), EVerdict::Stands,
			CoveredCourses, StandardCells, TwoCellOpening, {}, {}, nullptr);

		/*
		 * The wall the other three rows of section B are each compared against, and as of
		 * 2026-08-12 NOT ONE OF THE THREE PAIRS SEPARATES ON OUTCOME ANY MORE: cover went with case
		 * 8's re-ruling on 2026-08-11, span and abutment with cases 9 and 10 on 2026-08-12. The
		 * Isolates line says so rather than naming comparisons no pair test makes, and the file
		 * header lists where each of the three discriminations went.
		 */
		Add(7, TEXT("Four-brick opening, eight courses over"), EVerdict::Stands,
			CoveredCourses, StandardCells, FourCellOpening, {}, {},
			TEXT("span vs 9 — NOW IN THE READING; cover vs 8 and abutment vs 10 NO LONGER SEPARATE"));

		/*
		 * STANDS — RE-RULED 2026-08-11, and the catalogue is what moved. This row asked for a LOCAL
		 * LOSS of the two seatless middle bricks on the reading that one course of cover cannot
		 * arch; the LP oracle stands the fixture at lambda* = 324.73 through head-joint compression
		 * into the abutments, production drops nothing, and the user ruled STANDS on the physics per
		 * DESIGN §8's standing instruction. The whole derivation, the discount arithmetic and THE
		 * COST — this was the set's last "no room to arch" discriminator — are in the CASE 8 block
		 * of section B, immediately above Case9Cuts. No fall region, because nothing comes down.
		 */
		Add(8, TEXT("Four-brick opening, one course over"), EVerdict::Stands,
			5, StandardCells, FourCellOpening, {}, {},
			TEXT("depth of cover against case 7 — NO LONGER SEPARATES, on either outcome or reading"));

		/*
		 * STANDS — RE-RULED 2026-08-12, AND THE ROW GOES GREEN. The catalogue asked for a COLLAPSE
		 * on the published arching gate; the LP oracle stands the fixture at lambda* = 36.56, a
		 * hand deep-beam check reads 0.88x characteristic and ~0.15x mean, and production already
		 * stood it at a worst joint of 0.985. Three methods, one outlier, and the outlier was the
		 * catalogue. The whole derivation and THE COST — this was the third and last verdict
		 * resting on the arching gate — are in the CASE 9 block of section B. No fall region and no
		 * survivor region, because nothing comes down; and no `DropsToday`, because a row the model
		 * agrees with has no wrong answer to characterise.
		 */
		Add(9, TEXT("Ten-brick opening, eight courses over"), EVerdict::Stands,
			CoveredCourses, 14, Case9Cuts, {}, {},
			TEXT("span against case 7 — NOW IN THE READING, not the outcome"));

		/*
		 * THE THREE KNOWN REDS EACH CARRY A CHARACTERISATION OF TODAY'S WRONG ANSWER, measured off a
		 * run and never guessed. See FWallCase::DropsToday: these numbers say what the model DOES,
		 * never what it should do, and each is deleted by whichever slice fixes its row.
		 *
		 * AND SINCE 2026-08-12 THEY DO NOT ALL POINT THE SAME WAY, which is the thing to read
		 * carefully before touching any of them. 20 is the shape this field was invented for: a
		 * LOCAL LOSS of two named teeth where the model drops nine, i.e. too MANY. 10 and 19 are
		 * the new shape — rows the catalogue rules STANDS while the model drops 12 and 34, so their
		 * pins count pieces that should never have left the wall at all. Both are red in the
		 * INVERTED direction and both go green at DESIGN §7's evolution step 4, when equilibrium
		 * becomes the cascade's authority; 20 needs the same step for a different reason (its
		 * over-count is a real strength verdict spreading too far, not an absent mechanism).
		 *
		 * THREE ROWS HAVE LEFT THIS SET BY AN EXPECTATION MOVING RATHER THAN BY A FIX: case 12 on
		 * 2026-08-09, when its rewrite retired the ten-cell cut whose wrong answer was pinned at 76
		 * dropped and 11 stranded; case 8 on 2026-08-11, whose pinned wrong answer was 0 dropped
		 * against the two the catalogue named; and case 9 on 2026-08-12, whose pinned 0 likewise
		 * had nothing left to characterise once the row was ruled to stand.
		 *
		 * AND TWO OF THE THREE ALSO STRAND, WHICH IS A SEPARATE AND SHARPER FINDING — see
		 * FWallCase::StrandsToday. Of the twelve pieces case 10 drops, three are pieces the solver
		 * could not route rather than pieces the wall could not hold, and the oracle sweep measured
		 * that ALL twelve got there without a joint breaking.
		 */
		{
			FWallCase& Case = Add(10, TEXT("Opening at a free end, no abutment"), EVerdict::Stands,
				CoveredCourses, StandardCells, Case10Cuts, {}, Case10Stands,
				TEXT("abutment against case 7 — NO LONGER SEPARATES; see the CASE 10 block"));

			Case.DropsToday = 12;
			Case.StrandsToday = 3;
		}

		/* C — spanning between supports. */

		/*
		 * STANDS — RULED DOWN TO LOCAL LOSS ON 2026-08-08 AND RE-RULED BACK THE SAME DAY, and the
		 * second ruling is the keeper. The local-loss reading was BS 5977's 300-mm-above-the-apex
		 * arching gate, which this fixture fails by its whole height; the user then ruled that a
		 * published DESIGN threshold is not a collapse predictor, and the honest physics puts the
		 * 60 cm of bonded masonry over the span at span/depth 2.3 — a deep beam carrying its own
		 * ~120 kg at roughly 0.03-0.04 MPa, five to ten per cent of mean bond strength and under a
		 * third of even the conservative characteristic 0.10. See the block above Case11Cuts in
		 * section C for both rulings and the whole derivation.
		 */
		Add(11, TEXT("Wall on two piers, six-brick clear span"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case11Cuts, {}, {},
			TEXT("pier width against case 12 — IN THE MARGIN, not the outcome"));

		/*
		 * STANDS — REWRITTEN AND RE-RULED 2026-08-09. The old row cut ten cells out of twelve,
		 * varying span and pier at once and near-duplicating case 9; this is case 11's own span
		 * and cover on a ONE-cell pier, and the arithmetic above Case12Cuts says the pier takes
		 * the thrust — minimum thrust ~400 N against a bonded restoring capacity four to eight
		 * times that on the adopted mean basis. The recorded expectation that the thrust shoves
		 * the pier over was a rigid-body overturning reading, rejected here for the third time
		 * (cases 14 and 16 are the others). No fall region and no survivor region, because
		 * nothing comes down.
		 */
		Add(12, TEXT("The same span on a one-brick pier"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case12Cuts, {}, {},
			TEXT("pier width against case 11 — IN THE MARGIN, not the outcome"));

		/* D — corbelling, and the header that is held down by what sits on it. */

		{
			FWallCase& Case = Add(13, TEXT("Corbel, quarter brick per course"), EVerdict::Stands,
				10, CorbelCells, {}, {}, {},
				TEXT("corbel projection against case 14 — IN THE READING, not the outcome"));

			Case.CorbelFromCourse = CorbelFirstCourse;
			Case.CorbelStepCm = QuarterBrickStepCm;
		}

		/*
		 * STANDS, ON THE USER'S RULING OF 2026-08-07 AND ON THE ARITHMETIC ABOVE. Drafted as a
		 * collapse taking the four projecting bricks; the bottom rung reads 0.195160875 of f_xk1
		 * and nothing in the ladder is near 1.0. No fall region and no survivor region, because
		 * there is nothing to name — see the block above section D for the whole derivation and
		 * for what the abandoned overturning reading claimed instead.
		 */
		{
			FWallCase& Case = Add(14, TEXT("Corbel, half brick per course"), EVerdict::Stands,
				10, CorbelCells, {}, {}, {},
				TEXT("corbel projection against case 13 — IN THE READING, not the outcome"));

			Case.CorbelFromCourse = CorbelFirstCourse;
			Case.CorbelStepCm = HalfBrickStepCm;
		}

		{
			FWallCase& Case = Add(15, TEXT("Header out half a brick, six courses on top"),
				EVerdict::Stands, 10, StandardCells, {}, {}, {},
				TEXT("superimposed load against case 16 — IN THE READING, not the outcome"));

			Case.ProjectingCourse = 3;
		}

		/*
		 * STANDS, ON THE CATALOGUE'S 2026-08-07 REVISION AND ON THE ARITHMETIC ABOVE. Drafted as a
		 * local loss taking the projecting header; the header's own bed joint reads 0.058203838 of
		 * f_xk1, which is a sixth of what holds it. No fall region and no survivor region, because
		 * there is nothing to name — see the block above section E for the whole derivation and for
		 * what the abandoned overturning reading claimed instead.
		 */
		{
			FWallCase& Case = Add(16, TEXT("The same header at the top, nothing on it"),
				EVerdict::Stands, 10, StandardCells, {}, {}, {},
				TEXT("superimposed load against case 15 — IN THE READING, not the outcome"));

			Case.ProjectingCourse = 9;
		}

		/* E — bond pattern and head-joint shear. Both stand, and that is the finding. */

		{
			FWallCase& Case = Add(17, TEXT("Stack bond, intact"), EVerdict::Stands,
				10, StandardCells, {}, {}, {}, nullptr);

			Case.Bond = EBond::Stack;
		}

		{
			FWallCase& Case = Add(18, TEXT("Stack bond, one brick out"), EVerdict::Stands,
				10, StandardCells, Case18Cuts, {}, {}, TEXT("bond, against case 2"));

			Case.Bond = EBond::Stack;
		}

		/* F — losing the base, and the staircase void. */

		/*
		 * STANDS — RE-RULED 2026-08-12, AND THE ROW STAYS RED IN THE INVERTED DIRECTION. The
		 * catalogue asked for a COLLAPSE of everything over 135 cm of missing footing; the LP
		 * oracle stands it at lambda* = 12.38 (the lowest of the fifteen walls and still above 1),
		 * and production drops 34 WITHOUT BREAKING A JOINT, which is unroutability rather than
		 * strength. The hand check straddles — 1.25x characteristic f_xk1 against 0.31x f_xk2 and
		 * ~0.21x mean — so this is the closest call of the three ruled that day and it is recorded
		 * as one, END-cantilever caveat included, in the CASE 19 block above. NO FALL REGION, BUT
		 * THE SURVIVOR REGION STAYS: a STANDS verdict implies nothing comes down anywhere, so
		 * naming the still-footed right half is still true, and it is the only assertion on this
		 * row that can see the spreading front MOVE rather than merely change size. The pins stay
		 * because the model still disagrees.
		 */
		{
			FWallCase& Case = Add(19, TEXT("Bottom course out under half the wall"),
				EVerdict::Stands, 10, StandardCells, Case19Cuts, {}, Case19Stands, nullptr);

			Case.DropsToday = 34;
			Case.StrandsToday = 6;
		}

		/*
		 * LOCAL LOSS — RE-EXAMINED 2026-08-12 AND CONFIRMED UNCHANGED, which is a ruling too. It
		 * went to the user with cases 9, 10 and 19; those three moved and this one did not. The
		 * measurement had nothing to move it with: lambda* is global and a local loss is local, and
		 * unlike 10 and 19 production's answer here IS a strength verdict (one pass, worst 42.71).
		 * The standing doubt — the true count is above two and below the model's nine — survives
		 * the measurement and waits for equilibrium promotion. See the block above Case20Cuts.
		 */
		Add(20, TEXT("Staircase void"), EVerdict::LocalLoss,
			CoveredCourses, 14, Case20Cuts, Case20Falls, {}, nullptr)
			.DropsToday = 9;

		return Cases;
	}

	/** Every piece the regions name, in handle order. */
	TArray<int32> PiecesInRegions(const FWall& Wall, TArrayView<const FWallRegion> Regions)
	{
		TArray<int32> Named;

		for (int32 Piece = 0; Piece < Wall.NumPieces(); ++Piece)
		{
			if (Wall.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			if (AnyRegionContains(Regions, Wall.CourseOf[Piece], Wall.CellOf[Piece]))
			{
				Named.Add(Piece);
			}
		}

		return Named;
	}

	/* ================================================================================
	 * THE TWENTY AS PLAYABLE LEVELS — what a scenario row for one of these cases must be.
	 * ================================================================================
	 *
	 * These twenty configurations are the ones the user drew and reviewed, and until now the only
	 * thing that could lay them was the fixture bricklayer above — which lives in a test file, so
	 * no level could ever reach it. The three tests at the bottom of this file are the acceptance
	 * criteria for moving the GEOMETRY into production and leaving the VERDICTS here.
	 *
	 * THE VERDICTS DO NOT MOVE, and that is not a detail of the sequencing. `EVerdict`, `MustFall`,
	 * `MustStand` and `Isolates` are claims about what the solver ought to conclude; they are
	 * acceptance-test property. A level needs the geometry and the cuts and nothing else, and
	 * moving an expected outcome into production would be putting an assertion there.
	 */

	/**
	 * WHAT A CASE'S LEVEL IS CALLED, AND WHY IT IS THE NUMBER RATHER THAN THE TITLE.
	 *
	 * A scenario name is typed on a URL and a map name becomes a filename on disk, so both want to
	 * be short, stable and free of punctuation; a case title is prose with commas and hyphens in it
	 * and is carried VERBATIM in the row's own `Title` instead. Two digits so `wall-2` cannot sort
	 * or read as `wall-20`.
	 */
	FString LevelNameForCase(int32 Number)
	{
		return FString::Printf(TEXT("wall-%02d"), Number);
	}

	FString LevelMapNameForCase(int32 Number)
	{
		return FString::Printf(TEXT("Lvl_Wall%02d"), Number);
	}

	/**
	 * THE ONE MACHINE-CHECKABLE CLAIM A LEVEL'S CAPTION MAKES.
	 *
	 * A caption is prose and has to stay prose — it is the only thing telling a player what they
	 * are looking at. What cannot be prose is the VERDICT it claims, because a level captioned with
	 * an outcome the solver does not produce is a lie told to somebody standing in front of the
	 * counter-example. So the caption carries exactly one token naming its verdict, spelled the way
	 * this file spells verdicts, and the rest of the sentence is the writer's.
	 */
	FString VerdictClaimFor(EVerdict Verdict)
	{
		return FString::Printf(TEXT("Expected: %s"), VerdictName(Verdict));
	}

	/**
	 * AND THE TOKEN A CAPTION MUST CARRY WHEN THE MODEL DOES NOT AGREE WITH ITS OWN CLAIM.
	 *
	 * Three rows are red today. A caption that named only the expected verdict on one of those
	 * would be worse than silence — so the honest ones say both, and the ones the model agrees with
	 * must NOT say it, which is what stops the admission outliving the disagreement.
	 *
	 * AND SINCE 2026-08-12 THE MARKER NO LONGER IMPLIES "THE MODEL IS TOO OPTIMISTIC". Cases 10
	 * and 19 are captioned `Expected: STANDS` AND carry this marker, because the wall is ruled to
	 * stand and the model drops 12 and 34 bricks. A player reading either caption is being told the
	 * wall in front of them should be intact and is not — which is exactly as honest as the older
	 * direction and reads very differently.
	 */
	const TCHAR* const ModelDisagreesMarker = TEXT("THE MODEL CURRENTLY DISAGREES");

	/**
	 * Whether the model produced the verdict this row claims.
	 *
	 * DELIBERATELY THE SAME THREE SHAPES `Acceptance.Wall.Catalogue` ASSERTS ONE AT A TIME, and it
	 * is a second reading of them rather than a shared one because the catalogue's assertions are
	 * SEPARATE on purpose — each prints its own diagnosis — and folding them into one predicate
	 * would change three known-red failure messages. The cost is a copy, and the copy is held
	 * against the catalogue by the known-red tripwire in the caption test: it must name exactly the
	 * three rows that are red, so a predicate that drifted from the catalogue fails there rather
	 * than quietly captioning a level wrongly.
	 *
	 * THE `Stands` ARM IS WHAT DECIDES CASES 10 AND 19 SINCE THEIR 2026-08-12 RE-RULING, and it
	 * needs no change to do it: both rows are ruled STANDS, both drop pieces, so `Fallen.Num() == 0`
	 * is false and the predicate reports a disagreement. That is worth stating because it is the
	 * first time this arm has ever returned false — every previous STANDS row was one the model
	 * produced — and a reader might reasonably assume the arm was only ever exercised in the
	 * agreeing direction.
	 */
	bool ModelAgreesWithVerdict(const FWallCase& Case, const FWall& Wall, const FWallResult& Result)
	{
		switch (Case.Verdict)
		{
		case EVerdict::Stands:
			return Result.Fallen.Num() == 0
				&& Result.CutPasses == 0
				&& Wall.Structure.NumLivePieces() == Result.PiecesLaid - Result.PiecesCut;

		case EVerdict::LocalLoss:
		{
			const TArray<int32> Named = PiecesInRegions(Wall, Case.MustFall);

			if (Named.Num() == 0 || Named.Num() != Result.Fallen.Num())
			{
				return false;
			}

			for (const int32 Piece : Named)
			{
				if (!Result.Fallen.Contains(Piece))
				{
					return false;
				}
			}

			return true;
		}

		default:
		{
			const TArray<int32> MustFall = PiecesInRegions(Wall, Case.MustFall);
			const TArray<int32> MustStand = PiecesInRegions(Wall, Case.MustStand);

			if (MustFall.Num() == 0)
			{
				return false;
			}

			for (const int32 Piece : MustFall)
			{
				if (!Result.Fallen.Contains(Piece))
				{
					return false;
				}
			}

			for (const int32 Piece : MustStand)
			{
				if (Result.Fallen.Contains(Piece))
				{
					return false;
				}
			}

			return true;
		}
		}
	}

	/** The production spec that must lay this case's wall, transcribed field for field. */
	DestructionWallCases::FWallSpec ProductionSpecOf(const FWallCase& Case)
	{
		DestructionWallCases::FWallSpec Spec;

		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm);
		Spec.JointThicknessCm = JointCm;
		Spec.DensityGramsPerCubicCm = ClayBrickDensityGramsPerCubicCm;
		Spec.CoursesHigh = Case.Courses;
		Spec.Cells = Case.Cells;

		Spec.Bond = Case.Bond == EBond::Stack
			? DestructionWallCases::EWallBond::Stack
			: DestructionWallCases::EWallBond::Running;

		Spec.CorbelFromCourse = Case.CorbelFromCourse;
		Spec.CorbelStepCm = Case.CorbelStepCm;
		Spec.ProjectingCourse = Case.ProjectingCourse;
		Spec.Strength = GeneralPurposeMortar;

		return Spec;
	}

	/** The same rectangles, in production's own vocabulary. */
	TArray<DestructionWallCases::FWallRegion> ProductionRegionsOf(
		TArrayView<const FWallRegion> Regions)
	{
		TArray<DestructionWallCases::FWallRegion> Out;
		Out.Reserve(Regions.Num());

		for (const FWallRegion& Region : Regions)
		{
			DestructionWallCases::FWallRegion Theirs;

			Theirs.CourseLo = Region.CourseLo;
			Theirs.CourseHi = Region.CourseHi;
			Theirs.CellLo = Region.CellLo;
			Theirs.CellHi = Region.CellHi;

			Out.Add(Theirs);
		}

		return Out;
	}

	inline FString Bits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	inline FString VectorBits(const FVector& Value)
	{
		return FString::Printf(TEXT("(%.17g, %.17g, %.17g)"), Value.X, Value.Y, Value.Z);
	}

	/**
	 * Compare a laid layout against the fixture's wall, BRICK FOR BRICK AND IN HANDLE ORDER.
	 *
	 * HANDLE ORDER IS PART OF THE CLAIM AND IT IS THE HALF THAT LOOKS FINE WHEN IT IS WRONG. A
	 * builder that lays the same bricks in a different sequence renumbers every joint and every
	 * break stamp while every geometric check still passes — and every reading in this file, in
	 * `Core.Structure` and in the design documents is a statement about a particular joint index.
	 *
	 * THE FIRST DISAGREEMENT IS REPORTED, NOT ALL OF THEM. A wall is up to 375 pieces and 1,000
	 * joints; a producer that moved every brick would otherwise print thousands of failures and
	 * bury everything else in the run.
	 *
	 * @return true if they are the same wall.
	 */
	bool LayoutMatchesFixture(
		FAutomationTestBase& Test,
		const FString& Where,
		const DestructionLayout::FBrickLayout& Laid,
		const FWall& Fixture)
	{
		bool bSameWall = true;

		bSameWall &= Test.TestTrue(
			*FString::Printf(
				TEXT("%s: production laid %d pieces and %d joints; the fixture lays %d and %d"),
				*Where, Laid.Structure.NumPieces(), Laid.Structure.NumConnections(),
				Fixture.Structure.NumPieces(), Fixture.Structure.NumConnections()),
			Laid.Structure.NumPieces() == Fixture.Structure.NumPieces()
				&& Laid.Structure.NumConnections() == Fixture.Structure.NumConnections()
				&& Laid.Boxes.Num() == Fixture.Boxes.Num());

		int32 FirstWrongPiece = INDEX_NONE;
		FString WhyPieceWrong;

		const int32 CommonPieces = FMath::Min(Laid.Boxes.Num(), Fixture.Boxes.Num());

		for (int32 Piece = 0; Piece < CommonPieces; ++Piece)
		{
			const DestructionLayout::FPieceBox& Mine = Laid.Boxes[Piece];
			const DestructionLayout::FPieceBox& Theirs = Fixture.Boxes[Piece];

			const bool bSame = Mine.CentreCm == Theirs.CentreCm
				&& Mine.ExtentCm == Theirs.ExtentCm
				&& Laid.Structure.GetPiece(Piece).MassKg
					== Fixture.Structure.GetPiece(Piece).MassKg
				&& Laid.Structure.GetPiece(Piece).bIsGrounded
					== Fixture.Structure.GetPiece(Piece).bIsGrounded;

			if (!bSame)
			{
				FirstWrongPiece = Piece;

				WhyPieceWrong = FString::Printf(
					TEXT("production has it at %s, half-extent %s, %s kg, %s; the fixture has it at ")
					TEXT("%s, half-extent %s, %s kg, %s"),
					*VectorBits(Mine.CentreCm), *VectorBits(Mine.ExtentCm),
					*Bits(Laid.Structure.GetPiece(Piece).MassKg),
					Laid.Structure.GetPiece(Piece).bIsGrounded ? TEXT("grounded") : TEXT("free"),
					*VectorBits(Theirs.CentreCm), *VectorBits(Theirs.ExtentCm),
					*Bits(Fixture.Structure.GetPiece(Piece).MassKg),
					Fixture.Structure.GetPiece(Piece).bIsGrounded ? TEXT("grounded") : TEXT("free"));

				break;
			}
		}

		bSameWall &= Test.TestTrue(
			*FString::Printf(
				TEXT("%s: every brick must be BIT-IDENTICAL to the fixture's and carry the same ")
				TEXT("handle — piece %d is the first that is not: %s"),
				*Where, FirstWrongPiece,
				FirstWrongPiece == INDEX_NONE ? TEXT("none is") : *WhyPieceWrong),
			FirstWrongPiece == INDEX_NONE);

		int32 FirstWrongJoint = INDEX_NONE;
		FString WhyJointWrong;

		const int32 CommonJoints =
			FMath::Min(Laid.Structure.NumConnections(), Fixture.Structure.NumConnections());

		for (int32 Joint = 0; Joint < CommonJoints; ++Joint)
		{
			const FConnection& Mine = Laid.Structure.GetConnection(Joint);
			const FConnection& Theirs = Fixture.Structure.GetConnection(Joint);

			const bool bSame = Mine.PieceA == Theirs.PieceA
				&& Mine.PieceB == Theirs.PieceB
				&& Mine.InterfaceNormal == Theirs.InterfaceNormal
				&& Mine.InterfaceAreaSqCm == Theirs.InterfaceAreaSqCm
				&& Mine.InterfaceCentreCm == Theirs.InterfaceCentreCm
				&& Mine.InterfaceHalfExtentCm == Theirs.InterfaceHalfExtentCm;

			if (!bSame)
			{
				FirstWrongJoint = Joint;

				WhyJointWrong = FString::Printf(
					TEXT("production joins %d-%d, normal %s, %s cm2, centred %s, half-extent %s; the ")
					TEXT("fixture joins %d-%d, normal %s, %s cm2, centred %s, half-extent %s"),
					Mine.PieceA, Mine.PieceB, *VectorBits(Mine.InterfaceNormal),
					*Bits(Mine.InterfaceAreaSqCm), *VectorBits(Mine.InterfaceCentreCm),
					*VectorBits(Mine.InterfaceHalfExtentCm),
					Theirs.PieceA, Theirs.PieceB, *VectorBits(Theirs.InterfaceNormal),
					*Bits(Theirs.InterfaceAreaSqCm), *VectorBits(Theirs.InterfaceCentreCm),
					*VectorBits(Theirs.InterfaceHalfExtentCm));

				break;
			}
		}

		bSameWall &= Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the connection set must be the fixture's, IN ORDER — joint %d is the first ")
				TEXT("that is not: %s"),
				*Where, FirstWrongJoint,
				FirstWrongJoint == INDEX_NONE ? TEXT("none is") : *WhyJointWrong),
			FirstWrongJoint == INDEX_NONE);

		return bSameWall;
	}
}

/**
 * THE ACCEPTANCE SET ITSELF: every case in the catalogue, its own verdict, one row at a time.
 *
 * THE ASSERTION IS ON OUTCOME AND ITS SHAPE FOLLOWS THE VERDICT — see the file header for why the
 * middle verdict exists and why displacement is not used anywhere. A failure names the case, the
 * variable its matched pair isolates, and both the expected and the actual fallen set in
 * (course, cell) terms, so the log says which bricks came down rather than how many.
 *
 * MOST OF THIS IS EXPECTED TO BE RED, AND THAT IS CORRECT. These are the acceptance criteria for
 * the arching work described in claude_plans/ARCHING_DESIGN.md, not a regression net over
 * something finished. The signature to look for is the one CURRENT_STATE.md predicts: every
 * falling half of a pair passing while every standing half fails, which is what a model that can
 * only ever say "falls" looks like.
 *
 * SEVERAL ROWS ARE GREEN ON ARRIVAL AND EACH IS SAID TO BE, so nobody mistakes them for work this
 * suite drove. 1 and 17 are intact walls and are regression anchors. 18 is a stack-bond column
 * that really does sit at a few percent — its verdict passes and the property that made the case
 * interesting does NOT, which is why that property has a test of its own below. And 8, 9, 12, 13,
 * 14 and 16 all stand, which is a CORRECTION rather than an achievement: 14 was drafted as a
 * collapse and 16 as a local loss, revised on 2026-08-07 on the same reading — a bonded section
 * resists what a rigid block could not — 12 was rewritten and re-ruled on 2026-08-09 on that
 * reading's third outing (the one-cell pier holds the thrust; see the block above Case12Cuts), 8
 * was re-ruled on 2026-08-11 on its fourth (the coverless course jams into its abutments), and 9
 * on 2026-08-12 on its fifth (60 cm of bonded masonry spans 2.1 m as a deep beam). Each block in
 * section B carries its own arithmetic, cost and doubts.
 *
 * AND SINCE 2026-08-12 TWO ROWS ARE RED IN A DIRECTION THIS TEST HAD NEVER SEEN BEFORE. Cases 10
 * and 19 were re-ruled from COLLAPSE to STANDS on the same day, and the model DOES NOT AGREE: it
 * drops 12 and 34 pieces respectively, in zero cascade passes, so what fails is the STANDS
 * verdict's "nothing left the structure" half. Until then every red row here asked for MORE loss
 * than the model produced. Reading one of those two failures as "the wall came down and the test
 * wanted it to come down harder" is exactly backwards: the wall is ruled to stand and the solver
 * dropped a third of it.
 *
 * The pairs those corrections stopped separating on outcome have each moved onto what still
 * separates them, except 7 vs 8 and 7 vs 10, which are losses rather than relocations; the file
 * header lists all five pairs and the fate of each.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceCatalogueTest,
	"DestructionGame.Acceptance.Wall.Catalogue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace WallAcceptanceTestSupport;

	/*
	 * THE PROFILE THIS FILE'S ARITHMETIC ASSUMES, CHECKED RATHER THAN IMPORTED. Every expected
	 * value below is a consequence of these three published figures; a retune of any of them
	 * should turn this row red rather than silently move every case in the catalogue.
	 */
	TestEqual(TEXT("FIXTURE: general purpose mortar's cohesion is EN 1996-1-1 Table 3.4's f_vk0"),
		GeneralPurposeMortar.ShearCohesionMPa, MortarShearCohesionMPa);

	TestEqual(TEXT("FIXTURE: general purpose mortar's flexural bond is Table 3.2's f_xk1"),
		GeneralPurposeMortar.TensileStrengthMPa, MortarFlexuralBondMPa);

	TestEqual(TEXT("FIXTURE: the fixture's brick density is ClayBrick's published one"),
		ClayBrick.DensityGramsPerCubicCm, ClayBrickDensityGramsPerCubicCm);

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty cases"), Cases.Num(), 20);

	for (const FWallCase& Case : Cases)
	{
		const FSolvedWall& Solved = RunWallCase(*this, Case);

		const FWall& Wall = Solved.Wall;
		const FWallResult& Result = Solved.Result;

		ReportWallCase(*this, Case, Wall, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		const FString Where = FString::Printf(
			TEXT("case %d (%s)%s%s"),
			Case.Number, Case.Title,
			Case.Isolates == nullptr ? TEXT("") : TEXT(" [isolates "),
			Case.Isolates == nullptr ? TEXT("") : *FString::Printf(TEXT("%s]"), Case.Isolates));

		switch (Case.Verdict)
		{
		case EVerdict::Stands:
		{
			/*
			 * BOTH HALVES, AND NEITHER IS DECORATION. "Nothing fell" alone passes for a wall that
			 * severed every joint over the cut and stayed leaning together; "no joint gave" alone
			 * passes for a wall whose pieces were never in the load path to begin with.
			 */
			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means nothing left the structure; %d piece(s) did, %s"),
					*Where, Result.Fallen.Num(), *DescribePieces(Wall, Result.Fallen)),
				Result.Fallen.Num(), 0);

			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means no joint over the cut gave; the cascade ran %d breaking ")
					TEXT("pass(es)"),
					*Where, Result.CutPasses),
				Result.CutPasses, 0);

			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means the piece count is what was laid less what was cut; ")
					TEXT("laid %d, cut %d, live %d"),
					*Where, Result.PiecesLaid, Result.PiecesCut, Wall.Structure.NumLivePieces()),
				Wall.Structure.NumLivePieces(), Result.PiecesLaid - Result.PiecesCut);

			/*
			 * AND ON A ROW THAT CUTS NOTHING, THE INTACT SOLVE IS THE ONLY SOLVE THERE IS.
			 *
			 * `CutPasses` above is STRUCTURALLY ZERO on the six no-cut rows — 1, 13, 14, 15, 16 and
			 * 17 — because the whole block that sets it is inside `if (Case.Cuts.Num() > 0)`, so
			 * the "no joint over the cut gave" half of a STANDS verdict was asserting nothing at
			 * all on exactly the rows whose as-built state IS the case under test. A corbel that
			 * peeled every joint it stands on while nothing lost its footing would have read as a
			 * clean pass.
			 *
			 * The cutting rows already make this claim, harder, in `CheckWallFixture`: an intact
			 * wall that broke joints or dropped pieces before the player touched it fails the
			 * precondition there. So this is the same claim on the rows that precondition skips,
			 * and it is written here rather than there because on a no-cut row it is a VERDICT and
			 * not a fixture check.
			 */
			/*
			 * AND WHERE A STANDS ROW NAMES A SURVIVOR REGION, THAT REGION IS ASSERTED TOO — WHICH
			 * IS ONLY EVER USEFUL WHILE THE ROW IS RED.
			 *
			 * A STANDS verdict already says nothing comes down anywhere, so this claim is IMPLIED
			 * by the first assertion above and cannot fail on a row that passes. What it buys is on
			 * the rows that FAIL: cases 10 and 19 are ruled to stand and drop 12 and 34 pieces, and
			 * the failure line above reads the same for any twelve and any thirty-four. The pins
			 * beside it hold the COUNT. Neither holds the IDENTITY — a routing change that shifted
			 * case 19's spreading front three cells right, dropping pieces in the still-footed half
			 * while sparing an equal number at the far left, would keep the count, keep the stranded
			 * count, and keep every other assertion on the row green.
			 *
			 * SO IT IS ASSERTED INVERTED, as a characterisation of WHERE THE WRONGNESS IS NOT: the
			 * named masonry must contain NO fallen piece. It is a true claim about the ruling and a
			 * measured claim about today, and both blocks in section B record the drop sets the
			 * regions were checked against. When the rows go green at evolution step 4 these become
			 * tautologies alongside the rest of the STANDS shape, and they may be deleted with the
			 * pins in the same edit.
			 */
			if (Case.MustStand.Num() > 0)
			{
				const TArray<int32> Survivors = PiecesInRegions(Wall, Case.MustStand);

				TestTrue(
					*FString::Printf(
						TEXT("%s: FIXTURE the named survivor region must name at least one brick, or ")
						TEXT("the claim below is vacuous"),
						*Where),
					Survivors.Num() > 0);

				TArray<int32> WronglyFallen;

				for (const int32 Piece : Survivors)
				{
					if (Result.Fallen.Contains(Piece))
					{
						WronglyFallen.Add(Piece);
					}
				}

				TestEqual(
					*FString::Printf(
						TEXT("%s: STANDS means the named masonry keeps its footing, and on a row the ")
						TEXT("model gets wrong this is what pins WHICH bricks it drops rather than how ")
						TEXT("many; %d of %d named survivor(s) came down, %s"),
						*Where, WronglyFallen.Num(), Survivors.Num(),
						*DescribePieces(Wall, WronglyFallen)),
					WronglyFallen.Num(), 0);
			}

			if (Case.Cuts.Num() == 0)
			{
				TestEqual(
					*FString::Printf(
						TEXT("%s: STANDS means no joint gave as the wall was BUILT either — this row ")
						TEXT("cuts nothing, so the intact cascade is the whole case and it ran %d ")
						TEXT("breaking pass(es)"),
						*Where, Result.IntactPasses),
					Result.IntactPasses, 0);
			}

			break;
		}

		case EVerdict::LocalLoss:
		{
			/*
			 * IDENTITY, NOT A COUNT. The named set is what a bounded loss MEANS: the same number of
			 * bricks falling from somewhere else is a completely different — and wrong — answer,
			 * and a test that counted would call it a pass.
			 */
			const TArray<int32> Named = PiecesInRegions(Wall, Case.MustFall);

			TestTrue(
				*FString::Printf(
					TEXT("%s: FIXTURE the named local-loss set must name at least one brick"),
					*Where),
				Named.Num() > 0);

			bool bExact = Named.Num() == Result.Fallen.Num();

			for (const int32 Piece : Named)
			{
				bExact = bExact && Result.Fallen.Contains(Piece);
			}

			TestTrue(
				*FString::Printf(
					TEXT("%s: LOCAL LOSS means exactly %s came down and nothing else did; what ")
					TEXT("came down was %s"),
					*Where, *DescribePieces(Wall, Named), *DescribePieces(Wall, Result.Fallen)),
				bExact);

			break;
		}

		default:
		{
			/*
			 * TWO-SIDED, because a wall that comes down because EVERYTHING comes down says nothing
			 * about the term this row is meant to isolate. The named survivor region is what stops
			 * a model that always answers "falls" from passing every collapse row for free. Every
			 * collapse row names one: the last that could not — old case 12, whose picture had the
			 * entire wall coming down — was rewritten out of the set on 2026-08-09.
			 */
			const TArray<int32> MustFall = PiecesInRegions(Wall, Case.MustFall);
			const TArray<int32> MustStand = PiecesInRegions(Wall, Case.MustStand);

			TestTrue(
				*FString::Printf(
					TEXT("%s: FIXTURE the named collapse region must name at least one brick"),
					*Where),
				MustFall.Num() > 0);

			TArray<int32> WronglyStanding;

			for (const int32 Piece : MustFall)
			{
				if (!Result.Fallen.Contains(Piece))
				{
					WronglyStanding.Add(Piece);
				}
			}

			TestEqual(
				*FString::Printf(
					TEXT("%s: COLLAPSE means every brick of %s came down; %d of them did not, %s"),
					*Where, *DescribePieces(Wall, MustFall),
					WronglyStanding.Num(), *DescribePieces(Wall, WronglyStanding)),
				WronglyStanding.Num(), 0);

			TArray<int32> WronglyFallen;

			for (const int32 Piece : MustStand)
			{
				if (Result.Fallen.Contains(Piece))
				{
					WronglyFallen.Add(Piece);
				}
			}

			TestEqual(
				*FString::Printf(
					TEXT("%s: COLLAPSE must not take the masonry beside it; %d of %d named ")
					TEXT("survivor(s) came down, %s"),
					*Where, WronglyFallen.Num(), MustStand.Num(),
					*DescribePieces(Wall, WronglyFallen)),
				WronglyFallen.Num(), 0);

			break;
		}
		}

		/*
		 * AND THE THREE KNOWN REDS ARE PINNED TO THE WRONG ANSWER THEY GIVE TODAY.
		 *
		 * THIS ASSERTS NOTHING ABOUT PHYSICS AND ENDORSES NOTHING. The row above has already
		 * failed; what this adds is that the failure has a FIXED SIZE. A red row is otherwise a
		 * hole in the net — case 20 fails in identical words whether it drops nine bricks or
		 * ninety, so a change that made the model twice as wrong would land inside a known failure
		 * and never be seen. Measured on 2026-08-09, one number per red row, never guessed.
		 *
		 * THE PIN IS WHAT KEEPS THE 2026-08-12 RE-RULINGS HONEST, and it is the reason cases 10 and
		 * 19 were re-ruled WITH their pins rather than pinless. Their STANDS verdict now fails on
		 * one line — "%d piece(s) left the structure" — and that line reads the same for 12 pieces
		 * as for 120. The pins hold the failure at 12 and 34 EXACTLY, so its SIZE cannot drift while
		 * the row sits red waiting for evolution step 4.
		 *
		 * A COUNT IS NOT A SHAPE, THOUGH, AND THE PIN MUST NOT BE READ AS ONE. Twelve different
		 * bricks are still twelve, so a routing change that MOVED the drop set without resizing it
		 * passes this assertion and every other count on the row. What holds the identity is the
		 * survivor region asserted up in the STANDS arm — restored on both rows for exactly this
		 * reason — and the two are complementary rather than duplicates: the pin sees a failure grow
		 * or shrink, the region sees it move.
		 *
		 * A ROW THAT GETS FIXED FAILS HERE, AND THAT IS THE POINT: the slice that fixes it deletes
		 * the row's `DropsToday` in the same edit, exactly as it must delete the level caption's
		 * disagreement marker and the entry in the caption test's known-red list. Case 9's pin went
		 * that way on 2026-08-12 — not fixed, but RE-RULED into agreement, which owes the same
		 * paperwork.
		 */
		if (Case.DropsToday != INDEX_NONE)
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: CHARACTERISATION of a KNOWN RED — the model drops %d piece(s) here ")
					TEXT("today against the %s the catalogue asks for, and that count is pinned so a ")
					TEXT("regression inside a known failure is visible. It dropped %d: %s. If a slice ")
					TEXT("just fixed this row, delete its DropsToday; if nothing here was meant to ")
					TEXT("change, the model's answer has moved and something else moved it."),
					*Where, Case.DropsToday, VerdictName(Case.Verdict),
					Result.Fallen.Num(), *DescribePieces(Wall, Result.Fallen)),
				Result.Fallen.Num(), Case.DropsToday);
		}
	}

	return true;
}

/**
 * CASES 7 AND 9, THE PART A VERDICT CANNOT SAY: TRIPLING THE SPAN OVER AN OPENING TRIPLES WHAT ITS
 * JAMB READS, AND BOTH WALLS STAND ANYWAY.
 *
 * WHY THIS TEST EXISTS AT ALL, AND WHAT IT REPLACES. 7 vs 9 was an OUTCOME pair — a four-cell
 * opening stands, a ten-cell one brings the wall down — and it was the LAST outcome pair the
 * catalogue had. On 2026-08-12 the user re-ruled case 9 to STANDS (the CASE 9 block of section B
 * carries the derivation: 60 cm of bonded masonry spans 2.1 m as a deep beam, the LP oracle prices
 * it at lambda* = 36.56, and a hand check reads 0.88x characteristic bond and ~0.15x mean), and
 * with both halves standing the old claim — the greater half loses strictly more — became
 * unsatisfiable. `Acceptance.Wall.MatchedPairs` retired in the same edit because case 10 went the
 * same way on the same day and nothing was left in its table. This is where the span
 * discrimination went, and it is the stronger reading: an outcome pair could only ever have said
 * "these differ", while this says BY HOW MUCH AND IN WHICH DIRECTION.
 *
 * =====================================================================================
 * WHY THIS IS NOT THE 7-vs-8 TRAP, WHICH IS THE ONLY REASON THE RELOCATION WAS ALLOWED
 * =====================================================================================
 *
 * The cover pair, 7 vs 8, was DELETED rather than relocated onto readings when case 8 was re-ruled
 * on 2026-08-11, and the reasoning is recorded in the CASE 8 block: production reads case 7 at
 * 0.269 with eight courses of cover and case 8 at 0.219 with one, so the halves separate by 1.23x
 * AND IN THE WRONG DIRECTION. A downward-routing solver reads cover as LOAD and never as arch
 * capacity, so more of it can only ever read worse; pinning that number would have encoded a defect
 * as a discrimination. That refusal is the standard this row has to clear, and it clears it on
 * three counts:
 *
 *   THE DIRECTION IS RIGHT. A longer span delivers a larger reaction into its abutment, so the
 *   jamb beside the wider opening MUST read harder. It does: 0.269 at 68.5-91 cm of clear opening
 *   against 0.985 at 203.5-226 cm. (Those are the brackets the assertion's own arithmetic computes
 *   below — a toothed reveal opens a different width on the even and the odd courses, so each wall
 *   has a narrow and a wide value, and quoting the CELL gaps instead would double-count the brick
 *   that closes each reveal.) Nothing has to be excused.
 *
 *   THE MECHANISM IS THE SAME ON BOTH SIDES. Both fixtures are the same deep beam — eight courses
 *   (60 cm) of bonded cover over a toothed opening in a running-bond wall, same brick, same mortar,
 *   same 10.25 cm thickness — and both worst joints are bed joints in the RIGHT JAMB at or just
 *   below the head of the opening, i.e. where that beam delivers its reaction. The pair varies the
 *   span and the wall's cell count (12 against 14, which is what keeps two cells of jamb either
 *   side) and nothing else.
 *
 *   NEITHER READING IS TAKEN OFF A WALL THE ROUTER FAILED ON. Both cases drop NOTHING, break
 *   NOTHING and strand NOTHING, so both numbers are honest readings of a fully routed structure.
 *   THIS is what disqualified the abutment pair, 7 vs 10, from the same relocation on the same day:
 *   case 10 reads 0.300 against case 7's 0.269, which is also the right direction, but it gets
 *   there while dropping twelve pieces — three of them stranded — in ZERO cascade passes. A worst
 *   joint measured beside twelve bricks the router could not route at all is not a measurement of
 *   the abutment term, and pinning it would claim a term the model demonstrably does not have. The
 *   abutment discrimination therefore went to the LP oracle instead (wall-07 at 296.22 against
 *   wall-10 at 35.82, 8.27x) and NOT into this file.
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND WHICH PART IS DERIVED
 * =====================================================================================
 *
 * THE MAGNITUDES ARE A CHARACTERISATION AND ARE SAID TO BE. Unlike the corbel and header reading
 * tests, whose expected values are worked from the grid, the published density and the published
 * f_xk1, these two numbers are outputs of the composite-depth walk over a whole wall and this file
 * cannot re-derive them without re-implementing the thing under test. So they are PINNED AS
 * MEASURED, which is worth doing on its own account: today a 2x drift in either reading would move
 * no verdict and pass every test in the suite silently.
 *
 * THE RELATION IS DERIVED, AND IT IS THE ASSERTION THAT CARRIES THE PHYSICS. The reaction a
 * simply-supported deep beam delivers into each abutment is w.L/2, and w — eight courses of brick
 * per unit length — is identical in the two fixtures, so the reaction grows AT LEAST LINEARLY with
 * the clear span. The clear spans are computed below from the cell grid rather than quoted: a gap
 * of n cell pitches between the surviving bricks either side is n.CellPitch - BrickLength of open
 * air, the toothed reveal gives each wall a narrow and a wide value, and the mean of the two is
 * 79.75 cm for case 7 against 214.75 cm for case 9 — a ratio of 2.6928. The measured readings
 * separate by 3.6558, which clears the linear floor by 36%. A solver with no span term reads them
 * the same and fails; a solver that reads span SUBLINEARLY fails too, which a bare "greater than"
 * would have let through.
 *
 * THE THING BEING MEASURED MUST BE THE THING GOVERNING, so each wall's worst joint is asserted by
 * (course, cell) before its magnitude is read. THE TWO ARE NOT THE SAME JOINT and this deliberately
 * does not pretend otherwise: case 7's is c2/8-c3/7.5, a bed joint one course BELOW the head, and
 * case 9's is c3/11.5-c4/11, the bed joint AT the head where the first course of cover lands on the
 * jamb. Both are in the right jamb within one course of the head — the reaction path — which is
 * what makes the comparison a comparison of one mechanism. If a future slice moves either maximum
 * somewhere structurally different, this fails and the pair has to be re-argued rather than
 * re-tuned.
 *
 * AND CASE 9's READING IS WATCHED FOR THE CROSSING, which is the other job this test does.
 * Production reads it at 0.98502040901419818 — ONE RETUNE FROM 1.0. The day it crosses, this wall
 * drops its head, case 9's brand-new STANDS verdict becomes a catalogue red, and the oracle sweep's
 * pinned `AgreeStands` relation flips, all with nothing physical having changed. CURRENT_STATE
 * recorded that hazard with no test behind it; the absolute pin plus the explicit under-1.0 row
 * below is that test.
 *
 * GREEN ON ARRIVAL, AND SAID SO PLAINLY. This pins behaviour the model already produces; it drove
 * nothing. What it is for is that the case 9 ruling deleted an assertion — and, through
 * MatchedPairs' retirement, a whole test — and assertions deleted without ones put back in their
 * place are holes in the net.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceSpanTest,
	"DestructionGame.Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceSpanTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/*
	 * SLACK AGAINST A RE-ASSOCIATION OF A HANDFUL OF DOUBLES AND NOTHING ELSE, exactly as the
	 * corbel and header reading tests use it. The smallest difference either row is meant to be
	 * able to see is in the second decimal place.
	 */
	constexpr double UtilisationTolerance = 1.0e-9;

	/* --- the two walls --------------------------------------------------------------------- */

	const TArray<FWallCase> Cases = AllWallCases();

	auto RunNumbered = [this, &Cases](int32 Number) -> const FSolvedWall*
	{
		for (const FWallCase& Case : Cases)
		{
			if (Case.Number != Number)
			{
				continue;
			}

			const FSolvedWall& Solved = RunWallCase(*this, Case);
			ReportWallCase(*this, Case, Solved.Wall, Solved.Result);

			return Solved.Result.bLaid ? &Solved : nullptr;
		}

		AddError(FString::Printf(TEXT("FIXTURE: the catalogue has no case %d"), Number));

		return nullptr;
	};

	/* Short-circuited deliberately: a case 7 that could not be laid says nothing about case 9. */
	const FSolvedWall* const Narrow = RunNumbered(7);
	const FSolvedWall* const Wide = Narrow == nullptr ? nullptr : RunNumbered(9);

	if (Wide == nullptr)
	{
		return true;
	}

	const FWall& NarrowWall = Narrow->Wall;
	const FWallResult& NarrowResult = Narrow->Result;

	const FWall& WideWall = Wide->Wall;
	const FWallResult& WideResult = Wide->Result;

	AddInfo(FString::Printf(
		TEXT("span pair: case 7 reads %.17g, case 9 reads %.17g, ratio %.17g"),
		NarrowResult.Worst, WideResult.Worst,
		NarrowResult.Worst > 0.0 ? WideResult.Worst / NarrowResult.Worst : 0.0));

	/* --- neither wall comes down, which is the ruling --------------------------------------- */

	TestEqual(
		*FString::Printf(
			TEXT("THE RULING: a four-cell opening under eight courses stands, %d piece(s) came down %s"),
			NarrowResult.Fallen.Num(), *DescribePieces(NarrowWall, NarrowResult.Fallen)),
		NarrowResult.Fallen.Num(), 0);

	TestEqual(
		*FString::Printf(
			TEXT("THE RULING: a TEN-cell opening under the same cover stands too — that is the ")
			TEXT("2026-08-12 re-ruling of case 9 — and %d piece(s) came down %s"),
			WideResult.Fallen.Num(), *DescribePieces(WideWall, WideResult.Fallen)),
		WideResult.Fallen.Num(), 0);

	/*
	 * AND NEITHER WALL BREAKS A JOINT EITHER, WHICH THE HEADER CLAIMS AND HAD NOT ASSERTED.
	 *
	 * "Both cases drop NOTHING, break NOTHING and strand NOTHING" is the argument that these two
	 * readings are honest readings of a fully routed structure — the whole reason this pair was
	 * allowed to relocate onto readings where 7-vs-10 was not. The dropped and stranded halves are
	 * covered by the two rows above and by the catalogue's own zero-strand claim; the BROKEN half
	 * was prose. A wall that severed joints over its opening and stayed leaning together would
	 * still read a worst utilisation, and it would be a reading of a different structure.
	 */
	TestEqual(
		*FString::Printf(
			TEXT("BREAK-NOTHING: case 7's reading must come off a wall with every joint intact, or ")
			TEXT("the magnitude below is measured on a structure the cut already changed; the cascade ")
			TEXT("ran %d breaking pass(es)"),
			NarrowResult.CutPasses),
		NarrowResult.CutPasses, 0);

	TestEqual(
		*FString::Printf(
			TEXT("BREAK-NOTHING: and so must case 9's — this is the half of the header's ")
			TEXT("distinguishing count that separates this pair from 7-vs-10; the cascade ran %d ")
			TEXT("breaking pass(es)"),
			WideResult.CutPasses),
		WideResult.CutPasses, 0);

	/* --- and each reading is taken at the jamb beside its own opening ------------------------ */

	auto DescribeWorst = [](const FWall& Wall, const FWallResult& Result)
	{
		return Result.WorstPieceA == INDEX_NONE
			? FString(TEXT("no joint at all"))
			: FString::Printf(
				TEXT("c%d/%g-c%d/%g"),
				Wall.CourseOf[Result.WorstPieceA], Wall.CellOf[Result.WorstPieceA],
				Wall.CourseOf[Result.WorstPieceB], Wall.CellOf[Result.WorstPieceB]);
	};

	auto WorstJointIs = [](
		const FWall& Wall, const FWallResult& Result,
		int32 CourseA, double CellA, int32 CourseB, double CellB)
	{
		return Result.WorstPieceA != INDEX_NONE
			&& Wall.CourseOf[Result.WorstPieceA] == CourseA
			&& Wall.CourseOf[Result.WorstPieceB] == CourseB
			&& FMath::IsNearlyEqual(Wall.CellOf[Result.WorstPieceA], CellA, 1.0e-9)
			&& FMath::IsNearlyEqual(Wall.CellOf[Result.WorstPieceB], CellB, 1.0e-9);
	};

	TestTrue(
		*FString::Printf(
			TEXT("THE GOVERNING JOINT of case 7 must be c2/8-c3/7.5, the bed joint in the RIGHT JAMB ")
			TEXT("one course below the head of the opening, or the magnitude below is right about the ")
			TEXT("wrong joint and the pair is no longer comparing one mechanism; it is %s"),
			*DescribeWorst(NarrowWall, NarrowResult)),
		WorstJointIs(NarrowWall, NarrowResult, 2, 8.0, 3, 7.5));

	TestTrue(
		*FString::Printf(
			TEXT("THE GOVERNING JOINT of case 9 must be c3/11.5-c4/11, the bed joint AT the head where ")
			TEXT("the first course of cover lands on the RIGHT JAMB — one course higher than case 7's, ")
			TEXT("which the pair's header argues about at length; it is %s"),
			*DescribeWorst(WideWall, WideResult)),
		WorstJointIs(WideWall, WideResult, 3, 11.5, 4, 11.0));

	/* --- the magnitudes, pinned as measured -------------------------------------------------- */

	constexpr double NarrowWorst = 0.26944083288948872;
	constexpr double WideWorst = 0.98502040901419818;

	TestTrue(
		*FString::Printf(
			TEXT("CHARACTERISATION: case 7's jamb reads %.17g and the fixture reads %.17g. This is a ")
			TEXT("measured number, not a derived one — the whole-wall composite walk produces it — and ")
			TEXT("it is pinned because a 2x drift in it moves no verdict and would otherwise pass the ")
			TEXT("suite in silence"),
			NarrowWorst, NarrowResult.Worst),
		FMath::IsNearlyEqual(NarrowResult.Worst, NarrowWorst, UtilisationTolerance));

	TestTrue(
		*FString::Printf(
			TEXT("CHARACTERISATION: case 9's jamb reads %.17g and the fixture reads %.17g"),
			WideWorst, WideResult.Worst),
		FMath::IsNearlyEqual(WideResult.Worst, WideWorst, UtilisationTolerance));

	/*
	 * AND THE CROSSING IS WATCHED SEPARATELY FROM THE VALUE. The pin above already fails if the
	 * reading moves at all, but it fails in the words of a drift; this one fails in the words of the
	 * event that matters, so whoever reads the log is told what it MEANS rather than only that a
	 * number changed.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("THE FLAP WATCH: case 9's jamb reads %.17g and must stay UNDER 1.0. It sits within ")
			TEXT("1.5%% of capacity, so one retune of mortar, density or the composite walk puts it ")
			TEXT("over — and the day it does, this wall drops its head, case 9's 2026-08-12 STANDS ")
			TEXT("ruling becomes a catalogue red and the oracle sweep's pinned AgreeStands relation ")
			TEXT("flips, with nothing physical having changed. That is a re-derivation, not a retune"),
			WideResult.Worst),
		WideResult.Worst < 1.0);

	/* --- the pair's own claim: span is still measured, and at least linearly ------------------ */

	/*
	 * THE CLEAR SPANS, COMPUTED FROM THE CELL GRID RATHER THAN QUOTED. A gap of n cell pitches
	 * between the centres of the surviving bricks either side of an opening leaves
	 * n * CellPitch - BrickLength of open air. Each toothed reveal gives two values, since the even
	 * and odd courses stop at different cells, and the mean of the two is what the beam spans on
	 * average.
	 */
	auto MeanClearSpanCm = [](double EvenCellGap, double OddCellGap)
	{
		return 0.5
			* ((EvenCellGap * CellPitchCm - BrickLengthCm) + (OddCellGap * CellPitchCm - BrickLengthCm));
	};

	/* Case 7 cuts cells 4..7 of the even courses and 4.5..6.5 of the odd ones. */
	const double NarrowSpanCm = MeanClearSpanCm(8.0 - 3.0, 7.5 - 3.5);

	/* Case 9 cuts cells 2..11 of the even courses and 2.5..10.5 of the odd ones. */
	const double WideSpanCm = MeanClearSpanCm(12.0 - 1.0, 11.5 - 1.5);

	const double SpanRatio = WideSpanCm / NarrowSpanCm;

	const double ReadingRatio =
		NarrowResult.Worst > 0.0 ? WideResult.Worst / NarrowResult.Worst : 0.0;

	TestTrue(
		*FString::Printf(
			TEXT("SPAN: a wider opening must read HARDER at its jamb — %.8g clear against %.8g reads ")
			TEXT("%.8g against %.8g. A model with no span term reads them the same."),
			NarrowSpanCm, WideSpanCm, NarrowResult.Worst, WideResult.Worst),
		WideResult.Worst > NarrowResult.Worst);

	/*
	 * AT LEAST LINEARLY IN THE SPAN, NOT MERELY MORE. A strict inequality alone is satisfiable by a
	 * last-bit difference, which is not a span term working. The reaction a deep beam delivers into
	 * its abutment is w.L/2 with w identical in the two fixtures, so the jamb reading has to grow at
	 * least as fast as the span does — 2.6928x here — and the measured separation is 3.6558x.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("SPAN: and at least LINEARLY — %.8g cm of clear opening against %.8g cm is a span ")
			TEXT("ratio of %.8g, the readings separate by %.8g, and the reaction w.L/2 a deep beam ")
			TEXT("delivers into its abutment grows linearly with L at constant w. A reading that grew ")
			TEXT("more slowly than the span would be a span term measured too weakly to trust"),
			NarrowSpanCm, WideSpanCm, SpanRatio, ReadingRatio),
		NarrowResult.Worst > 0.0 && ReadingRatio >= SpanRatio);

	return true;
}

/**
 * CASES 13 AND 14, THE PART A VERDICT CANNOT SAY: DOUBLING THE CORBEL STEP DOUBLES THE JOINT
 * READING, AND BOTH CORBELS STAND ANYWAY.
 *
 * WHY THIS TEST EXISTS AT ALL. 13 and 14 were an OUTCOME pair — one stands, one collapses — until
 * the user ruled on 2026-08-07 that a bonded corbel resists with its full depth and case 14's
 * collapse verdict was wrong. Both halves now stand. A pair whose two halves answer identically
 * discriminates NOTHING, and the honest options were to delete it or to move it onto a quantity
 * that still separates the two. This is that move, and it is the stronger of the two readings: an
 * outcome pair could only ever say "these differ", while this says BY HOW MUCH AND WHY.
 *
 * THE EXPECTED VALUE IS DERIVED FROM THE FIXTURE, NOT READ BACK OFF THE SOLVER. Case 14's four
 * corbelled courses each keep one 10.25 x 10.25 bed patch with their mass 5.625 cm outboard of it,
 * so the load ladder, the moment ladder and the deep-beam section are all written above from the
 * grid, the published density and the published f_xk1 — none of them imported from production.
 * The whole point of the row is that a wrong constant makes this DISAGREE.
 *
 * AND THE DERIVATION IS CROSS-CHECKED AGAINST TWO FIGURES THIS FILE DID NOT PRODUCE, which is what
 * stops it agreeing with itself. The same four lines give 0.21858 at five steps — ARCHING_DESIGN's
 * published 0.219 — and 0.36903147272727271 at eleven, which is the staircase anchor
 * `Core.Structure.AStaircaseVoidCondemnsTheCorbel` pins to seventeen digits. Those are held at 2%
 * and at 1e-9 respectively, IN THE TEST, so a re-derivation that drifted fails here rather than
 * being quietly tuned into agreement.
 *
 * THE THING BEING MEASURED MUST BE THE THING GOVERNING, WHICH IS ASSERTED SEPARATELY. `Worst` is
 * the worst joint ANYWHERE in the wall, and a ten-course wall's base compression or a head joint
 * could in principle carry it; then the number would be right for the wrong joint. So the worst
 * joint is asserted to be the bed joint under the LOWEST corbelled course — course 5's half bat at
 * cell 7.25 carrying course 6's projecting brick at cell 7.5 — before its magnitude is read.
 *
 * GREEN ON ARRIVAL, AND SAID SO PLAINLY. This pins behaviour arching slice 5 already produces; it
 * drove nothing. What it is for is that the ruling above deleted an assertion, and an assertion
 * deleted without one put back in its place is a hole in the net.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceCorbelProjectionTest,
	"DestructionGame.Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceCorbelProjectionTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/*
	 * SLACK AGAINST A RE-ASSOCIATION OF A HANDFUL OF DOUBLES AND NOTHING ELSE. One course of depth
	 * either way moves the four-step reading from 0.1735 to 0.2186, so this is seven orders of
	 * magnitude below the smallest difference the row is meant to be able to see.
	 */
	constexpr double UtilisationTolerance = 1.0e-9;

	/* --- the derivation, cross-checked against two figures produced elsewhere ---------------- */

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: five steps should reproduce ARCHING_DESIGN's published 0.219, the ")
			TEXT("closed form gives %.8g"),
			CorbelBottomRungUtilisation(5)),
		FMath::IsNearlyEqual(CorbelBottomRungUtilisation(5), 0.219, 0.02 * 0.219));

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: eleven steps should reproduce the staircase anchor 0.36903147272727271, ")
			TEXT("the closed form gives %.17g"),
			CorbelBottomRungUtilisation(11)),
		FMath::IsNearlyEqual(
			CorbelBottomRungUtilisation(11), 0.36903147272727271, UtilisationTolerance));

	/* --- the two walls -------------------------------------------------------------------- */

	const TArray<FWallCase> Cases = AllWallCases();

	auto RunNumbered = [this, &Cases](int32 Number) -> const FSolvedWall*
	{
		for (const FWallCase& Case : Cases)
		{
			if (Case.Number != Number)
			{
				continue;
			}

			const FSolvedWall& Solved = RunWallCase(*this, Case);
			ReportWallCase(*this, Case, Solved.Wall, Solved.Result);

			return Solved.Result.bLaid ? &Solved : nullptr;
		}

		AddError(FString::Printf(TEXT("FIXTURE: the catalogue has no case %d"), Number));

		return nullptr;
	};

	/* Short-circuited deliberately: a case 13 that could not be laid says nothing about case 14. */
	const FSolvedWall* const Quarter = RunNumbered(13);
	const FSolvedWall* const Half = Quarter == nullptr ? nullptr : RunNumbered(14);

	if (Half == nullptr)
	{
		return true;
	}

	const FWall& QuarterWall = Quarter->Wall;
	const FWallResult& QuarterResult = Quarter->Result;

	const FWall& HalfWall = Half->Wall;
	const FWallResult& HalfResult = Half->Result;

	/* --- neither corbel comes down, which is the ruling ------------------------------------- */

	TestEqual(
		*FString::Printf(
			TEXT("THE RULING: a quarter-brick corbel stands, %d piece(s) came down %s"),
			QuarterResult.Fallen.Num(), *DescribePieces(QuarterWall, QuarterResult.Fallen)),
		QuarterResult.Fallen.Num(), 0);

	TestEqual(
		*FString::Printf(
			TEXT("THE RULING: a HALF-brick corbel stands too — that is the 2026-08-07 correction to ")
			TEXT("case 14 — and %d piece(s) came down %s"),
			HalfResult.Fallen.Num(), *DescribePieces(HalfWall, HalfResult.Fallen)),
		HalfResult.Fallen.Num(), 0);

	/* --- and the reading is the four-step ladder on the four courses standing over it -------- */

	const bool bWorstIsTheBottomRung =
		HalfResult.WorstPieceA != INDEX_NONE
		&& HalfWall.CourseOf[HalfResult.WorstPieceA] == CorbelFirstCourse - 1
		&& HalfWall.CourseOf[HalfResult.WorstPieceB] == CorbelFirstCourse
		&& FMath::IsNearlyEqual(HalfWall.CellOf[HalfResult.WorstPieceB], 7.5, 1.0e-9);

	TestTrue(
		*FString::Printf(
			TEXT("THE GOVERNING JOINT must be the bed joint under the lowest corbelled course, c%d/7.25 ")
			TEXT("carrying c%d/7.5, or the magnitude below is right about the wrong joint; it is %s"),
			CorbelFirstCourse - 1, CorbelFirstCourse,
			HalfResult.WorstPieceA == INDEX_NONE
				? TEXT("no joint at all")
				: *FString::Printf(
					TEXT("c%d/%g-c%d/%g"),
					HalfWall.CourseOf[HalfResult.WorstPieceA], HalfWall.CellOf[HalfResult.WorstPieceA],
					HalfWall.CourseOf[HalfResult.WorstPieceB], HalfWall.CellOf[HalfResult.WorstPieceB])),
		bWorstIsTheBottomRung);

	const double ExpectedHalf = CorbelBottomRungUtilisation(4);

	TestTrue(
		*FString::Printf(
			TEXT("A FOUR-STEP HALF-BRICK CORBEL reads its bottom rung at %.17g of f_xk1 — 112.5 ")
			TEXT("brick-weight-cm over the 1537.5 cm3 of four courses acting together — and the wall ")
			TEXT("reads %.17g"),
			ExpectedHalf, HalfResult.Worst),
		FMath::IsNearlyEqual(HalfResult.Worst, ExpectedHalf, UtilisationTolerance));

	/*
	 * AND THE OUTCOME FOLLOWS FROM THE NUMBER RATHER THAN THE OTHER WAY ROUND. 0.195 is a fifth of
	 * capacity; the verdict in the catalogue is "stands" BECAUSE of this, not beside it.
	 *
	 * THE `Worst < 1.0` ROW THAT USED TO SAY SO WAS DELETED ON 2026-08-09 AS A TAUTOLOGY, and it is
	 * worth writing down why so nobody puts it back thinking the claim went with it. `Worst` is
	 * `WorstUtilisation`, which SKIPS every joint that has given and reads the rest through
	 * `GetConnectionUtilisation`; `RunWallCase` gets it from `SolveAndBreak`, whose last pass is by
	 * definition one that broke nothing — that pass evaluated every surviving joint through
	 * `FConnection::ApplyForce`, on the same three solver arrays this accessor reads, and none of
	 * them latched. Giving is spelled `!(u <= 1.0)`, so EVERY joint the maximum can be taken over
	 * is already known to be at most 1.0 and the assertion could only ever have failed on a joint
	 * sitting at exactly 1.0 — which the exact-value assertion above, pinning 0.195160875 to 1e-9,
	 * would have failed on first and far more loudly. What makes the verdict follow from the number
	 * is that assertion, not a bound the cascade already guarantees.
	 */

	/* --- the pair's own claim: projection is still measured --------------------------------- */

	TestTrue(
		*FString::Printf(
			TEXT("CORBEL STEP: doubling the projection per course must read HARDER on the joint — a ")
			TEXT("quarter brick reads %.8g and a half brick reads %.8g, a factor of %.4g. A model ")
			TEXT("with no projection term reads them the same."),
			QuarterResult.Worst, HalfResult.Worst,
			QuarterResult.Worst > 0.0 ? HalfResult.Worst / QuarterResult.Worst : 0.0),
		HalfResult.Worst > QuarterResult.Worst);

	/*
	 * TWICE, NOT MERELY MORE. A strict inequality alone is satisfiable by a last-bit difference,
	 * which is not a projection term working; the step doubles, the arm doubles with it, and the
	 * measured separation is a factor of 2.8. Two is the floor and it is a long way under that.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("CORBEL STEP: and by a real margin, not the last bit — %.8g against %.8g is a ")
			TEXT("factor of %.4g and must be at least 2"),
			QuarterResult.Worst, HalfResult.Worst,
			QuarterResult.Worst > 0.0 ? HalfResult.Worst / QuarterResult.Worst : 0.0),
		QuarterResult.Worst > 0.0 && HalfResult.Worst >= 2.0 * QuarterResult.Worst);

	return true;
}

/**
 * CASES 15 AND 16, THE PART A VERDICT CANNOT SAY: PUTTING SIX COURSES ON A HEADER'S TAIL DIVIDES
 * WHAT ITS OWN BED JOINT READS BY THIRTY, AND BOTH HEADERS STAND ANYWAY.
 *
 * WHY THIS TEST EXISTS AT ALL. 15 and 16 were an OUTCOME pair — one stands, one drops its header —
 * until the catalogue was revised on 2026-08-07 and case 16 became "stands" too: an uncracked
 * bonded bed joint carries a half-cell projection at 0.0582 of f_xk1, and the local-loss verdict it
 * replaced was a rigid-body overturning reading that ignored the bond. That is the same correction
 * case 14 got on the same day, and it has the same consequence — a pair whose two halves answer
 * identically discriminates NOTHING. The honest options were to delete the pair or to move it onto
 * a quantity that still separates the two. This is that move, and it is the stronger reading: an
 * outcome pair could only ever have said "these differ", while this says BY HOW MUCH AND WHY.
 *
 * THE TWO WALLS ARE THE SAME WALL WITH THE HEADER AT A DIFFERENT HEIGHT — ten courses, twelve
 * cells, one course pushed half a cell out and closed with a full brick instead of a half bat. In
 * case 15 that course is course 3, so six courses stand on the header's tail; in case 16 it is
 * course 9, the top, so nothing does. THE GEOMETRY OF THE HEADER'S OWN BED JOINT IS IDENTICAL in
 * the two, which is what makes this a one-variable comparison: same 10.25 x 10.25 cm bearing patch,
 * same 5.625 cm arm, same brick. Only the superimposed load differs.
 *
 * THE EXPECTED VALUE IS DERIVED FROM THE FIXTURE, NOT READ BACK OFF THE SOLVER. The seat length is
 * what is left of a 21.5 cm brick when half a 22.5 cm cell hangs over air; the arm is half of what
 * hangs over; the section is that patch's own t.L^2/6; the weight is the published density times
 * the published dimensions times Unreal's gravity; and the capacity is EN 1996-1-1's f_xk1. None of
 * them is imported from production, so a wrong constant there makes this DISAGREE.
 *
 * AND IT IS CROSS-CHECKED AGAINST TWO FIGURES THIS FILE DID NOT PRODUCE, which is what stops it
 * agreeing with itself: WALL_CASES.html's own quoted 0.058204 for this case, held at 1e-6 so the
 * catalogue's six figures are enough and a real error is not; and the waist anchor
 * 0.058203838191552663 that `Core.Structure.AdoptedWallLoadsItsWaistEccentrically` pins to
 * seventeen digits on a completely different fixture — one brick, half seated, by cut rather than
 * by laying. A re-derivation that drifted fails HERE rather than being tuned into agreement.
 *
 * THE THING BEING MEASURED MUST BE THE THING GOVERNING, WHICH IS ASSERTED SEPARATELY. `Worst` is
 * the worst joint ANYWHERE in the wall, and a ten-course wall's base compression could in principle
 * carry it — case 17's intact stack-bond wall of the same height reads 0.00109 at its foot — so the
 * number would then be right for the wrong joint. Case 16's worst joint is therefore asserted to BE
 * the header's own bed joint, by (course, cell), before its magnitude is read.
 *
 * CASE 15's GOVERNING JOINT IS DELIBERATELY *NOT* PINNED, and that is not laziness. What is claimed
 * of case 15 is an UPPER bound — the worst joint anywhere in that wall is at most a tenth of case
 * 16's — and an upper bound over every joint says something strictly stronger about the header's
 * own joint than pinning where the maximum happens to sit. It also stays true if a later slice
 * drives the header's tension further down and the maximum migrates to the foot of the wall, which
 * pinning the identity would turn into a spurious failure. (It reads 0.00184 at c2/11-c3/11.5
 * today, which is the header's bed joint, and the failure message prints where it actually is.)
 *
 * GREEN ON ARRIVAL, AND SAID SO PLAINLY. This pins behaviour the model already produces; it drove
 * nothing. What it is for is that the case 16 revision deleted an assertion, and an assertion
 * deleted without one put back in its place is a hole in the net.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceSuperimposedLoadTest,
	"DestructionGame.Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceSuperimposedLoadTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/*
	 * SLACK AGAINST A RE-ASSOCIATION OF A HANDFUL OF DOUBLES AND NOTHING ELSE. The quantity is
	 * 0.058, so this is seven orders of magnitude below it and eight below the factor of thirty the
	 * row is meant to be able to see.
	 */
	constexpr double UtilisationTolerance = 1.0e-9;

	/* --- the derivation, worked forward from the grid ---------------------------------------- */

	/*
	 * WHAT THE HEADER KEEPS AND WHAT HANGS OVER. CourseGeometry pushes the projecting course's
	 * right face out by HALF A CELL and closes it with a FULL brick rather than the half bat a
	 * flush odd course closes with, so of the brick's 21.5 cm exactly 11.25 cm is over air and
	 * 10.25 cm is still bearing on the course below. (That it lands on the same 10.25 cm as the
	 * wall's depth is a coincidence of this brick's proportions, not a relation — it is written as
	 * a length along the wall because that is what it is.)
	 */
	constexpr double HeaderSeatLengthCm = BrickLengthCm - HalfCellCm;

	/* The mass sits at the middle of the brick, so it is half the unseated part outboard. */
	constexpr double HeaderArmCm = (BrickLengthCm - HeaderSeatLengthCm) * 0.5;

	constexpr double HeaderSeatAreaSqCm = HeaderSeatLengthCm * BrickDepthCm;

	/* W = t.L^2/6 about the axis the header bends about: through the wall, along its length. */
	constexpr double HeaderSeatModulusCm3 =
		BrickDepthCm * HeaderSeatLengthCm * HeaderSeatLengthCm / 6.0;

	const double BendingMPa = FullBrickWeightUu * HeaderArmCm
		/ (HeaderSeatModulusCm3 * ForceUnitsPerMPaSqCmHere);

	const double OwnWeightMPa = FullBrickWeightUu
		/ (HeaderSeatAreaSqCm * ForceUnitsPerMPaSqCmHere);

	/* Its own weight CLOSES the joint the bending opens, so the two subtract. */
	const double ExpectedUnloaded =
		FMath::Max(0.0, BendingMPa - OwnWeightMPa) / MortarFlexuralBondMPa;

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: WALL_CASES.html quotes 0.058204 for case 16, the derivation gives ")
			TEXT("%.8g"),
			ExpectedUnloaded),
		FMath::IsNearlyEqual(ExpectedUnloaded, 0.058204, 1.0e-6));

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: the same arithmetic on a CUT half-seated brick is the waist anchor ")
			TEXT("0.058203838191552663, the derivation gives %.17g"),
			ExpectedUnloaded),
		FMath::IsNearlyEqual(ExpectedUnloaded, 0.058203838191552663, UtilisationTolerance));

	/* --- the two walls ---------------------------------------------------------------------- */

	const TArray<FWallCase> Cases = AllWallCases();

	auto RunNumbered = [this, &Cases](int32 Number) -> const FSolvedWall*
	{
		for (const FWallCase& Case : Cases)
		{
			if (Case.Number != Number)
			{
				continue;
			}

			const FSolvedWall& Solved = RunWallCase(*this, Case);
			ReportWallCase(*this, Case, Solved.Wall, Solved.Result);

			return Solved.Result.bLaid ? &Solved : nullptr;
		}

		AddError(FString::Printf(TEXT("FIXTURE: the catalogue has no case %d"), Number));

		return nullptr;
	};

	/* Short-circuited deliberately: a case 15 that could not be laid says nothing about case 16. */
	const FSolvedWall* const Loaded = RunNumbered(15);
	const FSolvedWall* const Bare = Loaded == nullptr ? nullptr : RunNumbered(16);

	if (Bare == nullptr)
	{
		return true;
	}

	const FWall& LoadedWall = Loaded->Wall;
	const FWallResult& LoadedResult = Loaded->Result;

	const FWall& BareWall = Bare->Wall;
	const FWallResult& BareResult = Bare->Result;

	/* --- neither header comes down, which is the revision ------------------------------------ */

	TestEqual(
		*FString::Printf(
			TEXT("THE CATALOGUE: a header with six courses on its tail stands, %d piece(s) came ")
			TEXT("down %s"),
			LoadedResult.Fallen.Num(), *DescribePieces(LoadedWall, LoadedResult.Fallen)),
		LoadedResult.Fallen.Num(), 0);

	TestEqual(
		*FString::Printf(
			TEXT("THE CATALOGUE: a header with NOTHING on its tail stands too — that is the ")
			TEXT("2026-08-07 revision to case 16 — and %d piece(s) came down %s"),
			BareResult.Fallen.Num(), *DescribePieces(BareWall, BareResult.Fallen)),
		BareResult.Fallen.Num(), 0);

	/* --- and the reading is the header's own bed joint ---------------------------------------- */

	/*
	 * WHERE THE HEADER AND ITS SEAT ARE, IN CELLS, WALKED OFF THE BRICKLAYER RATHER THAN COUNTED
	 * OFF THE LOG. The projecting course's rightmost brick is a full brick whose right face is half
	 * a cell past the flush face; the course below it is flush and closes with a full brick too,
	 * since it is even. Both centres are half a brick in from their own right faces.
	 */
	const double HeaderCentreCm =
		FlushRightFaceCm(StandardCells) + HalfCellCm - BrickLengthCm * 0.5;

	const double SeatCentreCm = FlushRightFaceCm(StandardCells) - BrickLengthCm * 0.5;

	const double HeaderCell = HeaderCentreCm / CellPitchCm;
	const double SeatCell = SeatCentreCm / CellPitchCm;

	/* Case 16 pushes course 9 out, so its seat is the end brick of course 8. */
	constexpr int32 BareHeaderCourse = 9;

	const bool bWorstIsTheHeaderSeat =
		BareResult.WorstPieceA != INDEX_NONE
		&& BareWall.CourseOf[BareResult.WorstPieceA] == BareHeaderCourse - 1
		&& BareWall.CourseOf[BareResult.WorstPieceB] == BareHeaderCourse
		&& FMath::IsNearlyEqual(BareWall.CellOf[BareResult.WorstPieceA], SeatCell, 1.0e-9)
		&& FMath::IsNearlyEqual(BareWall.CellOf[BareResult.WorstPieceB], HeaderCell, 1.0e-9);

	TestTrue(
		*FString::Printf(
			TEXT("THE GOVERNING JOINT must be the header's own bed joint, c%d/%g carrying c%d/%g, ")
			TEXT("or the magnitude below is right about the wrong joint; it is %s"),
			BareHeaderCourse - 1, SeatCell, BareHeaderCourse, HeaderCell,
			BareResult.WorstPieceA == INDEX_NONE
				? TEXT("no joint at all")
				: *FString::Printf(
					TEXT("c%d/%g-c%d/%g"),
					BareWall.CourseOf[BareResult.WorstPieceA], BareWall.CellOf[BareResult.WorstPieceA],
					BareWall.CourseOf[BareResult.WorstPieceB], BareWall.CellOf[BareResult.WorstPieceB])),
		bWorstIsTheHeaderSeat);

	TestTrue(
		*FString::Printf(
			TEXT("A HEADER WITH NOTHING ON IT reads its bed joint at %.17g of f_xk1 — 0.0083591 MPa ")
			TEXT("of bending less 0.0025387 MPa of its own weight closing the joint — and the wall ")
			TEXT("reads %.17g"),
			ExpectedUnloaded, BareResult.Worst),
		FMath::IsNearlyEqual(BareResult.Worst, ExpectedUnloaded, UtilisationTolerance));

	/*
	 * AND THE OUTCOME FOLLOWS FROM THE NUMBER RATHER THAN THE OTHER WAY ROUND. 0.058 is a sixth of
	 * capacity; the verdict in the catalogue is "stands" BECAUSE of this, not beside it.
	 *
	 * THE `Worst < 1.0` ROW THAT USED TO SAY SO WAS DELETED ON 2026-08-09 AS A TAUTOLOGY, for the
	 * reason written out in full beside case 14's copy of it in
	 * Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome: `SolveAndBreak` returns only
	 * when a pass breaks nothing, so every joint `WorstUtilisation` is allowed to look at has
	 * already been evaluated at most 1.0 by the break sweep itself. What makes the verdict follow
	 * from the number is the exact-value assertion above, which pins 0.058203838191552663 to 1e-9.
	 */

	/* --- the pair's own claim: superimposed load is still measured ---------------------------- */

	TestTrue(
		*FString::Printf(
			TEXT("SUPERIMPOSED LOAD: six courses standing on the header's tail must read EASIER on ")
			TEXT("the joint — loaded reads %.8g (worst at %s) and bare reads %.8g, a factor of ")
			TEXT("%.4g. A model with no superimposed-load term reads them the same, and one that ")
			TEXT("counted the extra load without its compression reads the loaded wall HIGHER."),
			LoadedResult.Worst,
			LoadedResult.WorstPieceA == INDEX_NONE
				? TEXT("no joint at all")
				: *FString::Printf(
					TEXT("c%d/%g-c%d/%g"),
					LoadedWall.CourseOf[LoadedResult.WorstPieceA],
					LoadedWall.CellOf[LoadedResult.WorstPieceA],
					LoadedWall.CourseOf[LoadedResult.WorstPieceB],
					LoadedWall.CellOf[LoadedResult.WorstPieceB]),
			BareResult.Worst,
			LoadedResult.Worst > 0.0 ? BareResult.Worst / LoadedResult.Worst : 0.0),
		BareResult.Worst > LoadedResult.Worst);

	/*
	 * TEN TIMES, NOT MERELY MORE. A strict inequality alone is satisfiable by a last-bit
	 * difference, which is not a superimposed-load term working; the measured separation is a
	 * factor of 32, with the loaded joint's tension driven to exactly zero and compression left
	 * governing. Ten is the floor and the measurement is three times clear of it — and it is
	 * headroom in BOTH directions, because the ceiling is the ten-course wall's own base
	 * compression at 0.00109, which caps the achievable factor at about 53.
	 *
	 * NOTE WHAT THIS IS A BOUND ON. `Worst` for case 15 is the worst joint ANYWHERE in that wall,
	 * so bounding it above bounds the header's own joint above by the same number, whichever joint
	 * happens to be carrying the maximum. That is the direction that makes the claim safe against a
	 * later slice relieving the header further.
	 */
	constexpr double MinimumSeparation = 10.0;

	TestTrue(
		*FString::Printf(
			TEXT("SUPERIMPOSED LOAD: and by a real margin, not the last bit — %.8g against %.8g is ")
			TEXT("a factor of %.4g and must be at least %g"),
			LoadedResult.Worst, BareResult.Worst,
			LoadedResult.Worst > 0.0 ? BareResult.Worst / LoadedResult.Worst : 0.0,
			MinimumSeparation),
		LoadedResult.Worst > 0.0 && BareResult.Worst >= MinimumSeparation * LoadedResult.Worst);

	return true;
}

/**
 * CASE 18, THE PART A VERDICT CANNOT SAY: A HANGING STACK-BOND COLUMN CARRIES THE SAME FRACTION OF
 * ITS HEAD JOINTS' SHEAR CAPACITY AT ANY HEIGHT.
 *
 * WHY THIS IS ITS OWN TEST. Case 18's verdict is "stands", and stands is easy — the column sits at
 * a few percent of capacity, so a wall that got the load path completely wrong would still pass
 * the catalogue row. The finding that overturned this case's drafted verdict was ARITHMETIC, and
 * the property that makes it trustworthy is that the ratio does not move with height: every course
 * added to the column brings one brick of weight and two more head joints, and those cancel
 * exactly. TWO HEIGHTS SAYING THE SAME NUMBER IS A FAR STRONGER CLAIM THAN ONE HEIGHT SAYING IT,
 * because a model that simply piles the whole column onto the joints at its foot also reads a
 * plausible few percent — and grows linearly with height, which one height cannot see.
 *
 * THE ARITHMETIC, WORKED INDEPENDENTLY. A full brick weighs 1.9 x 21.5 x 10.25 x 6.5 / 1000 kg
 * x 980 cm/s2 = 2667.198625 Unreal force units. In stack bond a brick whose bed joint has been cut
 * away is bonded to a neighbour on each side over a head joint of 10.25 x 6.5 = 66.625 cm2, and
 * the load is parallel to both, so it is shear, split by area between two equal joints:
 *
 *     2667.198625 / (2 x 66.625) / 10000 uu per MPa.cm2  =  0.00200165 MPa
 *     0.00200165 / 0.2 MPa (f_vk0)                       =  0.01000825
 *
 * There is no friction to add: a vertical load on a vertical joint puts no compression across it,
 * so the capacity is bare cohesion.
 *
 * AND THE NUMBER DISAGREES WITH THE ONE IN THE BRIEF, WHICH IS WORTH SAYING OUT LOUD. WALL_CASES
 * and CURRENT_STATE both quote 0.0200 for this case, reached by dividing the same load over the
 * same two joints by 0.1 MPa — but 0.1 is f_xk1, the FLEXURAL BOND strength, and the joint here is
 * in shear against f_vk0 = 0.2. The repo's own 0.0200165 (Tests/ConnectionStrengthTest.cpp) is a
 * different quantity again: one brick weight over ONE head joint, which is MOMENTS_DESIGN case
 * (b)'s fixture, and it coincides only because the two errors are each a factor of two. The value
 * asserted here is the one that follows from the published strengths this project actually ships.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceStackBondHeightTest,
	"DestructionGame.Acceptance.Wall.StackBondColumnShearIsHeightIndependent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceStackBondHeightTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	constexpr double HangingColumnShearUtilisation =
		FullBrickWeightUu / (2.0 * HeadJointAreaSqCm) / ForceUnitsPerMPaSqCmHere
		/ MortarShearCohesionMPa;

	/*
	 * Slack against a re-association of a handful of doubles, not against a wrong answer. The
	 * value is about 0.01 and the model's answer today is expected to be several times it, so this
	 * is four orders of magnitude of headroom on the thing being distinguished.
	 */
	constexpr double UtilisationTolerance = 1e-9;

	/*
	 * TWO HEIGHTS, THE SAME CUT. Ten courses leaves four bricks hanging above the hole and sixteen
	 * leaves ten, so a model that concentrates the column on the joints at its foot reads two and a
	 * half times as much at the taller wall while the correct answer does not move at all.
	 */
	const int32 Heights[] = { 10, 16 };

	constexpr int32 HeightCount = static_cast<int32>(UE_ARRAY_COUNT(Heights));

	double Measured[HeightCount] = { 0.0, 0.0 };
	bool bRan[HeightCount] = { false, false };

	for (int32 Index = 0; Index < HeightCount; ++Index)
	{
		FWallCase Case;
		Case.Number = 18;
		Case.Title = TEXT("Stack bond, one brick out");
		Case.Verdict = EVerdict::Stands;
		Case.Courses = Heights[Index];
		Case.Cells = 12;
		Case.Bond = EBond::Stack;
		Case.Cuts = Case18Cuts;

		const FSolvedWall& Solved = RunWallCase(*this, Case);

		const FWall& Wall = Solved.Wall;
		const FWallResult& Result = Solved.Result;

		ReportWallCase(*this, Case, Wall, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		bRan[Index] = true;
		Measured[Index] = Result.Worst;

		/*
		 * THE COLUMN MUST ACTUALLY BE HANGING, or the number below is measuring an ordinary bed
		 * joint and the whole test is vacuous. The worst joint in a hanging stack-bond wall is a
		 * head joint: the bottom bed joint of a sixteen-course column reads about 0.0018 against
		 * 10 MPa, which is a fiftieth of what a head joint reads under the same load.
		 */
		TestEqual(
			*FString::Printf(
				TEXT("%d courses: the wall must still stand, %d piece(s) came down %s"),
				Heights[Index], Result.Fallen.Num(), *DescribePieces(Wall, Result.Fallen)),
			Result.Fallen.Num(), 0);

		TestTrue(
			*FString::Printf(
				TEXT("%d courses: the worst joint should be a HEAD joint of the hanging column, ")
				TEXT("it is c%d/%g-c%d/%g"),
				Heights[Index],
				Wall.CourseOf[Result.WorstPieceA], Wall.CellOf[Result.WorstPieceA],
				Wall.CourseOf[Result.WorstPieceB], Wall.CellOf[Result.WorstPieceB]),
			Result.WorstPieceA != INDEX_NONE
				&& Wall.CourseOf[Result.WorstPieceA] == Wall.CourseOf[Result.WorstPieceB]);

		TestTrue(
			*FString::Printf(
				TEXT("%d courses: the hanging column should read %.8g of head-joint shear ")
				TEXT("capacity, it reads %.8g"),
				Heights[Index], HangingColumnShearUtilisation, Result.Worst),
			FMath::IsNearlyEqual(Result.Worst, HangingColumnShearUtilisation, UtilisationTolerance));
	}

	if (bRan[0] && bRan[1])
	{
		/*
		 * THE PROPERTY, ASSERTED WITHOUT REFERENCE TO EITHER VALUE. This holds even if the
		 * absolute figure above is wrong, and it is the half that a model piling the column onto
		 * its foot cannot satisfy however it is tuned.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("HEIGHT-INDEPENDENT: %d courses reads %.8g and %d courses reads %.8g, a ")
				TEXT("factor of %.4g"),
				Heights[0], Measured[0], Heights[1], Measured[1],
				Measured[0] > 0.0 ? Measured[1] / Measured[0] : 0.0),
			FMath::IsNearlyEqual(Measured[0], Measured[1], UtilisationTolerance));
	}

	return true;
}

/**
 * THE FIXTURE'S OWN BRICKLAYER LAYS THE WALL Layout::RunningBond LAYS.
 *
 * WITHOUT THIS THE WHOLE FILE IS A SECOND DEFINITION OF WHAT A WALL IS. Nineteen expected outcomes
 * measured against a wall that is subtly not the game's wall would all be measuring the wrong
 * thing, plausibly, and every one of them would still read as a number. The fixture exists only
 * because six cases are not running-bond rectangles; on the shape both producers CAN lay they have
 * to agree brick for brick.
 *
 * ON BOXES RATHER THAN ON HANDLES, because the order a producer emits pieces in is its own
 * business and this makes no claim about it — the sets of boxes have to match, and each box has to
 * match somewhere.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceFixtureAgreesWithProducerTest,
	"DestructionGame.Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceFixtureAgreesWithProducerTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace WallAcceptanceTestSupport;

	constexpr int32 Courses = 10;
	constexpr int32 Cells = 12;

	/** Exact arithmetic on both sides; this is slack against a re-association and nothing else. */
	constexpr double PlacementToleranceCm = 1e-6;

	FRunningBondSpec Spec;
	Spec.BrickSizeCm = FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm);
	Spec.JointThicknessCm = JointCm;
	Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
	Spec.CoursesHigh = Courses;
	Spec.BricksPerCourse = Cells;
	Spec.End = EWallEnd::Flush;
	Spec.Strength = GeneralPurposeMortar;

	FBrickLayout Produced;

	if (!TestTrue(TEXT("FIXTURE: RunningBond should lay the reference wall"),
			RunningBond(Spec, Produced)))
	{
		return true;
	}

	FWallCase Case;
	Case.Number = 0;
	Case.Title = TEXT("the reference wall");
	Case.Courses = Courses;
	Case.Cells = Cells;

	FWall Wall;
	LayWall(Case, Wall);

	TestEqual(
		FString::Printf(
			TEXT("the fixture should lay the same number of bricks as RunningBond; it laid %d ")
			TEXT("against %d"),
			Wall.NumPieces(), Produced.Boxes.Num()),
		Wall.NumPieces(), Produced.Boxes.Num());

	TestEqual(
		FString::Printf(
			TEXT("the fixture should emit the same number of joints as RunningBond; it emitted %d ")
			TEXT("against %d"),
			Wall.Structure.NumConnections(), Produced.Structure.NumConnections()),
		Wall.Structure.NumConnections(), Produced.Structure.NumConnections());

	int32 Unmatched = 0;
	FString FirstUnmatched;

	for (int32 Piece = 0; Piece < Wall.NumPieces(); ++Piece)
	{
		bool bFound = false;

		for (const FPieceBox& Reference : Produced.Boxes)
		{
			if (Reference.CentreCm.Equals(Wall.Boxes[Piece].CentreCm, PlacementToleranceCm)
				&& Reference.ExtentCm.Equals(Wall.Boxes[Piece].ExtentCm, PlacementToleranceCm))
			{
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			++Unmatched;

			if (FirstUnmatched.IsEmpty())
			{
				FirstUnmatched = FString::Printf(
					TEXT("c%d/%g centred (%g, %g, %g) half-size (%g, %g, %g)"),
					Wall.CourseOf[Piece], Wall.CellOf[Piece],
					Wall.Boxes[Piece].CentreCm.X, Wall.Boxes[Piece].CentreCm.Y,
					Wall.Boxes[Piece].CentreCm.Z,
					Wall.Boxes[Piece].ExtentCm.X, Wall.Boxes[Piece].ExtentCm.Y,
					Wall.Boxes[Piece].ExtentCm.Z);
			}
		}
	}

	TestEqual(
		FString::Printf(
			TEXT("every brick the fixture lays should be one RunningBond lays; %d were not, the ")
			TEXT("first is %s"),
			Unmatched, FirstUnmatched.IsEmpty() ? TEXT("none") : *FirstUnmatched),
		Unmatched, 0);

	return true;
}

/**
 * THE PRODUCTION BRICKLAYER LAYS EXACTLY THE TWENTY WALLS THIS FILE HAS BEEN MEASURING.
 *
 * =====================================================================================
 * WHY THIS TEST EXISTS AT ALL
 * =====================================================================================
 *
 * The bricklayer above is TEST-ONLY, so no level can reach it — which is why the twenty walls the
 * user drew and reviewed could not be catalogue rows until the same geometry existed in production.
 * Writing that producer was the cheap part. The expensive part is that every reading in this file is
 * a statement about a particular arrangement of particular bricks, worked to seventeen digits:
 * 0.195160875 at a four-step corbel's bottom rung, 0.058203838191552663 at a bare header's bed
 * joint, 0.01000825 of head-joint shear, and three known-red rows whose failure messages name bricks
 * by (course, cell). A producer that differs from the fixture by one ulp anywhere is not "a fixture
 * that moved", it is a dozen tests changing their answers at once for no visible reason — and
 * several are ALREADY RED, so a changed message there would hide a regression inside a known
 * failure.
 *
 * =====================================================================================
 * WHAT IS COMPARED, AND WHY HANDLE ORDER IS IN THE LIST
 * =====================================================================================
 *
 * The piece count, every box centre and extent, every mass, every grounded flag, and the whole
 * connection set IN ORDER — pairing, normal, area, centre and half-extent. Exact `==` throughout,
 * because "bit-identical" is the claim and a tolerance lets exactly the drift this exists to
 * refuse through.
 *
 * A builder that laid the same bricks in a different sequence would renumber every joint and every
 * break stamp while every geometric check still passed. That is the failure that looks like
 * nothing, so it is asserted rather than assumed.
 *
 * AND THE GRID AND THE REGIONS MOVE WITH IT. `FWallRegion` is the vocabulary every cut and every
 * named outcome in this file is written in, and resolving one needs each piece's (course, cell) —
 * so production must hand those back and must name the same bricks the fixture names, or twenty
 * levels cut the wrong bricks while laying the right wall.
 *
 * =====================================================================================
 * IT IS STILL LIVE: THE BRICKLAYER WAS COPIED, NOT MOVED
 * =====================================================================================
 *
 * This block used to say the test went quiet the moment the producer landed, on the reasoning that
 * the fixture would by then be a call to production and the comparison would be a thing against
 * itself — the state the corbel builder's own fixture comparison reached, which was deleted for
 * exactly that reason and replaced by `Core.Corbel.LaysTheJointsTheGridImplies`.
 *
 * THAT NEVER HAPPENED HERE, AND THE NOTE WAS WRONG. `LayWall` above still lays its own bricks and
 * calls nothing in `DestructionWallCases`; the commit that landed the producer is 853 insertions
 * and ZERO deletions in this file. The bricklayer was COPIED into production rather than moved out
 * of the test, which is precisely why not one reading here moved across it.
 *
 * So this compares TWO INDEPENDENT BRICKLAYERS and it is the only test that does. Deleting it as
 * spent would leave one statement about `Core/WallCases`' handle order where there are currently
 * two — after which a change to the joint-discovery loop renumbers every joint index and every
 * break stamp while every geometric check still passes, which is the failure named above as the one
 * that looks like nothing.
 *
 * `Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays` stands BESIDE it rather than in place of
 * it: that one is the claim that this producer lays the wall `Layout::RunningBond` lays on the
 * shape both can lay, and is the reason this file cannot become a second definition of what a wall
 * is.
 *
 * NEEDS A TICKING WORLD: NO. Boxes and doubles; no solve at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceProducerMatchesFixtureTest,
	"DestructionGame.Acceptance.Wall.TheProducerLaysTheWallTheFixtureLays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceProducerMatchesFixtureTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/**
	 * A CELL INDEX IS AN INDEX, NOT A READING, so it is held to a tolerance rather than to a bit.
	 *
	 * Every region bound in this file is at least an eighth of a cell — 0.125 — clear of any brick
	 * centre, so this is eight orders of magnitude below anything that could change which region a
	 * piece falls in. The course number is an integer and is held exactly.
	 */
	constexpr double CellTolerance = 1.0e-9;

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty cases"), Cases.Num(), 20);

	for (const FWallCase& Case : Cases)
	{
		const FString Where = FString::Printf(TEXT("case %d (%s)"), Case.Number, Case.Title);

		FWall Fixture;
		LayWall(Case, Fixture);

		if (Fixture.NumPieces() == 0)
		{
			AddError(FString::Printf(TEXT("%s: FIXTURE laid no bricks at all"), *Where));

			continue;
		}

		DestructionWallCases::FWallLayout Laid;

		if (!DestructionWallCases::Build(ProductionSpecOf(Case), Laid))
		{
			AddError(FString::Printf(
				TEXT("%s: production must lay this wall before it can be compared to the fixture ")
				TEXT("every reading in this file was taken on"),
				*Where));

			continue;
		}

		/* --- the same wall, brick for brick, joint for joint, handle for handle ------------- */

		LayoutMatchesFixture(*this, Where, Laid.Layout, Fixture);

		/* --- and the same grid under it ----------------------------------------------------- */

		int32 FirstWrongGrid = INDEX_NONE;
		FString WhyGridWrong;

		const int32 CommonGrid = FMath::Min(
			FMath::Min(Laid.CourseOf.Num(), Laid.CellOf.Num()), Fixture.NumPieces());

		for (int32 Piece = 0; Piece < CommonGrid; ++Piece)
		{
			if (Laid.CourseOf[Piece] == Fixture.CourseOf[Piece]
				&& FMath::IsNearlyEqual(Laid.CellOf[Piece], Fixture.CellOf[Piece], CellTolerance))
			{
				continue;
			}

			FirstWrongGrid = Piece;

			WhyGridWrong = FString::Printf(
				TEXT("production puts it at c%d/%s, the fixture at c%d/%s"),
				Laid.CourseOf[Piece], *Bits(Laid.CellOf[Piece]),
				Fixture.CourseOf[Piece], *Bits(Fixture.CellOf[Piece]));

			break;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: production must hand back one (course, cell) per piece — %d courses and ")
				TEXT("%d cells for %d pieces"),
				*Where, Laid.CourseOf.Num(), Laid.CellOf.Num(), Laid.Layout.Boxes.Num()),
			Laid.CourseOf.Num() == Laid.Layout.Boxes.Num()
				&& Laid.CellOf.Num() == Laid.Layout.Boxes.Num());

		TestTrue(
			*FString::Printf(
				TEXT("%s: every piece must sit on the same (course, cell) the fixture puts it on, or ")
				TEXT("every region in this file names different bricks — piece %d is the first that ")
				TEXT("does not: %s"),
				*Where, FirstWrongGrid,
				FirstWrongGrid == INDEX_NONE ? TEXT("none is") : *WhyGridWrong),
			FirstWrongGrid == INDEX_NONE);

		/* --- and the regions resolve to the same bricks -------------------------------------- */

		struct FRegionCheck
		{
			const TCHAR* What;
			TArrayView<const FWallRegion> Regions;
		};

		const FRegionCheck RegionChecks[] =
		{
			{ TEXT("cut"), Case.Cuts },
			{ TEXT("must-fall"), Case.MustFall },
			{ TEXT("must-stand"), Case.MustStand },
		};

		for (const FRegionCheck& Check : RegionChecks)
		{
			if (Check.Regions.Num() == 0)
			{
				continue;
			}

			const TArray<int32> Expected = PiecesInRegions(Fixture, Check.Regions);

			const TArray<DestructionWallCases::FWallRegion> Theirs =
				ProductionRegionsOf(Check.Regions);

			TArray<int32> Named;
			DestructionWallCases::PiecesInRegions(Laid, Theirs, Named);

			TestTrue(
				*FString::Printf(
					TEXT("%s: the %s regions must name exactly the bricks the fixture names, in ")
					TEXT("handle order — the fixture names %d %s, production names %d %s"),
					*Where, Check.What,
					Expected.Num(), *DescribePieces(Fixture, Expected),
					Named.Num(), *DescribePieces(Fixture, Named)),
				Named == Expected);
		}
	}

	return true;
}

/**
 * EVERY ONE OF THE TWENTY IS A LEVEL A HUMAN CAN JOIN, LAYING THE SAME WALL AND CUTTING THE SAME
 * BRICKS THE ACCEPTANCE ROW MEASURES.
 *
 * =====================================================================================
 * WHY THE CHECK LIVES HERE RATHER THAN BESIDE THE CATALOGUE
 * =====================================================================================
 *
 * The verdicts stay in this file, so the only place that knows what case 8 IS, is this file. A
 * test living in `Tests/DestructionScenariosTest.cpp` could assert that a row called `wall-08`
 * exists and builds; it could not assert that the wall it builds is the wall case 8's expected
 * outcome was measured against, which is the only claim worth making. The include direction is
 * still test-includes-production: this reads `World/DestructionScenarios.h`, and nothing in
 * production reads anything here.
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND WHY THE CUT IS THE HALF THAT MATTERS
 * =====================================================================================
 *
 * Each case must have a row; the row must carry the case's title VERBATIM, so a level and a
 * catalogue entry cannot describe different things; the wall it builds must be the fixture's wall
 * brick for brick AND in handle order; and the bricks it resolves to cut must be exactly the
 * bricks the case's cut regions name.
 *
 * MOST OF THESE ROWS CUT, AND THAT IS THE POINT OF THE LEVEL. The wall stands, the player looks at
 * it for the row's own hold, and then the bricks go and they watch what the wall does about it. A
 * level that laid the right wall and cut a brick two cells over would show a plausible collapse
 * that has nothing to do with the case it is named after — which is exactly the failure the whole
 * acceptance set exists to make visible, relocated to where nobody is looking for it. Cases 1 and
 * 17 are intact walls and must cut nothing, and that is asserted from the case's own empty cut
 * list rather than from a list of numbers here.
 *
 * THE SET IS COMPARED SORTED, BECAUSE THE ORDER OF A CUT IS THE ROW'S OWN BUSINESS: every cut
 * brick goes in one batch and one solve, so nothing downstream can see the sequence. WHICH bricks
 * is the whole claim.
 *
 * NEEDS A TICKING WORLD: NO. The catalogue and the producer are both world-free; what a level does
 * with them is `World.Scenarios.*`'s business and is already covered.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceLevelsTest,
	"DestructionGame.Acceptance.Wall.EveryCaseIsAPlayableLevel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceLevelsTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;
	using namespace DestructionScenarios;

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty cases"), Cases.Num(), 20);

	for (const FWallCase& Case : Cases)
	{
		const FString LevelName = LevelNameForCase(Case.Number);
		const FString MapName = LevelMapNameForCase(Case.Number);

		const FString Where =
			FString::Printf(TEXT("case %d (%s) as '%s'"), Case.Number, Case.Title, *LevelName);

		/* --- ONE: there is a row, and it is this case ---------------------------------------- */

		const int32 Index = IndexOfName(FName(*LevelName));

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: the twenty walls the user drew are the whole reason the acceptance set ")
					TEXT("exists, and until one is a catalogue row it is a fixture nobody can stand ")
					TEXT("in front of. There is no row named '%s'."),
					*Where, *LevelName),
				Catalogue().IsValidIndex(Index)))
		{
			continue;
		}

		const FScenario& Row = Catalogue()[Index];

		TestEqual(
			*FString::Printf(
				TEXT("%s: must be joinable from its own map, '%s'"), *Where, *MapName),
			FString(Row.MapName), MapName);

		/*
		 * THE TITLE IS THE CASE'S, VERBATIM. The catalogue in WALL_CASES.html, the acceptance row
		 * and the banner a player reads have to be three views of one case rather than three
		 * descriptions that agree today.
		 */
		TestEqual(
			*FString::Printf(TEXT("%s: must carry the case's own title"), *Where),
			FString(Row.Title), FString(Case.Title));

		/* --- TWO: it lays the wall the case was measured on ---------------------------------- */

		FWall Fixture;
		LayWall(Case, Fixture);

		if (Fixture.NumPieces() == 0)
		{
			AddError(FString::Printf(TEXT("%s: FIXTURE laid no bricks at all"), *Where));

			continue;
		}

		DestructionLayout::FBrickLayout Built;
		TArray<int32> BuiltCut;

		if (!Build(Row, Built, BuiltCut))
		{
			AddError(FString::Printf(
				TEXT("%s: the row must build — a catalogue row that cannot be laid is a level that ")
				TEXT("cannot be joined"),
				*Where));

			continue;
		}

		LayoutMatchesFixture(*this, Where, Built, Fixture);

		/* --- THREE: and it cuts the bricks the case cuts, and only those ---------------------- */

		const TArray<int32> Expected = CutPieces(Case, Fixture);

		TArray<int32> Actual = BuiltCut;
		Actual.Sort();

		TestTrue(
			*FString::Printf(
				TEXT("%s: the level must cut exactly the bricks the case cuts — the case cuts %d %s, ")
				TEXT("the level cuts %d %s. A level that lays the right wall and cuts the wrong brick ")
				TEXT("shows a plausible collapse that has nothing to do with the case it is named ")
				TEXT("after."),
				*Where,
				Expected.Num(), *DescribePieces(Fixture, Expected),
				Actual.Num(), *DescribePieces(Fixture, Actual)),
			Actual == Expected);

		/*
		 * AND THE INTACT ROWS CUT NOTHING, DERIVED FROM THE CASE RATHER THAN LISTED. Cases 1 and 17
		 * are whole walls; a row that quietly took a brick out of one would turn a regression anchor
		 * into a different case.
		 */
		if (Case.Cuts.Num() == 0)
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: this case cuts nothing, so its level must cut nothing"), *Where),
				Row.CutCentresCm.Num(), 0);
		}
		else
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s: this case cuts %d brick(s), so its level must name at least one cut ")
					TEXT("centre; it names %d"),
					*Where, Expected.Num(), Row.CutCentresCm.Num()),
				Row.CutCentresCm.Num() > 0);
		}
	}

	return true;
}

/**
 * NO LEVEL PUTS A VERDICT ON SCREEN THAT THE SOLVER DOES NOT PRODUCE.
 *
 * =====================================================================================
 * WHY A CAPTION NEEDS A TEST AT ALL
 * =====================================================================================
 *
 * Three of these twenty rows are red today — 10, 19 and 20 — and a level captioned "the
 * course over the doorway drops and the wall stands" while the model drops nothing is a LIE TOLD TO
 * SOMEBODY STANDING IN FRONT OF THE COUNTER-EXAMPLE. That is worse than silence: the player can
 * see the wall, and the caption is the only thing telling them whether what they are looking at is
 * the answer or the bug.
 *
 * SINCE 2026-08-12 TWO OF THE THREE LIE IN THE OTHER DIRECTION, and the caption convention handles
 * it without changing: cases 10 and 19 are captioned `Expected: STANDS` AND carry the disagreement
 * marker, because they were re-ruled to stand while the model still drops 12 and 34 bricks. A
 * player standing in front of wall-10 sees a collapsed panel under a caption saying the wall should
 * be whole, which is exactly the shape of admission this test exists to force.
 *
 * So a caption may say what is expected, and it may say that the model disagrees, and it may say
 * only what the level does — but it may not claim a verdict the solver does not currently produce
 * without admitting it.
 *
 * =====================================================================================
 * THE CONVENTION, AND WHY IT IS A TOKEN RATHER THAN PROSE MATCHING
 * =====================================================================================
 *
 * A caption is prose and has to stay prose. What is pinned is one token in it: `Expected: STANDS`,
 * `Expected: LOCAL LOSS` or `Expected: COLLAPSE`, spelled the way `VerdictName` spells it, exactly
 * one of the three, and matching the verdict the acceptance row asserts. Everything else in the
 * sentence is the writer's. Matching whole sentences would have made the caption unwritable;
 * matching nothing would have let it drift the day a verdict changed — which has already happened
 * twice in this project, when cases 8 and 16 were corrected in WALL_CASES.html and left stale in
 * this file for a day.
 *
 * AND WHICH ROWS MUST ADMIT A DISAGREEMENT IS COMPUTED, NEVER LISTED. The row is RUN, its verdict
 * is evaluated, and the marker is required exactly when the model got it wrong — so a slice that
 * fixes case 20 turns this red until the caption stops claiming a disagreement that no longer
 * exists, and so does a RE-RULING that hands a row to the model, which is precisely what case 8's
 * 2026-08-11 STANDS ruling did: production's wall-08 caption still claimed LOCAL LOSS and still
 * carried the marker, and this test held that red until the caption moved in the same slice. A
 * hardcoded list of numbers would have rotted silently in the other direction.
 *
 * THE 2026-08-12 RULINGS EXERCISED BOTH DIRECTIONS AT ONCE, which is the sharpest demonstration
 * this convention has had. Case 9 was re-ruled STANDS and the model agrees, so its caption must
 * stop claiming COLLAPSE and must DROP the marker; cases 10 and 19 were re-ruled STANDS and the
 * model does NOT agree, so their captions must stop claiming COLLAPSE and must KEEP the marker.
 * Three rows, one verdict change each, two opposite marker outcomes — and none of it is listed
 * anywhere, it falls out of running the wall.
 *
 * THE KNOWN-RED SET IS A TRIPWIRE ON THIS TEST'S OWN PREDICATE, not a second copy of the rule.
 * `ModelAgreesWithVerdict` is a second reading of the three verdict shapes `Acceptance.Wall.
 * Catalogue` asserts one at a time, so it could drift from the catalogue and caption a level
 * wrongly while every catalogue row still failed exactly as before. Requiring it to name precisely
 * the three rows the suite knows to be red is what makes that drift visible.
 *
 * NEEDS A TICKING WORLD: NO. It solves the same twenty walls the catalogue solves.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceCaptionTest,
	"DestructionGame.Acceptance.Wall.EveryLevelsCaptionTellsTheTruth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceCaptionTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;
	using namespace DestructionScenarios;

	/**
	 * THE THREE ROWS THE MODEL GETS WRONG TODAY.
	 *
	 * This is NOT what decides which caption must admit a disagreement — that is computed per row
	 * below. It is a check on the predicate that computes it: a second reading of the verdict rules
	 * that drifted from the catalogue's would caption a level wrongly while every catalogue
	 * failure stayed word for word identical, which is the one failure this file could not
	 * otherwise see.
	 *
	 * IT BRIEFLY READ SEVEN ON 2026-08-08 AND CASE 11 HAS SINCE COME BACK OUT OF IT, both times
	 * because an EXPECTATION moved rather than because anything regressed — ruled down on BS 5977's
	 * arching gate, then re-ruled up when the user directed that a published design threshold is not
	 * a collapse predictor and the physics was worked honestly. CASE 12 CAME OUT THE SAME WAY ON
	 * 2026-08-09: its rewrite (see the block above Case12Cuts) replaced a geometry the model got
	 * wrong with one it gets right, so the exit is an expectation moving again, not a solver fix.
	 * AND CASE 8 ON 2026-08-11, the third of the same shape and the one that cost the most: the
	 * catalogue's LOCAL LOSS was the outlier of three methods once the LP oracle stood the fixture
	 * at lambda* = 324.73, and the user re-ruled it STANDS. CASE 9 IS THE FOURTH, ON 2026-08-12, on
	 * exactly the same reading applied to a ten-cell span (the CASE 9 block of section B carries the
	 * arithmetic and the cost).
	 *
	 * AND THE SAME DAY PRODUCED THE FIRST RE-RULINGS THAT DID *NOT* SHRINK THIS SET. Cases 10 and 19
	 * were re-ruled from COLLAPSE to STANDS beside case 9, and they STAY here: the model drops 12
	 * and 34 pieces where the catalogue now says nothing should come down, so the disagreement
	 * survives the ruling with its sign reversed. Do not read membership of this list as "the
	 * catalogue expects more damage than the model produces" — for two of the three it is now the
	 * other way round.
	 *
	 * Worth leaving on the record here: this set moving is nearly always a verdict being corrected,
	 * which is why the tripwire at the bottom of this test is TWO assertions rather than one — "the
	 * set grew" and "the set changed shape" are opposite pieces of news and used to fail in
	 * identical words.
	 */
	const int32 KnownDisagreements[] = { 10, 19, 20 };

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty cases"), Cases.Num(), 20);

	TArray<int32> Disagreed;

	for (const FWallCase& Case : Cases)
	{
		const FString LevelName = LevelNameForCase(Case.Number);

		const FString Where =
			FString::Printf(TEXT("case %d (%s) as '%s'"), Case.Number, Case.Title, *LevelName);

		/*
		 * WHAT THE MODEL ACTUALLY DOES WITH THIS WALL, READ BEFORE THE ROW IS LOOKED UP AND
		 * DELIBERATELY NOT SKIPPED WHEN THERE IS NO ROW YET.
		 *
		 * The tripwire at the bottom is a check on the predicate rather than on the captions, and a
		 * predicate that was only exercised once the levels existed would be unproven at exactly the
		 * moment somebody was relying on it to caption twenty of them. Run this way it reproduces
		 * the known reds on the day it is written.
		 */
		const FSolvedWall& Solved = RunWallCase(*this, Case);

		const FWall& Wall = Solved.Wall;
		const FWallResult& Result = Solved.Result;

		if (!Result.bLaid)
		{
			continue;
		}

		const bool bAgrees = ModelAgreesWithVerdict(Case, Wall, Result);

		if (!bAgrees)
		{
			Disagreed.Add(Case.Number);
		}

		const int32 Index = IndexOfName(FName(*LevelName));

		if (!Catalogue().IsValidIndex(Index))
		{
			AddError(FString::Printf(
				TEXT("%s: there is no row to caption. A level with nothing saying what should happen ")
				TEXT("leaves a human unable to tell a correct wall from a broken one."),
				*Where));

			continue;
		}

		const FString Caption = FString(Catalogue()[Index].Expectation);

		/* --- ONE: it claims its own verdict, and only its own -------------------------------- */

		const EVerdict Verdicts[] = { EVerdict::Stands, EVerdict::LocalLoss, EVerdict::Collapse };

		FString Claimed;
		int32 ClaimCount = 0;

		for (const EVerdict Verdict : Verdicts)
		{
			if (Caption.Contains(VerdictClaimFor(Verdict), ESearchCase::CaseSensitive))
			{
				++ClaimCount;
				Claimed = VerdictName(Verdict);
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: its caption must claim exactly one verdict, written '%s'; it claims %d ")
				TEXT("(%s). The caption reads: \"%s\""),
				*Where, *VerdictClaimFor(Case.Verdict), ClaimCount,
				ClaimCount == 0 ? TEXT("none") : *Claimed, *Caption),
			ClaimCount == 1
				&& Caption.Contains(VerdictClaimFor(Case.Verdict), ESearchCase::CaseSensitive));

		/* --- TWO: and it admits a disagreement exactly when there is one --------------------- */

		const bool bAdmits = Caption.Contains(ModelDisagreesMarker, ESearchCase::CaseSensitive);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the model %s this row's verdict of %s, so its caption %s say '%s'. A level ")
				TEXT("captioned with an outcome the solver does not produce is a lie told to somebody ")
				TEXT("standing in front of the counter-example; an admission left behind after the ")
				TEXT("model was fixed is the same lie the other way round. %d piece(s) came down %s. ")
				TEXT("The caption reads: \"%s\""),
				*Where,
				bAgrees ? TEXT("AGREES with") : TEXT("DISAGREES with"),
				VerdictName(Case.Verdict),
				bAgrees ? TEXT("must NOT") : TEXT("MUST"),
				ModelDisagreesMarker,
				Result.Fallen.Num(), *DescribePieces(Wall, Result.Fallen), *Caption),
			bAdmits, !bAgrees);
	}

	/* --- and the predicate above is the catalogue's, checked against the known reds ---------- */

	TArray<int32> Known;
	Known.Append(KnownDisagreements, UE_ARRAY_COUNT(KnownDisagreements));

	auto NumberList = [](TArrayView<const int32> Numbers)
	{
		FString Text;

		for (const int32 Number : Numbers)
		{
			if (!Text.IsEmpty())
			{
				Text += TEXT(", ");
			}

			Text += FString::FromInt(Number);
		}

		return Text.IsEmpty() ? FString(TEXT("none")) : Text;
	};

	const FString DisagreedText = NumberList(Disagreed);
	const FString KnownText = NumberList(Known);

	/*
	 * TWO FAILURES, NOT ONE, BECAUSE THE TWO WAYS THIS SET CAN MOVE MEAN OPPOSITE THINGS.
	 *
	 * The single `Disagreed == Known` row this replaces said only "these differ", and the two
	 * things it was reading are not the same event at all:
	 *
	 *   GREW    a row the model used to get RIGHT now reads wrong. Nothing about the expectations
	 *           moved, so either the solver regressed or a verdict was just re-ruled and this list
	 *           has not caught up. Read it as a REGRESSION until the diff proves otherwise.
	 *   SHRANK  a row on the known-red list now reads right. A slice fixed it — or an expectation
	 *           was corrected to what the model already did — and the paperwork is owed: this list,
	 *           the caption's disagreement marker, and the row's `DropsToday` anchor in
	 *           Acceptance.Wall.Catalogue all have to come off together.
	 *
	 * CASE 11 IS WHY THIS DISTINCTION IS WORTH A SECOND ASSERTION. It was ruled down to a local
	 * loss on 2026-08-08 and back to "stands" the same day; both edits moved this set while the
	 * solver did not change by one line, and a message reading "these differ" made the good news
	 * and the bad news look identical.
	 *
	 * TOGETHER THEY ARE STILL EXACTLY SET EQUALITY — every number appears at most once on either
	 * side, so mutual containment is the same claim the one row made, split by direction.
	 */
	TArray<int32> Grew;

	for (const int32 Number : Disagreed)
	{
		if (!Known.Contains(Number))
		{
			Grew.Add(Number);
		}
	}

	TArray<int32> Shrank;

	for (const int32 Number : Known)
	{
		if (!Disagreed.Contains(Number))
		{
			Shrank.Add(Number);
		}
	}

	TestTrue(
		*FString::Printf(
			TEXT("TRIPWIRE — THE SET GREW: case(s) {%s} read WRONG here and are not on the known-red ")
			TEXT("list. Nothing on that list was expected to grow on its own, so this is a ")
			TEXT("REGRESSION until proved otherwise: the model now disagrees somewhere it used to ")
			TEXT("agree, or this test's reading of a verdict has drifted from Acceptance.Wall.")
			TEXT("Catalogue's and a level is being captioned by the wrong rule. (It read {%s}, the ")
			TEXT("known reds are {%s}.) A verdict that was deliberately re-ruled lands here too — if ")
			TEXT("that is what happened, the same change must add the row to the list."),
			*NumberList(Grew), *DisagreedText, *KnownText),
		Grew.Num() == 0);

	TestTrue(
		*FString::Printf(
			TEXT("TRIPWIRE — THE SET CHANGED SHAPE: case(s) {%s} are on the known-red list and the ")
			TEXT("model now AGREES with them. That is a row being FIXED or an expectation being ")
			TEXT("corrected, not a regression — and it is unfinished paperwork: the same change owes ")
			TEXT("this list, the level caption's '%s' marker, and the row's DropsToday anchor in ")
			TEXT("Acceptance.Wall.Catalogue. (It read {%s}, the known reds are {%s}.)"),
			*NumberList(Shrank), ModelDisagreesMarker, *DisagreedText, *KnownText),
		Shrank.Num() == 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
