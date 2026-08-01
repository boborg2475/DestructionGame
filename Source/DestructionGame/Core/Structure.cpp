// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Structure.h"

namespace
{
	/**
	 * Unreal's gravity, 980 cm/s2.
	 *
	 * Mass is already kilograms and length is already centimetres, so MassKg * 980
	 * IS a force in Unreal units — the 1 N = 100 uu conversion of DESIGN.md §3 is
	 * baked into the 980 rather than applied on top of it. Multiplying by 100 a
	 * second time here is the standard way to be wrong by exactly 100x.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * Where the line between a bed joint and a head joint sits: cos(45 degrees).
	 *
	 * DESIGN.md §3 makes support two-tiered, and the tier turns on the interface
	 * normal being "substantially vertical". This is the only threshold that prefers
	 * neither tier — a joint bears when its normal is nearer vertical than
	 * horizontal — and it needs no material data, which matters because the tier is
	 * decided before any strength profile is consulted. A friction-based line
	 * (arctan mu) would be more physical but would make the load path depend on the
	 * connection profile, and it belongs with the generalisation to non-gravity load
	 * directions that DESIGN.md §3 already flags as outstanding.
	 */
	constexpr double BedJointCosine = 0.70710678118654752440;

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

	/** What a connection is TO ONE OF ITS PIECES — the relation is directed. */
	enum class EJointRole : uint8
	{
		/** Not a joint on this piece at all. */
		None,

		/** Substantially vertical normal, other piece BELOW: this bears the weight. */
		BedBeneath,

		/** Substantially vertical normal, other piece ABOVE: it rests on this one. */
		BedAbove,

		/** Substantially horizontal normal: can only carry weight in shear. */
		Head,
	};

	/**
	 * Which of those a connection is, looked at from PieceIndex.
	 *
	 * A connection is described by a normal pointing toward PieceB, so the first
	 * thing to do is turn it to point at the piece being asked about. Pointing
	 * substantially UP at this piece, the interface is beneath it and bears it.
	 * Pointing substantially DOWN, the bed joint is above — something resting on
	 * this piece, or something this piece is glued underneath — and neither holds it
	 * up. That direction is the whole correction: routing that only asked whether
	 * two pieces were joined let a load path run upward through a joint.
	 *
	 * A normal that will not normalise is answered None rather than being given a
	 * tier. Normalize returns false for a zero-length AND for a NaN normal, so the
	 * comparison below is never reached with a NaN in it — the guard is here rather
	 * than in the comparison because a NaN falling through would land in Head, and
	 * Head is a support tier.
	 */
	EJointRole RoleOf(const FConnection& Connection, int32 PieceIndex)
	{
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

		if (!(FMath::Abs(NormalZTowardPiece) > BedJointCosine))
		{
			return EJointRole::Head;
		}

		return NormalZTowardPiece > 0.0 ? EJointRole::BedBeneath : EJointRole::BedAbove;
	}

	/**
	 * Does the load leaving this piece come back round to it?
	 *
	 * That, and nothing wider, is what "caught in an unroutable knot" means: a piece
	 * that is ultimately one of its own supports cannot have its load divided without
	 * a rule for going round a loop, which DESIGN.md §3 says we do not have.
	 *
	 * IT IS DELIBERATELY NOT "the ordering could not reach this piece". Kahn orders
	 * TOP-DOWN, so a piece comes out unordered whenever a knot sits anywhere ABOVE
	 * it — which strands the foundation under a knot and then, on the next pass,
	 * everything resting on that foundation. Un-orderability is a solver artefact;
	 * being your own support is a statement about the structure.
	 *
	 * The walk STOPS AT A GROUNDED PIECE, because the earth absorbs what arrives and
	 * passes nothing on. Walking through one would find a loop in the most ordinary
	 * shape there is — two bricks side by side on the ground, each naming the other
	 * through the head joint between them.
	 *
	 * It walks LoadPaths rather than the raw support lists, so it sees exactly the
	 * edges the accumulation does: supports that themselves reach the ground. The
	 * starting piece is deliberately left unvisited so that arriving back at it is
	 * detected rather than quietly skipped.
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
	/*
	 * Written !(MassKg >= 0.0) rather than MassKg < 0.0 so that a NaN lands inside
	 * the guard instead of slipping past it — every comparison against NaN is
	 * false. A mass nobody can make sense of must not enter the array: once it is
	 * in, the arithmetic downstream launders it into plausible-looking loads.
	 * Zero is deliberately allowed; a massless piece is meaningful.
	 */
	if (!(MassKg >= 0.0) || !FMath::IsFinite(MassKg))
	{
		return INDEX_NONE;
	}

	FStructurePiece Piece;
	Piece.Index = Pieces.Num();
	Piece.MassKg = MassKg;
	Piece.bIsGrounded = bIsGrounded;

	return Pieces.Add(Piece);
}

int32 FStructure::AddConnection(const FConnection& Connection)
{
	/*
	 * The structure owns the graph, so it is the only place that can tell a valid
	 * piece handle from a nonsense one. A joint from a piece to itself, or to a
	 * piece that does not exist, is not a load path — and nothing below here has
	 * the piece array needed to notice.
	 */
	if (!Pieces.IsValidIndex(Connection.PieceA) || !Pieces.IsValidIndex(Connection.PieceB))
	{
		return INDEX_NONE;
	}

	if (Connection.PieceA == Connection.PieceB)
	{
		return INDEX_NONE;
	}

	/*
	 * Area fails closed at construction rather than at solve time: the load split
	 * divides by the total supporting area, and a zero, negative or NaN area
	 * leaves no sensible number to divide by. Guard written !(x > 0.0) so NaN is
	 * rejected by the same branch as zero. FConnection keeps its own area guard
	 * for callers that bypass the structure; this one is in addition to it.
	 */
	if (!(Connection.InterfaceAreaSqCm > 0.0) || !FMath::IsFinite(Connection.InterfaceAreaSqCm))
	{
		return INDEX_NONE;
	}

	/*
	 * A normal that will not normalise describes no interface plane, so there is
	 * no joint here to load. Normalize returns false for a zero-length AND for a
	 * NaN normal, which is why one check covers both. A non-unit normal is a
	 * legitimate description of the same plane and is stored as given.
	 */
	FVector UnitNormal = Connection.InterfaceNormal;
	if (!UnitNormal.Normalize())
	{
		return INDEX_NONE;
	}

	/*
	 * The break-pass stamp is grown here, with the connection it belongs to, so the
	 * two arrays are parallel by construction rather than by a solve remembering to
	 * resize one of them. A joint that has never broken is INDEX_NONE from the moment
	 * it exists, which is also the answer for a handle that names no joint at all.
	 */
	ConnectionBreakPass.Add(INDEX_NONE);

	return Connections.Add(Connection);
}

int32 FStructure::NumPieces() const
{
	return Pieces.Num();
}

int32 FStructure::NumConnections() const
{
	return Connections.Num();
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

void FStructure::SolveLoads()
{
	/*
	 * Step one: which connections hold each piece up?
	 *
	 * Two-tiered, per DESIGN.md §3: a piece rests on the bed joints BENEATH it, and
	 * only a piece with none of those falls back to its head joints. A bed joint
	 * above bears nothing at all — that is something resting on this piece.
	 *
	 * This is the correction the whole solver turned on. Routing purely by graph
	 * distance to the ground let a short sideways path exclude a bed joint: a brick
	 * spanning a gap ended up the same distance from the earth as the brick resting
	 * on top of it, so the joint between them carried zero and the keystone bore
	 * none of the wall. Classification was direction-aware from the start; routing
	 * was blind, and routing decides where the load ends up.
	 */
	TArray<TArray<int32>> SupportConnections;
	SupportConnections.SetNum(Pieces.Num());

	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		TArray<int32> HeadConnections;

		for (int32 Index = 0; Index < Connections.Num(); ++Index)
		{
			/*
			 * A JOINT THAT HAS GIVEN IS OUT OF THE STRUCTURE AND CONDUCTS NOTHING, and
			 * dropping it here — before the tier is even decided — is what makes that
			 * true everywhere at once: it leaves the support lists, and with them the
			 * reachability walk, the load paths, the accumulation order and the split.
			 *
			 * IT HAS TO LEAVE THE TIER DECISION, not merely the load path. A bed joint
			 * wins the tier outright over any number of head joints, so a broken bed
			 * joint that still won would leave the piece with an EMPTY support list —
			 * reporting it as falling with its intact head joint carrying zero, which is
			 * self-consistent, plausible-looking and wrong. The piece is meant to fall
			 * back onto the head joint and load it in shear.
			 *
			 * Its force stays at the zero every pass starts from, which is the zero
			 * redistribution is built on: the share it used to carry is now divided
			 * among the supports that are left.
			 */
			if (Connections[Index].HasGiven())
			{
				continue;
			}

			switch (RoleOf(Connections[Index], PieceIndex))
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

		/*
		 * The fallback, and only the fallback: one bed joint beneath wins outright
		 * over any number of head joints, because a joint that can bear in
		 * compression is what is actually carrying the piece.
		 */
		if (SupportConnections[PieceIndex].Num() == 0)
		{
			SupportConnections[PieceIndex] = MoveTemp(HeadConnections);
		}
	}

	/*
	 * The same relation read the other way: who rests on each piece. Both of the
	 * remaining steps walk the support relation backwards, and building it once is
	 * what lets them.
	 */
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
	 * Pieces caught in a knot the solver cannot route, and which are therefore
	 * reported as not held up at all.
	 *
	 * Kahn (step four) never makes the members of a cycle ready, so their joints keep
	 * a load of zero and the weight they carry never reaches the earth. Left there,
	 * IsPieceSupported would say "held up" while GetConnectionForce said "carrying
	 * nothing" — two answers to the same solve, contradicting each other with no
	 * signal. Reporting the piece unsupported makes them agree again, and it is the
	 * fail-closed direction: DESIGN.md §3 already says a piece with no path to ground
	 * is unsupported. It does NOT claim the cycle is solved; dividing load round a
	 * loop needs a rule we do not have.
	 *
	 * This set only ever grows, which is what makes the fixpoint below terminate.
	 */
	TArray<bool> PieceStranded;
	PieceStranded.Init(false, Pieces.Num());

	/*
	 * Reachability and the load split depend on each other, so the solve runs to a
	 * fixpoint rather than in one pass. Stranding a knot changes which pieces reach
	 * the ground; that changes which supports the split is allowed to use; and that
	 * can strand a piece which rested only on the one just stranded. Each pass either
	 * strands at least one more piece or is the last, so this runs at most
	 * NumPieces + 1 times. Every pass is a complete solve from scratch, so whatever
	 * the final one computes stands and the discarded passes leave nothing behind.
	 *
	 * Everything the passes work in is declared INSIDE the loop. Each is rebuilt at
	 * first use, so hoisting them would be correct today and would leave nothing but
	 * the opportunity for a later pass to read what an earlier one wrote.
	 */
	for (;;)
	{
		/*
		 * Recomputed from scratch every pass, so re-solving — and re-trying after a
		 * stranding — gives the same answer rather than accumulating onto the last one.
		 */
		ConnectionForces.Init(FVector::ZeroVector, Connections.Num());
		PieceSupported.Init(false, Pieces.Num());

		/*
		 * Step two: which pieces reach the ground?
		 *
		 * A breadth-first walk outward from every grounded piece at once, over SUPPORT
		 * rather than over raw connectivity. Being joined to a neighbour is not support,
		 * and neither is being joined to something that hangs off you — a piece glued
		 * under a grounded slab touches the earth and is not held up by it.
		 *
		 * Marking each piece once is what makes a cycle safe: two pieces can each hang
		 * from the other, and a walk that did not track where it had been would sit
		 * there forever instead of correctly calling both unsupported.
		 *
		 * A stranded piece conducts nothing: the walk neither marks it nor continues
		 * through it, which is what lets a later pass see that whatever rested on it
		 * has lost its only path to the earth.
		 */
		TArray<int32> SupportedFrontier;
		for (const FStructurePiece& Piece : Pieces)
		{
			if (Piece.bIsGrounded)
			{
				PieceSupported[Piece.Index] = true;
				SupportedFrontier.Add(Piece.Index);
			}
		}

		for (int32 Head = 0; Head < SupportedFrontier.Num(); ++Head)
		{
			for (const int32 Loader : Loaders[SupportedFrontier[Head]])
			{
				if (!PieceSupported[Loader] && !PieceStranded[Loader])
				{
					PieceSupported[Loader] = true;
					SupportedFrontier.Add(Loader);
				}
			}
		}

		/*
		 * Step three: A SUPPORT THAT IS ITSELF FALLING IS NOT A SUPPORT.
		 *
		 * The split may only use supports with their own path to the earth. A share
		 * given to a piece that never reaches the ground is credited to something
		 * falling and stops there: the load never arrives, and — worse — the joint that
		 * IS carrying it reports only its fraction, so a joint at 1.9x utilisation
		 * reads 0.95x and stands there for ever.
		 *
		 * Filtering ONCE, into the list every later step reads, is deliberate. The
		 * defect this replaced consulted PieceSupported when deciding who pushes load
		 * but not when deciding where it goes; one list nothing can forget to filter is
		 * what stops that coming back.
		 *
		 * Dropping falling supports is always safe: a supported ungrounded piece was
		 * only marked supported because the walk above reached it THROUGH a supported
		 * support, so at least one always survives and there is never nothing left to
		 * divide by.
		 * The load path of each piece: its supports that themselves reach the ground.
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
		 * Step four: accumulate weight downward.
		 *
		 * A piece must have received everything above it before it passes anything on,
		 * so the order is a topological sort of the support relation: a piece is ready
		 * once every piece resting on it has been dealt with. Distance to the ground is
		 * NOT that order — under the two-tier rule a spanning brick and the brick
		 * sitting on it are the same distance from the earth, and the second loads the
		 * first.
		 *
		 * Only supported, ungrounded pieces push load. A grounded piece TERMINATES the
		 * flow — whatever reaches it is taken by the earth — and an unsupported piece
		 * is not being held up by anything, so there is no static load path to report.
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

		// Accumulation order.
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

		for (int32 Order = 0; Order < Ready.Num(); ++Order)
		{
			const int32 Current = Ready[Order];

			const double TotalUU =
				ReceivedFromAboveUU[Current] + Pieces[Current].MassKg * GravityCmPerSecondSquared;

			double TotalAreaSqCm = 0.0;
			for (const int32 Index : LoadPaths[Current])
			{
				TotalAreaSqCm += Connections[Index].InterfaceAreaSqCm;
			}

			/*
			 * A supported ungrounded piece always has at least one support that reaches
			 * the ground — that is how it was reached — and AddConnection rejects any
			 * area that is not finite and positive, so this cannot be false. Written as
			 * a positive test so that a NaN total lands on "cannot split" rather than
			 * producing a confident-looking share out of a division by nothing, and kept
			 * inside the loop below rather than skipping it so the pieces underneath
			 * still become ready.
			 */
			const bool bCanSplit = TotalAreaSqCm > 0.0;

			for (const int32 Index : LoadPaths[Current])
			{
				const FConnection& Connection = Connections[Index];
				const int32 Support = OtherEndOf(Connection, Current);

				if (bCanSplit)
				{
					// Split weighted by interface area, so equal areas split evenly.
					const double ShareUU = TotalUU * (Connection.InterfaceAreaSqCm / TotalAreaSqCm);

					/*
					 * Straight down, and pointing at whichever end of the joint is being
					 * held up. Gravity does not change direction because a joint happens
					 * to be vertical — it is FConnection that resolves this same vector as
					 * compression on a bed joint and shear on a head joint, against the
					 * joint's own normal. Orienting the force along the interface normal
					 * here would give the right magnitudes and entirely the wrong
					 * direction.
					 *
					 * THE SIGN IS NOT FREE. ConnectionLoad.h's convention is that the force
					 * belonging to a connection is the force acting on PieceB, the piece
					 * the normal points toward. When the loaded piece is PieceA, describing
					 * the same joint from PieceB means flipping the normal AND taking the
					 * equal-and-opposite reaction, so the stored force points up. Storing
					 * it downward regardless is only right for joints that happen to name
					 * the loaded piece second; get it wrong and a plainly compressed joint
					 * reads as tension, and mortar's tensile limit is 0.1 MPa against
					 * 10 MPa compressive — a joint would give at one percent of its real
					 * capacity.
					 */
					const double SignedZUU = Connection.PieceB == Current ? -ShareUU : ShareUU;

					/*
					 * Assignment, not accumulation: a connection is the support of at most
					 * one of its two endpoints among the pieces reached here, because two
					 * pieces each supporting the other form a cycle that the ordering never
					 * makes ready — and which the pass below then strands outright, so
					 * neither end is even supported by the time the solve settles.
					 */
					ConnectionForces[Index] = FVector(0.0, 0.0, SignedZUU);

					ReceivedFromAboveUU[Support] += ShareUU;
				}

				/*
				 * Every entry in a load path reaches the ground, so the only thing left
				 * to exclude here is the earth itself: a grounded piece absorbs what
				 * arrives and passes nothing on.
				 */
				if (--PendingLoaders[Support] == 0 && !Pieces[Support].bIsGrounded)
				{
					Ready.Add(Support);
				}
			}
		}

		/*
		 * Step five: strand the pieces actually CAUGHT in an unroutable knot, and the
		 * next pass has to be run without them — both because they must report as
		 * unsupported and because the shares they were allocated have to go somewhere
		 * real.
		 *
		 * A piece is in the knot when its own load comes back round to it, never
		 * merely because the ordering failed to reach it. Those are not the same set:
		 * Kahn runs TOP-DOWN, so a piece is unordered whenever a knot sits anywhere
		 * ABOVE it. Stranding on un-orderability therefore walks DOWN to the first
		 * grounded piece and back up through everything resting on what it just
		 * stranded, which reports a brick bed-jointed to the earth as falling. DESIGN.md
		 * §3: a piece is unsupported only when it genuinely has no load path to the
		 * ground, and pieces beneath a knot keep their support and carry everything
		 * except the unroutable contribution.
		 *
		 * Stranding still travels UPWARD, and that is the fixpoint's job rather than
		 * this loop's: a piece whose only support has just been stranded is not reached
		 * by the next pass's walk from the ground, so it comes out unsupported there.
		 *
		 * A stranded piece is never marked supported again — the walk neither marks nor
		 * crosses one — so no piece can be stranded twice and every pass that does not
		 * break has shrunk the problem.
		 */
		bool bStrandedThisPass = false;
		for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
		{
			if (!PieceSupported[PieceIndex] || Pieces[PieceIndex].bIsGrounded)
			{
				continue;
			}

			if (LoadReturnsToPiece(PieceIndex, Pieces, Connections, LoadPaths))
			{
				PieceStranded[PieceIndex] = true;
				bStrandedThisPass = true;
			}
		}

		if (!bStrandedThisPass)
		{
			break;
		}
	}

	/*
	 * Nothing is evaluated against a strength here. Solving computes what each
	 * joint carries and must leave every connection exactly as intact as it found
	 * it: FConnection::ApplyForce latches, so calling it would break joints as a
	 * side effect of asking what they carry and make a solve unrepeatable.
	 */
}

int32 FStructure::SolveAndBreak()
{
	/*
	 * EVERY JOINT OVER CAPACITY GIVES IN THE SAME PASS (DESIGN.md §3). Each pass is a
	 * complete solve followed by one sweep that breaks everything the solve found over
	 * its own capacity, and the pass number is stamped on each joint that gives.
	 *
	 * Ordering WITHIN a pass is arbitrary and nothing may depend on it — two joints
	 * that give together are simultaneous and the array order they are visited in is an
	 * implementation detail. Ordering BETWEEN passes is real: it is caused, each break
	 * following from the load the previous one shed, and it is the sequence a collapse
	 * is played back in.
	 *
	 * The alternative, breaking only the worst joint each pass, reaches the same settled
	 * state and costs a solve per joint to do it — and worse, it invents a sequence
	 * where there is none, reporting three independently overloaded joints as failing
	 * one after another.
	 */
	int32 BreakingPasses = 0;

	for (;;)
	{
		SolveLoads();

		const int32 Pass = BreakingPasses + 1;
		bool bBrokeThisPass = false;

		for (int32 Index = 0; Index < Connections.Num(); ++Index)
		{
			/*
			 * BY REFERENCE, and this is the one line where that matters. FConnection is
			 * copyable and its "has given" latch is a member of the object, so
			 *
			 *     for (FConnection C : Connections)
			 *
			 * — one missing ampersand — would latch every overloaded joint on a
			 * TEMPORARY, leave the real connections intact, and report a structure that
			 * breaks nothing under any load whatsoever.
			 */
			FConnection& Connection = Connections[Index];

			/*
			 * A joint that has already given is skipped rather than re-evaluated, and
			 * THIS SKIP IS WHAT MAKES THE LOOP TERMINATE AT ALL. ApplyForce answers a
			 * given joint with zero without latching a second time, so calling it looks
			 * harmless — but HasGiven below asks about the JOINT, not about that call,
			 * and it is still true. Every joint broken in an earlier pass would
			 * re-report itself as breaking now: the stamp would be rewritten to the
			 * current pass, bBrokeThisPass would be set every time, and a structure that
			 * settled long ago would cascade for ever. Confirmed by mutation — deleting
			 * these four lines hangs the suite rather than merely failing it.
			 *
			 * The stamp is history; the latch is only the present. Only the transition
			 * from intact to given belongs to a pass.
			 */
			if (Connection.HasGiven())
			{
				continue;
			}

			Connection.ApplyForce(ConnectionForces[Index]);

			if (Connection.HasGiven())
			{
				ConnectionBreakPass[Index] = Pass;
				bBrokeThisPass = true;
			}
		}

		/*
		 * A pass that breaks nothing is the last one, and it is not counted: the loads
		 * it computed are the settled state, over exactly the joints that survived.
		 *
		 * TERMINATION. Joints never heal, so every counted pass permanently removes at
		 * least one connection from the structure and there can be no more of them than
		 * there are connections. (Not to be confused with SolveLoads' own internal
		 * fixpoint, which is a smaller thing nested inside each of these passes.)
		 */
		if (!bBrokeThisPass)
		{
			break;
		}

		++BreakingPasses;
	}

	return BreakingPasses;
}

int32 FStructure::GetBreakPass(int32 ConnectionIndex) const
{
	/*
	 * An unknown connection is not a joint that broke, so it fails closed to the same
	 * answer as one that never gave.
	 */
	return ConnectionBreakPass.IsValidIndex(ConnectionIndex)
		? ConnectionBreakPass[ConnectionIndex]
		: INDEX_NONE;
}

FVector FStructure::GetConnectionForce(int32 ConnectionIndex) const
{
	/*
	 * Zero for an out-of-range handle, and zero for a connection that no solve has
	 * reached — nothing in an ungrounded island is being held up, so there is no
	 * static load path to report.
	 */
	return ConnectionForces.IsValidIndex(ConnectionIndex)
		? ConnectionForces[ConnectionIndex]
		: FVector::ZeroVector;
}

bool FStructure::IsPieceSupported(int32 PieceIndex) const
{
	// An unknown piece is not being held up.
	return PieceSupported.IsValidIndex(PieceIndex) && PieceSupported[PieceIndex];
}
