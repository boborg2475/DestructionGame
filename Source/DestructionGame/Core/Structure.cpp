// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Structure.h"

/*
 * EVERY NAME IN HERE CARRIES A Solver PREFIX, AND THAT IS NOT DECORATION.
 *
 * An anonymous namespace is private to a TRANSLATION UNIT rather than to a file, and a
 * unity build merges many files into one — at which point every anonymous namespace in
 * the blob is the SAME namespace, and two file-local names that collide are a hard
 * compile error between files that never refer to each other. Several test files
 * legitimately transcribe these same constants (the tier cosine has to agree with
 * GetJointRole bit for bit, so it is spelled twice on purpose), and a `using namespace`
 * for their own support namespace then makes the bare name ambiguous against this one.
 * That is exactly what happened when this directory gained its third new file, and it is
 * luck-dependent on how UBT partitioned the blob that day.
 *
 * So production file-local names are spelled for what they belong to, per CURRENT_STATE.md.
 * Grep still finds both halves of the standing lockstep instruction below, because the
 * distinctive part of each name is unchanged.
 */
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
	constexpr double SolverGravityCmPerSecondSquared = 980.0;

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
	constexpr double SolverBedJointCosine = 0.70710678118654752440;

	/**
	 * How far a joint's rectangle may disagree with its own area, as a FRACTION of it.
	 *
	 * RELATIVE, because the quantity is an area: a joint may be a square centimetre or a
	 * square metre, and an absolute slack that is noise on one is a different face on the
	 * other.
	 *
	 * THE VALUE IS BRACKETED RATHER THAN PINNED, and anywhere in the band would do. Two
	 * derivations of one face can legitimately disagree in the last few bits — an area
	 * computed as o_u x o_v against one recovered as 4 x (o_u/2) x (o_v/2) rounds
	 * identically only because those factors are powers of two, and a producer that
	 * reached either by a different association would not — so exact equality is too
	 * strict. Equally the rule has to catch a rectangle describing a DIFFERENT face,
	 * which is the whole point of having it, so it cannot be slack. A relative 1e-12 is
	 * re-derivation noise and is accepted; a relative 1e-6 is not reachable by rounding a
	 * handful of multiplies and is refused. This sits in the middle of those six orders
	 * of magnitude, and Structure.GraphValidation asserts the two ends rather than this
	 * number, so moving it within the band breaks nothing.
	 */
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

	/*
	 * A piece that has been added to a structure is in it. The flag defaults to FALSE
	 * so that the placeholder GetPiece hands back for an unknown handle reads as dead,
	 * which means this is the one place that has to turn it on.
	 */
	Piece.bIsInTheStructure = true;

	return Pieces.Add(Piece);
}

int32 FStructure::AddPiece(double MassKg, bool bIsGrounded, const FVector& CentreOfMassCm)
{
	/*
	 * THE SAME DOOR, WITH ONE MORE FACT THROUGH IT — so the two-argument form is CALLED
	 * rather than have its guards restated here. Restated, they would be two rules that
	 * agree until somebody tightens one, and a mass this overload accepted while the
	 * other refused it would be a piece in the graph that no other route could produce.
	 */
	const int32 Handle = AddPiece(MassKg, bIsGrounded);

	if (Handle == INDEX_NONE)
	{
		return INDEX_NONE;
	}

	/*
	 * Supplying a centre buys the piece the ability to load a joint eccentrically and
	 * nothing else. The flag is what keeps "at the origin" and "nobody said" apart: a
	 * defaulted zero on a wall laid off the origin would be a lever arm of metres,
	 * invented out of a field nobody wrote.
	 */
	Pieces[Handle].CentreOfMassCm = CentreOfMassCm;
	Pieces[Handle].bHasCentreOfMass = true;

	return Handle;
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
	 * THE JOINT'S OWN GEOMETRY, AND EVERYTHING BELOW IS CONDITIONAL ON A RECTANGLE HAVING
	 * BEEN SUPPLIED AT ALL.
	 *
	 * Zero extents are not a degenerate joint, they are "no bending capacity was ever
	 * measured" — a perfectly healthy state, since with no moment the area alone answers
	 * a centred load bit for bit, the same way a zero friction coefficient reduces
	 * Mohr-Coulomb exactly rather than approximately. Checked unconditionally, the
	 * consistency rule below rejects 4 x 0 x 0 against a real area and takes every
	 * geometry-free fixture in the project with it, tilted ones included.
	 *
	 * ANY component non-zero, not all three: a rectangle with one extent left at zero is
	 * the shape of a value somebody assembled by hand and stopped halfway, and it has to
	 * reach the consistency rule as the line it is rather than read as no geometry.
	 */
	if (!Connection.InterfaceHalfExtentCm.IsZero())
	{
		/*
		 * A CENTRE IS A WORLD POSITION AND HAS NO SENSIBLE BOUND, so finiteness is the only
		 * rule there is for one. A wall laid off the origin puts every joint a long way
		 * from zero, and the running-bond producer's first bed joint already lands at
		 * (5.625, 0, 7.0). A NaN there launders into a NaN lever arm the moment anything
		 * subtracts it from a piece's centre of mass.
		 */
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!FMath::IsFinite(Connection.InterfaceCentreCm[Axis]))
			{
				return INDEX_NONE;
			}
		}

		/*
		 * A RECTANGLE MAY ONLY BE SUPPLIED ON AN AXIS-ALIGNED NORMAL. The in-plane frame
		 * is "the two world axes that are not the separation axis", which only names a
		 * frame when there IS a separation axis; on a normal 40 degrees off vertical there
		 * are two candidates and the choice between them silently picks a section modulus.
		 *
		 * MakeInterface sets exactly one component to +/-1 and leaves the other two
		 * untouched at zero, so nothing the producer builds is refused here — and a normal
		 * a millionth off an axis was not produced by rounding an exact axis vector, it
		 * was produced by somebody meaning something else. The RAW normal is read rather
		 * than the normalised one, so a non-unit (0, 0, 5) is still the same plane.
		 *
		 * A tilted normal carrying NO rectangle is untouched by this and stays a perfectly
		 * good geometry-free joint, which is what keeps every tilted fixture buildable.
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
		 * EVERY HALF-EXTENT IS NON-NEGATIVE AND FINITE, AND THAT IS NOT IMPLIED BY THE
		 * AREA CHECK BELOW. Two negative halves multiply into a perfectly plausible
		 * 4 x -5 x -5 = 100, so a guard that only compared the product would accept a
		 * rectangle that is inside out and hand the section modulus a negative lever arm
		 * that flips the sign of every stress downstream — the identical trap PieceMassKg
		 * carries a row for one level up. Written !(x >= 0.0) so a NaN lands inside the
		 * guard rather than slipping past it, and IsFinite separately because +inf >= 0.0
		 * is TRUE.
		 *
		 * ZERO ON THE SEPARATION AXIS, EXACTLY, and this is area-blind on purpose: an
		 * extent there can agree with the area perfectly and still describe a box rather
		 * than an interface, which means whoever wrote it had a different idea of which
		 * axes are in-plane.
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

		/*
		 * AND THE TWO MUST BE THE SAME FACE. An extent that disagrees with its area is the
		 * same class of fault as a normal that disagrees with its A/B pairing — a plausible
		 * number attached to the wrong geometry — except that the area governs the load
		 * SPLIT while the rectangle governs the LEVER ARM, and mortar's tensile strength is
		 * a hundredth of its compressive one, so a lever arm quietly out by a factor moves
		 * the governing axis rather than merely the number on it.
		 *
		 * Written !(diff <= tol) rather than diff > tol so a NaN difference lands inside
		 * the guard. The area is already positive and finite by the time this runs, so the
		 * relative bound is a bound.
		 */
		const double DisagreementSqCm =
			FMath::Abs(RectangleAreaSqCm - Connection.InterfaceAreaSqCm);

		if (!(DisagreementSqCm
				<= SolverRectangleAreaToleranceRatio * Connection.InterfaceAreaSqCm))
		{
			return INDEX_NONE;
		}
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

bool FStructure::RemovePiece(int32 PieceIndex)
{
	/*
	 * A handle that names no piece, and one whose piece has already gone, both remove
	 * nothing and say so. The second is not merely tidy: removal severs joints, and a
	 * second call that went through would sever a joint the cascade had already
	 * stamped, or decrement a live count that had already dropped.
	 */
	if (IsPieceRemoved(PieceIndex))
	{
		return false;
	}

	/*
	 * TOMBSTONE, never compaction. The slot stays exactly where it is and no piece
	 * ever moves down into it, because every handle in the connection array is an
	 * index into this one — see the note on bIsInTheStructure for why a free list is
	 * not the tidy-up it looks like.
	 */
	Pieces[PieceIndex].bIsInTheStructure = false;

	/*
	 * A joint holding a piece that is not there any more is not a joint, so it leaves
	 * the structure with it — and severing is the whole of removal's effect on the
	 * load model. SolveLoads already drops a given joint at the top of the tier
	 * decision, so this one line takes the piece out of the tier, the reachability
	 * walk, the load paths, the accumulation order and the split at once, through the
	 * path the cascade has always used rather than through a second one beside it.
	 *
	 * BY REFERENCE. FConnection is copyable and the latch is a member, so
	 *
	 *     for (FConnection Connection : Connections)
	 *
	 * — one missing ampersand — would sever a temporary and leave the real joints
	 * holding a piece that no longer exists.
	 *
	 * NOTHING IS STAMPED. ConnectionBreakPass is the record of what FAILED UNDER LOAD
	 * and the sequence phase 5 plays back; a joint that went with its piece never
	 * snapped and must not appear in that sequence at all. HasGiven answers whether it
	 * is still in the structure, which is the other question, and the pair encodes all
	 * three states with no sentinel — see GetBreakPass.
	 */
	for (FConnection& Connection : Connections)
	{
		if (Connection.PieceA == PieceIndex || Connection.PieceB == PieceIndex)
		{
			Connection.Sever();
		}
	}

	/*
	 * The forces the last solve computed are left as they are, and removal is
	 * deliberately immediate rather than re-solving here: what a structure carries is
	 * SolveLoads' answer to give, so a caller takes out however many pieces it means
	 * to and then asks. Every joint severed above is guaranteed a zero by the next
	 * solve, which starts from zero and never reaches a given joint.
	 */
	return true;
}

bool FStructure::IsPieceRemoved(int32 PieceIndex) const
{
	/*
	 * An unknown handle reads as removed, which is the fail-closed direction and the
	 * same answer the accessors either side of it give: a caller filtering with
	 * `if (IsPieceRemoved(H)) continue;` then skips a handle that names nothing rather
	 * than walking on into it as though it were a live piece.
	 */
	return !Pieces.IsValidIndex(PieceIndex) || !Pieces[PieceIndex].bIsInTheStructure;
}

int32 FStructure::NumPieces() const
{
	return Pieces.Num();
}

int32 FStructure::NumLivePieces() const
{
	/*
	 * Counted rather than cached, so it cannot drift out of step with the tombstones it
	 * is a count of. NumPieces is the handle RANGE and this is the other question; the
	 * two diverging is the whole point of leaving the hole.
	 */
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
	/*
	 * A CONJUNCTION OVER WHAT IS STILL IN THE STRUCTURE. A moment needs a point for the
	 * load to act at and a rectangle for the joint to resist it with, so either half
	 * missing anywhere means some joint here is answering a centred load because it has
	 * to rather than because the load is centred.
	 *
	 * A removed piece and a joint that has given are out of the graph entirely — they
	 * carry nothing and route nothing — so a tombstone left by a piece nobody placed must
	 * not condemn a structure whose live half is fully described. A predicate walking the
	 * raw arrays would say false for ever the first time a player pulled a brick.
	 *
	 * An empty structure therefore reads TRUE, which is the empty conjunction rather than
	 * a special case, and it is what keeps this composable: adding a fully described piece
	 * to a complete structure leaves it complete.
	 */
	for (const FStructurePiece& Piece : Pieces)
	{
		if (Piece.bIsInTheStructure && !Piece.bHasCentreOfMass)
		{
			return false;
		}
	}

	/*
	 * ZERO EXTENTS ARE THE ABSENCE OF A RECTANGLE, and that is the same reading
	 * AddConnection uses to decide whether there is one to validate at all. A face with
	 * no extent on any axis is not a degenerate joint — the area alone answers a centred
	 * load exactly — it is a joint whose bending capacity nobody measured, which is
	 * precisely what this predicate exists to make visible.
	 */
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

void FStructure::SolveLoads()
{
	/*
	 * COUNTED AT THE DOOR, ONCE PER CALL, AND NOTHING HERE READS IT BACK. The fixpoint
	 * below iterates, and those iterations are one solve rather than several: what a
	 * caller is charged for is the call. SolveAndBreak makes several calls and is meant
	 * to, so this is a count and never a budget.
	 */
	++SolveCount;

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
	 *
	 * KEPT RATHER THAN DISCARDED, as a member beside PieceSupported: it is the reason
	 * behind the answer that array holds, and GetPieceSupport hands it out. Rebuilt from
	 * scratch here, exactly like the rest of the solver's output, so a knot that has
	 * since been dissolved — by a break or by a removal — does not survive the next
	 * solve as a stale claim about a structure that no longer has one.
	 */
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
		ConnectionMoments.Init(FVector::ZeroVector, Connections.Num());
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
		 *
		 * A REMOVED PIECE IS NOT A ROOT, and this is the one line of removal that is not
		 * free. Severing its joints takes it out of every edge in the relation, but a
		 * GROUNDED piece seeds this walk on its own account rather than through any joint
		 * — so without the second conjunct a removed grounded piece reports itself held up
		 * by an earth it is no longer resting on.
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
				ReceivedFromAboveUU[Current] + Pieces[Current].MassKg * SolverGravityCmPerSecondSquared;

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

			/*
			 * A PIECE ON EXACTLY ONE SUPPORT IS STATICALLY DETERMINATE, AND ON SEVERAL IT
			 * IS NOT. That is the whole of the moment rule, and the obvious alternative —
			 * every supporting joint carrying its own share crossed with its own lever arm
			 * — is not an approximation of it but a different and wrong answer. On an
			 * ordinary running-bond brick with two symmetric bed patches the moments cancel
			 * across the PAIR and not on either one, so each joint would read sigma_b =
			 * 4.18e-3 against sigma_n = 1.269e-3 and every bed joint in a standing wall
			 * would report about 0.029 IN TENSION. A wall half-peeled everywhere, from a
			 * rule that looks like ordinary statics.
			 *
			 * On one support the reaction has nowhere to move to and the moment is exact.
			 * On several, the reactions rearrange until moment equilibrium is satisfied and
			 * this design has no rule for dividing that, so the area split stands and the
			 * moment stays zero — exact wherever the centre of mass sits at the
			 * area-weighted centroid of the supports, which every symmetric running bond
			 * does, and unconservative otherwise. Recorded as such in MOMENTS_DESIGN.md
			 * rather than hidden.
			 *
			 * A PIECE NOBODY PLACED CARRIES NO MOMENT AT ALL, which is not a tolerance:
			 * with no eccentricity the bending term vanishes and every joint reads bit for
			 * bit what it read before moments existed. That exactness is the only reason
			 * every geometry-free fixture in the project still works.
			 */
			const bool bLoadPathIsDeterminate =
				LoadPaths[Current].Num() == 1 && Pieces[Current].bHasCentreOfMass;

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

					/*
					 * (p - c) x F, ABOUT THE JOINT'S OWN CENTROID, AND CROSSED WITH THE
					 * FORCE JUST STORED. That force already carries the sign of whichever
					 * end is being held up, so a joint that names the loaded piece first
					 * flips BOTH the lever arm and the force and the moment comes out the
					 * same size — which is exactly the invariance the classification
					 * already has, and the reason nothing physical may depend on
					 * declaration order.
					 *
					 * THE MOMENT RIDES ALONGSIDE THE FORCE AND DOES NOT CHANGE IT. The
					 * split is still area-weighted and gravity still points straight down;
					 * a longer or tilted force vector would encode the moment in the one
					 * number a readout uses to explain the load, which is how a plausible
					 * quantity stops describing what is happening.
					 *
					 * IT IS THIS PIECE'S OWN WEIGHT PLUS WHAT IT RECEIVED, ACTING AT ITS
					 * OWN CENTRE — the load arriving from above is treated as though it
					 * arrived there. That is exact for a piece carrying only itself and an
					 * approximation for one carrying a wall; MOMENTS_DESIGN.md slice 5 is
					 * where the accumulation starts carrying a moment along the load path
					 * so that what came from above keeps its own lever arm.
					 */
					/*
					 * AND THE JOINT HAS TO KNOW WHERE IT IS, WHICH IS THE OTHER HALF OF THE
					 * CONJUNCTION HasCompleteGeometry ASKS. A joint carrying no rectangle
					 * carries no centroid either, and its zero is "nobody said" rather than a
					 * face at the world origin — so subtracting it from a placed piece
					 * measures the lever arm from the origin to the brick, which on a wall
					 * laid anywhere else is metres of eccentricity invented out of a field
					 * nobody wrote. It then meets a section modulus of zero, and
					 * ComputeUtilisation answers a moment against no section with Max: every
					 * joint of a half-described structure reads as failed, from geometry that
					 * was never supplied.
					 *
					 * Zero extents are read as the absence of a rectangle here in exactly the
					 * words HasCompleteGeometry uses, so there is one definition of what an
					 * unmeasured face is. A joint with no bending capacity measured is
					 * HEALTHY, not degenerate — the area alone answers a centred load exactly
					 * — so it keeps reading bit for bit what it read before moments existed.
					 */
					const bool bJointKnowsItsFace = !Connection.InterfaceHalfExtentCm.IsZero();

					if (bLoadPathIsDeterminate && bJointKnowsItsFace)
					{
						ConnectionMoments[Index] = FVector::CrossProduct(
							Pieces[Current].CentreOfMassCm - Connection.InterfaceCentreCm,
							ConnectionForces[Index]);
					}

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

	/*
	 * PASS NUMBERS ARE GLOBAL TO THE STRUCTURE, so this call continues from the highest
	 * stamp already written rather than starting again at 1. Stamps are the record of the
	 * ORDER a collapse happened in (DESIGN.md §3), and every consumer reads a shared number
	 * as "these gave simultaneously" — so a joint that gave after a player pulled a brick
	 * out has to carry a strictly larger number than everything that gave before it.
	 *
	 * This was unreachable until removal existed: a second cascade on a settled structure
	 * breaks nothing, so no two calls could both stamp. RemovePiece is the one operation
	 * that changes the graph between calls, and it is the MVP's own interaction.
	 *
	 * THE RETURN VALUE IS A DIFFERENT QUESTION AND STAYS PER-CALL. BreakingPasses below
	 * counts only the passes THIS call ran, which is what a caller polls to find out
	 * whether its removal did anything; only the stamps accumulate. Conflating the two —
	 * returning the high-water mark, or adding the previous count onto every stamp — is
	 * the obvious wrong fix and Structure.BreakPassesContinueAcrossCalls catches it.
	 *
	 * INDEX_NONE is -1 and every real stamp is at least 1, so an unstamped joint cannot
	 * raise the mark and a structure that has never broken starts, correctly, at 1.
	 */
	int32 PassesAlreadyStamped = 0;
	for (const int32 Stamp : ConnectionBreakPass)
	{
		PassesAlreadyStamped = FMath::Max(PassesAlreadyStamped, Stamp);
	}

	for (;;)
	{
		SolveLoads();

		const int32 Pass = PassesAlreadyStamped + BreakingPasses + 1;
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

			/*
			 * THE MOMENT GOES IN BESIDE THE FORCE, exactly as GetConnectionUtilisation
			 * hands the same pair to the same evaluator. The two are one question asked
			 * twice — what is this joint carrying — and a sweep that broke joints on the
			 * force alone would answer it differently from the readout the moment a load
			 * path missed a centroid: a joint drawn at 1.25 of capacity, holding for ever.
			 * Both arrays are rebuilt by the solve above and are indexed alike.
			 */
			Connection.ApplyForce(ConnectionForces[Index], ConnectionMoments[Index]);

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

EJointRole FStructure::GetJointRole(int32 ConnectionIndex, int32 PieceIndex) const
{
	/*
	 * THIS IS THE DECISION SolveLoads ITSELF ROUTES BY — it calls this rather than
	 * carrying its own copy — which is what makes a readout of a tier and the tier the
	 * load actually took the same answer rather than two that agree until they do not.
	 *
	 * A connection is described by a normal pointing toward PieceB, so the first thing
	 * to do is turn it to point at the piece being asked about. Pointing substantially
	 * UP at this piece, the interface is beneath it and bears it. Pointing substantially
	 * DOWN, the bed joint is above — something resting on this piece, or something this
	 * piece is glued underneath — and neither holds it up. That direction is the whole
	 * correction: routing that only asked whether two pieces were joined let a load path
	 * run upward through a joint.
	 *
	 * A normal that will not normalise is answered None rather than being given a tier.
	 * Normalize returns false for a zero-length AND for a NaN normal, so the comparison
	 * below is never reached with a NaN in it — the guard is here rather than in the
	 * comparison because a NaN falling through would land in Head, and Head is a support
	 * tier.
	 *
	 * AN UNKNOWN CONNECTION NEEDS A GUARD OF ITS OWN, AND THE ARGUMENT THAT IT DID NOT
	 * WAS THE BUG. It used to read "GetConnection hands back a placeholder whose zero
	 * normal will not normalise", which is the reasoning GetConnectionUtilisation is
	 * entitled to and this is not, because the two consume DIFFERENT FIELDS of the same
	 * placeholder. Utilisation consumes the AREA, and a default FConnection's area is
	 * zero, which ComputeUtilisation's own area guard already fails closed on. This
	 * consumes the NORMAL, and a default FConnection's normal is FVector::ZAxisVector —
	 * (0,0,1), which normalises perfectly. Carried across, the placeholder's PieceB of
	 * INDEX_NONE then matched an unidentified PieceIndex and the tier came back
	 * BedBeneath: the strongest support tier there is, reported for a joint that does
	 * not exist, on a piece that does not exist.
	 *
	 * WITH THE HANDLE KNOWN GOOD THE PIECE CHECK BELOW NEEDS NOTHING OF ITS OWN.
	 * AddConnection refuses a joint whose ends are not both valid piece handles, so a
	 * STORED connection never names INDEX_NONE at either end and an unidentified piece
	 * cannot find one to match. That is a door guard that exists and is tested, which
	 * is precisely what the placeholder-normal claim was not.
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
	/*
	 * Zero for an out-of-range handle, and zero for a connection that no solve has
	 * reached — nothing in an ungrounded island is being held up, so there is no
	 * static load path to report.
	 */
	return ConnectionForces.IsValidIndex(ConnectionIndex)
		? ConnectionForces[ConnectionIndex]
		: FVector::ZeroVector;
}

double FStructure::GetConnectionUtilisation(int32 ConnectionIndex) const
{
	/*
	 * Delegated whole, never re-derived. FConnection::UtilisationUnder is the one
	 * evaluator the break decision itself is made on, so composing it over the
	 * routed force keeps this accessor from becoming a third hand-copy of that
	 * arithmetic — and it inherits the degenerate-normal obligation for free.
	 *
	 * The fail-closed answer for an unknown handle needs no branch of its own:
	 * GetConnection returns a default-constructed placeholder whose interface area
	 * is zero, and ComputeUtilisation's area guard already answers that with
	 * TNumericLimits<double>::Max() rather than with a healthy-looking zero.
	 *
	 * THE MOMENT GOES IN BESIDE THE FORCE RATHER THAN BEING FOLDED INTO IT. Both are
	 * solver output, both are rebuilt by every solve, and both describe the same end
	 * of the same joint; how they resolve onto the face is FConnection's business and
	 * not this accessor's, which is what keeps this one line from becoming a second
	 * copy of the arithmetic the break decision is made on.
	 *
	 * Zero for a handle no solve has reached, exactly as the force is, and zero is a
	 * load path with no eccentricity rather than a tolerance.
	 */
	const FVector MomentUuCm = ConnectionMoments.IsValidIndex(ConnectionIndex)
		? ConnectionMoments[ConnectionIndex]
		: FVector::ZeroVector;

	return GetConnection(ConnectionIndex)
		.UtilisationUnder(GetConnectionForce(ConnectionIndex), MomentUuCm);
}

bool FStructure::IsPieceSupported(int32 PieceIndex) const
{
	// An unknown piece is not being held up.
	return PieceSupported.IsValidIndex(PieceIndex) && PieceSupported[PieceIndex];
}

EPieceSupport FStructure::GetPieceSupport(int32 PieceIndex) const
{
	/*
	 * DERIVED FROM IsPieceSupported RATHER THAN COMPUTED BESIDE IT. The two are one
	 * answer at two resolutions, so asking the coarser one here is what makes
	 *
	 *     IsPieceSupported(H) == (GetPieceSupport(H) is Grounded or Supported)
	 *
	 * true by construction instead of by agreement — including for an unknown handle,
	 * where the range check inside it is the only one either accessor needs, and before
	 * any solve, where the arrays are empty and every handle is unknown.
	 *
	 * Only the reason for NOT being held up needs the second array, and the order is
	 * deliberate: a piece is never both stranded and supported, but reading support
	 * first means that if it ever became possible the answer would still agree with the
	 * boolean rather than contradicting it.
	 */
	if (IsPieceSupported(PieceIndex))
	{
		return GetPiece(PieceIndex).bIsGrounded ? EPieceSupport::Grounded : EPieceSupport::Supported;
	}

	/*
	 * STRANDED IS A CLAIM ABOUT THE SOLVER, so it is only ever made about a piece the
	 * last solve actually found in a knot. Everything else falls through to Falling: a
	 * piece resting only on a knot, a piece with nothing beneath it at all, a removed
	 * piece once something has re-solved, and a handle that names no piece.
	 */
	return PieceStranded.IsValidIndex(PieceIndex) && PieceStranded[PieceIndex]
		? EPieceSupport::Stranded
		: EPieceSupport::Falling;
}

bool FStructure::HasSupportAnswer(int32 PieceIndex) const
{
	/*
	 * THE ARRAY'S OWN EXTENT IS THE ANSWER, which is why there is no "have we solved yet"
	 * flag anywhere near this. SolveLoads sizes PieceSupported to the piece count and
	 * nothing else writes it, so a flag beside it would be a second copy of something the
	 * array already carries — and a flag would answer the wrong question anyway: it says
	 * whether a solve HAPPENED, not whether it reached THIS handle, so a piece added since
	 * would sail past it.
	 *
	 * The same range check IsPieceSupported makes on the way to its own answer, asked here
	 * for its own sake rather than duplicated.
	 */
	return PieceSupported.IsValidIndex(PieceIndex);
}
