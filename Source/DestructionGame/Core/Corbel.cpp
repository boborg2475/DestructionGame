// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Corbel.h"

#include "Core/Profiles/MaterialProfiles.h"

/*
 * File-local names sit in the NAMED namespace and carry a Corbel prefix. An anonymous namespace
 * is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many files
 * into one — so two file-local names that collide are a hard compile error between files that
 * never refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionCorbel
{
	/*
	 * --- the brick, spelled out from first principles --------------------------------------
	 *
	 * DESIGN.md's standard UK metric clay brick and the standard 1 cm mortar joint that makes
	 * the coordinating grid 22.5 x 11.25 x 7.5. Every reading the solver suite has taken of a
	 * corbel is a reading of THESE numbers, so they are written out rather than derived from a
	 * running-bond spec that shares none of this producer's stepping.
	 */
	constexpr double CorbelBrickLengthCm = 21.5;
	constexpr double CorbelBrickWidthCm = 10.25;
	constexpr double CorbelBrickHeightCm = 6.5;
	constexpr double CorbelMortarJointCm = 1.0;

	/**
	 * A HALF-OPEN CELL COUNT, AND THE EPSILON IS NOT A TOLERANCE ON GEOMETRY.
	 *
	 * Several step sizes divide the cell pitch exactly — 7.5 goes into 22.5 three times, 11.25
	 * twice — so the quotient below lands on a whole number and a floor of it is one cell either
	 * way depending on the last bit. Nudging up rather than down keeps the leftmost brick inboard
	 * of the base's own left edge, which is the direction that never invents masonry.
	 */
	constexpr double CorbelCellCountEpsilon = 1.0e-9;

	bool Build(const FCorbelSpec& Spec, DestructionLayout::FBrickLayout& OutLayout)
	{
		using namespace DestructionLayout;

		/*
		 * EMPTIED FIRST AND FILLED LAST. A refused spec must leave a caller who ignored the
		 * return value with nothing, rather than with whatever it laid before it gave up.
		 *
		 * THE GUARDS ARE WRITTEN AS `!(x > 0)` AND NEVER AS `x <= 0`, because every comparison
		 * against a NaN is false: a NaN scale would slip PAST the second spelling and be laid as
		 * a structure of NaN-sized bricks whose every joint reads as intact.
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

		/* A brick plus a joint: one coordinating cell along the wall, and one course up. */
		const double CellPitchCm = BrickLengthCm + JointCm;
		const double CoursePitchCm = BrickHeightCm + JointCm;

		const double StepCm = Spec.StepCm * Spec.Scale;
		const double LeftOriginCm = Spec.LeftOriginCm * Spec.Scale;

		const FVector HalfExtentCm(BrickLengthCm / 2.0, BrickWidthCm / 2.0, BrickHeightCm / 2.0);

		const int32 TotalCourses = Spec.BaseCourses + Spec.Steps;

		FBrickLayout Laid;

		/** Piece handles by course, each in ascending X, so `.Last()` is the outer face. */
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

		/*
		 * THE BASE IS IMMOVABLE, WHICH IS `bIsGrounded` ON EVERY ONE OF ITS PIECES. A grounded
		 * piece terminates the flow of load and is a root of the reachability walk on its own
		 * account, so nothing here can topple as a body and the root joint really is the only
		 * failure available.
		 */
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

		/* The base's top course is even-indexed because BaseCourses is odd, so it is unshifted. */
		const double BaseOuterCentreCm = LeftOriginCm + (Spec.BaseCells - 1) * CellPitchCm;

		/*
		 * THE GEOMETRY, WRITTEN AS THE GENERALISATION OF THE PICTURE RATHER THAN AS THE PICTURE.
		 * `claude_plans/CORBEL_CASES.html` builds its fill from an alternating half-cell bond and
		 * a cell count that grows every second course, which is a half-cell step spelled two ways
		 * at once and cannot express any other step size. The equivalent statement that does
		 * generalise is: the arm's OUTERMOST brick advances by exactly `StepCm` per course, and
		 * each course carries as many whole cells inboard of it as fit before the base's left
		 * edge. At StepCm = half a cell the two constructions agree brick for brick.
		 *
		 * A BARE ARM IS ONE BRICK PER COURSE, and that single word is the whole of `bFilled`. It
		 * is a different load path rather than a thinner version of the same one.
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
		 * JOINTS ARE DISCOVERED WITHIN A COURSE AND BETWEEN ADJACENT COURSES ONLY, and every one
		 * of them goes through `MakeInterface` — the same door `RunningBond` uses, so the areas,
		 * the normals and the rectangles are that producer's rather than this one's. Pieces two
		 * courses apart or two cells apart are separated by more than the joint thickness and
		 * `MakeInterface` refuses them, so the restriction is a cost bound and not a second rule.
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
