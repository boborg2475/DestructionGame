// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/WallCases.h"

/*
 * File-local names sit in the NAMED namespace and carry a WallCases prefix. An anonymous namespace
 * is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many files into
 * one — so two file-local names that collide are a hard compile error between files that never
 * refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionWallCases
{
	/**
	 * THE SHORTEST CLOSING PIECE THIS PRODUCER WILL LAY AT A COURSE'S LEFT END, cm.
	 *
	 * A course laid right-to-left rarely divides exactly, and what is left at the left end is a cut
	 * brick. Below this it is dropped instead, because a two-centimetre sliver is a piece with a
	 * plausible mass and an implausible joint, and it would sit at the FAR end from every corbel and
	 * header these walls exist to measure while being the first thing to break. 4 cm is comfortably
	 * under the 10.25 cm half bat a flush wall really closes with and comfortably over anything that
	 * would be laid by accident.
	 */
	constexpr double WallCasesMinClosingPieceCm = 4.0;

	bool Build(const FWallSpec& Spec, FWallLayout& OutWall)
	{
		using namespace DestructionLayout;

		/*
		 * EMPTIED FIRST AND FILLED LAST. A refused spec must leave a caller who ignored the return
		 * value with nothing, rather than with whatever it laid before it gave up.
		 *
		 * THE GUARDS ARE WRITTEN AS `!(x > 0)` AND NEVER AS `x <= 0`, because every comparison
		 * against a NaN is false: a NaN dimension would slip PAST the second spelling and be laid
		 * as a wall of NaN-sized bricks whose every joint reads as intact.
		 */
		OutWall = FWallLayout();

		/*
		 * A COURSE ONE BRICK WIDE IS NOT A WALL. There is nothing for the course above to span, so
		 * a bond of any kind is meaningless and every case below it measures nothing.
		 */
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

		/*
		 * A joint as long as the brick leaves a half bat of zero length or less, and the half-cell
		 * offset stops landing inside the brick below. Written as the strict inequality it needs
		 * rather than as its negation, so a NaN thickness is refused here as well.
		 */
		if (!(Spec.JointThicknessCm < Spec.BrickSizeCm.X))
		{
			return false;
		}

		if (!(Spec.DensityGramsPerCubicCm > 0.0))
		{
			return false;
		}

		/*
		 * A CORBEL THAT DOES NOT STEP OUT IS NOT A CORBEL, and a NaN step is worse than either. Each
		 * course's right face is pushed out by a multiple of this, so a NaN would make every corbelled
		 * course's face NaN, fail the `while` below on the first comparison, and lay a wall that
		 * silently STOPS at the first stepped course while still reporting success.
		 */
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
		 * THE COORDINATING GRID. A brick plus a joint is one CELL along the wall and one COURSE up,
		 * running bond offsets alternate courses by half a cell, and a half bat is what is left of a
		 * brick when a joint is taken out of it and the remainder halved — so a half bat plus a joint
		 * is exactly the half cell a flush end has to make up.
		 */
		const double CellPitchCm = BrickLengthCm + JointCm;
		const double CoursePitchCm = BrickHeightCm + JointCm;
		const double HalfCellCm = CellPitchCm * 0.5;
		const double HalfBatLengthCm = (BrickLengthCm - JointCm) * 0.5;

		/** Every wall has its left face here, so cell 0 is the first full brick. */
		const double LeftFaceCm = -BrickLengthCm * 0.5;

		/** The right face of the flush rectangle every course is measured from. */
		const double FlushRightFaceCm = (Spec.Cells - 1) * CellPitchCm + BrickLengthCm * 0.5;

		FWallLayout Laid;

		/** Piece handles by course, in the order they were laid, so joints can be offered in pairs. */
		TArray<TArray<int32>> HandlesInCourse;

		for (int32 Course = 0; Course < Spec.CoursesHigh; ++Course)
		{
			const double CentreZCm = BrickHeightCm * 0.5 + Course * CoursePitchCm;

			/*
			 * ONE RULE, NOT FOUR SPECIAL CASES. A course is two numbers — where its right face is,
			 * and how long its rightmost piece is — and running bond, stack bond, a corbel and a
			 * projecting header are all values of those two:
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

			/*
			 * LAID RIGHT TO LEFT, AND THE DIRECTION IS PART OF THE PRODUCER RATHER THAN A TASTE.
			 * A course rarely divides exactly, so somewhere in it there is a cut piece; laying from
			 * the right puts that piece at the LEFT end, far from every corbel and header under
			 * test, instead of in the middle of what is being measured.
			 */
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

				/*
				 * THE BOX'S CENTRE IS THE CENTRE OF MASS, because a brick is a homogeneous solid and
				 * its mass came off that same box. Without it a wall has no eccentricity at all and
				 * every corbel in it reads as though its weight acted through the middle of its
				 * support, which is exactly the state FStructure::HasCompleteGeometry exists to make
				 * askable rather than silent.
				 */
				const int32 Handle = Laid.Layout.Structure.AddPiece(
					PieceMassKg(Box, Spec.DensityGramsPerCubicCm), Course == 0, Box.CentreCm);

				/*
				 * A REFUSED PIECE REFUSES THE WALL. The handle IS the box index, so laying the box
				 * anyway would put every piece after it on somebody else's mass and geometry.
				 */
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
		 * THE PAIRS: neighbours along a course, and every piece of the course below. Offering the
		 * whole course below rather than working out which pieces something spans is what keeps the
		 * mixed-size and corbelled courses honest — MakeInterface refuses the pairs that turn out to
		 * be diagonals, and that decision belongs to it and to nothing written here. Courses two
		 * apart are never offered because a brick shorter than the course pitch cannot reach.
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
	 * Whether one rectangle names this piece.
	 *
	 * STRICT ON THE CELL BOUNDS, and that is the vocabulary rather than a rounding decision: every
	 * brick centre in these walls is an exact multiple of a quarter cell, and every bound written
	 * against them is at least an eighth of a cell clear of any centre.
	 *
	 * A NaN CELL IS IN NO REGION, because every comparison against a NaN is false — which is the
	 * fail-closed direction for a cut: a brick nobody can place names nothing rather than everything.
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

		/*
		 * THE THREE ARRAYS ARE PARALLEL OR THERE IS NO GRID TO RESOLVE AGAINST. A wall whose
		 * (course, cell) is missing for some of its pieces would otherwise name whichever bricks
		 * happened to have one, which is a cut that looks deliberate and is not.
		 */
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
