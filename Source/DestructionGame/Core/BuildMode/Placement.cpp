// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/BuildMode/Placement.h"
#include "Core/Connection.h"
#include "Core/Structure.h"

namespace BuildMode
{
	/*
	 * Turn the best-ranked snap into a live, jointed piece of the layout.
	 *
	 * NearbyBoxes IS THE WHOLE Boxes array, so a candidate's OtherPieceIndex — an index
	 * into NearbyBoxes — is exactly the existing piece's handle: Boxes is kept parallel
	 * to the structure's piece array, so index and handle are the same number. That is
	 * what lets a formed joint name real endpoints without a lookup table.
	 */
	FPlacementResult PlacePiece(
		DestructionLayout::FBrickLayout& InOutLayout,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		bool bGrounded,
		const FSnapSettings& Settings)
	{
		using namespace DestructionLayout;

		/*
		 * Read each existing piece's material back for the solver's joint inference, so
		 * the profile of every joint this placement forms is derived from the two faces'
		 * materials — which requires those materials to have been STORED on placement.
		 * SetPieceMaterial below is what keeps that readable for the next placement.
		 */
		TArray<DestructionProfiles::FMaterialProfile> NearbyMaterials;
		NearbyMaterials.Reserve(InOutLayout.Boxes.Num());
		for (int32 i = 0; i < InOutLayout.Boxes.Num(); ++i)
		{
			const DestructionProfiles::FMaterialProfile* Existing =
				InOutLayout.Structure.GetPiece(i).Material;
			NearbyMaterials.Add(Existing != nullptr ? *Existing : DestructionProfiles::FMaterialProfile());
		}

		const FPieceBox Requested{ RequestedCentreCm, ExtentCm };
		const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
			Requested, Material, InOutLayout.Boxes, NearbyMaterials, Settings);

		// The solver always offers at least the Free fallback, so Candidates[0] exists.
		const FSnapCandidate& Chosen = Candidates[0];

		const FPieceBox Box{ Chosen.CentreCm, ExtentCm };
		const double MassKg = PieceMassKg(Box, Material.DensityGramsPerCubicCm);

		/*
		 * Add the piece FIRST so its handle exists before joints reference it. AddPiece
		 * FAILS CLOSED — a degenerate box gives a NaN mass, which it refuses with
		 * INDEX_NONE — and on refusal NOTHING must touch the layout: no orphan box, no
		 * material, no joint. Boxes is kept strictly parallel to the piece array, so a
		 * single stray push would desync every later placement, which resolves a box
		 * index as a piece handle.
		 */
		const int32 Handle = InOutLayout.Structure.AddPiece(MassKg, bGrounded, Box.CentreCm);
		if (Handle == INDEX_NONE)
		{
			return FPlacementResult{ INDEX_NONE, Chosen.Kind, 0 };
		}

		InOutLayout.Structure.SetPieceMaterial(Handle, &Material);
		InOutLayout.Boxes.Add(Box);

		int32 JointsFormed = 0;
		for (const FFormedJoint& Joint : Chosen.Joints)
		{
			/*
			 * Count only a joint the STRUCTURE accepted. AddConnection fails closed the
			 * same way AddPiece does, and the returned count is the caller's only evidence
			 * the joint is live — a refused one must not inflate it.
			 */
			FConnection Conn;
			if (MakeInterface(
					Handle,
					Box,
					Joint.OtherPieceIndex,
					InOutLayout.Boxes[Joint.OtherPieceIndex],
					Settings.JointThicknessCm,
					Joint.Profile,
					Conn)
				&& InOutLayout.Structure.AddConnection(Conn) != INDEX_NONE)
			{
				++JointsFormed;
			}
		}

		return FPlacementResult{ Handle, Chosen.Kind, JointsFormed };
	}
}
