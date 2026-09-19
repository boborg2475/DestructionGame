// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/RigidBlock/RigidBlockBridge.h"

namespace
{
	/*
	 * The house epsilon this file already measures an out-of-plane normal against. UNITLESS on
	 * purpose: it is applied both to a UNIT normal's Y component and to a Y offset in cm, and at
	 * 1e-9 either reading is "exactly on the plane up to floating-point noise" — a 1e-9 cm offset
	 * is no more a lever arm than a 1e-9 normal component is a tilt.
	 */
	constexpr double PlanarTolerance = 1.0e-9;

	/**
	 * DOES THE POSED PROBLEM LEAVE THE X-Z PLANE — the question that decides which pose is sound.
	 *
	 * Asked over what the bridge actually poses, never over NumConnections(). A joint that has
	 * given is out of the structure, a joint with two grounded ends constrains nothing the earth
	 * does not already absorb, and a grounded block writes no equilibrium rows — none of them
	 * reaches the LP, so none of them can make it three dimensional. An L laid entirely on the
	 * earth is the case that makes the distinction pay: every one of its Y-normal head joints is
	 * earth-to-earth, so the problem it poses is planar.
	 *
	 * TWO CONDITIONS, AND THE SECOND IS EASY TO MISS: in-plane normals alone are not enough.
	 * Moments are taken about each block's own centroid with lever arms to its contacts, so a
	 * posed row whose Y differs from its neighbours' generates a genuine out-of-plane moment
	 * demand even when every normal lies in X-Z — a roof bearing on walls at two different Y
	 * values is exactly that, and projecting it onto one plane makes a toppling overhang read as
	 * standing. So the problem is planar only when every Y that enters an equilibrium row — each
	 * ungrounded block's centroid and each posed joint's patch centre — is the same Y. Under that
	 * condition the out-of-plane force row and the two out-of-plane moment rows are linear
	 * combinations of the in-plane ones (M_x = y0 * SumFz, M_z = -y0 * SumFx) and carry no
	 * information, and the patch's own Y half-extent is symmetric about that plane. The bridge
	 * poses no applied forces, so gravity — in-plane by definition — is the only load.
	 *
	 * "Sound" does not mean the same feasible set. Every 3D-feasible force system projects to a
	 * 2D-feasible one (drop each corner's Y shear, sum the Y pairs), but the converse fails: the
	 * 3D friction pyramid and shear ceiling are an inscribed k=8 octagon
	 * (ThreeDPyramidInscribeFactor, cos(pi/8) = 0.924), so pure in-plane shear is capped at 0.924x
	 * the exact Coulomb cone the 2D rows carry. Posing a planar problem in 2D therefore chooses the
	 * exact cone every sweep pin is anchored on, and can only move a verdict toward standing, by at
	 * most that 7.6% shear band — a ruling with a cost (DESIGN §8, 2026-09-16), not a no-op.
	 *
	 * The common-Y test is global (one plane for the whole problem), which is sufficient but
	 * stronger than the rows need: the sound condition is really per ungrounded block (its
	 * centroid Y equals every posed patch centre it touches). Two independent straight walls at
	 * different Y in one flagged structure pose 3D under this test though each is planar;
	 * CURRENT_STATE carries the refinement.
	 *
	 * A joint the caller's own loop will fault on — an endpoint with no block, a normal
	 * Normalize() refuses — is skipped here rather than answered, since that bridge call returns
	 * false with an emptied problem. Both tests are written !(x <= tol) rather than x > tol so a
	 * non-finite normal or centre answers out of plane, the expensive-and-sound side: posing
	 * something the 2D oracle cannot express would be a plausible number with wrong statics, where
	 * paying for the 3D pose is only slow.
	 */
	bool PosedProblemLeavesThePlane(
		const FStructure& Structure,
		const TArray<int32>& BlockOfPiece,
		const RigidBlockOracle::FOracleProblem& Problem)
	{
		/* The plane every posed row must sit in, unnamed until the first row names it. */
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
				/* The earth balances by definition — it writes no row, so its Y names no plane. */
				continue;
			}

			/*
			 * Read from the STRUCTURE, not from the block: the block's own CentroidYCm is filled
			 * after this answer is known, precisely so the 2D pose leaves it at zero as it always did.
			 */
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
				/* Absent by inclusion, or the tombstone hole the caller refuses — either way unposed. */
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
		 * THE FLAG IS THE PERMISSION TO POSE 3D, NOT THE POSE (THREED_DESIGN E3). A 3D-flagged
		 * structure may be posed with its full Y geometry — the block's plan-Y, the joint's
		 * out-of-plane normal, its two in-plane half-extents — and the Y-normal refusal below is
		 * lifted for it. Every 2D structure (the default) takes the unchanged path: Y is dropped
		 * and a stray Y-normal is still refused rather than projected, so a 2D-flagged structure
		 * that has acquired an out-of-plane joint stays loudly refused rather than quietly
		 * promoted. Which pose is actually built is decided below, from the joints this bridge poses.
		 */
		const bool bThreeDimensionalPermitted = Structure.IsThreeDimensional();

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
			OutProblem.Blocks.Add(Block);
		}

		/*
		 * THE CHEAPEST SOUND POSE. Permitted to pose 3D, this bridge does so only when the problem
		 * needs it: when everything it poses lies in one X-Z plane (see the predicate above), the
		 * 3D pose's out-of-plane force row and its two out-of-plane moment rows carry no information
		 * the in-plane rows do not already carry, and the 2D pose answers ~37x faster (a 100-brick
		 * wall's cold Run: 2.5 s against 94 s).
		 *
		 * The two poses are not interchangeable: the 3D friction and shear-ceiling rows are an
		 * inscribed octagon, 0.924x the exact Coulomb cone the 2D rows carry, so the 2D pose is the
		 * more accurate one and can only move a shear-critical verdict toward standing. One
		 * consequence is visible in the mechanism reader: measured 2026-09-16, the two formulations
		 * agreed on the verdict of a free-falling wall and disagreed on which joints opened — the 3D
		 * reader severed eight joints inside a body the 2D reader dropped whole. Choosing one pose
		 * per problem makes a planar build's break sequence the 2D one by construction; what the 3D
		 * reader does inside a free-falling body remains an open question (THREED_DESIGN E2b).
		 */
		const bool bThreeDimensional =
			bThreeDimensionalPermitted
			&& PosedProblemLeavesThePlane(Structure, BlockOfPiece, OutProblem);

		if (bThreeDimensional)
		{
			OutProblem.Dim = EOracleDim::Dim3D;

			/* The plan-Y the 2D pose drops — a 3D centroid's third lever arm. */
			for (int32 Block = 0; Block < OutProblem.Blocks.Num(); ++Block)
			{
				OutProblem.Blocks[Block].CentroidYCm =
					Structure.GetPiece(OutProblem.PieceOfBlock[Block]).CentreOfMassCm.Y;
			}
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

			/*
			 * Refused rather than projected, read against the POSE (bThreeDimensional) rather than
			 * the flag alone, so a planar pose chosen under 3D permission still refuses a Y-normal
			 * here — if the pose decision and this test ever disagreed about which joints are
			 * posed, the answer is a loud refusal rather than a Y-normal joint flattened into X-Z.
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
				 * THE 3D POSE: carry the out-of-plane parts the 2D pose has no place for — the
				 * normal's Y component, the patch centre's plan-Y, and both in-plane half-extents,
				 * measured in the oracle's own (U, V) frame (the same one the assembler reads the
				 * patch against): project the interface's per-axis half-extents onto each, HalfUCm
				 * along U and HalfVCm along V. The interface is an axis-aligned rectangle
				 * (AddConnection enforces it) with zero extent on the normal axis, so each
				 * projection picks out exactly one in-plane extent.
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
			 * THE LP SOLVES AGAINST THE WEAKEST-LINK MATERIAL PAIRING, not the bare connection.
			 * FStructure::EffectiveJointStrength pairs a joint's connection with its two faces'
			 * materials (SHED_PATH.md B3) and is the same call the router's
			 * GetConnectionUtilisation reads, so the LP row and the router readout can never
			 * disagree about a cross-material joint's capacity. Where a face names no material it
			 * returns the bare connection.
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
		 * Same signal, same refusals as the whole-structure bridge (THREED_DESIGN E3): the flag
		 * is the permission to pose 3D, and a 2D structure drops the Y and still refuses a stray
		 * Y-normal rather than projecting it. The region pose changes only which pieces are
		 * included and which are earth — never how a joint is measured.
		 */
		const bool bThreeDimensionalPermitted = Structure.IsThreeDimensional();

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
			 * The one delta from the excluded-pieces form: a boundary piece is pinned grounded, so
			 * it writes no equilibrium rows and never moves; interior region pieces keep their own
			 * grounding, so a real foundation block inside the region stays grounded too.
			 */
			Block.bGrounded = BoundaryPieces.Contains(Piece) || Data.bIsGrounded;
			OutProblem.Blocks.Add(Block);
		}

		/*
		 * THE CHEAPEST SOUND POSE, on the same rule as the whole-structure bridge and for the same
		 * reason: a region whose posed rows all sit in one X-Z plane is a planar LP however the
		 * structure around it is flagged, and the 2D pose carries the exact Coulomb cone where the
		 * 3D pose carries its inscribed octagon. Kept in lock-step deliberately — a prover posing a
		 * region one way while the gate poses the whole structure the other would be two
		 * authorities on one collapse, judging the same joint 7.6% apart. The boundary ring is
		 * already pinned grounded above, so a boundary-to-boundary joint is skipped in the emit
		 * loop below exactly like any other doubly-grounded joint.
		 */
		const bool bThreeDimensional =
			bThreeDimensionalPermitted
			&& PosedProblemLeavesThePlane(Structure, BlockOfPiece, OutProblem);

		if (bThreeDimensional)
		{
			OutProblem.Dim = EOracleDim::Dim3D;

			/* The plan-Y the 2D pose drops — a 3D centroid's third lever arm. */
			for (int32 Block = 0; Block < OutProblem.Blocks.Num(); ++Block)
			{
				OutProblem.Blocks[Block].CentroidYCm =
					Structure.GetPiece(OutProblem.PieceOfBlock[Block]).CentreOfMassCm.Y;
			}
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

			/*
			 * Refused rather than projected, read against the POSE (bThreeDimensional) rather than
			 * the flag alone, so a planar pose chosen under 3D permission still refuses a Y-normal
			 * here — if the pose decision and this test ever disagreed about which joints are
			 * posed, the answer is a loud refusal rather than a Y-normal joint flattened into X-Z.
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
