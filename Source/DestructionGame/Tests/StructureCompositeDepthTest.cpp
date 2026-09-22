// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * Named, not anonymous: a unity build merges translation units, so two anonymous namespaces
 * become one and identically-named helpers in unrelated files collide at compile time.
 */
namespace StructureCompositeDepthTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Constants spelled out from first principles, not imported: same discipline as
	 * ArchingWallTestSupport and StaircaseWallTestSupport. Strengths are asserted against the
	 * profile, not read from it — a test that reads production's constant agrees with a wrong one.
	 */

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double MortarJointCm = 1.0;

	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;
	constexpr double CoursePitchCm = BrickHeightCm + MortarJointCm;
	constexpr double HalfStepCm = BrickPitchCm / 2.0;

	/** 1.9 g/cm3, density first in the product so it matches Layout::PieceMassKg to the last bit. */
	constexpr double BrickMassKg = 1.9 * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 980 cm/s2. With 1 uu = 1 cm and mass in kg, MassKg * 980 is a force in Unreal units. */
	constexpr double BrickWeightUu = BrickMassKg * 980.0;

	/**
	 * Force units that load 1 cm2 to 1 MPa. 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2
	 * is 10000 uu. Not DestructionForce::ForceUnitsPerMPaSqCm on purpose: this file must fail if
	 * that constant is wrong.
	 */
	constexpr double ForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/*
	 * Mean flexural bond f_x1 for general-purpose mortar, asserted against the profile (re-anchor
	 * 2026-08-13; retired characteristic f_xk1 was 0.10). Every expectation here divides by this,
	 * so the whole ladder re-derived /7.
	 */
	constexpr double MortarTensileMPa = 0.70;

	/** The half-brick bed patch a running-bond brick keeps when it loses one of its two seats. */
	constexpr double HalfSeatAreaSqCm = BrickWidthCm * BrickWidthCm;

	/** Half of that patch on each in-plane axis; it is square, so the two are equal. */
	constexpr double HalfSeatHalfExtentCm = BrickWidthCm / 2.0;

	/** How far a half-seated brick's own weight acts from the centroid of the patch it keeps. */
	constexpr double HalfSeatEccentricityCm = HalfStepCm / 2.0;

	/**
	 * Bed patch section modulus: W = (4/3) * h_u * h_v^2 for a rectangle, cm3. 179.4817708 cm3 for
	 * the square 10.25 x 10.25 patch — the number every corbel is read against today.
	 */
	constexpr double PatchModulusCm3 =
		(4.0 / 3.0) * HalfSeatHalfExtentCm * HalfSeatHalfExtentCm * HalfSeatHalfExtentCm;

	/**
	 * Composite section: W = t * D^2 / 6 for a wall of thickness t and vertical depth D
	 * (ARCHING_DESIGN.md slice 5). A stack of courses over a lost support acts as a deep beam, not
	 * a sequence of independent bed patches, so the section is the full depth of bonded masonry
	 * above the cut. At D = 82.5 cm (11 courses over the staircase void) that is 11,627 cm3 against
	 * the patch's 179.48 — a factor of 64.8, which flips the outcome.
	 *
	 * The two models are nested: the patch is square, so (4/3)*5.125*5.125^2 equals 10.25*10.25^2/6
	 * to the last bit — the patch is this formula at D = 10.25 cm. So a depth rule that finds nothing
	 * returns today's answer by arithmetic, no special case. Asserted below.
	 */
	constexpr double CompositeModulusCm3(double WallThicknessCm, double DepthCm)
	{
		return WallThicknessCm * DepthCm * DepthCm / 6.0;
	}

	/** Print a double so a comparison that failed in the last bit is readable as one. */
	inline FString Bits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/*
	 * The raking corbel, generalised in height: the staircase void with its 13 courses turned into
	 * a parameter, and the cut's height (CorbelSteps) separated from the wall's (CoursesHigh).
	 * Slice 5 is a claim about the depth of masonry acting compositely, so it needs the same cut at
	 * several depths.
	 *
	 * Until 2026-08-07 CorbelSteps and CoursesHigh were one number (CoursesHigh = Steps + 2), so the
	 * wall's own height fed the composite-depth answer unseen. Separating them recovers the four
	 * original rows bit for bit by passing CorbelSteps = CoursesHigh - 2.
	 *
	 * The step count must be odd — a fact about running bond, not a choice: the edge steps half a
	 * cell per course, so course c's corbelled brick sits at (CorbelSteps + 1 - c) * 11.25, which
	 * lands on an even course's grid only when CorbelSteps is odd. An even cut names a brick that is
	 * not there. The wall's height is now free — 40 courses over an 11-step cut is the scenario wall.
	 */

	/** The staircase edge for a cut of this height: everything left of it, in this course, is cut. */
	constexpr double RakingVoidEdgeXCm(int32 CorbelSteps, int32 Course)
	{
		return (CorbelSteps + 1 - Course) * HalfStepCm;
	}

	/** The lowest and highest courses the void cuts. Course 0 and everything above stay whole. */
	constexpr int32 LowestVoidCourse = 1;

	constexpr int32 HighestVoidCourse(int32 CorbelSteps)
	{
		return CorbelSteps;
	}

	/**
	 * The corbelled courses: 2 up to one course above the cut, so CorbelSteps of them. Course 1's
	 * leftmost survivor still rests on two grounded bricks, so it is not corbelled; the course above
	 * the cut is corbelled anyway, since the course beneath it now starts half a step right. In a
	 * taller wall, everything above is full-width masonry standing on it — the masonry this file
	 * exists to measure.
	 */
	constexpr int32 LowestCorbelCourse = 2;

	constexpr int32 HighestCorbelCourse(int32 CorbelSteps)
	{
		return CorbelSteps + 1;
	}

	/** Centre height of a course, cm: half a brick up, then one course pitch per course. */
	constexpr double CourseZCm(int32 Course)
	{
		return BrickHeightCm / 2.0 + Course * CoursePitchCm;
	}

	/**
	 * What one step of the corbel carries, in brick weights — the hand ladder as a closed form.
	 * Each corbelled brick takes its own weight, all of the corbelled brick above it, and half of
	 * the next brick along. From the top:
	 *
	 *     F(s) = 1 + s + s(s+1)/4          s steps below the top
	 *
	 * which unrolls to 1, 2.5, 4.5, 7, 10, 13.5, 17.5, 22, 27, 32.5, 38.5 —
	 * StaircaseWallTestSupport's hand count exactly.
	 */
	constexpr double LadderForceBrickWeights(int32 StepsBelowTop)
	{
		return 1.0 + StepsBelowTop + StepsBelowTop * (StepsBelowTop + 1) / 4.0;
	}

	/**
	 * What bends that step, in brick-weight-cm — the other hand ladder, closed. The top step carries
	 * only itself at the 5.625 cm half-seat arm; each step below adds its own 5.625 to the step
	 * above's moment across the 11.25 cm the corbel stepped out:
	 *
	 *     M(0) = 5.625
	 *     M(s) = 5.625 + 11.25 * F(s-1) + M(s-1)
	 *
	 * summing to 5.625(s+1) + 11.25 * SUM(F(0..s-1)). Unrolls to 5.625, 22.5, 56.25, 112.5,
	 * 196.875, 315, 472.5, 675, 928.125, 1237.5, 1608.75.
	 */
	constexpr double LadderMomentBrickWeightCm(int32 StepsBelowTop)
	{
		const double S = StepsBelowTop;

		const double SumOfForces =
			S + S * (S - 1.0) / 2.0 + (S - 1.0) * S * (S + 1.0) / 12.0;

		return HalfSeatEccentricityCm * (S + 1.0) + HalfStepCm * SumOfForces;
	}

	/**
	 * Bonded masonry standing over one step's bed joint, in courses: courses c through the top,
	 * CoursesHigh - c of them. For the bottom step of the 13-course staircase, 11 courses / 82.5 cm
	 * — the depth ARCHING_DESIGN's 11,627 cm3 is worked from.
	 *
	 * This is the unbounded reading: all the masonry there is, the most any depth rule may credit. A
	 * rule that bounds it further (shear transfer, bond continuity, span-to-depth) reads less, so
	 * every "must stand" row is an upper bound and every "must come down" row is one-sided. Only
	 * asserted where the wall stops at the top of the corbel; taller walls are PART 1B, printed not
	 * derived.
	 */
	constexpr int32 CoursesOverCorbelJoint(int32 CoursesHigh, int32 Course)
	{
		return CoursesHigh - Course;
	}

	/**
	 * What one step reads today: bending on its own bed patch, less the compression closing it,
	 * against f_xk1. The bottom rung on the 13-course wall is 22.9295 —
	 * `Core.Structure.AStaircaseVoidCondemnsTheCorbel` pins that.
	 */
	inline double PatchTensionMPa(double MomentBrickWeightCm, double ForceBrickWeights)
	{
		const double BendingMPa = MomentBrickWeightCm * BrickWeightUu
			/ (PatchModulusCm3 * ForceUnitsPerMPaPerSqCm);

		const double NormalMPa = ForceBrickWeights * BrickWeightUu
			/ (HalfSeatAreaSqCm * ForceUnitsPerMPaPerSqCm);

		return FMath::Max(0.0, BendingMPa - NormalMPa);
	}

	/**
	 * What the same step reads once the whole depth resists the moment together: pure bending on the
	 * composite section, no axial relief. The choice among three readings is the whole model:
	 *
	 *   (a) sigma_b on the composite section alone. Staircase reads M/(W*f_xk1) = 0.36903, the
	 *       design's published 0.369; a half seat reads 0.1561287 * n / m^2, whose m = 19 row is
	 *       0.0082173, the design's free-end figure. Both published targets fall out of (a) alone.
	 *
	 *   (b) composite modulus and composite bearing area. Staircase reads 0.2476, neither published
	 *       figure reproduces, and a half seat goes into pure compression past 4.5 courses.
	 *
	 *   (c) composite modulus, patch area still carrying the axial term (the minimal edit). Reads
	 *       exactly zero for the staircase; disqualified because sigma_n exceeds sigma_b for every
	 *       fixture here, so bending stops governing and nothing can ever break again — the
	 *       indestructible failure the design names.
	 *
	 * Physically (a) is also honest: the plane resisting a deep-beam moment is vertical, the wedge's
	 * weight is shear on it, and there is no compression on that plane to subtract.
	 */
	inline double CompositeTensionMPa(
		double MomentBrickWeightCm, double DepthCm, double WallThicknessCm)
	{
		return MomentBrickWeightCm * BrickWeightUu
			/ (CompositeModulusCm3(WallThicknessCm, DepthCm) * ForceUnitsPerMPaPerSqCm);
	}

	/**
	 * What a corbel step must read after slice 5: the lesser of the two readings, over f_xk1. The
	 * `min` makes the model nested, not a second model bolted on: composite action is an alternative
	 * way of carrying the moment, so it may only ever help. Three checkable consequences:
	 *
	 *   - One course of depth is not a deep beam. D = 7.5 cm gives W = 96.09 cm3, smaller than the
	 *     patch's 179.48, so the `min` discards it. That is why `StructureBinding`'s waisted brick
	 *     and `StructurePushTest`'s ragged end keep 0.058203838: both are the top course of their wall.
	 *
	 *   - An intact wall is untouched bit for bit: every seat has e = 0, so M = 0 and both readings are 0.
	 *
	 *   - A depth rule that finds nothing returns today's answer by arithmetic, because the patch is
	 *     this formula at D = 10.25 cm.
	 */
	inline double CorbelUtilisation(
		double MomentBrickWeightCm,
		double ForceBrickWeights,
		int32 CoursesOfDepth,
		double WallThicknessCm,
		double TensileStrengthMPa)
	{
		const double Patch = PatchTensionMPa(MomentBrickWeightCm, ForceBrickWeights);

		const double Composite = CompositeTensionMPa(
			MomentBrickWeightCm, CoursesOfDepth * CoursePitchCm, WallThicknessCm);

		if (!(TensileStrengthMPa > 0.0))
		{
			/*
			 * Dry stone has exactly zero tensile strength, so any tension has already gone.
			 * ComputeUtilisation returns TNumericLimits<double>::Max() for this; match it rather
			 * than dividing by zero and producing an infinity that compares differently.
			 */
			return FMath::Min(Patch, Composite) > 0.0 ? TNumericLimits<double>::Max() : 0.0;
		}

		return FMath::Min(Patch, Composite) / TensileStrengthMPa;
	}

	/** The wall this file cuts a raking void into: same brick, same joint, same bond, one height. */
	inline FRunningBondSpec RakingWallSpec(
		int32 CoursesHigh, int32 BricksPerCourse, const FConnectionStrength& Strength)
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		Spec.JointThicknessCm = MortarJointCm;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = CoursesHigh;
		Spec.BricksPerCourse = BricksPerCourse;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = Strength;
		return Spec;
	}

	/** Which course a laid box is in, read back off its height. */
	inline int32 CourseOf(const FPieceBox& Box)
	{
		return FMath::RoundToInt32((Box.CentreCm.Z - BrickHeightCm / 2.0) / CoursePitchCm);
	}

	/** Every piece a raking void of this many steps takes out, in handle order. */
	inline TArray<int32> RakingVoidPieces(TArrayView<const FPieceBox> Boxes, int32 CorbelSteps)
	{
		TArray<int32> Cut;

		for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
		{
			const int32 Course = CourseOf(Boxes[Piece]);

			if (Course < LowestVoidCourse || Course > HighestVoidCourse(CorbelSteps))
			{
				continue;
			}

			if (Boxes[Piece].CentreCm.X < RakingVoidEdgeXCm(CorbelSteps, Course) - 0.001)
			{
				Cut.Add(Piece);
			}
		}

		return Cut;
	}
}

/*
 * ArchingWallTestSupport is aliased, not opened: it re-derives the grid from first principles and
 * carries its own BrickLengthCm, HalfSeatAreaSqCm, BrickWeightUu, Bits and more, so a
 * `using namespace` over both would make every shared name ambiguous. The five names this file
 * wants are unique and are qualified.
 */
namespace ArchSupport = StructureArchingTestSupport;

/**
 * A corbel resists with the whole depth of masonry standing over it, and comes down when that
 * depth runs out.
 *
 * THE RULING (the user's): a brick deleted at a free end must NOT bring the wall down. Any rule
 * local enough to save the free end also saves the staircase corbel, so the ruling adopts composite
 * vertical action, which makes `Core.Structure.AStaircaseVoidCondemnsTheCorbel` and
 * `Integration.AStaircaseVoidBringsTheOverhangDown` change. Consistent with the same user's
 * acceptance case 20 (the staircase void as local loss: loose teeth drop, the mass stands).
 *
 * MECHANISM. A stack of courses over a lost support acts as a deep beam; the section resisting the
 * overturning moment is the full vertical depth of bonded masonry above the cut:
 *
 *     W = t * D^2 / 6                            against the bed patch's 179.4817708 cm3
 *     D = 82.5 cm (11 courses)  ->  11,627 cm3   a factor of 64.8
 *
 * Resistance grows with the square of depth, so the depth is the model, not a parameter.
 *
 * WHAT IS ASSERTED:
 *   - The load path is unchanged, asserted first and separately: every corbel joint still carries
 *     the hand ladder's force and moment; composite action changes only the section. This keeps the
 *     utilisation claim non-circular — CURRENT_STATE records that an expectation read back off
 *     `GetConnectionMoment` moves with the defect being tested (it ate a gate row at slice 1).
 *   - The utilisation is the ladder's moment over the composite section, min-ed with the patch
 *     reading, at 2% (one course of eleven is 18% of the answer).
 *   - The outcome, never a displacement: two pieces can sever and stay resting where they were.
 *     "Must stand" rows assert every corbelled brick still has a path to the ground after
 *     SolveAndBreak; the "must come down" row asserts the corbel top lost its path and the bottom
 *     rung is among the joints that failed under load.
 *
 * THE THIRD CONSTRAINT (something must still come down), now carried by the dry row only. A
 * 45-course corbel used to fail because the depth ran out: on the characteristic f_xk1 = 0.10 the
 * moment growing as k^3 against a section growing as k^2 crossed 1.0 near 36 courses:
 *
 *     k       5      11      29      35      37      45
 *     reads   0.219  0.369   0.834   0.990   1.042   1.250      (x 0.10 / 0.70 at the mean basis)
 *
 * The 2026-08-14 mean re-anchor (f_x1 = 0.70) divides the ladder by 7, so no buildable mortared
 * corbel fails by depth below ~250 steps — past the ~124-step crushing crossover
 * (`CorbelStepsBeforeTensionWins`). The two 45-step rows are now unasserted on outcome (lost
 * discrimination, logged in CURRENT_STATE); the dry row still falls, and the must-fall guarantee
 * lives in `AHundredStepCorbelMustComeDown` (150 steps, crushing). The standing rows remain the
 * upper bound on how little depth a correct rule may find.
 *
 * The k^3/k^2 argument holds only for a wall as tall as its cut, which every old row was and none
 * said (review 2026-08-07, finding B1; PART 1B is the fixture). The depth is whatever the upward
 * walk finds, so in a taller wall the moment goes as k and the section as m^2, the reading as
 * k^3/m^2, the exponents stop cancelling and the crossover vanishes. The game's wall is that case
 * — 11 steps under 40 courses — so the joint pinned at 0.36903147272727271 on a 13-course wall is
 * a different number on the rendered wall. PART 1B lays both.
 *
 * ARCHING_DESIGN's own depth table (4 courses 2.79, 6 courses 1.24, 7 courses 0.911, crossover at
 * 50.1 cm) is a hypothetical, not a fixture: every row is 0.369 * (82.5/D)^2, the staircase moment
 * held fixed while depth shrinks, which no wall presents. In a real corbel the moment shrinks with
 * the depth and the reading increases in k, so the crossover is near 36 courses, not under 7. The
 * rows below are real cut walls; the design's table is printed beside them, nothing derived from it.
 *
 * THE TWO PUBLISHED TARGETS. ARCHING_DESIGN flags both as unconfirmed idealisations:
 *   - The staircase's 0.369 reproduces exactly as M/(W*f_xk1), M = 1608.75, W = 11,627.34: 0.36903.
 *     Asserted at 2%.
 *   - The free end's 0.0082 reproduces only as one row of an identity no fixture presents: a half
 *     seat carrying n brick weights under m courses reads 0.1561287 * n / m^2, and n = m = 19 gives
 *     0.0082173. The 7 x 30 wall below has 29 courses over its joint and reads ~0.0052. The identity
 *     is asserted; the 0.0082 is printed as its m = 19 row, nothing derived.
 *
 * ANCHORS THIS FILE MUST NOT MOVE. `StructureBinding.AdoptedWallLoadsItsWaistEccentrically`
 * (0.058203838191552663) and `StructurePushTest`'s ragged end (0.0582038382): both are the top
 * course of their wall, D = 7.5 cm, W = 96.09 cm3 (smaller than the patch), so the `min` discards
 * the composite reading. Omitting the `min` moves both to 0.08359.
 *
 * Needs a ticking world: no. FStructure and Layout are arithmetic; slices 1-4 needed none either.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCompositeDepthTest,
	"DestructionGame.Core.Structure.ACorbelResistsWithItsWholeDepth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCompositeDepthTest::RunTest(const FString& Parameters)
{
	using namespace StructureCompositeDepthTestSupport;
	using namespace StaircaseWallTestSupport;

	/*
	 * The expected numbers are ratios of published strengths, so they hold only while the profile
	 * carries the figures they were derived against. Asserted, not imported: a test that read the
	 * profile would agree with a wrong one.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against f_xk1 = %g MPa, the profile carries %g"),
			MortarTensileMPa, GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == MortarTensileMPa);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: dry stone must have EXACTLY zero tensile strength, it has %g"),
			DryStone.TensileStrengthMPa),
		DryStone.TensileStrengthMPa == 0.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	/*
	 * ===================================================================================
	 * PART 0 — THE TWO SECTIONS ARE NESTED, AND THE TWO LADDER DERIVATIONS AGREE.
	 * ===================================================================================
	 *
	 * The bed patch is the composite formula at D = 10.25 cm: the patch is square, so (4/3)h^3 with
	 * h = 5.125 is 10.25 * 10.25^2 / 6 to the last bit. So a depth rule that finds nothing needs no
	 * special case — the cheapest statement that slice 5 is one model with a parameter, not two.
	 */
	/*
	 * Algebraically equal, not bit for bit: (4/3)*5.125*5.125^2 is 179.48177083333331 and
	 * 10.25*10.25^2/6 is 179.48177083333334, one ulp apart because IEEE multiplication does not
	 * associate. So recomputing the shallow case through the composite formula would move
	 * `AdoptedWallLoadsItsWaistEccentrically` and `StructurePushTest`'s ragged end in their last
	 * digits. The `min` avoids it: where composite is no better, the joint keeps the patch's own
	 * value verbatim. Same shape as slice 4's `d_e/L` choice.
	 */
	TestTrue(
		FString::Printf(
			TEXT("THE TWO SECTIONS ARE NESTED: the 10.25 x 10.25 bed patch is %s cm3 and ")
			TEXT("t*D^2/6 at D = 10.25 is %s — algebraically identical, %s apart relative"),
			*Bits(PatchModulusCm3), *Bits(CompositeModulusCm3(BrickWidthCm, BrickWidthCm)),
			*Bits(FMath::Abs(PatchModulusCm3 - CompositeModulusCm3(BrickWidthCm, BrickWidthCm))
				/ PatchModulusCm3)),
		FMath::Abs(PatchModulusCm3 - CompositeModulusCm3(BrickWidthCm, BrickWidthCm))
			<= 1.0e-15 * PatchModulusCm3);

	/*
	 * And the closed forms reproduce the hand count, all 22 numbers, exactly.
	 * StaircaseWallTestSupport walks both ladders by hand for the 13-course wall; this file needs
	 * them at other heights, so it carries the recursions closed. A drifted closed form would make
	 * every row below agree with itself and nothing else.
	 */
	for (int32 Course = StaircaseLowestCorbelCourse;
		Course <= StaircaseHighestCorbelCourse;
		++Course)
	{
		const int32 StepsBelowTop = StaircaseHighestCorbelCourse - Course;

		const double HandForce =
			StaircaseCorbelLoadBrickWeights[Course - StaircaseLowestCorbelCourse];
		const double HandMoment =
			StaircaseCorbelMomentBrickWeightCm[Course - StaircaseLowestCorbelCourse];

		TestTrue(
			FString::Printf(
				TEXT("THE LADDERS AGREE: course %d is %d steps below the top, so the hand count ")
				TEXT("says %s brick weights and %s brick-weight-cm; the closed forms say %s and %s"),
				Course, StepsBelowTop, *Bits(HandForce), *Bits(HandMoment),
				*Bits(LadderForceBrickWeights(StepsBelowTop)),
				*Bits(LadderMomentBrickWeightCm(StepsBelowTop))),
			LadderForceBrickWeights(StepsBelowTop) == HandForce
				&& LadderMomentBrickWeightCm(StepsBelowTop) == HandMoment);
	}

	/*
	 * ===================================================================================
	 * PART 1 — THE RAKING CORBEL AT THREE DEPTHS, THE SAME CUT LAID DRY, AND TWO CUTS
	 * LAID UNDER MORE WALL THAN THEY NEED.
	 * ===================================================================================
	 */

	/**
	 * What the cascade must do to the mass over the void — and one row declines to say.
	 * `Unasserted` is not a gap: whether an 11-step corbel under a 40-course wall stands depends on
	 * which composite-depth bound is chosen (its own 11 courses vs the wall's 39 give different
	 * verdicts), an open design question this file must not close. The row makes the two
	 * distinguishable and pins the property in PART 1B; the verdict is printed and left to whoever rules.
	 */
	enum class EVoidOutcome
	{
		MustStand,
		MustComeDown,
		Unasserted,
	};

	/** One raking void, and everything about it this file has an opinion on. */
	struct FCorbelCase
	{
		const TCHAR* Description;

		/** Odd, always (see RakingVoidEdgeXCm). Number of corbelled steps; sizes the cut — ladder, arms, moment. */
		int32 CorbelSteps;

		/**
		 * Sizes the wall: at least CorbelSteps + 2 courses to hold the cut, any number more. Extra
		 * courses are full-width masonry standing on the corbel — they add load to every rung and
		 * depth to the composite-section walk, and separating those two is the whole of PART 1B.
		 */
		int32 CoursesHigh;

		/**
		 * Wide enough that the load cone above the bottom step stays inside the wall: the cone
		 * reaches ~(2k - 2) half steps, so a k-step corbel wants ~k + 1 bricks per course.
		 * StaircaseWallTestSupport's 13 x 10 wall is 11.25 cm short and its bottom rung comes back
		 * 2.4e-6 low, inside this tolerance by three orders of magnitude.
		 */
		int32 BricksPerCourse;

		const FConnectionStrength* Strength;

		/** What must become of the mass over the void once the cascade has run. */
		EVoidOutcome Outcome;

		/** ARCHING_DESIGN's own figure for the bottom rung, or 0 where it published none. */
		double DesignUtilisation;
	};

	const TArray<FCorbelCase> Cases = {
		/*
		 * Five steps, the smallest unambiguous raking corbel (three steps reads 0.17 either way).
		 * Bottom rung 2.67180 today with 4 of 5 rungs over capacity; under a 37.5 cm section
		 * 0.21858 with none.
		 */
		{ TEXT("a FIVE-step raking corbel"), 5, 7, 7, &GeneralPurposeMortar,
			EVoidOutcome::MustStand, 0.0 },

		/*
		 * Eleven steps — the photographed failure, the identical fixture
		 * `AStaircaseVoidCondemnsTheCorbel` and `AStaircaseVoidBringsTheOverhangDown` cut. Bottom
		 * rung 22.929528199727653 today with 8 of 11 over capacity; under the 82.5 cm actually
		 * standing over it, 0.36903 with none. Those two tests invert with this row by the user's
		 * ruling, and the new number is derived here.
		 *
		 * Ten bricks per course, not twelve: it must be the same wall those two tests lay, cone
		 * deficit and all, or its number is about a different wall.
		 */
		/*
		 * The published 0.369 was against the characteristic f_xk1 = 0.10; at the mean basis
		 * (re-anchor 2026-08-13) the same statics divide by 0.70 and read 0.0527 — still
		 * ARCHING_DESIGN's figure re-based, not a new target.
		 */
		{ TEXT("ELEVEN steps: THE STAIRCASE VOID"), 11, 13, 10, &GeneralPurposeMortar,
			EVoidOutcome::MustStand, 0.369 / 7.0 },

		/*
		 * Forty-five steps — the row where the depth used to run out, un-run by the 2026-08-14 mean
		 * re-anchor. On the characteristic basis the k^3 moment against the k^2 section crossed
		 * capacity here (1.25019 against f_xk1 = 0.10) and the corbel came down. Against the mean
		 * 0.70 it reads 0.1786, and the tension ladder does not reach 1.0 below ~250 steps — past
		 * the ~124-step crushing crossover (the unmoved 10 MPa). So no mortared corbel below that
		 * crossover fails by depth any more; the falling arm is lost discrimination, recorded in
		 * CURRENT_STATE.
		 *
		 * Outcome now unasserted, not flipped to MustStand: whether a 7.9 m mortared overhang should
		 * stand is a ruling nobody has made, and asserting it would forbid any future depth bound
		 * crediting under ~2.4x fewer courses than the full walk. The must-fail guarantee lives in
		 * `AHundredStepCorbelMustComeDown` (150 steps, crushing) and the dry row below.
		 */
		{ TEXT("FORTY-FIVE steps: the depth runs out"), 45, 47, 47, &GeneralPurposeMortar,
			EVoidOutcome::Unasserted, 0.0 },

		/*
		 * The same staircase laid dry. You cannot corbel dry stone — no bond to transfer the shear
		 * composite action needs between courses — so any depth bound must leave it condemned.
		 *
		 * A guard, not a driver: DryStone's f_xk1 is an exact zero, so any tension has already gone
		 * at any section modulus, and this row cannot tell a bounded depth from an unbounded one. It
		 * catches an implementation that subtracts an axial term on the composite bearing area
		 * (reading (b) or (c)), which past 4.5 courses stands a dry-stone overhang up. Earns its
		 * place as the same fixture with one field changed.
		 */
		{ TEXT("ELEVEN steps, laid DRY"), 11, 13, 10, &DryStone,
			EVoidOutcome::MustComeDown, 0.0 },

		/*
		 * ===============================================================================
		 * AND THE SAME TWO CUTS AGAIN UNDER MORE WALL — the fixture the project lacked;
		 * without them nothing could see the composite depth.
		 * ===============================================================================
		 *
		 * Forty-five steps under 67 courses: identical cut to the row above (same ladder, arms,
		 * 91,209 brick-weight-cm of overturning) with 20 further full-width courses on top.
		 *
		 * A taller wall is not "arithmetically identical" to crediting more depth, which is the whole
		 * finding: the extra courses stand on the corbel, so they add load as well as depth. 47 to 67
		 * courses multiplies the bottom rung's moment by 2.2754 and its section by 2.0864, so the
		 * reading goes up (1.2501861088888881 -> 1.3634027672024234) and the corbel still comes down.
		 * A depth mutation moves one; a taller wall moves both.
		 *
		 * So it is the short cut under the tall wall (the row below) that bites: moment goes as k^3,
		 * section as m^2, so the ratio m/k decides. At 67/45 the added load nearly keeps up; at 40/11
		 * it is beaten twofold. A 45-step corbel needs ~160 courses before depth rescues it — not a
		 * buildable fixture.
		 *
		 * Kept as a guard, but the mean re-anchor took its outcome: 1.3634 against 0.10 is 0.1948
		 * against the mean 0.70, under the line, so the corbel stands. The strength-free
		 * moment/section measurements above still hold; only the verdict moved. Unasserted like the
		 * row above — lost discrimination (CURRENT_STATE), needing a re-design at the crushing
		 * crossover, not a re-tune.
		 */
		{ TEXT("FORTY-FIVE steps under SIXTY-SEVEN courses"), 45, 67, 47, &GeneralPurposeMortar,
			EVoidOutcome::Unasserted, 0.0 },

		/*
		 * Eleven steps under 40 courses — the wall the game renders, and the headless twin the
		 * screenshot fixture never had. `ADestructionGameGameMode` lays 30 x 40 and the visual
		 * harness cuts StaircaseWallTestSupport's void (courses 1-11, edge (12 - c) * 11.25) —
		 * character for character this cut. So the joint pinned at 0.36903147272727271 on a
		 * 13-course wall is the same joint the player sees under 39 courses. TEST_CHANGES.md section
		 * 4 names the gap: `Visual.StaircaseScreenshot` carries NonNullRHI, invisible to the
		 * documented test command, and rotted for five slices.
		 *
		 * Outcome unasserted (see EVoidOutcome): whether it stands is downstream of the bound — the
		 * corbel's own 11 courses vs all 39 give different verdicts. What is asserted is PART 1B's
		 * property, true under every candidate.
		 */
		{ TEXT("ELEVEN steps under FORTY courses: THE SCENARIO WALL"), 11, 40, 30,
			&GeneralPurposeMortar, EVoidOutcome::Unasserted, 0.0 },
	};

	/**
	 * The bottom rung of every case, kept so two cases can be compared. PART 1B's claim is about a
	 * pair of fixtures, not either alone, so it cannot be made inside the loop that reads one.
	 * Everything here is measured off the solver except the depth, which the fixture presents.
	 */
	struct FBottomRung
	{
		const TCHAR* Description;
		int32 CorbelSteps;
		int32 CoursesHigh;
		int32 BricksPerCourse;
		const FConnectionStrength* Strength;

		double ForceBrickWeights;
		double MomentBrickWeightCm;
		double Utilisation;

		/** What the walk finds if nothing bounds it: every course of the wall over this joint. */
		double UnboundedDepthCm;
	};

	TArray<FBottomRung> BottomRungs;

	for (const FCorbelCase& Case : Cases)
	{
		const int32 Steps = Case.CorbelSteps;

		/*
		 * Whether the wall stops at the top of the corbel decides what may be asserted rung by rung.
		 * Where it does, the corbel's own courses are all the masonry there is and the hand ladder is
		 * the whole load. Where the wall carries on, both stop being true at once (extra load and
		 * extra depth), so the rows below are one-sided and the two-sided claim moves to PART 1B.
		 */
		const bool bWallStopsAtTheCorbel = Case.CoursesHigh == Case.CorbelSteps + 2;

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the STEP count must be ODD or the raking edge names bricks ")
				TEXT("that are not there; it is %d"),
				Case.Description, Case.CorbelSteps),
			Case.CorbelSteps % 2 == 1);

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: a %d-step cut needs at least %d courses to stand in, the wall ")
				TEXT("has %d"),
				Case.Description, Case.CorbelSteps, Case.CorbelSteps + 2, Case.CoursesHigh),
			Case.CoursesHigh >= Case.CorbelSteps + 2);

		AddInfo(FString::Printf(
			TEXT("%s: %d x %d flush wall, raking void through courses %d..%d, %d corbelled steps, ")
			TEXT("%g cm of masonry over the bottom one (%g cm of it is the corbel's own)"),
			Case.Description, Case.BricksPerCourse, Case.CoursesHigh, LowestVoidCourse,
			HighestVoidCourse(Case.CorbelSteps), Steps,
			CoursesOverCorbelJoint(Case.CoursesHigh, LowestCorbelCourse) * CoursePitchCm,
			Case.CorbelSteps * CoursePitchCm));

		FBrickLayout Laid;

		if (!RunningBond(
				RakingWallSpec(Case.CoursesHigh, Case.BricksPerCourse, *Case.Strength), Laid))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: RunningBond should lay a %d x %d flush wall"),
				Case.Description, Case.BricksPerCourse, Case.CoursesHigh));

			continue;
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the laid wall must know where every piece and every joint is, ")
				TEXT("or every moment below is silently zero"),
				Case.Description),
			Laid.Structure.HasCompleteGeometry());

		/*
		 * Positive control: a wall that arrived with a joint past capacity would be condemned for a
		 * reason the void had nothing to do with. The flush end buys it — a ragged end brick is a
		 * half-seated cantilever before anything is cut.
		 */
		Laid.Structure.SolveLoads();

		double WorstAsBuilt = 0.0;

		for (int32 Joint = 0; Joint < Laid.Structure.NumConnections(); ++Joint)
		{
			WorstAsBuilt =
				FMath::Max(WorstAsBuilt, Laid.Structure.GetConnectionUtilisation(Joint));
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the wall as built must have nothing over capacity, its worst ")
				TEXT("joint reads %s"),
				Case.Description, *Bits(WorstAsBuilt)),
			WorstAsBuilt < 1.0);

		const TArray<int32> VoidPieces = RakingVoidPieces(Laid.Boxes, Case.CorbelSteps);

		bool bCutLaid = true;

		for (const int32 Piece : VoidPieces)
		{
			if (!Laid.Structure.RemovePiece(Piece))
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: piece %d should have been in the wall to remove"),
					Case.Description, Piece));

				bCutLaid = false;
				break;
			}
		}

		if (!bCutLaid || VoidPieces.Num() == 0)
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: the raking void should cut something, it cut %d pieces"),
				Case.Description, VoidPieces.Num()));

			continue;
		}

		Laid.Structure.SolveLoads();

		// --- the ladder, rung by rung --------------------------------------------------------

		int32 CorbelPieces[2] = { INDEX_NONE, INDEX_NONE };
		int32 BottomRungJoint = INDEX_NONE;
		TArray<int32> AllCorbelPieces;

		int32 RungsOverCapacity = 0;
		int32 PredictedRungsOverCapacity = 0;
		bool bLadderRead = true;

		for (int32 Course = LowestCorbelCourse;
			Course <= HighestCorbelCourse(Case.CorbelSteps);
			++Course)
		{
			const int32 StepsBelowTop = HighestCorbelCourse(Case.CorbelSteps) - Course;

			const double HandForceBrickWeights = LadderForceBrickWeights(StepsBelowTop);
			const double HandMomentBrickWeightCm = LadderMomentBrickWeightCm(StepsBelowTop);

			const int32 CoursesOfDepth = CoursesOverCorbelJoint(Case.CoursesHigh, Course);

			const double Expected = CorbelUtilisation(
				HandMomentBrickWeightCm, HandForceBrickWeights, CoursesOfDepth,
				BrickWidthCm, Case.Strength->TensileStrengthMPa);

			if (Expected > 1.0)
			{
				++PredictedRungsOverCapacity;
			}

			const int32 Corbel = StaircasePieceAt(
				Laid.Boxes,
				RakingVoidEdgeXCm(Case.CorbelSteps, Course),
				CourseZCm(Course));

			const int32 Support = StaircasePieceAt(
				Laid.Boxes,
				RakingVoidEdgeXCm(Case.CorbelSteps, Course) + HalfStepCm,
				CourseZCm(Course - 1));

			const int32 Joint = Corbel != INDEX_NONE && Support != INDEX_NONE
				? JointBetweenPieces(Laid.Structure, Corbel, Support)
				: INDEX_NONE;

			if (Joint == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: course %d should have a brick at x %g jointed to one at ")
					TEXT("x %g; they are pieces %d and %d"),
					Case.Description, Course, RakingVoidEdgeXCm(Case.CorbelSteps, Course),
					RakingVoidEdgeXCm(Case.CorbelSteps, Course) + HalfStepCm, Corbel, Support));

				bLadderRead = false;
				break;
			}

			AllCorbelPieces.Add(Corbel);

			if (Course == LowestCorbelCourse)
			{
				CorbelPieces[0] = Corbel;
				BottomRungJoint = Joint;
			}

			if (Course == HighestCorbelCourse(Case.CorbelSteps))
			{
				CorbelPieces[1] = Corbel;
			}

			/*
			 * Must be a half seat and nothing else, checked against the producer. Every number here
			 * is worked from a 10.25 x 10.25 patch loaded 5.625 cm off centroid; a fixture that
			 * stopped presenting one would agree with none of them.
			 */
			const FConnection& Bed = Laid.Structure.GetConnection(Joint);

			const bool bOneSeat = ArchSupport::TheOneIntactSeatBeneath(Laid.Structure, Corbel) == Joint;

			TestTrue(
				FString::Printf(
					TEXT("%s, course %d: FIXTURE: the corbel must rest on EXACTLY ONE bed joint ")
					TEXT("(it rests on %d), of %g cm2 with half-extents (%g, %g); MakeInterface ")
					TEXT("emitted %g cm2 with (%g, %g)"),
					Case.Description, Course, ArchSupport::IntactSeatsBeneath(Laid.Structure, Corbel),
					HalfSeatAreaSqCm, HalfSeatHalfExtentCm, HalfSeatHalfExtentCm,
					Bed.InterfaceAreaSqCm, Bed.InterfaceHalfExtentCm.X, Bed.InterfaceHalfExtentCm.Y),
				bOneSeat
					&& FMath::IsNearlyEqual(Bed.InterfaceAreaSqCm, HalfSeatAreaSqCm, 1.0e-9)
					&& FMath::IsNearlyEqual(
						Bed.InterfaceHalfExtentCm.X, HalfSeatHalfExtentCm, 1.0e-9)
					&& FMath::IsNearlyEqual(
						Bed.InterfaceHalfExtentCm.Y, HalfSeatHalfExtentCm, 1.0e-9));

			const FVector ForceUu = Laid.Structure.GetConnectionForce(Joint);
			const FVector MomentUuCm = Laid.Structure.GetConnectionMoment(Joint);

			const double Utilisation = Laid.Structure.GetConnectionUtilisation(Joint);

			const double MeasuredForceBrickWeights = ForceUu.Size() / BrickWeightUu;
			const double MeasuredMomentBrickWeightCm = MomentUuCm.Size() / BrickWeightUu;

			if (Utilisation > 1.0)
			{
				++RungsOverCapacity;
			}

			const bool bWorthPrinting =
				Steps <= 11 || Course == LowestCorbelCourse
				|| Course == HighestCorbelCourse(Case.CorbelSteps);

			if (bWorthPrinting)
			{
				/*
				 * The three readings printed beside the measurement use the moment this joint
				 * actually carries, not the hand ladder's. They coincide only where the wall stops at
				 * the top of the corbel; in a taller wall the hand ladder misses what the courses
				 * above add (7x on the scenario wall), so a line printed from it would read 0.031
				 * beside a joint reading 0.223 and look like a solver defect. The hand ladder is still
				 * printed, as the ladder.
				 */
				AddInfo(FString::Printf(
					TEXT("%s, course %2d: joint %d carries %s brick weights (hand ladder %s) and ")
					TEXT("%s brick-weight-cm (hand ladder %s) over %d courses = %g cm of depth. ")
					TEXT("It reads %s; on the moment it carries the patch alone says %s, the ")
					TEXT("composite section at that depth says %s, and the corbel's OWN %d ")
					TEXT("courses would say %s"),
					Case.Description, Course, Joint, *Bits(MeasuredForceBrickWeights),
					*Bits(HandForceBrickWeights), *Bits(MeasuredMomentBrickWeightCm),
					*Bits(HandMomentBrickWeightCm), CoursesOfDepth,
					CoursesOfDepth * CoursePitchCm, *Bits(Utilisation),
					*Bits(PatchTensionMPa(MeasuredMomentBrickWeightCm, MeasuredForceBrickWeights)
						/ MortarTensileMPa),
					*Bits(CompositeTensionMPa(
						MeasuredMomentBrickWeightCm, CoursesOfDepth * CoursePitchCm, BrickWidthCm)
						/ MortarTensileMPa),
					FMath::Min(CoursesOfDepth, Case.CorbelSteps),
					*Bits(CompositeTensionMPa(
						MeasuredMomentBrickWeightCm,
						FMath::Min(CoursesOfDepth, Case.CorbelSteps) * CoursePitchCm,
						BrickWidthCm)
						/ MortarTensileMPa)));
			}

			/*
			 * The load path first, separately from the section. Composite action changes what
			 * resists the moment, not what the wall hands down, so force and moment must both still
			 * be the hand ladder's. 2% rather than the staircase test's 1e-5 because ARCHING_DESIGN's
			 * own cross-check (wedge centroid arm against ladder average arm) agrees only to 1.3%.
			 *
			 * Two-sided only where the wall stops at the top of the corbel: there the hand ladder is
			 * the whole load. Where the wall carries on, those courses bear down through the same
			 * rungs and the ladder can only grow, so the claim is `at least` — the premise PART 1B's
			 * one-sided argument stands on.
			 */
			const double ForceFloor = HandForceBrickWeights * (1.0 - 0.02);
			const double MomentFloor = HandMomentBrickWeightCm * (1.0 - 0.02);

			TestTrue(
				FString::Printf(
					TEXT("%s, course %d: the load path is UNCHANGED — the joint must still carry ")
					TEXT("%s the hand ladder's %s brick weights, it carries %s"),
					Case.Description, Course, bWallStopsAtTheCorbel ? TEXT("") : TEXT("at least"),
					*Bits(HandForceBrickWeights), *Bits(MeasuredForceBrickWeights)),
				bWallStopsAtTheCorbel
					? FMath::Abs(MeasuredForceBrickWeights - HandForceBrickWeights)
						<= 0.02 * HandForceBrickWeights
					: MeasuredForceBrickWeights >= ForceFloor);

			TestTrue(
				FString::Printf(
					TEXT("%s, course %d: and %s the hand ladder's %s brick-weight-cm of moment, ")
					TEXT("it carries %s"),
					Case.Description, Course, bWallStopsAtTheCorbel ? TEXT("") : TEXT("at least"),
					*Bits(HandMomentBrickWeightCm), *Bits(MeasuredMomentBrickWeightCm)),
				bWallStopsAtTheCorbel
					? FMath::Abs(MeasuredMomentBrickWeightCm - HandMomentBrickWeightCm)
						<= 0.02 * HandMomentBrickWeightCm
					: MeasuredMomentBrickWeightCm >= MomentFloor);

			/*
			 * Then the section, which is the slice: `min(patch reading, M / (t D^2 / 6))`, both from
			 * the hand ladder and the depth the fixture presents. A dry row is asserted as an
			 * ordering, not a number, because f_xk1 = 0 makes the answer TNumericLimits<double>::Max()
			 * and a relative tolerance on that says nothing.
			 *
			 * Not asserted at all where the wall carries on above the corbel: both halves of
			 * `Expected` are wrong there — the moment is the hand ladder's, not the heavier one the
			 * wall produces, and the depth is the unbounded `CoursesHigh - Course`, the thing under
			 * suspicion. Asserting it would pin the defect. Printed above with both candidate
			 * sections; the claim is in PART 1B.
			 */
			if (Expected >= TNumericLimits<double>::Max())
			{
				TestTrue(
					FString::Printf(
						TEXT("%s, course %d: with f_xk1 exactly zero, ANY tension has already ")
						TEXT("gone — the joint must read past capacity, it reads %s"),
						Case.Description, Course, *Bits(Utilisation)),
					Utilisation > 1.0);
			}
			else if (bWallStopsAtTheCorbel)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s, course %d: %d courses of bonded masonry stand over this joint, ")
						TEXT("so its section is t*D^2/6 = %s cm3 rather than the patch's %s, and ")
						TEXT("it must read %s — it reads %s"),
						Case.Description, Course, CoursesOfDepth,
						*Bits(CompositeModulusCm3(BrickWidthCm, CoursesOfDepth * CoursePitchCm)),
						*Bits(PatchModulusCm3), *Bits(Expected), *Bits(Utilisation)),
					FMath::Abs(Utilisation - Expected) <= 0.02 * FMath::Max(Expected, 1.0e-12));
			}

			if (Course == LowestCorbelCourse)
			{
				BottomRungs.Add({
					Case.Description, Case.CorbelSteps, Case.CoursesHigh, Case.BricksPerCourse,
					Case.Strength, MeasuredForceBrickWeights, MeasuredMomentBrickWeightCm,
					Utilisation, CoursesOfDepth * CoursePitchCm });
			}
		}

		if (!bLadderRead)
		{
			continue;
		}

		AddInfo(FString::Printf(
			TEXT("%s: %d of %d rungs read over capacity; the composite section over the corbel's ")
			TEXT("OWN %d courses predicts %d"),
			Case.Description, RungsOverCapacity, Steps, Case.CorbelSteps,
			PredictedRungsOverCapacity));

		if (bWallStopsAtTheCorbel)
		{
			TestEqual(
				FString::Printf(
					TEXT("%s: exactly the rungs the composite section condemns must be over ")
					TEXT("capacity"),
					Case.Description),
				RungsOverCapacity, PredictedRungsOverCapacity);
		}

		/*
		 * The design's own figure where it published one — a factor of two either way, an
		 * order-of-magnitude cross-check. Still catches a missing 100x, a section modulus off by the
		 * 64.8 this slice turns on, or a moment deleted rather than re-sectioned.
		 */
		if (Case.DesignUtilisation > 0.0 && BottomRungJoint != INDEX_NONE)
		{
			const double Measured = Laid.Structure.GetConnectionUtilisation(BottomRungJoint);

			TestTrue(
				FString::Printf(
					TEXT("%s: ARCHING_DESIGN predicts %g for the bottom rung and it reads %s — a ")
					TEXT("cross-check, not a target, so it allows a factor of two"),
					Case.Description, Case.DesignUtilisation, *Bits(Measured)),
				Measured >= 0.5 * Case.DesignUtilisation
					&& Measured <= 2.0 * Case.DesignUtilisation);
		}

		// --- and the outcome ------------------------------------------------------------------

		const int32 BreakingPasses = Laid.Structure.SolveAndBreak();

		int32 Unrouted = 0;

		for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
		{
			if (!Laid.Structure.IsPieceRemoved(Piece) && !Laid.Structure.IsPieceSupported(Piece))
			{
				++Unrouted;
			}
		}

		AddInfo(FString::Printf(
			TEXT("%s: the cascade ran %d passes and left %d of the %d pieces it did not delete ")
			TEXT("with no path to the ground"),
			Case.Description, BreakingPasses, Unrouted,
			Laid.Structure.NumPieces() - VoidPieces.Num()));

		if (Case.Outcome == EVoidOutcome::Unasserted)
		{
			/*
			 * Printed, not asserted (see EVoidOutcome): the only row whose verdict is downstream of a
			 * design decision nobody has made, so it reports what happened and claims nothing.
			 */
			int32 CorbelsLost = 0;

			for (const int32 Piece : AllCorbelPieces)
			{
				if (!Laid.Structure.IsPieceSupported(Piece))
				{
					++CorbelsLost;
				}
			}

			AddInfo(FString::Printf(
				TEXT("%s: OUTCOME NOT ASSERTED — %d of the %d corbelled bricks lost their path to ")
				TEXT("the ground. Which way this row should read follows from the depth bound and ")
				TEXT("is a ruling, not a threshold."),
				Case.Description, CorbelsLost, AllCorbelPieces.Num()));
		}
		else if (Case.Outcome == EVoidOutcome::MustStand)
		{
			/*
			 * The ruling as an outcome: every corbelled brick must still reach the ground. Stated on
			 * the pieces, not a joint — a single severed joint is not a collapse. Local loss is
			 * allowed and not counted: a raking cut leaves survivors with no bed patch at all
			 * (acceptance case 20 names two), which composite action cannot rescue. Those are not
			 * corbels, so not in this list.
			 */
			int32 CorbelsLost = 0;

			for (const int32 Piece : AllCorbelPieces)
			{
				if (!Laid.Structure.IsPieceSupported(Piece))
				{
					++CorbelsLost;
				}
			}

			TestEqual(
				FString::Printf(
					TEXT("%s: THE OVERHANG MUST STAND — all %d corbelled bricks should still ")
					TEXT("reach the ground, %d do not"),
					Case.Description, AllCorbelPieces.Num(), CorbelsLost),
				CorbelsLost, 0);
		}
		else
		{
			/*
			 * And something still comes down: the top of the corbel loses its path to the ground,
			 * and the bottom rung is one of the joints that failed under load rather than one that
			 * went with a removed piece. Per GetBreakPass, a joint that left with its piece has
			 * HasGiven true and a pass of INDEX_NONE, because it never snapped.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: THE CORBEL MUST COME DOWN — the top of it (piece %d) must lose its ")
					TEXT("path to the ground"),
					Case.Description, CorbelPieces[1]),
				CorbelPieces[1] != INDEX_NONE
					&& !Laid.Structure.IsPieceSupported(CorbelPieces[1]));

			TestTrue(
				FString::Printf(
					TEXT("%s: and the bottom rung (joint %d) must be one of the joints that ")
					TEXT("failed UNDER LOAD; it broke in pass %d"),
					Case.Description, BottomRungJoint,
					BottomRungJoint == INDEX_NONE
						? -2
						: Laid.Structure.GetBreakPass(BottomRungJoint)),
				BottomRungJoint != INDEX_NONE
					&& Laid.Structure.GetBreakPass(BottomRungJoint) != INDEX_NONE);
		}
	}

	/*
	 * ===================================================================================
	 * PART 1B — THE SAME CORBEL UNDER MORE WALL. THE PROPERTY, AND THE DEFECT IT NAMES.
	 * ===================================================================================
	 *
	 * The composite depth is measured by walking up the masonry until it runs out, so the section
	 * credited to a corbel's bottom rung is set by the wall's height and the moment bending it by
	 * the cut's. While every fixture had CoursesHigh = Steps + 2 those were one number, k^3 of moment
	 * met k^2 of section, and the crossover near 36 courses was the whole "something must come down"
	 * defence. Decouple them and the reading goes as k^3/m^2, the crossover moves as far up as you
	 * build. The pairs below measure that.
	 *
	 * The property is asserted one-sided because the two-sided form is false for a reason unrelated
	 * to the depth: courses added above a corbel are full-width masonry standing on it, so the hand
	 * ladder is NOT unchanged between the pair (the moment ratios below show it). The honest claim is:
	 *
	 *     ADDING MASONRY ON TOP OF A CORBEL MUST NOT MAKE THE CORBEL SAFER.
	 *
	 * More load, more overturning; whatever section the model credits, the reading may not fall. That
	 * needs no bound to have been chosen, and it is exactly what the defect violates — today the
	 * section grows as the square of a depth the corbel had no part in.
	 *
	 * It refuses the rule the code has now (all the masonry there is) and is satisfied by a rule
	 * crediting the corbel's own courses or less (bond continuity, span-to-depth, a shear budget),
	 * each a fact about the cut, not the wall. It does not rank those — the depth each would credit
	 * is printed beside the measurement. The one candidate it could argue with is a shear-transfer
	 * budget growing with the weight above, which could credit more section in the taller wall; if
	 * that rule is chosen and this row goes red, the row is the argument the rule is wrong — a
	 * cantilever does not get stronger by having a building put on it.
	 */
	{
		int32 PairsCompared = 0;

		for (const FBottomRung& Short : BottomRungs)
		{
			for (const FBottomRung& Tall : BottomRungs)
			{
				if (Short.CorbelSteps != Tall.CorbelSteps
					|| Short.Strength != Tall.Strength
					|| Short.CoursesHigh >= Tall.CoursesHigh)
				{
					continue;
				}

				++PairsCompared;

				/*
				 * What the competing depth rules would say about the taller wall's own bottom rung,
				 * derived from the moment the solver publishes there rather than the shorter wall's
				 * hand ladder, which understates it.
				 */
				const double UnboundedMPa = CompositeTensionMPa(
					Tall.MomentBrickWeightCm, Tall.UnboundedDepthCm, BrickWidthCm);

				const double CorbelOwnDepthCm = Tall.CorbelSteps * CoursePitchCm;

				const double CorbelLimitedMPa = CompositeTensionMPa(
					Tall.MomentBrickWeightCm, CorbelOwnDepthCm, BrickWidthCm);

				const double PatchMPa = PatchTensionMPa(
					Tall.MomentBrickWeightCm, Tall.ForceBrickWeights);

				AddInfo(FString::Printf(
					TEXT("SAME CUT, MORE WALL: %d steps under %d courses against the same %d steps ")
					TEXT("under %d. The bottom rung carries %s brick weights against %s (x%s) and ")
					TEXT("%s brick-weight-cm against %s (x%s) — MORE, because the extra courses ")
					TEXT("stand on it. The walk credits %g cm of depth against %g (x%s), a section ")
					TEXT("x%s larger. It reads %s against %s (x%s)."),
					Tall.CorbelSteps, Tall.CoursesHigh, Short.CorbelSteps, Short.CoursesHigh,
					*Bits(Tall.ForceBrickWeights), *Bits(Short.ForceBrickWeights),
					*Bits(Tall.ForceBrickWeights / Short.ForceBrickWeights),
					*Bits(Tall.MomentBrickWeightCm), *Bits(Short.MomentBrickWeightCm),
					*Bits(Tall.MomentBrickWeightCm / Short.MomentBrickWeightCm),
					Tall.UnboundedDepthCm, Short.UnboundedDepthCm,
					*Bits(Tall.UnboundedDepthCm / Short.UnboundedDepthCm),
					*Bits((Tall.UnboundedDepthCm * Tall.UnboundedDepthCm)
						/ (Short.UnboundedDepthCm * Short.UnboundedDepthCm)),
					*Bits(Tall.Utilisation), *Bits(Short.Utilisation),
					*Bits(Tall.Utilisation / Short.Utilisation)));

				AddInfo(FString::Printf(
					TEXT("SAME CUT, MORE WALL: on the %d x %d wall's own moment the candidate ")
					TEXT("rules read — all the masonry there is (%g cm): %s; the corbel's own %d ")
					TEXT("courses (%g cm): %s; the bed patch alone: %s. The corbel-limited figure ")
					TEXT("is what a span-to-depth or bond-continuity bound would sit at or below, ")
					TEXT("and this fixture cannot tell those two apart."),
					Tall.BricksPerCourse, Tall.CoursesHigh,
					Tall.UnboundedDepthCm, *Bits(UnboundedMPa / MortarTensileMPa),
					Tall.CorbelSteps, CorbelOwnDepthCm,
					*Bits(CorbelLimitedMPa / MortarTensileMPa),
					*Bits(PatchMPa / MortarTensileMPa)));

				/*
				 * How much this pair actually decides, printed so nobody overclaims it. The property
				 * is satisfied by any depth rule crediting no more than
				 *
				 *     D_short * sqrt(M_tall / M_short)
				 *
				 * to the taller wall (section goes as D^2, reading held level). A ceiling, not the
				 * answer: it does not force depth to the corbel's own courses, nor rank the candidate
				 * rules under it. It refuses the walk the code takes now.
				 */
				const double DeepestPermittedCm = Short.UnboundedDepthCm
					* FMath::Sqrt(Tall.MomentBrickWeightCm / Short.MomentBrickWeightCm);

				AddInfo(FString::Printf(
					TEXT("SAME CUT, MORE WALL: this pair permits AT MOST %s cm of section on the ")
					TEXT("taller wall (%s courses) and the walk credits %g (%g courses); the ")
					TEXT("corbel's own depth is %g. Every rule at or under the ceiling passes and ")
					TEXT("this fixture ranks none of them."),
					*Bits(DeepestPermittedCm), *Bits(DeepestPermittedCm / CoursePitchCm),
					Tall.UnboundedDepthCm, Tall.UnboundedDepthCm / CoursePitchCm,
					CorbelOwnDepthCm));

				/*
				 * The premise first: the one-sided claim rests on it. If the taller wall did not load
				 * the corbel harder, "must not read lower" would assert nothing. Strictly greater —
				 * twenty-odd courses on a cantilever is not a rounding difference.
				 */
				TestTrue(
					FString::Printf(
						TEXT("FIXTURE: %d steps under %d courses must bend its bottom rung HARDER ")
						TEXT("than the same cut under %d — %s brick-weight-cm against %s"),
						Tall.CorbelSteps, Tall.CoursesHigh, Short.CoursesHigh,
						*Bits(Tall.MomentBrickWeightCm), *Bits(Short.MomentBrickWeightCm)),
					Tall.MomentBrickWeightCm > Short.MomentBrickWeightCm);

				/*
				 * And the property. No tolerance: the claim is an ordering, not a value, and a slack
				 * one would be satisfied by the defect it catches.
				 */
				TestTrue(
					FString::Printf(
						TEXT("MASONRY ABOVE A CORBEL MUST NOT MAKE IT SAFER — the same %d-step cut ")
						TEXT("under %d courses bends its bottom rung x%s harder than under %d, so ")
						TEXT("that joint may not read LESS; it reads %s against %s, x%s"),
						Tall.CorbelSteps, Tall.CoursesHigh,
						*Bits(Tall.MomentBrickWeightCm / Short.MomentBrickWeightCm),
						Short.CoursesHigh, *Bits(Tall.Utilisation), *Bits(Short.Utilisation),
						*Bits(Tall.Utilisation / Short.Utilisation)),
					Tall.Utilisation >= Short.Utilisation);
			}
		}

		/*
		 * And the table must still hold pairs to compare. Every claim above is conditional on two
		 * rows sharing a step count and mortar, so deleting one of a pair would take the property with
		 * it silently — which is how the depth came to be unbounded and unseen.
		 */
		TestTrue(
			FString::Printf(
				TEXT("FIXTURE: the table must hold at least two cuts each laid in a wall of its ")
				TEXT("own height AND in a taller one, or this property asserts nothing; it made ")
				TEXT("%d comparison(s)"),
				PairsCompared),
			PairsCompared >= 2);
	}

	/*
	 * ===================================================================================
	 * PART 2 — THE FREE END: THE DELETION THE USER ACTUALLY MADE, AND THE RULING ITSELF.
	 * ===================================================================================
	 *
	 * Delete one brick at the end of a wall and trace it. The half bat above loses its only seat and
	 * hangs from one head joint; the full brick beside it keeps one bed patch and overhangs it
	 * 5.625 cm outward, toward the free end — where there is no head joint, because a wall's free
	 * vertical end has no edge in the graph. So slice 1's arch is refused, correctly, and the ladder
	 * walks the failure up the wall at 33.69 degrees.
	 *
	 * Same shape as the jamb reveal, which is why one fixture covers both: in a multi-course opening
	 * the brick one course below the spanning course keeps a patch on the jamb and overhangs into
	 * the opening, no head joint on the eccentric side. CURRENT_STATE records it as the initiator of
	 * acceptance cases 7, 9 and 11; slices 2-4 cannot reach either.
	 *
	 * A half seat under composite action is an identity, not a number: the joint carries its column n
	 * at the 5.625 cm arm, so M = 5.625 n, and with m courses over it the section is 10.25*(7.5m)^2/6:
	 *
	 *     util = 5.625 * n * W_brick / (96.09375 m^2 * 10000 * f_x1)  =  0.0223041 * n / m^2
	 *
	 * at mean f_x1 = 0.70 (0.1561287 * n / m^2 on the retired characteristic 0.10), against today's
	 * unrelieved 0.0083148 * n (was 0.058203838 * n). Left unbounded that falls as 1/m^2 and never
	 * reaches 1.0 for any wall this game builds — which is why the depth must be bounded, or the
	 * mechanism makes every free end and jamb unbreakable in bending. The unbounded row is printed,
	 * nothing derived.
	 *
	 * What it actually reads is the arm's identity (COMPOSITE_DEPTH_DESIGN.md slice 2). The credited
	 * depth is `min(masonry above, max(corbelling body depth, lambda*e))` with e = |M|/|F|, and here
	 * the arm binds: 217.5 cm of masonry stands over the joint and the arm permits 38.92. The
	 * credited depth fell to ~5 courses and the reading went 0.00226883 -> 0.07159148 on the mean
	 * basis (0.0158818 -> 0.5011403 on the retired characteristic).
	 *
	 * That is correct physics, not a weakening. A half seat's arm settles at 11.25 cm however tall
	 * the column (e = (5.625 + 11.25(n-1))/n -> 11.25), so lambda*e settles near 39 cm and the
	 * section stops growing while the load keeps coming. The reading is therefore linear in height,
	 * not falling as 1/m^2 (the discrimination the bound bought survives the re-anchor even as the
	 * margin went from 1.97x to ~14x, logged in CURRENT_STATE). `Core.Structure.AFreeEndDeletionInATallWall`
	 * measures the whole ladder — 0.0469 / 0.0716 / 0.0963 / 0.1210 / 0.1457 at 20/30/40/50/60
	 * courses (each /7 of the characteristic 0.3283 / 0.5011 / 0.6740 / 0.8469 / 1.0198) — and this
	 * wall is its 30-course row.
	 *
	 * Where lambda*e governs, the reading collapses to one identity, asserted in that form rather
	 * than as the literal 0.5011403: an identity survives a change of lambda and a literal does not,
	 * and lambda is provisional until slice 3. Substituting D = lambda*M/F into
	 * sigma = M*W/(t*D^2/6 * 10^4):
	 *
	 *     util  =  K * F^2 / M         K = [6*W_brick/(t * 10^4 * f_xk1)] / lambda^2
	 *                                    = 1.561287 / lambda^2  =  0.130114883 at lambda = 3.464
	 *
	 * F in brick weights, M in brick-weight-cm. It goes as F^2 over M: a deeper arm buys section
	 * faster than it costs moment, which is why the same joint under a taller wall reads higher.
	 *
	 * Three rows, each load-bearing:
	 *   - The arm governs: the credited depth must be lambda*e, strictly between the one course under
	 *     the joint and the 217.5 cm over it. Without this the identity is a claim about a case that
	 *     is not occurring.
	 *   - The identity exactly: util == K*F^2/M on the solver's own force and moment, to 1e-12. It
	 *     pins the section (t*(lambda*e)^2/6, reading (a), no axial relief, lambda = 3.464). It does
	 *     not check the moment; the two rows either side do.
	 *   - The same identity on the hand moment, at 2%, the non-circular one: K*n^2/(5.625 + 11.25(n-1))
	 *     takes only the force from the solver and comes to 0.5063502, COMPOSITE_DEPTH_DESIGN's
	 *     published 0.5064. The 1.04% gap is the solver's moment exceeding the two-arm walk (the half
	 *     bat on its own arm); printed, not absorbed, and 2% is twice it.
	 *
	 * And one across files: this joint must read what `AFreeEndDeletionInATallWall` reads at thirty
	 * courses to the last digit, via the shared literal
	 * `ArchingWallTestSupport::FreeEndThirtyCourseUtilisation` (an agreement anchor, not a derivation).
	 */
	{
		/** 0.1561287: 5.625 cm of arm on one brick weight over a one-course-deep section. */
		const double HalfSeatCoefficient = HalfSeatEccentricityCm * BrickWeightUu
			/ (CompositeModulusCm3(BrickWidthCm, CoursePitchCm) * ForceUnitsPerMPaPerSqCm)
			/ MortarTensileMPa;

		AddInfo(FString::Printf(
			TEXT("FREE END: over the WHOLE wall a half seat would read %s * n / m^2, against ")
			TEXT("%s * n on its own bed patch. ARCHING_DESIGN's published 0.0082 is the ")
			TEXT("n = m = 19 row of that, %s; no fixture presents it and the depth is not ")
			TEXT("unbounded any more, so both are printed and nothing is derived from them."),
			*Bits(HalfSeatCoefficient),
			*Bits(PatchTensionMPa(HalfSeatEccentricityCm, 1.0) / MortarTensileMPa),
			*Bits(HalfSeatCoefficient * 19.0 / (19.0 * 19.0))));

		/**
		 * lambda, spelled out here rather than imported from the solver. 3.464 is 2*sqrt(3), and
		 * COMPOSITE_DEPTH_DESIGN.md is explicit it is a ruling inside a window (floor: this fixture;
		 * ceiling: PART 1B's property), not a derivation. A test that read the solver's constant
		 * would agree with a wrong one, and this file is one of two that decide it.
		 */
		constexpr double CompositeDepthPerArm = 3.464;

		/**
		 * K = [6*W/(t * 10^4 * f_xk1)] / lambda^2, the whole identity in one coefficient. The bracket
		 * is 1.561287 (the section arithmetic); dividing by lambda^2 is the depth rule. Kept as two
		 * factors so lambda and the brick/mortar move it separately and visibly.
		 */
		const double ArmIdentityCoefficient =
			6.0 * BrickWeightUu / (BrickWidthCm * ForceUnitsPerMPaPerSqCm * MortarTensileMPa)
			/ (CompositeDepthPerArm * CompositeDepthPerArm);

		FBrickLayout Laid;

		if (!RunningBond(ArchSupport::ArchWallSpec(), Laid) || Laid.Boxes.Num() != ArchSupport::ArchWallPieceCount)
		{
			AddError(FString::Printf(
				TEXT("FREE END: FIXTURE: a flush 7 x 30 wall should lay as %d pieces, got %d"),
				ArchSupport::ArchWallPieceCount, Laid.Boxes.Num()));

			return true;
		}

		/*
		 * The deletion: the outermost full brick of the grounded course, at x = 0. Course 0 runs
		 * 0, 22.5, 45 …, so x = 0 is the wall's end with nothing outboard but the odd courses' half
		 * bats. The click the user made.
		 */
		const int32 EndBrick = StaircasePieceAt(Laid.Boxes, ArchSupport::ArchWallEvenBrickXCm(0), ArchSupport::ArchWallCourseZCm(0));

		// The brick left half-seated by it: course 1's first full brick, overhanging outward.
		const int32 HalfSeated =
			StaircasePieceAt(Laid.Boxes, ArchSupport::ArchWallOddBrickXCm(0), ArchSupport::ArchWallCourseZCm(1));

		if (EndBrick == INDEX_NONE || HalfSeated == INDEX_NONE
			|| !Laid.Structure.RemovePiece(EndBrick))
		{
			AddError(TEXT("FREE END: FIXTURE: the wall should have an end brick to delete and a ")
				TEXT("full brick above it to leave half seated"));

			return true;
		}

		Laid.Structure.SolveLoads();

		const int32 Seat = ArchSupport::TheOneIntactSeatBeneath(Laid.Structure, HalfSeated);

		TestTrue(
			FString::Printf(
				TEXT("FREE END: FIXTURE: the brick above the deletion must keep EXACTLY ONE seat, ")
				TEXT("it rests on %d"),
				ArchSupport::IntactSeatsBeneath(Laid.Structure, HalfSeated)),
			Seat != INDEX_NONE);

		if (Seat != INDEX_NONE)
		{
			const FConnection& Bed = Laid.Structure.GetConnection(Seat);

			/*
			 * And it overhangs outward, toward the free end — the reason slice 1 refuses to arch it:
			 * the eccentric side is where the wall stops. A fixture overhanging inward would be
			 * measuring the arch instead.
			 */
			const double EccentricityCm =
				Laid.Boxes[HalfSeated].CentreCm.X - Bed.InterfaceCentreCm.X;

			TestTrue(
				FString::Printf(
					TEXT("FREE END: FIXTURE: it must overhang its seat by %g cm TOWARD the free ")
					TEXT("end, it overhangs %s"),
					HalfSeatEccentricityCm, *Bits(EccentricityCm)),
				FMath::IsNearlyEqual(EccentricityCm, -HalfSeatEccentricityCm, 1.0e-9));

			const double ColumnBrickWeights =
				FMath::Abs(Laid.Structure.GetConnectionForce(Seat).Z) / BrickWeightUu;

			/*
			 * The moment is a two-arm walk, not one arm, and measured rather than assumed — the first
			 * version assumed 5.625 * n and was wrong by exactly a factor of two. The brick's own
			 * weight acts at its centre, 5.625 cm from the seat it keeps; everything from above hands
			 * its share down through a patch a further half step inboard, 11.25 cm from that seat. So
			 * the free-end rung is
			 *
			 *     M = 5.625 * 1 + 11.25 * (n - 1)
			 *
			 * the first rung of ARCHING_DESIGN's free-end ladder, the same recursion as the raking
			 * corbel above. The solver publishes 1.04% more (the half bat on its own arm, not modelled
			 * by the two-arm walk), so the claim is at 2% and the residual printed.
			 *
			 * Derived, not read back: building the expectation from GetConnectionMoment is the trap
			 * CURRENT_STATE records — the section this slice changes also changes what that quantity is
			 * worth. Only the force comes from the solver, and a force is not what this slice changes.
			 */
			const double MeasuredMomentBrickWeightCm =
				Laid.Structure.GetConnectionMoment(Seat).Size() / BrickWeightUu;

			const double HandMomentBrickWeightCm = HalfSeatEccentricityCm * 1.0
				+ HalfStepCm * (ColumnBrickWeights - 1.0);

			TestTrue(
				FString::Printf(
					TEXT("FREE END: the moment is the two-arm walk — its own weight at 5.625 cm ")
					TEXT("and the other %s brick weights at 11.25 — so %s brick weights makes %s ")
					TEXT("brick-weight-cm; the joint publishes %s, which is %s%% out (one arm ")
					TEXT("alone would be %s)"),
					*Bits(ColumnBrickWeights - 1.0), *Bits(ColumnBrickWeights),
					*Bits(HandMomentBrickWeightCm), *Bits(MeasuredMomentBrickWeightCm),
					*Bits(100.0 * (MeasuredMomentBrickWeightCm / HandMomentBrickWeightCm - 1.0)),
					*Bits(HalfSeatEccentricityCm * ColumnBrickWeights)),
				FMath::Abs(MeasuredMomentBrickWeightCm - HandMomentBrickWeightCm)
					<= 0.02 * HandMomentBrickWeightCm);

			/*
			 * What the wall offers, what the arm permits, and what the joint was credited: 29 courses
			 * over the joint, the arm permits about five. Both stated so a moved reading says which
			 * term of `min(above, max(body, lambda*e))` moved.
			 */
			const int32 CoursesOfDepth = ArchSupport::ArchWallSpec().CoursesHigh - 1;

			const double MasonryStandingOverItCm = CoursesOfDepth * CoursePitchCm;

			const FVector SeatForceUu = Laid.Structure.GetConnectionForce(Seat);
			const FVector SeatMomentUuCm = Laid.Structure.GetConnectionMoment(Seat);

			const double EffectiveArmCm =
				SeatForceUu.Size() > 0.0 ? SeatMomentUuCm.Size() / SeatForceUu.Size() : 0.0;

			const double PermittedDepthCm = CompositeDepthPerArm * EffectiveArmCm;

			const double CreditedDepthCm = Laid.Structure.GetConnectionCompositeDepthCm(Seat);

			const double Utilisation = Laid.Structure.GetConnectionUtilisation(Seat);

			AddInfo(FString::Printf(
				TEXT("FREE END: the half seat carries %s brick weights and %s brick-weight-cm ")
				TEXT("(the two-arm walk says %s), so e = %s cm and lambda*e = %s. The wall offers ")
				TEXT("%s cm and the joint was credited %s. Unbounded it would have read %s; on ")
				TEXT("its own bed patch, %s. It reads %s."),
				*Bits(ColumnBrickWeights), *Bits(MeasuredMomentBrickWeightCm),
				*Bits(HandMomentBrickWeightCm), *Bits(EffectiveArmCm), *Bits(PermittedDepthCm),
				*Bits(MasonryStandingOverItCm), *Bits(CreditedDepthCm),
				*Bits(CompositeTensionMPa(
					HandMomentBrickWeightCm, MasonryStandingOverItCm, BrickWidthCm)
					/ MortarTensileMPa),
				*Bits(PatchTensionMPa(HandMomentBrickWeightCm, ColumnBrickWeights)
					/ MortarTensileMPa),
				*Bits(Utilisation)));

			/*
			 * Row one: the arm governs, established before the identity is worth anything.
			 * util = K*F^2/M only holds where lambda*e binds; if the wall or the corbelling body bound
			 * instead, the identity would go green while measuring something else. Strictly between the
			 * two, so neither bound equals it by luck.
			 */
			TestTrue(
				FString::Printf(
					TEXT("FREE END: THE ARM MUST GOVERN — e = |M|/|F| = %s cm permits ")
					TEXT("lambda*e = %s cm, and that must be what the joint was credited; it was ")
					TEXT("credited %s"),
					*Bits(EffectiveArmCm), *Bits(PermittedDepthCm), *Bits(CreditedDepthCm)),
				FMath::IsNearlyEqual(CreditedDepthCm, PermittedDepthCm, 1.0e-9));

			TestTrue(
				FString::Printf(
					TEXT("FREE END: and it must be STRICTLY under the %s cm of masonry standing ")
					TEXT("over the joint and STRICTLY over the one course of corbelling body ")
					TEXT("beneath it (%s cm), or one of those is the term that bound it; it is %s"),
					*Bits(MasonryStandingOverItCm), *Bits(CoursePitchCm),
					*Bits(CreditedDepthCm)),
				CreditedDepthCm > CoursePitchCm && CreditedDepthCm < MasonryStandingOverItCm);

			/*
			 * Row two: the identity, exactly. Substituting D = lambda*M/F gives util = K*F^2/M with no
			 * D left, reproducing the reading bit for bit, so 1e-12 relative is slack only for a
			 * different order of operations. It pins the section (t*(lambda*e)^2/6, reading (a), no
			 * axial relief, lambda = 3.464), not the moment (M is on both sides); the two-arm walk and
			 * row three check that, taking only the force from the solver.
			 */
			const double IdentityOnSolverLoads = SeatMomentUuCm.Size() > 0.0
				? ArmIdentityCoefficient * (SeatForceUu.Size() / BrickWeightUu)
					* (SeatForceUu.Size() / BrickWeightUu)
					/ (SeatMomentUuCm.Size() / BrickWeightUu)
				: 0.0;

			TestTrue(
				FString::Printf(
					TEXT("FREE END: WHERE THE ARM GOVERNS THE READING IS K*F^2/M — K = %s ")
					TEXT("(1.561287 / lambda^2 at lambda = %g), F = %s brick weights and ")
					TEXT("M = %s brick-weight-cm, so it must read %s; it reads %s"),
					*Bits(ArmIdentityCoefficient), CompositeDepthPerArm,
					*Bits(SeatForceUu.Size() / BrickWeightUu),
					*Bits(SeatMomentUuCm.Size() / BrickWeightUu),
					*Bits(IdentityOnSolverLoads), *Bits(Utilisation)),
				FMath::Abs(Utilisation - IdentityOnSolverLoads)
					<= 1.0e-12 * FMath::Max(IdentityOnSolverLoads, 1.0e-12));

			/*
			 * Row three: the same identity with the moment derived, not read back — the non-circular
			 * one. Only the force comes from the solver. It comes to 0.5063502 against the reading's
			 * 0.5011403, 1.04% apart (the two-arm walk residual printed above), and 0.5063502 is
			 * COMPOSITE_DEPTH_DESIGN.md's published figure to four digits — used as a cross-check that
			 * fails in the test rather than being tuned away.
			 */
			const double IdentityOnTheHandWalk = ArmIdentityCoefficient
				* ColumnBrickWeights * ColumnBrickWeights / HandMomentBrickWeightCm;

			TestTrue(
				FString::Printf(
					TEXT("FREE END: on the HAND moment the same identity says %s (the design ")
					TEXT("publishes ~0.5064 for this row) and it reads %s, %s%% out — which must ")
					TEXT("be the %s%% the solver's moment exceeds the two-arm walk by, and 2%% is ")
					TEXT("twice that and slack for nothing else"),
					*Bits(IdentityOnTheHandWalk), *Bits(Utilisation),
					*Bits(100.0 * (Utilisation / IdentityOnTheHandWalk - 1.0)),
					*Bits(100.0 * (MeasuredMomentBrickWeightCm / HandMomentBrickWeightCm - 1.0))),
				FMath::Abs(Utilisation - IdentityOnTheHandWalk)
					<= 0.02 * FMath::Max(IdentityOnTheHandWalk, 1.0e-12));

			/*
			 * The cross-file row. `Core.Structure.AFreeEndDeletionInATallWall` cuts the same brick out
			 * of the same 7 x 30 wall and reads the same seat, so the two files are one measurement
			 * made twice. Held to the last digit against the shared literal — two files disagreeing on
			 * one joint is worth catching, the only thing this literal is for.
			 */
			TestTrue(
				FString::Printf(
					TEXT("FREE END: this is the same joint AFreeEndDeletionInATallWall reads at ")
					TEXT("thirty courses, so the two files must agree to the last digit — the ")
					TEXT("shared anchor is %s and this file reads %s"),
					*Bits(ArchSupport::FreeEndThirtyCourseUtilisation), *Bits(Utilisation)),
				Utilisation == ArchSupport::FreeEndThirtyCourseUtilisation);
		}

		/*
		 * The ruling as an outcome: the wall does not come down. Exactly one brick may be lost —
		 * course 1's flush half bat sat entirely on the deleted brick, so it has no bed patch and
		 * composite action cannot help a piece resting on nothing. Counted, not named, so a second
		 * unseated piece anywhere fails here.
		 */
		int32 SeatlessAfterTheCut = 0;

		for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
		{
			if (!Laid.Structure.IsPieceRemoved(Piece)
				&& Laid.Boxes[Piece].CentreCm.Z > ArchSupport::ArchWallCourseZCm(0)
				&& ArchSupport::IntactSeatsBeneath(Laid.Structure, Piece) == 0)
			{
				++SeatlessAfterTheCut;
			}
		}

		const int32 BreakingPasses = Laid.Structure.SolveAndBreak();

		int32 Unrouted = 0;

		for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
		{
			if (!Laid.Structure.IsPieceRemoved(Piece) && !Laid.Structure.IsPieceSupported(Piece))
			{
				++Unrouted;
			}
		}

		AddInfo(FString::Printf(
			TEXT("FREE END: the cut left %d piece(s) with no bed patch at all; the cascade then ")
			TEXT("ran %d passes and left %d of %d pieces with no path to the ground"),
			SeatlessAfterTheCut, BreakingPasses, Unrouted, Laid.Structure.NumPieces() - 1));

		TestTrue(
			FString::Printf(
				TEXT("THE RULING: A BRICK DELETED AT A FREE END MUST NOT BRING THE WALL DOWN — ")
				TEXT("only the %d piece(s) the cut left with no bed patch may be lost, and %d ")
				TEXT("piece(s) of %d were"),
				SeatlessAfterTheCut, Unrouted, Laid.Structure.NumPieces() - 1),
			Unrouted <= SeatlessAfterTheCut);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
