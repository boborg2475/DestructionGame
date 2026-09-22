// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Corbel.h"

#include "Core/Profiles/MaterialProfiles.h"

// File-local names use a Corbel prefix in the named namespace, because of unity builds.
namespace DestructionCorbel
{
	// DESIGN.md's standard brick and 1 cm joint: coordinating grid 22.5 x 11.25 x 7.5.
	constexpr double CorbelBrickLengthCm = 21.5;
	constexpr double CorbelBrickWidthCm = 10.25;
	constexpr double CorbelBrickHeightCm = 6.5;
	constexpr double CorbelMortarJointCm = 1.0;

	/**
	 * Nudge for the cell-count floor: steps like 7.5 divide the 22.5 pitch exactly, so the floor
	 * would depend on the last bit. Nudging up keeps the leftmost brick within the base's left edge.
	 */
	constexpr double CorbelCellCountEpsilon = 1.0e-9;

	bool Build(const FCorbelSpec& Spec, DestructionLayout::FBrickLayout& OutLayout)
	{
		using namespace DestructionLayout;

		/*
		 * Emptied first and filled last, so a refusal leaves nothing. Guards are `!(x > 0)`, not
		 * `x <= 0`, so NaN is refused.
		 */
		OutLayout = FBrickLayout();

		if (Spec.BaseCourses < 1 || Spec.BaseCourses % 2 == 0 || Spec.BaseCells < 1
			|| Spec.Steps < 1 || !(Spec.Scale > 0.0) || !(Spec.StepCm > 0.0)
			|| Spec.StepCm >= CorbelBrickLengthCm)
		{
			return false;
		}

		const double BrickLengthCm = CorbelBrickLengthCm * Spec.Scale;
		const double BrickWidthCm = CorbelBrickWidthCm * Spec.Scale;
		const double BrickHeightCm = CorbelBrickHeightCm * Spec.Scale;
		const double JointCm = CorbelMortarJointCm * Spec.Scale;

		const double CellPitchCm = BrickLengthCm + JointCm;
		const double CoursePitchCm = BrickHeightCm + JointCm;

		const double StepCm = Spec.StepCm * Spec.Scale;
		const double LeftOriginCm = Spec.LeftOriginCm * Spec.Scale;

		const FVector HalfExtentCm(BrickLengthCm / 2.0, BrickWidthCm / 2.0, BrickHeightCm / 2.0);

		const int32 TotalCourses = Spec.BaseCourses + Spec.Steps;

		FBrickLayout Laid;

		// Piece handles by course, ascending X, so `.Last()` is the outer face.
		TArray<TArray<int32>> CoursePieces;
		CoursePieces.SetNum(TotalCourses);

		const auto CellsInArmCourse = [&](double OuterCentreCm)
		{
			return FMath::FloorToInt32(
				(OuterCentreCm - LeftOriginCm) / CellPitchCm + CorbelCellCountEpsilon) + 1;
		};

		const auto AddBrick = [&](double CentreXCm, int32 Course, bool bGrounded) -> int32
		{
			FPieceBox Box;
			Box.CentreCm =
				FVector(CentreXCm, 0.0, BrickHeightCm / 2.0 + Course * CoursePitchCm);
			Box.ExtentCm = HalfExtentCm;

			const int32 Piece = Laid.Structure.AddPiece(
				PieceMassKg(Box, DestructionProfiles::ClayBrick.DensityGramsPerCubicCm),
				bGrounded, Box.CentreCm);

			if (Piece == INDEX_NONE)
			{
				return INDEX_NONE;
			}

			Laid.Boxes.Add(Box);
			CoursePieces[Course].Add(Piece);

			return Piece;
		};

		// Every base piece is grounded, so the root joint is the only failure available.
		for (int32 Course = 0; Course < Spec.BaseCourses; ++Course)
		{
			const double CourseOriginCm = LeftOriginCm + (Course % 2 == 1 ? StepCm : 0.0);

			for (int32 Cell = 0; Cell < Spec.BaseCells; ++Cell)
			{
				if (AddBrick(CourseOriginCm + Cell * CellPitchCm, Course, true) == INDEX_NONE)
				{
					return false;
				}
			}
		}

		// BaseCourses is odd, so the base's top course is unshifted.
		const double BaseOuterCentreCm = LeftOriginCm + (Spec.BaseCells - 1) * CellPitchCm;

		/*
		 * The arm's outermost brick advances StepCm per course; a filled course adds whole cells
		 * inboard back to the base's left edge, a bare arm is one brick per course. At half-cell
		 * steps this matches claude_plans/CORBEL_CASES.html brick for brick.
		 */
		for (int32 StepIndex = 1; StepIndex <= Spec.Steps; ++StepIndex)
		{
			const int32 Course = Spec.BaseCourses + StepIndex - 1;
			const double OuterCentreCm = BaseOuterCentreCm + StepIndex * StepCm;

			const int32 Cells = Spec.bFilled ? CellsInArmCourse(OuterCentreCm) : 1;

			for (int32 Cell = Cells - 1; Cell >= 0; --Cell)
			{
				if (AddBrick(OuterCentreCm - Cell * CellPitchCm, Course, false) == INDEX_NONE)
				{
					return false;
				}
			}
		}

		/*
		 * Joints via MakeInterface, within a course and between adjacent courses only. Farther
		 * pairs cannot touch, so the restriction only bounds cost.
		 */
		for (int32 Course = 0; Course < TotalCourses; ++Course)
		{
			for (const int32 A : CoursePieces[Course])
			{
				for (const int32 B : CoursePieces[Course])
				{
					if (A >= B)
					{
						continue;
					}

					FConnection Head;

					if (MakeInterface(
							A, Laid.Boxes[A], B, Laid.Boxes[B], JointCm, Spec.Strength, Head))
					{
						Laid.Structure.AddConnection(Head);
					}
				}

				if (Course + 1 >= TotalCourses)
				{
					continue;
				}

				for (const int32 B : CoursePieces[Course + 1])
				{
					FConnection Bed;

					if (MakeInterface(
							A, Laid.Boxes[A], B, Laid.Boxes[B], JointCm, Spec.Strength, Bed))
					{
						Laid.Structure.AddConnection(Bed);
					}
				}
			}
		}

		OutLayout = MoveTemp(Laid);

		return true;
	}
}
