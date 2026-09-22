// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/BuildMode/JointInference.h"
#include "Core/Profiles/ConnectionProfiles.h"

namespace BuildMode
{
	/*
	 * The passive joint two touching faces make, with no fastener. Masonry on masonry is mortar:
	 * full mortar for a bed joint (|Z| dominant), the weaker perpend row for a horizontal normal.
	 * If either face is not compression-dominant (e.g. timber), the fail-safe is DryStone:
	 * compression and friction, no tension, until an explicit fastener is placed.
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
		// Half-extent difference, cm, before a piece has a long axis: half the grid's mortar joint.
		constexpr double LongAxisToleranceCm = 0.5;

		/** Which in-plane axis a piece runs along, if either does. */
		enum class ELongAxis : uint8
		{
			None,
			X,
			Y,
		};

		/*
		 * A piece's long axis, or None. The guard is `!(Difference > Tolerance)` so a NaN extent
		 * reads as None (fail-closed, never a quoin). An infinite extent is not caught, but
		 * AddPiece refuses those; an explicit IsFinite guard is logged in CURRENT_STATE.
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
		 * How many of the neighbour's two width-face planes (on the axis across its long axis) the
		 * other piece strictly crosses. Strict so a quoin return flush with one face crosses only
		 * the other. Comparisons are affirmative, so NaN counts no crossings, which gives Head.
		 */
		int32 CountWidthPlaneCrossings(
			const DestructionLayout::FPieceBox& Neighbour,
			ELongAxis NeighbourLongAxis,
			const DestructionLayout::FPieceBox& Crossing)
		{
			// X-long means the width faces are on Y, and vice versa.
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
	 * Bed, head or bonded corner, from the two footprints and the normal (DESIGN §8, 2026-09-15).
	 * A degenerate normal fails closed to Head, unlike the three-argument JointForContact, whose
	 * `>=` bed test passes a zero normal (CURRENT_STATE build-mode item (b)). Dominance is strict,
	 * so a 45-degree tie reads as horizontal; that never gives more strength than a Bed would.
	 */
	EMasonryContact ClassifyMasonryContact(
		const DestructionLayout::FPieceBox& A,
		const DestructionLayout::FPieceBox& B,
		const FVector& InterfaceNormalUnit)
	{
		// ContainsNaN only promises NaN; the IsFinite checks pin infinities.
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

		// A zero normal names no face.
		if (!(AbsX + AbsY + AbsZ > 0.0))
		{
			return EMasonryContact::Head;
		}

		if (AbsZ > AbsX && AbsZ > AbsY)
		{
			return EMasonryContact::Bed;
		}

		// Horizontal contact: crossed long axes may be a quoin; parallel or unreadable ones are Head.
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
		 * Crossed axes are not enough: a Flemish-bond closer also crosses (DESIGN §8). A Corner
		 * needs one piece's span to cross exactly one of the other's width-face planes, i.e. the
		 * wall turns. Checked both ways (OR), since callers order the arguments differently and the
		 * across-Y quoin only qualifies in one direction. Crossing both planes (a closer) or neither
		 * (a buried header) is Head; crediting either as a corner overstates cohesion 4.5x.
		 */
		const bool bTurnsACorner =
			CountWidthPlaneCrossings(A, LongAxisA, B) == 1 ||
			CountWidthPlaneCrossings(B, LongAxisB, A) == 1;

		return bTurnsACorner ? EMasonryContact::Corner : EMasonryContact::Head;
	}

	/*
	 * The passive joint using the pieces' geometry. Material wins first (a timber face is always
	 * DryStone). For masonry, Bed and Corner get full mortar and Head the perpend (DESIGN §8).
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
