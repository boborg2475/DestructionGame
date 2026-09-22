// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/WallCases.h"

// File-local names use a WallCases prefix in the named namespace, because of unity builds.
namespace DestructionWallCases
{
	/**
	 * Shortest cut piece laid at a course's left end, cm; anything shorter is dropped as a sliver
	 * that would break first. Well under the 10.25 cm half bat a flush wall closes with.
	 */
	constexpr double WallCasesMinClosingPieceCm = 4.0;

	bool Build(const FWallSpec& Spec, FWallLayout& OutWall)
	{
		using namespace DestructionLayout;

		/*
		 * Emptied first and filled last, so a refusal leaves nothing. Guards are `!(x > 0)`, not
		 * `x <= 0`, so NaN is refused.
		 */
		OutWall = FWallLayout();

		// One cell wide leaves nothing for a bond to span.
		if (Spec.CoursesHigh < 1 || Spec.Cells < 2)
		{
			return false;
		}

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!(Spec.BrickSizeCm[Axis] > 0.0) || !FMath::IsFinite(Spec.BrickSizeCm[Axis]))
			{
				return false;
			}
		}

		if (!(Spec.JointThicknessCm >= 0.0))
		{
			return false;
		}

		// A joint as long as the brick leaves no half bat; written so NaN is refused too.
		if (!(Spec.JointThicknessCm < Spec.BrickSizeCm.X))
		{
			return false;
		}

		if (!(Spec.DensityGramsPerCubicCm > 0.0))
		{
			return false;
		}

		// A NaN corbel step would silently stop the wall at the first stepped course.
		if (Spec.CorbelFromCourse != INDEX_NONE
			&& (!(Spec.CorbelStepCm > 0.0) || !FMath::IsFinite(Spec.CorbelStepCm)))
		{
			return false;
		}

		const double BrickLengthCm = Spec.BrickSizeCm.X;
		const double BrickDepthCm = Spec.BrickSizeCm.Y;
		const double BrickHeightCm = Spec.BrickSizeCm.Z;
		const double JointCm = Spec.JointThicknessCm;

		/*
		 * Coordinating grid: brick plus joint is one cell along and one course up. A half bat plus a
		 * joint is exactly the half cell a flush end makes up.
		 */
		const double CellPitchCm = BrickLengthCm + JointCm;
		const double CoursePitchCm = BrickHeightCm + JointCm;
		const double HalfCellCm = CellPitchCm * 0.5;
		const double HalfBatLengthCm = (BrickLengthCm - JointCm) * 0.5;

		// Left face, so cell 0 is the first full brick.
		const double LeftFaceCm = -BrickLengthCm * 0.5;

		const double FlushRightFaceCm = (Spec.Cells - 1) * CellPitchCm + BrickLengthCm * 0.5;

		FWallLayout Laid;

		// Piece handles by course, in laying order.
		TArray<TArray<int32>> HandlesInCourse;

		for (int32 Course = 0; Course < Spec.CoursesHigh; ++Course)
		{
			const double CentreZCm = BrickHeightCm * 0.5 + Course * CoursePitchCm;

			/*
			 * A course is its right face and its rightmost piece's length:
			 *
			 *      running bond    right face fixed; odd courses close with a half bat at the right
			 *      stack bond      right face fixed; every course closes with a full brick
			 *      corbel          right face steps out once per course from CorbelFromCourse
			 *      header          one course's right face is half a cell further out
			 */
			double RightCm = FlushRightFaceCm;

			double LenCm = (Spec.Bond == EWallBond::Stack || (Course % 2) == 0)
				? BrickLengthCm
				: HalfBatLengthCm;

			if (Spec.ProjectingCourse == Course)
			{
				RightCm += HalfCellCm;
				LenCm = BrickLengthCm;
			}

			if (Spec.CorbelFromCourse != INDEX_NONE && Course >= Spec.CorbelFromCourse)
			{
				RightCm += (Course - Spec.CorbelFromCourse + 1) * Spec.CorbelStepCm;
				LenCm = BrickLengthCm;
			}

			TArray<int32> Handles;

			// Laid right to left so any cut piece lands at the left end, away from corbels and headers.
			while (RightCm - LeftFaceCm >= WallCasesMinClosingPieceCm)
			{
				double LeftCm = RightCm - LenCm;

				if (LeftCm < LeftFaceCm)
				{
					LeftCm = LeftFaceCm;
					LenCm = RightCm - LeftCm;

					if (LenCm < WallCasesMinClosingPieceCm)
					{
						break;
					}
				}

				FPieceBox Box;
				Box.CentreCm = FVector((LeftCm + RightCm) * 0.5, 0.0, CentreZCm);
				Box.ExtentCm = FVector(LenCm, BrickDepthCm, BrickHeightCm) * 0.5;

				// The box centre is the centre of mass; without it the wall has no eccentricity.
				const int32 Handle = Laid.Layout.Structure.AddPiece(
					PieceMassKg(Box, Spec.DensityGramsPerCubicCm), Course == 0, Box.CentreCm);

				// The handle is the box index, so a refused piece refuses the wall.
				if (Handle == INDEX_NONE)
				{
					return false;
				}

				Laid.Layout.Boxes.Add(Box);
				Laid.CourseOf.Add(Course);
				Laid.CellOf.Add(Box.CentreCm.X / CellPitchCm);
				Handles.Add(Handle);

				RightCm = LeftCm - JointCm;
				LenCm = BrickLengthCm;
			}

			HandlesInCourse.Add(MoveTemp(Handles));
		}

		/*
		 * Offer neighbours along a course and every piece of the course below; MakeInterface
		 * rejects pairs that do not touch.
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
							Row[Index], Laid.Layout.Boxes[Row[Index]],
							Row[Other], Laid.Layout.Boxes[Row[Other]],
							JointCm, Spec.Strength, Joint))
					{
						Laid.Layout.Structure.AddConnection(Joint);
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
							Below, Laid.Layout.Boxes[Below],
							Row[Index], Laid.Layout.Boxes[Row[Index]],
							JointCm, Spec.Strength, Joint))
					{
						Laid.Layout.Structure.AddConnection(Joint);
					}
				}
			}
		}

		OutWall = MoveTemp(Laid);

		return true;
	}

	/**
	 * Whether a region contains this piece. Strict on cell bounds (centres sit at least an eighth
	 * of a cell clear); a NaN cell is in no region.
	 */
	static bool WallCasesRegionContains(const FWallRegion& Region, int32 Course, double Cell)
	{
		return Course >= Region.CourseLo
			&& Course <= Region.CourseHi
			&& Cell > Region.CellLo
			&& Cell < Region.CellHi;
	}

	void PiecesInRegions(
		const FWallLayout& Wall,
		TArrayView<const FWallRegion> Regions,
		TArray<int32>& OutPieces)
	{
		OutPieces.Reset();

		// The three arrays must be parallel, or there is no grid to resolve against.
		if (Wall.CourseOf.Num() != Wall.Layout.Boxes.Num()
			|| Wall.CellOf.Num() != Wall.Layout.Boxes.Num())
		{
			return;
		}

		for (int32 Piece = 0; Piece < Wall.Layout.Boxes.Num(); ++Piece)
		{
			if (Wall.Layout.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			for (const FWallRegion& Region : Regions)
			{
				if (WallCasesRegionContains(Region, Wall.CourseOf[Piece], Wall.CellOf[Piece]))
				{
					OutPieces.Add(Piece);

					break;
				}
			}
		}
	}
}
