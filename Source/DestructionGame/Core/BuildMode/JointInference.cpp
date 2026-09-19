// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/BuildMode/JointInference.h"
#include "Core/Profiles/ConnectionProfiles.h"

namespace BuildMode
{
	/*
	 * The passive resting joint two faces make just by touching, with no fastener.
	 *
	 * Masonry on masonry is mortar: a bed joint (normal ~ vertical, |Z| dominant)
	 * gets the strong GeneralPurposeMortar; a head joint or corner return (normal
	 * ~ horizontal) gets the weak-perpend row, knocked down because a perpend is
	 * the weak link in real masonry.
	 *
	 * If either face is not compression-dominant — timber today, anything else
	 * that carries real tension later — the passive default is DryStone: a
	 * bearing that carries compression and friction but no tension. This is the
	 * fail-safe, least-committal choice: timber is held by explicit fasteners (a
	 * later slice's override), and until one is placed, two timbers merely
	 * resting on each other haven't earned a tensile bond.
	 */
	FConnectionStrength JointForContact(
		const DestructionProfiles::FMaterialProfile& FaceA,
		const DestructionProfiles::FMaterialProfile& FaceB,
		const FVector& InterfaceNormalUnit)
	{
		if (!FaceA.bCompressionDominant || !FaceB.bCompressionDominant)
		{
			return DestructionProfiles::DryStone;
		}

		const double AbsZ = FMath::Abs(InterfaceNormalUnit.Z);
		const bool bBedJoint =
			AbsZ >= FMath::Abs(InterfaceNormalUnit.X) &&
			AbsZ >= FMath::Abs(InterfaceNormalUnit.Y);

		return bBedJoint
			? DestructionProfiles::GeneralPurposeMortar
			: DestructionProfiles::GeneralPurposeMortarPerpend;
	}

	namespace
	{
		/*
		 * How much longer one in-plane half-extent must be than the other before the
		 * piece counts as having a long axis, in cm. A square block has no
		 * orientation, and a hair's difference in a hand-laid pose isn't one either —
		 * so the tolerance is 0.5 cm, half the coordinating grid's mortar joint.
		 */
		constexpr double LongAxisToleranceCm = 0.5;

		/** Which in-plane axis a piece runs along, if either does. */
		enum class ELongAxis : uint8
		{
			None,
			X,
			Y,
		};

		/*
		 * A piece's long axis, or None.
		 *
		 * The guard is written `!(Difference > Tolerance)` rather than
		 * `Difference <= Tolerance` on purpose: every comparison against NaN is
		 * false, so a NaN extent falls into the guard and reads as no long axis —
		 * fail-closed (no readable footprint is never a quoin). An infinite extent
		 * isn't caught this way (Abs(inf - x) > Tolerance is true), but is
		 * unreachable since AddPiece refuses non-finite half-extents; logged in
		 * CURRENT_STATE with the row that would pin an explicit IsFinite guard.
		 */
		ELongAxis ReadLongAxis(const DestructionLayout::FPieceBox& Box)
		{
			const double Difference = Box.ExtentCm.X - Box.ExtentCm.Y;

			if (!(FMath::Abs(Difference) > LongAxisToleranceCm))
			{
				return ELongAxis::None;
			}

			return Difference > 0.0 ? ELongAxis::X : ELongAxis::Y;
		}

		/*
		 * How many of a neighbour's two width-face planes the other piece properly
		 * crosses.
		 *
		 * The neighbour's long axis is the wall's direction; the other in-plane axis
		 * is its width, and its two faces are planes on that axis. A crossing is
		 * strict — `low < plane < high` — so a span that merely ends on a plane
		 * doesn't cross it: a quoin's return finishes flush with one width face and
		 * earns its corner from the other, the one it genuinely passes. Loose
		 * comparisons would let a flush return cross two planes and demote to a
		 * closer.
		 *
		 * Both tests are written in the affirmative (the fail-closed direction, not
		 * `!(x > y)`): every comparison against NaN is false, so a non-finite centre
		 * or extent counts no crossings, and no crossings is Head, the weaker joint.
		 */
		int32 CountWidthPlaneCrossings(
			const DestructionLayout::FPieceBox& Neighbour,
			ELongAxis NeighbourLongAxis,
			const DestructionLayout::FPieceBox& Crossing)
		{
			// X-long means the width faces are the Y ones, and the other way round.
			const int32 WidthIndex = (NeighbourLongAxis == ELongAxis::X) ? 1 : 0;

			const double LowPlane = Neighbour.CentreCm[WidthIndex] - Neighbour.ExtentCm[WidthIndex];
			const double HighPlane = Neighbour.CentreCm[WidthIndex] + Neighbour.ExtentCm[WidthIndex];

			const double SpanLow = Crossing.CentreCm[WidthIndex] - Crossing.ExtentCm[WidthIndex];
			const double SpanHigh = Crossing.CentreCm[WidthIndex] + Crossing.ExtentCm[WidthIndex];

			int32 Crossings = 0;

			if (SpanLow < LowPlane && LowPlane < SpanHigh)
			{
				++Crossings;
			}

			if (SpanLow < HighPlane && HighPlane < SpanHigh)
			{
				++Crossings;
			}

			return Crossings;
		}
	}

	/*
	 * Bed, head or bonded corner, from the two footprints and the normal
	 * (owner-ratified ruling, DESIGN §8 2026-09-15).
	 *
	 * Degenerate input fails closed to Head, diverging from the three-argument
	 * JointForContact above on purpose: that function's bed test, `AbsZ >= |X| &&
	 * AbsZ >= |Y|`, is satisfied by a zero normal, handing the strongest bond to
	 * a contact nobody could measure (the fail-open recorded as CURRENT_STATE's
	 * build-mode item (b)). This path guards the normal first and only then asks
	 * which component dominates.
	 *
	 * Dominance is strict — `AbsZ > AbsX && AbsZ > AbsY`. A 45-degree tie between
	 * vertical and horizontal reads as horizontal, the conservative choice: for a
	 * collinear pair a tie yields the weak perpend rather than full mortar; for a
	 * crossed pair it yields Corner, which carries the same profile a Bed would
	 * — so no tie earns a joint more strength than dominance would have given it.
	 */
	EMasonryContact ClassifyMasonryContact(
		const DestructionLayout::FPieceBox& A,
		const DestructionLayout::FPieceBox& B,
		const FVector& InterfaceNormalUnit)
	{
		/*
		 * Both tests, deliberately: FVector::ContainsNaN also catches infinities
		 * today, but the name only promises NaN — the explicit IsFinite sweep is
		 * what actually pins the infinite normal the fixture feeds in.
		 */
		if (InterfaceNormalUnit.ContainsNaN()
			|| !FMath::IsFinite(InterfaceNormalUnit.X)
			|| !FMath::IsFinite(InterfaceNormalUnit.Y)
			|| !FMath::IsFinite(InterfaceNormalUnit.Z))
		{
			return EMasonryContact::Head;
		}

		const double AbsX = FMath::Abs(InterfaceNormalUnit.X);
		const double AbsY = FMath::Abs(InterfaceNormalUnit.Y);
		const double AbsZ = FMath::Abs(InterfaceNormalUnit.Z);

		// A zero normal names no face, so it names no contact either.
		if (!(AbsX + AbsY + AbsZ > 0.0))
		{
			return EMasonryContact::Head;
		}

		if (AbsZ > AbsX && AbsZ > AbsY)
		{
			return EMasonryContact::Bed;
		}

		/*
		 * A horizontal contact: the pieces' own orientations decide it. Crossed long
		 * axes interlock into a quoin; parallel ones — or any pair where either piece
		 * has no readable long axis — are a head joint.
		 */
		const ELongAxis LongAxisA = ReadLongAxis(A);
		const ELongAxis LongAxisB = ReadLongAxis(B);

		const bool bCrossed =
			LongAxisA != ELongAxis::None &&
			LongAxisB != ELongAxis::None &&
			LongAxisA != LongAxisB;

		if (!bCrossed)
		{
			return EMasonryContact::Head;
		}

		/*
		 * Crossed long axes are necessary but not sufficient — this closes DESIGN
		 * §8's known limit: a header laid beside a stretcher in the same wall line
		 * (a Flemish-bond closer) also crosses and would be credited as a corner
		 * without this refinement.
		 *
		 * The rule: the contact is a Corner exactly when one piece's span along the
		 * other's width axis properly crosses exactly one of that other piece's two
		 * width-face planes. A quoin turns a corner — the wall changes direction, so
		 * the return leaves the neighbour's line on one side and stops at or inside
		 * the other.
		 *
		 * Either piece may be the one that leaves the line, and the function can't
		 * know which argument the caller thinks of as the wall (the snap solver
		 * names the placed piece first, the shed sweep names the lower one), so the
		 * rule is symmetric by OR: A's span against B's width planes, or B's against
		 * A's. Not a convenience — the across-Y quoin (a return off a stretcher's
		 * end whose own long face is flush with that end) crosses nothing in the
		 * first direction and is a corner only by the second.
		 *
		 * Both failures are Head, the weaker answer — fail closed. Crossing both
		 * planes is a closer laid through the wall line; crossing neither is a
		 * header buried in a wall thicker than it is long. Neither turns a corner,
		 * and crediting either overstates the bond by 4.5x on cohesion (0.9 against
		 * the perpend's 0.2).
		 */
		const bool bTurnsACorner =
			CountWidthPlaneCrossings(A, LongAxisA, B) == 1 ||
			CountWidthPlaneCrossings(B, LongAxisB, A) == 1;

		return bTurnsACorner ? EMasonryContact::Corner : EMasonryContact::Head;
	}

	/*
	 * The passive resting joint with the pieces' geometry in hand.
	 *
	 * The material test still wins outright: mortar doesn't bond to a board, so
	 * a timber face is a DryStone bearing at a corner exactly as anywhere else.
	 * Only once both faces are masonry does orientation matter, and a bonded
	 * corner is credited like the bed joint it structurally resembles (DESIGN
	 * §8, 2026-09-15) while the collinear head joint keeps the weak perpend.
	 */
	FConnectionStrength JointForContact(
		const DestructionProfiles::FMaterialProfile& FaceA,
		const DestructionProfiles::FMaterialProfile& FaceB,
		const FVector& InterfaceNormalUnit,
		const DestructionLayout::FPieceBox& BoxA,
		const DestructionLayout::FPieceBox& BoxB)
	{
		if (!FaceA.bCompressionDominant || !FaceB.bCompressionDominant)
		{
			return DestructionProfiles::DryStone;
		}

		return ClassifyMasonryContact(BoxA, BoxB, InterfaceNormalUnit) == EMasonryContact::Head
			? DestructionProfiles::GeneralPurposeMortarPerpend
			: DestructionProfiles::GeneralPurposeMortar;
	}
}
