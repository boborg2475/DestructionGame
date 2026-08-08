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
	 * turned into a parameter, AND THE CUT'S HEIGHT SEPARATED FROM THE WALL'S.
	 * ================================================================================
	 *
	 * StaircaseWallTestSupport pins ONE wall, 13 courses of 10, and hand-counts the eleven
	 * rungs of the ladder it leaves. Slice 5 is a claim about the DEPTH of masonry acting
	 * compositely, so it needs the same cut at several depths — which means the same geometry
	 * with the height as an argument and the ladder as a closed form.
	 *
	 * THE CUT IS SIZED BY `CorbelSteps` AND THE WALL BY `CoursesHigh`, AND UNTIL 2026-08-07
	 * THEY WERE THE SAME NUMBER. Every row of this file's table was `CoursesHigh = Steps + 2`,
	 * so no fixture in the project had a corbel SHORTER than its wall — and the composite
	 * depth is measured by walking up the masonry, which means the wall's own height was
	 * feeding the answer with nothing able to see it. StaircaseWallTestSupport already made
	 * this separation for the void ("the course range and the edge are ABSOLUTE, not relative
	 * to the wall's height, which is what lets the SAME void be cut into a taller wall"); this
	 * is the same statement for the generalised cut, and the four original rows are recovered
	 * bit for bit by passing `CorbelSteps = CoursesHigh - 2`.
	 *
	 * THE STEP COUNT MUST BE ODD, and it is a fact about running bond rather than a
	 * restriction anybody chose. The void's edge steps half a cell per course, so the corbelled
	 * brick of course c sits at (CorbelSteps + 1 - c) * 11.25; that lands on an EVEN course's
	 * grid (multiples of 22.5) exactly when CorbelSteps + 1 - c is even, i.e. when CorbelSteps
	 * is odd. A cut of even height would name a brick that is not there. THE WALL'S OWN HEIGHT
	 * IS NOW FREE — 40 courses over an 11-step cut is the game's own scenario wall, and it was
	 * unreachable while the two were one number.
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
	 * The corbelled courses: 2 up to one course above the top of the cut, so there are
	 * CorbelSteps of them.
	 *
	 * Course 1's leftmost survivor still rests on two bricks of the grounded course, so it is
	 * not corbelled; the course above the cut is untouched by the void and is corbelled anyway,
	 * because the course beneath it now starts half a step to its right. In a wall TALLER than
	 * the cut, everything above that course is ordinary full-width masonry standing on it —
	 * which is exactly the masonry whose contribution this file exists to measure.
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
	 *
	 * AND IT IS ONLY ASSERTED WHERE THE WALL STOPS AT THE TOP OF THE CORBEL. In a wall taller
	 * than its cut this expression is a statement about masonry that is not part of the corbel
	 * at all, which is the thing PART 1B is about; there it is printed as the reading the
	 * unbounded rule gives and nothing is derived from it.
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
 * AND THE k^3/k^2 ARGUMENT ABOVE IS TRUE ONLY OF A WALL THE SAME HEIGHT AS ITS CUT, WHICH EVERY
 * ROW OF THAT TABLE WAS AND NO ROW SAID. *(Found in review 2026-08-07; it is finding B1 and PART
 * 1B below is the fixture for it.)* `k^3/k^2` assumes the depth credited to the bottom rung comes
 * from the corbel's OWN steps. It does not: the depth is whatever the upward walk finds, so in a
 * wall taller than the cut the moment is set by `k` and the section by the wall's `m`, the
 * reading goes as `k^3/m^2`, the two exponents stop cancelling and the thirty-six-course
 * crossover does not exist. The wall the game renders is exactly that case — eleven corbelled
 * steps under forty courses — so the joint the suite pins at 0.36903147272727271 on a
 * thirteen-course wall is a different number entirely on the wall the player is looking at.
 * PART 1B lays both and compares them.
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
	 * PART 1 — THE RAKING CORBEL AT THREE DEPTHS, THE SAME CUT LAID DRY, AND TWO CUTS
	 * LAID UNDER MORE WALL THAN THEY NEED.
	 * ===================================================================================
	 */

	/**
	 * WHAT THE CASCADE MUST DO TO THE MASS OVER THE VOID — and one row deliberately declines
	 * to say.
	 *
	 * `Unasserted` is not a gap. Whether an eleven-step corbel under a forty-course wall stands
	 * is a consequence of WHICH bound on the composite depth gets chosen, and that is an open
	 * design question this file must not close by asserting an outcome: crediting the corbel's
	 * own eleven courses makes it a different verdict from crediting the wall's thirty-nine.
	 * The row exists to make the two DISTINGUISHABLE and to pin the property in PART 1B; the
	 * verdict is printed with its arithmetic and left to whoever rules.
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

		/**
		 * ODD, always: see RakingVoidEdgeXCm. This is the number of corbelled steps and it
		 * sizes the CUT — the ladder, the arms, the whole overturning moment.
		 */
		int32 CorbelSteps;

		/**
		 * And this sizes the WALL, which needs at least CorbelSteps + 2 courses to hold the
		 * cut and may have any number more. Where it has more, the extra courses are ordinary
		 * full-width masonry standing ON the corbel: they add load to every rung and they add
		 * depth to the walk that measures the composite section, and separating those two is
		 * the whole of PART 1B.
		 */
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

		/** What must become of the mass over the void once the cascade has run. */
		EVoidOutcome Outcome;

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
		{ TEXT("a FIVE-step raking corbel"), 5, 7, 7, &GeneralPurposeMortar,
			EVoidOutcome::MustStand, 0.0 },

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
		{ TEXT("ELEVEN steps: THE STAIRCASE VOID"), 11, 13, 10, &GeneralPurposeMortar,
			EVoidOutcome::MustStand, 0.369 },

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
		{ TEXT("FORTY-FIVE steps: the depth runs out"), 45, 47, 47, &GeneralPurposeMortar,
			EVoidOutcome::MustComeDown, 0.0 },

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
		{ TEXT("ELEVEN steps, laid DRY"), 11, 13, 10, &DryStone,
			EVoidOutcome::MustComeDown, 0.0 },

		/*
		 * ===============================================================================
		 * AND THE SAME TWO CUTS AGAIN UNDER MORE WALL. These two rows are the fixture the
		 * project did not have, and without them nothing could see the composite depth.
		 * ===============================================================================
		 *
		 * FORTY-FIVE STEPS UNDER SIXTY-SEVEN COURSES. Identical cut to the row above — same
		 * ladder, same arms, same 91,209 brick-weight-cm of overturning from the corbel's own
		 * mass — with twenty further courses of ordinary full-width masonry standing on top of
		 * it.
		 *
		 * GREEN ON ARRIVAL, AND IT IS THE MEASUREMENT THAT SAYS SO RATHER THAN A JUDGEMENT.
		 * The review proposed this row as red, on the grounds that a wall 1.5x taller is
		 * "arithmetically identical" to crediting 1.5x the composite depth — a mutation which
		 * does stand this corbel up. IT IS NOT IDENTICAL, and the difference is the whole
		 * finding: the extra courses are masonry STANDING ON the corbel, so they add load as
		 * well as depth. Measured here, 47 courses to 67 multiplies the bottom rung's moment by
		 * 2.2754 and its section by 2.0864, so the reading goes UP — 1.2501861088888881 to
		 * 1.3634027672024234 — and the corbel still comes down. The mutation moved one of those
		 * two and a taller wall moves both.
		 *
		 * WHICH IS WHY IT IS THE SHORT CUT UNDER THE TALL WALL THAT BITES, and the row below is
		 * the one that does. A corbel's own moment grows as k^3 and the wall's section as m^2,
		 * so what decides is the RATIO m/k: at 67/45 the added load very nearly keeps up, and
		 * at 40/11 it is beaten by a factor of two. Extrapolating this row's own numbers, a
		 * 45-step corbel needs something like a hundred and sixty courses — twelve metres of
		 * wall — before the depth rescues it, and that is not a fixture anybody should build.
		 *
		 * KEPT AS A GUARD RATHER THAN DROPPED, AND IT BITES. The 47-course row is the design's
		 * "something must still come down" constraint and this row is that same corbel with
		 * twenty courses of extra load piled on it and nothing whatever removed; a depth rule
		 * that rescued it would be rescuing a cantilever by putting a building on it. Under the
		 * review's own 1.5x-depth mutation this row reads 0.606 and stands, so it fails there
		 * and the row is not asserting nothing.
		 */
		{ TEXT("FORTY-FIVE steps under SIXTY-SEVEN courses"), 45, 67, 47, &GeneralPurposeMortar,
			EVoidOutcome::MustComeDown, 0.0 },

		/*
		 * ELEVEN STEPS UNDER FORTY COURSES — THE WALL THE GAME ACTUALLY RENDERS, and the
		 * headless twin the screenshot fixture never had.
		 *
		 * `ADestructionGameGameMode` lays 30 bricks by 40 courses and the visual harness cuts
		 * StaircaseWallTestSupport's void into it — courses 1 to 11, edge (12 - c) * 11.25 —
		 * which is character for character this row's cut. So the joint the suite pins at
		 * 0.36903147272727271 on a 13-course wall is the SAME joint the player is looking at,
		 * standing under 39 courses instead of 11. Nothing headless covered that wall, and
		 * TEST_CHANGES.md section 4 names the gap independently: `Visual.StaircaseScreenshot`
		 * carries NonNullRHI, so it is invisible to the documented test command, and it rotted
		 * for five slices behind that flag.
		 *
		 * OUTCOME DELIBERATELY UNASSERTED — see EVoidOutcome. Whether this corbel stands is
		 * downstream of which bound gets chosen, and the arithmetic runs close: crediting the
		 * corbel's own eleven courses against a ladder the wall above has made several times
		 * heavier is a different verdict from crediting all thirty-nine. What IS asserted is
		 * the property in PART 1B, which is true under every candidate.
		 */
		{ TEXT("ELEVEN steps under FORTY courses: THE SCENARIO WALL"), 11, 40, 30,
			&GeneralPurposeMortar, EVoidOutcome::Unasserted, 0.0 },
	};

	/**
	 * THE BOTTOM RUNG OF EVERY CASE, KEPT SO THAT TWO CASES CAN BE COMPARED WITH EACH OTHER.
	 *
	 * PART 1B's claim is a claim about a PAIR of fixtures rather than about either of them, so
	 * it cannot be made inside the loop that reads one. Everything here is measured off the
	 * solver except the depth, which is what the fixture presents.
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
		 * WHETHER THE WALL STOPS AT THE TOP OF THE CORBEL, AND IT DECIDES WHAT MAY BE ASSERTED
		 * RUNG BY RUNG.
		 *
		 * Where it does, the corbel's own courses are all the masonry there is, the hand ladder
		 * is the whole load and CoursesOverCorbelJoint is a statement about the corbel. Where
		 * the wall carries on above, both of those stop being true at once — there is extra
		 * load AND extra depth — so the rows below become one-sided and the two-sided claim
		 * moves to PART 1B, where it belongs, as a comparison between the pair.
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
				|| Course == HighestCorbelCourse(Case.CorbelSteps);

			if (bWorthPrinting)
			{
				/*
				 * THE THREE READINGS PRINTED BESIDE THE MEASUREMENT ARE BUILT FROM THE MOMENT
				 * THIS JOINT ACTUALLY CARRIES, NOT FROM THE HAND LADDER'S.
				 *
				 * They coincide where the wall stops at the top of the corbel, and that is the
				 * only place they may be quietly conflated: in a taller wall the hand ladder is
				 * the corbel's own mass and misses everything the courses above add — on the
				 * scenario wall by a factor of seven — so a line printed from it would read
				 * 0.031 beside a joint reading 0.223 and look like a defect in the solver
				 * rather than a defect in the print. The hand ladder is still printed, as the
				 * ladder, next to what the joint carries.
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
			 * THE LOAD PATH FIRST, AND SEPARATELY FROM THE SECTION. Composite action is a claim
			 * about what resists the moment, not about what the wall hands down, so the force
			 * and the moment must both still be the hand ladder's. 2% rather than the 1e-5 the
			 * staircase test uses, because ARCHING_DESIGN's own cross-check — the wedge's
			 * centroid arm against the ladder's average arm — only agrees to 1.3%, so an
			 * implementation that computed the wedge as one composite body rather than as a
			 * ladder is inside this and is not what this row is about.
			 *
			 * AND IT IS TWO-SIDED ONLY WHERE THE WALL STOPS AT THE TOP OF THE CORBEL. The hand
			 * ladder counts the corbel's own bricks and the half-neighbours feeding them, so it
			 * is the WHOLE load exactly when there is nothing above the corbel to add to it.
			 * Where the wall carries on, every one of those courses bears down through the same
			 * rungs and the ladder can only grow — so the claim there is `at least`, which is a
			 * statement about statics rather than about the depth rule, and it is the premise
			 * PART 1B's one-sided argument stands on.
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
			 * AND THEN THE SECTION, WHICH IS THE SLICE. `min(patch reading, M / (t D^2 / 6))`,
			 * both built from the hand ladder and from the depth the fixture presents.
			 *
			 * A DRY ROW IS ASSERTED AS AN ORDERING RATHER THAN AS A NUMBER, because f_xk1 = 0
			 * makes the answer TNumericLimits<double>::Max() and a relative tolerance on that
			 * says nothing.
			 *
			 * AND NOT AT ALL WHERE THE WALL CARRIES ON ABOVE THE CORBEL, because both halves of
			 * `Expected` are wrong there for reasons that have nothing to do with each other:
			 * the moment is the hand ladder's rather than the heavier one the wall above
			 * produces, and the depth is `CoursesHigh - Course`, which is the UNBOUNDED reading
			 * and is precisely the thing under suspicion. Asserting it would pin the defect in
			 * place. The reading is printed above with both candidate sections beside it, and
			 * the claim those rows make is in PART 1B.
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

		if (Case.Outcome == EVoidOutcome::Unasserted)
		{
			/*
			 * PRINTED AND NOT ASSERTED — see EVoidOutcome. This is the only row in the file whose
			 * verdict is downstream of a design decision nobody has made yet, so it says what
			 * happened and claims nothing about what should have.
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
	 * PART 1B — THE SAME CORBEL UNDER MORE WALL. THE PROPERTY, AND THE DEFECT IT NAMES.
	 * ===================================================================================
	 *
	 * THE COMPOSITE DEPTH IS MEASURED BY WALKING UP THE MASONRY UNTIL IT RUNS OUT, so the
	 * section credited to a corbel's bottom rung is set by THE WALL'S height and the moment
	 * bending it by THE CUT'S. While every fixture in the project had `CoursesHigh = Steps + 2`
	 * those were one number, `k^3` of moment met `k^2` of section, and the resulting crossover
	 * near thirty-six courses was the whole of the "something must still come down" defence.
	 * Decouple them and the reading goes as `k^3/m^2`, the exponents stop cancelling and the
	 * crossover moves as far up as you care to build. That is what the pairs below measure.
	 *
	 * THE PROPERTY, AND IT IS ASSERTED ONE-SIDED BECAUSE THE TWO-SIDED FORM IS FALSE FOR A
	 * REASON THAT HAS NOTHING TO DO WITH THE DEPTH. The review stated it as "the two
	 * utilisations must not diverge, because the hand ladder is unchanged between them". The
	 * hand ladder is NOT unchanged, and the fixture says so out loud: courses added above a
	 * corbel are full-width masonry STANDING ON IT, they bear down through every rung, and the
	 * moment ratios printed below are what that comes to. So the honest claim is the one that
	 * survives measuring:
	 *
	 *     ADDING MASONRY ON TOP OF A CORBEL MUST NOT MAKE THE CORBEL SAFER.
	 *
	 * More load, more overturning; whatever section the model credits, the reading may not
	 * FALL. That is one-sided, it needs no bound to have been chosen, and it is exactly what
	 * the defect violates — today the section grows as the square of a depth the corbel had no
	 * part in, so the reading collapses by the ratios printed below.
	 *
	 * WHAT THIS DISTINGUISHES, AND WHAT IT DOES NOT. It refuses the rule the code has now (all
	 * the masonry there is). It is satisfied by a rule that credits the corbel's own courses
	 * and by any rule that credits less — bond continuity, span-to-depth, a shear-transfer
	 * budget — because every one of those is a fact about the cut rather than about the wall.
	 * It is NOT a test of which of those is right, and it deliberately cannot be: the depth
	 * each would credit for the corbel's OWN courses is printed beside the measurement so the
	 * choice can be made with the numbers in hand. THE ONE CANDIDATE IT COULD ARGUE WITH is a
	 * shear-transfer budget that grows with the weight standing above — more clamping, more
	 * depth — since that could in principle credit MORE section in the taller wall. If that
	 * rule is ever chosen and this row goes red on it, the row is the argument that the rule is
	 * wrong rather than the other way round: a cantilever does not get stronger by having a
	 * building put on it.
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
				 * WHAT THE COMPETING DEPTH RULES WOULD SAY ABOUT THE TALLER WALL'S OWN BOTTOM
				 * RUNG, printed side by side and derived from the moment the solver actually
				 * publishes there rather than from the hand ladder — which, being the shorter
				 * wall's, understates it.
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
				 * HOW MUCH THIS PAIR ACTUALLY DECIDES, PRINTED SO NOBODY OVERCLAIMS IT. The
				 * property is satisfied by any depth rule crediting no more than
				 *
				 *     D_short * sqrt(M_tall / M_short)
				 *
				 * to the taller wall, since the section goes as D^2 and the reading may only
				 * be held level. That is a CEILING and not the answer: it does not force the
				 * depth down to the corbel's own courses, and it cannot distinguish a
				 * span-to-depth rule from a bond-continuity one from a shear budget that
				 * happens to land under it. What it does refuse is the walk the code takes now.
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
				 * THE PREMISE FIRST, BECAUSE THE ONE-SIDED CLAIM RESTS ENTIRELY ON IT. If the
				 * taller wall did NOT load the corbel harder, "must not read lower" would be an
				 * assertion about nothing. Strictly greater: twenty-odd courses of masonry
				 * standing on a cantilever is not a rounding difference.
				 */
				TestTrue(
					FString::Printf(
						TEXT("FIXTURE: %d steps under %d courses must bend its bottom rung HARDER ")
						TEXT("than the same cut under %d — %s brick-weight-cm against %s"),
						Tall.CorbelSteps, Tall.CoursesHigh, Short.CoursesHigh,
						*Bits(Tall.MomentBrickWeightCm), *Bits(Short.MomentBrickWeightCm)),
					Tall.MomentBrickWeightCm > Short.MomentBrickWeightCm);

				/*
				 * AND THE PROPERTY. No tolerance, because the claim is an ORDERING rather than a
				 * value and a slack one would be satisfied by the defect it exists to catch.
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
		 * AND THE TABLE MUST STILL CONTAIN PAIRS TO COMPARE. Every claim above is conditional on
		 * two rows sharing a step count and a mortar, so deleting one of a pair would take the
		 * property with it in silence — which is how the depth came to be unbounded and unseen in
		 * the first place.
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
	 * against today's 0.058203838 * n. Left unbounded that reading is the lesser from m = 2
	 * upward, falls as 1/m^2, and never reaches 1.0 for any wall this game builds — which is
	 * exactly why the depth has to be bounded by SOMETHING, since on its own this mechanism
	 * makes every free end and every jamb unbreakable in bending. The unbounded row is printed
	 * below and nothing is derived from it.
	 *
	 * =========================================================================================
	 * AND WHAT IT ACTUALLY READS IS THE ARM'S OWN IDENTITY, NOT THAT ONE. RE-DERIVED FOR
	 * COMPOSITE_DEPTH_DESIGN.md SLICE 2, AND THE MOVE IS THE WHOLE POINT OF THE DEPTH BOUND.
	 * =========================================================================================
	 *
	 * The credited depth is `min(masonry above, max(the corbelling body's own depth, lambda*e))`
	 * with `e = |M|/|F|`, and on this fixture the ARM is what binds: the wall has 217.5 cm of
	 * masonry standing over the joint and the joint's own statical arm permits 38.92 of it.
	 * The credited depth fell from the whole wall to about five courses, and the reading went
	 * 0.0158818 -> 0.5011403 with it.
	 *
	 * THAT IS CORRECT PHYSICS AND NOT A WEAKENING. A half seat's arm settles at 11.25 cm
	 * however tall the column gets — its own weight acts at 5.625 and everything from above at
	 * 11.25, so `e = (5.625 + 11.25(n-1))/n -> 11.25` — so `lambda*e` settles at about 39 cm
	 * and the section STOPS GROWING while the load keeps coming. The reading is therefore
	 * LINEAR in the wall's height rather than falling as 1/m^2, and the ruling survives by
	 * 1.97x rather than by 62x. `Core.Structure.AFreeEndDeletionInATallWall` measures the whole
	 * ladder — 0.3283 / 0.5011 / 0.6740 / 0.8469 / 1.0198 at 20 / 30 / 40 / 50 / 60 courses —
	 * and this fixture's wall is its 30-course row.
	 *
	 * WHERE `lambda*e` GOVERNS THE READING COLLAPSES TO ONE IDENTITY, and it is asserted in
	 * that form rather than as the literal 0.5011403 for the same reason PART 2's moment walk
	 * has always been asserted as a walk: AN IDENTITY SURVIVES A CHANGE OF lambda AND A LITERAL
	 * DOES NOT, and lambda is formally provisional until slice 3 rules on it. Substituting
	 * `D = lambda*M/F` into `sigma = M*W/(t*D^2/6 * 10^4)`,
	 *
	 *     util  =  K * F^2 / M         K = [6*W_brick/(t * 10^4 * f_xk1)] / lambda^2
	 *                                    = 1.561287 / lambda^2  =  0.130114883 at lambda = 3.464
	 *
	 * with F in brick weights and M in brick-weight-centimetres. Note it goes as F SQUARED over
	 * M and not as M over anything: a deeper arm buys section faster than it costs moment, which
	 * is why the same joint under a taller wall reads higher rather than lower.
	 *
	 * THREE ROWS, AND EACH ONE IS LOAD-BEARING RATHER THAN A RESTATEMENT OF THE LAST.
	 *
	 *   - THE ARM GOVERNS. The credited depth must BE `lambda*e`, and must sit strictly between
	 *     the one course under the joint and the 217.5 cm over it. Without this the identity is
	 *     a claim about a case that is not occurring, and it would go silently green the day
	 *     the wall or the floor started binding instead.
	 *
	 *   - THE IDENTITY, EXACTLY. `util == K*F^2/M` on the solver's own force and moment, to a
	 *     relative 1e-12. It reproduces bit for bit, because it IS the arithmetic production
	 *     performs, in a form derived independently of the order production performs it in;
	 *     what it pins is that the section is `t*(lambda*e)^2/6` with NO axial relief, which is
	 *     reading (a) of three, and that lambda is 3.464. It does not check the moment, and
	 *     says so — the two rows either side of it are what check that.
	 *
	 *   - AND THE SAME IDENTITY ON THE HAND MOMENT, at 2%, WHICH IS THE NON-CIRCULAR ONE.
	 *     `K * n^2 / (5.625 + 11.25(n-1))` takes ONLY the force from the solver — a force is not
	 *     what this slice changes — and comes to 0.5063502, which is COMPOSITE_DEPTH_DESIGN's
	 *     own published 0.5064 for this row. The two differ by the 1.04% the solver's moment
	 *     exceeds the two-arm walk by, which is the half bat hanging off the head joint
	 *     contributing on an arm of its own; the residual is measured and printed rather than
	 *     absorbed, and 2% is twice it and slack for nothing else.
	 *
	 * AND ONE MORE, ACROSS FILES: the number this joint reads must be the number
	 * `AFreeEndDeletionInATallWall` reads at thirty courses, TO THE LAST DIGIT. Two files
	 * measuring one joint and disagreeing is worth catching on its own, and the shared literal
	 * is `ArchingWallTestSupport::FreeEndThirtyCourseUtilisation`, which is documented there as
	 * an agreement anchor and not as a derivation.
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
		 * lambda, SPELLED OUT HERE RATHER THAN IMPORTED FROM THE SOLVER.
		 *
		 * 3.464 is 2*sqrt(3), and COMPOSITE_DEPTH_DESIGN.md is explicit that it is a RULING
		 * inside a window rather than a derivation — its floor is this very fixture's ruling
		 * and its ceiling is PART 1B's one-sided property, and slice 3 exists so somebody
		 * chooses knowingly. A test that read the solver's own constant would agree with a
		 * wrong one, and this file is one of the two that decide whether it is wrong.
		 */
		constexpr double CompositeDepthPerArm = 3.464;

		/**
		 * K = [6*W/(t * 10^4 * f_xk1)] / lambda^2, THE WHOLE IDENTITY IN ONE COEFFICIENT.
		 *
		 * The bracket is 1.561287 exactly and is the section arithmetic; dividing by lambda^2
		 * is the depth rule. Kept as two factors rather than one number so that a change to
		 * lambda moves it and a change to the brick or the mortar moves it, separately and
		 * visibly.
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

			/*
			 * WHAT THE WALL OFFERS, WHAT THE ARM PERMITS, AND WHAT THE JOINT WAS ACTUALLY
			 * CREDITED. The wall has 29 courses over this joint; the arm permits about five of
			 * them. Both are stated so a reading that moved says WHICH of the three terms of
			 * `min(above, max(body, lambda*e))` moved.
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
			 * ROW ONE: THE ARM GOVERNS, AND IT HAS TO BE ESTABLISHED BEFORE THE IDENTITY IS
			 * WORTH ANYTHING. `util = K*F^2/M` is what the reading collapses to only where
			 * `lambda*e` is the binding term; if the wall or the corbelling body were binding
			 * instead, the identity would be a claim about a case that is not occurring and
			 * would go green while measuring something else entirely. STRICTLY between the two,
			 * so neither bound is merely equal to it by luck.
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
			 * ROW TWO: THE IDENTITY, EXACTLY. Substituting D = lambda*M/F into the section
			 * arithmetic gives util = K*F^2/M with no D left in it, and it reproduces the
			 * reading bit for bit — so 1e-12 relative is slack for a different order of
			 * operations and for nothing else. WHAT IT PINS is the section: t*(lambda*e)^2/6,
			 * reading (a), no axial relief, and lambda = 3.464. WHAT IT DOES NOT PIN is the
			 * moment, since M appears on both sides; the two-arm walk above and row three
			 * below are what check that, and they take only the force from the solver.
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
			 * ROW THREE: THE SAME IDENTITY WITH THE MOMENT DERIVED RATHER THAN READ BACK, which
			 * is the one that is not circular. Only the force comes from the solver, and a force
			 * is not what this slice changes. It comes to 0.5063502 against the reading's
			 * 0.5011403 — 1.04% apart, which is exactly the residual the two-arm walk leaves and
			 * which is printed above — and 0.5063502 is COMPOSITE_DEPTH_DESIGN.md's own
			 * published figure for this row, to four digits. That figure is being used as a
			 * cross-check here and it FAILS IN THE TEST rather than being tuned away.
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
			 * AND THE CROSS-FILE ROW. `Core.Structure.AFreeEndDeletionInATallWall` cuts the same
			 * brick out of the same 7 x 30 wall and reads the same seat, so the two files are
			 * one measurement made twice. Held to the LAST DIGIT, against the literal both of
			 * them share — two files measuring one joint and disagreeing is worth catching on
			 * its own, and it is the only thing this literal is for.
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
