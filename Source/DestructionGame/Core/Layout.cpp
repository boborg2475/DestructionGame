// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Layout.h"

#include <limits>

namespace DestructionLayout
{
	namespace
	{
		/**
		 * How near two faces count as the same plane, cm. A picometre, not a tuning knob: it only
		 * absorbs rounding in the grid's arithmetic, and everything refused sits whole millimetres out.
		 */
		constexpr double ContactToleranceCm = 1.0e-9;

		/**
		 * Whether a box describes a volume at all. Written !(x > 0.0) so a NaN lands inside the guard
		 * (every NaN comparison is false, so an unguarded NaN would refuse pairs by accident). Extent
		 * finiteness is checked separately because +inf > 0.0 is true and would accept an infinite
		 * interface area, fail-open in a fail-closed function.
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
		 * Place one brick: a box, and the piece carrying its mass. They go in together because the
		 * handle is the box index (FBrickLayout's arrays are parallel), and mass derives from the box
		 * (via PieceMassKg), so a half bat cannot weigh a full brick and there is no driftable second route.
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

			/*
			 * The box centre is the centre of mass (a brick is homogeneous), so a piece never levers
			 * its joint from a different place than its weight acts. Without it the wall has no
			 * eccentricity and every corbel reads as if loaded through the middle of its support.
			 */
			const int32 Handle = Layout.Structure.AddPiece(
				PieceMassKg(Box, Spec.DensityGramsPerCubicCm), bIsGrounded, Box.CentreCm);
			Layout.Boxes.Add(Box);

			return Handle;
		}

		/**
		 * Offer a pair the wall laid, keeping the joint if the two share a face. The producer offers
		 * its own neighbours, but whether one shares a face is MakeInterface's decision alone, so a
		 * course end offering a diagonal is refused by the same rule as any other.
		 */
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
		 * Fail closed is NaN here; zero would be fail-open. AddPiece accepts a mass of zero (a
		 * massless piece is meaningful), so a zero for a degenerate box would launder into a real
		 * load-routing piece; NaN is refused by AddPiece's own guard. IsUsableBox is MakeInterface's
		 * notion, which also catches an inside-out box whose two negative extents cancel. The density
		 * guard is !(x > 0.0) to catch NaN, checked for finiteness separately since +inf > 0.0 is true.
		 */
		if (!IsUsableBox(Box)
			|| !(DensityGramsPerCubicCm > 0.0)
			|| !FMath::IsFinite(DensityGramsPerCubicCm))
		{
			return std::numeric_limits<double>::quiet_NaN();
		}

		/*
		 * Density first, and the order is part of the contract: only 1.9 x 21.5 x 10.25 x 6.5 / 1000
		 * lands exactly on 2.72163125, while multiplying density last lands one ulp low
		 * (2.7216312499999997). Layout.PieceMass asserts the exact decimal with ==. No force
		 * conversion here: DESIGN.md §3's 1 N = 100 uu is a property of forces, and mass is unconverted.
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
		 * Fail closed before anything can refuse: zeroing the area first means every return false
		 * below leaves the joint reading as failed (through ComputeUtilisation's area guard). The
		 * rectangle is zeroed for the opposite reason: zero extents read as healthy ("no bending
		 * capacity measured"), so leaving them would claim a lever arm on an absent face. Both must clear.
		 */
		OutConnection.InterfaceAreaSqCm = 0.0;
		OutConnection.InterfaceCentreCm = FVector::ZeroVector;
		OutConnection.InterfaceHalfExtentCm = FVector::ZeroVector;

		/* AddConnection rejects these too, but a meaningless pairing with perfect geometry is worth
		 * refusing where it is built rather than a step later. */
		if (HandleA < 0 || HandleB < 0 || HandleA == HandleB)
		{
			return false;
		}

		// !(x >= 0.0) rather than x < 0.0, so a NaN thickness is caught rather than compared.
		if (!(JointThicknessCm >= 0.0))
		{
			return false;
		}

		if (!IsUsableBox(BoxA) || !IsUsableBox(BoxB))
		{
			return false;
		}

		/*
		 * The face test. Two boxes share a face when they fail to overlap on exactly one axis and
		 * overlap positively on the other two; the shared face is the product of those two overlaps.
		 *
		 * An overlap is the intersection of the two spans, min(highs) - max(lows), not reach minus
		 * distance: the two agree when the centres are separated but part company once one span
		 * contains the other. Reach minus distance would report a 40 cm pier 20 cm in from a 220 cm
		 * beam bearing over 60 cm, ten hanging in mid air; the intersection reports the pier's own
		 * 40 cm wherever it stands, the only answer a bearing can have.
		 *
		 * The bounds are computed once and the rectangle below reads the same ones. Max/Min are safe
		 * only because IsUsableBox refused every non-finite bound (both discard a NaN operand), so
		 * nothing may be inserted between that guard and this loop. Counting axes that do not overlap
		 * (rather than separated axes) lets a zero-thickness joint be real: dry stone touches with no
		 * gap. Two such axes is an edge, three a corner, either a spurious diagonal.
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
		 * The normal is the separation axis, signed by which handle is B, never the centroid
		 * direction (a centroid normal misclassifies every bed joint as a head joint; see the class
		 * doc). Emitting the pair and normal together makes an inconsistent normal inexpressible; a
		 * consistently flipped joint is harmless since GetJointRole turns the normal per query.
		 */
		FVector InterfaceNormal = FVector::ZeroVector;
		InterfaceNormal[SeparationAxis] =
			BoxB.CentreCm[SeparationAxis] > BoxA.CentreCm[SeparationAxis] ? 1.0 : -1.0;

		/*
		 * The face's own rectangle, emitted with the area it is the shape of. Half-extents are the
		 * in-plane overlaps halved, and 4 x h_u x h_v reproduces the area exactly; deriving them from
		 * the box bounds would be a second, differently-rounded route to the same face. Exactly zero
		 * on the separation axis, as a branch: a face is a rectangle, not a box.
		 *
		 * The centre is the mid-plane of the mortar, not either brick's face: a 1 cm bed has two
		 * contact planes, and taking either brick's face would make the geometry depend on which
		 * handle is A. The mid-plane also degenerates continuously to the coincident faces at zero
		 * thickness. It is the midpoint of the same interval the overlaps are the widths of (on the
		 * separation axis the intersection is empty, and its midpoint is the mortar's mid-plane).
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
		/* A course one brick wide is not a bond: nothing for the course above to span, so a ragged
		 * wall stacks disconnected bricks, which still solves and still looks plausible. */
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

		/* A joint as long as the brick leaves a half bat of zero length or less, so the offset no
		 * longer lands inside the brick below. Strict inequality, not its negation, to refuse NaN. */
		if (!(Spec.JointThicknessCm < Spec.BrickSizeCm.X))
		{
			return false;
		}

		if (!(Spec.DensityGramsPerCubicCm > 0.0))
		{
			return false;
		}

		/*
		 * The coordinating grid. A brick plus a joint is one cell along the wall and one course up;
		 * running bond offsets alternate courses by half a cell, giving the bed joint 105.0625 cm2.
		 * A half bat is a brick minus a joint, halved, so a half bat plus a joint is the half cell a
		 * flush end makes up.
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
				/*
				 * An odd course is offset half a cell; the two end treatments differ only in what
				 * fills the half cell at each end (empty for ragged, a half bat for flush).
				 */
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
		 * The pairs the wall laid: neighbours along a course, and everything in the course below a
		 * piece lands on. Offering the whole course below, rather than computing which bricks a piece
		 * spans, keeps the mixed-size case honest; MakeInterface refuses the diagonals.
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
