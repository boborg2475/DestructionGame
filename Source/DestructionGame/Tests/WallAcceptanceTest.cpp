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
 * The wall acceptance set — twenty-two configurations with an expected outcome for each, drawn
 * from how real masonry behaves rather than from what the solver computes.
 *
 * The catalogue is claude_plans/WALL_CASES.html, agreed by the user 2026-08-06. This file is that
 * catalogue as a parameterised table: adding a case is adding numbers, not code. Case 20's own
 * uncertainty is recorded beside it. Cases 21 and 22 were user-directed 2026-08-12, built
 * 2026-08-13 — see section G.
 *
 * WHY A SPREAD RATHER THAN ONE FIXTURE. Five matched pairs originally differed by exactly one
 * variable each — 7 vs 8 depth of cover, 7 vs 9 span, 7 vs 10 abutment, 13 vs 14 corbel
 * projection, 15 vs 16 whether superimposed compression suppresses bending tension — so a
 * disagreement between the two halves points at one term in the force calculation, which a single
 * wall never can.
 *
 * NONE OF THE FIVE IS AN OUTCOME PAIR ANY MORE; `Acceptance.Wall.MatchedPairs` retired with the
 * last of them on 2026-08-12, once five re-rulings (none of them a solver fix) made every half of
 * every pair stand:
 *
 *   13 vs 14   2026-08-07   a bonded corbel resists with its full depth
 *   15 vs 16   2026-08-07   a bonded header with nothing on it reads a sixth of f_xk1
 *   11 vs 12   2026-08-09   the one-cell pier takes the thrust
 *    7 vs  8   2026-08-11   the coverless course jams into its abutments as a flat arch
 *    7 vs  9   2026-08-12   the deep beam spans
 *    7 vs 10   2026-08-12   the free-end panel cantilevers
 *
 * 7 vs 10 forced the retirement rather than one more deleted row: both halves now rule STANDS, so
 * the assertion (0 against 0) is unsatisfiable, while the model's 12 unrouted bricks on case 10
 * made it read as a discrimination it no longer is.
 *
 * WHERE EACH DISCRIMINATION WENT, so deleting the outcome assertion doesn't mean the suite
 * quietly stopped measuring anything:
 *
 *   13 vs 14  Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome — doubling the step
 *             per course takes the worst joint from 0.070 to 0.195, 2.8x.
 *   15 vs 16  Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome — 0.0018 with six
 *             courses on the header's tail against 0.0582 with nothing on it, 32x.
 *    7 vs  9  Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome — the jamb beside the opening
 *             reads 0.269 under a four-cell span and 0.985 under a ten-cell one, 3.66x.
 *   11 vs 12  the LP oracle: 128.12 on three cells of bearing against 89.12 on one, pinned in
 *             OracleSweepFull.RigidBlock.WallsAndLadders.
 *    7 vs 10  the LP oracle likewise, deliberately not relocated onto a production reading (see
 *             the abutment paragraph in the span test's header): 296.22 against 35.82, 8.27x.
 *    7 vs  8  nowhere — the readings run the wrong way (0.269 at eight courses of cover against
 *             0.219 at one), because a downward-routing solver reads cover as load rather than
 *             arch capacity. Recorded in the CASE 8 block of section B; after that ruling no case
 *             in this set refuses arching for lack of cover.
 *
 * IF AN OUTCOME PAIR EVER COMES BACK — a discriminator that starves the abutment, or a lintel over
 * case 7's opening — the shape is: a table of {variable, lesser case, greater case}, run both
 * halves, assert the greater loses strictly more while the lesser loses nothing whenever its own
 * `EVerdict` says so (read off the enum, never duplicated in the test).
 *
 * THREE VERDICTS, THREE ASSERTION SHAPES, per DESIGN.md §4's outcome-not-mechanism rule:
 *
 *   STANDS      nothing left the structure and no joint anywhere gave. Both halves, because
 *               "no piece fell" alone passes for a wall that severed half its joints and stayed
 *               leaning together.
 *   LOCAL LOSS  the set of pieces that lost the ground is exactly the named set. Identity, not a
 *               count: a test that only counted would be satisfied by the wrong bricks falling,
 *               and "the course over the doorway drops but the wall is fine" is the whole point
 *               of having a middle verdict at all.
 *   COLLAPSE    every piece of a named region lost the ground, and every piece of a named
 *               survivor region kept it. Two-sided, because a wall that comes down because
 *               everything comes down is not evidence that the span term works.
 *
 * Between 2026-08-12 and 2026-08-13 the catalogue held no `Collapse` row at all — the collapse arm
 * of `Acceptance.Wall.Catalogue` and the matching branch of `ModelAgreesWithVerdict` were dead
 * code until cases 21 and 22 landed 2026-08-13, the set's first two `Collapse` rows. Their bite
 * was re-proven by mutation on arrival (a widened survivor region on case 21; the
 * `ReseatSpannedGroups` early-return) — both are TRAPS.md registry rows.
 *
 * Displacement is never used as a break assertion: DESIGN.md §4 is explicit that two pieces can
 * sever and stay resting exactly in place. What is read is whether a piece still has a path to
 * the earth after the cascade.
 *
 * NEEDS A TICKING WORLD: NO, DELIBERATELY. Gravity is on (weight is mass x 980, no way to switch
 * it off), everything is connected, and the assertion is on outcome — everything DESIGN.md §4
 * asks of an integration test. The one thing a world would add is the wire from the solver's
 * answer to Chaos, which Tests/StructureIntegrationTest.cpp already covers three times over and
 * which is identical for all twenty-two rows; twenty-two worlds of up to 474 brick actors would
 * cost minutes to say nothing new. Promote a row into the integration file if it ever needs
 * watching fall, rather than moving this table.
 *
 * Named namespace, and named differently from every other one in this module — an anonymous
 * namespace is private to a translation unit rather than a file, and a unity build merges many
 * files into one (CURRENT_STATE.md). The `using namespace` lives inside each RunTest body for the
 * same reason.
 *
 * Nothing here is imported from the code under test except the producer itself. The grid, the
 * brick weight, the newton-to-Unreal conversion and every strength are re-derived below, so a
 * wrong constant in production makes this file disagree with it rather than agree with it. The
 * one deliberate exception is Layout::MakeInterface, which decides whether two boxes share a
 * face: re-implementing that would be re-implementing the thing under test, and
 * Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays pins the fixture's own bricklaying against
 * Layout::RunningBond so this file cannot quietly become a second, drifting wall producer.
 */
namespace WallAcceptanceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* The coordinating grid, re-derived here. UK metric standard brick with a 1 cm joint, so a
	 * brick plus a joint is one cell along the wall (22.5 cm) and one course up (7.5 cm), and
	 * running bond offsets alternate courses by half a cell. Every position in the table below is
	 * quoted in cells — a piece's cell index is its centre X divided by 22.5 — because that is
	 * the unit the bond is built on and the only one in which "one brick out, mid-wall" is a
	 * number somebody can check by eye. */
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
	 * A course laid right-to-left rarely divides exactly, and what's left at the left end is a cut
	 * brick; below this it's dropped instead, since a two-centimetre sliver has a plausible mass
	 * and an implausible joint, and would sit far from everything under test while being the first
	 * thing to break. 4 cm is comfortably under the 10.25 cm half bat a flush wall really closes
	 * with, and comfortably over anything laid by accident.
	 */
	constexpr double MinClosingPieceCm = 4.0;

	/** g/cm3. ClayBrick's published density, spelled out so a changed profile shows up here. */
	constexpr double ClayBrickDensityGramsPerCubicCm = 1.9;

	/**
	 * Weight from mass, derived rather than imported. Unreal's gravity is 980 cm/s2 and mass is in
	 * kilograms, so kg x 980 is the weight in Unreal force units — DESIGN.md §3's 1 N = 100 uu is
	 * already inside that number, and applying it again is the 100x error the units section exists
	 * to prevent. Density first, then dimensions, landing exactly on 2.72163125 kg.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double FullBrickWeightUu =
		ClayBrickDensityGramsPerCubicCm * BrickLengthCm * BrickDepthCm * BrickHeightCm / 1000.0
		* GravityCmPerSecondSquared;

	/** Unreal force units per MPa per cm2, spelled out rather than imported. Production has one
	 * named boundary for this, Core/ConnectionStrength.h's ForceUnitsPerMPaSqCm — a test that read
	 * it would agree with a wrong one, so this writes the number itself and disagrees instead. */
	constexpr double ForceUnitsPerMPaSqCmHere = 10000.0;

	/* The mean shear bond f_v0 for general-purpose mortar, asserted against the profile
	 * (re-anchor 2026-08-13: Gooch et al. 2023's unconfined M4/M6 triplet means average 1.117,
	 * the 2025 regression intercepts span 0.58-1.04; 0.90 is the centre of the regression range.
	 * The retired characteristic f_vk0 was EN 1996-1-1 Table 3.4's 0.20). */
	constexpr double MortarShearCohesionMPa = 0.9;

	/* The mean flexural bond f_x1, asserted against the profile (re-anchor 2026-08-13: twelve
	 * measured M4/M6 batch means averaging 0.571 bracketed with UK NA Table NA.6's 0.4 x the
	 * campaign's 1.89 mean/characteristic ratio = 0.76; 0.70 is the centre. The retired
	 * characteristic f_xk1 was EN 1996-1-1 Table 3.2's 0.10). */
	constexpr double MortarFlexuralBondMPa = 0.7;

	/* The rest of the Mohr-Coulomb triple, needed since 2026-08-14 to say which axis a reading is:
	 * mu is the centre of the measured means (initial 0.64-1.00, residual 0.60-1.11, Gooch et al.
	 * 2025), the truncation is the mean-basis 0.1.f_b against a 20 MPa unit, and the compressive
	 * strength is the unmoved declared-class figure. Asserted against the profile in
	 * `SpanIsReadInTheJointNotInTheOutcome` — the only test here that reads them. */
	constexpr double MortarFrictionCoefficient = 0.75;
	constexpr double MortarMaxShearStrengthMPa = 2.0;
	constexpr double MortarCompressiveStrengthMPa = 10.0;

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
		 * How many pieces the model drops here today — a characterisation of a wrong answer, set
		 * on the three known-red rows and INDEX_NONE everywhere else.
		 *
		 * Not an expectation and endorses nothing: `MustFall` and `MustStand` above are what a
		 * real wall does; this is what the solver does instead, measured off a run and written
		 * down. It exists because a row that is already red absorbs a regression silently: case
		 * 20 is supposed to drop two bricks, drops nine, and would go on failing in exactly the
		 * same words if a change made it drop ninety. Pinning the count makes the known failure a
		 * fixed point rather than a hole in the net.
		 *
		 * Since 2026-08-12 two of the three sit on a row whose verdict is `Stands`, a new shape
		 * for this field worth reading twice. Until then every red row asked for more loss than
		 * the model produced, so a `DropsToday` was always a count the catalogue would have liked
		 * to be bigger. Cases 10 and 19 are the other way round: the user re-ruled both to STANDS
		 * on the same day, the model still drops 12 and 34, and the pin now points at pieces that
		 * should never have left the wall. The direction does not change what the field means —
		 * it is still "what the solver does, never what it should do" — but a reader who assumes
		 * a `DropsToday` implies an expected collapse will misread those two.
		 *
		 * When the row is fixed, this anchor must be deleted in the same edit — it will fail, and
		 * that failure is the reminder. Never "update" it to a new wrong number without saying in
		 * the change why the model's answer moved. Case 9's pin went exactly that way on
		 * 2026-08-12: its re-ruling to STANDS handed the row to the model, so `DropsToday = 0` had
		 * nothing left to characterise and was deleted rather than kept as a zero.
		 */
		int32 DropsToday = INDEX_NONE;

		/**
		 * How many live pieces the solver cannot route here today. Zero is the claim; anything
		 * else is a characterised defect, written down on the row that has it.
		 *
		 * DESIGN.md §4 requires a collapse test to assert that nothing is `Stranded` at the moment
		 * it goes, so a solver limitation cannot wear a collapse's clothes — and this file had
		 * never asserted it. Writing it down for the first time on 2026-08-09 found that three of
		 * the then-six red rows did strand: cases 10, 12 and 19 routed part of what they drop
		 * nowhere at all, so part of those verdicts was the solver declining to divide load round
		 * a loop rather than masonry failing. That is a finding about the solver, recorded here
		 * rather than hidden by relaxing the assertion off those rows. Case 12's 11-strand pin
		 * died with its 2026-08-09 rewrite (the ten-cell cut that stranded is no longer laid), so
		 * two exceptions remain — cases 10 and 19 — and eighteen of the twenty make the plain zero
		 * claim. Neither exception can grow by one piece without failing.
		 *
		 * THE 2026-08-12 RULINGS SHARPENED WHAT THOSE TWO NUMBERS MEAN RATHER THAN MOVING THEM.
		 * Both rows are now ruled STANDS, and the LP-oracle sweep measured that production reaches
		 * its 12 and its 34 in zero cascade passes at worst readings of 0.300 and 0.318 — no joint
		 * anywhere came near capacity. So on those two rows the stranding is not a footnote to a
		 * collapse; it is the same finding as the drop count, said twice: nothing broke, the
		 * router simply had nowhere to send the load. That is precisely why the catalogue stopped
		 * calling either one a collapse — an absent mechanism is not a strength verdict.
		 *
		 * What would retire it: the loop-division rule DESIGN.md §5.1 records as still absent.
		 * When it lands, these two go to zero and the exceptions are deleted.
		 */
		int32 StrandsToday = 0;

		/**
		 * The worst reading the cascade started from, pinned — or 0.0 for a row that does not
		 * pin it.
		 *
		 * A count says that a wall came down; this says how hard it was pushed. Case 22 is the
		 * catalogue's one green Collapse row and it collapses on a knife edge: the mean re-anchor
		 * cut its margin from 8.2x of capacity to a little over 1, so one modest routing change
		 * now flips a verdict that used to have room for three. The drop count cannot see that
		 * coming — a wall that fell by 1% and a wall that fell by 800% drop the same bricks — and
		 * the figure quoted in the CASE 22 block was taken by hand once, at strengths the project
		 * no longer carries, with nothing recomputing it.
		 *
		 * Read before any joint is allowed to give, which is what makes it a different number
		 * from `FWallResult::Worst`: see that field for why the post-cascade reading is a
		 * statement about the survivors instead.
		 */
		double PreCascadeWorstToday = 0.0;
	};

	/*
	 * The fixture's own bricklayer.
	 *
	 * Why not Layout::RunningBond: six of the twenty-two cases are not running-bond rectangles (two
	 * corbel out, two carry a projecting header, two are stack bond), and RunningBond lays only one
	 * shape — dropping those six would lose two of the five matched pairs, the whole reason the set
	 * exists.
	 *
	 * The bricklayer is one rule, not six special cases: every course is described by two numbers
	 * (where its right face is, how long its rightmost piece is) and laid right to left on the
	 * 22.5 cm pitch, closing with whatever is left at the wall's left face:
	 *
	 *      running bond    right face fixed; odd courses close with a half bat at the right
	 *      stack bond      right face fixed; every course closes with a full brick
	 *      corbel          right face steps out once per course from CorbelFromCourse
	 *      header          one course's right face is half a cell further out
	 *
	 * Laying right-to-left keeps the cut piece at the left end, far from every corbel and header
	 * under test, instead of in the middle of what's being measured.
	 *
	 * Checked against the real producer: a flush running-bond wall laid here must be the same wall
	 * Layout::RunningBond lays, brick for brick (Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays).
	 * Without that, this file is a second definition of what a wall is.
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

	/** Mass from geometry, derived here rather than imported. Density is g/cm3 and dimensions are
	 * cm, so cm3 x g/cm3 is grams and grams / 1000 is kilograms. No force conversion belongs here:
	 * 1 N = 100 uu is a property of forces and mass goes into Unreal unconverted. Density first
	 * for the same reason production does it — the association that lands exactly on 2.72163125
	 * for a full brick. */
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

				/* The box's centre is the centre of mass, since a brick is a homogeneous solid and
				 * its mass came off that same box. Without it the wall has no eccentricity at all
				 * and every corbel in it reads as though its weight acted through the middle of
				 * its support — exactly the state HasCompleteGeometry exists to make askable, and
				 * it is asserted as a fixture precondition below. */
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

		/* The pairs: neighbours along a course, and every piece of the course below. Offering the
		 * whole course below rather than working out which pieces something spans is what keeps
		 * the mixed-size and corbelled cases honest — MakeInterface refuses the pairs that turn
		 * out to be diagonals, and that decision belongs to it and to nothing written here.
		 * Courses two apart are never offered because a 6.5 cm brick on a 7.5 cm pitch cannot
		 * reach. */
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
	 * Which live pieces have lost their path to the earth. This is the outcome, not the
	 * mechanism: a joint severing is a step; what the player sees is which bricks come down, and
	 * a piece comes down exactly when the solve says nothing is holding it any more. Stranded
	 * counts as fallen for the same reason Falling does — the piece is not being carried, and
	 * whether the solver could not route it or genuinely has nothing to route it through is a
	 * different question, asked in the report line below, never folded into the verdict.
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
	 * How many live pieces the solver could not route at all. A precondition, never a verdict.
	 * Stranded means the solver declined to divide load round a loop, so it counts as fallen in
	 * FallenPieces above for the reason stated there — the piece is not being carried. But a row
	 * whose verdict was decided by that is a row about the solver's limit rather than about the
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

		/** Read as built, before anything is removed or broken, because the question is weaker
		 * afterwards: HasCompleteGeometry is a conjunction over what is still in the structure,
		 * so a removed piece with no centre of mass and a joint that has given both stop
		 * counting. Asking after the cut would let exactly the fixture defect this exists to
		 * catch through. */
		bool bCompleteGeometryAsBuilt = false;

		TArray<int32> Fallen;

		/** Of those, how many the solver could not route rather than could not hold up. */
		int32 Stranded = 0;

		double Worst = 0.0;
		int32 WorstPieceA = INDEX_NONE;
		int32 WorstPieceB = INDEX_NONE;

		/**
		 * The worst reading the cascade started from, a different quantity from `Worst`. `Worst`
		 * is read after the cascade has run, by which time every joint that gave has been skipped
		 * by `WorstUtilisation` — so on a row that collapses it reports what survived, and says
		 * nothing about how far past capacity the wall was when it started coming down. This is
		 * the loads solved once on the structure the cascade is about to be handed (after the
		 * cut, for a cutting row; as built, for a row whose as-built state is the case), with no
		 * joint yet allowed to give.
		 *
		 * It exists because the collapse rows' margin was unmeasured. Case 22's was taken by hand
		 * once, at the retired characteristic data, and nothing recomputed it across the mean
		 * re-anchor; the row's own block carried a green-phase marker saying so for a day. A
		 * margin quoted in a comment and computed nowhere is exactly the figure that goes stale
		 * silently.
		 */
		double PreCascadeWorst = 0.0;
		int32 PreCascadeWorstPieceA = INDEX_NONE;
		int32 PreCascadeWorstPieceB = INDEX_NONE;
	};

	/**
	 * Lay it, cut it, let the cascade run, and record what came down. No assertions at all.
	 *
	 * THE INTACT WALL IS SOLVED FIRST AND THAT IS NOT DECORATION. A case whose wall was already
	 * falling apart before the player touched it measures nothing, and every "stands" row would
	 * fail for a reason that has nothing to do with the case. What is done with that reading is
	 * CheckWallFixture's business: a cutting row must have stood before the cut, and a row that
	 * cuts nothing — the corbels, the header and the intact walls — is a case whose as-built state
	 * is the thing under test, so the catalogue reads it as a verdict rather than a precondition.
	 *
	 * PURE, SO IT CAN BE CACHED. Six tests in this file ask for the same twenty walls, and the
	 * walls were being laid and cascaded roughly sixty times a run to answer them. What may be
	 * shared between two tests is a wall's answer, a function of the case and nothing else; what
	 * may never be shared is the assertions about it, since a fixture failure that fired only for
	 * whichever test happened to ask first would move when tests are reordered. Hence the split:
	 * this half is cached, and the checking half below is re-run, against the cached answer,
	 * exactly as often as it was before.
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

		/* The as-built reading, taken before the first cascade is allowed to break anything. For a
		 * row that cuts nothing this is the reading the cascade started from; a cutting row
		 * overwrites it below with the post-cut one, the state that decides that row. */
		OutWall.Structure.SolveLoads();

		OutResult.PreCascadeWorst = WorstUtilisation(
			OutWall, OutResult.PreCascadeWorstPieceA, OutResult.PreCascadeWorstPieceB);

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

			OutWall.Structure.SolveLoads();

			OutResult.PreCascadeWorst = WorstUtilisation(
				OutWall, OutResult.PreCascadeWorstPieceA, OutResult.PreCascadeWorstPieceB);

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
		 * And nothing may be Stranded, the precondition that makes a verdict honest. The same
		 * claim `Core.Structure.AStaircaseVoidCondemnsTheCorbel` and the collapse rows of
		 * `Tests/StructureIntegrationTest.cpp` make, and `Acceptance.Beam.Catalogue` makes row by
		 * row: a Stranded piece is one the solver declined to route round a loop, not one the wall
		 * failed to hold up. `FallenPieces` folds the two together on purpose — a piece nothing is
		 * carrying comes down either way — so without this the two are indistinguishable inside a
		 * verdict, and a row could name exactly the right bricks for entirely the wrong reason.
		 *
		 * Written against the row's own figure rather than a bare zero, because two rows are not
		 * zero: see FWallCase::StrandsToday for what that means and why it is recorded on the
		 * rows instead of being relaxed away. Eighteen of the twenty make the plain claim.
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
	 * Everything the solve reads off a case, and nothing else — what makes it a key. `LayWall`
	 * reads the course count, the cell count and (through `CourseGeometry`) the bond, the corbel
	 * and the projecting course; `CutPieces` reads the cut regions. It reads no other field, so
	 * two cases agreeing on these are the same wall cut the same way and cannot differ in their
	 * answer. The case number is deliberately not part of the key: the height ladder in
	 * `StackBondColumnShearIsHeightIndependent` builds its own case 18 at ten courses, which is
	 * the catalogue's case 18 brick for brick, and its sixteen-course sibling differs here in the
	 * first field. Verdicts, titles and the named fall regions are not part of it either, since
	 * the solver never sees them — they are what the assertions compare the answer against.
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

	/** The answer for this wall, laid and cascaded once however many tests ask for it. Held by
	 * pointer rather than value because a TMap moves its values when it grows, and every caller
	 * here holds a reference across the rest of its own test. It outlives the run rather than the
	 * test, which costs twenty-one walls of memory and is safe for the same reason the cache is
	 * sound at all: the answer is a pure function of the key, so a second run in the same process
	 * recomputes nothing and reads exactly what the first one would have computed. */
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
			TEXT("stranded) %s, worst %.6g%s, pre-cascade worst %.17g%s"),
			Case.Number, Case.Title, VerdictName(Case.Verdict),
			Result.PiecesLaid, Result.PiecesCut, Result.IntactPasses, Result.CutPasses,
			Result.Fallen.Num(), Result.Stranded, *DescribePieces(Wall, Result.Fallen),
			Result.Worst,
			Result.WorstPieceA == INDEX_NONE
				? TEXT("")
				: *FString::Printf(
					TEXT(" at c%d/%g-c%d/%g"),
					Wall.CourseOf[Result.WorstPieceA], Wall.CellOf[Result.WorstPieceA],
					Wall.CourseOf[Result.WorstPieceB], Wall.CellOf[Result.WorstPieceB]),
			Result.PreCascadeWorst,
			Result.PreCascadeWorstPieceA == INDEX_NONE
				? TEXT("")
				: *FString::Printf(
					TEXT(" at c%d/%g-c%d/%g"),
					Wall.CourseOf[Result.PreCascadeWorstPieceA],
					Wall.CellOf[Result.PreCascadeWorstPieceA],
					Wall.CourseOf[Result.PreCascadeWorstPieceB],
					Wall.CellOf[Result.PreCascadeWorstPieceB])));
	}

	/* The catalogue. */

	/* --- A: one brick out. ---------------------------------------------------------
	 *
	 * Thirty courses, not the ten the drawing shows, and the height is the whole point.
	 * ARCHING_DESIGN.md works the half-seated joint out at 0.058203838 of capacity per brick
	 * weight it carries, so a brick with nine courses over it reads 0.52 and a wall ten courses
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

	/*
	 * CASE 8, re-ruled 2026-08-11: the one course over the hole stands, and the catalogue loses
	 * its last "no room to arch" row with it.
	 *
	 * Geometry, walked off the bricklayer above: five courses, twelve cells, case 7's own
	 * four-cell cut through courses 1..3 (one course of cover). Of the four course-4 bricks over
	 * the hole, cells 4 and 7 keep one bed patch each (a corbel); cells 5 and 6 have no bed patch
	 * at all.
	 *
	 * What the row used to claim: LOCAL LOSS of the two seatless bricks (c4/5, c4/6), on the
	 * reading that a single course cannot arch — it is a beam in flexure and mortar has 0.1 MPa to
	 * offer. Red for its whole life because the model drops nothing here.
	 *
	 * The ruling: as of the 2026-08-11 LP-oracle sweep this row was the outlier of three
	 * independently derived methods, not one:
	 *
	 *     catalogue   LOCAL LOSS — two bricks drop — on flexure of a single spanning course
	 *     production  drops nothing, breaks nothing; worst joint 0.218869 at c2/8-c3/7.5, in the
	 *                 jamb, not over the hole
	 *     LP oracle   STANDS at lambda* = 324.732096 (52 blocks / 101 joints, 1,289 pivots, 4.7 s —
	 *                 RigidBlockOracleSweepTest's opt-in wall sweep, measured 2026-08-11)
	 *
	 * The user ruled STANDS on 2026-08-11, per DESIGN §8's standing instruction (a published gate
	 * and a prior ruling are both evidence, neither is gospel):
	 *
	 *     the mechanism   flexure is not the only path across a four-cell hole. The limit theorem
	 *                     needs only one admissible equilibrium, and the one it finds puts the flat
	 *                     course into its abutments in head-joint compression — a flat arch, which
	 *                     needs abutments, not cover.
	 *     the margin      lambda* = 324.73 says the fixture is nowhere near strength-governed:
	 *                     compressive capacity is 100x tensile (10.0 / 0.1 MPa) and four bricks of
	 *                     self weight is a rounding error against it.
	 *     the charity     discounted by both of the sweep header's slants at once (/3
	 *                     plastic-vs-first-crack, /6 characteristic-vs-mean) it still reads 18.0.
	 *                     The /6 runs the honest way: the oracle ran at coded characteristic 0.10
	 *                     while every §8 ruling argues at mean 0.4-0.8, so the ruling basis would
	 *                     multiply lambda*, not divide it.
	 *     the abutment    "the jamb spreads and the flat arch lets go" is the one reading that
	 *                     could still condemn it, and the LP's joint rows (no-tension plus
	 *                     Mohr-Coulomb friction) would have shown a sliding abutment as a low
	 *                     lambda*, not a high one.
	 *
	 * The model's agreement (re-seating the spanned group through its head joints, the same
	 * jamming action in cruder form) is not confirmation: at this wall's height the springing
	 * carries almost no pre-compression, and as sigma_n goes to zero the Mohr-Coulomb cohesion term
	 * dominates and the thrust check goes toothless however large H/V becomes (measured 2026-08-07,
	 * TestResults/2026-08-07_0230/RESULTS.md §6.2). The verdict's weight is carried by the LP.
	 *
	 * The cost: case 8 was the catalogue's last "no room to arch" discriminator (DESIGN §8's
	 * case-11 entry names it twice). After this ruling no case anywhere refuses arching for lack of
	 * cover. Two consequences: the 7-vs-8 pair stops separating on outcome and left
	 * Acceptance.Wall.MatchedPairs, since retired entirely (2026-08-12, the file header records
	 * where every pair's discrimination went); and a replacement discriminator, if wanted, has to
	 * starve the ABUTMENT rather than the cover — case 10's shape, though case 10 has itself since
	 * been ruled to STAND.
	 *
	 * It also supersedes the standing doubt that was the other reason this went to the user:
	 * CURRENT_STATE recorded that a bricklayer would expect the whole course to come down rather
	 * than two named bricks. That doubt was about which bricks the LOCAL LOSS names; removing the
	 * local loss removes the doubt with it. If physical evidence ever contradicts this row it
	 * should come back through DESIGN §8 as a fourth ruling, not a quiet edit.
	 */

	/*
	 * CASE 9, re-ruled 2026-08-12: the ten-cell opening spans as a deep beam, and the third
	 * arching-gate verdict retires with it.
	 *
	 * Geometry, walked off the bricklayer above, cm: cut { 1, 3, 1.75, 11.25 } takes 28 of the 174
	 * laid; jambs toothed (even courses stop at 33.25, resume at 259.25; odd stop at 44.50, resume
	 * at 248.00); span 203.50 to 226.00 (~9.5 cells); head 60.00 cm of masonry (eight courses)
	 * spans the hole.
	 *
	 * What the row used to claim: COLLAPSE of { 4, 11, 1.75, 11.25 } with both jamb regions
	 * standing, on the published arching gate — BS 5977 wants 300 mm of masonry above the apex of a
	 * 45-degree triangle on the clear span, and a 2.1 m span needs 1.07 m of rise; this fixture has
	 * 0.60 m total. Red for its whole life because the model drops nothing here.
	 *
	 * The ruling: as of the 2026-08-12 sweep this row was the outlier of three independent methods,
	 * as case 8 was:
	 *
	 *     catalogue   COLLAPSE, on the arching gate
	 *     production  STANDS: drops nothing, breaks nothing, zero cascade passes; worst joint
	 *                 0.98502040901419818 at c3/11.5-c4/11, where the cover lands on the right jamb
	 *     LP oracle   STANDS at lambda* = 36.5639285 (146 blocks / 350 joints — OracleSweepFull.
	 *                 RigidBlock.WallsAndLadders; 4,885 pivots in 50.4 s, measured pre-partial-
	 *                 pricing 2026-08-12, cost figures since moved but lambda* and counts did not)
	 *
	 * The user ruled STANDS on 2026-08-12 per DESIGN §8's standing instruction. Worked through, and
	 * the hand check is independent of both other methods:
	 *
	 *     the mechanism   60 cm of bonded masonry over a 213.75 cm span is a deep beam at
	 *                     span/depth 3.6, not a triangle of loose bricks looking for an arch. A
	 *                     bonded section carries it in flexure whether or not there is room for the
	 *                     gate's triangle.
	 *     the arithmetic  eight courses over 9.5 cells is 76 brick weights = 2027 N; W L/8 = 542
	 *                     N.m; section t D^2/6 = 6150 cm^3; extreme fibre 0.088 MPa — 0.88x
	 *                     characteristic f_xk1 (0.10), ~0.15x the mean basis (0.4-0.8).
	 *     the margin      lambda* = 36.56 survives both sweep-header discounts at once (/3, /6) at
	 *                     2.03; the /6 runs the honest way (the oracle ran at coded characteristic).
	 *     the agreement   production's 0.985 and the hand figure's 0.88 are the same answer within
	 *                     the routing — two independently-implemented methods both put this fixture
	 *                     just under capacity, and the third stands it outright.
	 *
	 * The cost: the third verdict to leave the set on the retirement of the published arching gate
	 * (case 11 on 2026-08-08, case 8 on 2026-08-11, case 9 now) — after it, no case refuses a span
	 * for want of rise. The wanted-list discriminator that would restore the idea (CURRENT_STATE,
	 * "new acceptance cases wanted") has to starve the abutment instead.
	 *
	 * The row goes green, so its pins are deleted rather than moved: the model already stands this
	 * wall, so `DropsToday` has nothing to characterise, and case 9 leaves the caption test's
	 * known-disagreement list in the same edit.
	 *
	 * Worth watching: production's 0.98502040901 is one retune from 1.0. Nothing physical separates
	 * this wall from one that drops its whole head, and the day that reading crosses over, this row
	 * and the sweep's `AgreeStands` relation both flip with no change in the physics. Pinned by
	 * Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome so the crossing fails loudly.
	 */
	const FWallRegion Case9Cuts[] = { { 1, 3, 1.75, 11.25 } };

	/*
	 * CASE 10, re-ruled 2026-08-12: the free-end panel cantilevers, and production's "collapse" is
	 * an absent mechanism rather than a strength verdict.
	 *
	 * Geometry: case 7's cut extended through to the free right end — { 1, 3, 7.75, 11.50 }, twelve
	 * of the 150 laid. A jamb on the left, nothing on the right: the panel of cover has one
	 * support, not two, and 3.75 cells (84.4 cm) hangs past it.
	 *
	 * What the row used to claim: COLLAPSE of { 4, 11, 8.00, 11.50 } with { 0, 3, -1.0, 7.75 }
	 * standing, on the reading that an opening with no abutment cannot arch — true, but not the
	 * question. The fall region died with the ruling; the survivor region did not (see below).
	 *
	 * The ruling. As of the 2026-08-12 sweep:
	 *
	 *     catalogue   COLLAPSE — the overhanging panel comes down
	 *     production  drops 12, three stranded, in zero cascade passes at a worst reading of
	 *                 0.299975 (c2/7-c3/7.5, in the jamb); nothing broke
	 *     LP oracle   STANDS at lambda* = 35.8172298 (138 blocks / 349 joints; 2,640 pivots in
	 *                 11.5 s, measured pre-partial-pricing 2026-08-12 — cost figures since moved,
	 *                 lambda* and counts did not)
	 *
	 * The user ruled STANDS on 2026-08-12. What decided it is the zero in "zero cascade passes":
	 *
	 *     the finding     production is not saying this panel is too weak — its router could not
	 *                     find a path for those twelve pieces, because load only ever goes down and
	 *                     there is nothing under them. Unrouted, not overloaded.
	 *     the mechanism   84.4 cm of bonded panel 60 cm deep off one jamb is a cantilever, and a
	 *                     bonded section cantilevers — the same composite action the 2026-08-06
	 *                     free-end ruling credited for case 3, at 3.75 cells instead of one.
	 *     the arithmetic  8 courses x 3.75 cells is 30 brick weights = 800 N at a 42.2 cm lever,
	 *                     M = 337 N.m; over t D^2/6 = 6150 cm^3 that is 0.055 MPa — 0.55x
	 *                     characteristic f_xk1, 0.14x f_xk2 — and charitable to COLLAPSE twice over
	 *                     (the assumed crack plane is a straight vertical cut; a real one is a
	 *                     toothed staircase picking up bed-joint cohesion).
	 *     the margin      35.82 discounted by /3 and /6 together is 1.99, still standing.
	 *
	 * A new shape for the catalogue: a STANDS row the model still disagrees with (every previous
	 * STANDS verdict here was one the model produced). Stays on the known-red list and the
	 * caption's disagreement marker, keeps `DropsToday = 12` / `StrandsToday = 3` so the failure
	 * doesn't drift inside its own red. Goes green at DESIGN §7's evolution step 4, when
	 * equilibrium becomes the cascade's authority and load can route sideways.
	 *
	 * What survives the ruling: the survivor region, restored verbatim. A STANDS verdict says
	 * nothing comes down anywhere, so { 0, 3, -1.0, 7.75 } is still true and stronger than "twelve
	 * fell" — the drop/strand counts are blind to WHICH twelve, only the region names the identity.
	 * Asserted inverted (no piece inside may be among the fallen). Verified against the measured
	 * drop set: all twelve sit in courses 4..9 at cells 9.0 and above, clear of the region and cut.
	 *
	 * The cost: the 7-vs-10 abutment pair stops separating on outcome and leaves with the retired
	 * `Acceptance.Wall.MatchedPairs` (file header lists all five pairs and where each went). Not
	 * relocated onto a production reading — case 10's 0.300 against case 7's 0.269 is a real number
	 * measured on a wall the router could not route, and pinning it would claim an abutment term
	 * the model doesn't have. Its home is the LP: 296.22 against 35.82, 8.27x.
	 */
	const FWallRegion Case10Cuts[] = { { 1, 3, 7.75, 11.50 } };
	const FWallRegion Case10Stands[] = { { 0, 3, -1.0, 7.75 } };

	/* --- C: spanning between supports. --------------------------------------------- */

	/*
	 * CASE 11, ruled twice on 2026-08-08: a published design gate said LOCAL LOSS, the physics
	 * says STANDS, and the physics is the keeper.
	 *
	 * Drafted "stands", ruled "local loss", re-ruled "stands" the same day — both rulings are
	 * recorded because which one is right is the whole content of this row. The first rested on
	 * the BS 5977 arching gate (an opening arches if it has an overlapping bond and 300 mm of
	 * masonry above the apex of a 45-degree triangle on the clear span; this fixture fails it by
	 * its whole height). The second is that a published design threshold is not a collapse
	 * predictor: the 300 mm rule is a never-even-crack serviceability line carrying safety
	 * factors, and the question here is what a real wall does. Worked honestly, the wall holds.
	 *
	 * Geometry, walked off the bricklayer above, cm: cut { 0, 3, 2.75, 8.25 }, 22 of the 150 laid;
	 * jambs toothed (even courses stop at 55.75, resume 191.75; odd stop 67.00, resume 180.50);
	 * span 113.50 to 136.00 (1.2-1.4 m); head 60.00 cm of cover over the opening; piers three cells
	 * (66.5 cm) of bearing each.
	 *
	 * So there is between 32 mm and nothing above the gate's apex, against the 300 mm it wants —
	 * true as far as it goes: no thrust line fits, so the panel isn't held up by an arch.
	 *
	 * It is held up by being a deep beam, which the gate never asks about. 60 cm of bonded masonry
	 * spanning 136 cm at worst is span/depth 2.3 — deep-beam territory — carrying its own 44 bricks:
	 *
	 *     W   44 x 2.72163125 kg = 119.75 kg, x 980 = 1.1735e5 Unreal force units
	 *     M   W L / 8 = 1.1735e5 x 136 / 8 = 1.995e6 uu.cm at midspan
	 *     Z   t D^2 / 6 = 10.25 x 60^2 / 6 = 6150 cm3
	 *     f   M / Z = 324 uu/cm2, i.e. 0.0324 MPa
	 *
	 * Against the mean bond carried since the 2026-08-13 re-anchor (0.70 MPa), that is ~5% of
	 * capacity, and the model's own worst joint (c3/8.5-c4/8, the toothed corner, not the midspan)
	 * agrees at 0.0517 of mean f_x1 — the same comfortable answer by a different route. Real
	 * half-brick walls bridge 1.2 m of missing masonry routinely; a rule that forbids it is about
	 * cracking, not falling.
	 *
	 * Hence STANDS, no fall region, no survivor region. Paired against case 12 this row isolates
	 * pier width (three cells of bearing against one); on 2026-08-09 the same honest-physics rule
	 * reached case 12 too — the one-cell pier also holds (arithmetic above Case12Cuts) — so the
	 * pair separates in the margin, not the outcome, as 13/14 and 15/16 came to.
	 *
	 * This block used to end by naming case 8 as the set's one remaining "no room to arch"
	 * discriminator; on 2026-08-11 the same standing instruction reached case 8 too (the LP oracle
	 * stands the coverless course at lambda* = 324.73 through head-joint compression), so the set
	 * now has no case where arching is refused for lack of cover — what a flat or deep arch needs
	 * is an abutment that can receive the thrust, case 10's variable and the one row still red.
	 */
	const FWallRegion Case11Cuts[] = { { 0, 3, 2.75, 8.25 } };

	/*
	 * CASE 12, rewritten 2026-08-09: the same span on a one-cell pier — and, worked honestly, the
	 * pier holds.
	 *
	 * What the old row was: it cut ten cells out of twelve, varying span and pier at once against
	 * case 11 and near-duplicating case 9's collapse. It was one of the six known reds (76 dropped,
	 * 11 stranded) and named no survivors, so a model that always answered "falls" would have
	 * passed it. The 2026-08-08 review approved rewriting it as case 11's own span on a narrow
	 * pier, so pier width is the one variable between the halves of the pair.
	 *
	 * The new geometry, walked off the bricklayer above. The wall is case 11's (twelve courses,
	 * twelve cells) with case 11's cut shifted to the left end: cut { 0, 3, 0.75, 6.25 }, 22 of the
	 * 150 laid, exactly case 11's count; pier is a bonded running-bond column 21.5-32.75 cm along
	 * the wall, four courses to the springing; abutment (right) keeps five cells, 111.5 cm of
	 * solid masonry — case 11's pier grown wider, so any failure is attributable to the narrow side
	 * alone; span the same toothed 113.5-136.0 cm as case 11 under the same 60 cm of cover.
	 *
	 * One narrow pier rather than two symmetric ones, because two one-cell piers bound an
	 * eight-cell wall in which a pier failure leaves nothing standing to name (old case 12's exact
	 * weakness). Narrowing one pier keeps the wall, span and cover bit-identical to case 11 and
	 * leaves the wide side able to survive anything the narrow does.
	 *
	 * The panel is not the question — case 11 settled it on identical numbers (span/depth 2.3,
	 * M/Z ~= 0.033 MPa, a twentieth of mean bond strength). The pier is. The recorded expectation
	 * was that thrust shoves it over; worked honestly:
	 *
	 *     H       minimum thrust at r ~= 50 cm rise: H = W L/(8r) = 1174 N x 1.36 m / (8 x 0.50 m)
	 *             ~= 400 N, a third of the panel's own weight. (The solver's kern-limited r = d_e/3
	 *             = 20 cm reads H = 998 N, a deliberately 2.5x-harsher serviceability construction
	 *             — DESIGN §5.4.)
	 *     hinge   the course-0/1 bed joint, ~22 cm below the springing: overturning demand
	 *             ~= 400 N x 0.22 m = 88 N.m (kern: 220).
	 *     rigid   restoring is N x b/2. The pier's share of the upper wall plus its own courses is
	 *             550-1,050 N over b/2 = 10.75 cm: 59-113 N.m — a rigid stack sits at the line,
	 *             0.7 to 1.3.
	 *     bonded  the joint's modulus is 790 cm3, so mean flexural bond (0.4-0.8 MPa) adds
	 *             316-632 N.m of capacity: four to eight times the honest demand, still 1.7-3.4x
	 *             the kern-limited thrust. Sliding never governs (cohesion alone is 4,400 N, ten
	 *             times H).
	 *
	 * So only the rigid-block / characteristic-vs-kern-thrust reading condemns this pier — the
	 * never-even-crack design stack, already rejected three times in this catalogue (cases 14, 16,
	 * the free-end ruling): an uncracked bonded section carries what a rigid block cannot. Verdict:
	 * STANDS, no fall region, no survivor region.
	 *
	 * The model agrees: measured 2026-08-09, it drops 0, breaks 0, strands 0, worst joint 0.362067
	 * at c3/0.5-c4/1 — within 0.03% of case 11's 0.362193, the same half-seat eccentricity at a
	 * reveal corner. The solver carries thrust as springing shear only, never as a moment down the
	 * pier (DESIGN §7 item 6), so it reads no pier-width term at all — the margin separation exists
	 * only in the arithmetic above, which is why the discrimination waits on the leaning stack and
	 * the LP oracle.
	 *
	 * At self-weight there is no honest fixture in this family where a pier fails while its span
	 * survives — thrust scales with the panel's weight while pier capacity scales with the wall on
	 * it. The pair against case 11 separates in the margin rather than the outcome, and the
	 * MatchedPairs row asserting the outcome half retired with the same dating. The LP oracle has
	 * since paid that debt: 128.12 on three cells of bearing against 89.12 on one, 1.44x, pinned in
	 * OracleSweepFull.RigidBlock.WallsAndLadders.
	 */
	const FWallRegion Case12Cuts[] = { { 0, 3, 0.75, 6.25 } };

	/* --- D: corbelling and the projecting header. ----------------------------------- */

	constexpr int32 CorbelCells = 8;
	constexpr int32 CorbelFirstCourse = 6;
	constexpr double QuarterBrickStepCm = HalfCellCm * 0.5;
	constexpr double HalfBrickStepCm = HalfCellCm;

	/*
	 * CASE 14, re-derived: a bonded four-step corbel stands at 0.19516 and nothing comes down.
	 *
	 * The verdict was COLLAPSE until 2026-08-07, on a rigid-body overturning reading ("the
	 * resultant walks outside the bed below") — geometrically true here, stated below for the
	 * record. It is overruled because this project models an uncracked bonded section, and the
	 * user had already ruled on 2026-08-06 that a brick deleted at a free end must not bring a
	 * wall down. Any rule that honours that also honours this corbel:
	 * `Core.Structure.ACorbelResistsWithItsWholeDepth` stands a five-step raking corbel at 0.219,
	 * and a five-step and four-step corbel are the same fixture with different provenance — a
	 * threshold between them would make two contradictory statements both true.
	 *
	 * What the fixture presents, walked off the bricklayer above. CourseGeometry pushes each
	 * course from 6 upward one more half cell (11.25 cm) out, closing with a full brick, so every
	 * one of the four end bricks overlaps exactly one piece below it over 10.25 cm — the same
	 * square half seat the raking staircase corbel presents — with its centre of mass 5.625 cm
	 * outboard of that patch's centroid: four steps of the staircase fixture instead of eleven.
	 * (This is also the overturning claim: mass acts 5.625 cm out on a patch only 5.125 cm wide, so
	 * the resultant is outside the bearing. Bond carries it.)
	 *
	 * The ladder is the staircase topology: each step takes its own weight, all of the step above,
	 * and half of the next brick along, whose share grows the same way. Section is the depth of
	 * bonded masonry over the joint, W = t D^2/6, taken at the lesser of that and the bed patch's
	 * own 179.4817708 cm3:
	 *
	 *     course   s   F (weights)   M (weight.cm)   courses over   reads
	 *        9     0        1             5.625            1        0.058203838   (patch governs)
	 *        8     1        2.5          22.5              2        0.156128700
	 *        7     2        4.5          56.25             3        0.173476333
	 *        6     3        7           112.5              4        0.195160875   <- the worst
	 *
	 * 0.195 of f_xk1 is a corbel standing at a fifth of what holds it: STANDS, no fall region, no
	 * survivor region. Asserted in its own test below rather than left as a comment, since "stands"
	 * is satisfied by 0.195 and by 0.0001 alike.
	 *
	 * Cross-checked against two figures this file did not produce: the same arithmetic gives
	 * 0.21858 for a five-step corbel and 0.36903147272727271 for the eleven-step staircase —
	 * ARCHING_DESIGN.md's published 0.219 and `Core.Structure.AStaircaseVoidCondemnsTheCorbel`'s
	 * pin to seventeen digits. Both asserted below so a drifted derivation fails here.
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

	/*
	 * CASE 16, re-derived: a bonded header with nothing on it stands at 0.058204.
	 *
	 * The verdict was LOCAL LOSS until 2026-08-07 and was revised in the catalogue without this
	 * file following — corrected here by re-deriving the number rather than flipping the word.
	 *
	 * The header projects half a cell (11.25 cm) past the face, keeping 10.25 cm of bearing on the
	 * brick below, centre of mass 5.625 cm outboard of that patch's centroid. On a 10.25 x 10.25 cm
	 * patch (section modulus 179.4817708 cm3):
	 *
	 *     2667.198625 x 5.625 / 179.4817708 = 0.0083591 MPa   bending, opening the outer edge
	 *     2667.198625 / 105.0625            = 0.0025387 MPa   its own weight, closing it
	 *     tension 0.0058204 / 0.1 (f_xk1)   = 0.058204        of flexural bond capacity
	 *
	 * — a brick standing at six percent of what holds it, not one falling off. The abandoned "local
	 * loss" was a rigid-body overturning reading (the resultant is 5.625 cm out on a patch only
	 * 5.125 cm wide, so it does lie outside the bearing) — the same error case 14 was corrected for
	 * the same day: this project models an uncracked bonded section, which cured mortar carries.
	 *
	 * No fall region to name; the row carries no MustFall and no MustStand.
	 *
	 * The pair had to move with it: 15 and 16 differ only in what sits on the header's tail, and
	 * both now stand, so as an outcome pair they discriminate nothing (as 13/14 stopped doing).
	 * What still separates them is the reading on one joint — 0.00184 for case 15 against 0.05820
	 * for case 16, 32x from superimposed load alone, case 15's tension driven to zero and
	 * compression left governing — asserted in
	 * Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome. The row was removed from the
	 * outcome-pair test (Acceptance.Wall.MatchedPairs, since retired entirely 2026-08-12) rather
	 * than left there unsatisfiable.
	 */

	/* --- E: bond pattern and head-joint shear. --------------------------------------- */

	const FWallRegion Case18Cuts[] = { { 5, 5, 4.75, 5.25 } };

	/* --- F: losing the base, and the staircase void. --------------------------------- */

	/*
	 * CASE 19, re-ruled 2026-08-12: the underpinned half cantilevers. The closest call of the
	 * three, and ruled STANDS knowingly.
	 *
	 * The geometry is six whole cells, not the 5.75 an earlier draft of this block claimed.
	 * { 0, 0, -0.50, 5.25 } cuts course 0, an even course of twelve whole bricks (no half bat —
	 * the fixture lays right to left, so the short closing piece falls on odd courses), so the
	 * region takes cells 0..5, six whole bricks, matching the run's own `cut 6`. That is
	 * 6.0 x 22.5 = 135 cm of footing gone under a ten-course, twelve-cell wall, nine courses
	 * standing over the void. The old "5.75 cells (129 cm)" mixed a piece count into a cell span
	 * and read 4% light; every figure below is re-derived from 6.0.
	 *
	 * What the row used to claim: COLLAPSE of { 1, 9, -1.00, 4.60 } with { 0, 9, 7.75, 13.0 }
	 * standing. The survivor region reached two and a half cells clear of the cut on purpose: a
	 * failure there would be the 33.69-degree spreading front walking further than the missing
	 * support can account for. The fall region went with the ruling; the survivor region did not
	 * (see below).
	 *
	 * The ruling. As of the 2026-08-12 sweep:
	 *
	 *     catalogue   COLLAPSE — the masonry over the void has no path to the earth
	 *     production  drops 34, six stranded, in zero cascade passes at a worst reading of 0.31804
	 *                 (c0/6-c1/5.5). As with case 10: nothing broke
	 *     LP oracle   STANDS at lambda* = 12.3824832 — the lowest of the fifteen walls measured,
	 *                 still above 1 (119 blocks / 308 joints; 2,970 pivots in 16.1 s, measured
	 *                 pre-partial-pricing 2026-08-12, cost figures since moved)
	 *
	 * The user ruled STANDS on 2026-08-12, recorded as the closest call in the set — the hand
	 * check straddles the verdict rather than settling it:
	 *
	 *     the arithmetic  nine courses over 6.0 cells is 54 brick weights = 1440 N at a 67.5 cm
	 *                     lever, M = 972 N.m; section t D^2/6 = 7784 cm^3, giving 0.125 MPa.
	 *     and it lands    1.25x characteristic f_xk1 (over capacity), 0.31x f_xk2 (comfortably
	 *                     under), ~0.21x the mean basis (0.4-0.8). One published number condemns
	 *                     it, two acquit it — a judgement, written down as one. The correction
	 *                     from 5.75 to 6.0 cells moved every figure and no conclusion (0.115 became
	 *                     0.125 MPa, the straddle 9% wider on the condemning side).
	 *     the caveat      it is an end cantilever, not a span between two supports — no second
	 *                     support to redistribute to, so of the three fixtures ruled 2026-08-12
	 *                     this is the one most plausibly wrong.
	 *     the anchor      real practice underpins in ~1 m bays and expects the wall above to
	 *                     bridge them; 135 cm is longer, but rests on the wall being bonded over
	 *                     nine courses, which a bay of underpinning also relies on.
	 *     the finding     the same zero that decided case 10 applies: production reaches 34
	 *                     dropped without breaking a joint — unroutability, not strength, so not a
	 *                     third opinion but the same missing mechanism reported twice.
	 *
	 * If physical evidence ever contradicts this row it should come back through DESIGN §8 as a
	 * further ruling, not a quiet edit restoring the fall region this block deleted.
	 *
	 * What survives the ruling: the survivor region, restored verbatim as { 0, 9, 7.75, 13.0 }. A
	 * STANDS verdict says nothing comes down anywhere, so naming the still-footed right half is
	 * true and stronger than "thirty-four fell". It matters more here than elsewhere because of the
	 * shape of the wrong answer: production's 34 are the spreading front walking out of the missing
	 * footing, and a routing change shifting that front three cells right would keep every count pin
	 * on this row unchanged — counts cannot see a translation, the region can. Asserted inverted (no
	 * piece inside may be among the fallen). Verified: all 34 sit at cell 4.5 and below, clear of
	 * the region and the cut.
	 *
	 * The row stays red, in the inverted direction — expected STANDS, measured 34 dropped. Like
	 * case 10 it keeps its pins (`DropsToday = 34`, `StrandsToday = 6`) and its caption marker, and
	 * goes green at evolution step 4.
	 */
	const FWallRegion Case19Cuts[] = { { 0, 0, -0.50, 5.25 } };
	const FWallRegion Case19Stands[] = { { 0, 9, 7.75, 13.0 } };

	/*
	 * CASE 20 — the staircase void, the one case in the set drafted with a question mark. The user
	 * ruled on 2026-08-06: LOCAL LOSS, the loose toothed bricks at the cut edge drop and the mass
	 * of the wall stands.
	 *
	 * The raking cut, one region per course, reading up: each course above the last is cut one
	 * cell less far right, so the surviving masonry to the right steps left over the hole as it
	 * rises — the overhang in the screenshot.
	 *
	 * Which bricks are the teeth, worked from the geometry. An even-course brick at cell k sits on
	 * the odd course below at k - 0.5 and k + 0.5; an odd-course brick at k + 0.5 sits on k and
	 * k + 1. Walking the surviving pieces against the cuts, exactly two are left with no bed patch
	 * at all: course 3/cell 4.5 (over cut cells 4 and 5) and course 5/cell 2.5 (over cut cells 2
	 * and 3). Everything else along the cut face keeps at least one patch — a corbel, not a tooth.
	 * Naming those two by identity rather than count matters: a rule dropping the corbelled
	 * half-seats instead would fall the same number of bricks and be completely wrong.
	 *
	 * The fall set drawn in WALL_CASES.html is not usable and deliberately not transcribed: its
	 * region overlaps its own cut regions almost entirely, rendering one brick in course 7 that
	 * nothing in the prose claims. The prose is what was agreed.
	 *
	 * Re-examined and confirmed 2026-08-12 — a ruling that moved nothing, worth recording because
	 * the other three rows examined beside it (9, 10, 19) all moved. What the measurement said:
	 *
	 *     production  unlike cases 10 and 19 this one is a strength verdict: one cascade pass, a
	 *                 worst reading of 42.71, a joint genuinely 42x over capacity — the raking cut
	 *                 leaves teeth hanging rather than pieces merely unrouted. 42.71 is the
	 *                 pre-cascade peak; `ReportWallCase` reads `Result.Worst` after the cascade, by
	 *                 which time that joint has given, and the survivors' worst is 0.296506 — one
	 *                 structure, two instants.
	 *     LP oracle   lambda* = 82.629597, but a global load factor has no local vocabulary: it
	 *                 says every block including both teeth has some admissible equilibrium, and
	 *                 cannot say whether nine bricks or two come down. By hand a tooth is cheap to
	 *                 hang — 0.1 MPa over its head joints and bed patches is 3434 N against a
	 *                 brick's 26.67 N, ~129x — so the named teeth are not what the LP is measuring.
	 *
	 * The standing doubt survives the measurement untouched: the true count is more than the two
	 * named and fewer than the model's nine, and the LP cannot narrow it — it waits for equilibrium
	 * promotion (DESIGN §7 step 4), not another ruling.
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

	/* --- G: the counter-cases — an opening that is genuinely too big for what covers it. ------ */

	/*
	 * SECTION G, user-directed 2026-08-12, built 2026-08-13: the two rows that put a falling
	 * verdict back in the set, the catalogue's first two `Collapse` rows of any kind.
	 *
	 * Why they exist. Five re-rulings in five days took every falling verdict out of the
	 * catalogue: case 11 (2026-08-08), case 8 (2026-08-11), cases 9, 10 and 19 (2026-08-12). Each
	 * was right on its own numbers and each cost the set a discriminator — "no case refuses
	 * arching for lack of cover" (case 8's cost) and "no case refuses a span for want of rise"
	 * (case 9's). The user directed the replacement on 2026-08-12: two counter-cases to case 9, one
	 * starving the cover and one growing the span, each sized so the verdict clears the mean-basis
	 * ceiling rather than sitting in the bracket where a ruling would be a judgement call.
	 *
	 * The one criterion both rows are sized on, the same three lines every §8 ruling since case 11
	 * has argued with. The masonry over an opening is a deep beam carrying its own weight between
	 * the two jambs:
	 *
	 *     w = (courses of cover) x (one brick weight) / (one cell pitch)     load per cm of span
	 *     M = w L^2 / 8                                                      simply supported
	 *     Z = t D^2 / 6                                                      t = 10.25, D = cover
	 *     sigma = M / Z
	 *
	 * so sigma is proportional to L^2/D. Read against characteristic f_xk1 = 0.10 MPa (what the
	 * profiles carry today), f_xk2 = 0.40 (a toothed vertical crack path's real strength), and the
	 * mean basis 0.4-0.8 MPa DESIGN §3 adopted 2026-08-08. A verdict clears the ceiling when sigma
	 * exceeds 0.8 — the top of the mean bracket — because then no basis this project quotes can
	 * acquit it.
	 *
	 * The ladder, with a stood row at the bottom so the criterion isn't self-serving:
	 *
	 *     case 8     one course over a 4-cell opening    L = 79.75   D = 7.5    0.098 MPa   0.12x
	 *     case 9     eight courses over a 10-cell one    L = 214.75  D = 60     0.089 MPa   0.11x
	 *     case 21    two courses over an 18-cell one     L = 394.75  D = 15     1.201 MPa   1.50x
	 *     case 22    eight courses over a 35-cell one    L = 777.25  D = 60     1.164 MPa   1.46x
	 *
	 * (last column against the 0.8 MPa ceiling). The two stood rows sit at an eighth of the
	 * ceiling, the two new rows at half again above it — 12x, not a boundary to adjudicate.
	 *
	 * Every figure in that ladder derives from those three lines, including case 9's: both new
	 * blocks below reconcile their sigma against case 9's 0.089 MPa through sigma ~ L^2/D, which is
	 * the same formula re-expressed (catches an arithmetic slip, is not a second method). Nothing
	 * about case 9 was measured at 0.089 MPa — what was measured is production's worst joint
	 * reading of 0.985 of capacity, pinned in Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome.
	 * The three genuinely independent methods on these rows are hand statics, production, and the
	 * LP oracle — and on case 21 the third one disagrees.
	 *
	 * THE THIRD METHOD DISAGREES ON CASE 21, AND WHAT IT PRICES INSTEAD IS NOT KNOWN.
	 *
	 * The rigid-block LP oracle stands case 21 at lambda* = 5.511 (measured 2026-08-13, 83 blocks /
	 * 133 joints, 2,754 pivots, 1.3 s) — the first time in this catalogue the LP is the outlier
	 * (the last four rulings all moved the catalogue toward it), pinned as a measured relation
	 * (`OracleStandsProductionFalls` in OracleSweepFull.RigidBlock.WallsAndLadders).
	 *
	 *   What is established, and it is less than an earlier draft claimed: the LP is not pricing
	 *   the mechanism this row's verdict is argued on. A 15 cm panel on characteristic flexural
	 *   bond fails at lambda = 0.10/1.2014 = 0.0832, so the oracle's 5.511 is 66x stronger than the
	 *   tension-bond panel the hand check condemns. What carries this wall in the LP's world has
	 *   not been identified; the identification that used to stand here was withdrawn 2026-08-13.
	 *
	 *   Two measured ladders, both run 2026-08-13 on the case-21 family (span L = 394.75 +
	 *   (cells - 22) x 22.5):
	 *
	 *     cover, held at 22 cells   2 crs 5.511   4 crs 7.683   6 crs 9.183   8 crs REFUSED
	 *     span, held at 2 courses   18 c 10.408   22 c 5.511   27 c 3.102   32 c 1.985
	 *                               40 c  1.143   45 c 0.863
	 *
	 *   The hypothesis priced was a full-wall-height arch whose thrust dives through the jambs into
	 *   the fixed course 0: at 22 cells, H = W L/(8r) = 936 x 394.75/(8 x 30) = 1,539 N against jamb
	 *   bed joints affording 8,440 N, i.e. 5.48 against the LP's 5.511 — one agreeing point. Walked
	 *   along the cover ladder it was fitted on, the same arithmetic contradicts the measurement:
	 *   adding cover grows W in proportion while rise grows only with the wall, so the mechanism
	 *   predicts lambda* falling by a third where the measurement rises by two thirds (wrong
	 *   direction, out by 1.9-2.5x). A mechanism that reproduces one point and mispredicts the
	 *   ladder has been curve-fitted, not identified.
	 *
	 *   The two observations the withdrawn reading leaned on discriminate nothing: "lambda* rises
	 *   with cover while sigma falls" is true of any mechanism that improves with depth (a load
	 *   factor and a stress move oppositely by construction), and "lambda* x L^2 is roughly
	 *   constant" is exactly what a beam does too (sigma ~ L^2 at fixed D) — and it isn't even very
	 *   constant, drifting 26% across the full span ladder.
	 *
	 *   So the row records an open disagreement, not a diagnosed one. The LP stands a wall hand
	 *   statics and production both condemn, by a mechanism at least 66x stronger than the bond,
	 *   and nobody has named it — a charity of the oracle (free foundation restraint, a third
	 *   charity beside the sweep header's plastic limit and full redistribution) or a real load
	 *   path the downward-routing solver can't see is a user call not yet made (CURRENT_STATE).
	 *   This catalogue's verdict does not wait on it: the row is sized on the user's mean-basis
	 *   criterion, and sizing on the oracle was never available anyway — lambda* crosses 1.0 only
	 *   between 40 and 45 cells (an 8-9 m opening under two courses), and even there 0.863 x 6 is
	 *   5.2 and still stands on the basis the rulings use.
	 *
	 *   Two ladders would discriminate and neither is built (~1.3 s a rung, both under a minute):
	 *   the rise ladder (hold span/cover, vary courses below the opening 1..4 — an arch from ground
	 *   predicts lambda* rising with it, a cover-carried mechanism predicts flat; settles the
	 *   withdrawn reading directly) and the abutment ladder (widen jambs 2 to 4 cells — a
	 *   jamb-reacting mechanism roughly doubles capacity, a cover-carried one barely moves).
	 *   Specified and logged in CURRENT_STATE, deferred deliberately. Until one runs, every
	 *   attribution here is a hypothesis.
	 *
	 * Case 22 now has an oracle verdict (2026-08-15): the LP stands it at lambda* =
	 * 8.4149459982219277 (see its own block). Production is unambiguous on both rows: 45 and 254
	 * pieces down, three and two cascade passes, joints 3.8429 and 8.2241 over capacity before the
	 * cascade starts — nothing here is the zero-pass unroutability that decided cases 10 and 19.
	 *
	 * Where the two pre-cascade readings come from, since neither is a figure this file computes.
	 * `FWallResult::Worst` reads after the cascade, a different quantity; both readings below are a
	 * one-off `SolveLoads` read — loads solved once, before any joint gives, worst finite
	 * utilisation taken off the result:
	 *
	 *   CASE 21 — the slow sweep's wall-21 row does this on every run: `SolveLoads`, scan, then
	 *           cascade. Printed 3.842883954008586 at the characteristic data (2026-08-13) and
	 *           0.93542327561664174 at the mean data (2026-08-14) — not the 3.8429/7 = 0.549 the
	 *           fallout plan predicted, because the worst joint's governing axis flipped, leaving
	 *           the wall a knife-edge 6.5% under the line. Reported, not asserted, so it drifts
	 *           visibly. Which axis it flipped to is recorded as squeezed-edge compression and is
	 *           unverified — the same attribution for case 9's jamb and case 22's worst joint both
	 *           decomposed (2026-08-14) to Mohr-Coulomb shear instead, and case 21's joint is the
	 *           same jamb-bed shape. Decomposing it is logged in CURRENT_STATE.
	 *   CASE 22 — no sweep row (the oracle refuses the fixture), so its 8.2241 was taken by hand at
	 *           the characteristic data on 2026-08-13. Computed here now, on every run:
	 *           `SolveWallCase` solves loads once on the cut structure before the cascade, and
	 *           `FWallResult`'s pre-cascade fields carry the reading, pinned at 2.9133264370386338
	 *           (`FWallCase::PreCascadeWorstToday`). The fallout plan's re-base of 8.2241/7 = 1.1749
	 *           was wrong by 2.5x, as it warned it might be (/7 is the tension-axis factor; the
	 *           measured ratio between eras is 2.82). The collapse is three passes and 316 down
	 *           (two passes, 254, at the characteristic data — the marginal cascade sheds more, not
	 *           fewer; the region pins hold because Falls/Stands are containment claims).
	 *
	 * Case 21's reading is quoted as evidence, asserted nowhere; case 22's is asserted twice — past
	 * 1.0 at all, and at its measured size. This file also asserts the drop/survivor sets and pass
	 * counts (case 22) and the DropsToday/StrandsToday zeros of the inverted red (case 21).
	 */

	/*
	 * CASE 21 — two courses over an eighteen-cell opening: the cover counter-case.
	 *
	 * What it is for. Case 8's ruling cost the set its last case where arching is refused for want
	 * of cover, and recorded the requirement: a fixture where the cover genuinely cannot carry what
	 * stands on it. The user's constraint is a minimum of two courses (2026-08-12; one course is
	 * case 8, ruled to stand), so cover is fixed at two and span is moved until the verdict clears
	 * the ceiling.
	 *
	 * Geometry, walked off the bricklayer above, cm: wall six courses of 22 cells, 135 laid; cut
	 * { 1, 3, 1.75, 19.25 }, 52 of the 135, 83 live; jambs two cells each side, case 9's jamb
	 * exactly; reveals toothed (even courses stop at 33.25, resume 439.25; odd stop 44.50, resume
	 * 428.00); span 383.50 to 406.00, mean 394.75; head 15.00 cm (two courses) of masonry over the
	 * opening.
	 *
	 * The arithmetic, on section G's three lines:
	 *
	 *     w      2 courses x 26.67198625 N / 22.5 cm            = 2.3708432 N/cm
	 *     W      w x 394.75                                     = 935.89 N  (35.1 brick weights)
	 *     M      W L / 8 = 935.89 x 394.75 / 8                  = 461.80 N.m
	 *     Z      t D^2 / 6 = 10.25 x 15^2 / 6                   = 384.375 cm3
	 *     sigma  M / Z = 46,180 N.cm / 384.375 cm3              = 120.14 N/cm2 = 1.2014 MPa
	 *
	 * And it lands: 1.50x the 0.8 MPa ceiling, 3.00x f_xk2, 12.01x characteristic f_xk1 — every
	 * basis this project quotes condemns it, which is what "clears the ceiling" asked for.
	 *
	 * Reconciled with case 9's figure, an arithmetic check rather than a second method: sigma ~
	 * L^2/D, so from case 9's 0.0889 MPa at L = 214.75, D = 60: (394.75/214.75)^2 x (60/15) =
	 * 13.516, and 0.0889 x 13.516 = 1.2016 — the same number to 0.02%. Case 9's 0.0889 comes out of
	 * the same three lines, so this only confirms the multiplication, not the physics elsewhere
	 * (section G's ladder block).
	 *
	 * Why the flat arch does not rescue it (the reading that stood case 8, priced rather than
	 * ignored): the solver's kern-limited rise is d_e/3 = 5 cm, so H/V = 3L/(4 d_e) = 19.74; even
	 * at the most generous rise (r = 7.5), H/V = 13.2 and H = 6,159 N. The springing bed joints
	 * would carry that as 0.154 MPa of shear against Mohr-Coulomb capacity 0.209 (friction 4%,
	 * cohesion the rest). Case 8's identical check reads 0.0125 against the same 0.209 — 6%. So
	 * this fixture's flat arch is 12x nearer its limit than the one the 2026-08-11 ruling stood,
	 * held by the bond term DESIGN §5.2 says is gone the moment the joint cracks (demand/capacity
	 * 18 at c = 0, the same shape as dry stone's 1.2372 in StructureThrustTest). Production reaches
	 * that conclusion and drops the cover; the LP stands it through something that is not this
	 * arch (5.511 is 66x what the bond can hold) but has not been identified — section G's ladder
	 * block.
	 *
	 * What comes down, and why the region is the seatless set rather than a drawn wedge: a brick in
	 * even course 4 at cell k sits on odd course 3 at k - 0.5 and k + 0.5, and the cut takes
	 * 2.5..18.5 of that course, so every course-4 brick from cell 2 to 19 has no bed patch at all
	 * (the case-20 shape). Course 5 rides on course 4 and goes with it. `Case21Falls` is that set,
	 * { 4, 5, 1.75, 19.25 }, 35 pieces, a lower bound: the two courses over each jamb are
	 * deliberately not claimed either way, since where a two-course panel tears as it drops isn't
	 * something this catalogue can rule. (Production drops those too — all 45 of courses 4 and 5.)
	 *
	 * What must keep its footing is the two jambs, making this a COLLAPSE rather than "everything
	 * falls": `Case21Stands` names courses 0..3 of both, twenty bricks, so a model answering
	 * "falls" to everything could not pass this row. Verified: production's 45 are all in courses 4
	 * and 5, clear of every named survivor.
	 *
	 * Production, measured 2026-08-13 and agreeing: 45 down, zero stranded, three cascade passes,
	 * worst joint 3.8429 before the cascade starts (a one-off read, see section G's provenance
	 * note). The zero and the three separate this row from cases 10 and 19 (which reach their drop
	 * counts in zero passes with nothing near capacity — a router with nowhere to send load); this
	 * is a strength verdict, joints genuinely nearly four times over capacity. The three is
	 * asserted: the collapse arm of `Acceptance.Wall.Catalogue` requires a broken joint, so this
	 * row cannot quietly become an unroutability of the same shape.
	 */
	const FWallRegion Case21Cuts[] = { { 1, 3, 1.75, 19.25 } };
	const FWallRegion Case21Falls[] = { { 4, 5, 1.75, 19.25 } };
	const FWallRegion Case21Stands[] = { { 0, 3, -1.0, 1.75 }, { 0, 3, 19.25, 22.0 } };

	/*
	 * CASE 22 — case 9's own cover over a thirty-five-cell opening: the span counter-case.
	 *
	 * What it is for. Case 9's ruling cost the set its last case refusing a span for want of rise.
	 * This is case 9's fixture with one variable moved — the span: same brick, mortar, bond,
	 * toothed reveal, eight courses (60.00 cm) of cover, two-cell jambs, same cut through courses
	 * 1..3. Only the opening is wider.
	 *
	 * Geometry, walked off the bricklayer, cm: wall twelve courses of 39 cells, 474 laid; cut
	 * { 1, 3, 1.75, 36.25 }, 103 of the 474, 371 live; reveals (even courses stop at 33.25, resume
	 * 821.75; odd stop 44.50, resume 810.50); span 766.00 to 788.50, mean 777.25 — 3.62x case 9's
	 * 214.75; head 60.00 cm of bonded masonry, case 9's cover to the millimetre.
	 *
	 * The arithmetic:
	 *
	 *     w      8 courses x 26.67198625 N / 22.5 cm            = 9.4833730 N/cm
	 *     W      w x 777.25                                     = 7,371.5 N  (276.4 brick weights)
	 *     M      W L / 8                                        = 7,161.3 N.m
	 *     Z      t D^2 / 6 = 10.25 x 60^2 / 6                   = 6,150 cm3
	 *     sigma  M / Z = 716,133 N.cm / 6,150 cm3               = 116.44 N/cm2 = 1.1644 MPa
	 *
	 * And it lands: 1.46x the 0.8 MPa ceiling, 2.91x f_xk2, 11.64x characteristic f_xk1.
	 *
	 * The reconciliation against case 9 is exact here, the point of keeping cover fixed: with D
	 * held, sigma ~ L^2 alone, (777.25/214.75)^2 = 13.098, and 0.0889 x 13.098 = 1.1644 to five
	 * figures — this only confirms the arithmetic (section G's block), not the physics elsewhere.
	 * The pair against case 9 is a real one-variable pair and separates on outcome — the first
	 * since 2026-08-12 — 0.089 MPa standing against 1.164 MPa falling, from a 3.62x span alone.
	 *
	 * Why deep-beam action is still the right model at this span: span/depth was 3.6 at case 9 and
	 * is 13.0 here — no longer a deep beam but an ordinary beam, exactly what W L/8 prices, and if
	 * anything charitable (a deep beam redistributes toward an arch, a slender one cannot).
	 *
	 * What comes down. Course 4 is the seatless course (every brick cell 2..36 sits on two cut
	 * cells), narrowing above as masonry corbels in from each reveal. `Case22Falls` is two regions,
	 * a lower bound: { 4, 4, 1.75, 36.25 } the whole seatless course, { 5, 11, 6.25, 31.75 } the
	 * core of the seven courses over it, four and a half cells clear of each reveal (where a
	 * stepping edge can hang on, which the catalogue cannot rule). 214 of the 254 production drops
	 * named.
	 *
	 * The second region's bound was tightened one step on 2026-08-13: production's drop set narrows
	 * exactly half a cell a course (the running-bond corbel step), and the bound as first drawn
	 * (5.25..32.75) claimed the same outermost bricks (5.5, 32.5) as the top course's actual fallen
	 * ones — course 11 was the exact last course the claim could hold, an over-claim one course
	 * further. 6.25..31.75 gives one whole cell of margin (holds through a hypothetical course 13)
	 * at the cost of fourteen named bricks out of 228 — deliberately loose near the reveal, where
	 * the catalogue has already said it cannot rule the tear.
	 *
	 * What must keep its footing: courses 0..3 of both jambs, sixteen bricks, { 0, 3, -1.0, 1.25 }
	 * and { 0, 3, 36.75, 39.0 } — tighter than case 21's by one piece a side, because the odd-course
	 * brick at the very top of each jamb is half seated and production drops both; believable when
	 * 7.8 m of wall comes off a jamb, so the catalogue declines to claim it either way.
	 *
	 * Production, measured 2026-08-13 and agreeing: 254 down, zero stranded, two cascade passes,
	 * worst joint 8.2241 before the cascade (a one-off read, section G's provenance note). A
	 * strength verdict, not unroutability; the two is what the collapse arm of
	 * `Acceptance.Wall.Catalogue` requires — at least one joint given, or this is cases 10/19 again.
	 *
	 * The oracle had no verdict here until 2026-08-15; the refusal this block used to record (371
	 * blocks / 900 joints, 49,557 pivots, 546 s, "phase-2 simplex failed") was measured at the
	 * characteristic strengths, one day before the mean re-anchor changed every number the LP
	 * reads. Re-measured at today's profiles it answers: lambda* = 8.4149459982219277, 371 blocks /
	 * 900 joints, 88,810 pivots, ~1,197 s, `bAnswered` true, unchanged by slice 0a's solver fix.
	 *
	 * So the row's third method has arrived, and dissents: catalogue rules Collapse, hand statics
	 * and production agree, and the LP stands the wall at 8.41x its own weight — the same shape of
	 * disagreement as case 21's (17.24), on the same kind of fixture, with the same rigid-plastic
	 * scope limit (DESIGN §8, 2026-08-13) as the standing explanation until priced. The verdict
	 * does not move: PROMOTION_DESIGN §4 measures why no brittleness treatment plausibly brings a
	 * reading this size under 1.0, and case 21 is the precedent that the catalogue row stands.
	 *
	 * What a sweep row would cost: `ERelation::OracleStandsProductionFalls` (case 21's enumerator)
	 * with a lambda window around 8.4149459982219277 beside production's 254/0/2, in
	 * `RigidBlockOracleSweepTest.cpp`'s wall table. Twenty minutes a run on an opt-in group that is
	 * thirteen minutes today — more than doubling it to add a second reading of a disagreement case
	 * 21 already pins at a fifth of the cost — so this is a specification, not a row, pending a
	 * reason to watch this wall specifically and a solve that fits.
	 *
	 * The hazard that comes with the answer: 88,810 pivots against MaxPivots 100,000 is 89% of the
	 * termination cap — any pivot-path change could push this wall to a refusal by a different
	 * route. Nothing in the suite watches that number; CURRENT_STATE carries it.
	 *
	 * What the old extrapolation said, kept because it predates the measurement and is now
	 * checkable: the cover ladder at 22 cells (5.511/7.683/9.183 at two/four/six courses,
	 * characteristic data) suggested lambda* falls roughly as 1/L^2 and eight courses at 777 cm
	 * "would land near 2.6". Direction right, number wrong — the measured 8.41 is 3.2x the
	 * extrapolation, on data the re-anchor moved underneath it. An extrapolation of a curve is not
	 * a prediction of a mechanism.
	 */
	const FWallRegion Case22Cuts[] = { { 1, 3, 1.75, 36.25 } };
	const FWallRegion Case22Falls[] = { { 4, 4, 1.75, 36.25 }, { 5, 11, 6.25, 31.75 } };
	const FWallRegion Case22Stands[] = { { 0, 3, -1.0, 1.25 }, { 0, 3, 36.75, 39.0 } };

	/**
	 * The catalogue, built rather than aggregate-initialised so every field is named at its value.
	 *
	 * TWENTY-TWO CASES, TWENTY-ONE OF THEM RECTANGLES WITH HOLES IN. Nothing here is a shape
	 * somebody invented to break the solver: cases 1-5 are the deletions that fire on every click,
	 * 6-10 are doorways, 11-12 are a wall on piers, 13-16 are corbelling, 17-18 are the bond, 19-20
	 * are where collapse is the right answer, and 21-22 are openings too big for what covers them.
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
		 * Case 3 is the one a player reported; the user ruled 2026-08-06 that a brick deleted at a
		 * free end must not bring the wall down. ARCHING_DESIGN.md's slices 1-4 don't reach it — the
		 * surviving brick overhangs outward with nothing to abut against, so the arch is correctly
		 * refused and the cantilever ladder starts — so this row stays red until composite vertical
		 * action lands. Not a defect in the row.
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
		 * The wall the other three rows of section B compare against. As of 2026-08-12 none of the
		 * three pairs separates on outcome any more (cover with case 8's re-ruling, span and
		 * abutment with cases 9 and 10) — the file header lists where each discrimination went.
		 */
		Add(7, TEXT("Four-brick opening, eight courses over"), EVerdict::Stands,
			CoveredCourses, StandardCells, FourCellOpening, {}, {},
			TEXT("span vs 9 — NOW IN THE READING; cover vs 8 and abutment vs 10 NO LONGER SEPARATE"));

		/*
		 * STANDS — re-ruled 2026-08-11, the catalogue is what moved. Asked for LOCAL LOSS of the two
		 * seatless middle bricks on the reading that one course of cover cannot arch; the LP oracle
		 * stands it at lambda* = 324.73 through head-joint compression, production drops nothing,
		 * and the user ruled STANDS per DESIGN §8. Full derivation in the CASE 8 block above
		 * Case9Cuts. No fall region.
		 */
		Add(8, TEXT("Four-brick opening, one course over"), EVerdict::Stands,
			5, StandardCells, FourCellOpening, {}, {},
			TEXT("depth of cover against case 7 — NO LONGER SEPARATES, on either outcome or reading"));

		/*
		 * STANDS — re-ruled 2026-08-12, row goes green. Asked for COLLAPSE on the published arching
		 * gate; the LP oracle stands it at lambda* = 36.56, hand deep-beam reads 0.88x characteristic
		 * (~0.15x mean), production already stood it at 0.985. Three methods, one outlier — the
		 * catalogue. Full derivation in the CASE 9 block above. No regions, no `DropsToday`.
		 */
		Add(9, TEXT("Ten-brick opening, eight courses over"), EVerdict::Stands,
			CoveredCourses, 14, Case9Cuts, {}, {},
			TEXT("span against case 7 — NOW IN THE READING, not the outcome"));

		/*
		 * The three known reds each carry a characterisation of today's wrong answer (see
		 * FWallCase::DropsToday), and since 2026-08-12 they don't all point the same way: 20 is a
		 * LOCAL LOSS of two named teeth where the model drops nine (too many); 10 and 19 are rows
		 * the catalogue rules STANDS while the model drops 12 and 34 (inverted red) — both go green
		 * at DESIGN §7's evolution step 4, as does 20 for a different reason (over-count, not an
		 * absent mechanism). Cases 12, 8 and 9 left this set by their expectation moving rather than
		 * a fix. Two of the three (10) also strand — see FWallCase::StrandsToday.
		 */
		/*
		 * GREEN since slice 3b/4 (2026-08-27): below the 200-block cap the equilibrium LP is the
		 * sole break authority, stands this wall (lambda* 111.5) and carries the panel the router
		 * could only strand, so production now agrees with the ruled STANDS. The old
		 * `DropsToday = 12` / `StrandsToday = 3` are deleted per FWallCase::DropsToday's own rule.
		 * The survivor region stays as a now-tautological identity pin.
		 */
		Add(10, TEXT("Opening at a free end, no abutment"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case10Cuts, {}, Case10Stands,
			TEXT("abutment against case 7 — NO LONGER SEPARATES; see the CASE 10 block"));

		/* C — spanning between supports. */

		/*
		 * STANDS — ruled down to LOCAL LOSS on 2026-08-08 then re-ruled back the same day; the second
		 * ruling is the keeper. The local-loss reading was BS 5977's 300-mm arching gate, which this
		 * fixture fails by its whole height; honest physics puts the 60 cm of bonded masonry over
		 * the span at span/depth 2.3, a deep beam at ~0.03-0.04 MPa, well under bond strength. Full
		 * derivation above Case11Cuts.
		 */
		Add(11, TEXT("Wall on two piers, six-brick clear span"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case11Cuts, {}, {},
			TEXT("pier width against case 12 — IN THE MARGIN, not the outcome"));

		/*
		 * STANDS — rewritten and re-ruled 2026-08-09. The old row cut ten cells out of twelve,
		 * varying span and pier at once; this is case 11's own span on a one-cell pier, and the
		 * arithmetic above Case12Cuts says the pier takes the thrust — ~400 N demand against a
		 * bonded restoring capacity four to eight times that. The rigid-body overturning reading
		 * that expected it to fail is rejected here for the third time (cases 14, 16 the others).
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
		 * STANDS, per the user's 2026-08-07 ruling and the arithmetic above. Drafted as a collapse
		 * taking the four projecting bricks; the bottom rung reads 0.195160875 of f_xk1 and nothing
		 * in the ladder is near 1.0. Full derivation above section D.
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
		 * STANDS, per the catalogue's 2026-08-07 revision and the arithmetic above. Drafted as a
		 * local loss taking the projecting header; its bed joint reads 0.058203838 of f_xk1, a sixth
		 * of what holds it. Full derivation above section E.
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
		 * STANDS — re-ruled 2026-08-12, row stays red in the inverted direction. Asked for COLLAPSE
		 * of everything over 135 cm of missing footing; the LP oracle stands it at lambda* = 12.38
		 * (lowest of the fifteen walls, still above 1), production drops 34 without breaking a
		 * joint — unroutability, not strength. Closest call of the three ruled that day; full
		 * derivation in the CASE 19 block above. No fall region; survivor region stays, since a
		 * STANDS verdict makes it a strictly true claim and the pins record the model's disagreement.
		 */
		/*
		 * GREEN since slice 3b/4 (2026-08-27), exactly as case 10: below the cap the equilibrium LP
		 * stands this wall (lambda* 48.0) and carries the half the router could only strand, so
		 * production now agrees with STANDS. Old `DropsToday = 34` / `StrandsToday = 6` deleted.
		 * Survivor region stays as an identity pin.
		 */
		Add(19, TEXT("Bottom course out under half the wall"),
			EVerdict::Stands, 10, StandardCells, Case19Cuts, {}, Case19Stands, nullptr);

		/*
		 * Re-ruled to STANDS at slice 4 (2026-08-27), the standing doubt settled by the LP. Through
		 * 2026-08-12 this was a LOCAL LOSS of two teeth that production over-answered by dropping
		 * nine; the equilibrium LP (sole break authority below the 200-block cap) stands the whole
		 * wall (lambda* 218.42) and holds the two teeth ~129x clear, so the true count is zero — the
		 * old two-tooth local loss was the per-joint heuristic's error. `DropsToday = 9` deleted.
		 *
		 * Why STANDS and not a row-21-style inverted red: the global feasibility LP and production
		 * agree here (unlike row 21, whose ruled COLLAPSE the LP contradicts), so there is no
		 * residual disagreement to pin red. A genuinely-local two-tooth mechanism, if one exists,
		 * needs per-region interrogation the global solve can't express (PROMOTION_DESIGN §3.5,
		 * §12 D7) — a later slice, not a reason to hold this row red. The teeth are named as the
		 * survivor region so their identity is pinned.
		 */
		Add(20, TEXT("Staircase void"), EVerdict::Stands,
			CoveredCourses, 14, Case20Cuts, {}, Case20Falls, nullptr);

		/* G — openings too big for what covers them. The set's only two falling verdicts. */

		/*
		 * COLLAPSE — user-directed 2026-08-12, built 2026-08-13, and since the 2026-08-14 mean
		 * re-anchor a deliberate red in the inverted direction, joining rows 10 and 19: the ruled
		 * verdict is Collapse and stands (hand deep-beam 1.2014 MPa is 1.71x the coded mean f_x1),
		 * but at mean strengths the model now STANDS the wall — pre-cascade worst measured
		 * 0.93542327561664174 at the flip (2026-08-14), not the predicted /7 = 0.549, because the
		 * worst joint's governing axis flipped (to squeezed-edge compression, unverified — see the
		 * pre-cascade block above), leaving the model 6.5% under the line. The cause is the
		 * composite-relief gap, not the data: production's tension sigma sits 3.1x below the hand
		 * sigma because composite vertical action re-sections the moment. Goes green when that gap
		 * closes (evolution step 4 or the composite/f_x2 rework), never by weakening this row.
		 *
		 * The pins hold the model's wrong answer at its measured size, per the rows-10/19
		 * convention: `DropsToday = 0`, `StrandsToday = 0`, and `Case21Stands` still names the
		 * jambs, so a regression fails loudly rather than hiding in an already-red row. The LP
		 * oracle's stand (17.24 at mean data, 5.511 at characteristic — a rigid-plastic scope limit)
		 * agrees with production and both disagree with this row's ruled Collapse, which is exactly
		 * what this inverted red records.
		 *
		 * `Isolates` says "not a pair", deliberately: against case 9 this row moves both cover
		 * (60 to 15 cm) and span (214.75 to 394.75, 1.84x), so it isn't a one-variable pair — it is
		 * a fixture where thin cover is the decisive term. Case 22 beside it is the genuine
		 * one-variable article (case 9's own cover, only the span moved).
		 */
		{
			FWallCase& Case = Add(21, TEXT("Eighteen-brick opening, two courses over"),
				EVerdict::Collapse,
				6, 22, Case21Cuts, Case21Falls, Case21Stands,
				TEXT("thin cover made decisive — NOT a one-variable pair against case 9: cover 60 cm ")
				TEXT("to 15 AND span 1.84x, both moved"));

			Case.DropsToday = 0;
			Case.StrandsToday = 0;
		}

		/*
		 * COLLAPSE — user-directed 2026-08-12, built 2026-08-13, survives the mean re-anchor as the
		 * catalogue's one green Collapse row. Case 9's own eight courses of cover over a span grown
		 * 3.62x, so the pair against case 9 varies span alone: 1.1644 MPa is 1.66x the coded mean
		 * f_x1.
		 *
		 * The margin is measured and pinned, and it is not what the fallout plan predicted: /7
		 * re-basing the characteristic-era pre-cascade worst (8.2241) predicted ~1.175, but the
		 * flip-measured (2026-08-14) reading is 2.9133264370386338, 2.5x the prediction — the worst
		 * joint's axis flipped (decomposed to Mohr-Coulomb shear: 0.35540386 MPa compression,
		 * 3.3985494 MPa in-plane, capacity 1.1665529 MPa — sliding under 35,706 N of arch thrust,
		 * flexural tension exactly zero, so /7 could never have re-based it). The wall still
		 * collapses in three passes shedding 316 pieces (two passes, 254, at characteristic data —
		 * the marginal cascade sheds more, not fewer). Pinned at `FWallCase::PreCascadeWorstToday`.
		 *
		 * The set's only outcome pair since the 2026-08-12 rulings retired the last of the old five.
		 * The oracle answered this fixture for the first time 2026-08-15 and stands it at
		 * lambda* = 8.4149459982219277 — a third LP-vs-catalogue disagreement of case 21's shape;
		 * the CASE 22 block above carries the measurement and what a sweep row would cost.
		 */
		Add(22, TEXT("Thirty-five-brick opening, eight courses over"), EVerdict::Collapse,
			CoveredCourses, 39, Case22Cuts, Case22Falls, Case22Stands,
			TEXT("span against case 9 — 7.77 m against 2.15, and it FALLS"))
			.PreCascadeWorstToday = 2.9133264370386338;

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

	/*
	 * The twenty-two as playable levels — what a scenario row for one of these cases must be.
	 *
	 * These configurations are the ones the user drew and reviewed, and until now the only thing
	 * that could lay them was the fixture bricklayer above — which lives in a test file, so no
	 * level could ever reach it. The three tests at the bottom of this file are the acceptance
	 * criteria for moving the geometry into production and leaving the verdicts here.
	 *
	 * The verdicts do not move: `EVerdict`, `MustFall`, `MustStand` and `Isolates` are claims about
	 * what the solver ought to conclude, an acceptance-test property. A level needs the geometry
	 * and the cuts and nothing else; moving an expected outcome into production would be putting an
	 * assertion there.
	 */

	/**
	 * What a case's level is called, and why it's the number rather than the title.
	 *
	 * A scenario name is typed on a URL and a map name becomes a filename on disk, so both want to
	 * be short, stable and free of punctuation; a case title is prose with commas and hyphens and
	 * is carried verbatim in the row's own `Title` instead. Two digits so `wall-2` cannot sort or
	 * read as `wall-20`.
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
	 * The one machine-checkable claim a level's caption makes.
	 *
	 * A caption is prose and stays prose — it's the only thing telling a player what they're
	 * looking at. What can't be prose is the verdict it claims, since a level captioned with an
	 * outcome the solver doesn't produce is a lie told to someone standing in front of the
	 * counter-example. So the caption carries exactly one token naming its verdict, spelled the way
	 * this file spells verdicts, and the rest is the writer's.
	 */
	FString VerdictClaimFor(EVerdict Verdict)
	{
		return FString::Printf(TEXT("Expected: %s"), VerdictName(Verdict));
	}

	/**
	 * The token a caption must carry when the model doesn't agree with its own claim.
	 *
	 * Three rows are red today. A caption naming only the expected verdict on one of those would be
	 * worse than silence, so the honest ones say both, and the ones the model agrees with must not
	 * say it, which stops the admission outliving the disagreement.
	 *
	 * Since 2026-08-12 the marker no longer implies "the model is too optimistic": cases 10 and 19
	 * are captioned `Expected: STANDS` and carry this marker, because the wall is ruled to stand
	 * and the model drops 12 and 34 bricks — telling a player the wall should be intact and isn't.
	 */
	const TCHAR* const ModelDisagreesMarker = TEXT("THE MODEL CURRENTLY DISAGREES");

	/**
	 * Whether the model produced the verdict this row claims.
	 *
	 * Deliberately the same three shapes `Acceptance.Wall.Catalogue` asserts one at a time, and a
	 * second reading of them rather than a shared one — the catalogue's assertions stay separate so
	 * each prints its own diagnosis. Held against the catalogue by the known-red tripwire in the
	 * caption test, which must name exactly the red rows, so a drifted predicate fails there rather
	 * than quietly captioning a level wrongly.
	 *
	 * The `Stands` arm is what decides cases 10 and 19 since their 2026-08-12 re-ruling: both rows
	 * are ruled STANDS, both drop pieces, so `Fallen.Num() == 0` is false and the predicate reports
	 * a disagreement — the first time this arm has ever returned false (every previous STANDS row
	 * was one the model produced).
	 *
	 * The `default` (Collapse) arm ran for the first time 2026-08-13 when cases 21 and 22 landed;
	 * between 2026-08-12 and then the catalogue held no `Collapse` row, so it was dead code. It now
	 * decides two rows in the agreeing direction (`MustFall` contained in the fallen set, `MustStand`
	 * disjoint from it). Its bite is proven by the survivor-widening mutation on case 21 (TRAPS.md
	 * registry) rather than by its presence.
	 *
	 * It also carries the pass-count claim on both falling shapes: a `Collapse` row whose pieces
	 * reached the ground in zero breaking passes is the unroutability of cases 10 and 19 wearing a
	 * collapse's outcome, so a caption calling that agreement would tell a player the wall failed on
	 * strength when no joint broke. `CutPasses` is structurally 0 on a row that cuts nothing, so
	 * this also refuses to agree with a `Collapse` row that cuts nothing — no such row exists.
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

			if (MustFall.Num() == 0 || Result.CutPasses == 0)
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
	 * Compare a laid layout against the fixture's wall, brick for brick and in handle order.
	 *
	 * Handle order is part of the claim and the half that looks fine when wrong: a builder that
	 * lays the same bricks in a different sequence renumbers every joint and break stamp while
	 * every geometric check still passes, and every reading in this file, `Core.Structure` and the
	 * design docs is a statement about a particular joint index.
	 *
	 * Only the first disagreement is reported: a wall is up to 375 pieces and 1,000 joints, and a
	 * producer that moved every brick would otherwise bury everything else in thousands of failures.
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
 * The acceptance set itself: every case in the catalogue, its own verdict, one row at a time.
 *
 * The assertion is on outcome and its shape follows the verdict — see the file header for why the
 * middle verdict exists and why displacement is never used. A failure names the case, the variable
 * its matched pair isolates, and both the expected and actual fallen set in (course, cell) terms.
 *
 * Most of this is expected to be red, and that is correct: these are the acceptance criteria for
 * the arching work in claude_plans/ARCHING_DESIGN.md, not a regression net over something
 * finished. The signature to look for is the one CURRENT_STATE.md predicts: every falling half of
 * a pair passing while every standing half fails — what a model that can only ever say "falls"
 * looks like.
 *
 * Several rows are green on arrival, each said to be so nobody mistakes them for work this suite
 * drove. 1 and 17 are intact walls, regression anchors. 18 is a stack-bond column that really
 * sits at a few percent — its verdict passes but the property that made the case interesting does
 * not, which is why that property has its own test below. And 8, 9, 12, 13, 14 and 16 all stand,
 * which is a correction rather than an achievement (14 and 16 revised 2026-08-07 on "a bonded
 * section resists what a rigid block could not"; 12 on 2026-08-09; 8 on 2026-08-11; 9 on
 * 2026-08-12 — each block in section B carries its own arithmetic, cost and doubts).
 *
 * Since 2026-08-12 two rows are red in a direction this test had never seen before: cases 10 and
 * 19 were re-ruled from COLLAPSE to STANDS the same day, and the model does not agree — it drops
 * 12 and 34 pieces in zero cascade passes, so what fails is the STANDS verdict's "nothing left the
 * structure" half. Every red row before that asked for more loss than the model produced; reading
 * one of these two as "wanted it to come down harder" is exactly backwards — the wall is ruled to
 * stand and the solver dropped a third of it.
 *
 * The pairs those corrections stopped separating on outcome have each moved onto what still
 * separates them, except 7 vs 8 and 7 vs 10, which are losses rather than relocations; the file
 * header lists all five pairs and the fate of each.
 *
 * Needs a ticking world: no. See the file header.
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
	 * The profile this file's arithmetic assumes, checked rather than imported: every expected
	 * value below is a consequence of these three figures, so a retune of any should turn this
	 * row red rather than silently move every case in the catalogue.
	 */
	TestEqual(TEXT("FIXTURE: general purpose mortar's cohesion is EN 1996-1-1 Table 3.4's f_vk0"),
		GeneralPurposeMortar.ShearCohesionMPa, MortarShearCohesionMPa);

	TestEqual(TEXT("FIXTURE: general purpose mortar's flexural bond is Table 3.2's f_xk1"),
		GeneralPurposeMortar.TensileStrengthMPa, MortarFlexuralBondMPa);

	TestEqual(TEXT("FIXTURE: the fixture's brick density is ClayBrick's published one"),
		ClayBrick.DensityGramsPerCubicCm, ClayBrickDensityGramsPerCubicCm);

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty-two cases"), Cases.Num(), 22);

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
			 * Both halves, neither decoration: "nothing fell" alone passes for a wall that severed
			 * every joint over the cut and stayed leaning together; "no joint gave" alone passes for
			 * a wall whose pieces were never in the load path to begin with.
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
			 * On a row that cuts nothing, the intact solve is the only solve there is. `CutPasses`
			 * above is structurally zero on the six no-cut rows (1, 13-17) since the whole block
			 * that sets it is inside `if (Case.Cuts.Num() > 0)`, so the "no joint over the cut gave"
			 * half of a STANDS verdict asserted nothing on exactly the rows whose as-built state is
			 * the case under test — a corbel that peeled every joint it stands on would have read as
			 * a clean pass. `CheckWallFixture` already makes this claim, harder, on the cutting rows
			 * (an intact wall that broke or dropped before the cut fails the precondition there);
			 * this is the same claim on the rows that precondition skips, written here because on a
			 * no-cut row it is a verdict, not a fixture check.
			 */
			/*
			 * Where a STANDS row names a survivor region, that region is asserted too — useful only
			 * while the row is red. A STANDS verdict already implies nothing comes down anywhere, so
			 * this is implied by the assertion above and can't fail on a passing row. What it buys is
			 * on cases 10 and 19: ruled to stand, dropping 12 and 34, the failure line above reads
			 * the same for any twelve or thirty-four — it holds the count, not the identity. A
			 * routing change shifting case 19's spreading front three cells right would keep every
			 * count-based assertion green. So it is asserted inverted, as a characterisation of where
			 * the wrongness is NOT: the named masonry must contain no fallen piece. Both blocks in
			 * section B record the drop sets the regions were checked against; these become
			 * tautologies (deletable with the pins) when the rows go green at evolution step 4.
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
			 * Identity, not a count: the named set is what a bounded loss means, so the same number
			 * of bricks falling from somewhere else is a completely different, wrong answer that a
			 * counting test would call a pass.
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
			 * Two-sided, because a wall down because everything is down says nothing about the term
			 * this row isolates. The named survivor region stops a model that always answers "falls"
			 * from passing every collapse row for free; the last row that couldn't name one — old
			 * case 12, whose picture had the entire wall coming down — was rewritten out 2026-08-09.
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

			/*
			 * And the collapse must have been a strength verdict — the claim both section-G blocks
			 * rest on, unasserted until 2026-08-13. Cases 10 and 19 are why: both drop a third of a
			 * wall in zero breaking passes with no joint near capacity — a router with nowhere to
			 * send load, not masonry that failed — which is why the user re-ruled them to STANDS.
			 * The two assertions above can't tell those apart ("every named brick came down and the
			 * jambs did not" is satisfied just as neatly by unroutability), so the pass count is
			 * asserted here rather than merely recited in the case blocks.
			 *
			 * One-sided on purpose: claims at least one joint gave, not how many passes it took (a
			 * property of the cascade's scheduling). Today's actual counts are three (case 21) and
			 * two (case 22), recorded in their blocks.
			 *
			 * Proven to bite by the TRAPS registry's `ReseatSpannedGroups` early-return mutation,
			 * which takes a routing mechanism away: both rows go stranded-heavy (case 21 to 31
			 * fallen / 16 stranded, case 22 to 236 / 33) and both fire this line at "0 breaking
			 * pass(es)" — every piece comes down because nothing would route it, exactly the reading
			 * this assertion refuses. Measured 2026-08-13: 50 log-error lines / 4 failing tests
			 * against a clean 8 / 2.
			 */
			TestTrue(
				*FString::Printf(
					TEXT("%s: COLLAPSE means the masonry GAVE — at least one joint must have broken ")
					TEXT("for the pieces to come down; the cascade ran %d breaking pass(es), so ")
					TEXT("either nothing broke and this row's collapse is unroutability like cases ")
					TEXT("10 and 19 rather than strength, or the cut alone took the ground from ")
					TEXT("under %d piece(s)"),
					*Where, Result.CutPasses, MustFall.Num()),
				Result.CutPasses > 0);

			break;
		}
		}

		/*
		 * The three known reds are pinned to the wrong answer they give today. This asserts nothing
		 * about physics — the row above has already failed; what this adds is that the failure has
		 * a fixed size, since a red row is otherwise a hole in the net (case 20 fails in identical
		 * words whether it drops nine bricks or ninety). Measured 2026-08-09, one number per red
		 * row, never guessed.
		 *
		 * The pin is what keeps the 2026-08-12 re-rulings honest: cases 10 and 19 were re-ruled with
		 * their pins rather than pinless, so their STANDS failure ("%d piece(s) left the structure")
		 * holds the failure at 12 and 34 exactly rather than drifting while red.
		 *
		 * A count is not a shape, though: a routing change that moved the drop set without resizing
		 * it passes this and every other count. Identity is held by the survivor region asserted in
		 * the STANDS arm above — complementary, not a duplicate, since the pin sees a failure grow
		 * or shrink and the region sees it move.
		 *
		 * A row that gets fixed fails here, and that is the point: the slice that fixes it deletes
		 * `DropsToday` in the same edit, along with the caption's disagreement marker and the
		 * known-red list entry. Case 9's pin went that way on 2026-08-12 — re-ruled into agreement
		 * rather than fixed, owing the same paperwork.
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

		/*
		 * A row may also pin the margin the cascade started from, which is not a count. Two
		 * assertions: the first says the wall really was past capacity when it went (a Collapse
		 * reached from under 1.0 would be pieces the router lost, the same trap `StrandsToday`
		 * exists for one layer up); the second holds the margin at its measured size, so a change
		 * that took case 22 from 1.17x capacity to 1.01x — still collapsing, still green, one
		 * retune from silently inverting the catalogue's only green Collapse row — fails loudly.
		 */
		if (Case.PreCascadeWorstToday > 0.0)
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s: the cascade must have started PAST capacity for a %s to be a strength ")
					TEXT("verdict — the worst joint read %.17g before any joint was allowed to give"),
					*Where, VerdictName(Case.Verdict), Result.PreCascadeWorst),
				Result.PreCascadeWorst > 1.0);

			TestTrue(
				*FString::Printf(
					TEXT("%s: CHARACTERISATION of the MARGIN — the worst joint read %.17g of capacity ")
					TEXT("before the cascade started, pinned at %.17g. A count cannot see this move: ")
					TEXT("the same bricks come down at 1.01x as at 8x, and this row is the one thing ")
					TEXT("that fails while a verdict is still standing but has stopped being safe"),
					*Where, Result.PreCascadeWorst, Case.PreCascadeWorstToday),
				FMath::IsNearlyEqual(
					Result.PreCascadeWorst, Case.PreCascadeWorstToday, 1.0e-9));
		}
	}

	return true;
}

/**
 * Cases 7 and 9, the part a verdict cannot say: widening an opening changes what its jamb is
 * doing, not merely how hard — and both walls stand anyway.
 *
 * Why this test exists. 7 vs 9 was an outcome pair — a four-cell opening stands, a ten-cell one
 * brings the wall down — and the last outcome pair the catalogue had. On 2026-08-12 the user
 * re-ruled case 9 to STANDS (CASE 9 block: 60 cm of bonded masonry spans 2.1 m as a deep beam,
 * LP oracle lambda* = 36.56, hand check 0.88x characteristic), and with both halves standing the
 * old "greater half loses strictly more" claim became unsatisfiable. `Acceptance.Wall.MatchedPairs`
 * retired in the same edit (case 10 went the same way the same day). This is where the span
 * discrimination went, and it's the stronger reading: an outcome pair could only say "these
 * differ"; this says by how much and in which direction.
 *
 * Why this is not the 7-vs-8 trap, the standard this relocation had to clear. The cover pair, 7 vs
 * 8, was deleted rather than relocated when case 8 was re-ruled 2026-08-11: production reads case
 * 7 at 0.269 (eight courses of cover) and case 8 at 0.219 (one), separating 1.23x in the WRONG
 * direction — a downward-routing solver reads cover as load, never arch capacity, so pinning that
 * would encode a defect as a discrimination. This row clears the same standard on three counts:
 *
 *   The direction is right. A longer span delivers a larger reaction into its abutment, and the
 *   jamb beside the wider opening does carry more — the normal force through the governing joint
 *   is 56,010.7 uu (68.5-91 cm clear) against 106,676.3 uu (203.5-226 cm), no strengths involved.
 *
 *   The joint is the same kind on both sides: both fixtures are the same deep beam (eight courses,
 *   60 cm, bonded cover over a toothed opening, same brick/mortar/thickness), both worst joints are
 *   bed joints in the right jamb at or just below the head — the reaction path. The pair varies
 *   span and cell count (12 against 14, keeping two cells of jamb either side) and nothing else.
 *   The failure mode at that joint is NOT the same on both sides since the 2026-08-14 re-anchor —
 *   the pair's finding, not a caveat (see the axis section below).
 *
 *   Neither reading is taken off a wall the router failed on: both cases drop, break and strand
 *   nothing. This is what disqualified the abutment pair, 7 vs 10, from the same relocation: case
 *   10 reads 0.300 against case 7's 0.269 (also the right direction) but gets there dropping twelve
 *   pieces (three stranded) in zero cascade passes — not a measurement of the abutment term. That
 *   discrimination went to the LP oracle instead (wall-07 296.22 against wall-10 35.82, 8.27x).
 *
 * What is asserted, and which part is derived. The magnitudes are a characterisation: unlike the
 * corbel/header reading tests, these two numbers are outputs of the composite-depth walk over a
 * whole wall and can't be re-derived without re-implementing the thing under test, so they're
 * pinned as measured — worth doing since today a 2x drift in either would move no verdict.
 *
 * The span-law arm is retired (measured 2026-08-14). This test used to assert `ReadingRatio >=
 * SpanRatio` (6.84 against 2.6928), on the reasoning that a simply-supported deep beam's reaction
 * grows at least linearly with span. That doesn't survive the axes parting: a utilisation ratio
 * taken on different axes is a ratio of two reactions times two unrelated strengths, and the
 * measured 3.66 -> 6.84 move at the mean re-anchor is exactly that (case 9's divisor moved from
 * flexural bond to Mohr-Coulomb sliding capacity). Worked three ways and the span law holds on
 * none: worst-axis stresses (0.0269441 MPa tension against 0.2570131 MPa shear, comparing two
 * different quantities), same axis both walls (each ratio has a zero in it — case 9's tension is
 * clamped to zero at the kern, case 7's jamb carries no horizontal force), and the honest reaction
 * (56,010.69 -> 106,676.31 uu, 1.9046x for 2.6928x of span — sublinear, w.L/2 not satisfied here).
 * So the arm is gone rather than restated (CURRENT_STATE). What replaced it needs no strength to
 * be meaningful: the wider opening puts 270,024 uu of horizontal thrust through its jamb where the
 * narrow one puts none — a span term the model demonstrably has.
 *
 * The governing axes, named and derived — the other job this test does. Each wall's worst joint is
 * asserted by (course, cell) before its magnitude is read, and the two are not the same joint:
 * case 7's is c2/8-c3/7.5 (one course below the head), case 9's is c3/11.5-c4/11 (at the head).
 * Both are in the right jamb within one course of the head. Since 2026-08-14 the axis is asserted
 * too, because prose got it wrong: DESIGN.md §6, CURRENT_STATE and this file all recorded case 9's
 * post-flip reading as squeezed-edge compression, but worked from the joint's own force and area
 * that axis reads 0.0203072, 13x below what production reports. The reading is Mohr-Coulomb shear:
 *
 *     case 7   |M_y| / (t.D^2/6) / f_x1, composite deep-beam section (D = 35.128 cm)
 *              = 0.038491547555641249 exactly — tension
 *     case 9   |F_xy| / A / (f_v0 + mu.|F_z|/A), bare Mohr-Coulomb on the joint's own patch
 *              = 0.26329211195559277 exactly — shear, the jamb sliding under arch thrust
 *
 * Both asserted to 1e-9 below, each one formula over quantities production publishes plus the
 * published strengths. Getting the axis wrong is what made the retired span-law arm look like a
 * span term, and what made the old flap watch vacuous.
 *
 * The old flap watch is kept and said to be vacuous: production once read case 9's jamb at
 * 0.98502040901419818, one retune from 1.0; at mean strengths the governing axis moved and the
 * reading sits ~3.8x clear. It stays because the day it crosses, case 9's STANDS verdict becomes a
 * catalogue red and the oracle sweep's `AgreeStands` relation flips — but it now watches a shear
 * reading, not the flexural one the knife edge was about.
 *
 * Green on arrival, said plainly: this pins behaviour the model already produces and drove
 * nothing. It exists because the case 9 ruling deleted an assertion (and, via MatchedPairs'
 * retirement, a whole test), and a deleted assertion without a replacement is a hole in the net.
 *
 * Needs a ticking world: no. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceSpanTest,
	"DestructionGame.Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceSpanTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/*
	 * Slack against a re-association of a handful of doubles and nothing else, as the corbel and
	 * header reading tests use it. The smallest difference either row should see is in the second
	 * decimal place.
	 */
	constexpr double UtilisationTolerance = 1.0e-9;

	/*
	 * The whole Mohr-Coulomb triple, checked rather than imported: since 2026-08-14 this test
	 * names each reading's axis, and the naming is only as good as the strengths it divides by.
	 * `Acceptance.Wall.Catalogue` pins the two this file already used; the other three are pinned
	 * here, the only place that reads them.
	 */
	TestEqual(TEXT("FIXTURE: general purpose mortar's friction coefficient is the profile's mu"),
		DestructionProfiles::GeneralPurposeMortar.FrictionCoefficient, MortarFrictionCoefficient);

	TestEqual(TEXT("FIXTURE: general purpose mortar's shear truncation is the profile's"),
		DestructionProfiles::GeneralPurposeMortar.MaxShearStrengthMPa, MortarMaxShearStrengthMPa);

	TestEqual(TEXT("FIXTURE: general purpose mortar's compressive strength is the profile's"),
		DestructionProfiles::GeneralPurposeMortar.CompressiveStrengthMPa,
		MortarCompressiveStrengthMPa);

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
	 * Neither wall breaks a joint either, which the header claims and had not asserted. "Both cases
	 * drop, break and strand nothing" is the argument that these readings are honest readings of a
	 * fully routed structure — the whole reason this pair was allowed to relocate onto readings
	 * where 7-vs-10 was not. Dropped and stranded are covered above; the broken half was prose. A
	 * wall that severed joints over its opening and stayed leaning together would still read a
	 * worst utilisation, of a different structure.
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

	/*
	 * --- the magnitudes, pinned as measured ---------------------------------------------------
	 *
	 * Mean re-anchor (2026-08-13), measured at the flip (2026-08-14) — the pair's two readings
	 * parted axes, exactly what TRAPS' strength-basis entry warns about. Case 7's jamb stayed
	 * tension-governed and reads the old pin / 7 to the bit. Case 9's did not: its tension fell to
	 * 0.98502040901419818 / 7 and then to exactly zero (production clamps that joint's moment at
	 * the kern), while the shear axis, 5% behind tension at the characteristic data, came forward
	 * to read the 0.26329211195559277 pinned below.
	 *
	 * The axis is derived and asserted below rather than named in prose, because prose got it
	 * wrong: this comment used to call it the squeezed edge's compression against the unmoved
	 * 10 MPa, which reads 0.0203072 here, 13x under what production reports. The governing joint
	 * did not move; the governing axis did, twice.
	 */
	constexpr double NarrowWorst = 0.26944083288948872 / 7.0;
	constexpr double WideWorst = 0.26329211195559277;

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
	 * The crossing is watched separately from the value: the pin above already fails on any drift,
	 * but this one fails in the words of the event that matters, so the log says what it means
	 * rather than only that a number changed.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("THE FLAP WATCH: case 9's jamb reads %.17g and must stay UNDER 1.0. VACUOUS at the ")
			TEXT("mean basis (re-anchor 2026-08-13) — the reading sits ~3.8x clear and is the jamb ")
			TEXT("SLIDING under the arch thrust, not the flexural tension the characteristic-era ")
			TEXT("0.985 knife-edge was about — and kept because the day it crosses, this wall drops ")
			TEXT("its head, case 9's 2026-08-12 STANDS ruling becomes a catalogue red and the oracle ")
			TEXT("sweep's pinned AgreeStands relation flips. That is a re-derivation, not a retune"),
			WideResult.Worst),
		WideResult.Worst < 1.0);

	/* --- which AXIS each reading is, derived from the joint's own force and moment ------------- */

	/**
	 * The two joints' loads, straight off production's published accessors.
	 *
	 * Nothing here re-implements the solver. `GetConnectionForce`, `GetConnectionMoment` and
	 * `GetConnectionCompositeDepthCm` report what the routing layer decided the joint carries;
	 * this file applies one published formula per axis against the published strengths at the
	 * top of this namespace — enough to say which of the three axes `ComputeUtilisation` returned,
	 * which a bare utilisation cannot.
	 */
	auto JointLoad = [](const FWall& Wall, const FWallResult& Result, int32& OutJoint)
	{
		OutJoint = INDEX_NONE;

		for (int32 Joint = 0; Joint < Wall.Structure.NumConnections(); ++Joint)
		{
			const FConnection& Connection = Wall.Structure.GetConnection(Joint);

			if (Connection.PieceA == Result.WorstPieceA && Connection.PieceB == Result.WorstPieceB)
			{
				OutJoint = Joint;
				break;
			}
		}
	};

	int32 NarrowJoint = INDEX_NONE;
	int32 WideJoint = INDEX_NONE;

	JointLoad(NarrowWall, NarrowResult, NarrowJoint);
	JointLoad(WideWall, WideResult, WideJoint);

	if (NarrowJoint == INDEX_NONE || WideJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: could not find the connection carrying a wall's worst reading"));

		return true;
	}

	const FVector NarrowForceUu = NarrowWall.Structure.GetConnectionForce(NarrowJoint);
	const FVector NarrowMomentUuCm = NarrowWall.Structure.GetConnectionMoment(NarrowJoint);
	const double NarrowDepthCm = NarrowWall.Structure.GetConnectionCompositeDepthCm(NarrowJoint);

	const FVector WideForceUu = WideWall.Structure.GetConnectionForce(WideJoint);
	const double WideAreaSqCm = WideWall.Structure.GetConnection(WideJoint).InterfaceAreaSqCm;

	/*
	 * Case 7 is flexural tension, on the composite section: a bed joint bends about Y here, so the
	 * deep beam over it has section t.D^2/6 (t the wall's 10.25 cm thickness, D the depth
	 * production reports for that joint), stress is |M_y| over it, capacity is the published mean
	 * flexural bond.
	 */
	const double NarrowCompositeModulusCm3 =
		BrickDepthCm * NarrowDepthCm * NarrowDepthCm / 6.0;

	const double NarrowTensionUtilisation =
		FMath::Abs(NarrowMomentUuCm.Y)
		/ (NarrowCompositeModulusCm3 * ForceUnitsPerMPaSqCmHere) / MortarFlexuralBondMPa;

	TestTrue(
		*FString::Printf(
			TEXT("THE AXIS: case 7's jamb must be governed by FLEXURAL TENSION on the composite ")
			TEXT("section — |M_y| %.8g uu.cm over t.D^2/6 = %.8g cm3 (D = %.8g cm) against f_x1 ")
			TEXT("%.8g MPa is %.17g, and production reads %.17g. If these part company the reading ")
			TEXT("has changed axis and every sentence about this pair has to be re-derived"),
			FMath::Abs(NarrowMomentUuCm.Y), NarrowCompositeModulusCm3, NarrowDepthCm,
			MortarFlexuralBondMPa, NarrowTensionUtilisation, NarrowResult.Worst),
		FMath::IsNearlyEqual(NarrowResult.Worst, NarrowTensionUtilisation, UtilisationTolerance));

	/*
	 * Case 9 is Mohr-Coulomb shear, on the joint's own patch and nothing else: the jamb is pushed
	 * sideways by the arch thrust, shear stress is the in-plane force over the area, capacity is
	 * cohesion plus friction times the compression across the joint. No bending term or composite
	 * section — why calling this a bending or compression reading was wrong.
	 */
	const double WideNormalStressMPa =
		FMath::Abs(WideForceUu.Z) / (WideAreaSqCm * ForceUnitsPerMPaSqCmHere);

	const double WideShearStressMPa =
		FVector(WideForceUu.X, WideForceUu.Y, 0.0).Size()
		/ (WideAreaSqCm * ForceUnitsPerMPaSqCmHere);

	const double WideShearCapacityMPa = FMath::Min(
		MortarShearCohesionMPa + MortarFrictionCoefficient * WideNormalStressMPa,
		MortarMaxShearStrengthMPa);

	const double WideShearUtilisation = WideShearStressMPa / WideShearCapacityMPa;

	TestTrue(
		*FString::Printf(
			TEXT("THE AXIS: case 9's jamb must be governed by MOHR-COULOMB SHEAR — %.8g MPa of ")
			TEXT("in-plane stress against a capacity of f_v0 %.8g + mu %.8g x %.8g MPa of ")
			TEXT("compression = %.8g MPa is %.17g, and production reads %.17g. The squeezed edge's ")
			TEXT("COMPRESSION, which DESIGN §6 and CURRENT_STATE both named for this reading before ")
			TEXT("2026-08-14, is %.8g — thirteen times under it"),
			WideShearStressMPa, MortarShearCohesionMPa, MortarFrictionCoefficient,
			WideNormalStressMPa, WideShearCapacityMPa, WideShearUtilisation, WideResult.Worst,
			2.0 * WideNormalStressMPa / MortarCompressiveStrengthMPa),
		FMath::IsNearlyEqual(WideResult.Worst, WideShearUtilisation, UtilisationTolerance));

	/* --- the pair's own claim: the span is read in the joint ---------------------------------- */

	/*
	 * The clear spans, computed from the cell grid rather than quoted: a gap of n cell pitches
	 * between the centres of the surviving bricks either side of an opening leaves
	 * n * CellPitch - BrickLength of open air. Each toothed reveal gives two values (even and odd
	 * courses stop at different cells); the mean is what the beam spans on average.
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

	/*
	 * The span term, on a quantity with no strength in it. The narrow opening's jamb carries a
	 * purely vertical force; the wide one carries ~2,700 N of horizontal thrust too — a different
	 * mechanism arriving, not a bigger number. This is what the retired `ReadingRatio >= SpanRatio`
	 * row was reaching for and couldn't express: a solver with no span term delivers no thrust into
	 * either jamb and fails here, and no choice of strengths can satisfy it since none appears.
	 */
	const double NarrowThrustUu = FVector(NarrowForceUu.X, NarrowForceUu.Y, 0.0).Size();
	const double WideThrustUu = FVector(WideForceUu.X, WideForceUu.Y, 0.0).Size();

	TestTrue(
		*FString::Printf(
			TEXT("SPAN: the wider opening must push its jamb SIDEWAYS where the narrow one does not ")
			TEXT("— %.8g cm of clear opening delivers %.8g uu of in-plane thrust into its jamb and ")
			TEXT("%.8g cm delivers %.8g uu. This is the pair's span discrimination since the ")
			TEXT("2026-08-14 re-anchor retired the reading-ratio floor, and it carries no strength ")
			TEXT("at all"),
			NarrowSpanCm, NarrowThrustUu, WideSpanCm, WideThrustUu),
		WideThrustUu > 0.0 && NarrowThrustUu <= 0.0);

	/*
	 * The reaction grows too, the other half and also strength-free: the normal force through the
	 * joint is the reaction the cover delivers into that jamb, the closest like-for-like the two
	 * walls have.
	 */
	const double NarrowReactionUu = FMath::Abs(NarrowForceUu.Z);
	const double WideReactionUu = FMath::Abs(WideForceUu.Z);

	const double ReactionRatio =
		NarrowReactionUu > 0.0 ? WideReactionUu / NarrowReactionUu : 0.0;

	TestTrue(
		*FString::Printf(
			TEXT("SPAN: and a wider opening must deliver a BIGGER reaction into its jamb — %.8g uu ")
			TEXT("at %.8g cm of clear opening against %.8g uu at %.8g cm. A model with no span term ")
			TEXT("reads them the same"),
			NarrowReactionUu, NarrowSpanCm, WideReactionUu, WideSpanCm),
		NarrowReactionUu > 0.0 && WideReactionUu > NarrowReactionUu);

	/*
	 * The sublinearity is pinned as a finding, not asserted as physics: w.L/2 says the reaction
	 * grows at least linearly with span; the model grows it 1.9046x for 2.6928x of span, so the
	 * published relation doesn't hold here and demanding it would be a deliberate red for a defect
	 * nothing else is waiting on. Pinned instead, so the day equilibrium promotion (DESIGN §7 step
	 * 4) gives the reaction an honest span term, this fires and someone re-derives rather than
	 * re-tunes.
	 */
	constexpr double PinnedReactionRatio = 1.9045707413720012;

	TestTrue(
		*FString::Printf(
			TEXT("CHARACTERISATION: the reaction ratio is %.17g against a span ratio of %.17g — ")
			TEXT("SUBLINEAR, where a deep beam's w.L/2 requires at least linear. The pin is %.17g. ")
			TEXT("This row PASSES and records a known-weak span term; it is not an endorsement"),
			ReactionRatio, SpanRatio, PinnedReactionRatio),
		FMath::IsNearlyEqual(ReactionRatio, PinnedReactionRatio, UtilisationTolerance)
			&& ReactionRatio < SpanRatio);

	return true;
}

/**
 * Cases 13 and 14, the part a verdict cannot say: doubling the corbel step doubles the joint
 * reading, and both corbels stand anyway.
 *
 * Why this test exists. 13 and 14 were an outcome pair — one stands, one collapses — until the
 * user ruled 2026-08-07 that a bonded corbel resists with its full depth and case 14's collapse
 * verdict was wrong. Both halves now stand, discriminating nothing, so this moves the pair onto a
 * quantity that still separates them — stronger than an outcome pair, since it says by how much
 * and why rather than just "these differ".
 *
 * The expected value is derived from the fixture, not read back off the solver: case 14's four
 * corbelled courses each keep one 10.25 x 10.25 bed patch with mass 5.625 cm outboard of it, so
 * the load ladder, moment ladder and deep-beam section are all written above from the grid, the
 * published density and f_xk1 — none imported from production. The point is that a wrong constant
 * makes this disagree.
 *
 * Cross-checked against two figures this file did not produce, so it can't agree with itself: the
 * same four lines give 0.21858 at five steps (ARCHING_DESIGN's published 0.219) and
 * 0.36903147272727271 at eleven (the staircase anchor
 * `Core.Structure.AStaircaseVoidCondemnsTheCorbel`'s pin to seventeen digits), held at 2% and
 * 1e-9 respectively in the test.
 *
 * The thing being measured must be the thing governing, asserted separately: `Worst` is the worst
 * joint anywhere in the wall, so the worst joint is asserted to be the bed joint under the lowest
 * corbelled course (course 5's half bat at cell 7.25 carrying course 6's projecting brick) before
 * its magnitude is read.
 *
 * Green on arrival, said plainly: this pins behaviour arching slice 5 already produces and drove
 * nothing. It exists because the ruling above deleted an assertion, and a deleted assertion
 * without a replacement is a hole in the net.
 *
 * Needs a ticking world: no. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceCorbelProjectionTest,
	"DestructionGame.Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceCorbelProjectionTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/*
	 * Slack against a re-association of a handful of doubles and nothing else. One course of depth
	 * either way moves the four-step reading from 0.1735 to 0.2186 — seven orders of magnitude
	 * above this tolerance.
	 */
	constexpr double UtilisationTolerance = 1.0e-9;

	/* --- the derivation, cross-checked against two figures produced elsewhere ---------------- */

	/*
	 * Both cross-check figures are the characteristic-basis publications divided by 7 — the
	 * 2026-08-14 mean re-anchor flip moved f_x1 0.10 -> 0.70 and these ladders are pure tension.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: five steps should reproduce ARCHING_DESIGN's published 0.219 / 7, the ")
			TEXT("closed form gives %.8g"),
			CorbelBottomRungUtilisation(5)),
		FMath::IsNearlyEqual(CorbelBottomRungUtilisation(5), 0.219 / 7.0, 0.02 * (0.219 / 7.0)));

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: eleven steps should reproduce the staircase anchor 0.0527187818..., ")
			TEXT("the closed form gives %.17g"),
			CorbelBottomRungUtilisation(11)),
		FMath::IsNearlyEqual(
			CorbelBottomRungUtilisation(11), 0.36903147272727271 / 7.0, UtilisationTolerance));

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
	 * The outcome follows from the number rather than the other way round: 0.195 is a fifth of
	 * capacity, and the catalogue's "stands" verdict is because of this, not beside it.
	 *
	 * The `Worst < 1.0` row that used to say so was deleted 2026-08-09 as a tautology: `Worst`
	 * skips every joint that has given, and `SolveAndBreak`'s last pass by definition broke
	 * nothing, so every joint the maximum can be taken over is already at most 1.0 — the assertion
	 * could only fail on a joint at exactly 1.0, which the exact-value assertion above (pinning
	 * 0.195160875 to 1e-9) would already have caught, louder. The verdict follows from that
	 * assertion, not a bound the cascade already guarantees.
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
	 * Twice, not merely more: a strict inequality alone is satisfiable by a last-bit difference,
	 * which isn't a projection term working. The step doubles, the arm doubles with it, and the
	 * measured separation is 2.8x — two is the floor, well under that.
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
 * Cases 15 and 16, the part a verdict cannot say: putting six courses on a header's tail divides
 * what its own bed joint reads by thirty, and both headers stand anyway.
 *
 * Why this test exists. 15 and 16 were an outcome pair — one stands, one drops its header — until
 * the catalogue was revised 2026-08-07 and case 16 became "stands" too: an uncracked bonded bed
 * joint carries a half-cell projection at 0.0582 of f_xk1, and the local-loss verdict it replaced
 * was a rigid-body overturning reading that ignored the bond (the same correction case 14 got the
 * same day). Both halves now discriminate nothing, so this moves the pair onto a quantity that
 * still separates them — stronger than an outcome pair, since it says by how much and why.
 *
 * The two walls are the same wall with the header at a different height — ten courses, twelve
 * cells, one course pushed half a cell out and closed with a full brick. In case 15 that's course
 * 3 (six courses stand on the header's tail); in case 16 it's course 9, the top (nothing does).
 * The header's own bed joint geometry is identical in both, making this a one-variable comparison.
 *
 * The expected value is derived from the fixture, not read back off the solver: seat length is
 * what's left of a 21.5 cm brick when half a 22.5 cm cell hangs over air, the arm is half of what
 * hangs over, the section is that patch's t.L^2/6, the weight is published density x dimensions x
 * gravity, and capacity is EN 1996-1-1's f_xk1 — none imported from production.
 *
 * Cross-checked against two figures this file did not produce: WALL_CASES.html's quoted 0.058204
 * for this case (held at 1e-6) and the waist anchor 0.058203838191552663 that
 * `Core.Structure.AdoptedWallLoadsItsWaistEccentrically` pins to seventeen digits on a completely
 * different fixture (one brick, half seated, by cut rather than laying).
 *
 * The thing being measured must be the thing governing, asserted separately: `Worst` is the worst
 * joint anywhere in the wall, and a ten-course wall's base compression could in principle carry it
 * (case 17's intact stack-bond wall of the same height reads 0.00109 at its foot), so case 16's
 * worst joint is asserted to be the header's own bed joint by (course, cell) before its magnitude
 * is read.
 *
 * Case 15's governing joint is deliberately not pinned — not laziness. What's claimed is an upper
 * bound (the worst joint anywhere in that wall is at most a tenth of case 16's), which says
 * something stronger than pinning identity and stays true if a later slice migrates the maximum
 * elsewhere. (It reads 0.00184 at c2/11-c3/11.5 today, the header's bed joint, and the failure
 * message prints where it actually is.)
 *
 * Green on arrival, said plainly: this pins behaviour the model already produces and drove
 * nothing. It exists because the case 16 revision deleted an assertion, and a deleted assertion
 * without a replacement is a hole in the net.
 *
 * Needs a ticking world: no. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceSuperimposedLoadTest,
	"DestructionGame.Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceSuperimposedLoadTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/*
	 * Slack against a re-association of a handful of doubles and nothing else. The quantity is
	 * 0.058, seven orders of magnitude above this tolerance.
	 */
	constexpr double UtilisationTolerance = 1.0e-9;

	/* --- the derivation, worked forward from the grid ---------------------------------------- */

	/*
	 * What the header keeps and what hangs over. CourseGeometry pushes the projecting course's
	 * right face out by half a cell and closes it with a full brick rather than the half bat a
	 * flush odd course closes with, so of the brick's 21.5 cm exactly 11.25 cm is over air and
	 * 10.25 cm still bears on the course below. (Landing on the same 10.25 cm as the wall's depth
	 * is a coincidence of this brick's proportions, not a relation.)
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

	/*
	 * Both cross-checks are the characteristic-basis figures divided by 7 (mean re-anchor
	 * 2026-08-13; the html's 0.058204 is the characteristic-era quote and its documentation
	 * sweep is part of the green phase).
	 */
	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: WALL_CASES.html's characteristic-era 0.058204 / 7 for case 16; the ")
			TEXT("derivation gives %.8g"),
			ExpectedUnloaded),
		FMath::IsNearlyEqual(ExpectedUnloaded, 0.058204 / 7.0, 1.0e-6));

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: the same arithmetic on a CUT half-seated brick is the waist anchor ")
			TEXT("0.0083148340..., the derivation gives %.17g"),
			ExpectedUnloaded),
		FMath::IsNearlyEqual(ExpectedUnloaded, 0.058203838191552663 / 7.0, UtilisationTolerance));

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
	 * Where the header and its seat are, in cells, walked off the bricklayer rather than counted
	 * off the log. The projecting course's rightmost brick is a full brick whose right face is half
	 * a cell past the flush face; the course below is flush and closes with a full brick too, since
	 * it's even. Both centres are half a brick in from their own right faces.
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
	 * The outcome follows from the number rather than the other way round: 0.058 is a sixth of
	 * capacity, and the catalogue's "stands" verdict is because of this. The `Worst < 1.0` row that
	 * used to say so was deleted 2026-08-09 as a tautology, for the reason written out beside case
	 * 14's copy in Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome. The verdict
	 * follows from the exact-value assertion above, which pins 0.058203838191552663 to 1e-9.
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
	 * Three times, not merely more: a strict inequality alone is satisfiable by a last-bit
	 * difference. The measured separation is 4.51x, with the loaded joint's tension driven to
	 * exactly zero and compression left governing. The floor was ten against a measured 32 in the
	 * characteristic era; the 2026-08-13 mean re-anchor shrank it by exactly the /7 the bare
	 * reading moved, since the loaded side is compression-governed and didn't move (the ratio
	 * isn't axis-invariant — TRAPS, strength basis). Three is the re-derived floor with 1.5x
	 * headroom below the measurement, capped above by the ten-course wall's own base compression
	 * (0.00109), which caps the achievable factor at about 7.6.
	 *
	 * Note what this bounds: `Worst` for case 15 is the worst joint anywhere in that wall, so
	 * bounding it above bounds the header's own joint by the same number regardless of which joint
	 * carries the maximum — safe against a later slice relieving the header further.
	 */
	constexpr double MinimumSeparation = 3.0;

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
 * its head joints' shear capacity at any height.
 *
 * Why this is its own test. Case 18's verdict is "stands", and stands is easy — the column sits at
 * a few percent of capacity, so a wall that got the load path completely wrong would still pass
 * the catalogue row. The finding that overturned this case's drafted verdict was arithmetic, and
 * the property that makes it trustworthy is that the ratio doesn't move with height: every course
 * added brings one brick of weight and two more head joints, which cancel exactly. Two heights
 * saying the same number is a far stronger claim than one, since a model that simply piles the
 * whole column onto the joints at its foot also reads a plausible few percent — and grows linearly
 * with height, which one height can't see.
 *
 * The arithmetic, worked independently. A full brick weighs 2667.198625 Unreal force units. In
 * stack bond a brick whose bed joint has been cut away is bonded to a neighbour on each side over
 * a head joint of 10.25 x 6.5 = 66.625 cm2, load parallel to both, so it is shear, split by area
 * between two equal joints:
 *
 *     2667.198625 / (2 x 66.625) / 10000 uu per MPa.cm2  =  0.00200165 MPa
 *     0.00200165 / 0.9 MPa (mean f_v0)                   =  0.0022240556
 *
 * (0.01000825 on the retired characteristic f_vk0 = 0.2 — the mean re-anchor is a clean x2/9 here
 * since the capacity is bare cohesion and nothing else moved.) No friction to add: a vertical load
 * on a vertical joint puts no compression across it.
 *
 * This disagrees with the number in the brief: WALL_CASES and CURRENT_STATE both quoted 0.0200,
 * reached by dividing the same load by the flexural bond — but the joint here is in shear against
 * the shear bond. The value asserted here is what follows from the strengths this project ships.
 *
 * The heights are 12 and 20 because the head joint has to be the worst joint at both, and at the
 * old 10 and 16 it was not (measured 2026-08-14). `FWallResult::Worst` is the worst joint anywhere
 * in the wall, so this test is only about the hanging column while the hanging column reads worst.
 * It stopped being that at the mean re-anchor: the head-joint reading (bare cohesion) moved x2/9
 * with f_v0 while the bed joint beside the hole (c5/6-c6/6) didn't, so the two crossed over and the
 * old pair's "factor of 2.465" compared two unrelated numbers on different axes.
 *
 * The ladder, measured rather than argued (worst head against worst bed, one wall per rung):
 *
 *      courses   head joint    bed joint c5/6-c6/6   governs
 *          7     0.00222406    0.00906354            BED
 *          8     0.00444811    0.00904987            BED
 *          9     0.00667217    0.00903624            BED
 *         10     0.00889622    0.00902265            BED    <- the old lower rung, vacuous
 *         11     0.01112028    0.00900910            head, by 1.23x
 *         12     0.01334433    0.01038752            head, by 1.28x
 *         16     0.02224056    0.01720933            head, by 1.29x
 *         20     0.03113678    0.02395029            head, by 1.30x
 *         30     0.05337733    0.04045825            head, by 1.32x
 *
 * The head reading is (courses - 6) x the per-pair figure to the last bit at every rung — the cut
 * is in course 5, so a wall of n courses hangs n - 6 bricks over the hole and the model puts every
 * one of them on the pair of head joints at the column's foot. The bed competitor bottoms out
 * around 0.0090 and grows sublinearly, so the head joint takes over at eleven courses. 12 and 20
 * are the pair: both clear of the crossing, a 1.667x height ratio, margin asserted below so this
 * can't go quietly vacuous again.
 *
 * What is red and what is not. The absolute rows and the height-independence row are the red — the
 * model reads 6x and 14x the correct per-pair figure. The row that passes is the characterisation
 * beside them, pinning the wrong answer at exactly (courses - 6) x that figure: the defect is the
 * whole hanging column's weight arriving at its foot, not "some larger number".
 *
 * Needs a ticking world: no.
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

	/**
	 * The course the cut is in, read off Case18Cuts rather than written twice. A wall of n courses
	 * hangs n - (CutCourse + 1) bricks over the hole, which is what the model multiplies the
	 * per-pair figure by — deriving it from the cut stops the characterisation below silently
	 * meaning something else if the cut moves.
	 */
	const int32 CutCourse = Case18Cuts[0].CourseLo;

	/*
	 * Two heights, the same cut. Twelve courses leaves six bricks hanging above the hole and
	 * twenty leaves fourteen, so a model that concentrates the column on the joints at its foot
	 * reads 2.33x as much at the taller wall while the correct answer doesn't move. Both are past
	 * the eleven-course crossing where the head joint overtakes the bed joint beside the hole (the
	 * header's measured ladder) — do not lower either rung without re-measuring it.
	 */
	const int32 Heights[] = { 12, 20 };

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
		 * The column must actually be hanging, or the number below measures an ordinary bed joint
		 * and the test is vacuous. Two assertions rather than one, since the first was true at the
		 * old heights and still let the test go vacuous: a head joint says the column governs, and
		 * knowing by how much is what fails while the crossing is still one retune away.
		 */
		double WorstHead = 0.0;
		double WorstBed = 0.0;
		int32 WorstBedA = INDEX_NONE;
		int32 WorstBedB = INDEX_NONE;

		for (int32 Joint = 0; Joint < Wall.Structure.NumConnections(); ++Joint)
		{
			const FConnection& Connection = Wall.Structure.GetConnection(Joint);

			if (Connection.HasGiven())
			{
				continue;
			}

			const double Utilisation = Wall.Structure.GetConnectionUtilisation(Joint);

			/* One course index for both ends is a head joint; two is a bed joint. */
			if (Wall.CourseOf[Connection.PieceA] == Wall.CourseOf[Connection.PieceB])
			{
				WorstHead = FMath::Max(WorstHead, Utilisation);
			}
			else if (Utilisation > WorstBed)
			{
				WorstBed = Utilisation;
				WorstBedA = Connection.PieceA;
				WorstBedB = Connection.PieceB;
			}
		}

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

		/*
		 * And it must govern with room to spare. 1.15 is a tripwire, not a physical claim: the
		 * measured margins are 1.285 at twelve courses and 1.300 at twenty, so this fires before
		 * the bed joint quietly takes over. At the old ten-course rung this ratio was 0.986 and the
		 * test was measuring the bed joint.
		 */
		constexpr double MinimumHeadOverBed = 1.15;

		TestTrue(
			*FString::Printf(
				TEXT("%d courses: the hanging column's HEAD joint must govern with margin — it ")
				TEXT("reads %.8g against the worst BED joint's %.8g at c%d/%g-c%d/%g, a ratio of ")
				TEXT("%.4g, and below %.2f this test is measuring the wrong joint"),
				Heights[Index], WorstHead, WorstBed,
				WorstBedA == INDEX_NONE ? -1 : Wall.CourseOf[WorstBedA],
				WorstBedA == INDEX_NONE ? -1.0 : Wall.CellOf[WorstBedA],
				WorstBedB == INDEX_NONE ? -1 : Wall.CourseOf[WorstBedB],
				WorstBedB == INDEX_NONE ? -1.0 : Wall.CellOf[WorstBedB],
				WorstBed > 0.0 ? WorstHead / WorstBed : 0.0, MinimumHeadOverBed),
			WorstBed > 0.0 && WorstHead >= MinimumHeadOverBed * WorstBed);

		/*
		 * The red: every course the column gains sheds its own weight into its own pair of head
		 * joints, so the joints at the foot never see more than one brick — the same number at any
		 * height.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%d courses: the hanging column should read %.8g of head-joint shear ")
				TEXT("capacity, it reads %.8g"),
				Heights[Index], HangingColumnShearUtilisation, Result.Worst),
			FMath::IsNearlyEqual(Result.Worst, HangingColumnShearUtilisation, UtilisationTolerance));

		/*
		 * And the wrong answer is pinned at its measured size, exactly as the catalogue's
		 * `DropsToday` pins a known-red row's drop count. This passes, turning "the model reads
		 * some larger number" into a named defect: the reading is the per-pair figure times the
		 * whole hanging column's brick count. A change that made the model differently wrong fails
		 * here rather than hiding inside the already-red rows above.
		 */
		const int32 HangingBricks = Heights[Index] - (CutCourse + 1);

		TestTrue(
			*FString::Printf(
				TEXT("%d courses: CHARACTERISATION of the known red — the model should read the ")
				TEXT("per-pair figure %.8g times the %d brick(s) hanging over the hole, %.17g, and ")
				TEXT("it reads %.17g. This row PASSES: it holds the defect at its measured size"),
				Heights[Index], HangingColumnShearUtilisation, HangingBricks,
				HangingBricks * HangingColumnShearUtilisation, Result.Worst),
			FMath::IsNearlyEqual(
				Result.Worst, HangingBricks * HangingColumnShearUtilisation, UtilisationTolerance));
	}

	if (bRan[0] && bRan[1])
	{
		/*
		 * The property, asserted without reference to either value. This holds even if the absolute
		 * figure above is wrong, and is the half a model piling the column onto its foot cannot
		 * satisfy however it's tuned. Both rungs are past the eleven-course crossing asserted above,
		 * so this compares the same joint of the same column on the same axis at two heights — what
		 * the old 10-and-16 pair had stopped being.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("HEIGHT-INDEPENDENT: %d courses reads %.8g and %d courses reads %.8g, a ")
				TEXT("factor of %.4g over a height ratio of %.4g — the model reads the column's ")
				TEXT("whole weight at its foot, so its separation is the ratio of the HANGING ")
				TEXT("BRICK COUNTS (%d against %d) and not of the heights"),
				Heights[0], Measured[0], Heights[1], Measured[1],
				Measured[0] > 0.0 ? Measured[1] / Measured[0] : 0.0,
				static_cast<double>(Heights[1]) / static_cast<double>(Heights[0]),
				Heights[0] - (CutCourse + 1), Heights[1] - (CutCourse + 1)),
			FMath::IsNearlyEqual(Measured[0], Measured[1], UtilisationTolerance));
	}

	return true;
}

/**
 * The fixture's own bricklayer lays the wall Layout::RunningBond lays.
 *
 * Without this the whole file is a second definition of what a wall is: nineteen expected outcomes
 * measured against a wall that is subtly not the game's wall would all measure the wrong thing,
 * plausibly, and still read as a number. The fixture exists only because six cases are not
 * running-bond rectangles; on the shape both producers can lay, they have to agree brick for brick.
 *
 * On boxes rather than handles, because the order a producer emits pieces in is its own business —
 * the sets of boxes have to match, and each box has to match somewhere.
 *
 * Needs a ticking world: no.
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
 * The production bricklayer lays exactly the twenty walls this file has been measuring.
 *
 * Why this test exists. The bricklayer above is test-only, so no level can reach it — which is why
 * the walls the user drew and reviewed could not be catalogue rows until the same geometry existed
 * in production. Writing that producer was the cheap part; the expensive part is that every
 * reading in this file is a statement about a particular arrangement of particular bricks, worked
 * to seventeen digits: 0.195160875 at a four-step corbel's bottom rung, 0.058203838191552663 at a
 * bare header's bed joint, 0.01000825 of head-joint shear, and three known-red rows whose failure
 * messages name bricks by (course, cell). A producer that differs from the fixture by one ulp
 * anywhere is not "a fixture that moved" — it's a dozen tests changing their answers at once, and
 * several are already red, so a changed message there would hide a regression inside a known
 * failure.
 *
 * What is compared, and why handle order is in the list.
 *
 * The piece count, every box centre and extent, every mass, every grounded flag, and the whole
 * connection set IN ORDER — pairing, normal, area, centre and half-extent. Exact `==` throughout,
 * because "bit-identical" is the claim and a tolerance lets exactly the drift this exists to
 * refuse through.
 *
 * A builder that laid the same bricks in a different sequence would renumber every joint and every
 * break stamp while every geometric check still passed — the failure that looks like nothing, so
 * it is asserted rather than assumed.
 *
 * The grid and the regions move with it: `FWallRegion` is the vocabulary every cut and named
 * outcome is written in, and resolving one needs each piece's (course, cell), so production must
 * hand those back and name the same bricks the fixture names, or twenty levels cut the wrong
 * bricks while laying the right wall.
 *
 * It is still live: the bricklayer was copied, not moved. This block used to say the test would go
 * quiet once the producer landed (the fixture becoming a call to production, comparing a thing
 * against itself — the state the corbel builder's fixture comparison reached and was deleted for).
 * That never happened here: `LayWall` above still lays its own bricks and calls nothing in
 * `DestructionWallCases` — the producer's landing commit was 853 insertions and zero deletions in
 * this file. So this compares two independent bricklayers and is the only test that does; deleting
 * it as spent would leave a change to the joint-discovery loop free to renumber every joint index
 * and break stamp while every geometric check still passes.
 *
 * `Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays` stands beside it rather than in place of
 * it: that one claims this producer lays the wall `Layout::RunningBond` lays, on the shape both can
 * lay, and is why this file cannot become a second definition of what a wall is.
 *
 * Needs a ticking world: no. Boxes and doubles; no solve at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceProducerMatchesFixtureTest,
	"DestructionGame.Acceptance.Wall.TheProducerLaysTheWallTheFixtureLays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceProducerMatchesFixtureTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/**
	 * A cell index is an index, not a reading, held to a tolerance rather than a bit. Every region
	 * bound in this file is at least an eighth of a cell (0.125) clear of any brick centre, so this
	 * is eight orders of magnitude below anything that could change which region a piece falls in.
	 * The course number is an integer and is held exactly.
	 */
	constexpr double CellTolerance = 1.0e-9;

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty-two cases"), Cases.Num(), 22);

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
 * Every one of the twenty is a level a human can join, laying the same wall and cutting the same
 * bricks the acceptance row measures.
 *
 * Why the check lives here rather than beside the catalogue. The verdicts stay in this file, so
 * the only place that knows what case 8 is, is this file. A test in
 * `Tests/DestructionScenariosTest.cpp` could assert that a row called `wall-08` exists and builds,
 * not that the wall it builds is the one case 8's outcome was measured against — the only claim
 * worth making. The include direction stays test-includes-production: this reads
 * `World/DestructionScenarios.h`, and nothing in production reads anything here.
 *
 * What is asserted, and why the cut is the half that matters. Each case must have a row carrying
 * the case's title verbatim, so a level and a catalogue entry can't describe different things; the
 * wall it builds must be the fixture's wall brick for brick and in handle order; and the bricks it
 * resolves to cut must be exactly the bricks the case's cut regions name.
 *
 * Most of these rows cut, and that is the point of the level: the wall stands, the player looks at
 * it, then the bricks go and they watch what the wall does about it. A level that laid the right
 * wall and cut a brick two cells over would show a plausible collapse unrelated to the case it's
 * named after — the exact failure the whole acceptance set exists to make visible, relocated to
 * where nobody is looking. Cases 1 and 17 are intact walls and must cut nothing, asserted from the
 * case's own empty cut list rather than a list of numbers here.
 *
 * The set is compared sorted, because the order of a cut is the row's own business: every cut
 * brick goes in one batch and one solve, so nothing downstream can see the sequence — which bricks
 * is the whole claim.
 *
 * Needs a ticking world: no. The catalogue and producer are both world-free; what a level does with
 * them is `World.Scenarios.*`'s business and is already covered.
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

	TestEqual(TEXT("FIXTURE: the catalogue is twenty-two cases"), Cases.Num(), 22);

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
					TEXT("%s: the walls the user drew are the whole reason the acceptance set ")
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
		 * The title is the case's, verbatim: the catalogue, the acceptance row and the banner a
		 * player reads have to be three views of one case rather than three descriptions that
		 * merely agree today.
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
		 * The intact rows cut nothing, derived from the case rather than listed: cases 1 and 17 are
		 * whole walls, and a row that quietly took a brick out of one would turn a regression anchor
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
 * No level puts a verdict on screen that the solver doesn't produce.
 *
 * Why a caption needs a test at all. Three of these twenty-two rows are red today (10, 19, 20),
 * and a level captioned "the course over the doorway drops and the wall stands" while the model
 * drops nothing is a lie told to somebody standing in front of the counter-example — worse than
 * silence, since the caption is the only thing telling the player what they're looking at. Since
 * 2026-08-12 two of the three lie the other way: cases 10 and 19 are captioned `Expected: STANDS`
 * and carry the disagreement marker, because they were re-ruled to stand while the model still
 * drops 12 and 34 bricks. So a caption may say what's expected, may say the model disagrees, but
 * may not claim a verdict the solver doesn't currently produce without admitting it.
 *
 * The convention, and why it's a token rather than prose matching. A caption is prose and has to
 * stay prose; what's pinned is one token — `Expected: STANDS`, `Expected: LOCAL LOSS` or
 * `Expected: COLLAPSE`, spelled the way `VerdictName` spells it — matching the verdict the
 * acceptance row asserts. Matching whole sentences would make the caption unwritable; matching
 * nothing would let it drift, which has already happened twice (cases 8 and 16 corrected in
 * WALL_CASES.html, left stale here for a day).
 *
 * Which rows must admit a disagreement is computed, never listed: the row is run, its verdict
 * evaluated, and the marker required exactly when the model got it wrong — so a slice that fixes
 * case 20 turns this red until the caption catches up, and so does a re-ruling that hands a row to
 * the model (case 8's 2026-08-11 STANDS ruling did exactly this: wall-08's caption still claimed
 * LOCAL LOSS with the marker, and this test held red until the caption moved in the same slice). A
 * hardcoded list would have rotted silently the other way.
 *
 * The 2026-08-12 rulings exercised both directions at once: case 9 re-ruled STANDS with the model
 * agreeing (caption drops the marker), cases 10 and 19 re-ruled STANDS with the model disagreeing
 * (captions keep it) — three rows, one verdict change each, two opposite outcomes, none of it
 * listed anywhere.
 *
 * The known-red set is a tripwire on this test's own predicate, not a second copy of the rule:
 * `ModelAgreesWithVerdict` is a second reading of the three verdict shapes
 * `Acceptance.Wall.Catalogue` asserts one at a time, and could drift from the catalogue while every
 * catalogue row still failed identically. Requiring it to name precisely the known-red rows makes
 * that drift visible.
 *
 * Needs a ticking world: no. It solves the same twenty walls the catalogue solves.
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
	 * The rows the model gets wrong today.
	 *
	 * This is not what decides which caption must admit a disagreement — that's computed per row
	 * below. It's a check on the predicate that computes it: a second reading of the verdict rules
	 * that drifted from the catalogue's would caption a level wrongly while every catalogue failure
	 * stayed word for word identical, the one failure this file couldn't otherwise see.
	 *
	 * Cases 11, 12, 8 and 9 all left this set by an expectation moving rather than a regression
	 * (re-rulings on 2026-08-08, -09, -11 and -12 respectively — see their blocks in section B/C).
	 * Cases 10 and 19 joined the same day but in the inverted direction: re-ruled STANDS while the
	 * model still drops 12 and 34 pieces, so membership here no longer means "the catalogue expects
	 * more damage than the model produces" — for these two it's the reverse.
	 *
	 * Case 21 joined at the 2026-08-14 mean re-anchor flip, inverted alongside 10 and 19: ruled
	 * COLLAPSE (hand deep-beam statics at 1.71x mean bond), but at mean strengths the model now
	 * STANDS the wall — measured worst 0.93542327561664174, a knife edge 6.5% under the line, on an
	 * axis other than the bending the hand check condemns (unverified — see the pre-cascade block
	 * above the case-21 catalogue row). Case 22 is deliberately not on it: its collapse survived the
	 * re-anchor on a knife edge (~1.2x, was 8.2x) and the model still produces it, so the row agrees
	 * and is green.
	 */
	/*
	 * Shrank to {21} at slice 4 (2026-08-27). Cases 10, 19 and 20 left this set the day the
	 * equilibrium LP became the break authority below the 200-block cap: it stands all three
	 * (lambda* 111.5 / 48.0 / 218.42) where the router only stranded or over-dropped, so the model
	 * now agrees with their STANDS verdicts. Case 21 stays — its ruled COLLAPSE the LP still
	 * contradicts (stands the wall at 17.24), the one surviving inverted red. This edit is the
	 * paperwork the tripwire below demands when the set shrinks: the list, the captions'
	 * disagreement markers, and the rows' DropsToday/StrandsToday anchors all came off together.
	 */
	const int32 KnownDisagreements[] = { 21 };

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty-two cases"), Cases.Num(), 22);

	TArray<int32> Disagreed;

	for (const FWallCase& Case : Cases)
	{
		const FString LevelName = LevelNameForCase(Case.Number);

		const FString Where =
			FString::Printf(TEXT("case %d (%s) as '%s'"), Case.Number, Case.Title, *LevelName);

		/*
		 * What the model actually does with this wall, read before the row is looked up and
		 * deliberately not skipped when there is no row yet. The tripwire at the bottom checks the
		 * predicate rather than the captions, and a predicate only exercised once the levels existed
		 * would be unproven exactly when it was needed. Run this way it reproduces the known reds on
		 * the day it is written.
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
	 * Two failures, not one, because the two ways this set can move mean opposite things. The
	 * single `Disagreed == Known` row this replaces said only "these differ":
	 *
	 *   GREW    a row the model used to get right now reads wrong. Nothing about the expectations
	 *           moved, so either the solver regressed or a verdict was just re-ruled and this list
	 *           hasn't caught up. Read it as a regression until the diff proves otherwise.
	 *   SHRANK  a row on the known-red list now reads right. A slice fixed it, or an expectation
	 *           was corrected to what the model already did — the paperwork is owed: this list, the
	 *           caption's disagreement marker, and the row's `DropsToday` anchor all come off
	 *           together.
	 *
	 * Case 11 is why this distinction is worth a second assertion: ruled down to a local loss on
	 * 2026-08-08 and back to "stands" the same day, both edits moved this set while the solver
	 * didn't change by one line, and "these differ" made the good news and the bad news look
	 * identical.
	 *
	 * Together they are still exactly set equality — every number appears at most once on either
	 * side, so mutual containment is the same claim split by direction.
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
