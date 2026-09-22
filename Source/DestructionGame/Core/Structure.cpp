// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Structure.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/RigidBlock/RigidBlockBridge.h"
#include "HAL/PlatformTime.h"

// Solver's own log: Log per cascade that broke something or ran long, Verbose per pass.
DEFINE_LOG_CATEGORY_STATIC(LogDestructionSolve, Log, All);

// Solver prefix on every name: a unity build merges anonymous namespaces across files.
namespace
{
	/*
	 * Gravity, 980 cm/s2. MassKg * 980 is already a force in uu; the 1 N = 100 uu conversion is
	 * baked in (DESIGN.md §3). Multiplying by 100 again is out by 100x.
	 */
	constexpr double SolverGravityCmPerSecondSquared = 980.0;

	// Bed vs head joint threshold: cos(45 degrees), decided before any material profile.
	constexpr double SolverBedJointCosine = 0.70710678118654752440;

	// Max arch depth per span: sqrt(3)/2, BS 5977-1's equilateral triangle (ARCHING_DESIGN.md).
	constexpr double SolverArchingDepthPerSpan = 0.866;

	/*
	 * Max deep-beam depth as a multiple of the effective arm |M|/|F| (COMPOSITE_DEPTH_DESIGN.md
	 * slice 3). Fixtures need >= 2.465 (wall-end deletion stands) and <= 3.822 (one-sided corbel).
	 */
	constexpr double SolverCompositeDepthPerArm = 3.464;

	// Relative tolerance between a joint's rectangle and its area: above 1e-12 noise, below 1e-6.
	constexpr double SolverRectangleAreaToleranceRatio = 1.0e-9;

	/** The piece at the far end of a connection, or INDEX_NONE if it is not on it. */
	int32 OtherEndOf(const FConnection& Connection, int32 PieceIndex)
	{
		if (Connection.PieceA == PieceIndex)
		{
			return Connection.PieceB;
		}

		if (Connection.PieceB == PieceIndex)
		{
			return Connection.PieceA;
		}

		return INDEX_NONE;
	}

	/*
	 * Whether load leaving this piece returns to it, i.e. it is its own support (a knot DESIGN.md
	 * §3 cannot split). Stops at grounded pieces; the start stays unvisited to detect the return.
	 */
	bool LoadReturnsToPiece(
		int32 PieceIndex,
		const TArray<FStructurePiece>& Pieces,
		const TArray<FConnection>& Connections,
		const TArray<TArray<int32>>& LoadPaths)
	{
		TArray<bool> Visited;
		Visited.Init(false, Pieces.Num());

		TArray<int32> Frontier;
		Frontier.Add(PieceIndex);

		for (int32 Head = 0; Head < Frontier.Num(); ++Head)
		{
			const int32 Current = Frontier[Head];

			if (Pieces[Current].bIsGrounded)
			{
				continue;
			}

			for (const int32 Index : LoadPaths[Current])
			{
				const int32 Support = OtherEndOf(Connections[Index], Current);

				if (Support == PieceIndex)
				{
					return true;
				}

				if (!Visited[Support])
				{
					Visited[Support] = true;
					Frontier.Add(Support);
				}
			}
		}

		return false;
	}
}

int32 FStructure::AddPiece(double MassKg, bool bIsGrounded)
{
	// !(x >= 0) so NaN is rejected too. Zero is allowed.
	if (!(MassKg >= 0.0) || !FMath::IsFinite(MassKg))
	{
		return INDEX_NONE;
	}

	FStructurePiece Piece;
	Piece.Index = Pieces.Num();
	Piece.MassKg = MassKg;
	Piece.bIsGrounded = bIsGrounded;

	// Defaults false so GetPiece's placeholder reads as dead; this is the only place it turns on.
	Piece.bIsInTheStructure = true;

	return Pieces.Add(Piece);
}

int32 FStructure::AddPiece(double MassKg, bool bIsGrounded, const FVector& CentreOfMassCm)
{
	/*
	 * Refuse a non-finite centre before adding anything. A NaN centre becomes a NaN lever arm
	 * that reads intact. ContainsNaN also catches infinities.
	 */
	if (CentreOfMassCm.ContainsNaN())
	{
		return INDEX_NONE;
	}

	const int32 Handle = AddPiece(MassKg, bIsGrounded);

	if (Handle == INDEX_NONE)
	{
		return INDEX_NONE;
	}

	// The flag separates "at the origin" from "not supplied".
	Pieces[Handle].CentreOfMassCm = CentreOfMassCm;
	Pieces[Handle].bHasCentreOfMass = true;

	return Handle;
}

int32 FStructure::AddConnection(const FConnection& Connection)
{
	if (!Pieces.IsValidIndex(Connection.PieceA) || !Pieces.IsValidIndex(Connection.PieceB))
	{
		return INDEX_NONE;
	}

	if (Connection.PieceA == Connection.PieceB)
	{
		return INDEX_NONE;
	}

	// The load split divides by area; !(x > 0) rejects NaN along with zero and negative.
	if (!(Connection.InterfaceAreaSqCm > 0.0) || !FMath::IsFinite(Connection.InterfaceAreaSqCm))
	{
		return INDEX_NONE;
	}

	// Normalize fails for both zero-length and NaN. A non-unit normal is stored as given.
	FVector UnitNormal = Connection.InterfaceNormal;
	if (!UnitNormal.Normalize())
	{
		return INDEX_NONE;
	}

	/*
	 * Geometry checks apply only when a rectangle is supplied; zero extents mean no bending
	 * capacity measured. A half-filled rectangle still reaches the consistency rule below.
	 */
	if (!Connection.InterfaceHalfExtentCm.IsZero())
	{
		// A NaN centre would become a NaN lever arm.
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!FMath::IsFinite(Connection.InterfaceCentreCm[Axis]))
			{
				return INDEX_NONE;
			}
		}

		/*
		 * A rectangle needs an axis-aligned normal; off-axis would have no defined section
		 * modulus. MakeInterface always produces one.
		 */
		int32 SeparationAxis = INDEX_NONE;
		int32 AxisCount = 0;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Connection.InterfaceNormal[Axis] != 0.0)
			{
				SeparationAxis = Axis;
				++AxisCount;
			}
		}

		if (AxisCount != 1)
		{
			return INDEX_NONE;
		}

		/*
		 * Half-extents must be non-negative and finite: two negatives multiply to a plausible area
		 * (4 x -5 x -5 = 100) and flip every stress sign. Exactly zero on the separation axis.
		 */
		double RectangleAreaSqCm = 4.0;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double HalfExtentCm = Connection.InterfaceHalfExtentCm[Axis];

			if (!(HalfExtentCm >= 0.0) || !FMath::IsFinite(HalfExtentCm))
			{
				return INDEX_NONE;
			}

			if (Axis == SeparationAxis)
			{
				if (HalfExtentCm != 0.0)
				{
					return INDEX_NONE;
				}

				continue;
			}

			RectangleAreaSqCm *= HalfExtentCm;
		}

		// Rectangle and area must describe the same face. !(diff <= tol) also rejects NaN.
		const double DisagreementSqCm =
			FMath::Abs(RectangleAreaSqCm - Connection.InterfaceAreaSqCm);

		if (!(DisagreementSqCm
				<= SolverRectangleAreaToleranceRatio * Connection.InterfaceAreaSqCm))
		{
			return INDEX_NONE;
		}
	}

	// Parallel arrays; INDEX_NONE means never broken.
	ConnectionBreakPass.Add(INDEX_NONE);
	ConnectionBreakAuthority.Add(INDEX_NONE);

	return Connections.Add(Connection);
}

bool FStructure::RemovePiece(int32 PieceIndex)
{
	// Unknown or already-removed handles remove nothing, so joints are never severed twice.
	if (IsPieceRemoved(PieceIndex))
	{
		return false;
	}

	// Tombstone, never compact: connection handles index this array.
	Pieces[PieceIndex].bIsInTheStructure = false;

	/*
	 * Severing the piece's joints is removal's whole effect on the load model. Not stamped in
	 * ConnectionBreakPass: these joints did not fail under load.
	 */
	for (FConnection& Connection : Connections)
	{
		if (Connection.PieceA == PieceIndex || Connection.PieceB == PieceIndex)
		{
			Connection.Sever();
		}
	}

	// No re-solve; the last solve's forces stand until the caller solves again.
	return true;
}

bool FStructure::IsPieceRemoved(int32 PieceIndex) const
{
	// Fail-closed: an unknown handle reads as removed.
	return !Pieces.IsValidIndex(PieceIndex) || !Pieces[PieceIndex].bIsInTheStructure;
}

int32 FStructure::NumPieces() const
{
	return Pieces.Num();
}

int32 FStructure::NumLivePieces() const
{
	// Counted, not cached, so it cannot drift from the tombstones.
	int32 LivePieces = 0;
	for (const FStructurePiece& Piece : Pieces)
	{
		if (Piece.bIsInTheStructure)
		{
			++LivePieces;
		}
	}

	return LivePieces;
}

int32 FStructure::NumConnections() const
{
	return Connections.Num();
}

int32 FStructure::NumSolves() const
{
	return SolveCount;
}

bool FStructure::HasCompleteGeometry() const
{
	// Live pieces need a centre and live joints a rectangle. An empty structure reads true.
	for (const FStructurePiece& Piece : Pieces)
	{
		if (Piece.bIsInTheStructure && !Piece.bHasCentreOfMass)
		{
			return false;
		}
	}

	for (const FConnection& Connection : Connections)
	{
		if (!Connection.HasGiven() && Connection.InterfaceHalfExtentCm.IsZero())
		{
			return false;
		}
	}

	return true;
}

const FStructurePiece& FStructure::GetPiece(int32 PieceIndex) const
{
	static const FStructurePiece Placeholder;
	return Pieces.IsValidIndex(PieceIndex) ? Pieces[PieceIndex] : Placeholder;
}

const FConnection& FStructure::GetConnection(int32 ConnectionIndex) const
{
	static const FConnection Placeholder;
	return Connections.IsValidIndex(ConnectionIndex) ? Connections[ConnectionIndex] : Placeholder;
}

FConnection& FStructure::GetConnectionMutable(int32 ConnectionIndex)
{
	static FConnection Placeholder;
	Placeholder = FConnection{};
	return Connections.IsValidIndex(ConnectionIndex) ? Connections[ConnectionIndex] : Placeholder;
}

void FStructure::SolveLoads()
{
	// Once per call; the fixpoint below counts as one solve.
	++SolveCount;

	// Phase clocks feed GetLastSolveLoadsProfile only.
	LastSolveLoadsProfile = FSolveLoadsProfile();
	const double ProfileStartSeconds = FPlatformTime::Seconds();

	/*
	 * Each piece's joints, built once (pieces x connections was 4.3M calls on the 1,220-piece
	 * wall). Ascending connection index is contract: reordering shifts the last bit of sums that
	 * decide breaks.
	 */
	TArray<TArray<int32>> PieceJoints;
	PieceJoints.SetNum(Pieces.Num());

	for (int32 Index = 0; Index < Connections.Num(); ++Index)
	{
		const FConnection& Connection = Connections[Index];

		if (PieceJoints.IsValidIndex(Connection.PieceA))
		{
			PieceJoints[Connection.PieceA].Add(Index);
		}

		if (PieceJoints.IsValidIndex(Connection.PieceB))
		{
			PieceJoints[Connection.PieceB].Add(Index);
		}
	}

	/*
	 * Step one: each piece's supports, two-tiered (DESIGN.md §3). Bed joints beneath; with none,
	 * fall back to head joints.
	 */
	TArray<TArray<int32>> SupportConnections;
	SupportConnections.SetNum(Pieces.Num());

	// Pieces that took the head-joint fallback, for ReseatSpannedGroups. Excludes grounded/removed.
	TArray<bool> PieceHasNoSeat;
	PieceHasNoSeat.Init(false, Pieces.Num());

	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		TArray<int32> HeadConnections;

		for (const int32 Index : PieceJoints[PieceIndex])
		{
			/*
			 * Drop given joints before the tier decision: a broken bed joint that still won the
			 * tier would leave the piece falling instead of on its head joints.
			 */
			if (Connections[Index].HasGiven())
			{
				continue;
			}

			switch (GetJointRole(Index, PieceIndex))
			{
			case EJointRole::BedBeneath:
				SupportConnections[PieceIndex].Add(Index);
				break;

			case EJointRole::Head:
				HeadConnections.Add(Index);
				break;

			default:
				break;
			}
		}

		// Fallback only: one bed joint beneath beats any number of head joints.
		if (SupportConnections[PieceIndex].Num() == 0)
		{
			PieceHasNoSeat[PieceIndex] =
				Pieces[PieceIndex].bIsInTheStructure && !Pieces[PieceIndex].bIsGrounded;

			SupportConnections[PieceIndex] = MoveTemp(HeadConnections);
		}
	}

	/*
	 * Step 1.5: a run of seatless pieces spans the hole. The only geometric routing; last to touch
	 * SupportConnections.
	 */
	TArray<bool> PieceReseatedOnAnArch;
	TArray<bool> PieceInRefusedArchGroup;
	TArray<FSpannedArch> Arches;

	const double ReseatStartSeconds = FPlatformTime::Seconds();
	LastSolveLoadsProfile.SupportListsMs = (ReseatStartSeconds - ProfileStartSeconds) * 1000.0;

	ReseatSpannedGroups(
		PieceJoints, PieceHasNoSeat, SupportConnections, PieceReseatedOnAnArch,
		PieceInRefusedArchGroup, Arches);

	LastSolveLoadsProfile.ReseatMs = (FPlatformTime::Seconds() - ReseatStartSeconds) * 1000.0;

	// The inverse relation: who rests on each piece.
	TArray<TArray<int32>> Loaders;
	Loaders.SetNum(Pieces.Num());

	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		for (const int32 Index : SupportConnections[PieceIndex])
		{
			Loaders[OtherEndOf(Connections[Index], PieceIndex)].Add(PieceIndex);
		}
	}

	/*
	 * Pieces in an unroutable knot, reported unsupported (fail-closed, DESIGN.md §3). The set only
	 * grows, so the fixpoint terminates.
	 */
	PieceStranded.Init(false, Pieces.Num());

	/*
	 * Pieces on 2+ compression-only bearings whose centre of mass lies outside them: no admissible
	 * equilibrium. Excluded from the next walk; they come out Falling.
	 */
	TArray<bool> PieceOverturned;
	PieceOverturned.Init(false, Pieces.Num());

	/*
	 * Pieces of a refused one-sided arch group. The refused arch was their only path to earth, so
	 * they are Falling, not Stranded (DESIGN §8 case-21).
	 */
	TArray<bool> PieceReleasedFromRefusedArch;
	PieceReleasedFromRefusedArch.Init(false, Pieces.Num());

	/*
	 * Reachability and the load split depend on each other, so iterate to a fixpoint: at most
	 * NumPieces + 1 passes, each a full solve from scratch.
	 */
	const double FixpointStartSeconds = FPlatformTime::Seconds();

	for (;;)
	{
		++LastSolveLoadsProfile.FixpointIterations;
		LastSolveLoadsProfile.SupportedPerIteration.Add(0);
		LastSolveLoadsProfile.OverturnedPerIteration.Add(0);
		LastSolveLoadsProfile.StrandedPerIteration.Add(0);
		LastSolveLoadsProfile.ReleasedPerIteration.Add(0);

		ConnectionForces.Init(FVector::ZeroVector, Connections.Num());
		ConnectionMoments.Init(FVector::ZeroVector, Connections.Num());
		ConnectionCompositeDepthCm.Init(0.0, Connections.Num());
		PieceSupported.Init(false, Pieces.Num());

		/*
		 * Step two: BFS up the support relation from live grounded pieces. Excluded pieces are
		 * neither marked nor crossed.
		 */
		TArray<int32> SupportedFrontier;
		for (const FStructurePiece& Piece : Pieces)
		{
			if (Piece.bIsGrounded && Piece.bIsInTheStructure)
			{
				PieceSupported[Piece.Index] = true;
				SupportedFrontier.Add(Piece.Index);
			}
		}

		for (int32 Head = 0; Head < SupportedFrontier.Num(); ++Head)
		{
			for (const int32 Loader : Loaders[SupportedFrontier[Head]])
			{
				if (!PieceSupported[Loader] && !PieceStranded[Loader] && !PieceOverturned[Loader]
					&& !PieceReleasedFromRefusedArch[Loader])
				{
					PieceSupported[Loader] = true;
					SupportedFrontier.Add(Loader);
				}
			}
		}

		/*
		 * Step three: split only over supports with their own path to earth, or a joint carrying
		 * 1.9x reads 0.95x. A supported piece always keeps at least one.
		 */
		TArray<TArray<int32>> LoadPaths;
		LoadPaths.SetNum(Pieces.Num());

		for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
		{
			for (const int32 Index : SupportConnections[PieceIndex])
			{
				if (PieceSupported[OtherEndOf(Connections[Index], PieceIndex)])
				{
					LoadPaths[PieceIndex].Add(Index);
				}
			}
		}

		/*
		 * Step four: accumulate weight downward in topological order (Kahn): a piece is ready once
		 * everything on it is done. Grounded pieces terminate flow.
		 */
		TArray<int32> PendingLoaders;
		PendingLoaders.Init(0, Pieces.Num());

		for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
		{
			if (!PieceSupported[PieceIndex] || Pieces[PieceIndex].bIsGrounded)
			{
				continue;
			}

			for (const int32 Index : LoadPaths[PieceIndex])
			{
				++PendingLoaders[OtherEndOf(Connections[Index], PieceIndex)];
			}
		}

		TArray<int32> Ready;

		for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
		{
			if (PieceSupported[PieceIndex] && !Pieces[PieceIndex].bIsGrounded
				&& PendingLoaders[PieceIndex] == 0)
			{
				Ready.Add(PieceIndex);
			}
		}

		TArray<double> ReceivedFromAboveUU;
		ReceivedFromAboveUU.Init(0.0, Pieces.Num());

		/*
		 * Moment received from above, about the receiving piece's centre of mass (not the world
		 * origin, which loses precision far from it).
		 */
		TArray<FVector> ReceivedMomentUuCm;
		ReceivedMomentUuCm.Init(FVector::ZeroVector, Pieces.Num());

		// A pass that overturns something is not the last.
		bool bOverturnedThisPass = false;

		for (int32 Order = 0; Order < Ready.Num(); ++Order)
		{
			const int32 Current = Ready[Order];

			const double TotalUU =
				ReceivedFromAboveUU[Current] + Pieces[Current].MassKg * SolverGravityCmPerSecondSquared;

			double TotalAreaSqCm = 0.0;
			for (const int32 Index : LoadPaths[Current])
			{
				TotalAreaSqCm += Connections[Index].InterfaceAreaSqCm;
			}

			// Should always hold; a NaN total lands on "cannot split" and the loop still continues.
			const bool bCanSplit = TotalAreaSqCm > 0.0;

			/*
			 * Moments only on a statically determinate piece: one support, a placed centre, not
			 * re-seated on an arch. On several supports the moment is left zero, which is exact only
			 * when the centre sits at their area-weighted centroid (MOMENTS_DESIGN.md). Per-joint
			 * moments there would read ~0.029 tension on every bed joint of a standing wall.
			 */
			const bool bLoadPathIsDeterminate = LoadPaths[Current].Num() == 1
				&& Pieces[Current].bHasCentreOfMass && !PieceReseatedOnAnArch[Current];

			/*
			 * Zeroing the moment is wrong if the centre of mass has left the supports' region (the
			 * shed's ridge without its back gable). Mark it; the next walk drops it to Falling.
			 */
			if (LoadPaths[Current].Num() >= 2 && !PieceOverturned[Current]
				&& PieceOverturnsOffItsSupports(Current, LoadPaths[Current]))
			{
				PieceOverturned[Current] = true;
				bOverturnedThisPass = true;
				++LastSolveLoadsProfile.OverturnedPerIteration.Last();
			}

			for (const int32 Index : LoadPaths[Current])
			{
				const FConnection& Connection = Connections[Index];
				const int32 Support = OtherEndOf(Connection, Current);

				if (bCanSplit)
				{
					const double ShareUU = TotalUU * (Connection.InterfaceAreaSqCm / TotalAreaSqCm);

					/*
					 * Straight down. The stored force acts on PieceB (ConnectionLoad.h), so when the
					 * loaded piece is PieceA it is the upward reaction. A wrong sign reads compression
					 * as tension, which mortar resists at 1% of compressive strength.
					 */
					const double SignedZUU = Connection.PieceB == Current ? -ShareUU : ShareUU;

					// Assignment: a connection supports at most one endpoint (mutual support strands).
					ConnectionForces[Index] = FVector(0.0, 0.0, SignedZUU);

					// Physical weight, straight down; moments use this, not the signed stored force.
					const FVector ShareWeightUu(0.0, 0.0, -ShareUU);

					// A joint without a rectangle has no centroid; its zero is not the origin.
					const bool bJointKnowsItsFace = !Connection.InterfaceHalfExtentCm.IsZero();

					/*
					 * Moment about this joint's centroid: the received moment re-referenced
					 * (Varignon, (c_from - c_to) x F) plus this piece's weight. The force split is
					 * unchanged.
					 */
					FVector MomentAboutJointUuCm = FVector::ZeroVector;

					if (bLoadPathIsDeterminate && bJointKnowsItsFace)
					{
						MomentAboutJointUuCm = ReceivedMomentUuCm[Current]
							+ FVector::CrossProduct(
								Pieces[Current].CentreOfMassCm - Connection.InterfaceCentreCm,
								ShareWeightUu);

						/*
						 * A seat with something to push against arches rather than cantilevers: the
						 * brick over a deleted one reads 1.63 tension as a cantilever, 0.0142 compression
						 * as an arch (ARCHING_DESIGN.md). The joint checks the kern; the graph check is here.
						 */
						if (GetJointRole(Index, Current) == EJointRole::BedBeneath)
						{
							const double ArchingRelief = Connection.ArchingMomentScale(
								ConnectionForces[Index], MomentAboutJointUuCm);

							if (ArchingRelief < 1.0)
							{
								/*
								 * The (1 - k) of the moment removed must be supplied as thrust the abutment can
								 * deliver without sliding (DESIGN.md §7 gap 4).
								 */
								const double DeletedCoupleUuCm =
									(1.0 - ArchingRelief) * MomentAboutJointUuCm.Size();

								if (HasArchingAbutment(
										Current, Connection, ConnectionForces[Index],
										DeletedCoupleUuCm, PieceJoints, SupportConnections,
										PieceReseatedOnAnArch))
								{
									MomentAboutJointUuCm *= ArchingRelief;
								}
							}

							/*
							 * Masonry stacked over a lost support resists as a deep beam, t*D^2/6, not one bed
							 * patch (eleven courses is 65x the patch's 179.48 cm3; ARCHING_DESIGN.md slice 5).
							 * Depth is capped at lambda * |M|/|F|, floored at the corbelling body's own depth.
							 * Comparisons are written out so a NaN depth fails closed (COMPOSITE_DEPTH_DESIGN.md).
							 */
							if (!MomentAboutJointUuCm.IsZero()
								&& PieceRestingOn(Current, PieceJoints) != INDEX_NONE)
							{
								const double PermittedDepthCm = SolverCompositeDepthPerArm
									* MomentAboutJointUuCm.Size()
									/ ConnectionForces[Index].Size();

								if (PermittedDepthCm > 0.0 && FMath::IsFinite(PermittedDepthCm))
								{
									const double BodyDepthCm =
										CorbellingBodyDepthCm(Current, Index, PieceJoints);

									const double CreditableDepthCm =
										BodyDepthCm > PermittedDepthCm
											? BodyDepthCm
											: PermittedDepthCm;

									const double StandingOverItCm = MasonryDepthAboveCm(
										Current, Index, PieceJoints, CreditableDepthCm);

									ConnectionCompositeDepthCm[Index] =
										StandingOverItCm < CreditableDepthCm
											? StandingOverItCm
											: CreditableDepthCm;
								}
							}
						}

						ConnectionMoments[Index] = Connection.PieceB == Current
							? MomentAboutJointUuCm
							: -MomentAboutJointUuCm;
					}

					/*
					 * Pass the moment on, re-referenced to the support's centre. Both ends must be
					 * placed, or the origin enters a lever arm.
					 */
					if (bJointKnowsItsFace && Pieces[Support].bHasCentreOfMass)
					{
						ReceivedMomentUuCm[Support] += MomentAboutJointUuCm
							+ FVector::CrossProduct(
								Connection.InterfaceCentreCm - Pieces[Support].CentreOfMassCm,
								ShareWeightUu);
					}

					ReceivedFromAboveUU[Support] += ShareUU;
				}

				// A grounded piece absorbs load and passes nothing on.
				if (--PendingLoaders[Support] == 0 && !Pieces[Support].bIsGrounded)
				{
					Ready.Add(Support);
				}
			}
		}

		/*
		 * Step five: strand pieces whose own load returns to them (DESIGN.md §3); the next pass
		 * runs without them.
		 */
		bool bStrandedThisPass = false;
		bool bReleasedThisPass = false;
		for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
		{
			if (!PieceSupported[PieceIndex] || Pieces[PieceIndex].bIsGrounded)
			{
				continue;
			}

			if (LoadReturnsToPiece(PieceIndex, Pieces, Connections, LoadPaths))
			{
				// A refused-arch member lost its only load path, so it falls rather than strands.
				if (PieceInRefusedArchGroup[PieceIndex])
				{
					PieceReleasedFromRefusedArch[PieceIndex] = true;
					bReleasedThisPass = true;
					++LastSolveLoadsProfile.ReleasedPerIteration.Last();
				}
				else
				{
					PieceStranded[PieceIndex] = true;
					bStrandedThisPass = true;
					++LastSolveLoadsProfile.StrandedPerIteration.Last();
				}
			}
		}

		for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
		{
			if (PieceSupported[PieceIndex])
			{
				++LastSolveLoadsProfile.SupportedPerIteration.Last();
			}
		}

		if (!bStrandedThisPass && !bOverturnedThisPass && !bReleasedThisPass)
		{
			break;
		}
	}

	/*
	 * Step six: arch thrust on the springings. Outside the fixpoint because it depends on the
	 * settled seat loads and feeds nothing back.
	 */
	const double ArchingStartSeconds = FPlatformTime::Seconds();

	ApplyArchingThrust(PieceJoints, Arches);

	const double ProfileEndSeconds = FPlatformTime::Seconds();

	LastSolveLoadsProfile.FixpointMs = (ArchingStartSeconds - FixpointStartSeconds) * 1000.0;
	LastSolveLoadsProfile.ArchingMs = (ProfileEndSeconds - ArchingStartSeconds) * 1000.0;
	LastSolveLoadsProfile.TotalMs = (ProfileEndSeconds - ProfileStartSeconds) * 1000.0;

	// No strength checks here: ApplyForce latches, so a solve must not call it.
}

bool FStructure::PieceOverturnsOffItsSupports(int32 PieceIndex, const TArray<int32>& LoadPath) const
{
	const FStructurePiece& Piece = Pieces[PieceIndex];

	// No centre of mass: fail closed, does not overturn.
	if (!Piece.bHasCentreOfMass)
	{
		return false;
	}

	// A support that carries tension holds the lifting side down (the porch overhang's Screw).
	for (const int32 Index : LoadPath)
	{
		if (Connections[Index].Strength.TensileStrengthMPa > 0.0)
		{
			return false;
		}
	}

	/*
	 * Compression-only: the body stands while its centre of mass projects (in X,Y) inside the
	 * contacts' hull. Tests the bounding box, a superset, so it never fells a body inside the hull.
	 */
	double MinX = 0.0;
	double MaxX = 0.0;
	double MinY = 0.0;
	double MaxY = 0.0;
	bool bHaveBox = false;

	for (const int32 Index : LoadPath)
	{
		const FConnection& Connection = Connections[Index];

		// No rectangle, no box: fail closed.
		if (Connection.InterfaceHalfExtentCm.IsZero())
		{
			return false;
		}

		const double LoX = Connection.InterfaceCentreCm.X - Connection.InterfaceHalfExtentCm.X;
		const double HiX = Connection.InterfaceCentreCm.X + Connection.InterfaceHalfExtentCm.X;
		const double LoY = Connection.InterfaceCentreCm.Y - Connection.InterfaceHalfExtentCm.Y;
		const double HiY = Connection.InterfaceCentreCm.Y + Connection.InterfaceHalfExtentCm.Y;

		if (!bHaveBox)
		{
			MinX = LoX;
			MaxX = HiX;
			MinY = LoY;
			MaxY = HiY;
			bHaveBox = true;
		}
		else
		{
			MinX = FMath::Min(MinX, LoX);
			MaxX = FMath::Max(MaxX, HiX);
			MinY = FMath::Min(MinY, LoY);
			MaxY = FMath::Max(MaxY, HiY);
		}
	}

	if (!bHaveBox)
	{
		return false;
	}

	// Four positive comparisons, so a NaN coordinate reads "inside" rather than overturned.
	const FVector Com = Piece.CentreOfMassCm;
	return Com.X < MinX || Com.X > MaxX || Com.Y < MinY || Com.Y > MaxY;
}

void FStructure::ReseatSpannedGroups(
	const TArray<TArray<int32>>& PieceJoints,
	const TArray<bool>& PieceHasNoSeat,
	TArray<TArray<int32>>& SupportConnections,
	TArray<bool>& PieceReseatedOnAnArch,
	TArray<bool>& PieceInRefusedArchGroup,
	TArray<FSpannedArch>& Arches) const
{
	// Sized before the gate so callers can always index them.
	PieceReseatedOnAnArch.Init(false, Pieces.Num());
	PieceInRefusedArchGroup.Init(false, Pieces.Num());
	Arches.Reset();

	/*
	 * Finding an arch needs positions; without them this is a no-op. The fuzz generators emit no
	 * geometry, so an arch firing without it would diverge from their oracles unnoticed.
	 */
	if (!HasCompleteGeometry())
	{
		return;
	}

	TArray<bool> Grouped;
	Grouped.Init(false, Pieces.Num());

	// Hops from the nearest abutment; INDEX_NONE if unreached. Reused across groups.
	TArray<int32> HopsFromAbutment;
	HopsFromAbutment.Init(INDEX_NONE, Pieces.Num());

	/*
	 * The piece across an intact head joint, or INDEX_NONE. Head joints only: following bed
	 * joints would fuse the courses above and below a hole.
	 */
	const auto AcrossHeadJoint = [this](int32 PieceIndex, int32 Index) -> int32
	{
		return Connections[Index].HasGiven() || GetJointRole(Index, PieceIndex) != EJointRole::Head
			? INDEX_NONE
			: OtherEndOf(Connections[Index], PieceIndex);
	};

	for (int32 Seed = 0; Seed < Pieces.Num(); ++Seed)
	{
		if (!PieceHasNoSeat[Seed] || Grouped[Seed])
		{
			continue;
		}

		// The connected run of seatless pieces containing Seed.
		TArray<int32> Group;
		Group.Add(Seed);
		Grouped[Seed] = true;

		for (int32 Head = 0; Head < Group.Num(); ++Head)
		{
			for (const int32 Index : PieceJoints[Group[Head]])
			{
				const int32 Neighbour = AcrossHeadJoint(Group[Head], Index);

				if (Neighbour != INDEX_NONE && PieceHasNoSeat[Neighbour] && !Grouped[Neighbour])
				{
					Grouped[Neighbour] = true;
					Group.Add(Neighbour);
				}
			}
		}

		FVector GroupCentreCm = FVector::ZeroVector;
		for (const int32 Member : Group)
		{
			GroupCentreCm += Pieces[Member].CentreOfMassCm;
		}

		GroupCentreCm /= static_cast<double>(Group.Num());

		/*
		 * Abutments: seated pieces one head joint from the group, each counted once (twice would
		 * double its thrust share). Members touching one seed the walk inward.
		 */
		TArray<int32> Abutments;
		TArray<FVector> TowardAbutmentCm;
		TArray<int32> Frontier;

		for (const int32 Member : Group)
		{
			for (const int32 Index : PieceJoints[Member])
			{
				const int32 Abutment = AcrossHeadJoint(Member, Index);

				if (Abutment == INDEX_NONE || PieceHasNoSeat[Abutment]
					|| !Pieces[Abutment].bIsInTheStructure)
				{
					continue;
				}

				if (Abutments.Find(Abutment) == INDEX_NONE)
				{
					Abutments.Add(Abutment);
					TowardAbutmentCm.Add(Pieces[Abutment].CentreOfMassCm - GroupCentreCm);
				}

				if (HopsFromAbutment[Member] == INDEX_NONE)
				{
					HopsFromAbutment[Member] = 1;
					Frontier.Add(Member);
				}
			}
		}

		/*
		 * Spans only with abutments on opposite sides (negative dot product); one side is a
		 * cantilever. A NaN direction fails the test.
		 */
		bool bAbutsOnBothSides = false;

		for (int32 First = 0; First < TowardAbutmentCm.Num() && !bAbutsOnBothSides; ++First)
		{
			for (int32 Second = First + 1; Second < TowardAbutmentCm.Num(); ++Second)
			{
				if (FVector::DotProduct(TowardAbutmentCm[First], TowardAbutmentCm[Second]) < 0.0)
				{
					bAbutsOnBothSides = true;
					break;
				}
			}
		}

		if (!bAbutsOnBothSides)
		{
			// Recorded so SolveLoads reports the run Falling rather than Stranded.
			for (const int32 Member : Group)
			{
				PieceInRefusedArchGroup[Member] = true;
			}
			continue;
		}

		for (int32 Head = 0; Head < Frontier.Num(); ++Head)
		{
			for (const int32 Index : PieceJoints[Frontier[Head]])
			{
				const int32 Neighbour = AcrossHeadJoint(Frontier[Head], Index);

				if (Neighbour != INDEX_NONE && PieceHasNoSeat[Neighbour]
					&& HopsFromAbutment[Neighbour] == INDEX_NONE)
				{
					HopsFromAbutment[Neighbour] = HopsFromAbutment[Frontier[Head]] + 1;
					Frontier.Add(Neighbour);
				}
			}
		}

		/*
		 * Acyclic by construction: a member keeps only head joints one hop closer to an abutment
		 * (ARCHING_DESIGN trap 1). Filtering in place keeps ascending joint order.
		 */
		for (const int32 Member : Group)
		{
			if (HopsFromAbutment[Member] == INDEX_NONE)
			{
				continue;
			}

			TArray<int32> TowardTheAbutments;

			for (const int32 Index : PieceJoints[Member])
			{
				const int32 Neighbour = AcrossHeadJoint(Member, Index);

				if (Neighbour == INDEX_NONE)
				{
					continue;
				}

				// An abutment is zero hops.
				const int32 NeighbourHops = PieceHasNoSeat[Neighbour]
					? HopsFromAbutment[Neighbour]
					: 0;

				if (NeighbourHops == HopsFromAbutment[Member] - 1)
				{
					TowardTheAbutments.Add(Index);
				}
			}

			if (TowardTheAbutments.Num() > 0)
			{
				SupportConnections[Member] = MoveTemp(TowardTheAbutments);
				PieceReseatedOnAnArch[Member] = true;
			}
		}

		/*
		 * Record the arch with abutments split into two ends by the sign of their projection on
		 * the first abutment's direction.
		 */
		FSpannedArch Arch;
		Arch.TowardEndZero = TowardAbutmentCm[0];

		if (!Arch.TowardEndZero.Normalize())
		{
			continue;
		}

		FVector EndCentreCm[2] = { FVector::ZeroVector, FVector::ZeroVector };

		for (int32 Which = 0; Which < Abutments.Num(); ++Which)
		{
			const double AlongAxisCm =
				FVector::DotProduct(TowardAbutmentCm[Which], Arch.TowardEndZero);

			if (AlongAxisCm > 0.0)
			{
				Arch.Abutments[0].Add(Abutments[Which]);
				EndCentreCm[0] += Pieces[Abutments[Which]].CentreOfMassCm;
			}
			else if (AlongAxisCm < 0.0)
			{
				Arch.Abutments[1].Add(Abutments[Which]);
				EndCentreCm[1] += Pieces[Abutments[Which]].CentreOfMassCm;
			}
		}

		// Always true after bAbutsOnBothSides; kept so a one-ended arch cannot slip through (trap 2).
		if (Arch.Abutments[0].Num() > 0 && Arch.Abutments[1].Num() > 0)
		{
			// Span L: distance between the ends' mean abutment centres.
			EndCentreCm[0] /= static_cast<double>(Arch.Abutments[0].Num());
			EndCentreCm[1] /= static_cast<double>(Arch.Abutments[1].Num());

			Arch.SpanCm = (EndCentreCm[0] - EndCentreCm[1]).Size();

			/*
			 * Thrust axis: horizontal line between the end centres, not the first abutment's
			 * direction, which would push out of the wall plane at a corner.
			 */
			FVector ThrustAxisCm = EndCentreCm[0] - EndCentreCm[1];
			ThrustAxisCm.Z = 0.0;

			if (!ThrustAxisCm.Normalize())
			{
				continue;
			}

			Arch.TowardEndZero = ThrustAxisCm;

			Arches.Add(MoveTemp(Arch));
		}
	}
}

void FStructure::ApplyArchingThrust(
	const TArray<TArray<int32>>& PieceJoints,
	const TArray<FSpannedArch>& Arches)
{
	for (const FSpannedArch& Arch : Arches)
	{
		/*
		 * The seats the thrust leaves through. Sign per seat: the stored force acts on PieceB
		 * (ConnectionLoad.h); backwards, the ends pull together instead of apart.
		 */
		TArray<int32> Seats[2];
		TArray<double> SeatSign[2];
		double SeatAreaSqCm[2] = { 0.0, 0.0 };

		// Depth if only the angle governed; the cover walk need look no further.
		const double AngleCappedDepthCm = SolverArchingDepthPerSpan * Arch.SpanCm;

		/*
		 * The thinnest cover at either end, one value for the arch: per-end values would leave a
		 * net horizontal force (trap 2).
		 */
		double CoverCm = TNumericLimits<double>::Max();

		// W: the whole load on the abutments, springings' columns included (ARCHING_DESIGN).
		double TotalVerticalUu = 0.0;

		for (int32 End = 0; End < 2; ++End)
		{
			for (const int32 Abutment : Arch.Abutments[End])
			{
				// The first seat: the plane the cover is measured from.
				int32 SpringingJointIndex = INDEX_NONE;

				for (const int32 Index : PieceJoints[Abutment])
				{
					const FConnection& Connection = Connections[Index];

					// GetJointRole still answers for a severed joint, so check HasGiven.
					if (Connection.HasGiven()
						|| GetJointRole(Index, Abutment) != EJointRole::BedBeneath)
					{
						continue;
					}

					if (SpringingJointIndex == INDEX_NONE)
					{
						SpringingJointIndex = Index;
					}

					Seats[End].Add(Index);
					SeatSign[End].Add(Connection.PieceB == Abutment ? 1.0 : -1.0);
					SeatAreaSqCm[End] += Connection.InterfaceAreaSqCm;

					TotalVerticalUu += FMath::Abs(ConnectionForces[Index].Z);
				}

				if (SpringingJointIndex == INDEX_NONE)
				{
					continue;
				}

				// Not Min: a NaN cover must be taken so the guard below leaves the arch unthrust.
				const double AtThisEndCm = MasonryDepthAboveCm(
					Abutment, SpringingJointIndex, PieceJoints, AngleCappedDepthCm);

				if (!(AtThisEndCm >= CoverCm))
				{
					CoverCm = AtThisEndCm;
				}
			}
		}

		/*
		 * Both ends or neither (trap 2). Positive tests throughout, so a NaN leaves the arch
		 * unthrust.
		 */
		if (Seats[0].Num() == 0 || Seats[1].Num() == 0)
		{
			continue;
		}

		if (!(TotalVerticalUu > 0.0) || !FMath::IsFinite(TotalVerticalUu)
			|| !(SeatAreaSqCm[0] > 0.0) || !(SeatAreaSqCm[1] > 0.0))
		{
			continue;
		}

		if (!(Arch.SpanCm > 0.0) || !FMath::IsFinite(Arch.SpanCm)
			|| !(CoverCm > 0.0) || !FMath::IsFinite(CoverCm))
		{
			continue;
		}

		/*
		 * d_e/L: the smaller of the cover and the angle cap. Written out, not FMath::Min, which
		 * would silently replace a NaN cover with the angle's answer.
		 */
		const double DepthPerSpan = CoverCm < AngleCappedDepthCm
			? CoverCm / Arch.SpanCm
			: SolverArchingDepthPerSpan;

		if (!(DepthPerSpan > 0.0))
		{
			continue;
		}

		/*
		 * H = W*L/(8r) with rise r = d_e/3, so H = 3W/(8*(d_e/L)); thin cover raises the thrust.
		 * At the angle cap, d_e/L = 0.866.
		 */
		const double ThrustUu = 3.0 * TotalVerticalUu / (8.0 * DepthPerSpan);

		for (int32 End = 0; End < 2; ++End)
		{
			// +H and -H along one axis cancel exactly.
			const FVector EndThrustUu = (End == 0 ? ThrustUu : -ThrustUu) * Arch.TowardEndZero;

			for (int32 Which = 0; Which < Seats[End].Num(); ++Which)
			{
				const int32 Index = Seats[End][Which];

				// Split among an end's seats by area, as the load split is.
				const double AreaShare =
					Connections[Index].InterfaceAreaSqCm / SeatAreaSqCm[End];

				ConnectionForces[Index] += SeatSign[End][Which] * AreaShare * EndThrustUu;
			}
		}
	}
}

int32 FStructure::PieceRestingOn(
	int32 Piece, const TArray<TArray<int32>>& PieceJoints) const
{
	/*
	 * The first live piece on an intact BedAbove joint, by ascending joint index. A chain, not a
	 * traversal: untested on stepped or gabled walls.
	 */
	for (const int32 Index : PieceJoints[Piece])
	{
		if (Connections[Index].HasGiven()
			|| GetJointRole(Index, Piece) != EJointRole::BedAbove)
		{
			continue;
		}

		const int32 Other = OtherEndOf(Connections[Index], Piece);

		if (Other != INDEX_NONE && Pieces[Other].bIsInTheStructure)
		{
			return Other;
		}
	}

	return INDEX_NONE;
}

double FStructure::MasonryDepthAboveCm(
	int32 Piece,
	int32 SeatJointIndex,
	const TArray<TArray<int32>>& PieceJoints,
	double EnoughDepthCm) const
{
	const int32 Seat = OtherEndOf(Connections[SeatJointIndex], Piece);

	if (Seat == INDEX_NONE)
	{
		return 0.0;
	}

	/*
	 * The first course always counts. Depth is measured as course pitch (centre to centre), not
	 * brick height, which reads ~13% shallow.
	 */
	const double FirstCourseRiseCm =
		Pieces[Piece].CentreOfMassCm.Z - Pieces[Seat].CentreOfMassCm.Z;

	if (!(FirstCourseRiseCm > 0.0))
	{
		return 0.0;
	}

	/*
	 * Bounded by EnoughDepthCm and by the piece count (defence against a cyclic graph). Kept as a
	 * double so a tiny pitch cannot overflow an integer conversion.
	 */
	const double MaxCourses = FMath::Min(
		FMath::CeilToDouble(EnoughDepthCm / FirstCourseRiseCm),
		static_cast<double>(Pieces.Num()));

	double CoverCm = FirstCourseRiseCm;
	int32 Current = Piece;

	for (int32 Course = 1; CoverCm < EnoughDepthCm && Course < MaxCourses; ++Course)
	{
		const int32 Above = PieceRestingOn(Current, PieceJoints);

		if (Above == INDEX_NONE)
		{
			break;
		}

		// A step that does not rise ends the walk; less cover means more thrust, the safe side.
		const double RiseCm =
			Pieces[Above].CentreOfMassCm.Z - Pieces[Current].CentreOfMassCm.Z;

		if (!(RiseCm > 0.0))
		{
			break;
		}

		CoverCm += RiseCm;
		Current = Above;
	}

	return CoverCm;
}

double FStructure::CorbellingBodyDepthCm(
	int32 Piece,
	int32 SeatJointIndex,
	const TArray<TArray<int32>>& PieceJoints) const
{
	const int32 Seat = OtherEndOf(Connections[SeatJointIndex], Piece);

	if (Seat == INDEX_NONE)
	{
		return 0.0;
	}

	// The piece on the joint is the first course by construction; measured as a course pitch.
	const double FirstCourseRiseCm =
		Pieces[Piece].CentreOfMassCm.Z - Pieces[Seat].CentreOfMassCm.Z;

	if (!(FirstCourseRiseCm > 0.0))
	{
		return 0.0;
	}

	// Seated on exactly one live piece through an intact bed joint.
	auto IsCorbelling = [this, &PieceJoints](int32 Candidate)
	{
		int32 Seats = 0;

		for (const int32 Index : PieceJoints[Candidate])
		{
			if (Connections[Index].HasGiven()
				|| GetJointRole(Index, Candidate) != EJointRole::BedBeneath)
			{
				continue;
			}

			const int32 Below = OtherEndOf(Connections[Index], Candidate);

			if (Below != INDEX_NONE && Pieces[Below].bIsInTheStructure)
			{
				++Seats;
			}
		}

		return Seats == 1;
	};

	double DepthCm = FirstCourseRiseCm;
	int32 Current = Piece;

	// The piece-count bound only guards against an inconsistent graph.
	for (int32 Course = 1; Course < Pieces.Num(); ++Course)
	{
		int32 Above = INDEX_NONE;

		for (const int32 Index : PieceJoints[Current])
		{
			if (Connections[Index].HasGiven()
				|| GetJointRole(Index, Current) != EJointRole::BedAbove)
			{
				continue;
			}

			const int32 Other = OtherEndOf(Connections[Index], Current);

			if (Other != INDEX_NONE && Pieces[Other].bIsInTheStructure && IsCorbelling(Other))
			{
				Above = Other;
				break;
			}
		}

		if (Above == INDEX_NONE)
		{
			break;
		}

		// !(x > 0) stops on a NaN too; a shallower body credits less section, the safe side.
		const double RiseCm =
			Pieces[Above].CentreOfMassCm.Z - Pieces[Current].CentreOfMassCm.Z;

		if (!(RiseCm > 0.0))
		{
			break;
		}

		DepthCm += RiseCm;
		Current = Above;
	}

	return DepthCm;
}

bool FStructure::HasArchingAbutment(
	int32 PieceIndex,
	const FConnection& BedJoint,
	const FVector& SeatForceUu,
	double DeletedCoupleUuCm,
	const TArray<TArray<int32>>& PieceJoints,
	const TArray<TArray<int32>>& SupportConnections,
	const TArray<bool>& PieceReseatedOnAnArch) const
{
	// Unreachable in practice; guards the projections below.
	FVector UnitNormal = BedJoint.InterfaceNormal;

	if (!UnitNormal.Normalize())
	{
		return false;
	}

	// Overhang direction in the seat plane (5.625 cm for a half-seated running-bond brick).
	const FVector EccentricCm = FVector::VectorPlaneProject(
		Pieces[PieceIndex].CentreOfMassCm - BedJoint.InterfaceCentreCm, UnitNormal);

	/*
	 * The seat's sliding capacity, MPa: Mohr-Coulomb, cohesion plus friction on its compression.
	 * Force is uu, strength MPa; ForceUnitsPerMPaSqCm is the one conversion.
	 */
	const FConnectionLoad SeatLoad = DestructionForce::ClassifyForce(SeatForceUu, UnitNormal);

	const double SeatCompressionMPa = SeatLoad.Compression
		/ (BedJoint.InterfaceAreaSqCm * DestructionForce::ForceUnitsPerMPaSqCm);

	const double CohesionAndFrictionMPa = BedJoint.Strength.ShearCohesionMPa
		+ BedJoint.Strength.FrictionCoefficient * SeatCompressionMPa;

	// Not FMath::Min, which would replace a NaN capacity with the ceiling; this keeps the NaN.
	const double SlidingCapacityMPa =
		BedJoint.Strength.MaxShearStrengthMPa < CohesionAndFrictionMPa
			? BedJoint.Strength.MaxShearStrengthMPa
			: CohesionAndFrictionMPa;

	for (const int32 Index : PieceJoints[PieceIndex])
	{
		const FConnection& Head = Connections[Index];

		// GetJointRole still answers for a severed joint, so check HasGiven.
		if (Head.HasGiven() || GetJointRole(Index, PieceIndex) != EJointRole::Head)
		{
			continue;
		}

		// An unmeasured face has no centroid to take a side from.
		if (Head.InterfaceHalfExtentCm.IsZero())
		{
			continue;
		}

		const FVector TowardAbutmentCm = FVector::VectorPlaneProject(
			Head.InterfaceCentreCm - Pieces[PieceIndex].CentreOfMassCm, UnitNormal);

		// Must be on the overhang side; a positive test drops zero and NaN.
		if (!(FVector::DotProduct(EccentricCm, TowardAbutmentCm) > 0.0))
		{
			continue;
		}

		const int32 Abutment = OtherEndOf(Head, PieceIndex);

		/*
		 * The abutment must reach the ground on its own account, not by leaning on this piece
		 * (trap 3: two bricks propping each other over open air).
		 */
		if (!PieceSupported.IsValidIndex(Abutment) || !PieceSupported[Abutment])
		{
			continue;
		}

		bool bAbutmentLeansOnUs = false;

		for (const int32 Support : SupportConnections[Abutment])
		{
			if (OtherEndOf(Connections[Support], Abutment) == PieceIndex)
			{
				bAbutmentLeansOnUs = true;
				break;
			}
		}

		// Leaning on us is fine only for a neighbour re-seated on a two-sided spanning group.
		if (bAbutmentLeansOnUs && !PieceReseatedOnAnArch[Abutment])
		{
			continue;
		}

		// A spanned group's thrust is checked by ApplyArchingThrust instead, not twice.
		if (PieceReseatedOnAnArch[Abutment])
		{
			return true;
		}

		/*
		 * The deleted couple (1 - k)*|M| must be supplied by a horizontal push through this head
		 * joint, reacted as shear in the seat, over an arm along the seat normal (3.75 cm for a
		 * standard brick). !(demand <= capacity) refuses a NaN.
		 */
		const double ThrustArmCm = FMath::Abs(
			FVector::DotProduct(Head.InterfaceCentreCm - BedJoint.InterfaceCentreCm, UnitNormal));

		const double ThrustDemandMPa = DeletedCoupleUuCm
			/ (ThrustArmCm * BedJoint.InterfaceAreaSqCm * DestructionForce::ForceUnitsPerMPaSqCm);

		if (!(ThrustDemandMPa <= SlidingCapacityMPa))
		{
			continue;
		}

		return true;
	}

	return false;
}

FStructure::EEquilibriumGateDisposition FStructure::BreakByEquilibrium(int32 Pass)
{
	// Cleared per pass so a declined pass cannot serve an earlier pass's reading.
	ConnectionReadoutCache.Reset();

	// Size cap keeps synchronous LP authority off the flagship scenarios (PROMOTION_DESIGN.md §12 D6-c).
	if (NumPieces() > EquilibriumGateBlockCap)
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	// The LP needs centres and rectangles; geometry-free structures (the fuzzers) stay on the router.
	if (!HasCompleteGeometry())
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * Feasibility at lambda = 1 (PROMOTION_DESIGN §12 D6-b): same verdict as lambda*, 5-16x
	 * cheaper, and the infeasible arm yields a Farkas-verified mechanism. Refusal declines.
	 */
	RigidBlockOracle::FOracleProblem Problem;
	FString WhyNot;

	if (!RigidBlockOracle::BuildRigidBlockProblem(*this, Problem, WhyNot))
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * 2 for an X-Z pose, 3 for volumetric (the bridge may pose a 3D build in-plane). A declining
	 * pass leaves the previous value.
	 */
	LastEquilibriumProblemDim = Problem.Dim == RigidBlockOracle::EOracleDim::Dim3D ? 3 : 2;

	Problem.bGravityIsLive = false;

	/*
	 * First-crack rows: bonded joints (f_t > 0) crack at their elastic limit, 3x stricter than the
	 * plastic form. Dry joints are unchanged.
	 */
	Problem.bFirstCrackRows = true;

	const RigidBlockOracle::FOracleResult Result = RigidBlockOracle::SolveRigidBlock(Problem);
	const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Result);

	if (Outcome == RigidBlockOracle::EOracleOutcome::Unanswerable)
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	if (Outcome == RigidBlockOracle::EOracleOutcome::Stands)
	{
		/*
		 * Every piece is held, including ones the router would strand in a knot (rows 10 and 19).
		 * The only effect is writing LP support.
		 */
		ApplyLimitAnalysisSupport(Problem, Result);
		CacheMinViolationReadout(Problem);
		return EEquilibriumGateDisposition::AuthoritativeNoBreak;
	}

	// Falls always carries a certified mechanism; the guard is fail-closed defence.
	const RigidBlockOracle::FOracleMechanism& Mechanism = Result.Mechanism;

	if (!Mechanism.bPresent || !Mechanism.bIsCertified)
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * The mechanism is the sole break authority (PROMOTION_DESIGN.md §12 D7 3b): sever each intact
	 * joint it opens or slides, stamped with this pass. Skipping given joints ends the cascade.
	 */
	ApplyLimitAnalysisSupport(Problem, Result);

	bool bSeveredThisPass = false;

	for (int32 Joint = 0; Joint < Mechanism.JointOpensOrSlides.Num(); ++Joint)
	{
		if (!Mechanism.JointOpensOrSlides[Joint])
		{
			continue;
		}

		if (!Problem.ConnectionOfJoint.IsValidIndex(Joint))
		{
			continue;
		}

		const int32 Connection = Problem.ConnectionOfJoint[Joint];

		if (!Connections.IsValidIndex(Connection) || Connections[Connection].HasGiven())
		{
			continue;
		}

		Connections[Connection].Sever();
		ConnectionBreakPass[Connection] = Pass;
		ConnectionBreakAuthority[Connection] = 1;
		bSeveredThisPass = true;
	}

	// Readout only on the settled (non-breaking) pass; a severing pass would be overwritten.
	if (!bSeveredThisPass)
	{
		CacheMinViolationReadout(Problem);
	}

	return bSeveredThisPass
		? EEquilibriumGateDisposition::AuthoritativeBroke
		: EEquilibriumGateDisposition::AuthoritativeNoBreak;
}

void FStructure::CacheMinViolationReadout(const RigidBlockOracle::FOracleProblem& Problem)
{
	/*
	 * Strain readout (PROMOTION_DESIGN.md §3.5): the min-violation LP on a copy, equilibrium hard and
	 * strength rows as penalised slacks. Writes only this cache. First-crack rows match the break
	 * authority.
	 */

	// Test observable: readout LPs per cascade.
	++MinViolationReadoutSolves;

	ConnectionReadoutCache.Init(FConnectionReadout{}, Connections.Num());

	RigidBlockOracle::FOracleProblem ReadoutProblem = Problem;
	ReadoutProblem.bMinViolationReadout = true;
	ReadoutProblem.bFirstCrackRows = true;

	const RigidBlockOracle::FOracleResult ReadoutResult = RigidBlockOracle::SolveRigidBlock(ReadoutProblem);

	if (!ReadoutResult.Readout.bPresent)
	{
		return;
	}

	// Map each oracle joint back to its connection; skip any without provenance.
	for (int32 Joint = 0; Joint < ReadoutResult.Readout.Joints.Num(); ++Joint)
	{
		if (!ReadoutProblem.ConnectionOfJoint.IsValidIndex(Joint))
		{
			continue;
		}

		const int32 Connection = ReadoutProblem.ConnectionOfJoint[Joint];

		if (!ConnectionReadoutCache.IsValidIndex(Connection))
		{
			continue;
		}

		const RigidBlockOracle::FOracleJointReadout& JointReadout = ReadoutResult.Readout.Joints[Joint];

		FConnectionReadout& Entry = ConnectionReadoutCache[Connection];
		Entry.bPresent = true;
		Entry.NormalUu = JointReadout.NormalUu;
		Entry.MomentUuCm = JointReadout.MomentUuCm;
		Entry.ViolationUu = JointReadout.ViolationUu;
		Entry.Utilisation = JointReadout.Utilisation;
	}
}

void FStructure::ApplyLimitAnalysisSupport(
	const RigidBlockOracle::FOracleProblem& Problem,
	const RigidBlockOracle::FOracleResult& Result)
{
	/*
	 * Overwrite each bridged piece's support with the LP verdict (PROMOTION_DESIGN.md §12 D7 3b):
	 * moved pieces fall, the rest are held. Unbridged pieces keep the router's answer.
	 */
	const RigidBlockOracle::FOracleMechanism& Mechanism = Result.Mechanism;

	for (int32 Block = 0; Block < Problem.PieceOfBlock.Num(); ++Block)
	{
		const int32 Piece = Problem.PieceOfBlock[Block];

		if (!PieceSupported.IsValidIndex(Piece))
		{
			continue;
		}

		const bool bMoves = Mechanism.bPresent
			&& Mechanism.Blocks.IsValidIndex(Block)
			&& Mechanism.Blocks[Block].bMoves;

		PieceSupported[Piece] = !bMoves;
		PieceStranded[Piece] = false;
	}
}

bool FStructure::BreakByCapacitySweep(int32 Pass)
{
	/*
	 * Per-joint capacity sweep: every joint over capacity gives this pass (DESIGN.md §3). The break
	 * authority whenever the equilibrium gate declines.
	 */
	bool bBroke = false;

	for (int32 Index = 0; Index < Connections.Num(); ++Index)
	{
		// By reference: a copy would latch on a temporary and nothing would break.
		FConnection& Connection = Connections[Index];

		// Skipping given joints is what lets the cascade terminate.
		if (Connection.HasGiven())
		{
			continue;
		}

		/*
		 * Same inputs as GetConnectionUtilisation (force, moment, composite depth) so break and
		 * readout agree, with the weakest-link strength. The copy decides; the real joint severs.
		 */
		FConnection Paired = Connection;
		Paired.Strength = EffectiveJointStrength(Index);
		Paired.ApplyForce(
			ConnectionForces[Index], ConnectionMoments[Index],
			ConnectionCompositeDepthCm[Index]);

		if (Paired.HasGiven())
		{
			Connection.Sever();
			ConnectionBreakPass[Index] = Pass;
			ConnectionBreakAuthority[Index] = 2;
			bBroke = true;
		}
	}

	return bBroke;
}

int32 FStructure::SolveAndBreak()
{
	/*
	 * Each pass is a solve plus one break step; everything over capacity gives together
	 * (DESIGN.md §3). Order between passes is the collapse sequence.
	 */
	int32 BreakingPasses = 0;

	ConnectionReadoutCache.Reset();

	MinViolationReadoutSolves = 0;

	/*
	 * Pass numbers are global: continue from the highest existing stamp so later breaks carry
	 * larger numbers. Unstamped joints are -1 and cannot raise it.
	 */
	int32 PassesAlreadyStamped = 0;
	for (const int32 Stamp : ConnectionBreakPass)
	{
		PassesAlreadyStamped = FMath::Max(PassesAlreadyStamped, Stamp);
	}

	auto IsLiveInStructure = [this](int32 Piece)
	{
		return Pieces.IsValidIndex(Piece) && !IsPieceRemoved(Piece) && Pieces[Piece].bIsInTheStructure;
	};

	/*
	 * Regional prover seed (REGIONAL_PROVER_PLAN.md §1). First pass: live neighbours of removed
	 * pieces. Later: live endpoints of joints broken last pass.
	 */
	auto DeriveRegionalSeed = [this, &IsLiveInStructure](int32 Pass, bool bFirstPass) -> TArray<int32>
	{
		TSet<int32> SeedSet;

		if (bFirstPass)
		{
			for (int32 Piece = 0; Piece < Pieces.Num(); ++Piece)
			{
				if (Pieces[Piece].bIsInTheStructure)
				{
					continue;
				}

				for (const FConnection& Connection : Connections)
				{
					const int32 Other = OtherEndOf(Connection, Piece);

					if (IsLiveInStructure(Other))
					{
						SeedSet.Add(Other);
					}
				}
			}
		}
		else
		{
			for (int32 Index = 0; Index < Connections.Num(); ++Index)
			{
				if (!ConnectionBreakPass.IsValidIndex(Index) || ConnectionBreakPass[Index] != Pass - 1)
				{
					continue;
				}

				for (const int32 End : { Connections[Index].PieceA, Connections[Index].PieceB })
				{
					if (IsLiveInStructure(End))
					{
						SeedSet.Add(End);
					}
				}
			}
		}

		return SeedSet.Array();
	};

	// Intact joints only decrease, so this is the cascade's progress measure.
	auto CountIntactJoints = [this]() -> int32
	{
		int32 Intact = 0;
		for (const FConnection& Connection : Connections)
		{
			if (!Connection.HasGiven())
			{
				++Intact;
			}
		}
		return Intact;
	};

	// Live, answered pieces that are Falling or Stranded.
	auto CountNotHeld = [this]() -> int32
	{
		int32 NotHeld = 0;
		for (int32 Piece = 0; Piece < Pieces.Num(); ++Piece)
		{
			if (IsPieceRemoved(Piece) || !HasSupportAnswer(Piece))
			{
				continue;
			}
			const EPieceSupport Support = GetPieceSupport(Piece);
			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				++NotHeld;
			}
		}
		return NotHeld;
	};

	// Report only; the cascade never reads it back.
	LastSolveAndBreakReport = FSolveAndBreakReport();
	LastSolveAndBreakReport.LivePiecesBefore = NumLivePieces();
	LastSolveAndBreakReport.IntactJointsBefore = CountIntactJoints();

	const double CascadeStartSeconds = FPlatformTime::Seconds();

	for (;;)
	{
		const double PassStartSeconds = FPlatformTime::Seconds();

		SolveLoads();

		const int32 Pass = PassesAlreadyStamped + BreakingPasses + 1;
		bool bBrokeThisPass = false;

		FBreakPassReport& Report = LastSolveAndBreakReport.Passes.AddDefaulted_GetRef();
		Report.Pass = Pass;
		Report.Solve = LastSolveLoadsProfile;

		const double GateStartSeconds = FPlatformTime::Seconds();
		const int32 IntactBeforeGate = CountIntactJoints();

		/*
		 * Below the block cap the rigid-block LP is the sole break authority (DESIGN.md §7 step 4,
		 * PROMOTION_DESIGN.md §6 Slice 3). When it declines, the capacity sweep decides.
		 */
		const EEquilibriumGateDisposition Gate = BreakByEquilibrium(Pass);

		Report.GateMs = (FPlatformTime::Seconds() - GateStartSeconds) * 1000.0;
		Report.GateDisposition = static_cast<int32>(Gate);
		Report.JointsSeveredByGate = IntactBeforeGate - CountIntactJoints();

		if (Gate == EEquilibriumGateDisposition::AuthoritativeBroke)
		{
			bBrokeThisPass = true;
		}
		else if (Gate == EEquilibriumGateDisposition::DeclinedToRouter)
		{
			const double SweepStartSeconds = FPlatformTime::Seconds();
			const int32 IntactBeforeSweep = CountIntactJoints();

			bBrokeThisPass = BreakByCapacitySweep(Pass);

			Report.CapacitySweepMs = (FPlatformTime::Seconds() - SweepStartSeconds) * 1000.0;
			Report.JointsGivenToSweep = IntactBeforeSweep - CountIntactJoints();

			/*
			 * Regional prover (REGIONAL_PROVER_PLAN.md §4): the sweep cannot see a global mechanism,
			 * so a local grounded-boundary LP may prove one. It only moves answers toward Falling.
			 */
			if (HasCompleteGeometry())
			{
				/*
				 * Progress is a severed joint, not a Falling count: a disconnected block moves in every
				 * re-pose, so counting Falling would loop forever.
				 */
				const int32 IntactBefore = CountIntactJoints();
				const double ProverStartSeconds = FPlatformTime::Seconds();

				Report.PiecesFelledByProver = ProveRegionalCollapse(
					DeriveRegionalSeed(Pass, /*bFirstPass*/ BreakingPasses == 0), RegionalProverBlockCap, Pass);

				Report.RegionalProverMs = (FPlatformTime::Seconds() - ProverStartSeconds) * 1000.0;
				Report.RegionalPoses = LastProverPoses;
				Report.RegionalLpPivots = LastProverLpPivots;
				Report.RegionalLpMs = LastProverLpMs;
				Report.RegionalLastBlocks = LastProverPoses > 0 ? LastRegionalProblemBlockCount : INDEX_NONE;
				Report.RegionalPoseBreakdown = LastProverPoseBreakdown;
				Report.bRegionalFell = bLastProverFell;
				Report.JointsSeveredByProver = IntactBefore - CountIntactJoints();

				if (CountIntactJoints() < IntactBefore)
				{
					bBrokeThisPass = true;
				}
			}
		}

		Report.LivePieces = NumLivePieces();
		Report.IntactJointsAfter = CountIntactJoints();
		Report.NotHeldAfter = CountNotHeld();
		Report.PassMs = (FPlatformTime::Seconds() - PassStartSeconds) * 1000.0;

		UE_LOG(LogDestructionSolve, Verbose,
			TEXT("SolveAndBreak pass %d: solve %.2f ms (%d fixpoint iterations), gate %.2f ms (disposition %d, severed %d), ")
			TEXT("sweep %.2f ms gave %d, prover %.2f ms (%d poses, %d pivots, %.2f ms LP, last %d blocks, fell %d) ")
			TEXT("severed %d felled %d; after: %d live, %d intact joints, %d not held; pass %.2f ms"),
			Pass, Report.Solve.TotalMs, Report.Solve.FixpointIterations, Report.GateMs, Report.GateDisposition,
			Report.JointsSeveredByGate, Report.CapacitySweepMs, Report.JointsGivenToSweep, Report.RegionalProverMs, Report.RegionalPoses,
			Report.RegionalLpPivots, Report.RegionalLpMs, Report.RegionalLastBlocks, Report.bRegionalFell ? 1 : 0,
			Report.JointsSeveredByProver, Report.PiecesFelledByProver, Report.LivePieces, Report.IntactJointsAfter,
			Report.NotHeldAfter, Report.PassMs);

		// A pass that breaks nothing is the settled state and is not counted. Joints never heal.
		if (!bBrokeThisPass)
		{
			break;
		}

		++BreakingPasses;
	}

	LastSolveAndBreakReport.BreakingPasses = BreakingPasses;
	LastSolveAndBreakReport.IntactJointsAfter = CountIntactJoints();
	LastSolveAndBreakReport.TotalMs = (FPlatformTime::Seconds() - CascadeStartSeconds) * 1000.0;

	// Log level only when something broke or it took longer than a frame.
	if (BreakingPasses > 0 || LastSolveAndBreakReport.TotalMs > 50.0)
	{
		UE_LOG(LogDestructionSolve, Log,
			TEXT("SolveAndBreak: %d breaking pass(es) of %d in %.1f ms on %d live pieces; intact joints %d -> %d"),
			BreakingPasses, LastSolveAndBreakReport.Passes.Num(), LastSolveAndBreakReport.TotalMs,
			LastSolveAndBreakReport.LivePiecesBefore, LastSolveAndBreakReport.IntactJointsBefore,
			LastSolveAndBreakReport.IntactJointsAfter);
	}

	return BreakingPasses;
}

const FStructure::FSolveAndBreakReport& FStructure::GetLastSolveAndBreakReport() const
{
	return LastSolveAndBreakReport;
}

const FStructure::FSolveLoadsProfile& FStructure::GetLastSolveLoadsProfile() const
{
	return LastSolveLoadsProfile;
}

int32 FStructure::SolveAndBreak_WithRegionalProver(const TArray<int32>& Seed, int32 RegionBlockCap)
{
	// Test entry (REGIONAL_PROVER_PLAN.md slice 1): settle the router, then run the prover.
	SolveLoads();
	return ProveRegionalCollapse(Seed, RegionBlockCap);
}

int32 FStructure::ProveRegionalCollapse(const TArray<int32>& Seed, int32 RegionBlockCap, int32 BreakPass)
{
	/*
	 * Regional collapse prover (REGIONAL_PROVER_PLAN.md §§1-4). A grounded-boundary LP over the
	 * disturbance's neighbourhood. Pinning the frontier only adds support, so it can prove a fall
	 * but never a stand. The caller has already solved.
	 */

	// Intact joints per piece, ascending index, so the flood never crosses a break.
	TArray<TArray<int32>> PieceJoints;
	PieceJoints.SetNum(Pieces.Num());

	for (int32 Index = 0; Index < Connections.Num(); ++Index)
	{
		const FConnection& Connection = Connections[Index];

		if (Connection.HasGiven())
		{
			continue;
		}

		if (PieceJoints.IsValidIndex(Connection.PieceA))
		{
			PieceJoints[Connection.PieceA].Add(Index);
		}

		if (PieceJoints.IsValidIndex(Connection.PieceB))
		{
			PieceJoints[Connection.PieceB].Add(Index);
		}
	}

	auto IsLiveInStructure = [this](int32 Piece)
	{
		return Pieces.IsValidIndex(Piece) && !IsPieceRemoved(Piece) && Pieces[Piece].bIsInTheStructure;
	};

	/*
	 * Region and its grounded one-hop boundary. A candidate is admitted only if region plus boundary
	 * still fits the budget; bounding the region alone lets the ring overspend.
	 */
	TSet<int32> Region;
	TSet<int32> Boundary;

	auto AdmitToRegion = [&](int32 Candidate, int32 Budget) -> bool
	{
		if (!IsLiveInStructure(Candidate) || Region.Contains(Candidate))
		{
			return false;
		}

		TSet<int32> NewRingPieces;

		for (const int32 Index : PieceJoints[Candidate])
		{
			const int32 Other = OtherEndOf(Connections[Index], Candidate);

			if (IsLiveInStructure(Other) && Other != Candidate && !Region.Contains(Other)
				&& !Boundary.Contains(Other))
			{
				NewRingPieces.Add(Other);
			}
		}

		const int32 ProspectiveRegion = Region.Num() + 1;
		const int32 ProspectiveBoundary =
			Boundary.Num() - (Boundary.Contains(Candidate) ? 1 : 0) + NewRingPieces.Num();

		if (ProspectiveRegion + ProspectiveBoundary > Budget)
		{
			return false;
		}

		Region.Add(Candidate);
		Boundary.Remove(Candidate);

		for (const int32 RingPiece : NewRingPieces)
		{
			Boundary.Add(RingPiece);
		}

		return true;
	};

	/*
	 * BFS from the seeds up to Budget; returns the count admitted. An unadmitted seed is dropped
	 * (grounding it would pin the collapse); an unadmitted neighbour becomes boundary.
	 */
	auto GrowFrom = [&](const TArray<int32>& FrontierSeeds, int32 Budget) -> int32
	{
		const int32 Before = Region.Num();
		TArray<int32> Active;

		for (const int32 SeedPiece : FrontierSeeds)
		{
			if (AdmitToRegion(SeedPiece, Budget))
			{
				Active.Add(SeedPiece);
			}
		}

		for (int32 Head = 0; Head < Active.Num(); ++Head)
		{
			const int32 Piece = Active[Head];

			for (const int32 Index : PieceJoints[Piece])
			{
				const int32 Other = OtherEndOf(Connections[Index], Piece);

				if (!IsLiveInStructure(Other) || Region.Contains(Other))
				{
					continue;
				}

				if (AdmitToRegion(Other, Budget))
				{
					Active.Add(Other);
				}
				else
				{
					Boundary.Add(Other);
				}
			}
		}

		return Region.Num() - Before;
	};

	// TSet has no operator==.
	auto RegionsMatch = [](const TSet<int32>& A, const TSet<int32>& B) -> bool
	{
		if (A.Num() != B.Num())
		{
			return false;
		}

		for (const int32 Piece : A)
		{
			if (!B.Contains(Piece))
			{
				return false;
			}
		}

		return true;
	};

	/*
	 * Start small, well under the cap: a cap-sized first pose (~200 blocks every pass) made the
	 * flagship 3D scenarios impractical. Seed footprint plus a ring, at least 16.
	 */
	const int32 InitialBudget = FMath::Min(RegionBlockCap, FMath::Max(16, Seed.Num() + 8));
	int32 EffectiveBudget = InitialBudget;

	GrowFrom(Seed, EffectiveBudget);

	/*
	 * Grow-on-contact (REGIONAL_PROVER_PLAN.md slice 3). An interior certified fall is stitched; a
	 * fall touching an artificial boundary re-floods from the moved set; no fall widens a bounded
	 * speculative search. The budget only grows, so this settles in a few solves.
	 */
	RigidBlockOracle::FOracleProblem Problem;
	RigidBlockOracle::FOracleResult Result;
	bool bLastPoseFell = false;

	// Pass-report observables.
	LastProverPoses = 0;
	LastProverLpPivots = 0;
	LastProverLpMs = 0.0;
	bLastProverFell = false;
	LastProverJointsSevered = 0;
	LastProverPoseBreakdown.Reset();

	const int32 MaxGrowIterations = Pieces.Num() + 4;

	for (int32 Iteration = 0; Iteration < MaxGrowIterations; ++Iteration)
	{
		// Same pose as BreakByEquilibrium. A bridge refusal fails closed to the router's answer.
		Problem = RigidBlockOracle::FOracleProblem();
		FString WhyNot;

		if (!RigidBlockOracle::BuildRegionalProblem(*this, Region, Boundary, Problem, WhyNot))
		{
			return 0;
		}

		LastRegionalProblemBlockCount = Problem.Blocks.Num();

		Problem.bGravityIsLive = false;
		Problem.bFirstCrackRows = true;

		const double LpStartSeconds = FPlatformTime::Seconds();

		Result = RigidBlockOracle::SolveRigidBlock(Problem);

		// Measured once so the per-pose times sum to LastProverLpMs.
		const double PoseMs = (FPlatformTime::Seconds() - LpStartSeconds) * 1000.0;

		++LastProverPoses;
		LastProverLpPivots += Result.SimplexIterations;
		LastProverLpMs += PoseMs;

		const bool bCertifiedFall =
			RigidBlockOracle::OutcomeOf(Result) == RigidBlockOracle::EOracleOutcome::Falls
			&& Result.Mechanism.bPresent && Result.Mechanism.bIsCertified;

		bLastPoseFell = bCertifiedFall;

		// One record per pose, in lockstep with LastProverPoses.
		FProverPoseReport& Pose = LastProverPoseBreakdown.AddDefaulted_GetRef();
		Pose.Blocks = Problem.Blocks.Num();
		Pose.LpPivots = Result.SimplexIterations;
		Pose.LpMs = PoseMs;
		Pose.bFell = bCertifiedFall;

		/*
		 * A fall re-floods from the moved set up to RegionBlockCap. No fall searches from the whole
		 * boundary up to a lower speculative ceiling; a mechanism beyond it is an accepted miss
		 * (REGIONAL_PROVER_PLAN.md §1).
		 */
		const int32 RegionSpeculativeCeiling = FMath::Min(RegionBlockCap, 48);

		TArray<int32> GrowFrontier;
		int32 GrowCeiling = RegionBlockCap;
		bool bReFloodFromMechanism = false;

		if (bCertifiedFall)
		{
			TSet<int32> MovedPieces;

			for (int32 Block = 0; Block < Result.Mechanism.Blocks.Num(); ++Block)
			{
				if (Result.Mechanism.Blocks[Block].bMoves && Problem.PieceOfBlock.IsValidIndex(Block))
				{
					MovedPieces.Add(Problem.PieceOfBlock[Block]);
				}
			}

			/*
			 * Does a moved block touch an artificially grounded boundary block (not a real
			 * foundation)? If not, the mechanism is interior and complete.
			 */
			bool bContact = false;

			for (const int32 BoundaryPiece : Boundary)
			{
				if (!Pieces.IsValidIndex(BoundaryPiece) || Pieces[BoundaryPiece].bIsGrounded)
				{
					continue;
				}

				for (const int32 Index : PieceJoints[BoundaryPiece])
				{
					if (MovedPieces.Contains(OtherEndOf(Connections[Index], BoundaryPiece)))
					{
						bContact = true;
						break;
					}
				}

				if (bContact)
				{
					break;
				}
			}

			if (!bContact)
			{
				break;
			}

			// Re-flooding from the moved set prunes disconnected standing components.
			GrowFrontier = MovedPieces.Array();
			bReFloodFromMechanism = true;
		}
		else
		{
			GrowFrontier = Boundary.Array();
			GrowCeiling = RegionSpeculativeCeiling;
		}

		// Deterministic admission order.
		GrowFrontier.Sort();

		// At the ceiling: stitch what fell (the router keeps the rest), or accept the miss.
		if (EffectiveBudget >= GrowCeiling)
		{
			break;
		}

		if (bReFloodFromMechanism)
		{
			/*
			 * Budget = moved set plus two rings: one it must be free to move into, one to pin it.
			 * Budgeting only the first would pin it and hide a deeper collapse. An unchanged region
			 * is a fixpoint.
			 */
			TSet<int32> MovableRing(GrowFrontier);

			for (const int32 Piece : GrowFrontier)
			{
				for (const int32 Index : PieceJoints[Piece])
				{
					MovableRing.Add(OtherEndOf(Connections[Index], Piece));
				}
			}

			TSet<int32> PinnedRing = MovableRing;

			for (const int32 Piece : MovableRing.Array())
			{
				for (const int32 Index : PieceJoints[Piece])
				{
					PinnedRing.Add(OtherEndOf(Connections[Index], Piece));
				}
			}

			EffectiveBudget = FMath::Min(GrowCeiling, PinnedRing.Num());

			TSet<int32> PriorRegion = Region;
			Region.Reset();
			Boundary.Reset();
			GrowFrom(GrowFrontier, EffectiveBudget);

			if (RegionsMatch(Region, PriorRegion))
			{
				break;
			}
		}
		else
		{
			// No mechanism to size to: double the budget.
			EffectiveBudget = FMath::Min(GrowCeiling, EffectiveBudget * 2);

			if (GrowFrom(GrowFrontier, EffectiveBudget) == 0)
			{
				break;
			}
		}
	}

	bLastProverFell = bLastPoseFell;

	if (!bLastPoseFell)
	{
		return 0;
	}

	const RigidBlockOracle::FOracleMechanism& Mechanism = Result.Mechanism;

	/*
	 * Falling-only stitch: mark moved blocks Falling. Never Supported; a regional stand proves
	 * nothing (case-21).
	 */
	int32 Released = 0;

	for (int32 Block = 0; Block < Mechanism.Blocks.Num(); ++Block)
	{
		if (!Mechanism.Blocks[Block].bMoves)
		{
			continue;
		}

		if (!Problem.PieceOfBlock.IsValidIndex(Block))
		{
			continue;
		}

		const int32 Piece = Problem.PieceOfBlock[Block];

		if (!PieceSupported.IsValidIndex(Piece))
		{
			continue;
		}

		PieceSupported[Piece] = false;
		PieceStranded[Piece] = false;
		++Released;
	}

	// Sever the intact joints the mechanism opens, as BreakByEquilibrium does.
	for (int32 Joint = 0; Joint < Mechanism.JointOpensOrSlides.Num(); ++Joint)
	{
		if (!Mechanism.JointOpensOrSlides[Joint])
		{
			continue;
		}

		if (!Problem.ConnectionOfJoint.IsValidIndex(Joint))
		{
			continue;
		}

		const int32 Connection = Problem.ConnectionOfJoint[Joint];

		if (!Connections.IsValidIndex(Connection) || Connections[Connection].HasGiven())
		{
			continue;
		}

		Connections[Connection].Sever();
		ConnectionBreakPass[Connection] = BreakPass;
		ConnectionBreakAuthority[Connection] = 3;
		++LastProverJointsSevered;
	}

	return Released;
}

int32 FStructure::GetLastRegionalProblemBlockCount() const
{
	return LastRegionalProblemBlockCount;
}

int32 FStructure::GetLastEquilibriumProblemDim() const
{
	return LastEquilibriumProblemDim;
}

void FStructure::SetEquilibriumGateBlockCap(int32 MaxBlocks)
{
	EquilibriumGateBlockCap = MaxBlocks;
}

void FStructure::SetRegionBlockCap(int32 MaxBlocks)
{
	RegionalProverBlockCap = MaxBlocks;
}

void FStructure::SetPieceMaterial(int32 PieceIndex, const DestructionProfiles::FMaterialProfile* Material)
{
	if (Pieces.IsValidIndex(PieceIndex))
	{
		Pieces[PieceIndex].Material = Material;
	}
}

void FStructure::SetThreeDimensional(bool bIsThreeDimensional)
{
	bThreeDimensional = bIsThreeDimensional;
}

bool FStructure::IsThreeDimensional() const
{
	return bThreeDimensional;
}

int32 FStructure::GetBreakPass(int32 ConnectionIndex) const
{
	return ConnectionBreakPass.IsValidIndex(ConnectionIndex)
		? ConnectionBreakPass[ConnectionIndex]
		: INDEX_NONE;
}

int32 FStructure::GetBreakAuthority(int32 ConnectionIndex) const
{
	// 1 gate, 2 sweep, 3 prover; INDEX_NONE if unknown or unbroken.
	return ConnectionBreakAuthority.IsValidIndex(ConnectionIndex)
		? ConnectionBreakAuthority[ConnectionIndex]
		: INDEX_NONE;
}

EJointRole FStructure::GetJointRole(int32 ConnectionIndex, int32 PieceIndex) const
{
	/*
	 * The rule SolveLoads routes by. The normal, turned toward the asked piece: mostly up is
	 * BedBeneath, mostly down BedAbove, else Head. An unknown connection needs its own guard: the
	 * placeholder's (0,0,1) normal would read BedBeneath.
	 */
	if (!Connections.IsValidIndex(ConnectionIndex))
	{
		return EJointRole::None;
	}

	const FConnection& Connection = Connections[ConnectionIndex];

	FVector UnitNormal = Connection.InterfaceNormal;
	if (!UnitNormal.Normalize())
	{
		return EJointRole::None;
	}

	double NormalZTowardPiece = 0.0;
	if (Connection.PieceB == PieceIndex)
	{
		NormalZTowardPiece = UnitNormal.Z;
	}
	else if (Connection.PieceA == PieceIndex)
	{
		NormalZTowardPiece = -UnitNormal.Z;
	}
	else
	{
		return EJointRole::None;
	}

	if (!(FMath::Abs(NormalZTowardPiece) > SolverBedJointCosine))
	{
		return EJointRole::Head;
	}

	return NormalZTowardPiece > 0.0 ? EJointRole::BedBeneath : EJointRole::BedAbove;
}

FVector FStructure::GetConnectionForce(int32 ConnectionIndex) const
{
	// Zero for an unknown handle or a connection no solve reached.
	return ConnectionForces.IsValidIndex(ConnectionIndex)
		? ConnectionForces[ConnectionIndex]
		: FVector::ZeroVector;
}

FVector FStructure::GetConnectionMoment(int32 ConnectionIndex) const
{
	return ConnectionMoments.IsValidIndex(ConnectionIndex)
		? ConnectionMoments[ConnectionIndex]
		: FVector::ZeroVector;
}

double FStructure::GetConnectionCompositeDepthCm(int32 ConnectionIndex) const
{
	// Zero fails closed: a depth is a relief, so none credited reads the joint as more loaded.
	return ConnectionCompositeDepthCm.IsValidIndex(ConnectionIndex)
		? ConnectionCompositeDepthCm[ConnectionIndex]
		: 0.0;
}

double FStructure::GetConnectionUtilisation(int32 ConnectionIndex) const
{
	// Delegates to the same evaluator and inputs the break decision uses, with weakest-link strength.
	FConnection Paired = GetConnection(ConnectionIndex);
	Paired.Strength = EffectiveJointStrength(ConnectionIndex);

	return Paired.UtilisationUnder(
		GetConnectionForce(ConnectionIndex),
		GetConnectionMoment(ConnectionIndex),
		GetConnectionCompositeDepthCm(ConnectionIndex));
}

FConnectionStrength FStructure::EffectiveJointStrength(int32 ConnectionIndex) const
{
	// Per axis min(connection, matA, matB) when both faces name a material; else the bare connection.
	const FConnection& Connection = GetConnection(ConnectionIndex);

	const DestructionProfiles::FMaterialProfile* MaterialA = GetPiece(Connection.PieceA).Material;
	const DestructionProfiles::FMaterialProfile* MaterialB = GetPiece(Connection.PieceB).Material;

	if (MaterialA == nullptr || MaterialB == nullptr)
	{
		return Connection.Strength;
	}

	return DestructionForce::EffectiveBondedStrength(Connection.Strength, *MaterialA, *MaterialB);
}

FStructure::FConnectionReadout FStructure::GetConnectionReadout(int32 ConnectionIndex) const
{
	// Absent above the cap, before any solve, or for an unknown handle.
	return ConnectionReadoutCache.IsValidIndex(ConnectionIndex)
		? ConnectionReadoutCache[ConnectionIndex]
		: FConnectionReadout{};
}

int32 FStructure::GetMinViolationReadoutSolveCount() const
{
	return MinViolationReadoutSolves;
}

bool FStructure::IsPieceSupported(int32 PieceIndex) const
{
	return PieceSupported.IsValidIndex(PieceIndex) && PieceSupported[PieceIndex];
}

EPieceSupport FStructure::GetPieceSupport(int32 PieceIndex) const
{
	// Derived from IsPieceSupported so the two always agree.
	if (IsPieceSupported(PieceIndex))
	{
		return GetPiece(PieceIndex).bIsGrounded ? EPieceSupport::Grounded : EPieceSupport::Supported;
	}

	// Stranded only for a piece the last solve found in a knot; everything else is Falling.
	return PieceStranded.IsValidIndex(PieceIndex) && PieceStranded[PieceIndex]
		? EPieceSupport::Stranded
		: EPieceSupport::Falling;
}

bool FStructure::HasSupportAnswer(int32 PieceIndex) const
{
	// SolveLoads sizes PieceSupported, so its extent says whether this piece has been solved.
	return PieceSupported.IsValidIndex(PieceIndex);
}
