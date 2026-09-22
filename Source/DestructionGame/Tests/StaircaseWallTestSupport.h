// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * The staircase void (a staircase-shaped cut through a wall), as geometry and hand arithmetic,
 * shared by the world-free, integration and visual tests that cut it.
 *
 * World-free, so it is kept out of BrickWorldTestSupport.h. Named namespace (not anonymous) and
 * inline functions, for unity and non-unity builds alike.
 *
 * Nothing is imported from the code under test: the grid, the load ladder and the 10000 uu per
 * MPa.cm2 conversion are written out, so a wrong production constant makes these tests disagree.
 */
namespace StaircaseWallTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * A small wall of the kind the game mode lays. Flush, not ragged: a ragged end brick has one
	 * support and a 5.625 cm eccentricity (the same as the corbel), giving a baseline (0.058 at 13
	 * courses) that moves with wall height. A flush end's half bat makes the eccentricity zero.
	 * Thirteen courses is the smallest that holds the staircase with an intact course on top.
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

	/** 7 courses of 10 bricks + 6 of 9 bricks and 2 half bats. Asserted as a fixture precondition. */
	constexpr int32 StaircaseWallPieceCount = 136;

	/*
	 * The grid, re-derived. Running bond offsets alternate courses by half a cell, so the
	 * staircase steps 11.25 cm per course.
	 */
	constexpr double StaircaseBrickPitchCm = 21.5 + 1.0;
	constexpr double StaircaseCoursePitchCm = 6.5 + 1.0;
	constexpr double StaircaseHalfStepCm = StaircaseBrickPitchCm * 0.5;

	/** Centre height of a course, cm. */
	inline double StaircaseCourseZCm(int32 Course)
	{
		return 6.5 * 0.5 + Course * StaircaseCoursePitchCm;
	}

	/** Courses the void cuts through; courses 0 and 12 stay whole. */
	constexpr int32 StaircaseLowestVoidCourse = 1;
	constexpr int32 StaircaseHighestVoidCourse = 11;

	/**
	 * Everything left of this X in this course is cut. The edge steps a half step left per course,
	 * so the run above the void overhangs its bottom by 123.75 cm. The brick centred exactly on the
	 * edge is kept (it is the corbel); nothing else is within 11.25 cm, so no tolerance issue.
	 */
	inline double StaircaseVoidEdgeXCm(int32 Course)
	{
		return (StaircaseHighestVoidCourse + 1 - Course) * StaircaseHalfStepCm;
	}

	/** Which course a laid box is in, from its height. */
	inline int32 StaircaseCourseOf(const FPieceBox& Box)
	{
		return FMath::RoundToInt32((Box.CentreCm.Z - 6.5 * 0.5) / StaircaseCoursePitchCm);
	}

	/** The piece at this spot, or INDEX_NONE. Found by position, since handle order is incidental. */
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
	 * Every piece the void removes, in handle order. Absolute coordinates, so the same hole can be
	 * cut into the 40-course scenario wall by the visual harness.
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

	/** Counted by hand from the diagram in the test. */
	constexpr int32 StaircaseVoidPieceCount = 36;

	/**
	 * Courses whose leftmost brick is corbelled (one bed joint, overhanging by half a brick). Course
	 * 1's still rests on two bricks; course 12 is uncut but corbelled, since course 11 starts further
	 * right.
	 */
	constexpr int32 StaircaseLowestCorbelCourse = 2;
	constexpr int32 StaircaseHighestCorbelCourse = 12;

	constexpr int32 StaircaseCorbelStepCount =
		StaircaseHighestCorbelCourse - StaircaseLowestCorbelCourse + 1;

	/** A course's corbelled brick: the leftmost survivor, on the void edge. */
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

	/** The far end of the wall (last brick of an even course), well clear of the void; must not move. */
	constexpr double StaircaseFarSideXCm = 9 * StaircaseBrickPitchCm;

	/*
	 * The arithmetic, worked independently of the solver.
	 *
	 * A corbelled brick keeps one bed joint, the 10.25 cm strip it shares with the brick below,
	 * whose centroid is 5.625 cm from its centre of mass. Each step carries, about its own patch:
	 *
	 *     its own weight                          1 x 5.625 brick-weight-centimetres
	 *     the corbel above, re-referenced         M_above + 11.25 x F_above
	 *     everything else resting on it           nothing
	 *
	 * The third line: the next brick along sits on two supports (indeterminate, so no moment per
	 * MOMENTS_DESIGN.md) and hands down through the same strip, so it adds force only.
	 *
	 * W = 1.9 g/cm3 x 21.5 x 10.25 x 6.5 = 2.72163125 kg x 980 = 2667.198625 uu (1 N = 100 uu is
	 * already inside that product; do not apply it again). The bed patch is 105.0625 cm2 with
	 * W_v = (4/3) x 5.125^3 = 179.48 cm3, so tension at the opened edge is
	 *
	 *     2667.198625 x (M / 179.4817708 - F / 105.0625) / 10000  MPa
	 *
	 * Against mean flexural bond f_x1 = 0.70 MPa (2026-08-13 re-anchor, Gooch et al. 2023) the
	 * bottom step reads 3.28 on the patch alone.
	 *
	 * But the wall resists as a deep beam (user ruling 2026-08-06, ARCHING_DESIGN.md slice 5): a
	 * vertical section t*D^2/6 through the masonry over the joint. Eleven courses (82.5 cm) give
	 * 11,627.34 cm3, 64.8x the patch, so the bottom rung reads 0.0527.
	 *
	 * The joint gives at the lesser of the two readings. At the top, one course (96.09 cm3) is
	 * shallower than the patch, so course 12 keeps its patch value 0.0083148340. No axial term is
	 * subtracted from the composite reading: the weight is shear on the vertical plane, and
	 * subtracting N/A would read zero. A single course is not a composite, so it uses the patch.
	 */

	constexpr double StaircaseFullBrickWeightUu = 1.9 * 21.5 * 10.25 * 6.5 / 1000.0 * 980.0;

	/** The square half-brick bed patch a corbel keeps. */
	constexpr double StaircaseCorbelBedAreaSqCm = 10.25 * 10.25;

	constexpr double StaircaseCorbelBedHalfCm = 10.25 / 2.0;

	/**
	 * Arm of the corbel's own weight about its patch: a quarter pitch, 5.625 cm. Own weight only;
	 * load from the corbel above arrives on a different patch (see the moment ladder).
	 */
	constexpr double StaircaseCorbelOwnWeightArmCm = StaircaseHalfStepCm / 2.0;

	/** (4/3) x h_u x h_v^2 for the square patch, cm3. */
	constexpr double StaircaseCorbelSectionModulusCm3 =
		(4.0 / 3.0) * StaircaseCorbelBedHalfCm * StaircaseCorbelBedHalfCm * StaircaseCorbelBedHalfCm;

	/**
	 * Unreal force units per MPa.cm2, written out rather than imported so a wrong production
	 * constant (out by 100x) makes the test disagree. DESIGN.md §3.
	 */
	constexpr double StaircaseForceUnitsPerMPaSqCm = 10000.0;

	/*
	 * Mean flexural bond f_x1 for general-purpose mortar, asserted against the profile, not
	 * imported. DESIGN §3 mean basis (2026-08-13; Gooch et al. 2023, ConBuildMat 386:131578).
	 */
	constexpr double StaircaseMortarTensileMPa = 0.70;

	/** EN 1996-1-1 compressive strength for the same joint, asserted, not imported. */
	constexpr double StaircaseMortarCompressiveMPa = 10.0;

	/**
	 * Load per corbel step in brick weights, counted by hand: own weight, all of the corbel above,
	 * and half the next brick along (1 + n + n(n+1)/4 for n steps below the top). Indexed by
	 * Course - StaircaseLowestCorbelCourse, so entry 0 is the bottom.
	 */
	constexpr double StaircaseCorbelLoadBrickWeights[StaircaseCorbelStepCount] =
	{
		38.5, 32.5, 27.0, 22.0, 17.5, 13.5, 10.0, 7.0, 4.5, 2.5, 1.0
	};

	/** Load on one corbel step, uu. */
	inline double StaircasePredictedCorbelForceUu(int32 Course)
	{
		return StaircaseCorbelLoadBrickWeights[Course - StaircaseLowestCorbelCourse]
			* StaircaseFullBrickWeightUu;
	}

	/**
	 * Moment per corbel step in brick-weight-cm, walked by hand from the top:
	 *
	 *     M_12 = 5.625
	 *     M_k  = 5.625 + 11.25 x F_(k+1) + M_(k+1)
	 *
	 * The force grows 38.5-fold top to bottom but the moment 286-fold, since the arm grows too.
	 * Indexed by Course - StaircaseLowestCorbelCourse, so entry 0 is the bottom.
	 */
	constexpr double StaircaseCorbelMomentBrickWeightCm[StaircaseCorbelStepCount] =
	{
		1608.75, 1237.5, 928.125, 675.0, 472.5, 315.0, 196.875, 112.5, 56.25, 22.5, 5.625
	};

	// The top step carries only itself, so its moment must equal the own-weight arm.
	static_assert(
		StaircaseCorbelMomentBrickWeightCm[StaircaseCorbelStepCount - 1]
			== StaircaseCorbelOwnWeightArmCm,
		"the top step of the corbel carries only its own weight, so its moment is that one arm");

	/** Moment on one corbel step, uu.cm. */
	inline double StaircasePredictedCorbelMomentUuCm(int32 Course)
	{
		return StaircaseCorbelMomentBrickWeightCm[Course - StaircaseLowestCorbelCourse]
			* StaircaseFullBrickWeightUu;
	}

	/** Courses of masonry over a step's bed joint: 13 - c (11 at the bottom step, 1 at the top). */
	constexpr int32 StaircaseCoursesOverCorbelJoint(int32 Course)
	{
		return StaircaseCoursesHigh - Course;
	}

	/** Deep-beam section t x D^2 / 6, cm3. Zero for a single course, which is not a composite. */
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
	 * Tension utilisation at the opened edge: the lesser of the patch and deep-beam readings, over
	 * f_x1. ComputeUtilisation returns the worst axis, so tests assert tension governs before relying
	 * on this; at the bottom rung compression reads 0.0098, so tension governs by 5.4x.
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
	 * Compression utilisation at the squeezed edge, against 10 MPa. When the deep beam governs, the
	 * patch is not bending: the bed plane has N/A and the vertical plane M/W_c, and the worst fibre
	 * is the larger of the two, not their sum.
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
	 * Steps over capacity: none. On the patch alone (top step first) the ladder reads 0.0083 ...
	 * 3.2756, with the bottom four over. Against the masonry over each step it reads 0.0083, 0.0223,
	 * ... 0.0527, well under. Still not flat (bottom is 6x top), so a model that dropped the moment
	 * would not match.
	 */
	constexpr int32 StaircasePredictedCorbelJointsOverCapacity = 0;

	/**
	 * The bottom rung: 1608.75 brick-weight-cm against the 11,627.34 cm3 deep-beam section, not the
	 * patch's 179.48 (which would read 3.2756).
	 */
	constexpr double StaircasePredictedWorstCorbelUtilisation = 0.0527187818;

	/** Whether the arithmetic condemns this step of the corbel outright. */
	inline bool StaircaseCorbelIsCondemned(int32 Course)
	{
		return StaircasePredictedCorbelUtilisation(Course) > 1.0;
	}

	/**
	 * The joint between two pieces in either orientation, or INDEX_NONE. Found by pair, since joint
	 * order is incidental. General-purpose; lives here because every caller can include this header.
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
