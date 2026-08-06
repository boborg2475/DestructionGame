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
	 * moment, at the same 5.625 cm eccentricity the staircase's own corbel has. A 13-course ragged
	 * wall reads 0.36 of capacity at its own ends before anything has been done to it, and a
	 * fixture whose baseline is a third of the way to failure cannot say the staircase caused
	 * anything. A flush end fills that half cell with a half bat, so the end brick has two
	 * supports, its centre of mass sits at the area-weighted centroid of them, and the
	 * eccentricity is exactly zero. ADestructionGameGameMode lays a flush wall for its scenario,
	 * so this is also the wall the player was actually looking at.
	 *
	 * THIRTEEN COURSES OF TEN, because the picture is of a void ten to fifteen courses tall with
	 * the brickwork above it reaching several bricks out over nothing. Thirteen is the smallest
	 * height that carries the whole staircase AND leaves an intact course on top of it.
	 */
	inline FRunningBondSpec StaircaseWallSpec()
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = 13;
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
	 * whole of the eccentricity, and it is the SAME on every step of the staircase — a brick two
	 * steps out is not levered twice as hard, because it sits on a brick that is itself
	 * overhanging and its own lever arm is still just its own offset. What grows down the
	 * staircase is not the arm but the FORCE on it.
	 *
	 * A full brick is 21.5 x 10.25 x 6.5 cm of clay at 1.9 g/cm3, so
	 * W = 2.72163125 kg x 980 cm/s2 = 2667.198625 Unreal force units — and DESIGN.md §3's
	 * 1 N = 100 uu is already inside that product and must not be applied again. A bed joint of a
	 * corbelled brick is 10.25 x 10.25 = 105.0625 cm2, and bending about the joint's Y axis is
	 * resisted by W_v = (4/3) x 5.125 x 5.125^2 = 179.4817708 cm3. Beam theory on an uncracked
	 * rectangle puts the fibre stress at |M|/W_v -+ N/A, so for a force F the tension at the
	 * opened edge is
	 *
	 *     F x (5.625 / 179.4817708 - 1 / 105.0625) / 10000  MPa
	 *
	 * where 10000 uu per MPa.cm2 is spelled out rather than imported. Against general-purpose
	 * mortar's f_xk1 = 0.10 MPa (EN 1996-1-1 §3.6.3 Table 3.2, and already the number in the
	 * profile) a corbelled brick reaches capacity at F = 45,825.13 uu — 17.181 brick weights.
	 */

	/** 1.9 g/cm3 x 21.5 x 10.25 x 6.5 cm / 1000 = 2.72163125 kg, x 980 cm/s2. */
	constexpr double StaircaseFullBrickWeightUu = 1.9 * 21.5 * 10.25 * 6.5 / 1000.0 * 980.0;

	/** The half-brick bed patch a corbel keeps: 10.25 along the wall by 10.25 through it. */
	constexpr double StaircaseCorbelBedAreaSqCm = 10.25 * 10.25;

	/** Half the bed patch's extent on each in-plane axis; it is square, so they are equal. */
	constexpr double StaircaseCorbelBedHalfCm = 10.25 / 2.0;

	/**
	 * How far the corbelled brick's weight acts from the centroid of the patch that carries it.
	 *
	 * Half of one half-step: the brick spans a full cell and keeps half of it, so its centre is
	 * a quarter of a brick pitch clear of the patch's centre. 22.5 / 4 = 5.625 cm.
	 */
	constexpr double StaircaseCorbelEccentricityCm = StaircaseHalfStepCm / 2.0;

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
	 * What the tension at the opened edge of that step's bed joint comes to, as a fraction of
	 * mortar's flexural bond strength.
	 *
	 * WHICH AXIS GOVERNS IS NOT ASSUMED — see StaircasePredictedCorbelCompressionUtilisation.
	 * ComputeUtilisation returns the WORST of the three axes, so a fixture aimed at bending would
	 * silently measure compression the moment compression happened to be higher, and every test
	 * using these numbers asserts the comparison before it claims anything.
	 */
	inline double StaircasePredictedCorbelUtilisation(int32 Course)
	{
		const double ForceUu = StaircasePredictedCorbelForceUu(Course);

		const double BendingMPa = ForceUu * StaircaseCorbelEccentricityCm
			/ (StaircaseCorbelSectionModulusCm3 * StaircaseForceUnitsPerMPaSqCm);

		const double NormalMPa =
			ForceUu / (StaircaseCorbelBedAreaSqCm * StaircaseForceUnitsPerMPaSqCm);

		return FMath::Max(0.0, BendingMPa - NormalMPa) / StaircaseMortarTensileMPa;
	}

	/** The other edge of the same joint, squeezed rather than opened, against 10 MPa. */
	inline double StaircasePredictedCorbelCompressionUtilisation(int32 Course)
	{
		const double ForceUu = StaircasePredictedCorbelForceUu(Course);

		const double BendingMPa = ForceUu * StaircaseCorbelEccentricityCm
			/ (StaircaseCorbelSectionModulusCm3 * StaircaseForceUnitsPerMPaSqCm);

		const double NormalMPa =
			ForceUu / (StaircaseCorbelBedAreaSqCm * StaircaseForceUnitsPerMPaSqCm);

		return (BendingMPa + NormalMPa) / StaircaseMortarCompressiveMPa;
	}

	/**
	 * How many steps of the corbel the arithmetic puts OVER capacity, and it is 5 of 11.
	 *
	 * The ladder crosses 1.0 between its fifth and sixth rungs — 1.019 and 0.786 — so a single
	 * joint over the line could be a fixture on a knife edge and eleven rungs marching 0.058,
	 * 0.146, 0.262, 0.407, 0.582, 0.786, 1.019, 1.280, 1.572, 1.892, 2.241 cannot.
	 */
	constexpr int32 StaircasePredictedCorbelJointsOverCapacity = 5;

	/** The bottom rung, computed above: 38.5 brick weights at 5.625 cm on a 105.0625 cm2 patch. */
	constexpr double StaircasePredictedWorstCorbelUtilisation = 2.24084777;

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
