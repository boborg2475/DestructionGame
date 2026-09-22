// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Layout.h"

#include <limits>

namespace DestructionLayout
{
	namespace
	{
		/** Same-plane tolerance, cm. Absorbs grid rounding only; not a tuning knob. */
		constexpr double ContactToleranceCm = 1.0e-9;

		/**
		 * Whether a box has finite centre and positive finite extents. !(x > 0.0) catches NaN;
		 * IsFinite is separate because +inf > 0.0 would accept an infinite area.
		 */
		bool IsUsableBox(const FPieceBox& Box)
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (!FMath::IsFinite(Box.CentreCm[Axis])
					|| !FMath::IsFinite(Box.ExtentCm[Axis])
					|| !(Box.ExtentCm[Axis] > 0.0))
				{
					return false;
				}
			}

			return true;
		}

		/**
		 * Place one brick: its box and its piece together, since the handle is the box index. Mass
		 * comes from the box via PieceMassKg, so a half bat weighs a half bat.
		 */
		int32 LayBrick(
			FBrickLayout& Layout,
			const FRunningBondSpec& Spec,
			double CentreXCm,
			double CentreZCm,
			double LengthCm,
			bool bIsGrounded)
		{
			FPieceBox Box;
			Box.CentreCm = FVector(CentreXCm, 0.0, CentreZCm);
			Box.ExtentCm = FVector(LengthCm, Spec.BrickSizeCm.Y, Spec.BrickSizeCm.Z) * 0.5;

			// The box centre is the centre of mass; without it the wall has no eccentricity.
			const int32 Handle = Layout.Structure.AddPiece(
				PieceMassKg(Box, Spec.DensityGramsPerCubicCm), bIsGrounded, Box.CentreCm);
			Layout.Boxes.Add(Box);

			return Handle;
		}

		/** Add a joint if MakeInterface finds the pair shares a face. */
		void JoinIfTouching(
			FBrickLayout& Layout,
			const FRunningBondSpec& Spec,
			int32 HandleA,
			int32 HandleB)
		{
			FConnection Connection;

			const bool bTouching = MakeInterface(
				HandleA, Layout.Boxes[HandleA],
				HandleB, Layout.Boxes[HandleB],
				Spec.JointThicknessCm,
				Spec.Strength,
				Connection);

			if (bTouching)
			{
				Layout.Structure.AddConnection(Connection);
			}
		}
	}

	double PieceMassKg(const FPieceBox& Box, double DensityGramsPerCubicCm)
	{
		/*
		 * Fail closed with NaN, not zero: AddPiece accepts zero mass but refuses NaN. IsUsableBox
		 * also catches an inside-out box whose negative extents cancel.
		 */
		if (!IsUsableBox(Box)
			|| !(DensityGramsPerCubicCm > 0.0)
			|| !FMath::IsFinite(DensityGramsPerCubicCm))
		{
			return std::numeric_limits<double>::quiet_NaN();
		}

		/*
		 * Density first is part of the contract: it gives exactly 2.72163125 for a standard brick,
		 * density last is one ulp low (Layout.PieceMass asserts ==). Mass takes no unit conversion.
		 */
		const FVector SizeCm = Box.ExtentCm * 2.0;

		return DensityGramsPerCubicCm * SizeCm.X * SizeCm.Y * SizeCm.Z / 1000.0;
	}

	bool MakeInterface(
		int32 HandleA,
		const FPieceBox& BoxA,
		int32 HandleB,
		const FPieceBox& BoxB,
		double JointThicknessCm,
		const FConnectionStrength& Strength,
		FConnection& OutConnection)
	{
		/*
		 * Clear before any early return: a zero area reads as failed (fail closed), and stale
		 * extents would claim a lever arm on an absent face.
		 */
		OutConnection.InterfaceAreaSqCm = 0.0;
		OutConnection.InterfaceCentreCm = FVector::ZeroVector;
		OutConnection.InterfaceHalfExtentCm = FVector::ZeroVector;

		// AddConnection also rejects these; refuse them here where the pair is built.
		if (HandleA < 0 || HandleB < 0 || HandleA == HandleB)
		{
			return false;
		}

		// !(x >= 0.0) so a NaN thickness is refused.
		if (!(JointThicknessCm >= 0.0))
		{
			return false;
		}

		if (!IsUsableBox(BoxA) || !IsUsableBox(BoxB))
		{
			return false;
		}

		/*
		 * Two boxes share a face when they fail to overlap on exactly one axis; the face is the
		 * product of the other two overlaps. An overlap is min(highs) - max(lows), which stays correct
		 * when one span contains the other (a pier under a wider beam).
		 *
		 * Max/Min silently drop NaN, so this relies on IsUsableBox above; insert nothing between them.
		 * Counting non-overlapping axes lets a zero-thickness (dry stone) joint count; two such axes
		 * is an edge, three a corner, both refused.
		 */
		double LowCm[3];
		double HighCm[3];
		double OverlapCm[3];
		int32 SeparationAxis = INDEX_NONE;
		int32 SeparationCount = 0;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			LowCm[Axis] = FMath::Max(
				BoxA.CentreCm[Axis] - BoxA.ExtentCm[Axis],
				BoxB.CentreCm[Axis] - BoxB.ExtentCm[Axis]);
			HighCm[Axis] = FMath::Min(
				BoxA.CentreCm[Axis] + BoxA.ExtentCm[Axis],
				BoxB.CentreCm[Axis] + BoxB.ExtentCm[Axis]);

			OverlapCm[Axis] = HighCm[Axis] - LowCm[Axis];

			if (!(OverlapCm[Axis] > ContactToleranceCm))
			{
				SeparationAxis = Axis;
				++SeparationCount;
			}
		}

		if (SeparationCount != 1)
		{
			return false;
		}

		if (!FMath::IsNearlyEqual(-OverlapCm[SeparationAxis], JointThicknessCm, ContactToleranceCm))
		{
			return false;
		}

		double AreaSqCm = 1.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Axis != SeparationAxis)
			{
				AreaSqCm *= OverlapCm[Axis];
			}
		}

		/*
		 * The normal is the separation axis, signed toward B; never the centroid direction, which
		 * would misclassify bed joints as head joints.
		 */
		FVector InterfaceNormal = FVector::ZeroVector;
		InterfaceNormal[SeparationAxis] =
			BoxB.CentreCm[SeparationAxis] > BoxA.CentreCm[SeparationAxis] ? 1.0 : -1.0;

		/*
		 * Half-extents are the in-plane overlaps halved, so 4 x h_u x h_v equals the area exactly;
		 * zero on the separation axis. The centre is the mortar's mid-plane, so it does not depend on
		 * which handle is A.
		 */
		FVector InterfaceCentreCm = FVector::ZeroVector;
		FVector InterfaceHalfExtentCm = FVector::ZeroVector;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			InterfaceCentreCm[Axis] = (LowCm[Axis] + HighCm[Axis]) * 0.5;
			InterfaceHalfExtentCm[Axis] =
				Axis == SeparationAxis ? 0.0 : OverlapCm[Axis] * 0.5;
		}

		OutConnection.PieceA = HandleA;
		OutConnection.PieceB = HandleB;
		OutConnection.InterfaceNormal = InterfaceNormal;
		OutConnection.InterfaceAreaSqCm = AreaSqCm;
		OutConnection.InterfaceCentreCm = InterfaceCentreCm;
		OutConnection.InterfaceHalfExtentCm = InterfaceHalfExtentCm;
		OutConnection.Strength = Strength;

		return true;
	}

	bool RunningBond(const FRunningBondSpec& Spec, FBrickLayout& OutLayout)
	{
		// A one-brick course has nothing for the course above to span, so it is not a bond.
		if (Spec.CoursesHigh < 1 || Spec.BricksPerCourse < 2)
		{
			return false;
		}

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!(Spec.BrickSizeCm[Axis] > 0.0))
			{
				return false;
			}
		}

		if (!(Spec.JointThicknessCm >= 0.0))
		{
			return false;
		}

		// A joint as long as the brick leaves no half bat. Written to refuse NaN.
		if (!(Spec.JointThicknessCm < Spec.BrickSizeCm.X))
		{
			return false;
		}

		if (!(Spec.DensityGramsPerCubicCm > 0.0))
		{
			return false;
		}

		/*
		 * A cell is a brick plus a joint. Alternate courses offset by half a cell (bed joint
		 * 105.0625 cm2). A half bat plus a joint fills the half cell at a flush end.
		 */
		const double BrickPitchCm = Spec.BrickSizeCm.X + Spec.JointThicknessCm;
		const double CoursePitchCm = Spec.BrickSizeCm.Z + Spec.JointThicknessCm;
		const double BondOffsetCm = BrickPitchCm * 0.5;
		const double HalfBatLengthCm = (Spec.BrickSizeCm.X - Spec.JointThicknessCm) * 0.5;

		const double LeftEndCm = -Spec.BrickSizeCm.X * 0.5;
		const double RightEndCm = (Spec.BricksPerCourse - 1) * BrickPitchCm + Spec.BrickSizeCm.X * 0.5;

		TArray<TArray<int32>> HandlesInCourse;

		for (int32 Course = 0; Course < Spec.CoursesHigh; ++Course)
		{
			const double CentreZCm = Spec.BrickSizeCm.Z * 0.5 + Course * CoursePitchCm;
			const bool bIsGrounded = Course == 0;

			TArray<int32> Handles;

			if ((Course % 2) == 0)
			{
				for (int32 Brick = 0; Brick < Spec.BricksPerCourse; ++Brick)
				{
					Handles.Add(LayBrick(
						OutLayout, Spec,
						Brick * BrickPitchCm, CentreZCm, Spec.BrickSizeCm.X, bIsGrounded));
				}
			}
			else
			{
				// Odd course, offset half a cell; flush ends get a half bat, ragged ends nothing.
				if (Spec.End == EWallEnd::Flush)
				{
					Handles.Add(LayBrick(
						OutLayout, Spec,
						LeftEndCm + HalfBatLengthCm * 0.5, CentreZCm, HalfBatLengthCm, bIsGrounded));
				}

				for (int32 Brick = 0; Brick < Spec.BricksPerCourse - 1; ++Brick)
				{
					Handles.Add(LayBrick(
						OutLayout, Spec,
						BondOffsetCm + Brick * BrickPitchCm, CentreZCm, Spec.BrickSizeCm.X, bIsGrounded));
				}

				if (Spec.End == EWallEnd::Flush)
				{
					Handles.Add(LayBrick(
						OutLayout, Spec,
						RightEndCm - HalfBatLengthCm * 0.5, CentreZCm, HalfBatLengthCm, bIsGrounded));
				}
			}

			HandlesInCourse.Add(Handles);
		}

		/*
		 * Offer neighbours along each course and every piece in the course below; MakeInterface
		 * refuses non-faces, which keeps mixed brick sizes correct.
		 */
		for (int32 Course = 0; Course < HandlesInCourse.Num(); ++Course)
		{
			const TArray<int32>& Row = HandlesInCourse[Course];

			for (int32 Index = 0; Index + 1 < Row.Num(); ++Index)
			{
				JoinIfTouching(OutLayout, Spec, Row[Index], Row[Index + 1]);
			}

			if (Course == 0)
			{
				continue;
			}

			for (const int32 Below : HandlesInCourse[Course - 1])
			{
				for (const int32 Above : Row)
				{
					JoinIfTouching(OutLayout, Spec, Below, Above);
				}
			}
		}

		return true;
	}
}
