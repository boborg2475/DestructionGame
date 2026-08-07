// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * THE STAIRCASE VOID, AS GEOMETRY AND AS ARITHMETIC — SHARED BY EVERY TEST THAT CUTS ONE.
 *
 * A player cut a staircase-shaped hole through the game's own wall and the brickwork above it
 * stood there hanging metres out over open air. Three tests now photograph that same cut from
 * three different distances: the world-free one that reads what the corbel joints CARRY, the
 * integration one that watches the bricks actually come down, and the visual one that writes a
 * PNG for a human. The geometry lived in the integration test until the second caller needed
 * it, and a second copy of a void definition is two fixtures that drift.
 *
 * WORLD-FREE ON PURPOSE, AND THAT IS WHY IT IS NOT IN BrickWorldTestSupport.h. That header
 * drags in UWorld, the player controller and the input subsystem; everything here is boxes and
 * doubles, so the fast suite can include it without paying for any of that.
 *
 * NAMED NAMESPACE, and named differently from every other one in this directory. An anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file and a unity build merges
 * many files into one; the `using namespace` for this one lives inside each RunTest body for
 * the same reason. Free functions are `inline` because more than one translation unit includes
 * this and a non-unity build would otherwise fail at link.
 *
 * NOTHING HERE IS IMPORTED FROM THE CODE UNDER TEST. The grid is re-derived from the brick and
 * the joint, the ladder of loads is counted by hand off the picture, and the newton-to-Unreal
 * conversion is spelled out as the literal 10000 rather than read off
 * DestructionStrength::ForceUnitsPerMPaSqCm — so a wrong constant in production makes these
 * tests DISAGREE with it instead of agreeing with it.
 */
namespace StaircaseWallTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * A wall of the same KIND the game mode lays, small enough to spawn one actor per piece.
	 *
	 * FLUSH RATHER THAN RAGGED, and it is not a style choice. A ragged wall's alternate courses
	 * step in, so the end brick of every even course rests on ONE brick below it instead of two —
	 * which is a single-support piece, which under DESIGN.md's determinate rule carries a real
	 * moment, at the same 5.625 cm eccentricity the staircase's own corbel has. A flush end fills
	 * that half cell with a half bat, so the end brick has two supports, its centre of mass sits at
	 * the area-weighted centroid of them, and the eccentricity is exactly zero.
	 * ADestructionGameGameMode lays a flush wall for its scenario, so this is also the wall the
	 * player was actually looking at.
	 *
	 * A RAGGED BASELINE IS SMALL RATHER THAN LARGE, AND IT IS STILL THE WRONG BASELINE. A plain
	 * toothed end is the ZIG-ZAG shape: the joint the course above arrives through has the same
	 * centroid as the joint the end brick leaves by, so the load it hands down carries no lever arm
	 * across, and the end joint is left with its OWN weight's 5.625 cm and nothing else while the
	 * compression under it grows course by course. A 13-course ragged wall therefore reads 0.058 at
	 * its ends and falls from there. It is a baseline that MOVES with the height of the wall, which
	 * is exactly what a fixture measuring the staircase must not have.
	 *
	 * THIRTEEN COURSES OF TEN, because the picture is of a void ten to fifteen courses tall with
	 * the brickwork above it reaching several bricks out over nothing. Thirteen is the smallest
	 * height that carries the whole staircase AND leaves an intact course on top of it.
	 */
	constexpr int32 StaircaseCoursesHigh = 13;

	inline FRunningBondSpec StaircaseWallSpec()
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = StaircaseCoursesHigh;
		Spec.BricksPerCourse = 10;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	/**
	 * 136 pieces: 7 even courses of 10 full bricks, 6 odd courses of 9 full bricks and 2 half bats.
	 *
	 * Asserted as a fixture precondition so that a producer that quietly laid a different wall
	 * fails there rather than making every index below name something else.
	 */
	constexpr int32 StaircaseWallPieceCount = 136;

	/*
	 * THE COORDINATING GRID, RE-DERIVED HERE RATHER THAN IMPORTED. A brick plus a joint is one
	 * cell along the wall and one course up; running bond offsets alternate courses by HALF a
	 * cell, and that half cell is the only step a corbel can take — which is why the staircase
	 * below moves 11.25 cm per course and not a brick's length.
	 */
	constexpr double StaircaseBrickPitchCm = 21.5 + 1.0;
	constexpr double StaircaseCoursePitchCm = 6.5 + 1.0;
	constexpr double StaircaseHalfStepCm = StaircaseBrickPitchCm * 0.5;

	/** Centre height of a course, cm: half a brick up, then one course pitch per course. */
	inline double StaircaseCourseZCm(int32 Course)
	{
		return 6.5 * 0.5 + Course * StaircaseCoursePitchCm;
	}

	/** The lowest and highest courses the void cuts through. Course 0 and course 12 stay whole. */
	constexpr int32 StaircaseLowestVoidCourse = 1;
	constexpr int32 StaircaseHighestVoidCourse = 11;

	/**
	 * THE STAIRCASE ITSELF: everything left of this X, in this course, is cut away.
	 *
	 * One half step further left per course going up, so the wall's remaining left edge steps out
	 * over the hole exactly as a corbel does — and the run above the void reaches
	 * (12 - 1) x 11.25 = 123.75 cm, five and a half brick lengths, past where the void's bottom
	 * course ends. That is the "cantilevered many brick-widths out over empty space" in the
	 * picture.
	 *
	 * The brick whose centre sits exactly ON the edge is KEPT — it is the corbelled brick, and it
	 * is the whole subject of the test. Nothing else is within 11.25 cm of the boundary, so the
	 * comparison has half a brick of slack either way and is not a tolerance question.
	 */
	inline double StaircaseVoidEdgeXCm(int32 Course)
	{
		return (StaircaseHighestVoidCourse + 1 - Course) * StaircaseHalfStepCm;
	}

	/** Which course a laid box is in, read back off its height. */
	inline int32 StaircaseCourseOf(const FPieceBox& Box)
	{
		return FMath::RoundToInt32((Box.CentreCm.Z - 6.5 * 0.5) / StaircaseCoursePitchCm);
	}

	/**
	 * The handle of the piece laid at this exact spot, or INDEX_NONE.
	 *
	 * BY POSITION RATHER THAN BY INDEX, for the same reason joints are found by piece pair: a
	 * handle is an artefact of the order the producer happened to lay pieces in, and a fixture
	 * that hard-coded one would silently start naming a different brick the day that order
	 * changed. Every coordinate here is an exact multiple of 11.25 or 7.5, so the tolerance is
	 * slack against a re-association and nothing else.
	 */
	inline int32 StaircasePieceAt(TArrayView<const FPieceBox> Boxes, double XCm, double ZCm)
	{
		for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
		{
			if (FMath::IsNearlyEqual(Boxes[Piece].CentreCm.X, XCm, 0.01)
				&& FMath::IsNearlyEqual(Boxes[Piece].CentreCm.Z, ZCm, 0.01))
			{
				return Piece;
			}
		}

		return INDEX_NONE;
	}

	/**
	 * Every piece the void takes out, in handle order.
	 *
	 * THE COURSE RANGE AND THE EDGE ARE ABSOLUTE, NOT RELATIVE TO THE WALL'S HEIGHT, which is
	 * what lets the SAME void be cut into a taller wall: the visual harness cuts it into the
	 * game mode's own 40-course scenario wall, and it must be the same hole in the same place.
	 * A wall with more courses above the void simply has more brickwork hanging over it.
	 */
	inline TArray<int32> StaircaseVoidPieces(TArrayView<const FPieceBox> Boxes)
	{
		TArray<int32> Cut;

		for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
		{
			const int32 Course = StaircaseCourseOf(Boxes[Piece]);

			if (Course < StaircaseLowestVoidCourse || Course > StaircaseHighestVoidCourse)
			{
				continue;
			}

			if (Boxes[Piece].CentreCm.X < StaircaseVoidEdgeXCm(Course) - 0.001)
			{
				Cut.Add(Piece);
			}
		}

		return Cut;
	}

	/** The 36 bricks the staircase takes out, counted by hand from the diagram in the test. */
	constexpr int32 StaircaseVoidPieceCount = 36;

	/**
	 * The lowest and highest courses whose leftmost brick is CORBELLED — held by one bed joint,
	 * hanging half a brick past it, with the whole run above bearing down on it.
	 *
	 * Course 1's leftmost survivor still rests on two bricks of the grounded course, so it is not
	 * corbelled; course 12 is untouched by the void and is corbelled anyway, because the course
	 * beneath it now starts half a step to its right.
	 */
	constexpr int32 StaircaseLowestCorbelCourse = 2;
	constexpr int32 StaircaseHighestCorbelCourse = 12;

	constexpr int32 StaircaseCorbelStepCount =
		StaircaseHighestCorbelCourse - StaircaseLowestCorbelCourse + 1;

	/** The corbelled brick of a course: the leftmost survivor, sitting exactly on the void edge. */
	inline int32 StaircaseCorbelPiece(TArrayView<const FPieceBox> Boxes, int32 Course)
	{
		return StaircasePieceAt(Boxes, StaircaseVoidEdgeXCm(Course), StaircaseCourseZCm(Course));
	}

	/** The one brick still under a corbelled brick: half a step right, one course down. */
	inline int32 StaircaseCorbelSupportPiece(TArrayView<const FPieceBox> Boxes, int32 Course)
	{
		return StaircasePieceAt(
			Boxes,
			StaircaseVoidEdgeXCm(Course) + StaircaseHalfStepCm,
			StaircaseCourseZCm(Course - 1));
	}

	/**
	 * The far end of the wall, which the void never reaches and which must not move.
	 *
	 * Nine brick pitches from the origin is the last full brick of an even course, and it is four
	 * brick lengths clear of the highest step the staircase takes.
	 */
	constexpr double StaircaseFarSideXCm = 9 * StaircaseBrickPitchCm;

	/*
	 * ================================================================================
	 * THE ARITHMETIC, WORKED THROUGH INDEPENDENTLY OF THE SOLVER.
	 * ================================================================================
	 *
	 * WHY A CORBEL LOADS ITS JOINT OFF CENTRE, STATED HERE SO NOBODY RE-DERIVES IT. Running bond
	 * offsets alternate courses by half a cell, so the smallest step a wall's edge can take is
	 * 11.25 cm — half a brick pitch. A brick whose left-hand neighbour below has been cut away
	 * keeps exactly ONE bed joint, and that joint is the 10.25 cm strip the two still share: its
	 * centroid sits 5.625 cm to the RIGHT of the brick's own centre of mass. That 5.625 cm is the
	 * arm the brick's OWN weight acts on.
	 *
	 * AND THE ARM ACCUMULATES DOWN THE STAIRCASE, WHICH IS THE WHOLE OF WHY THE LADDER IS STEEP.
	 * The corbelled brick one course UP is half a step further out, so the patch its load arrives
	 * through sits 11.25 cm to the LEFT of the patch this brick leaves by — and a moment carried
	 * across that gap picks up the transfer term of ordinary statics. So each step of the corbel
	 * takes, about its own patch:
	 *
	 *     its own weight                          1 x 5.625 brick-weight-centimetres
	 *     the corbel above, re-referenced         M_above + 11.25 x F_above
	 *     everything else resting on it           NOTHING AT ALL
	 *
	 * THE THIRD LINE IS THE ONE WORTH CHECKING. What else rests on a corbelled brick is the next
	 * brick along the course above, and that brick sits on TWO supports — statically indeterminate,
	 * so the model gives its joints no moment (MOMENTS_DESIGN.md) — while the patch it hands its
	 * share down through is the SAME 10.25 cm strip this brick leaves by, so re-referencing it adds
	 * a lever arm of exactly zero. It contributes force and nothing else.
	 *
	 * A full brick is 21.5 x 10.25 x 6.5 cm of clay at 1.9 g/cm3, so
	 * W = 2.72163125 kg x 980 cm/s2 = 2667.198625 Unreal force units — and DESIGN.md §3's
	 * 1 N = 100 uu is already inside that product and must not be applied again. A bed joint of a
	 * corbelled brick is 10.25 x 10.25 = 105.0625 cm2, and bending about the joint's Y axis is
	 * resisted by W_v = (4/3) x 5.125 x 5.125^2 = 179.4817708 cm3. Beam theory on an uncracked
	 * rectangle puts the fibre stress at |M|/W_v -+ N/A, so for a moment M in brick-weight-
	 * centimetres and a force F in brick weights the tension at the opened edge is
	 *
	 *     2667.198625 x (M / 179.4817708 - F / 105.0625) / 10000  MPa
	 *
	 * where 10000 uu per MPa.cm2 is spelled out rather than imported. Against general-purpose
	 * mortar's f_xk1 = 0.10 MPa (EN 1996-1-1 §3.6.3 Table 3.2, and already the number in the
	 * profile) the bottom step of the staircase would reach 22.9 times capacity ON THAT PATCH
	 * ALONE.
	 *
	 * AND THE PATCH ALONE IS NOT WHAT RESISTS IT — WHICH IS THE USER'S RULING OF 2026-08-06 AND
	 * IS WHY EVERY NUMBER BELOW MOVED.
	 *
	 * A stack of courses over a lost support does not resist its overturning moment as a
	 * sequence of independent bed patches. The wall acts as a DEEP BEAM and the plane taking the
	 * moment is a VERTICAL section through the bonded masonry standing over the joint, `t*D^2/6`
	 * for a wall t thick and D deep. Eleven courses stand over the bottom step of this staircase
	 * — 82.5 cm — so that section is 10.25 x 82.5^2 / 6 = 11,627.34 cm3 against the patch's
	 * 179.48, a factor of 64.8, and the rung reads 0.369 instead of 22.93. ARCHING_DESIGN.md
	 * slice 5.
	 *
	 * THE JOINT GIVES AT THE LESSER OF THE TWO, because composite action is an ALTERNATIVE way
	 * of carrying the moment rather than an extra one. That `min` is what keeps the model nested
	 * rather than replaced, and it is load-bearing at the TOP of the ladder: one course of depth
	 * is 96.09 cm3, SHALLOWER than the bed patch, so course 12 keeps its own 0.058203838 to the
	 * last bit. It is also why `StructureBinding.AdoptedWallLoadsItsWaistEccentrically` and
	 * `StructurePushTest`'s ragged end do not move — both are the top course of their wall.
	 *
	 * NO AXIAL TERM IS SUBTRACTED FROM THE COMPOSITE READING. The plane resisting a deep-beam
	 * moment is vertical; the weight standing over the joint is shear on it rather than load
	 * across it, so there is no compression there to close it. Subtracting the patch's own N/A
	 * from a stress on a different plane reads this corbel as EXACTLY ZERO.
	 *
	 * AND A COMPOSITE OF ONE IS NOT A COMPOSITE. The top step of a corbel has nothing resting on
	 * it, so there is no stack to act together and its own bed patch is the only section there
	 * is. The arithmetic would have said the same — one course is shallower than the patch — but
	 * the two agreeing is a coincidence of this brick's proportions rather than a rule.
	 */

	/** 1.9 g/cm3 x 21.5 x 10.25 x 6.5 cm / 1000 = 2.72163125 kg, x 980 cm/s2. */
	constexpr double StaircaseFullBrickWeightUu = 1.9 * 21.5 * 10.25 * 6.5 / 1000.0 * 980.0;

	/** The half-brick bed patch a corbel keeps: 10.25 along the wall by 10.25 through it. */
	constexpr double StaircaseCorbelBedAreaSqCm = 10.25 * 10.25;

	/** Half the bed patch's extent on each in-plane axis; it is square, so they are equal. */
	constexpr double StaircaseCorbelBedHalfCm = 10.25 / 2.0;

	/**
	 * How far the corbelled brick's OWN weight acts from the centroid of the patch that carries it.
	 *
	 * Half of one half-step: the brick spans a full cell and keeps half of it, so its centre is
	 * a quarter of a brick pitch clear of the patch's centre. 22.5 / 4 = 5.625 cm.
	 *
	 * ITS OWN WEIGHT ONLY, and that is the whole reason the moment ladder below exists as a
	 * separate walk: the load a step receives from the corbel above it arrives on a DIFFERENT
	 * patch, a further half-step out, and carries its own arm across.
	 */
	constexpr double StaircaseCorbelOwnWeightArmCm = StaircaseHalfStepCm / 2.0;

	/** (4/3) x h_u x h_v^2 for the square patch, cm3. */
	constexpr double StaircaseCorbelSectionModulusCm3 =
		(4.0 / 3.0) * StaircaseCorbelBedHalfCm * StaircaseCorbelBedHalfCm * StaircaseCorbelBedHalfCm;

	/**
	 * Unreal force units per MPa per cm2, SPELLED OUT RATHER THAN IMPORTED.
	 *
	 * DESIGN.md §3's whole units section exists because a missing or duplicated conversion is out
	 * by exactly 100x and tuned thresholds conceal it well. Production has one named boundary for
	 * this; a test that read it would agree with a wrong one, so this writes the number itself.
	 */
	constexpr double StaircaseForceUnitsPerMPaSqCm = 10000.0;

	/** EN 1996-1-1 Table 3.2's f_xk1 for general-purpose mortar, asserted against the profile. */
	constexpr double StaircaseMortarTensileMPa = 0.10;

	/** EN 1996-1-1's compressive figure for the same joint, likewise asserted, never imported. */
	constexpr double StaircaseMortarCompressiveMPa = 10.0;

	/**
	 * WHAT EACH STEP OF THE CORBEL CARRIES, IN BRICK WEIGHTS, COUNTED BY HAND OFF THE PICTURE.
	 *
	 * Each corbelled brick takes its own weight, ALL of the corbelled brick above it — which has
	 * nowhere else to go — and half of the next brick along, whose own share grows the same way.
	 * Reading down from the top of the staircase that gives 1, 2.5, 4.5, 7, 10, 13.5, 17.5, 22,
	 * 27, 32.5, 38.5, i.e. 1 + n + n(n+1)/4 with n steps below the top. Written out rather than
	 * evaluated from that closed form, because the eleven numbers are the hand count and the
	 * closed form is only a summary of them.
	 *
	 * INDEXED BY COURSE - StaircaseLowestCorbelCourse, so entry 0 is the BOTTOM of the corbel and
	 * carries the most.
	 */
	constexpr double StaircaseCorbelLoadBrickWeights[StaircaseCorbelStepCount] =
	{
		38.5, 32.5, 27.0, 22.0, 17.5, 13.5, 10.0, 7.0, 4.5, 2.5, 1.0
	};

	/** The load on one step of the corbel, in Unreal force units. */
	inline double StaircasePredictedCorbelForceUu(int32 Course)
	{
		return StaircaseCorbelLoadBrickWeights[Course - StaircaseLowestCorbelCourse]
			* StaircaseFullBrickWeightUu;
	}

	/**
	 * WHAT BENDS EACH STEP OF THE CORBEL, IN BRICK-WEIGHT-CENTIMETRES, WALKED DOWN BY HAND.
	 *
	 * Straight off the three lines of the block comment above, from the top of the staircase
	 * downward: the top step carries only itself at 5.625, and every step below it adds its own
	 * 5.625 to the step above's moment carried across the 11.25 cm the corbel has stepped out.
	 *
	 *     M_12 = 5.625
	 *     M_k  = 5.625 + 11.25 x F_(k+1) + M_(k+1)
	 *
	 * which unrolls to 5.625, 22.5, 56.25, 112.5, 196.875, 315, 472.5, 675, 928.125, 1237.5,
	 * 1608.75. Written out rather than evaluated from that recursion for the same reason the
	 * forces are: the eleven numbers are the hand walk, and the recursion is a summary of it.
	 *
	 * THE ARM IS NOT CONSTANT AND THE FORCE IS NOT THE WHOLE STORY. Between the top step and the
	 * bottom one the force grows 38.5-fold and the moment grows 286-fold, because the arm the
	 * accumulated load acts on grows with it — which is why the ladder's top three rungs hold and
	 * the eight below them do not.
	 *
	 * INDEXED BY COURSE - StaircaseLowestCorbelCourse, so entry 0 is the BOTTOM of the corbel.
	 */
	constexpr double StaircaseCorbelMomentBrickWeightCm[StaircaseCorbelStepCount] =
	{
		1608.75, 1237.5, 928.125, 675.0, 472.5, 315.0, 196.875, 112.5, 56.25, 22.5, 5.625
	};

	/*
	 * The base case of that walk, tied to the arm it is made of. The top step of the corbel carries
	 * one brick weight and nothing else, so its moment IS the own-weight arm — and a hand walk whose
	 * first rung disagreed with the arm it was written from would be a ladder built on nothing.
	 */
	static_assert(
		StaircaseCorbelMomentBrickWeightCm[StaircaseCorbelStepCount - 1]
			== StaircaseCorbelOwnWeightArmCm,
		"the top step of the corbel carries only its own weight, so its moment is that one arm");

	/** The moment on one step of the corbel, in Unreal force units times centimetres. */
	inline double StaircasePredictedCorbelMomentUuCm(int32 Course)
	{
		return StaircaseCorbelMomentBrickWeightCm[Course - StaircaseLowestCorbelCourse]
			* StaircaseFullBrickWeightUu;
	}

	/**
	 * How many courses of bonded masonry stand over one step's bed joint.
	 *
	 * The joint sits under the brick of course c, so what is above it is courses c through the
	 * top: 13 - c of them. For the BOTTOM step that is ELEVEN courses, 82.5 cm; for the top step
	 * it is one, which is the step's own brick and nothing else.
	 */
	constexpr int32 StaircaseCoursesOverCorbelJoint(int32 Course)
	{
		return StaircaseCoursesHigh - Course;
	}

	/**
	 * The DEEP BEAM's section, cm3: t x D^2 / 6, with t the wall's thickness and D the depth of
	 * masonry standing over the joint. Zero where there is no stack to act together.
	 *
	 * ZERO FOR THE TOP STEP, AND THAT IS THE MECHANISM RATHER THAN A GUARD. One course is one
	 * brick with nothing resting on it, which is not a composite of anything, so there is no
	 * second section and the bed patch is the only one there is.
	 */
	inline double StaircaseCorbelCompositeModulusCm3(int32 Course)
	{
		const int32 Courses = StaircaseCoursesOverCorbelJoint(Course);

		if (Courses < 2)
		{
			return 0.0;
		}

		const double DepthCm = Courses * StaircaseCoursePitchCm;

		return 10.25 * DepthCm * DepthCm / 6.0;
	}

	/**
	 * What the tension at the opened edge of that step's joint comes to, as a fraction of
	 * mortar's flexural bond strength: the LESSER of the bed patch's own reading and the deep
	 * beam's.
	 *
	 * WHICH AXIS GOVERNS IS NOT ASSUMED — see StaircasePredictedCorbelCompressionUtilisation.
	 * ComputeUtilisation returns the WORST of the three axes, so a fixture aimed at bending would
	 * silently measure compression the moment compression happened to be higher, and every test
	 * using these numbers asserts the comparison before it claims anything. It is a closer thing
	 * than it was: relieving the opened edge from 22.93 to 0.369 leaves it only 1.5x clear of the
	 * squeezed one, where it used to be 92x.
	 */
	inline double StaircasePredictedCorbelUtilisation(int32 Course)
	{
		const double BendingMPa = StaircasePredictedCorbelMomentUuCm(Course)
			/ (StaircaseCorbelSectionModulusCm3 * StaircaseForceUnitsPerMPaSqCm);

		const double NormalMPa = StaircasePredictedCorbelForceUu(Course)
			/ (StaircaseCorbelBedAreaSqCm * StaircaseForceUnitsPerMPaSqCm);

		const double PatchMPa = FMath::Max(0.0, BendingMPa - NormalMPa);

		const double CompositeModulusCm3 = StaircaseCorbelCompositeModulusCm3(Course);

		if (!(CompositeModulusCm3 > 0.0))
		{
			return PatchMPa / StaircaseMortarTensileMPa;
		}

		const double CompositeMPa = StaircasePredictedCorbelMomentUuCm(Course)
			/ (CompositeModulusCm3 * StaircaseForceUnitsPerMPaSqCm);

		return FMath::Min(PatchMPa, CompositeMPa) / StaircaseMortarTensileMPa;
	}

	/**
	 * The other edge of the same joint, squeezed rather than opened, against 10 MPa.
	 *
	 * BOTH EDGES MOVE TOGETHER. Where the deep beam is what resists the moment the bed patch is
	 * not bending at all, so what is left is two planes with one stress each — the bed plane
	 * under a uniform N/A and the vertical plane under +-M/W_c — and the worst squeezed fibre is
	 * the larger of them rather than their sum. Leaving this edge at the patch's own
	 * M/W_v + N/A would put the bottom of THIS ladder at 0.249 against a relieved tension of
	 * 0.369, which is close enough that a slightly deeper corbel would fail in an axis nobody
	 * was measuring.
	 */
	inline double StaircasePredictedCorbelCompressionUtilisation(int32 Course)
	{
		const double BendingMPa = StaircasePredictedCorbelMomentUuCm(Course)
			/ (StaircaseCorbelSectionModulusCm3 * StaircaseForceUnitsPerMPaSqCm);

		const double NormalMPa = StaircasePredictedCorbelForceUu(Course)
			/ (StaircaseCorbelBedAreaSqCm * StaircaseForceUnitsPerMPaSqCm);

		const double CompositeModulusCm3 = StaircaseCorbelCompositeModulusCm3(Course);

		if (!(CompositeModulusCm3 > 0.0))
		{
			return (BendingMPa + NormalMPa) / StaircaseMortarCompressiveMPa;
		}

		const double CompositeMPa = StaircasePredictedCorbelMomentUuCm(Course)
			/ (CompositeModulusCm3 * StaircaseForceUnitsPerMPaSqCm);

		if (!(CompositeMPa < FMath::Max(0.0, BendingMPa - NormalMPa)))
		{
			return (BendingMPa + NormalMPa) / StaircaseMortarCompressiveMPa;
		}

		return FMath::Max(CompositeMPa, NormalMPa) / StaircaseMortarCompressiveMPa;
	}

	/**
	 * How many steps of the corbel the arithmetic puts OVER capacity, and it is NONE OF THE
	 * ELEVEN.
	 *
	 * IT WAS EIGHT, AND THE CHANGE IS THE WHOLE POINT OF SLICE 5. Read against each step's own
	 * bed patch the ladder marched 0.058, 0.271, 0.722, 1.494, 2.672, 4.338, 6.577, 9.472,
	 * 13.107, 17.565, 22.930 and crossed 1.0 between its eighth and ninth rungs. Read against
	 * the masonry actually standing over each step it marches 0.058, 0.156, 0.173, 0.195, 0.219,
	 * 0.243, 0.268, 0.293, 0.318, 0.343, 0.369 — monotonic in the same direction, a factor of 62
	 * lower at the bottom, and nowhere near the line.
	 *
	 * THE LADDER STILL HAS TEETH. It is not flat: the bottom step reads six times the top one,
	 * so a model that had simply deleted the moment would not produce it either.
	 */
	constexpr int32 StaircasePredictedCorbelJointsOverCapacity = 0;

	/**
	 * The bottom rung: 1608.75 brick-weight-centimetres of bending against the 11,627.34 cm3 of
	 * vertical section that eleven courses of masonry make, and NOT against the patch's 179.48.
	 *
	 * 22.92952589 is what the same moment reads on the bed patch alone, and it is kept in the
	 * comment because it is the number the photographed failure was condemned by.
	 */
	constexpr double StaircasePredictedWorstCorbelUtilisation = 0.3690314727;

	/** Whether the arithmetic condemns this step of the corbel outright. */
	inline bool StaircaseCorbelIsCondemned(int32 Course)
	{
		return StaircasePredictedCorbelUtilisation(Course) > 1.0;
	}

	/**
	 * The joint between two named pieces, or INDEX_NONE.
	 *
	 * BY PIECE PAIR RATHER THAN BY INDEX, because a joint index is an artefact of the order the
	 * producer happened to emit pairs in, and a test that hard-coded one would silently start
	 * watching a different joint the day that order changed. Both orientations are accepted:
	 * which piece is A and which is B is the producer's business, and DESIGN.md §3 is explicit
	 * that a consistently oriented joint reports identical loads either way.
	 *
	 * GENERAL RATHER THAN STAIRCASE-SPECIFIC, and it lives here because this is the only header
	 * every test that needs it can include — the integration file's FindIntegrationJoint now
	 * forwards to it rather than keeping a second copy.
	 */
	inline int32 JointBetweenPieces(const FStructure& Structure, int32 FirstPiece, int32 SecondPiece)
	{
		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Structure.GetConnection(Index);

			if ((Joint.PieceA == FirstPiece && Joint.PieceB == SecondPiece)
				|| (Joint.PieceA == SecondPiece && Joint.PieceB == FirstPiece))
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}
}
