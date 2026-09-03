// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/RigidBlock/RigidBlockBridge.h"

namespace RigidBlockOracle
{
	bool BuildRigidBlockProblem(
		const FStructure& Structure,
		FOracleProblem& OutProblem,
		FString& OutWhyNot)
	{
		static const TSet<int32> None;
		return BuildRigidBlockProblem(Structure, None, OutProblem, OutWhyNot);
	}

	bool BuildRigidBlockProblem(
		const FStructure& Structure,
		const TSet<int32>& ExcludedPieces,
		FOracleProblem& OutProblem,
		FString& OutWhyNot)
	{
		OutProblem = FOracleProblem();
		OutWhyNot.Empty();

		/*
		 * THE ONE SIGNAL THAT SPLITS THE TWO POSES (THREED_DESIGN E3). A 3D-flagged structure is
		 * posed with its full Y geometry — the block's plan-Y, the joint's out-of-plane normal, its
		 * two in-plane half-extents — and the Y-normal refusal below is lifted for it. Every 2D
		 * structure (the default) takes the unchanged path: the Y is dropped and a stray Y-normal is
		 * still refused rather than projected, so a 2D pose stays byte-for-byte what it was.
		 */
		const bool bThreeDimensional = Structure.IsThreeDimensional();

		if (bThreeDimensional)
		{
			OutProblem.Dim = EOracleDim::Dim3D;
		}

		/*
		 * A defaulted centre or rectangle would silently become a lever arm "at the
		 * origin"; the structure's own completeness question is exactly this guard.
		 */
		if (!Structure.HasCompleteGeometry())
		{
			OutWhyNot = TEXT("the structure does not have complete geometry, so honest "
				"lever arms cannot be built");
			return false;
		}

		TArray<int32> BlockOfPiece;
		BlockOfPiece.Init(INDEX_NONE, Structure.NumPieces());

		for (int32 Piece = 0; Piece < Structure.NumPieces(); ++Piece)
		{
			if (Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			if (ExcludedPieces.Contains(Piece))
			{
				/* Deliberately treated as absent — the gate's "remainder without this body". */
				continue;
			}

			const FStructurePiece& Data = Structure.GetPiece(Piece);

			if (!Data.bIsInTheStructure)
			{
				continue;
			}

			BlockOfPiece[Piece] = OutProblem.Blocks.Num();

			/*
			 * The inverse of BlockOfPiece, in block-index order, so a mechanism named over
			 * oracle blocks can name the FStructure piece a caller understands (PROMOTION_DESIGN
			 * §12 D7). Appended in lock-step with Blocks, so PieceOfBlock[b] is piece b's source.
			 */
			OutProblem.PieceOfBlock.Add(Piece);

			FOracleBlock Block;
			Block.MassKg = Data.MassKg;
			Block.CentroidXCm = Data.CentreOfMassCm.X;
			Block.CentroidZCm = Data.CentreOfMassCm.Z;
			Block.bGrounded = Data.bIsGrounded;

			if (bThreeDimensional)
			{
				/* The plan-Y the 2D pose drops — a 3D centroid's third lever arm. */
				Block.CentroidYCm = Data.CentreOfMassCm.Y;
			}

			OutProblem.Blocks.Add(Block);
		}

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Structure.GetConnection(Index);

			/* A joint that has given is out of the structure — latch included. */
			if (Joint.HasGiven())
			{
				continue;
			}

			/*
			 * A joint that touches an excluded body is skipped, not faulted: the body is
			 * deliberately gone, so a live joint to it is expected rather than the tombstone
			 * hole the check below refuses for an INCLUDED piece.
			 */
			if (ExcludedPieces.Contains(Joint.PieceA) || ExcludedPieces.Contains(Joint.PieceB))
			{
				continue;
			}

			if (Joint.PieceA < 0 || Joint.PieceA >= BlockOfPiece.Num()
				|| Joint.PieceB < 0 || Joint.PieceB >= BlockOfPiece.Num()
				|| BlockOfPiece[Joint.PieceA] == INDEX_NONE
				|| BlockOfPiece[Joint.PieceB] == INDEX_NONE)
			{
				/* A live joint on a removed piece is the known tombstone hole. */
				OutWhyNot = FString::Printf(
					TEXT("joint %d is live but names a piece that is not"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			const FOracleBlock& BlockA = OutProblem.Blocks[BlockOfPiece[Joint.PieceA]];
			const FOracleBlock& BlockB = OutProblem.Blocks[BlockOfPiece[Joint.PieceB]];

			/* Two grounded ends constrain nothing the earth does not already absorb. */
			if (BlockA.bGrounded && BlockB.bGrounded)
			{
				continue;
			}

			FVector Normal = Joint.InterfaceNormal;

			if (!Normal.Normalize())
			{
				OutWhyNot = FString::Printf(TEXT("joint %d has a degenerate normal"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			if (!bThreeDimensional && FMath::Abs(Normal.Y) > 1.0e-9)
			{
				OutWhyNot = FString::Printf(
					TEXT("joint %d has an out-of-plane (Y) normal, which a 2D X-Z ")
					TEXT("oracle must refuse rather than project"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			FOracleJoint Out;
			Out.BlockA = BlockOfPiece[Joint.PieceA];
			Out.BlockB = BlockOfPiece[Joint.PieceB];
			Out.NormalX = Normal.X;
			Out.NormalZ = Normal.Z;
			Out.CentreXCm = Joint.InterfaceCentreCm.X;
			Out.CentreZCm = Joint.InterfaceCentreCm.Z;

			if (bThreeDimensional)
			{
				/*
				 * THE 3D POSE: carry the out-of-plane parts the 2D pose has no place for — the
				 * normal's Y component, the patch centre's plan-Y, and BOTH in-plane half-extents.
				 * The pair must be measured in the oracle's OWN frame, so derive the same (U, V) the
				 * assembler will read the patch against and project the interface's per-axis
				 * half-extents onto each: HalfUCm along U, HalfVCm along V. The interface is an
				 * axis-aligned rectangle (AddConnection enforces it) with zero extent on the normal
				 * axis, so each projection picks out exactly one in-plane extent and the posed
				 * rectangle is the real face in the frame the solver measures it in.
				 */
				Out.NormalY = Normal.Y;
				Out.CentreYCm = Joint.InterfaceCentreCm.Y;

				const double N[3] = { Normal.X, Normal.Y, Normal.Z };
				double U[3];
				double V[3];
				DeriveInPlaneAxes(N, U, V);

				const FVector Half = Joint.InterfaceHalfExtentCm;
				Out.HalfUCm = FMath::Abs(Half.X * U[0]) + FMath::Abs(Half.Y * U[1]) + FMath::Abs(Half.Z * U[2]);
				Out.HalfVCm = FMath::Abs(Half.X * V[0]) + FMath::Abs(Half.Y * V[1]) + FMath::Abs(Half.Z * V[2]);
			}
			else
			{
				/*
				 * The in-plane half length: the rectangle's extent on the X-Z axis that is
				 * not the separation axis. The wythe (Y) extent enters through the area
				 * alone, exactly as it does in production's stress arithmetic.
				 */
				Out.HalfLengthCm = FMath::Abs(Normal.Z) >= FMath::Abs(Normal.X)
					? Joint.InterfaceHalfExtentCm.X
					: Joint.InterfaceHalfExtentCm.Z;
			}

			Out.AreaSqCm = Joint.InterfaceAreaSqCm;

			/*
			 * THE LP SOLVES AGAINST THE WEAKEST-LINK MATERIAL PAIRING, not the bare
			 * connection. FStructure::EffectiveJointStrength is the single point that pairs a
			 * joint's connection with its two faces' materials (SHED_PATH.md B3), and it is the
			 * same one the router's GetConnectionUtilisation reads — so the LP row and the
			 * router readout can never disagree about a cross-material joint's capacity. Where a
			 * face names no material it returns the bare connection, so a single-material or
			 * unlabelled joint bridges bit for bit as it did before.
			 */
			Out.Strength = Structure.EffectiveJointStrength(Index);

			/* In lock-step with Joints, so ConnectionOfJoint[j] is joint j's source connection. */
			OutProblem.ConnectionOfJoint.Add(Index);
			OutProblem.Joints.Add(Out);
		}

		return true;
	}

	bool BuildRegionalProblem(
		const FStructure& Structure,
		const TSet<int32>& RegionPieces,
		const TSet<int32>& BoundaryPieces,
		FOracleProblem& OutProblem,
		FString& OutWhyNot)
	{
		OutProblem = FOracleProblem();
		OutWhyNot.Empty();

		/*
		 * SAME SIGNAL, SAME REFUSALS as the whole-structure bridge (THREED_DESIGN E3): a 3D-flagged
		 * structure carries its full Y geometry and lifts the Y-normal refusal; a 2D structure drops
		 * the Y and still refuses a stray Y-normal rather than projecting it. The region pose changes
		 * only WHICH pieces are included and which are earth — never how a joint is measured.
		 */
		const bool bThreeDimensional = Structure.IsThreeDimensional();

		if (bThreeDimensional)
		{
			OutProblem.Dim = EOracleDim::Dim3D;
		}

		if (!Structure.HasCompleteGeometry())
		{
			OutWhyNot = TEXT("the structure does not have complete geometry, so honest "
				"lever arms cannot be built");
			return false;
		}

		/*
		 * INCLUSION IS R UNITED WITH B; everything else is absent exactly as ExcludedPieces are.
		 * A piece in BOTH sets is boundary (grounded) — the conservative reading, since grounding
		 * only adds support. The "included but not boundary" pieces are the interior region R.
		 */
		auto IsIncluded = [&RegionPieces, &BoundaryPieces](int32 Piece)
		{
			return RegionPieces.Contains(Piece) || BoundaryPieces.Contains(Piece);
		};

		TArray<int32> BlockOfPiece;
		BlockOfPiece.Init(INDEX_NONE, Structure.NumPieces());

		for (int32 Piece = 0; Piece < Structure.NumPieces(); ++Piece)
		{
			if (Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			if (!IsIncluded(Piece))
			{
				/* Outside R and B — deliberately absent, exactly as the excluded-pieces path. */
				continue;
			}

			const FStructurePiece& Data = Structure.GetPiece(Piece);

			if (!Data.bIsInTheStructure)
			{
				continue;
			}

			BlockOfPiece[Piece] = OutProblem.Blocks.Num();
			OutProblem.PieceOfBlock.Add(Piece);

			FOracleBlock Block;
			Block.MassKg = Data.MassKg;
			Block.CentroidXCm = Data.CentreOfMassCm.X;
			Block.CentroidZCm = Data.CentreOfMassCm.Z;

			/*
			 * THE ONE DELTA FROM THE EXCLUDED-PIECES FORM: a boundary piece is earth. It is pinned
			 * grounded so it writes no equilibrium rows and never moves; interior region pieces keep
			 * their own grounding, so a real foundation block inside the region stays grounded too.
			 */
			Block.bGrounded = BoundaryPieces.Contains(Piece) || Data.bIsGrounded;

			if (bThreeDimensional)
			{
				/* The plan-Y the 2D pose drops — a 3D centroid's third lever arm. */
				Block.CentroidYCm = Data.CentreOfMassCm.Y;
			}

			OutProblem.Blocks.Add(Block);
		}

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Structure.GetConnection(Index);

			/* A joint that has given is out of the structure — latch included. */
			if (Joint.HasGiven())
			{
				continue;
			}

			/*
			 * A joint touching a piece outside R∪B is skipped, not faulted: that body is
			 * deliberately absent, so a live joint to it is expected rather than the tombstone
			 * hole the check below refuses for an INCLUDED piece.
			 */
			if (!IsIncluded(Joint.PieceA) || !IsIncluded(Joint.PieceB))
			{
				continue;
			}

			if (Joint.PieceA < 0 || Joint.PieceA >= BlockOfPiece.Num()
				|| Joint.PieceB < 0 || Joint.PieceB >= BlockOfPiece.Num()
				|| BlockOfPiece[Joint.PieceA] == INDEX_NONE
				|| BlockOfPiece[Joint.PieceB] == INDEX_NONE)
			{
				/* A live joint on a removed piece is the known tombstone hole. */
				OutWhyNot = FString::Printf(
					TEXT("joint %d is live but names a piece that is not"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			const FOracleBlock& BlockA = OutProblem.Blocks[BlockOfPiece[Joint.PieceA]];
			const FOracleBlock& BlockB = OutProblem.Blocks[BlockOfPiece[Joint.PieceB]];

			/*
			 * Two grounded ends constrain nothing the earth does not already absorb. Now that a
			 * boundary piece is earth, this also skips a boundary-to-boundary joint and leaves an
			 * interior-to-boundary joint as the region's only tie to the ground it hangs from.
			 */
			if (BlockA.bGrounded && BlockB.bGrounded)
			{
				continue;
			}

			FVector Normal = Joint.InterfaceNormal;

			if (!Normal.Normalize())
			{
				OutWhyNot = FString::Printf(TEXT("joint %d has a degenerate normal"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			if (!bThreeDimensional && FMath::Abs(Normal.Y) > 1.0e-9)
			{
				OutWhyNot = FString::Printf(
					TEXT("joint %d has an out-of-plane (Y) normal, which a 2D X-Z ")
					TEXT("oracle must refuse rather than project"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			FOracleJoint Out;
			Out.BlockA = BlockOfPiece[Joint.PieceA];
			Out.BlockB = BlockOfPiece[Joint.PieceB];
			Out.NormalX = Normal.X;
			Out.NormalZ = Normal.Z;
			Out.CentreXCm = Joint.InterfaceCentreCm.X;
			Out.CentreZCm = Joint.InterfaceCentreCm.Z;

			if (bThreeDimensional)
			{
				/*
				 * THE 3D POSE, identical to the whole-structure bridge: carry the out-of-plane
				 * normal component, the patch centre's plan-Y, and both in-plane half-extents
				 * projected onto the oracle's own (U, V) frame the assembler reads the patch in.
				 */
				Out.NormalY = Normal.Y;
				Out.CentreYCm = Joint.InterfaceCentreCm.Y;

				const double N[3] = { Normal.X, Normal.Y, Normal.Z };
				double U[3];
				double V[3];
				DeriveInPlaneAxes(N, U, V);

				const FVector Half = Joint.InterfaceHalfExtentCm;
				Out.HalfUCm = FMath::Abs(Half.X * U[0]) + FMath::Abs(Half.Y * U[1]) + FMath::Abs(Half.Z * U[2]);
				Out.HalfVCm = FMath::Abs(Half.X * V[0]) + FMath::Abs(Half.Y * V[1]) + FMath::Abs(Half.Z * V[2]);
			}
			else
			{
				/*
				 * The in-plane half length: the rectangle's extent on the X-Z axis that is not the
				 * separation axis. The wythe (Y) extent enters through the area alone.
				 */
				Out.HalfLengthCm = FMath::Abs(Normal.Z) >= FMath::Abs(Normal.X)
					? Joint.InterfaceHalfExtentCm.X
					: Joint.InterfaceHalfExtentCm.Z;
			}

			Out.AreaSqCm = Joint.InterfaceAreaSqCm;

			/* The SAME weakest-link material pairing the whole-structure bridge and router read. */
			Out.Strength = Structure.EffectiveJointStrength(Index);

			/* In lock-step with Joints, so ConnectionOfJoint[j] is joint j's source connection. */
			OutProblem.ConnectionOfJoint.Add(Index);
			OutProblem.Joints.Add(Out);
		}

		return true;
	}
}
