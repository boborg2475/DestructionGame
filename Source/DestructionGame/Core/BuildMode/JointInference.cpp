// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/BuildMode/JointInference.h"
#include "Core/Profiles/ConnectionProfiles.h"

namespace BuildMode
{
	/*
	 * The passive resting joint two faces make just by touching, with no fastener.
	 *
	 * Masonry on masonry is mortar: a bed joint (interface normal ~ vertical, |Z|
	 * the dominant component) gets the strong GeneralPurposeMortar, while a head
	 * joint or corner return (normal ~ horizontal) gets the weak-perpend row, whose
	 * two bond axes are knocked down because a perpend is the weak link in real
	 * masonry.
	 *
	 * If EITHER face is not compression-dominant — timber today, and anything else
	 * that carries real tension later — the passive default is DryStone: a bearing
	 * that carries compression and friction but no tension. This is the fail-safe,
	 * least-committal choice. Timber pieces are held by explicit fasteners (a later
	 * slice's override); until one is placed, two timbers merely resting on each
	 * other should not be credited with a tensile bond they have not earned, so we
	 * fall back to the pure friction-plus-bearing joint rather than to mortar.
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
		 * piece counts as having a long axis at all, in cm. A square block has no
		 * orientation to agree or disagree with, and a hair's difference in a hand-laid
		 * pose is not an orientation either — so the tolerance is a whole 0.5 cm, half
		 * the coordinating grid's mortar joint.
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
		 * `Difference <= Tolerance` on purpose: every comparison against a NaN is
		 * false, so a box with a NaN extent falls INTO the guard and reads as having
		 * no long axis, which is the fail-closed answer (no readable footprint is
		 * never a quoin). Turning the comparison round would let it out. An INFINITE
		 * extent is NOT caught this way (Abs(inf - x) > Tolerance is true) — it is
		 * unreachable because FStructure::AddPiece refuses non-finite half-extents
		 * and the palette's extents are constants; logged in CURRENT_STATE with the
		 * row that would pin an explicit IsFinite guard.
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
		 * How many of a NEIGHBOUR's two width-face planes the other piece PROPERLY CROSSES.
		 *
		 * Take the neighbour's long axis as the wall's direction; the other in-plane axis is
		 * its WIDTH, and its two faces are planes on that axis. Measure the other piece's
		 * span along that same axis. A crossing is STRICT — `low < plane < high` — so a span
		 * that merely ENDS on a plane does not cross it, which is the whole distinction the
		 * caller turns on: a quoin's return finishes FLUSH with one width face and earns its
		 * corner from the OTHER face, the one it genuinely passes. Write either comparison
		 * loosely and a flush return crosses two planes and is demoted to a closer.
		 *
		 * BOTH TESTS ARE WRITTEN IN THE AFFIRMATIVE, which is the fail-closed direction here
		 * rather than the usual `!(x > y)` shape. Every comparison against a NaN is false, so
		 * a non-finite centre or extent counts NO crossings, and no crossings is not a corner
		 * — the weaker joint. Turning either comparison round would let a NaN manufacture one.
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
	 * Bed, head or bonded corner, from the two footprints and the normal (owner-ratified
	 * ruling, DESIGN §8 2026-09-15).
	 *
	 * DEGENERATE INPUT FAILS CLOSED TO Head, AND THAT DIVERGES FROM THE THREE-ARGUMENT
	 * JointForContact ABOVE ON PURPOSE. That function's bed test is `AbsZ >= |X| && AbsZ
	 * >= |Y|`, which a ZERO normal satisfies — so it answers "bed" and hands the
	 * strongest bond in the library to a contact nobody could measure (the fail-OPEN
	 * recorded as CURRENT_STATE's build-mode item (b)). This path must not inherit that,
	 * so it does not delegate its vertical test: it guards the normal first and only then
	 * asks which component dominates. A joint reading as bonded when nothing proves it is
	 * the expensive direction to be wrong in.
	 *
	 * DOMINANCE IS STRICT — `AbsZ > AbsX && AbsZ > AbsY`. An exact 45 degree tie between
	 * the vertical and a horizontal component therefore reads as HORIZONTAL, so the
	 * orientation test decides it. That is the conservative choice: for the common
	 * collinear pair a tie yields the weak perpend rather than a bed's full mortar, and
	 * for a crossed pair it yields Corner, which carries the same profile a Bed would —
	 * so no tie can earn a joint more strength than dominance would have given it.
	 */
	EMasonryContact ClassifyMasonryContact(
		const DestructionLayout::FPieceBox& A,
		const DestructionLayout::FPieceBox& B,
		const FVector& InterfaceNormalUnit)
	{
		/*
		 * BOTH TESTS, AND DELIBERATELY NOT ONE. FVector::ContainsNaN also reports the
		 * infinities today, but the name only promises NaN; the explicit IsFinite sweep is
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
		 * A horizontal contact: the pieces' own orientations decide it. Crossed long axes
		 * interlock into a quoin; parallel ones — and any pair where at least one piece has
		 * no readable long axis — are a head joint.
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
		 * CROSSED LONG AXES ARE NECESSARY BUT NOT SUFFICIENT, and this is the test that
		 * closes DESIGN §8's KNOWN LIMIT — "a header laid beside a stretcher IN THE SAME
		 * WALL LINE (a Flemish-bond closer) also crosses and will be credited as a corner;
		 * refine with an L-footprint test (the return extends past the neighbour's outer
		 * face) when CR-2's rotate control makes a header placeable". CR-2 makes it
		 * placeable, so the refinement lands with it.
		 *
		 * THE RULE. The contact is a CORNER exactly when one piece's span along the OTHER
		 * piece's width axis PROPERLY CROSSES exactly ONE of that other piece's two
		 * width-face planes. A quoin turns a corner: the wall changes direction, so the
		 * return leaves the neighbour's line on ONE side and stops at or inside the other.
		 *
		 * EITHER PIECE MAY BE THE ONE THAT LEAVES THE LINE, and the function cannot know
		 * which of its two arguments the caller thinks of as the wall — the snap solver
		 * names the placed piece first, the shed sweep names the lower one — so the rule is
		 * symmetric by OR: A's span against B's width planes, or B's against A's. That is
		 * not a convenience; the across-Y quoin (a return off a stretcher's end whose own
		 * long face is flush with that end) crosses NOTHING in the first direction and is
		 * a corner only by the second.
		 *
		 * BOTH FAILURES ARE HEAD, THE WEAKER MASONRY ANSWER — fail closed. Crossing BOTH
		 * planes is a closer laid THROUGH the wall line; crossing NEITHER is a header
		 * buried in a wall thicker than it is long, or two pieces that never leave each
		 * other's line at all. Neither turns a corner, and crediting either with full
		 * mortar overstates the bond by 4.5x on cohesion (0.9 against the perpend's 0.2).
		 */
		const bool bTurnsACorner =
			CountWidthPlaneCrossings(A, LongAxisA, B) == 1 ||
			CountWidthPlaneCrossings(B, LongAxisB, A) == 1;

		return bTurnsACorner ? EMasonryContact::Corner : EMasonryContact::Head;
	}

	/*
	 * The passive resting joint with the pieces' geometry in hand.
	 *
	 * The material test is unchanged and still wins outright: mortar does not bond to a
	 * board, so a timber face is a DryStone bearing at a corner exactly as it is anywhere
	 * else. Only once both faces are masonry does orientation matter, and then a bonded
	 * corner is credited like the bed joint it structurally resembles (DESIGN §8,
	 * 2026-09-15) while the collinear head joint keeps the weak perpend.
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
