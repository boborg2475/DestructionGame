// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ContactSweep.h"

#include "Core/BuildMode/JointInference.h"
#include "Core/Profiles/ConnectionProfiles.h"

namespace DestructionLayout
{
	bool SweepContacts(FBrickLayout& Layout, double JointThicknessCm)
	{
		const int32 NumPieces = Layout.Structure.NumPieces();

		if (Layout.Boxes.Num() != NumPieces || !(JointThicknessCm > 0.0))
		{
			return false;
		}

		/*
		 * Every piece needs a material before any joint is formed, so a refusal happens BEFORE the
		 * first AddConnection rather than half way through the graph.
		 */
		double TallestCm = 0.0;

		for (int32 Piece = 0; Piece < NumPieces; ++Piece)
		{
			if (Layout.Structure.GetPiece(Piece).Material == nullptr)
			{
				return false;
			}

			TallestCm = FMath::Max(TallestCm, 2.0 * Layout.Boxes[Piece].ExtentCm.Z);
		}

		/*
		 * Buckets keyed on the bottom of each box, one bucket per band of the tallest piece's height —
		 * so a piece's own band plus one below and one above hold every box whose Z span could reach
		 * it across a joint. The band is never narrower than a joint.
		 */
		const double BandCm = FMath::Max(TallestCm, JointThicknessCm);
		const auto BucketOf = [BandCm](double ZCm) -> int32
		{
			return FMath::FloorToInt32(ZCm / BandCm);
		};

		TMap<int32, TArray<int32>> Buckets;

		for (int32 Piece = 0; Piece < NumPieces; ++Piece)
		{
			const FPieceBox& Box = Layout.Boxes[Piece];
			Buckets.FindOrAdd(BucketOf(Box.CentreCm.Z - Box.ExtentCm.Z)).Add(Piece);
		}

		for (int32 A = 0; A < NumPieces; ++A)
		{
			const FPieceBox& BoxA = Layout.Boxes[A];
			const double BottomACm = BoxA.CentreCm.Z - BoxA.ExtentCm.Z;
			const double TopACm = BoxA.CentreCm.Z + BoxA.ExtentCm.Z;

			const int32 FirstBucket = BucketOf(BottomACm - TallestCm - JointThicknessCm);
			const int32 LastBucket = BucketOf(TopACm + JointThicknessCm);

			for (int32 Bucket = FirstBucket; Bucket <= LastBucket; ++Bucket)
			{
				const TArray<int32>* Candidates = Buckets.Find(Bucket);

				if (Candidates == nullptr)
				{
					continue;
				}

				for (const int32 B : *Candidates)
				{
					if (B <= A)
					{
						continue;
					}

					int32 Lower = A;
					int32 Upper = B;

					if (Layout.Boxes[B].CentreCm.Z < BoxA.CentreCm.Z)
					{
						Lower = B;
						Upper = A;
					}

					/*
					 * MakeInterface is offered a placeholder profile because it wants one; the profile
					 * that is kept is decided from the normal it computes, below.
					 */
					FConnection Connection;

					if (!MakeInterface(
							Lower, Layout.Boxes[Lower], Upper, Layout.Boxes[Upper], JointThicknessCm,
							DestructionProfiles::GeneralPurposeMortar, Connection))
					{
						continue;
					}

					Connection.Strength = BuildMode::JointForContact(
						*Layout.Structure.GetPiece(Lower).Material,
						*Layout.Structure.GetPiece(Upper).Material,
						Connection.InterfaceNormal.GetSafeNormal(),
						Layout.Boxes[Lower], Layout.Boxes[Upper]);

					if (Layout.Structure.AddConnection(Connection) == INDEX_NONE)
					{
						return false;
					}
				}
			}
		}

		return true;
	}
}
