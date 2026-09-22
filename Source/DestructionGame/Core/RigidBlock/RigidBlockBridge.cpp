// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/RigidBlock/RigidBlockBridge.h"

namespace
{
	/*
	 * Unitless on purpose: applied both to a unit normal's Y and to a Y offset in cm. At 1e-9
	 * either reads as "on the plane up to floating-point noise".
	 */
	constexpr double PlanarTolerance = 1.0e-9;

	/**
	 * Whether the posed problem leaves the X-Z plane, which decides whether the 2D pose is sound.
	 * Asked only over what reaches the LP: given joints, doubly-grounded joints and grounded
	 * blocks are ignored (so an L laid entirely on the earth is planar).
	 *
	 * Two conditions: every posed normal lies in X-Z, and every Y entering an equilibrium row
	 * (ungrounded centroids, posed patch centres) is the same Y. Otherwise a roof on walls at two
	 * Y values would project to a toppling overhang that reads as standing.
	 *
	 * The 2D pose is not the same feasible set: the 3D friction and shear rows are an inscribed
	 * octagon at 0.924x the exact Coulomb cone, so choosing 2D can move a verdict toward standing by
	 * up to 7.6% in shear (DESIGN §8, 2026-09-16). The common-Y test is global, stronger than needed
	 * (see CURRENT_STATE).
	 *
	 * Joints the caller will fault on are skipped. Tests are !(x <= tol) so a non-finite value
	 * answers out of plane: the 3D pose is only slow, a wrong 2D pose is wrong.
	 */
	bool PosedProblemLeavesThePlane(
		const FStructure& Structure,
		const TArray<int32>& BlockOfPiece,
		const RigidBlockOracle::FOracleProblem& Problem)
	{
		// The plane every posed row must sit in, set by the first row.
		bool bHavePlaneY = false;
		double PlaneYCm = 0.0;

		auto SitsOffThePlane = [&bHavePlaneY, &PlaneYCm](double YCm)
		{
			if (!bHavePlaneY)
			{
				bHavePlaneY = true;
				PlaneYCm = YCm;
				return false;
			}

			return !(FMath::Abs(YCm - PlaneYCm) <= PlanarTolerance);
		};

		for (int32 Block = 0; Block < Problem.Blocks.Num(); ++Block)
		{
			if (Problem.Blocks[Block].bGrounded)
			{
				// The earth writes no row, so its Y names no plane.
				continue;
			}

			// Read from the structure: the block's CentroidYCm is filled only once this answer is known.
			if (SitsOffThePlane(Structure.GetPiece(Problem.PieceOfBlock[Block]).CentreOfMassCm.Y))
			{
				return true;
			}
		}

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Structure.GetConnection(Index);

			if (Joint.HasGiven())
			{
				continue;
			}

			if (Joint.PieceA < 0 || Joint.PieceA >= BlockOfPiece.Num()
				|| Joint.PieceB < 0 || Joint.PieceB >= BlockOfPiece.Num()
				|| BlockOfPiece[Joint.PieceA] == INDEX_NONE
				|| BlockOfPiece[Joint.PieceB] == INDEX_NONE)
			{
				// Excluded, or the tombstone hole the caller refuses: unposed either way.
				continue;
			}

			if (Problem.Blocks[BlockOfPiece[Joint.PieceA]].bGrounded
				&& Problem.Blocks[BlockOfPiece[Joint.PieceB]].bGrounded)
			{
				continue;
			}

			FVector Normal = Joint.InterfaceNormal;

			if (!Normal.Normalize())
			{
				continue;
			}

			if (!(FMath::Abs(Normal.Y) <= PlanarTolerance))
			{
				return true;
			}

			if (SitsOffThePlane(Joint.InterfaceCentreCm.Y))
			{
				return true;
			}
		}

		return false;
	}
}

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
		 * The 3D flag is permission to pose 3D, not the pose (THREED_DESIGN E3). A 2D structure drops
		 * Y and refuses a stray Y-normal rather than projecting it. The actual pose is chosen below.
		 */
		const bool bThreeDimensionalPermitted = Structure.IsThreeDimensional();

		// A defaulted centre or rectangle would silently become a lever arm at the origin.
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
				// Treated as absent: the gate's "remainder without this body".
				continue;
			}

			const FStructurePiece& Data = Structure.GetPiece(Piece);

			if (!Data.bIsInTheStructure)
			{
				continue;
			}

			BlockOfPiece[Piece] = OutProblem.Blocks.Num();

			// Inverse of BlockOfPiece, in lock-step with Blocks (PROMOTION_DESIGN §12 D7).
			OutProblem.PieceOfBlock.Add(Piece);

			FOracleBlock Block;
			Block.MassKg = Data.MassKg;
			Block.CentroidXCm = Data.CentreOfMassCm.X;
			Block.CentroidZCm = Data.CentreOfMassCm.Z;
			Block.bGrounded = Data.bIsGrounded;
			OutProblem.Blocks.Add(Block);
		}

		/*
		 * The cheapest sound pose: 3D only when the posed problem leaves the plane. A planar problem's
		 * extra 3D rows carry no information, and 2D is ~37x faster (100-brick wall: 2.5 s vs 94 s)
		 * and uses the exact Coulomb cone rather than the 0.924x octagon. The two readers can disagree
		 * on which joints open inside a falling body (THREED_DESIGN E2b).
		 */
		const bool bThreeDimensional =
			bThreeDimensionalPermitted
			&& PosedProblemLeavesThePlane(Structure, BlockOfPiece, OutProblem);

		if (bThreeDimensional)
		{
			OutProblem.Dim = EOracleDim::Dim3D;

			// The plan-Y the 2D pose drops.
			for (int32 Block = 0; Block < OutProblem.Blocks.Num(); ++Block)
			{
				OutProblem.Blocks[Block].CentroidYCm =
					Structure.GetPiece(OutProblem.PieceOfBlock[Block]).CentreOfMassCm.Y;
			}
		}

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Structure.GetConnection(Index);

			if (Joint.HasGiven())
			{
				continue;
			}

			// A joint to an excluded body is expected, so skipped rather than refused.
			if (ExcludedPieces.Contains(Joint.PieceA) || ExcludedPieces.Contains(Joint.PieceB))
			{
				continue;
			}

			if (Joint.PieceA < 0 || Joint.PieceA >= BlockOfPiece.Num()
				|| Joint.PieceB < 0 || Joint.PieceB >= BlockOfPiece.Num()
				|| BlockOfPiece[Joint.PieceA] == INDEX_NONE
				|| BlockOfPiece[Joint.PieceB] == INDEX_NONE)
			{
				// A live joint on a removed piece: the known tombstone hole.
				OutWhyNot = FString::Printf(
					TEXT("joint %d is live but names a piece that is not"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			const FOracleBlock& BlockA = OutProblem.Blocks[BlockOfPiece[Joint.PieceA]];
			const FOracleBlock& BlockB = OutProblem.Blocks[BlockOfPiece[Joint.PieceB]];

			// Two grounded ends constrain nothing the earth does not already absorb.
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

			/*
			 * Refused rather than projected, tested against the chosen pose, not the flag: if the pose
			 * decision and this test ever disagree, the result is a loud refusal, not a flattened joint.
			 */
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
				 * 3D pose: add the normal's Y, the patch centre's Y, and the half-extents projected onto
				 * the oracle's (U, V) frame. The interface is an axis-aligned rectangle with zero extent
				 * on the normal axis, so each projection picks out one in-plane extent.
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
				// In-plane half length along the non-separation X-Z axis; the Y extent enters via area only.
				Out.HalfLengthCm = FMath::Abs(Normal.Z) >= FMath::Abs(Normal.X)
					? Joint.InterfaceHalfExtentCm.X
					: Joint.InterfaceHalfExtentCm.Z;
			}

			Out.AreaSqCm = Joint.InterfaceAreaSqCm;

			/*
			 * The weakest-link material pairing (SHED_PATH.md B3), the same call the router reads, so
			 * the LP and the readout agree on a cross-material joint's capacity.
			 */
			Out.Strength = Structure.EffectiveJointStrength(Index);

			// In lock-step with Joints: ConnectionOfJoint[j] is joint j's source connection.
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
		 * Same 3D permission and refusals as the whole-structure bridge (THREED_DESIGN E3). The
		 * region changes only which pieces are included and which are earth.
		 */
		const bool bThreeDimensionalPermitted = Structure.IsThreeDimensional();

		if (!Structure.HasCompleteGeometry())
		{
			OutWhyNot = TEXT("the structure does not have complete geometry, so honest "
				"lever arms cannot be built");
			return false;
		}

		/*
		 * Included pieces are R ∪ B; everything else is absent. A piece in both sets is boundary
		 * (grounded), the conservative reading since grounding only adds support.
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

			// Boundary pieces are pinned grounded; interior pieces keep their own grounding.
			Block.bGrounded = BoundaryPieces.Contains(Piece) || Data.bIsGrounded;
			OutProblem.Blocks.Add(Block);
		}

		/*
		 * Same pose rule as the whole-structure bridge, kept in lock-step: otherwise the prover and
		 * the gate would judge the same joint up to 7.6% apart.
		 */
		const bool bThreeDimensional =
			bThreeDimensionalPermitted
			&& PosedProblemLeavesThePlane(Structure, BlockOfPiece, OutProblem);

		if (bThreeDimensional)
		{
			OutProblem.Dim = EOracleDim::Dim3D;

			// The plan-Y the 2D pose drops.
			for (int32 Block = 0; Block < OutProblem.Blocks.Num(); ++Block)
			{
				OutProblem.Blocks[Block].CentroidYCm =
					Structure.GetPiece(OutProblem.PieceOfBlock[Block]).CentreOfMassCm.Y;
			}
		}

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Structure.GetConnection(Index);

			if (Joint.HasGiven())
			{
				continue;
			}

			// A joint to a piece outside R∪B is expected, so skipped rather than refused.
			if (!IsIncluded(Joint.PieceA) || !IsIncluded(Joint.PieceB))
			{
				continue;
			}

			if (Joint.PieceA < 0 || Joint.PieceA >= BlockOfPiece.Num()
				|| Joint.PieceB < 0 || Joint.PieceB >= BlockOfPiece.Num()
				|| BlockOfPiece[Joint.PieceA] == INDEX_NONE
				|| BlockOfPiece[Joint.PieceB] == INDEX_NONE)
			{
				// A live joint on a removed piece: the known tombstone hole.
				OutWhyNot = FString::Printf(
					TEXT("joint %d is live but names a piece that is not"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			const FOracleBlock& BlockA = OutProblem.Blocks[BlockOfPiece[Joint.PieceA]];
			const FOracleBlock& BlockB = OutProblem.Blocks[BlockOfPiece[Joint.PieceB]];

			// Two grounded ends (including boundary-to-boundary) constrain nothing.
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

			// Refused rather than projected, tested against the chosen pose (see the bridge above).
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
				// 3D pose, identical to the whole-structure bridge.
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
				// In-plane half length along the non-separation X-Z axis; the Y extent enters via area only.
				Out.HalfLengthCm = FMath::Abs(Normal.Z) >= FMath::Abs(Normal.X)
					? Joint.InterfaceHalfExtentCm.X
					: Joint.InterfaceHalfExtentCm.Z;
			}

			Out.AreaSqCm = Joint.InterfaceAreaSqCm;

			// The same weakest-link material pairing the whole-structure bridge and router read.
			Out.Strength = Structure.EffectiveJointStrength(Index);

			// In lock-step with Joints: ConnectionOfJoint[j] is joint j's source connection.
			OutProblem.ConnectionOfJoint.Add(Index);
			OutProblem.Joints.Add(Out);
		}

		return true;
	}
}
