// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Layout.h"

#include <limits>

namespace DestructionLayout
{
	namespace
	{
		/**
		 * How near two faces have to be to count as the same plane, cm — a picometre,
		 * deliberately not a tuning knob. The producer is generative, so this only has
		 * to absorb rounding in the coordinating grid's own arithmetic; everything
		 * refused here sits whole millimetres outside it, so nothing depends on exactly
		 * where the line is drawn.
		 */
		constexpr double ContactToleranceCm = 1.0e-9;

		/**
		 * Whether a box describes a volume at all.
		 *
		 * Written !(x > 0.0) rather than x <= 0.0 so a NaN lands inside the guard instead
		 * of slipping past it — every comparison against NaN is false, so an unguarded
		 * NaN would make every gap and overlap compare false and refuse the pair by
		 * accident.
		 *
		 * The extent is also checked for finiteness, because +inf > 0.0 is true and the
		 * sign test alone would let it through: an infinite extent overlaps infinitely,
		 * so that axis is never the separation axis and a pair could be ACCEPTED with an
		 * infinite interface area — fail-open in the one function whose job is fail-closed.
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
		 * Place one brick: a box, and the piece that carries its mass.
		 *
		 * The two go in together because the handle IS the box index — FBrickLayout's
		 * arrays are parallel — and mass is derived from the box just built rather than
		 * from the spec, so a half bat cannot end up weighing a full brick. The
		 * derivation itself is PieceMassKg's, not this function's, so the brick actor
		 * never has a second, driftable route to the same number.
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
			 * The box's centre is the centre of mass — a brick is homogeneous and its mass
			 * came off this same box a line above — so a piece never weighs one brick while
			 * levering its joint from where a different one sits.
			 *
			 * Without this the wall has no eccentricity at all and reads perfectly healthy
			 * while every corbel in it is answered as though its weight acted through the
			 * middle of its support. FStructure::HasCompleteGeometry makes that state askable.
			 */
			const int32 Handle = Layout.Structure.AddPiece(
				PieceMassKg(Box, Spec.DensityGramsPerCubicCm), bIsGrounded, Box.CentreCm);
			Layout.Boxes.Add(Box);

			return Handle;
		}

		/**
		 * Offer a pair the wall laid, and keep the joint if the two really share a face.
		 *
		 * The producer offers its own neighbours rather than the result of a search, but
		 * whether one shares a FACE is MakeInterface's decision alone — a course end can
		 * offer a diagonal, and it must be refused by the same rule that refuses any other.
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
		 * FAIL CLOSED IS NaN HERE; ZERO WOULD BE FAIL-OPEN. AddPiece deliberately accepts a
		 * mass of zero — a massless piece is meaningful — so a zero returned for a
		 * degenerate box would be laundered into a real piece that routes load and never
		 * breaks anything. NaN is refused by AddPiece's own guard, so nothing else needs
		 * to check for it.
		 *
		 * IsUsableBox is the same notion MakeInterface refuses on, not a narrower one
		 * written here — it also catches a box that is inside out, where two negative
		 * extents cancel in the product and would otherwise hand back a plausible mass.
		 *
		 * The density guard is !(x > 0.0) so a NaN lands inside it, checked for
		 * finiteness separately since +inf > 0.0 is true; an infinite density would
		 * overflow to an infinite mass anyway, but refused by accident is one comparison
		 * from accepted.
		 */
		if (!IsUsableBox(Box)
			|| !(DensityGramsPerCubicCm > 0.0)
			|| !FMath::IsFinite(DensityGramsPerCubicCm))
		{
			return std::numeric_limits<double>::quiet_NaN();
		}

		/*
		 * DENSITY FIRST, AND THE ORDER IS PART OF THE CONTRACT. Only one association of
		 * the product is exact for a standard brick: 1.9 x 21.5 x 10.25 x 6.5 / 1000 lands
		 * exactly on 2.72163125, while (21.5 x 10.25 x 6.5) x 1.9 / 1000 lands one ulp low
		 * at 2.7216312499999997 — enough to move every full brick in the suite.
		 * Layout.PieceMass asserts the exact decimal with ==, so the order is the assertion.
		 *
		 * No force conversion belongs here: DESIGN.md §3's 1 N = 100 uu is a property of
		 * forces, not masses, and mass goes into Unreal unconverted.
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
		 * FAIL CLOSED BEFORE ANYTHING CAN REFUSE. Zeroing the area first means every
		 * return false below leaves a joint reading as failed, routing an ignored return
		 * value through ComputeUtilisation's existing area guard.
		 *
		 * The rectangle is zeroed for the OPPOSITE reason: a zero area reads as failed,
		 * but zero extents read as healthy ("no bending capacity was ever measured"), so
		 * a refusal that left them alone would claim a lever arm on a face that isn't
		 * there. Both halves must be cleared; neither substitutes for the other.
		 */
		OutConnection.InterfaceAreaSqCm = 0.0;
		OutConnection.InterfaceCentreCm = FVector::ZeroVector;
		OutConnection.InterfaceHalfExtentCm = FVector::ZeroVector;

		/*
		 * FStructure::AddConnection rejects these too, but a joint whose pairing is
		 * meaningless while its geometry looks perfect is worth refusing where it is
		 * built rather than a step later.
		 */
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
		 * THE FACE TEST. Two boxes share a face when they fail to overlap on exactly one
		 * axis and overlap positively on the other two; the shared face is the product of
		 * those two overlaps.
		 *
		 * AN OVERLAP IS THE INTERSECTION OF THE TWO SPANS, min(highs) - max(lows), and NOT
		 * reach minus distance — the two agree whenever the centres are separated, but
		 * part company the moment one span wholly contains the other. Reach minus distance
		 * grows as the smaller piece slides inboard, so a 40 cm pier standing 20 cm in from
		 * the end of a 220 cm beam would be reported bearing over 60 cm, ten of it hanging
		 * in mid air at each end on an area that divides a force. The intersection reports
		 * the pier's own 40 cm wherever it stands, the only answer a bearing can have — true
		 * of a padstone, a bearing plate or a lintel on a wide pier, never of a same-sized wall.
		 *
		 * The bounds are computed once here and the rectangle below reads the same ones, so
		 * the emitted centre and width can never disagree about which face they describe.
		 * Max and Min are safe only because IsUsableBox already refused every non-finite
		 * bound; both DISCARD a NaN operand rather than propagate it, so nothing may be
		 * inserted between that guard and this loop.
		 *
		 * Counting axes that do NOT overlap, rather than axes that are separated, is what
		 * lets a zero-thickness joint be real — dry stone has faces touching with no gap,
		 * and a zero gap must count the same as a centimetre one. Two such axes is an edge,
		 * three a corner, and either emitted as a joint would be a spurious diagonal whose
		 * normal is a coin flip between the bed tier and the head tier.
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
		 * THE NORMAL IS THE AXIS OF SEPARATION, SIGNED BY WHICH HANDLE IS B — never the
		 * centroid direction, and the sign is read on that axis alone (see the class doc
		 * for why a centroid normal misclassifies every bed joint as a head joint).
		 *
		 * Writing the pair and the normal together is what makes a normal inconsistent
		 * with its pairing inexpressible; a consistently flipped joint is harmless since
		 * GetJointRole turns the normal toward whichever piece it is asked about.
		 */
		FVector InterfaceNormal = FVector::ZeroVector;
		InterfaceNormal[SeparationAxis] =
			BoxB.CentreCm[SeparationAxis] > BoxA.CentreCm[SeparationAxis] ? 1.0 : -1.0;

		/*
		 * THE FACE'S OWN RECTANGLE, EMITTED WITH THE AREA IT IS THE SHAPE OF. Half-extents
		 * are the same in-plane overlaps halved, and 4 x h_u x h_v reproduces the area
		 * exactly (halving and quadrupling are exact in binary). Deriving them from the box
		 * bounds instead would be a second, differently-rounded route to the same face —
		 * exactly the disagreement emitting the two together exists to make inexpressible.
		 * Exactly zero on the separation axis, as a branch rather than fallen into: a face
		 * is a rectangle, not a box.
		 *
		 * THE CENTRE IS THE MID-PLANE OF THE MORTAR, NOT EITHER BRICK'S FACE — a 1 cm bed
		 * has two contact planes, so "the contact plane" is ambiguous for a mortared joint.
		 * Taking either brick's face would make the geometry depend on WHICH HANDLE IS A,
		 * when swapping the handles should swap the normal and nothing else. The mid-plane
		 * also degenerates continuously: at zero thickness the two faces coincide on it.
		 *
		 * It is the midpoint of the same interval the overlaps above are the widths of. On
		 * an in-plane axis [Low, High] is the shared span; on the separation axis the
		 * intersection is EMPTY — Low the near face of the further box, High the near face
		 * of the nearer one — and the midpoint of that empty interval is the mortar's
		 * mid-plane.
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
		/*
		 * A course one brick wide is not a bond: there is nothing for the course above to
		 * span, so a ragged wall would stack disconnected bricks with a gap where a course
		 * should be — which still solves, and still looks plausible.
		 */
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

		/*
		 * A joint as long as the brick leaves a half bat of zero length or less, so the
		 * half-brick offset no longer lands inside the brick below. Written as the strict
		 * inequality it needs, not its negation, so a NaN thickness is refused too.
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
		 * THE COORDINATING GRID. A brick plus a joint is one cell along the wall and one
		 * course up; running bond offsets alternate courses by half a cell, which is what
		 * gives the bed joint its 105.0625 cm2 rather than a whole bed face. A half bat is
		 * a brick minus a joint, halved, so a half bat plus a joint is exactly the half
		 * cell a flush end has to make up.
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
				 * An odd course is offset half a cell; the two end treatments differ only in
				 * what fills the half cell left at each end — empty for ragged, a half bat for
				 * flush. The full bricks between them are in the same places either way.
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
		 * THE PAIRS THE WALL LAID: neighbours along a course, and everything in the course
		 * below a piece lands on. Offering the whole course below, rather than working out
		 * which bricks a piece spans, keeps the mixed-size case honest; MakeInterface
		 * refuses the pairs that turn out to be diagonals.
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
