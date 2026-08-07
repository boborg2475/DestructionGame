// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named for what it holds. An anonymous namespace is private
 * to a TRANSLATION UNIT rather than to a file, and a unity build merges many files into one — at
 * which point two anonymous namespaces in the blob are the SAME namespace and identically-named
 * helpers in files that never refer to each other are a hard compile error.
 */
namespace StructureCompositeDepthTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * --- everything below is spelled out here rather than imported ------------------------
	 *
	 * Same discipline as ArchingWallTestSupport and StaircaseWallTestSupport: the brick, the
	 * grid, the SI conversion and the section modulus are written from first principles, and
	 * the strengths are ASSERTED against the profile rather than read out of it. A test that
	 * reaches for production's own constant agrees with a wrong one.
	 */

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double MortarJointCm = 1.0;

	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;
	constexpr double CoursePitchCm = BrickHeightCm + MortarJointCm;
	constexpr double HalfStepCm = BrickPitchCm / 2.0;

	/** 1.9 g/cm3, DENSITY FIRST in the product so it matches Layout::PieceMassKg to the last bit. */
	constexpr double BrickMassKg = 1.9 * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 980 cm/s2. With 1 uu = 1 cm and mass in kg, MassKg * 980 IS a force in Unreal units. */
	constexpr double BrickWeightUu = BrickMassKg * 980.0;

	/**
	 * Unreal force units that load one square centimetre to one megapascal.
	 *
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. DELIBERATELY NOT
	 * DestructionForce::ForceUnitsPerMPaSqCm: this file has to fail if that constant is wrong.
	 */
	constexpr double ForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/** EN 1996-1-1 Table 3.2's f_xk1 for general purpose mortar, asserted against the profile. */
	constexpr double MortarTensileMPa = 0.10;

	/** The half-brick bed patch a running-bond brick keeps when it loses one of its two seats. */
	constexpr double HalfSeatAreaSqCm = BrickWidthCm * BrickWidthCm;

	/** Half of that patch on each in-plane axis; it is square, so the two are equal. */
	constexpr double HalfSeatHalfExtentCm = BrickWidthCm / 2.0;

	/** How far a half-seated brick's own weight acts from the centroid of the patch it keeps. */
	constexpr double HalfSeatEccentricityCm = HalfStepCm / 2.0;

	/**
	 * THE BED PATCH'S OWN SECTION MODULUS: W = (4/3) * h_u * h_v^2 for a rectangle, cm3.
	 *
	 * 179.4817708 cm3 for the square 10.25 x 10.25 patch, which is the number every corbel in
	 * this project is read against today.
	 */
	constexpr double PatchModulusCm3 =
		(4.0 / 3.0) * HalfSeatHalfExtentCm * HalfSeatHalfExtentCm * HalfSeatHalfExtentCm;

	/**
	 * THE COMPOSITE SECTION: W = t * D^2 / 6 FOR A WALL OF THICKNESS t AND VERTICAL DEPTH D.
	 *
	 * ARCHING_DESIGN.md slice 5: a stack of courses over a lost support does not resist as a
	 * sequence of independent bed patches. The wall acts as a DEEP BEAM, and the section
	 * resisting the overturning moment is the full vertical depth of bonded masonry above the
	 * cut. At D = 82.5 cm — the eleven courses standing over the staircase void — that is
	 * 11,627 cm3 against the patch's 179.48, a factor of 64.8, and it is what flips the outcome.
	 *
	 * THE TWO MODELS ARE NESTED, EXACTLY, AND THAT IS WORTH KNOWING BEFORE ANYTHING ELSE. The
	 * patch is square, so (4/3) * 5.125 * 5.125^2 IS 10.25 * 10.25^2 / 6 to the last bit — the
	 * bed patch is simply this formula at D = 10.25 cm, i.e. at about 1.37 courses. So a depth
	 * rule that finds nothing does not need a special case to return today's answer; it returns
	 * it by arithmetic. Asserted below rather than asserted here.
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
	 * ================================================================================
	 * THE RAKING CORBEL, GENERALISED IN HEIGHT — the staircase void with its 13 courses
	 * turned into a parameter.
	 * ================================================================================
	 *
	 * StaircaseWallTestSupport pins ONE wall, 13 courses of 10, and hand-counts the eleven
	 * rungs of the ladder it leaves. Slice 5 is a claim about the DEPTH of masonry acting
	 * compositely, so it needs the same cut at several depths — which means the same geometry
	 * with the height as an argument and the ladder as a closed form.
	 *
	 * THE CLOSED FORMS ARE CHECKED AGAINST THE HAND COUNT, NOT SUBSTITUTED FOR IT. The test
	 * asserts that this recursion reproduces StaircaseWallTestSupport's eleven hand-written
	 * forces and eleven hand-written moments EXACTLY at 13 courses before it uses it anywhere
	 * else. Two independently written derivations agreeing on twenty-two numbers is evidence;
	 * one derivation used twice is not.
	 *
	 * THE COURSE COUNT MUST BE ODD, and it is a fact about running bond rather than a
	 * restriction anybody chose. The void's edge steps half a cell per course, so the corbelled
	 * brick of course c sits at (Courses - 1 - c) * 11.25; that lands on an EVEN course's grid
	 * (multiples of 22.5) exactly when Courses - 1 - c is even, i.e. when Courses is odd. An
	 * even-course wall would name a brick that is not there.
	 */

	/** The staircase edge for a wall of this height: everything left of it, in this course, is cut. */
	constexpr double RakingVoidEdgeXCm(int32 CoursesHigh, int32 Course)
	{
		return (CoursesHigh - 1 - Course) * HalfStepCm;
	}

	/** The lowest and highest courses the void cuts. Course 0 and the top course stay whole. */
	constexpr int32 LowestVoidCourse = 1;

	constexpr int32 HighestVoidCourse(int32 CoursesHigh)
	{
		return CoursesHigh - 2;
	}

	/**
	 * The corbelled courses: 2 up to the topmost course, so there are CoursesHigh - 2 of them.
	 *
	 * Course 1's leftmost survivor still rests on two bricks of the grounded course, so it is
	 * not corbelled; the top course is untouched by the void and is corbelled anyway, because
	 * the course beneath it now starts half a step to its right.
	 */
	constexpr int32 LowestCorbelCourse = 2;

	constexpr int32 HighestCorbelCourse(int32 CoursesHigh)
	{
		return CoursesHigh - 1;
	}

	constexpr int32 CorbelStepCount(int32 CoursesHigh)
	{
		return CoursesHigh - 2;
	}

	/** Centre height of a course, cm: half a brick up, then one course pitch per course. */
	constexpr double CourseZCm(int32 Course)
	{
		return BrickHeightCm / 2.0 + Course * CoursePitchCm;
	}

	/**
	 * WHAT ONE STEP OF THE CORBEL CARRIES, IN BRICK WEIGHTS — the hand ladder as a closed form.
	 *
	 * Each corbelled brick takes its own weight, ALL of the corbelled brick above it (which has
	 * nowhere else to go) and half of the next brick along, whose own share grows the same way.
	 * Reading down from the TOP of the staircase that is
	 *
	 *     F(s) = 1 + s + s(s+1)/4          s steps below the top
	 *
	 * which unrolls to 1, 2.5, 4.5, 7, 10, 13.5, 17.5, 22, 27, 32.5, 38.5 — character for
	 * character StaircaseWallTestSupport's hand count.
	 */
	constexpr double LadderForceBrickWeights(int32 StepsBelowTop)
	{
		return 1.0 + StepsBelowTop + StepsBelowTop * (StepsBelowTop + 1) / 4.0;
	}

	/**
	 * WHAT BENDS THAT STEP, IN BRICK-WEIGHT-CENTIMETRES — the other hand ladder, closed.
	 *
	 * The top step carries only itself, at the 5.625 cm arm a half seat gives it; every step
	 * below adds its own 5.625 to the step above's moment carried across the 11.25 cm the corbel
	 * has stepped out:
	 *
	 *     M(0) = 5.625
	 *     M(s) = 5.625 + 11.25 * F(s-1) + M(s-1)
	 *
	 * which sums to 5.625 (s+1) + 11.25 * SUM(F(0..s-1)), and SUM has the closed form below.
	 * Unrolls to 5.625, 22.5, 56.25, 112.5, 196.875, 315, 472.5, 675, 928.125, 1237.5, 1608.75.
	 */
	constexpr double LadderMomentBrickWeightCm(int32 StepsBelowTop)
	{
		const double S = StepsBelowTop;

		const double SumOfForces =
			S + S * (S - 1.0) / 2.0 + (S - 1.0) * S * (S + 1.0) / 12.0;

		return HalfSeatEccentricityCm * (S + 1.0) + HalfStepCm * SumOfForces;
	}

	/**
	 * HOW MUCH BONDED MASONRY STANDS OVER ONE STEP'S BED JOINT, IN COURSES.
	 *
	 * The joint sits under the brick of course c, so what is above it is courses c through the
	 * top: CoursesHigh - c of them. For the bottom step of the 13-course staircase that is
	 * ELEVEN courses, 82.5 cm — the depth ARCHING_DESIGN's own 11,627 cm3 is worked from.
	 *
	 * THIS IS THE UNBOUNDED READING AND THE TEST SAYS SO. It is all the masonry there is, which
	 * is the most any depth rule may credit. A rule that bounds the depth further — by the shear
	 * the bed joints can transfer, by bond continuity, by span-to-depth — reads LESS, so every
	 * "must stand" row below is an upper bound on the depth a correct rule may find and every
	 * "must come down" row is one-sided and survives any bound at all.
	 */
	constexpr int32 CoursesOverCorbelJoint(int32 CoursesHigh, int32 Course)
	{
		return CoursesHigh - Course;
	}

	/**
	 * WHAT ONE STEP OF THE CORBEL READS TODAY: bending on its own bed patch, less the
	 * compression closing it, against f_xk1.
	 *
	 * The ladder's bottom rung on the 13-course wall is 22.9295 by this, and that is the number
	 * `Core.Structure.AStaircaseVoidCondemnsTheCorbel` pins today.
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
	 * WHAT THE SAME STEP READS ONCE THE WHOLE DEPTH ABOVE IT RESISTS THE MOMENT TOGETHER.
	 *
	 * PURE BENDING ON THE COMPOSITE SECTION, WITH NO AXIAL RELIEF, AND THE CHOICE IS THE WHOLE
	 * MODEL RATHER THAN A DETAIL. Three readings are available and only this one is usable:
	 *
	 *   (a) sigma_b on the composite section alone. The staircase reads M/(W*f_xk1) = 0.36903,
	 *       which is ARCHING_DESIGN's published 0.369 to three digits, and a half seat reads
	 *       0.1561287 * n / m^2 for n brick weights under m courses — whose m = 19 row is
	 *       0.0082173, the design's published free-end figure to two digits. BOTH published
	 *       targets fall out of this reading and out of no other.
	 *
	 *   (b) composite modulus AND composite bearing area. The staircase reads 0.2476, neither
	 *       published figure reproduces, and a half seat goes into pure compression past 4.5
	 *       courses of depth.
	 *
	 *   (c) composite modulus with the joint's own patch area still carrying the axial term —
	 *       the minimal edit, since only Section.SectionModulus*Cm3 would change. It reads
	 *       EXACTLY ZERO for the staircase, and it is disqualified by the third of slice 5's
	 *       three constraints rather than by taste: under (c) sigma_n exceeds sigma_b for every
	 *       fixture in this file, INCLUDING the forty-five-course corbel below, so bending stops
	 *       governing anywhere and nothing can ever be destroyed again. That is the indestructible
	 *       failure the design names by name.
	 *
	 * Physically (a) is also the honest one: the plane resisting a deep-beam moment is VERTICAL,
	 * the wedge's weight is shear on it rather than axial load, and there is no compression on
	 * that plane to subtract.
	 */
	inline double CompositeTensionMPa(
		double MomentBrickWeightCm, double DepthCm, double WallThicknessCm)
	{
		return MomentBrickWeightCm * BrickWeightUu
			/ (CompositeModulusCm3(WallThicknessCm, DepthCm) * ForceUnitsPerMPaPerSqCm);
	}

	/**
	 * WHAT A CORBEL STEP MUST READ AFTER SLICE 5: the LESSER of the two readings, over f_xk1.
	 *
	 * THE `min` IS WHAT MAKES THE MODEL NESTED RATHER THAN A SECOND MODEL BOLTED ON, and it is
	 * what keeps every anchor in place. A joint fails at the lesser of two demands because the
	 * structure finds the stiffer path; composite action is an ALTERNATIVE way of carrying the
	 * moment, not an extra one, so it may only ever help. Three consequences, all checkable:
	 *
	 *   - ONE COURSE OF DEPTH IS NOT A DEEP BEAM. D = 7.5 cm gives W = 96.09 cm3, which is
	 *     SMALLER than the bed patch's own 179.48 — so the composite reading is WORSE there and
	 *     the `min` discards it. That is precisely why `StructureBinding`'s waisted brick and
	 *     `StructurePushTest`'s ragged end keep their 0.058203838 to the last bit: both are the
	 *     TOP course of their wall, with one course of masonry over the joint and nothing else.
	 *
	 *   - AN INTACT WALL IS UNTOUCHED BIT FOR BIT. Every seat has e = 0 exactly, so M = 0, so
	 *     both readings are 0 and the `min` of them is 0.
	 *
	 *   - AND A DEPTH RULE THAT FINDS NOTHING RETURNS TODAY'S ANSWER BY ARITHMETIC rather than
	 *     by a special case, because the patch IS this formula at D = 10.25 cm.
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
			 * DRY STONE HAS NO TENSILE STRENGTH AT ALL — an exact zero, not a small number —
			 * so any tension whatever has already gone. ComputeUtilisation returns
			 * TNumericLimits<double>::Max() for exactly this, and the oracle says so rather
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

	/** Every piece the raking void takes out of a wall of this height, in handle order. */
	inline TArray<int32> RakingVoidPieces(TArrayView<const FPieceBox> Boxes, int32 CoursesHigh)
	{
		TArray<int32> Cut;

		for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
		{
			const int32 Course = CourseOf(Boxes[Piece]);

			if (Course < LowestVoidCourse || Course > HighestVoidCourse(CoursesHigh))
			{
				continue;
			}

			if (Boxes[Piece].CentreCm.X < RakingVoidEdgeXCm(CoursesHigh, Course) - 0.001)
			{
				Cut.Add(Piece);
			}
		}

		return Cut;
	}
}

/**
 * ArchingWallTestSupport IS ALIASED RATHER THAN OPENED, and that is not a style preference.
 *
 * It carries its own BrickLengthCm, HalfSeatAreaSqCm, BrickWeightUu, Bits and a dozen more —
 * deliberately, because every one of these headers re-derives the grid from first principles
 * rather than sharing one copy. Two `using namespace` directives over both would therefore make
 * every shared name AMBIGUOUS at the point of use, and the compiler would say so on twenty lines
 * that have nothing to do with the ambiguity. Only the five names this file actually wants out of
 * it are unique, so they are qualified.
 */
namespace ArchSupport = StructureArchingTestSupport;

/**
 * A CORBEL RESISTS WITH THE WHOLE DEPTH OF MASONRY STANDING OVER IT, AND IT COMES DOWN WHEN THAT
 * DEPTH RUNS OUT.
 *
 * THE RULING THIS SLICE EXISTS FOR, AND IT WAS THE USER'S. A brick deleted at a free end must NOT
 * bring the wall down. Any rule local enough to save the free end also saves the staircase corbel,
 * so that ruling adopts COMPOSITE VERTICAL ACTION and makes
 * `Core.Structure.AStaircaseVoidCondemnsTheCorbel` and
 * `Integration.AStaircaseVoidBringsTheOverhangDown` tests this slice must CHANGE. It is internally
 * consistent rather than a preference: the same user independently agreed acceptance case 20, the
 * staircase void, as LOCAL LOSS — the loose teeth at the cut face drop and the mass stands — which
 * is exactly what composite action produces.
 *
 * THE MECHANISM. A stack of courses over a lost support does not resist as a sequence of
 * independent bed patches. The wall acts as a DEEP BEAM and the section resisting the overturning
 * moment is the full vertical depth of bonded masonry above the cut:
 *
 *     W = t * D^2 / 6                            against the bed patch's 179.4817708 cm3
 *     D = 82.5 cm (11 courses)  ->  11,627 cm3   a factor of 64.8
 *
 * RESISTANCE GROWS WITH THE SQUARE OF THE DEPTH, SO THE DEPTH IS THE MODEL AND NOT A PARAMETER.
 * Take it as unbounded and every wall is a monolith; take it as one course and nothing changes.
 *
 * =========================================================================================
 * WHAT IS ASSERTED, AND WHY EACH FORM WAS CHOSEN
 * =========================================================================================
 *
 *   - THE LOAD PATH IS UNCHANGED, ASSERTED FIRST AND SEPARATELY. Every corbel joint must still
 *     carry the hand ladder's own force and the hand ladder's own moment. Composite action
 *     changes the SECTION the moment is read against, not what the wall hands down — and
 *     ARCHING_DESIGN checked that independently, finding the wedge's centroid arm (41.25 cm) and
 *     the ladder's own average arm (41.79 cm) agree to 1.3%, as statics requires. This is also
 *     what makes the utilisation claim below non-circular: CURRENT_STATE records that any
 *     expectation read back off `GetConnectionMoment` moves WITH a defect in the thing being
 *     tested, and that trap already ate one gate row at slice 1.
 *
 *   - THE UTILISATION IS THE LADDER'S MOMENT OVER THE COMPOSITE SECTION THE FIXTURE PRESENTS,
 *     `min` -ed with today's patch reading. Both terms are built from the hand ladder and from
 *     the fixture's own geometry; the only thing taken from the solver is the force, and the
 *     force is not what this slice changes. 2% throughout, which distinguishes the depth to
 *     better than one course everywhere (one course out of eleven is 18% of the answer).
 *
 *   - AND THE OUTCOME, NEVER A DISPLACEMENT. Two pieces can sever and stay resting exactly where
 *     they were. Rows that must stand assert that every corbelled brick still has a PATH TO THE
 *     GROUND after `SolveAndBreak`; the row that must come down asserts that the top of the
 *     corbel has lost its path and that the bottom rung is among the joints that failed under
 *     load. A single severed joint would not be a collapse.
 *
 * =========================================================================================
 * THE THIRD CONSTRAINT, WHICH IS THE NON-NEGOTIABLE ONE: SOMETHING MUST STILL COME DOWN
 * =========================================================================================
 *
 * A FORTY-FIVE-COURSE RAKING CORBEL FAILS AND IT FAILS BECAUSE THE DEPTH RAN OUT. The overturning
 * moment of a k-course corbel grows as roughly k^3 while the section it is resisted by grows only
 * as k^2, so the utilisation climbs with k and crosses 1.0 at about thirty-six courses:
 *
 *     k       5      11      29      35      37      45
 *     reads   0.219  0.369   0.834   0.990   1.042   1.250
 *
 * That row is ONE-SIDED — it asserts at least 1.25019 — and that is deliberate. Every candidate
 * bound on the composite depth (shear transfer through the bed joints, bond continuity, a
 * span-to-depth ratio) can only credit LESS depth than the masonry actually standing there, and
 * less depth reads HIGHER. So the falling row survives whichever bound slice 5 lands, and the
 * standing rows are the upper bound on how little depth a correct rule may find.
 *
 * ARCHING_DESIGN's OWN DEPTH TABLE — 4 courses 2.79, 6 courses 1.24, 7 courses 0.911, the
 * crossover at 50.1 cm — IS A HYPOTHETICAL AND NOT A FIXTURE FAMILY, and it is worth saying so
 * because it points the opposite way. Every row of it is `0.369 * (82.5/D)^2`: the STAIRCASE's own
 * moment held fixed while the depth is shrunk. No wall can present that pair. In a real k-course
 * corbel the moment shrinks with the depth as well, and the reading is monotonically INCREASING in
 * k — 0.173, 0.219, 0.268, 0.318, 0.369 … — so the crossover is at about thirty-six courses and
 * not under seven, and it runs the other way. The rows below are real cut walls, so they are what
 * this file asserts; the design's table is printed beside them and nothing is derived from it.
 *
 * =========================================================================================
 * THE TWO PUBLISHED TARGETS, AND WHICH OF THEM THIS FILE WILL STAND BEHIND
 * =========================================================================================
 *
 * ARCHING_DESIGN flags both figures as idealisations no test has confirmed. Worked through here:
 *
 *   - THE STAIRCASE'S 0.369 REPRODUCES EXACTLY, as `M/(W*f_xk1)` with M = 1608.75 brick-weight-cm
 *     and W = 11,627.34 cm3: 0.36903. It is asserted, at 2%.
 *
 *   - THE FREE END'S 0.0082 REPRODUCES ONLY AS ONE ROW OF AN IDENTITY, and no fixture in this
 *     project presents that row. A half-seated brick carrying n brick weights under m courses of
 *     depth reads `0.1561287 * n / m^2`, and n = m = 19 gives 0.0082173. The 7 x 30 wall below has
 *     29 courses over its free-end joint and reads about 0.0052 instead. THE IDENTITY IS WHAT IS
 *     ASSERTED; the 0.0082 is printed as the m = 19 row of it and nothing is derived from it. A
 *     loose cross-check against a figure that reproduces from no fixture would be red for a reason
 *     nobody could fix.
 *
 * =========================================================================================
 * ANCHORS THIS FILE IS DELIBERATELY COMPATIBLE WITH
 * =========================================================================================
 *
 * `StructureBinding.AdoptedWallLoadsItsWaistEccentrically` (0.058203838191552663) and
 * `StructurePushTest`'s ragged end (0.0582038382) must not move, and the `min` above is what makes
 * that structural rather than lucky: BOTH are the top course of their wall, so there is exactly one
 * course of masonry over the joint, D = 7.5 cm, W = 96.09 cm3 — SMALLER than the bed patch — and
 * the composite reading is discarded. Any implementation that omits the `min` moves both to
 * 0.08359 and takes those two tests with it.
 *
 * NEEDS A TICKING WORLD: NO. FStructure is arithmetic over a graph and Layout is arithmetic over
 * boxes. Slices 1 through 4 needed none either.
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
	 * THE EXPECTED NUMBERS ARE RATIOS OF PUBLISHED STRENGTHS, so they mean what they say only
	 * while the profile still carries the figures they were derived against. Asserted rather
	 * than imported: a test that read the profile would agree with a wrong profile.
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
	 * THE BED PATCH IS THE COMPOSITE FORMULA AT D = 10.25 cm. Both are a rectangle of the same
	 * material; the patch is square, so (4/3) h^3 with h = 5.125 is 10.25 * 10.25^2 / 6 to the
	 * last bit. That is why a depth rule that finds nothing needs no special case to return
	 * today's answer, and it is the cheapest possible statement that slice 5 is one model with a
	 * parameter rather than two models with a switch.
	 */
	/*
	 * ALGEBRAICALLY, NOT BIT FOR BIT — MEASURED, AND IT IS A NOTE TO WHOEVER IMPLEMENTS THIS.
	 * (4/3)*5.125*5.125^2 is 179.48177083333331 and 10.25*10.25^2/6 is 179.48177083333334: the
	 * same product in a different order, one ulp apart, because IEEE multiplication does not
	 * associate. So an implementation that RECOMPUTED the shallow case through the composite
	 * formula would move `AdoptedWallLoadsItsWaistEccentrically` and `StructurePushTest`'s
	 * ragged end in their last digits for no reason anybody chose. The `min` below is what
	 * avoids that: where the composite section is no better, the joint keeps the patch's own
	 * value verbatim and nothing is recomputed. Same shape as slice 4's `d_e/L` choice.
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
	 * AND THE CLOSED FORMS REPRODUCE THE HAND COUNT, ALL TWENTY-TWO NUMBERS, EXACTLY.
	 *
	 * StaircaseWallTestSupport walks both ladders down the picture by hand for the 13-course
	 * wall. This file needs them at other heights, so it carries the recursions in closed form —
	 * and a closed form that had drifted from the hand walk would make every row below agree
	 * with itself and with nothing else.
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
	 * PART 1 — THE RAKING CORBEL AT THREE DEPTHS, PLUS THE SAME CUT LAID DRY.
	 * ===================================================================================
	 */

	/** One raking void, and everything about it this file has an opinion on. */
	struct FCorbelCase
	{
		const TCHAR* Description;

		/** ODD, always: see RakingVoidEdgeXCm. The corbel is CoursesHigh - 2 steps tall. */
		int32 CoursesHigh;

		/**
		 * Wide enough that the load cone above the BOTTOM step still lies inside the wall.
		 * The cone reaches about (2k - 2) half steps from the origin, so a k-step corbel wants
		 * roughly k + 1 bricks per course. StaircaseWallTestSupport records what happens when
		 * it does not: the 13 x 10 wall is 11.25 cm short and its bottom rung comes back
		 * 2.4e-6 low, which is inside the tolerance here by three orders of magnitude.
		 */
		int32 BricksPerCourse;

		const FConnectionStrength* Strength;

		/** Whether the mass over the void must still reach the ground once the cascade has run. */
		bool bMustStand;

		/** ARCHING_DESIGN's own figure for the bottom rung, or 0 where it published none. */
		double DesignUtilisation;
	};

	const TArray<FCorbelCase> Cases = {
		/*
		 * FIVE STEPS, the smallest raking corbel that is unambiguously one — three steps is
		 * two courses of overhang and reads 0.17 either way. Today its bottom rung is 2.67180
		 * and four of its five rungs are over capacity; under a 37.5 cm section it is 0.21858
		 * and none are.
		 */
		{ TEXT("a FIVE-step raking corbel"), 7, 7, &GeneralPurposeMortar, true, 0.0 },

		/*
		 * ELEVEN STEPS — THE PHOTOGRAPHED FAILURE, and the identical fixture
		 * `AStaircaseVoidCondemnsTheCorbel` and `AStaircaseVoidBringsTheOverhangDown` cut. Its
		 * bottom rung is 22.929528199727653 today with 8 of 11 over capacity; under the 82.5 cm
		 * of masonry actually standing over it, 0.36903 with none. Those two tests INVERT with
		 * this row, by the user's ruling, and this is where the new number is derived.
		 *
		 * TEN BRICKS PER COURSE RATHER THAN TWELVE, deliberately: it must be the same wall the
		 * two changing tests lay, cone deficit and all, or its number is about a different wall.
		 */
		{ TEXT("ELEVEN steps: THE STAIRCASE VOID"), 13, 10, &GeneralPurposeMortar, true, 0.369 },

		/*
		 * FORTY-FIVE STEPS — THE DEPTH RUNS OUT, and this is the row the whole slice is gated
		 * on. The moment climbs as k^3 and the section only as k^2, so 337.5 cm of masonry is
		 * no longer enough for the 91,209 brick-weight-cm hanging off it: 1.25019, over
		 * capacity, and the corbel comes down. Today it reads 1341.7, so the ROW is red on the
		 * number and green on the outcome — which is the point. Slice 5 must not take the
		 * outcome away.
		 *
		 * ASSERTED ONE-SIDED, AT LEAST 1.25019. Any bound on the composite depth credits less
		 * than the masonry standing there and therefore reads higher, so this row holds
		 * whichever of the three candidate bounds slice 5 picks.
		 */
		{ TEXT("FORTY-FIVE steps: the depth runs out"), 47, 47, &GeneralPurposeMortar, false, 0.0 },

		/*
		 * AND THE SAME STAIRCASE LAID DRY. You cannot corbel a dry-stone wall — there is no
		 * bond to transfer the horizontal shear composite action demands between courses — so
		 * whatever bounds the depth must leave this one condemned.
		 *
		 * GREEN ON ARRIVAL AND SAID SO. DryStone's f_xk1 is an EXACT zero, so any tension
		 * anywhere on the face has already gone, at any section modulus; this row cannot tell a
		 * bounded depth from an unbounded one. What it CAN catch is an implementation that
		 * subtracts an axial term on the composite bearing area — reading (b) or (c) above —
		 * because past 4.5 courses of depth that puts a corbel into pure compression and stands
		 * a dry-stone overhang up. It is a guard, not a driver, and it earns its place only
		 * because it is the same fixture with one field changed.
		 */
		{ TEXT("ELEVEN steps, laid DRY"), 13, 10, &DryStone, false, 0.0 },
	};

	for (const FCorbelCase& Case : Cases)
	{
		const int32 Steps = CorbelStepCount(Case.CoursesHigh);

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the course count must be ODD or the raking edge names bricks ")
				TEXT("that are not there; it is %d"),
				Case.Description, Case.CoursesHigh),
			Case.CoursesHigh % 2 == 1);

		AddInfo(FString::Printf(
			TEXT("%s: %d x %d flush wall, raking void through courses %d..%d, %d corbelled steps, ")
			TEXT("%g cm of masonry over the bottom one"),
			Case.Description, Case.BricksPerCourse, Case.CoursesHigh, LowestVoidCourse,
			HighestVoidCourse(Case.CoursesHigh), Steps,
			CoursesOverCorbelJoint(Case.CoursesHigh, LowestCorbelCourse) * CoursePitchCm));

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
		 * THE POSITIVE CONTROL. A wall that arrived with a joint past capacity would be
		 * condemned for a reason the void had nothing to do with — and the flush end is what
		 * buys it, because a ragged end brick is already a half-seated cantilever before
		 * anybody has cut anything.
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

		const TArray<int32> VoidPieces = RakingVoidPieces(Laid.Boxes, Case.CoursesHigh);

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
			Course <= HighestCorbelCourse(Case.CoursesHigh);
			++Course)
		{
			const int32 StepsBelowTop = HighestCorbelCourse(Case.CoursesHigh) - Course;

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
				RakingVoidEdgeXCm(Case.CoursesHigh, Course),
				CourseZCm(Course));

			const int32 Support = StaircasePieceAt(
				Laid.Boxes,
				RakingVoidEdgeXCm(Case.CoursesHigh, Course) + HalfStepCm,
				CourseZCm(Course - 1));

			const int32 Joint = Corbel != INDEX_NONE && Support != INDEX_NONE
				? JointBetweenPieces(Laid.Structure, Corbel, Support)
				: INDEX_NONE;

			if (Joint == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: course %d should have a brick at x %g jointed to one at ")
					TEXT("x %g; they are pieces %d and %d"),
					Case.Description, Course, RakingVoidEdgeXCm(Case.CoursesHigh, Course),
					RakingVoidEdgeXCm(Case.CoursesHigh, Course) + HalfStepCm, Corbel, Support));

				bLadderRead = false;
				break;
			}

			AllCorbelPieces.Add(Corbel);

			if (Course == LowestCorbelCourse)
			{
				CorbelPieces[0] = Corbel;
				BottomRungJoint = Joint;
			}

			if (Course == HighestCorbelCourse(Case.CoursesHigh))
			{
				CorbelPieces[1] = Corbel;
			}

			/*
			 * IT MUST BE A HALF SEAT AND NOTHING ELSE, arbitrated against the producer. Every
			 * number in this file is worked from a 10.25 x 10.25 patch loaded 5.625 cm off its
			 * own centroid, and a fixture that had quietly stopped presenting one would agree
			 * with none of them for a reason nothing else would say out loud.
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
				|| Course == HighestCorbelCourse(Case.CoursesHigh);

			if (bWorthPrinting)
			{
				AddInfo(FString::Printf(
					TEXT("%s, course %2d: joint %d carries %s brick weights (hand ladder %s) and ")
					TEXT("%s brick-weight-cm (hand ladder %s) over %d courses = %g cm of depth. ")
					TEXT("It reads %s; the patch alone says %s, the composite section says %s, ")
					TEXT("and the lesser of the two is %s"),
					Case.Description, Course, Joint, *Bits(MeasuredForceBrickWeights),
					*Bits(HandForceBrickWeights), *Bits(MeasuredMomentBrickWeightCm),
					*Bits(HandMomentBrickWeightCm), CoursesOfDepth,
					CoursesOfDepth * CoursePitchCm, *Bits(Utilisation),
					*Bits(PatchTensionMPa(HandMomentBrickWeightCm, HandForceBrickWeights)
						/ MortarTensileMPa),
					*Bits(CompositeTensionMPa(
						HandMomentBrickWeightCm, CoursesOfDepth * CoursePitchCm, BrickWidthCm)
						/ MortarTensileMPa),
					*Bits(Expected)));
			}

			/*
			 * THE LOAD PATH FIRST, AND SEPARATELY FROM THE SECTION. Composite action is a claim
			 * about what resists the moment, not about what the wall hands down, so the force
			 * and the moment must both still be the hand ladder's. 2% rather than the 1e-5 the
			 * staircase test uses, because ARCHING_DESIGN's own cross-check — the wedge's
			 * centroid arm against the ladder's average arm — only agrees to 1.3%, so an
			 * implementation that computed the wedge as one composite body rather than as a
			 * ladder is inside this and is not what this row is about.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s, course %d: the load path is UNCHANGED — the joint must still carry ")
					TEXT("the hand ladder's %s brick weights, it carries %s"),
					Case.Description, Course, *Bits(HandForceBrickWeights),
					*Bits(MeasuredForceBrickWeights)),
				FMath::Abs(MeasuredForceBrickWeights - HandForceBrickWeights)
					<= 0.02 * HandForceBrickWeights);

			TestTrue(
				FString::Printf(
					TEXT("%s, course %d: and the hand ladder's %s brick-weight-cm of moment, it ")
					TEXT("carries %s"),
					Case.Description, Course, *Bits(HandMomentBrickWeightCm),
					*Bits(MeasuredMomentBrickWeightCm)),
				FMath::Abs(MeasuredMomentBrickWeightCm - HandMomentBrickWeightCm)
					<= 0.02 * HandMomentBrickWeightCm);

			/*
			 * AND THEN THE SECTION, WHICH IS THE SLICE. `min(patch reading, M / (t D^2 / 6))`,
			 * both built from the hand ladder and from the depth the fixture presents.
			 *
			 * A DRY ROW IS ASSERTED AS AN ORDERING RATHER THAN AS A NUMBER, because f_xk1 = 0
			 * makes the answer TNumericLimits<double>::Max() and a relative tolerance on that
			 * says nothing.
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
			else
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
		}

		if (!bLadderRead)
		{
			continue;
		}

		AddInfo(FString::Printf(
			TEXT("%s: %d of %d rungs read over capacity; the composite section predicts %d"),
			Case.Description, RungsOverCapacity, Steps, PredictedRungsOverCapacity));

		TestEqual(
			FString::Printf(
				TEXT("%s: exactly the rungs the composite section condemns must be over capacity"),
				Case.Description),
			RungsOverCapacity, PredictedRungsOverCapacity);

		/*
		 * AND THE DESIGN'S OWN FIGURE, WHERE IT PUBLISHED ONE — a factor of two either way,
		 * which is an order-of-magnitude cross-check and nothing more. It still catches a
		 * missing 100x, a section modulus off by the 64.8 this slice turns on, or a moment
		 * deleted rather than re-sectioned.
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

		if (Case.bMustStand)
		{
			/*
			 * THE RULING, AS AN OUTCOME. Every corbelled brick must still reach the ground. That
			 * is the whole of what the user decided and it is stated on the pieces rather than
			 * on a joint: a single severed joint is not a collapse, and a wall can shed one and
			 * stand.
			 *
			 * LOCAL LOSS IS ALLOWED AND IS NOT COUNTED HERE. A raking cut leaves survivors with
			 * no bed patch AT ALL — acceptance case 20 names two of them — and composite action
			 * cannot rescue a brick that is not sitting on anything. Those are not corbels, so
			 * they are not in this list.
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
			 * AND SOMETHING STILL COMES DOWN. Stated as the TOP of the corbel — the mass hanging
			 * furthest out over nothing — losing its path to the ground, plus the bottom rung
			 * being one of the joints that FAILED UNDER LOAD rather than one that went with a
			 * removed piece. GetBreakPass's contract spells that encoding out: a joint that left
			 * with its piece has HasGiven true and a pass of INDEX_NONE, because it never
			 * snapped.
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
	 * PART 2 — THE FREE END: THE DELETION THE USER ACTUALLY MADE, AND THE RULING ITSELF.
	 * ===================================================================================
	 *
	 * Delete one brick at the END of a wall and trace it. The half bat above loses its only seat
	 * and hangs from one head joint; the full brick beside it keeps ONE bed patch and overhangs
	 * it 5.625 cm OUTWARD, toward the free end — where there is no head joint at all, because a
	 * wall's free vertical end simply has no edge in the graph. So slice 1's arch is refused,
	 * correctly, and today the ladder starts and walks the failure up the wall in a stepping
	 * triangle at 33.69 degrees.
	 *
	 * IT IS THE SAME SHAPE AS THE JAMB REVEAL, which is why one fixture covers both. In a
	 * multi-course opening the brick one course below the spanning course keeps a patch on the
	 * jamb and overhangs into the opening where its neighbour was cut away — no head joint on the
	 * eccentric side either — and CURRENT_STATE records it as what initiates the collapse in
	 * acceptance cases 7, 9 and 11. Slices 2, 3 and 4 cannot reach either of them.
	 *
	 * WHAT A HALF SEAT READS UNDER COMPOSITE ACTION, AND IT IS AN IDENTITY RATHER THAN A NUMBER.
	 * The joint carries its whole column n at the same 5.625 cm arm, so M = 5.625 n, and with m
	 * courses of masonry over it the section is 10.25 * (7.5m)^2 / 6:
	 *
	 *     util = 5.625 * n * W_brick / (96.09375 m^2 * 10000 * f_xk1)  =  0.1561287 * n / m^2
	 *
	 * against today's 0.058203838 * n. The composite reading is the lesser from m = 2 upward,
	 * falls as 1/m^2, and never reaches 1.0 for any wall this game builds — WHICH IS THE RULING,
	 * and also exactly why the depth has to be bounded by something, since on its own this
	 * mechanism makes every free end and every jamb unbreakable in bending.
	 */
	{
		/** 0.1561287: 5.625 cm of arm on one brick weight over a one-course-deep section. */
		const double HalfSeatCoefficient = HalfSeatEccentricityCm * BrickWeightUu
			/ (CompositeModulusCm3(BrickWidthCm, CoursePitchCm) * ForceUnitsPerMPaPerSqCm)
			/ MortarTensileMPa;

		AddInfo(FString::Printf(
			TEXT("FREE END: a half seat reads %s * n / m^2 under composite action against ")
			TEXT("%s * n today. ARCHING_DESIGN's published 0.0082 is the n = m = 19 row of it, ")
			TEXT("%s; no fixture in this project presents that row, so it is printed and not ")
			TEXT("asserted."),
			*Bits(HalfSeatCoefficient),
			*Bits(PatchTensionMPa(HalfSeatEccentricityCm, 1.0) / MortarTensileMPa),
			*Bits(HalfSeatCoefficient * 19.0 / (19.0 * 19.0))));

		FBrickLayout Laid;

		if (!RunningBond(ArchSupport::ArchWallSpec(), Laid) || Laid.Boxes.Num() != ArchSupport::ArchWallPieceCount)
		{
			AddError(FString::Printf(
				TEXT("FREE END: FIXTURE: a flush 7 x 30 wall should lay as %d pieces, got %d"),
				ArchSupport::ArchWallPieceCount, Laid.Boxes.Num()));

			return true;
		}

		/*
		 * THE DELETION: the OUTERMOST full brick of the grounded course, at x = 0.
		 *
		 * Course 0 runs 0, 22.5, 45 … so x = 0 is the end of the wall and there is nothing
		 * outboard of it but the odd courses' half bats. That is the click the user made.
		 */
		const int32 EndBrick = StaircasePieceAt(Laid.Boxes, ArchSupport::ArchWallEvenBrickXCm(0), ArchSupport::ArchWallCourseZCm(0));

		/* The brick left half-seated by it: course 1's first full brick, overhanging outward. */
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
			 * AND IT OVERHANGS OUTWARD, TOWARD THE FREE END, which is the whole reason slice 1
			 * refuses to arch it: the eccentric side is where the wall stops. A fixture that had
			 * drifted into overhanging INWARD would be measuring the arch instead.
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
			 * THE MOMENT IS A TWO-ARM WALK, NOT ONE ARM, AND THAT IS MEASURED RATHER THAN
			 * ASSUMED — the first version of this row assumed `5.625 * n` and was wrong by
			 * exactly a factor of two, which is what a fixture is for.
			 *
			 * The brick's OWN weight acts at its centre, 5.625 cm from the centroid of the seat
			 * it keeps. EVERYTHING ARRIVING FROM ABOVE does not: the brick over it rests on this
			 * one and on the half bat outboard, and the patch it hands its share down through
			 * sits a further half step INBOARD — 11.25 cm from the seat this brick leaves by.
			 * So the free-end rung is
			 *
			 *     M = 5.625 * 1 + 11.25 * (n - 1)
			 *
			 * which is the first rung of exactly the ladder ARCHING_DESIGN says the free end
			 * walks, and it is the same recursion the raking corbel above is built from. The
			 * solver publishes 1.04% more than that on this fixture — the half bat hanging off
			 * the head joint contributes its share on an arm of its own, which the two-arm walk
			 * does not model — so the claim is made at 2% and the residual is printed.
			 *
			 * DERIVED, NOT READ BACK. Building the expectation out of GetConnectionMoment is the
			 * trap CURRENT_STATE records: the section this slice changes also changes what that
			 * quantity is worth, so an oracle standing on it agrees with whatever landed. Only
			 * the FORCE comes from the solver, and a force is not what this slice changes.
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

			const int32 CoursesOfDepth = ArchSupport::ArchWallSpec().CoursesHigh - 1;

			const double Expected = CorbelUtilisation(
				HandMomentBrickWeightCm, ColumnBrickWeights, CoursesOfDepth,
				BrickWidthCm, MortarTensileMPa);

			const double Utilisation = Laid.Structure.GetConnectionUtilisation(Seat);

			AddInfo(FString::Printf(
				TEXT("FREE END: the half seat carries %s brick weights and %s brick-weight-cm ")
				TEXT("under %d courses of masonry; the patch alone says %s, the composite ")
				TEXT("section says %s, and it reads %s"),
				*Bits(ColumnBrickWeights), *Bits(HandMomentBrickWeightCm), CoursesOfDepth,
				*Bits(PatchTensionMPa(HandMomentBrickWeightCm, ColumnBrickWeights)
					/ MortarTensileMPa),
				*Bits(CompositeTensionMPa(
					HandMomentBrickWeightCm, CoursesOfDepth * CoursePitchCm, BrickWidthCm)
					/ MortarTensileMPa),
				*Bits(Utilisation)));

			/*
			 * FIVE PER CENT HERE RATHER THAN TWO, AND THE EXTRA THREE ARE THE RESIDUAL ABOVE.
			 * The expectation is built from the two-arm walk, which the solver's own moment
			 * exceeds by 1.04% on this fixture, so a correct implementation reading its own
			 * moment against the composite section lands 1% off this number by construction.
			 * Five per cent is slack for that and for nothing else: the claim being made is a
			 * factor of 380.
			 */
			TestTrue(
				FString::Printf(
					TEXT("FREE END: %d courses of bonded masonry stand over this joint, so its ")
					TEXT("section is t*D^2/6 = %s cm3 rather than the patch's %s, and it must ")
					TEXT("read %s; it reads %s"),
					CoursesOfDepth,
					*Bits(CompositeModulusCm3(BrickWidthCm, CoursesOfDepth * CoursePitchCm)),
					*Bits(PatchModulusCm3), *Bits(Expected), *Bits(Utilisation)),
				FMath::Abs(Utilisation - Expected) <= 0.05 * FMath::Max(Expected, 1.0e-12));
		}

		/*
		 * AND THE RULING, AS AN OUTCOME: THE WALL DOES NOT COME DOWN.
		 *
		 * One brick is deliberately allowed to be lost, and exactly one is available to lose:
		 * course 1's flush half bat sat entirely on the deleted brick, so it has no bed patch at
		 * all, and composite action has nothing to offer a piece that is not resting on
		 * anything. Counted rather than named, so a second unseated piece appearing anywhere
		 * fails here.
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
