// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Layout.h"

#include <limits>

namespace DestructionLayout
{
	namespace
	{
		/**
		 * How near two faces have to be to count as the same plane, cm.
		 *
		 * A picometre, and deliberately not a tuning knob: the producer is GENERATIVE, so
		 * this is not the radius of a proximity search. It only has to absorb rounding in
		 * the coordinating grid's own arithmetic, which is exact for a brick whose
		 * dimensions are binary quarters and lands within an ulp or two of 20 cm for one
		 * that is not. Everything refused here sits whole millimetres outside it — the
		 * closest near miss in the suite is a face a quarter of a joint too close — so
		 * nothing depends on where between those two scales the line is drawn.
		 */
		constexpr double ContactToleranceCm = 1.0e-9;

		/**
		 * Whether a box describes a volume at all.
		 *
		 * Written !(x > 0.0) rather than x <= 0.0 so a NaN lands INSIDE the guard instead
		 * of slipping past it — every comparison against NaN is false. That direction
		 * matters more here than usual: a NaN that got through would make every gap and
		 * overlap below NaN, all of which compare false, so the pair would be refused by
		 * accident rather than on purpose and would start being accepted the day a
		 * comparison was written the other way round.
		 *
		 * The extent is checked for finiteness as well as for sign, because +inf > 0.0 is
		 * TRUE and the sign test alone therefore lets it through — and unlike a NaN it is
		 * not refused by accident downstream either. An infinite extent overlaps
		 * infinitely, so that axis is never the axis of separation, and a pair whose other
		 * two axes form a face is ACCEPTED with an infinite interface area. That is
		 * fail-open in the one function whose documented job is to fail closed, so the
		 * extent half of this guard has to match the centre half rather than merely rhyme
		 * with it.
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
		 * whole contract is that the two arrays are parallel — and the mass is derived
		 * from the box that was just built rather than from the spec, so a half bat
		 * cannot end up weighing a full brick.
		 *
		 * The derivation itself is PieceMassKg's and not this function's, because the
		 * brick actor needs the same number for the same piece and a second derivation is
		 * a second place for it to drift.
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

			const int32 Handle =
				Layout.Structure.AddPiece(PieceMassKg(Box, Spec.DensityGramsPerCubicCm), bIsGrounded);
			Layout.Boxes.Add(Box);

			return Handle;
		}

		/**
		 * Offer a pair the wall laid, and keep the joint if the two really share a face.
		 *
		 * The producer knows what it laid, so the pairs it offers are its own neighbours
		 * rather than the result of a search — but whether a given neighbour shares a
		 * FACE is MakeInterface's decision and only its decision. A course end offers a
		 * pair that turns out to be a diagonal, and that pair has to be refused by the
		 * same rule that refuses one anywhere else, not by a second opinion written here.
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
		 * FAIL CLOSED IS NaN HERE, AND ZERO WOULD BE FAIL-OPEN. FStructure::AddPiece
		 * deliberately ACCEPTS a mass of zero — a massless piece is meaningful — so a zero
		 * returned for a degenerate box is laundered into a real piece that weighs nothing,
		 * sits in the graph, routes load and never breaks anything. NaN is refused by the
		 * guard AddPiece already has, !(MassKg >= 0.0) || !IsFinite, and so needs no second
		 * check written anywhere.
		 *
		 * IsUsableBox is the SAME notion of a usable box MakeInterface refuses on, rather
		 * than a narrower one written here: there is one definition of a box this producer
		 * can do anything with, and a mass function with its own weaker opinion would be a
		 * second. It is also what catches the box that is inside out — two negative extents
		 * whose signs CANCEL in the product and would otherwise hand back a perfectly
		 * plausible 2.72163125 kg.
		 *
		 * The density guard is written !(x > 0.0) so a NaN lands inside it, and checked for
		 * finiteness separately because +inf > 0.0 is TRUE. An infinite density would in
		 * fact overflow to an infinite mass and be refused at the door anyway, but refused
		 * BY ACCIDENT is one comparison away from accepted, and RunningBond already rejects
		 * all four degenerate densities in its own spec — a mass function that accepted any
		 * of them would be the more permissive of the two.
		 */
		if (!IsUsableBox(Box)
			|| !(DensityGramsPerCubicCm > 0.0)
			|| !FMath::IsFinite(DensityGramsPerCubicCm))
		{
			return std::numeric_limits<double>::quiet_NaN();
		}

		/*
		 * DENSITY FIRST, AND THE ORDER IS PART OF THE CONTRACT. Density is g/cm3 and
		 * dimensions are cm, so the volume in cm3 divided by 1000 is kilograms — but only
		 * one association of that product is exact for a standard brick. 1.9 x 21.5 x 10.25
		 * x 6.5 / 1000 lands exactly on 2.72163125; (21.5 x 10.25 x 6.5) x 1.9 / 1000 lands
		 * one ulp low at 2.7216312499999997, which would move every full brick in every
		 * wall in the suite. Layout.PieceMass asserts the exact decimal with ==, so the
		 * order is the assertion.
		 *
		 * No force conversion belongs here: DESIGN.md §3's 1 N = 100 uu is a property of
		 * forces, not of masses, and mass goes into Unreal unconverted.
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
		 * return false below leaves a joint that reads as failed rather than one that
		 * reads as fine, which routes a caller who ignored the return value through
		 * ComputeUtilisation's existing area guard instead of handing it a healthy-looking
		 * connection. It is the one thing a refusal must actively do.
		 */
		OutConnection.InterfaceAreaSqCm = 0.0;

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
		 * THE FACE TEST. Two boxes share a face when they fail to overlap on EXACTLY ONE
		 * axis and overlap positively on the other two; the shared face is then the
		 * product of those two overlaps.
		 *
		 * Counting the axes that do NOT overlap, rather than the axes that are separated,
		 * is what lets a zero-thickness joint be a real joint — dry stone has faces
		 * touching with no gap at all, and an axis whose gap is exactly zero has to fall
		 * on the same side of this count as one whose gap is a centimetre. Two such axes
		 * is an edge, three is a corner, and either emitted as a joint would be a spurious
		 * diagonal whose normal is a coin flip between the bed tier and the head tier.
		 */
		double OverlapCm[3];
		int32 SeparationAxis = INDEX_NONE;
		int32 SeparationCount = 0;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double DistanceCm = FMath::Abs(BoxB.CentreCm[Axis] - BoxA.CentreCm[Axis]);
			const double ReachCm = BoxA.ExtentCm[Axis] + BoxB.ExtentCm[Axis];

			OverlapCm[Axis] = ReachCm - DistanceCm;

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
		 * direction between the two centroids, and the sign is read on THAT AXIS ALONE.
		 * Taking it from the whole centre difference is exactly how the centroid normal
		 * gets in: a running-bond bed joint's centre difference is (11.25, 0, 7.5) cm,
		 * whose normalised Z is 0.5547, below cos 45 degrees, so every bed joint in the
		 * wall would classify as a head joint and gravity would resolve as shear against
		 * mortar's cohesion rather than compression against its compressive strength.
		 *
		 * Writing the pair and the normal in the same breath is what makes a normal
		 * inconsistent with its pairing inexpressible. A consistently flipped joint is
		 * harmless — RoleOf turns the normal toward whichever piece it is asked about, and
		 * the accumulation signs the force by which end is loaded — so the pairing is the
		 * only thing that can go wrong here undetected.
		 */
		FVector InterfaceNormal = FVector::ZeroVector;
		InterfaceNormal[SeparationAxis] =
			BoxB.CentreCm[SeparationAxis] > BoxA.CentreCm[SeparationAxis] ? 1.0 : -1.0;

		OutConnection.PieceA = HandleA;
		OutConnection.PieceB = HandleB;
		OutConnection.InterfaceNormal = InterfaceNormal;
		OutConnection.InterfaceAreaSqCm = AreaSqCm;
		OutConnection.Strength = Strength;

		return true;
	}

	bool RunningBond(const FRunningBondSpec& Spec, FBrickLayout& OutLayout)
	{
		/*
		 * A COURSE ONE BRICK WIDE IS NOT A BOND. There is nothing for the course above to
		 * span, so a ragged wall would leave every odd course empty and stack disconnected
		 * bricks with a gap where a course should be — which still solves, and still looks
		 * plausible.
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
		 * A joint as long as the brick leaves a half bat of zero length or less, and the
		 * half-brick offset stops landing inside the brick below. Written as the strict
		 * inequality it needs rather than as its negation, so a NaN thickness is refused
		 * here as well.
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
		 * course up, and running bond offsets alternate courses by half a cell — which is
		 * what makes a brick span two below it and gives the bed joint its 105.0625 cm2
		 * rather than a whole bed face. A half bat is what is left of a brick when a joint
		 * is taken out of it and the remainder halved, so a half bat plus a joint is
		 * exactly the half cell a flush end has to make up.
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
				 * AN ODD COURSE IS OFFSET HALF A CELL, and the two end treatments differ only
				 * in what fills the half cell that leaves at each end: a ragged wall leaves it
				 * empty and steps in, a flush wall fills it with a half bat. The full bricks
				 * between them are in the same places either way.
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
		 * below a piece lands on. Offering the whole course below rather than working out
		 * which two bricks a piece spans keeps the mixed-size case honest — a half bat at
		 * a course end spans one brick, a full brick spans two — and MakeInterface refuses
		 * the pairs that turn out to be diagonals, which is the one place that decision
		 * belongs.
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
