// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Structure.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/RigidBlock/RigidBlockBridge.h"
#include "HAL/PlatformTime.h"

// Solver's own log: Log per cascade that broke something or ran long, Verbose per pass.
DEFINE_LOG_CATEGORY_STATIC(LogDestructionSolve, Log, All);

/*
 * Every name carries a Solver prefix. An anonymous namespace is private to a translation
 * unit, not a file, so a unity build merges them; a collision with a test file's constants
 * is a hard compile error (it happened once). Prefixed per CURRENT_STATE.md.
 */
namespace
{
	/*
	 * Unreal's gravity, 980 cm/s2. Mass is kg and length cm, so MassKg * 980 is already a
	 * force in uu (the 1 N = 100 uu conversion, DESIGN.md §3, is baked into the 980).
	 * Multiplying by 100 again is the standard way to be wrong by exactly 100x.
	 */
	constexpr double SolverGravityCmPerSecondSquared = 980.0;

	/*
	 * Bed joint vs head joint threshold: cos(45 degrees). DESIGN.md §3 splits support on
	 * whether the normal is "substantially vertical"; 45 degrees prefers neither tier and
	 * needs no material data, which matters since the tier is decided before any profile.
	 */
	constexpr double SolverBedJointCosine = 0.70710678118654752440;

	/*
	 * Max arch depth as a fraction of span: sqrt(3)/2. BS 5977-1's equilateral load triangle
	 * over an opening (60 degree base, height sqrt(3)/2 of span). ARCHING_DESIGN.md uses the
	 * angle only as a depth cap, not to reduce load — the strictly harsher reading. Spelled
	 * 0.866 (the published figure, 3e-5 relative off the exact value).
	 */
	constexpr double SolverArchingDepthPerSpan = 0.866;

	/*
	 * Max deep-beam depth as a multiple of the joint's effective arm e = |M|/|F|. Provisional,
	 * a ruling not a derivation (COMPOSITE_DEPTH_DESIGN.md slice 3 to settle it). The form is
	 * fixed: only a bound proportional to e satisfies both ends, and a shear-transfer budget
	 * can't bound depth at all (tau_max ~ 1/D, deeper is easier). The value is set by the
	 * free-end ruling: a brick deleted at a wall's end must not bring it down, needing >= 2.465;
	 * the one-sided corbel property needs <= 3.822; 3.464 is the only value with margin on both.
	 * A literal, not 4 * SolverArchingDepthPerSpan — the arching angle does not govern this.
	 */
	constexpr double SolverCompositeDepthPerArm = 3.464;

	/*
	 * How far a joint's rectangle may disagree with its own area, relative (the quantity is an
	 * area). Bracketed, not pinned: two derivations of one face differ in the last bits so exact
	 * equality is too strict, but the rule must still catch a different face. 1e-12 is accepted
	 * as noise, 1e-6 refused; this sits mid-band and Structure.GraphValidation asserts the ends.
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

	/*
	 * Does the load leaving this piece come back round to it? That, and nothing wider, is a
	 * "caught in an unroutable knot": a piece that is ultimately its own support can't have its
	 * load divided without a rule for going round a loop, which DESIGN.md §3 says we don't have.
	 *
	 * Not "the ordering couldn't reach this piece" — Kahn orders top-down, so a piece comes out
	 * unordered whenever a knot sits above it. Un-orderability is a solver artefact; being your
	 * own support is a fact about the structure.
	 *
	 * The walk stops at a grounded piece (the earth passes nothing on; walking through one finds
	 * a loop in two bricks naming each other through a head joint). It walks LoadPaths, not the
	 * raw support lists, so it sees the edges the accumulation does; the start piece is left
	 * unvisited so arriving back at it is detected.
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
	 * Written !(MassKg >= 0.0) so a NaN lands inside the guard (every comparison against NaN is
	 * false); downstream arithmetic would launder it into plausible loads. Zero is allowed.
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
	 * A piece added to a structure is in it. The flag defaults false so GetPiece's placeholder
	 * for an unknown handle reads as dead; this is the one place that turns it on.
	 */
	Piece.bIsInTheStructure = true;

	return Pieces.Add(Piece);
}

int32 FStructure::AddPiece(double MassKg, bool bIsGrounded, const FVector& CentreOfMassCm)
{
	/*
	 * A non-finite centre is refused before the two-argument door adds anything, so "refused"
	 * stays distinct from "added then removed". A non-finite centre becomes a NaN lever arm the
	 * moment SolveLoads subtracts a joint centroid, and every comparison against NaN is false,
	 * so the joint would read intact and a wall with one unplaceable brick would stand. Clearing
	 * the flag instead would wrongly read as the healthy "nobody said where it is".
	 *
	 * ContainsNaN is !IsFinite per component, so an infinity and a broken Y (the axis every
	 * running-bond bed joint bends about) are caught too. StructureBinding checks the same rule
	 * one layer up; this is the door.
	 */
	if (CentreOfMassCm.ContainsNaN())
	{
		return INDEX_NONE;
	}

	// Call the two-argument form rather than restating its guards, which could drift apart.
	const int32 Handle = AddPiece(MassKg, bIsGrounded);

	if (Handle == INDEX_NONE)
	{
		return INDEX_NONE;
	}

	/*
	 * A centre only buys eccentric loading. The flag keeps "at the origin" and "nobody said"
	 * apart: a defaulted zero on a wall laid off the origin is a lever arm of metres.
	 */
	Pieces[Handle].CentreOfMassCm = CentreOfMassCm;
	Pieces[Handle].bHasCentreOfMass = true;

	return Handle;
}

int32 FStructure::AddConnection(const FConnection& Connection)
{
	/*
	 * The structure owns the graph, so it's the only place that can validate a piece handle. A
	 * joint to itself or to a nonexistent piece is not a load path.
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
	 * Area fails closed at construction: the load split divides by total supporting area, and a
	 * zero, negative or NaN area leaves nothing to divide by. Guard !(x > 0.0) rejects NaN by
	 * the same branch as zero. FConnection keeps its own area guard for callers that bypass this.
	 */
	if (!(Connection.InterfaceAreaSqCm > 0.0) || !FMath::IsFinite(Connection.InterfaceAreaSqCm))
	{
		return INDEX_NONE;
	}

	/*
	 * A normal that won't normalise describes no interface plane. Normalize returns false for
	 * both a zero-length and a NaN normal, so one check covers both. A non-unit normal is a
	 * valid description of the same plane and stored as given.
	 */
	FVector UnitNormal = Connection.InterfaceNormal;
	if (!UnitNormal.Normalize())
	{
		return INDEX_NONE;
	}

	/*
	 * Joint geometry — everything below is conditional on a rectangle having been supplied.
	 * Zero extents mean "no bending capacity measured", a healthy state (with no moment the area
	 * alone answers a centred load exactly), not a degenerate joint. Any component non-zero, not
	 * all three: a half-filled rectangle must reach the consistency rule rather than read as no
	 * geometry.
	 */
	if (!Connection.InterfaceHalfExtentCm.IsZero())
	{
		/*
		 * A centre has no sensible bound, so finiteness is its only rule. A NaN here launders
		 * into a NaN lever arm the moment anything subtracts it from a piece's centre of mass.
		 */
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!FMath::IsFinite(Connection.InterfaceCentreCm[Axis]))
			{
				return INDEX_NONE;
			}
		}

		/*
		 * A rectangle may only be supplied on an axis-aligned normal: the in-plane frame is the
		 * two world axes that aren't the separation axis, and a normal 40 degrees off vertical
		 * has two candidates, silently picking a section modulus. MakeInterface sets exactly one
		 * component to +/-1, so nothing the producer builds is refused. The raw normal is read,
		 * not the normalised one, so a non-unit (0, 0, 5) is still the same plane. A tilted
		 * normal with no rectangle is untouched and stays a good geometry-free joint.
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
		 * Every half-extent must be non-negative and finite, which the area check doesn't imply:
		 * two negative halves multiply into a plausible 4 x -5 x -5 = 100, giving the section
		 * modulus a negative lever arm that flips every stress sign. !(x >= 0.0) lands NaN inside
		 * the guard; IsFinite is separate since +inf >= 0.0 is true.
		 *
		 * Zero on the separation axis exactly, area-blind on purpose: an extent there can match
		 * the area and still describe a box rather than an interface.
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
		 * And the two must be the same face. The area governs the load split, the rectangle the
		 * lever arm, and mortar's tensile strength is a hundredth of its compressive, so a lever
		 * arm out by a factor moves the governing axis, not just the number. !(diff <= tol) lands
		 * a NaN difference inside the guard; area is positive and finite here so the relative
		 * bound is a bound.
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
	 * The break-pass stamps grow with the connection, so the arrays stay parallel by
	 * construction. A never-broken joint is INDEX_NONE, the same answer as an unknown handle.
	 */
	ConnectionBreakPass.Add(INDEX_NONE);
	ConnectionBreakAuthority.Add(INDEX_NONE);

	return Connections.Add(Connection);
}

bool FStructure::RemovePiece(int32 PieceIndex)
{
	/*
	 * A handle naming no piece, or one already gone, removes nothing. The second matters:
	 * removal severs joints, and a second call would re-sever a joint the cascade already
	 * stamped, or decrement a live count already dropped.
	 */
	if (IsPieceRemoved(PieceIndex))
	{
		return false;
	}

	/*
	 * Tombstone, never compaction: every handle in the connection array indexes this one, so no
	 * piece may move down into a freed slot. See the note on bIsInTheStructure.
	 */
	Pieces[PieceIndex].bIsInTheStructure = false;

	/*
	 * A joint holding a removed piece isn't a joint, so it's severed — the whole of removal's
	 * effect on the load model. SolveLoads drops a given joint at the top of the tier decision,
	 * so this one line takes the piece out of the tier, walk, load paths, order and split at
	 * once, through the path the cascade already uses.
	 *
	 * By reference: FConnection is copyable and the latch is a member, so `for (FConnection
	 * Connection : ...)` — one missing ampersand — would sever a temporary and leave the real
	 * joints holding a removed piece.
	 *
	 * Nothing is stamped: ConnectionBreakPass records what failed under load (the sequence phase
	 * 5 plays back), and a joint that went with its piece never snapped. HasGiven answers the
	 * separate "still in the structure" question; the pair encodes all three states — see
	 * GetBreakPass.
	 */
	for (FConnection& Connection : Connections)
	{
		if (Connection.PieceA == PieceIndex || Connection.PieceB == PieceIndex)
		{
			Connection.Sever();
		}
	}

	/*
	 * The last solve's forces are left as they are; removal is immediate, not a re-solve — a
	 * caller removes however many pieces it means to and then asks. Every joint severed above
	 * gets a zero from the next solve, which starts from zero and never reaches a given joint.
	 */
	return true;
}

bool FStructure::IsPieceRemoved(int32 PieceIndex) const
{
	/*
	 * An unknown handle reads as removed — fail-closed, so `if (IsPieceRemoved(H)) continue;`
	 * skips a handle naming nothing rather than walking into it as if live.
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
	 * Counted, not cached, so it can't drift from the tombstones. NumPieces is the handle range;
	 * this is the live count, and the two diverging is the point of leaving the hole.
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
	 * A conjunction over live pieces and joints. A moment needs a point to act at and a
	 * rectangle to resist it, so either half missing means some joint answers a centred load
	 * because it must, not because the load is centred. Removed pieces and given joints are out
	 * of the graph, so a tombstone must not condemn a fully-described live structure. An empty
	 * structure reads true (the empty conjunction), which keeps this composable.
	 */
	for (const FStructurePiece& Piece : Pieces)
	{
		if (Piece.bIsInTheStructure && !Piece.bHasCentreOfMass)
		{
			return false;
		}
	}

	/*
	 * Zero extents are the absence of a rectangle, as AddConnection reads them. Not a degenerate
	 * joint (the area alone answers a centred load exactly) but one whose bending capacity nobody
	 * measured, which is what this predicate exists to surface.
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

FConnection& FStructure::GetConnectionMutable(int32 ConnectionIndex)
{
	static FConnection Placeholder;
	Placeholder = FConnection{};
	return Connections.IsValidIndex(ConnectionIndex) ? Connections[ConnectionIndex] : Placeholder;
}

void FStructure::SolveLoads()
{
	/*
	 * Counted once per call; nothing reads it back. The fixpoint below iterates, but that is one
	 * solve, not several — the caller is charged for the call. A count, never a budget.
	 */
	++SolveCount;

	// Profiled, not budgeted: the phase clocks feed GetLastSolveLoadsProfile and nothing else.
	LastSolveLoadsProfile = FSolveLoadsProfile();
	const double ProfileStartSeconds = FPlatformTime::Seconds();

	/*
	 * Which joints touch which piece, built once by walking the connections. The tier decision
	 * wants a piece's own joints; finding them by asking every connection about every piece was
	 * pieces x connections calls to GetJointRole (4.3M per solve on the 1,220-piece scenario
	 * wall, times a solve per pass — a bottom-course delete measured 31 passes, ~1.25 s of lag).
	 *
	 * Ascending connection index is contract, not incidental: appending in index order leaves
	 * each list as the old connection-major sweep did, so the support lists, split and every
	 * float sum reproduce bit for bit. Sorting or a hash container would reorder an accumulation
	 * whose last bit decides breaks (the cascade fuzz has five joints at exactly 1.0, one a ulp
	 * below). Reserving is fine — only append order matters, not storage sizing.
	 *
	 * The bounds check guards a write. AddConnection refuses a joint whose ends aren't both valid
	 * handles (which is what lets GetJointRole read a stored connection unchecked), but an index
	 * chosen to append to has no such licence: the failure here is a memory overwrite.
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
	 * Step one: which connections hold each piece up? Two-tiered, per DESIGN.md §3: a piece
	 * rests on the bed joints beneath it, and only one with none of those falls back to its head
	 * joints. A bed joint above bears nothing — that's something resting on this piece.
	 *
	 * Routing by graph distance to ground let a short sideways path exclude a bed joint: a brick
	 * spanning a gap ended up the same distance from earth as the brick on top of it, so the
	 * joint between carried zero. Classification was direction-aware; routing was blind, and
	 * routing decides where the load ends up.
	 */
	TArray<TArray<int32>> SupportConnections;
	SupportConnections.SetNum(Pieces.Num());

	/*
	 * And which have no seat at all — the fallback firing, which nothing downstream can tell
	 * from a one-seat piece by the finished list alone. A one-brick hole leaves nobody here; a
	 * wider one leaves the middle bricks, which is what ReseatSpannedGroups is for. Grounded
	 * pieces and tombstones are excluded: the earth needs no seat and is a good abutment.
	 */
	TArray<bool> PieceHasNoSeat;
	PieceHasNoSeat.Init(false, Pieces.Num());

	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		TArray<int32> HeadConnections;

		for (const int32 Index : PieceJoints[PieceIndex])
		{
			/*
			 * A given joint conducts nothing; dropping it here, before the tier is decided,
			 * takes it out of the support lists, walk, load paths, order and split at once. It
			 * must leave the tier decision, not just the load path: a bed joint wins the tier
			 * over any head joints, so a broken bed joint that still won would leave an empty
			 * support list, wrongly reporting the piece falling instead of falling back onto the
			 * head joint in shear. Its force stays at the pass's starting zero, and the share it
			 * carried is redistributed among the supports left.
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
		 * The fallback only: one bed joint beneath wins over any number of head joints, because
		 * a joint that can bear in compression is what actually carries the piece.
		 */
		if (SupportConnections[PieceIndex].Num() == 0)
		{
			PieceHasNoSeat[PieceIndex] =
				Pieces[PieceIndex].bIsInTheStructure && !Pieces[PieceIndex].bIsGrounded;

			SupportConnections[PieceIndex] = MoveTemp(HeadConnections);
		}
	}

	/*
	 * Step one and a half: a run of seatless pieces spans the hole rather than hanging sideways
	 * off its edges. The only place the solver reads geometry to decide a route, and it sits
	 * above the tier, not inside it — GetJointRole is untouched. See ReseatSpannedGroups. It's
	 * the last thing to touch SupportConnections, which every step below reads, so none of them
	 * can forget the re-seat the way the old solver forgot to filter falling supports.
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

	/*
	 * The same relation the other way: who rests on each piece. Both remaining steps walk the
	 * support relation backwards, so it's built once here.
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
	 * Pieces caught in a knot the solver can't route, reported as not held up. Kahn (step four)
	 * never makes cycle members ready, so their joints keep zero load and the weight never
	 * reaches earth. Left there, IsPieceSupported would say "held up" while GetConnectionForce
	 * said "carrying nothing"; reporting unsupported makes them agree, the fail-closed direction
	 * (DESIGN.md §3: a piece with no path to ground is unsupported). It doesn't claim the cycle
	 * is solved — dividing load round a loop needs a rule we don't have.
	 *
	 * The set only grows (which makes the fixpoint terminate) and is kept beside PieceSupported
	 * as the reason for its answer, handed out by GetPieceSupport. Rebuilt from scratch each
	 * solve, so a knot since dissolved doesn't survive as a stale claim.
	 */
	PieceStranded.Init(false, Pieces.Num());

	/*
	 * And which pieces have overturned off their supports: a body on two or more compression-only
	 * bearings whose centre of mass projects outside their union has no admissible equilibrium
	 * and must lose the earth, yet each seat reads a comfortable split. Detected during the
	 * accumulation below and, like stranding, excluded from the next pass's walk. A local, not a
	 * member — GetPieceSupport needs only PieceSupported and PieceStranded, and an overturned
	 * piece falls through to Falling. Grows monotonically, so the fixpoint terminates.
	 */
	TArray<bool> PieceOverturned;
	PieceOverturned.Init(false, Pieces.Num());

	/*
	 * And which pieces a refused one-sided arch has released. A seatless run spanning a hole
	 * forms a group; when its one-sided abutment makes the opposition gate refuse the arch
	 * (PieceInRefusedArchGroup, from ReseatSpannedGroups), the run keeps its sign-blind head
	 * joints, and a mutual-support cycle among them would have the stranding pass report it
	 * Stranded. But the refused arch was its only path to earth, so the honest answer is Falling
	 * (DESIGN §8 case-21: above the cap a one-sided masonry cantilever falls). So a refused-group
	 * member is released here, not stranded, and excluded from the next walk like an overturned
	 * piece. Gated on refused-group membership, which forms only under complete geometry, so a
	 * geometry-free propping pair keeps its genuine Stranded. Grows monotonically.
	 */
	TArray<bool> PieceReleasedFromRefusedArch;
	PieceReleasedFromRefusedArch.Init(false, Pieces.Num());

	/*
	 * Reachability and the load split depend on each other, so the solve runs to a fixpoint.
	 * Stranding a knot changes which pieces reach ground, which changes which supports the split
	 * may use, which can strand a piece resting only on the one just stranded. Each pass strands
	 * at least one more piece or is the last, so at most NumPieces + 1 passes; each is a complete
	 * solve from scratch, so the final answer stands and discarded passes leave nothing behind.
	 * Everything is declared inside the loop and rebuilt at first use, so no pass reads what an
	 * earlier one wrote.
	 */
	const double FixpointStartSeconds = FPlatformTime::Seconds();

	for (;;)
	{
		++LastSolveLoadsProfile.FixpointIterations;
		LastSolveLoadsProfile.SupportedPerIteration.Add(0);
		LastSolveLoadsProfile.OverturnedPerIteration.Add(0);
		LastSolveLoadsProfile.StrandedPerIteration.Add(0);
		LastSolveLoadsProfile.ReleasedPerIteration.Add(0);

		// Recomputed from scratch every pass, so re-solving never accumulates onto the last one.
		ConnectionForces.Init(FVector::ZeroVector, Connections.Num());
		ConnectionMoments.Init(FVector::ZeroVector, Connections.Num());
		ConnectionCompositeDepthCm.Init(0.0, Connections.Num());
		PieceSupported.Init(false, Pieces.Num());

		/*
		 * Step two: which pieces reach the ground? A breadth-first walk outward from every
		 * grounded piece, over support rather than raw connectivity — being joined to a neighbour,
		 * or to something hanging off you, is not support. Marking each piece once makes a cycle
		 * safe (two pieces can each hang from the other). A stranded piece conducts nothing: the
		 * walk neither marks nor crosses it, so a later pass sees whatever rested on it has lost
		 * its path to earth. The removed-piece conjunct below is the one line of removal that
		 * isn't free: a grounded piece seeds the walk on its own account, not through a joint, so
		 * without it a removed grounded piece would report itself held up by earth it left.
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
				/*
				 * An overturned body is excluded like a stranded one: neither marked nor
				 * crossed, so a piece resting only on something toppled loses its path to earth
				 * next pass. Empty on the first pass, so the walk is bit-identical until the
				 * accumulation finds a body past tipping. A piece released from a refused arch is
				 * excluded the same way — a seatless run whose one-sided arch was declined and
				 * found in a head-joint cycle has no load path and drops through to Falling.
				 * Grows only when the stranding pass releases a refused-group member, so a
				 * refused cantilever reaching an abutment without a cycle keeps Supported.
				 */
				if (!PieceSupported[Loader] && !PieceStranded[Loader] && !PieceOverturned[Loader]
					&& !PieceReleasedFromRefusedArch[Loader])
				{
					PieceSupported[Loader] = true;
					SupportedFrontier.Add(Loader);
				}
			}
		}

		/*
		 * Step three: a support that is itself falling is not a support. The split may only use
		 * supports with their own path to earth. A share given to a piece that never reaches
		 * ground stops there, and worse, the joint actually carrying it reports only its fraction
		 * — a joint at 1.9x reads 0.95x and stands forever. Filtered once, into the one list
		 * every later step reads. Always safe: a supported ungrounded piece was marked supported
		 * because the walk reached it through a supported support, so at least one survives.
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
		 * Step four: accumulate weight downward. A piece must receive everything above it before
		 * passing anything on, so the order is a topological sort of the support relation: ready
		 * once everything resting on it is dealt with. Distance to ground is not that order — a
		 * spanning brick and the brick on it are the same distance from earth, and the second
		 * loads the first. Only supported ungrounded pieces push load: a grounded piece
		 * terminates the flow, and an unsupported piece has no static load path to report.
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

		/*
		 * What arrives from above is a force and a moment about a point — here the receiving
		 * piece's own centre of mass, a bookkeeping choice: transfer is transitive, so
		 * re-referencing joint to centre to joint equals joint straight to joint bit for bit, and
		 * a per-piece accumulator needs one point per piece. Not the world origin: valid but
		 * numerically awful, since every entry would be the whole wall's moment about a point a
		 * wall-length away. Zero for an unplaced piece, and nothing is ever added to one.
		 */
		TArray<FVector> ReceivedMomentUuCm;
		ReceivedMomentUuCm.Init(FVector::ZeroVector, Pieces.Num());

		/*
		 * Set when this pass finds a body past tipping, so the fixpoint runs once more with it
		 * excluded — the role bStrandedThisPass plays for a knot. A pass that only overturns
		 * isn't the last one.
		 */
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

			/*
			 * A supported ungrounded piece always has a ground-reaching support, and
			 * AddConnection rejects any non-finite or non-positive area, so this can't be false.
			 * A positive test lands a NaN total on "cannot split", and it's kept inside the loop
			 * so the pieces underneath still become ready.
			 */
			const bool bCanSplit = TotalAreaSqCm > 0.0;

			/*
			 * A piece on exactly one support is statically determinate; on several it isn't.
			 * That's the whole moment rule. The obvious alternative (every joint carrying its own
			 * share crossed with its own lever arm) is a different, wrong answer: on a running-bond
			 * brick with two symmetric bed patches the moments cancel across the pair, not on
			 * either one, so every bed joint in a standing wall would report about 0.029 in
			 * tension — a wall half-peeled everywhere.
			 *
			 * On one support the moment is exact. On several the reactions rearrange until moment
			 * equilibrium holds, and this design has no rule to divide that, so the area split
			 * stands and the moment stays zero — exact where the centre of mass sits at the
			 * area-weighted centroid of the supports (every symmetric running bond), and
			 * unconservative otherwise. Recorded in MOMENTS_DESIGN.md.
			 *
			 * An unplaced piece carries no moment: with no eccentricity the bending term vanishes
			 * and every joint reads what it did before moments existed. A piece re-seated onto an
			 * arch is indeterminate however many edges it has left — ReseatSpannedGroups only
			 * routes a group seated on both sides, so its weight is shared between two abutments
			 * and the one head joint left is the bookkeeping route, not a claim that the brick
			 * hangs off it alone. Treated as determinate it would carry its whole column across
			 * 11.25 cm to that centroid (about 11.6 of capacity) and the head joint would snap.
			 */
			const bool bLoadPathIsDeterminate = LoadPaths[Current].Num() == 1
				&& Pieces[Current].bHasCentreOfMass && !PieceReseatedOnAnArch[Current];

			/*
			 * Before zeroing the moment for two or more supports, ask whether the body overturns.
			 * Zeroing is right for a centre of mass over the supports (reactions rearrange, no
			 * couple) but wrong for one that has left the region its supports can push up through
			 * with no tension to hold it down: no admissible equilibrium, and pretending each seat
			 * carries a share leaves a board floating on bearings it has toppled off (the shed's
			 * ridge, once its back gable is gone). PieceOverturnsOffItsSupports answers it, and on
			 * yes the piece is marked so the next walk drops it and it comes out Falling.
			 *
			 * Nothing else changes: the piece is still split and reads Supported for the rest of
			 * this pass (its forces are discarded when the fixpoint re-runs), so every piece is
			 * bit-identical until a body past tipping is found. Gated on N >= 2 so single-support
			 * determinate pieces never reach it.
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
					// Split weighted by interface area, so equal areas split evenly.
					const double ShareUU = TotalUU * (Connection.InterfaceAreaSqCm / TotalAreaSqCm);

					/*
					 * Straight down, at whichever end is held up. Gravity doesn't change
					 * direction because a joint is vertical — FConnection resolves this same
					 * vector as compression on a bed joint and shear on a head joint. Not the
					 * whole of what a springing carries: ApplyArchingThrust adds a horizontal
					 * component at spanned openings once this settles; every joint no arch
					 * touches keeps this vector bit for bit.
					 *
					 * The sign is not free. Per ConnectionLoad.h a connection's force acts on
					 * PieceB (the piece the normal points toward), so when the loaded piece is
					 * PieceA the stored force is the equal-and-opposite reaction, pointing up.
					 * Get it wrong and a compressed joint reads as tension — mortar's tensile
					 * limit is 0.1 MPa against 10 MPa compressive, so it gives at one percent.
					 */
					const double SignedZUU = Connection.PieceB == Current ? -ShareUU : ShareUU;

					/*
					 * Assignment, not accumulation: a connection supports at most one of its two
					 * endpoints among the pieces reached here (mutual support forms a cycle the
					 * ordering never makes ready, and the pass below strands it).
					 */
					ConnectionForces[Index] = FVector(0.0, 0.0, SignedZUU);

					/*
					 * The same share as a physical force, straight down. Every moment below is built
					 * from this, not the stored vector, so the declaration-order sign is applied once.
					 */
					const FVector ShareWeightUu(0.0, 0.0, -ShareUU);

					/*
					 * The joint also has to know where it is (the other half of HasCompleteGeometry).
					 * A joint with no rectangle has no centroid, and its zero means "nobody said", not
					 * the origin; subtracting it from a placed piece would invent metres of
					 * eccentricity, meet a zero section modulus, and read every joint of a
					 * half-described structure as failed. Zero extents are the absence of a rectangle,
					 * as HasCompleteGeometry reads them: healthy, not degenerate.
					 */
					const bool bJointKnowsItsFace = !Connection.InterfaceHalfExtentCm.IsZero();

					/*
					 * What this joint carries, about its own centroid: everything from above, carried
					 * down and re-referenced, plus this piece's weight about the same point. Written
					 * physically, with the declaration-order sign restored at the store below.
					 *
					 * (c_from - c_to) x F is ordinary Varignon: the received load keeps its own lever
					 * arm instead of being placed on this piece's middle. The old rule vanished
					 * wherever a chain stacked squarely, which is why nothing that stacks squarely
					 * moves now. The moment rides alongside the force and doesn't change it — the
					 * split stays area-weighted and gravity points straight down.
					 */
					FVector MomentAboutJointUuCm = FVector::ZeroVector;

					if (bLoadPathIsDeterminate && bJointKnowsItsFace)
					{
						MomentAboutJointUuCm = ReceivedMomentUuCm[Current]
							+ FVector::CrossProduct(
								Pieces[Current].CentreOfMassCm - Connection.InterfaceCentreCm,
								ShareWeightUu);

						/*
						 * A seat with something to push against arches rather than cantilevers. Delete one
						 * brick from a running-bond wall and the brick above keeps one seat, overhangs it
						 * by 5.625 cm, and reads 1.63 in tension — so it goes, and the failure walks the
						 * wall at 33.69 degrees, one step per course. Add the intact head joint into the
						 * hole and the two bricks either side lean on each other, thrust line through the
						 * opening, and the same joint reads 0.0142 in compression. ARCHING_DESIGN.md.
						 *
						 * The four gates split across two objects: "compressive and outside the kern" is
						 * arithmetic on one face (the joint's); "a bed joint beneath a placed piece with an
						 * abutment on the overhanging side" is about the graph (here). Cheap test first —
						 * an intact wall has e = 0 at every seat, so nothing below the first condition is
						 * reached. The force handed over is the stored one (only its orientation states
						 * "compressive"); the moment goes in physically oriented, only in-plane magnitudes read.
						 */
						if (GetJointRole(Index, Current) == EJointRole::BedBeneath)
						{
							const double ArchingRelief = Connection.ArchingMomentScale(
								ConnectionForces[Index], MomentAboutJointUuCm);

							if (ArchingRelief < 1.0)
							{
								/*
								 * The couple the cap deletes is what something else must supply, so it's
								 * handed over with the gates. Capping the moment by k removes (1 - k) of it,
								 * and an arch is only an arch if the replacing thrust can be delivered —
								 * HasArchingAbutment measures that against the seat's sliding capacity.
								 * DESIGN.md §7 gap 4. The whole vector's magnitude is conservative: a
								 * torsion component (DESIGN.md §5.3, never produced by gravity on an
								 * axis-aligned rectangle) can only ask for more thrust, so the difference
								 * withholds a relief, never grants one. Inside the relief test, so an intact
								 * wall's seats never pay for the square root.
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
							 * What resists the remaining moment isn't one bed patch. A stack of courses over
							 * a lost support acts as a deep beam: a vertical section through the bonded masonry
							 * over this joint, t*D^2/6, against the patch's 179.48 cm3 — eleven courses is a
							 * factor of sixty-five, deciding whether a brick deleted at a free end takes the
							 * wall down. ARCHING_DESIGN.md slice 5. The depth is measured and the moment
							 * untouched: composite action changes the section the moment is read against, not
							 * what the wall hands down, so only this joint's utilisation moves.
							 *
							 * Gated on a bed joint with a real moment (a head joint has no masonry over it,
							 * MOMENTS_DESIGN case (b); an intact wall at e = 0 never reaches here) and on a
							 * stack existing (a piece with nothing on it is one unit, not a composite).
							 *
							 * Two caps. First, the joint's effective arm e = |M|/|F| (already accumulated, no
							 * new field), times lambda: for a k-step corbel e is about half its depth so
							 * lambda*e is ~1.73x it, and the cap fires on the rendered wall, not the corbel.
							 * The division credits less — a near-zero force makes the arm enormous, and an
							 * infinite depth is the whole wall this refuses; tested positively so a NaN arm
							 * lands outside the relief. Second, the corbelling body's own depth is a floor,
							 * since the arm may only trim masonry above the cut (those courses generate the
							 * moment, bonded into one body; masonry above must be dragged in by shear over
							 * lambda*e). Max written `greater than` so a NaN body depth falls through to
							 * lambda*e. MasonryDepthAboveCm overshoots by up to one course (202.5 vs 200.77 cm
							 * on the scenario corbel, permissive); written `less than`, not FMath::Min, for
							 * the same NaN reason. COMPOSITE_DEPTH_DESIGN.md.
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
					 * Handed on as the pair it is: a force through this joint and the moment stored on
					 * it, re-referenced to the receiving piece's centre. What travels is what the joint
					 * reads, so an indeterminate joint transmits zero moment about itself, not nothing,
					 * and the load below knows it came through that patch. Both ends must be placed, or
					 * there's nothing to measure the transfer against and the origin enters a lever arm.
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

				/*
				 * Every load-path entry reaches the ground, so the only thing to exclude is the
				 * earth: a grounded piece absorbs what arrives and passes nothing on.
				 */
				if (--PendingLoaders[Support] == 0 && !Pieces[Support].bIsGrounded)
				{
					Ready.Add(Support);
				}
			}
		}

		/*
		 * Step five: strand the pieces caught in an unroutable knot; the next pass runs
		 * without them, since they must report unsupported and their shares must go
		 * somewhere real. A piece is in the knot when its own load comes back round to it,
		 * not merely when the ordering failed to reach it — Kahn runs top-down, so a piece
		 * is unordered whenever a knot sits above it, and stranding on that would report a
		 * brick bed-jointed to earth as falling (DESIGN.md §3). Stranding travels upward
		 * via the fixpoint, not this loop: a piece whose only support was just stranded isn't
		 * reached by the next walk. A stranded piece is never re-marked supported, so no
		 * piece strands twice and every non-breaking pass shrinks the problem.
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
				/*
				 * A cycle on a refused-arch member is a fall, not a knot: the piece is in this
				 * head-joint cycle only because its one-sided arch was refused, losing its only load
				 * path, so it's released to Falling. A cycle on any other piece is a genuine knot
				 * and stays Stranded, which keeps the geometry-free propping pair (no group forms)
				 * reading Stranded.
				 */
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
	 * Step six: an arch pushes sideways, and the springing carries that too. Outside the
	 * fixpoint because it reads its answer: the thrust is a fraction of the load the
	 * abutments' seats already carry, computable only once the accumulation settles, and
	 * it feeds nothing back (a horizontal force changes no support list, split, order or
	 * moment), so the vertical answer stays bit-identical to one before arches existed.
	 */
	const double ArchingStartSeconds = FPlatformTime::Seconds();

	ApplyArchingThrust(PieceJoints, Arches);

	const double ProfileEndSeconds = FPlatformTime::Seconds();

	LastSolveLoadsProfile.FixpointMs = (ArchingStartSeconds - FixpointStartSeconds) * 1000.0;
	LastSolveLoadsProfile.ArchingMs = (ProfileEndSeconds - ArchingStartSeconds) * 1000.0;
	LastSolveLoadsProfile.TotalMs = (ProfileEndSeconds - ProfileStartSeconds) * 1000.0;

	/*
	 * Nothing is evaluated against a strength here. Solving computes what each joint
	 * carries and must leave every connection as intact as it found it: ApplyForce
	 * latches, so calling it would break joints as a side effect and make a solve
	 * unrepeatable.
	 */
}

bool FStructure::PieceOverturnsOffItsSupports(int32 PieceIndex, const TArray<int32>& LoadPath) const
{
	const FStructurePiece& Piece = Pieces[PieceIndex];

	/*
	 * No centre of mass, no point to project — fail closed and keep today's stand. An
	 * unplaced piece carries no overturning; its joints answer a centred load exactly, so
	 * it must read as it did before this gate existed.
	 */
	if (!Piece.bHasCentreOfMass)
	{
		return false;
	}

	/*
	 * The tension clause, asked first and off the strength data. Any support in the load
	 * path that can carry tension holds the lifting side down in withdrawal, so the body
	 * has an admissible equilibrium however far its centre of mass reaches, and must not
	 * overturn. This spares the porch overhang (tied by a Screw) and the anti-regression
	 * fixture's tension-tied board. Reading TensileStrengthMPa directly keeps the
	 * distinction in the material data: a new tension-capable type spares its bodies here
	 * with no change.
	 */
	for (const int32 Index : LoadPath)
	{
		if (Connections[Index].Strength.TensileStrengthMPa > 0.0)
		{
			return false;
		}
	}

	/*
	 * All supports are compression-only: the body stands only while its centre of mass
	 * projects onto the region its bearings can push up through — the convex hull of the
	 * contact rectangles. This tests the axis-aligned bounding box instead, a superset of
	 * the hull, so felling on "outside the box" is conservative: it can fail to fell a
	 * body outside a non-rectangular hull, never fell one inside. That keeps symmetric
	 * running bond exact (its centre of mass sits at the seats' area-weighted centroid,
	 * inside the box) at the cost of under-felling some diagonal arrangements no fixture
	 * has. Projected onto the horizontal bed plane (X and Y), since gravity is vertical.
	 */
	double MinX = 0.0;
	double MaxX = 0.0;
	double MinY = 0.0;
	double MaxY = 0.0;
	bool bHaveBox = false;

	for (const int32 Index : LoadPath)
	{
		const FConnection& Connection = Connections[Index];

		/*
		 * A joint with no rectangle leaves the box undefined, so the body keeps today's
		 * reading — the fail-closed rule that a body with no support geometry must not
		 * spuriously overturn.
		 */
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

	// No usable rectangle anywhere — fail closed.
	if (!bHaveBox)
	{
		return false;
	}

	/*
	 * Outside the box, as four positive comparisons. A negated conjunction, or FMath
	 * Min/Max, would let a non-finite coordinate through as an overturn (every NaN
	 * comparison is false, so !inside reads true). Four <,> tests each fail against a
	 * NaN, landing such a piece on "inside" and keeping its stand.
	 */
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
	/*
	 * Sized before the gate, so every caller may index it without asking whether this
	 * pass ran; all-false means "no group formed". PieceInRefusedArchGroup is sized here
	 * too and stays all-false unless a group forms and its opposition gate refuses it, so
	 * a geometry-free structure keeps a genuine cycle-strand out of this reclassification.
	 */
	PieceReseatedOnAnArch.Init(false, Pieces.Num());
	PieceInRefusedArchGroup.Init(false, Pieces.Num());
	Arches.Reset();

	/*
	 * The geometry gate, load-bearing not defensive. Deciding a run of bricks over a hole
	 * is an arch rather than a chain of hangers needs to know where the hole is, so
	 * ARCHING_DESIGN revises MOMENTS_DESIGN's discipline line here. With no positions no
	 * group forms, so the pass is a no-op and a geometry-free structure routes as ever.
	 * Both fuzz generators emit no geometry (20,000 cases, the only routing property tests
	 * here); an arch firing without positions would set every one against an oracle that
	 * never heard of one, and they'd go dark quietly.
	 */
	if (!HasCompleteGeometry())
	{
		return;
	}

	TArray<bool> Grouped;
	Grouped.Init(false, Pieces.Num());

	/*
	 * Reused across groups: hops from the nearest abutment, INDEX_NONE for a piece this
	 * pass hasn't reached. Only the group being worked is read, and each group writes its
	 * own before reading.
	 */
	TArray<int32> HopsFromAbutment;
	HopsFromAbutment.Init(INDEX_NONE, Pieces.Num());

	/*
	 * What "contiguous" means, written once and asked four times: the piece across an
	 * intact head joint, or INDEX_NONE otherwise. Head joints only — a bed joint to a
	 * seatless piece is the tier that already failed, and following one would fuse the
	 * courses above and below a hole into one group. A given joint conducts nothing, so
	 * HasGiven is asked here since GetJointRole keeps answering for a severed joint.
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

		/* THE GROUP: the connected run of seatless pieces this one belongs to. */
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

		/* Where the group sits, which is the point its abutments are counted either side of. */
		FVector GroupCentreCm = FVector::ZeroVector;
		for (const int32 Member : Group)
		{
			GroupCentreCm += Pieces[Member].CentreOfMassCm;
		}

		GroupCentreCm /= static_cast<double>(Group.Num());

		/*
		 * The abutments: the seated pieces the group pushes against, one head joint away. A
		 * member touching one is a hop from ground and seeds the walk inward. Each abutment
		 * once, however many members touch it: the thrust divides among the abutments per
		 * end, and one counted twice would take twice its share.
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
		 * The group only spans if something seated stands on both sides. One abutment is a
		 * cantilever however long, and granting it would hang a wall's free end off a joint
		 * with nothing to thrust into (the permissive failure ARCHING_DESIGN names). Opposite
		 * sides is a negative dot product about the group's centre, needing no nominated axis.
		 * A positive test leaves a NaN direction unabutted rather than spanning.
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
			/*
			 * The gate refuses a one-sided cantilever, but the declined run is left on its
			 * sign-blind head joints, becomes a mutual-support chain, and LoadReturnsToPiece
			 * strands it. That Stranded is a solver artefact — a refused arch was the run's only
			 * load path, so the honest answer is Falling. Record every member so SolveLoads
			 * excludes them from the walk like an overturned piece. Gated on group membership,
			 * which forms only under HasCompleteGeometry(), so a geometry-free knot is untouched.
			 */
			for (const int32 Member : Group)
			{
				PieceInRefusedArchGroup[Member] = true;
			}
			continue;
		}

		/* How far each member is from the nearest abutment, in head joints. */
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
		 * The re-seat is acyclic by construction: a member keeps only the head joints that
		 * take it strictly closer to an abutment, so every edge runs long-path to short and no
		 * walk returns to its start — what separates this from ARCHING_DESIGN's trap 1, where
		 * making the neighbour a support outright puts two bricks over a hole in a cycle and
		 * strands the pair. Ascending joint index survives, since this filters PieceJoints in
		 * place; the accumulation downstream is a float sum whose order is these lists' order.
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

				/* An abutment is where the walk started, so it is a hop from nowhere: zero. */
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
		 * And the opening is recorded as an arch with its two ends told apart. The thrust pass
		 * needs to know which abutments face each other: H is one number for the span, pushed
		 * out at both ends, and a pass that couldn't tell them apart would have nothing to
		 * make equal and opposite. The first abutment's own direction is the axis (nominating
		 * no world axis); sides fall out as the sign of a projection, and one square on to the
		 * axis is dropped via positive tests. A direction that won't normalise is an abutment
		 * on the group's centre, describing no span, so there's no arch to thrust.
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

		/*
		 * Both ends or neither. bAbutsOnBothSides already found a pair in opposition, so this
		 * can't be false; asked because the thrust pass may not be what discovers a one-ended
		 * arch (trap 2 dressed as a refactor).
		 */
		if (Arch.Abutments[0].Num() > 0 && Arch.Abutments[1].Num() > 0)
		{
			/*
			 * And L is how far the two ends stand apart, one mean abutment centre per end. Slice 3
			 * never needed it (d_e = 0.866*L cancelled the span out of the thrust ratio); capping
			 * d_e by the cover puts it back, and the abutment positions give it with no new query.
			 */
			EndCentreCm[0] /= static_cast<double>(Arch.Abutments[0].Num());
			EndCentreCm[1] /= static_cast<double>(Arch.Abutments[1].Num());

			Arch.SpanCm = (EndCentreCm[0] - EndCentreCm[1]).Size();

			/*
			 * The thrust axis is the line between the two ends' mean abutment centres, not the
			 * first abutment's own direction. That vector earned its keep as the classification
			 * axis, but pushing along it would shove each springing out of the wall plane when a
			 * group re-seats onto perpendicular (corner) walls. The end-to-end difference cancels
			 * whatever out-of-plane offset both ends share; EndCentreCm[0] is the +TowardEndZero
			 * side, keeping the sign convention. Zeroing Z projects onto the seat plane. For a
			 * planar wall this normalises to the same (+/-1,0,0) the raw vector did. A difference
			 * that won't normalise means the ends coincide horizontally — no span, no thrust.
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
		 * The seats the arch delivers through, and what they already carry. The thrust leaves
		 * through the same patch its weight does, which is why the springing plane is
		 * critical: demand is constant with depth while the friction resisting it grows with
		 * weight above. Each seat's sign is recorded here: per ConnectionLoad a joint's force
		 * acts on PieceB, so a joint naming the abutment second stores the push as given, one
		 * naming it first the reaction. Backwards, and the two ends pull together, not apart.
		 */
		TArray<int32> Seats[2];
		TArray<double> SeatSign[2];
		double SeatAreaSqCm[2] = { 0.0, 0.0 };

		/*
		 * How deep the arch may be if only the angle governed, and how far the cover walk
		 * below must look: past this much masonry the angle governs and the cover changes
		 * nothing.
		 */
		const double AngleCappedDepthCm = SolverArchingDepthPerSpan * Arch.SpanCm;

		/*
		 * The thinnest cover either end stands under, one number for the whole arch. A cover
		 * measured and applied per abutment is trap 2 again: the two ends would disagree about
		 * d_e, push by different amounts, and hand the structure a net horizontal force.
		 * Reducing to one before anything is pushed makes equal-and-opposite structural. The
		 * thinnest, not the mean, because thin cover is the direction that fails.
		 */
		double CoverCm = TNumericLimits<double>::Max();

		/*
		 * W is the whole load the arch puts on its abutments, springings' own columns
		 * included; ARCHING_DESIGN is explicit it isn't a triangle. Taking the re-seated
		 * group's load alone would leave out the two delivering bricks and under-report the
		 * thrust by about a cell's worth.
		 */
		double TotalVerticalUu = 0.0;

		for (int32 End = 0; End < 2; ++End)
		{
			for (const int32 Abutment : Arch.Abutments[End])
			{
				/* Whichever of this abutment's seats came first: the plane its cover stands on. */
				int32 SpringingJointIndex = INDEX_NONE;

				for (const int32 Index : PieceJoints[Abutment])
				{
					const FConnection& Connection = Connections[Index];

					/*
					 * A given joint conducts nothing, so takes no thrust either — the tier rule.
					 * GetJointRole keeps answering for a severed joint, so HasGiven is asked here.
					 */
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

				/*
				 * Written `not at least as deep`, not `Min`, so a NaN cover is taken, not discarded:
				 * every NaN comparison is false, so FMath::Min would keep the good end's depth; this
				 * way the NaN reaches the guard below and the arch is left unthrust.
				 */
				const double AtThisEndCm = MasonryDepthAboveCm(
					Abutment, SpringingJointIndex, PieceJoints, AngleCappedDepthCm);

				if (!(AtThisEndCm >= CoverCm))
				{
					CoverCm = AtThisEndCm;
				}
			}
		}

		/*
		 * Both ends must be able to take it, or neither is pushed. An end whose abutments are
		 * grounded has no seat to deliver into, and thrusting only the other end gives a net
		 * horizontal force out of nowhere (ARCHING_DESIGN's trap 2, which no per-joint check
		 * catches). Every guard is a positive test so a NaN area or load leaves it unthrust.
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

		/*
		 * A degenerate span or cover leaves the arch unthrust, as a degenerate area or load
		 * does above. Positive tests, so a NaN lands inside; neither is reachable from a wall
		 * anyone laid.
		 */
		if (!(Arch.SpanCm > 0.0) || !FMath::IsFinite(Arch.SpanCm)
			|| !(CoverCm > 0.0) || !FMath::IsFinite(CoverCm))
		{
			continue;
		}

		/*
		 * The arching depth, a min not a replacement: the angle says how deep an arch may be,
		 * the masonry over the opening says how deep it can be, and the smaller governs — a
		 * buried opening reads the same however much wall is piled on, a shallow one is
		 * governed by what it has. Held as d_e/L, not d_e, which is arithmetic: the thrust
		 * depends only on the ratio (H = 3W/(8*(d_e/L))), and where the angle governs d_e/L is
		 * the constant itself, so slice 3's answers stay bit-identical. Written out, not FMath
		 * Min, because a NaN cover in Min's first argument is silently replaced by the angle's
		 * answer — the permissive direction this slice exists to remove.
		 */
		const double DepthPerSpan = CoverCm < AngleCappedDepthCm
			? CoverCm / Arch.SpanCm
			: SolverArchingDepthPerSpan;

		if (!(DepthPerSpan > 0.0))
		{
			continue;
		}

		/*
		 * H = W*L/(8r) with r = d_e/3, so H = 3W/(8*(d_e/L)). The rise is a fixed fraction of
		 * the depth, not the span, which is what makes the cover matter: H climbs as 1/d_e
		 * while V doesn't, so H/V = 3L/(4*d_e) blows up as the masonry over an opening thins.
		 * Where the angle governs, d_e/L is 0.866, the span drops out, and it settles at
		 * 3/(4*0.866) = 0.866. One number for the whole arch, pushed at both ends, which makes
		 * trap 2 exact.
		 */
		const double ThrustUu = 3.0 * TotalVerticalUu / (8.0 * DepthPerSpan);

		for (int32 End = 0; End < 2; ++End)
		{
			/*
			 * One direction, two signs, so the ends sum to exactly zero: +H*D and -H*D cancel bit
			 * for bit.
			 */
			const FVector EndThrustUu = (End == 0 ? ThrustUu : -ThrustUu) * Arch.TowardEndZero;

			for (int32 Which = 0; Which < Seats[End].Num(); ++Which)
			{
				const int32 Index = Seats[End][Which];

				/*
				 * Divided among an end's seats by interface area, the same rule the load split uses;
				 * with one seat (a half-seated springing) it's the whole of it.
				 */
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
	 * What stands on this piece is whatever BedAbove says rests on it, so this costs a
	 * walk over the six or so joints a brick has — no broadphase, no world. A given joint
	 * conducts nothing and holds nothing up, so HasGiven is asked (GetJointRole keeps
	 * answering for a severed joint). The first by ascending joint index, a chain not a
	 * traversal; a stepped or gabled wall would have its answer decided by which column
	 * this took, and nothing tests that yet. Two callers: MasonryDepthAboveCm steps once
	 * per course, and SolveLoads asks whether there's a stack over a joint at all.
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
	 * The course the joint is under counts, and is the first, so the shallowest depth a
	 * wall offers is one course, never zero (the spanning course for the arch, the
	 * corbelled brick for the composite section). Its depth is a course pitch, not a brick
	 * height: the wall works through the mortar too, exactly the rise from the piece below
	 * the joint to the piece on it. A brick height reads about 13% shallow on standard
	 * brickwork.
	 */
	const double FirstCourseRiseCm =
		Pieces[Piece].CentreOfMassCm.Z - Pieces[Seat].CentreOfMassCm.Z;

	if (!(FirstCourseRiseCm > 0.0))
	{
		return 0.0;
	}

	/*
	 * The walk is bounded twice. Past EnoughDepthCm no further course changes the caller's
	 * answer (0.866*L for the arch, lambda*|M|/|F| for the composite section), at most
	 * ceil(EnoughDepthCm / pitch) steps, so neither caller walks a whole wall it won't
	 * use. The piece count is the second bound, pure defence against a graph whose normals
	 * claim A over B and B over A. Compared as a double, so a vanishing pitch gives an
	 * enormous bound, not an undefined integer conversion.
	 */
	const double MaxCourses = FMath::Min(
		FMath::CeilToDouble(EnoughDepthCm / FirstCourseRiseCm),
		static_cast<double>(Pieces.Num()));

	double CoverCm = FirstCourseRiseCm;
	int32 Current = Piece;

	for (int32 Course = 1; CoverCm < EnoughDepthCm && Course < MaxCourses; ++Course)
	{
		// One step up, over a bed joint and never through space. See PieceRestingOn.
		const int32 Above = PieceRestingOn(Current, PieceJoints);

		if (Above == INDEX_NONE)
		{
			break;
		}

		/*
		 * Measured rise by rise, so courses of unequal depth add up to what they are. A step
		 * that doesn't rise isn't a course, and stopping on it is fail-closed: less cover is
		 * more thrust, and reading intact when it should read failed is the expensive error.
		 */
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

	/*
	 * The body's first course is the piece on the joint, not asked whether it's
	 * corbelling — it's the cut by construction, whose overhang generated the moment; the
	 * walk above it is what must be justified. Measured as a course pitch from the seat,
	 * for MasonryDepthAboveCm's reason: the wall works through the mortar too.
	 */
	const double FirstCourseRiseCm =
		Pieces[Piece].CentreOfMassCm.Z - Pieces[Seat].CentreOfMassCm.Z;

	if (!(FirstCourseRiseCm > 0.0))
	{
		return 0.0;
	}

	/*
	 * Seated on exactly one course. Counted over the piece's own joints, excluding given
	 * joints and anything on a piece that left the structure — the same two exclusions
	 * PieceRestingOn makes, since neither is masonry the body can bond into.
	 */
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

	/*
	 * The piece count bound is pure defence: every step rises and no piece is visited
	 * twice on a rising chain, so a consistent graph stops on its own; this stops an
	 * inconsistent one walking forever.
	 */
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

		/*
		 * Measured rise by rise; a step that doesn't rise isn't a course. Written !(x > 0.0)
		 * so unmeasured geometry leaves the body at the depth it earned rather than adding a
		 * NaN — a shallower body credits less section, the direction to be wrong in.
		 */
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
	/*
	 * The seat's plane is what the sides are measured in, so a normal that won't normalise
	 * can't abut anything. Nothing reaching here can fail this (the caller already had a
	 * tier and moment from this joint); it's here so the projections below get a real
	 * direction.
	 */
	FVector UnitNormal = BedJoint.InterfaceNormal;

	if (!UnitNormal.Normalize())
	{
		return false;
	}

	/*
	 * Which way the piece overhangs: where its centre of mass sits relative to the patch
	 * it left, flattened into that patch's plane. On a bed joint that's the 5.625 cm a
	 * half-seated running-bond brick leans by; the projection drops the height between the
	 * two points from a sideways-only comparison.
	 */
	const FVector EccentricCm = FVector::VectorPlaneProject(
		Pieces[PieceIndex].CentreOfMassCm - BedJoint.InterfaceCentreCm, UnitNormal);

	/*
	 * How hard this seat may be pushed sideways, MPa, bought with its own squeeze. The
	 * deleted couple leaves through this patch as shear, limited by the Mohr-Coulomb
	 * envelope ComputeUtilisation already uses (bond plus friction on the mean
	 * compression). Read off the force, not the profile name: a cohesionless joint earns
	 * only its friction, which is what stops dry stone flat-arching with no line saying
	 * so. One conversion, the named one: force is uu, strengths MPa, so the area carries
	 * the 10000 once and the comparison is stress against stress.
	 */
	const FConnectionLoad SeatLoad = DestructionForce::ClassifyForce(SeatForceUu, UnitNormal);

	const double SeatCompressionMPa = SeatLoad.Compression
		/ (BedJoint.InterfaceAreaSqCm * DestructionForce::ForceUnitsPerMPaSqCm);

	const double CohesionAndFrictionMPa = BedJoint.Strength.ShearCohesionMPa
		+ BedJoint.Strength.FrictionCoefficient * SeatCompressionMPa;

	/*
	 * The truncation is written out, not FMath::Min, and the comparison order is the
	 * reason: Min is `(A <= B) ? A : B`, and every NaN comparison is false, so a NaN
	 * capacity as the first argument is replaced by the profile ceiling (Max() when
	 * unset) — an arch afforded by arithmetic nobody can read. Asking whether the ceiling
	 * is smaller keeps a NaN, refusing the relief. Unreachable today; spelled so it
	 * wouldn't matter.
	 */
	const double SlidingCapacityMPa =
		BedJoint.Strength.MaxShearStrengthMPa < CohesionAndFrictionMPa
			? BedJoint.Strength.MaxShearStrengthMPa
			: CohesionAndFrictionMPa;

	for (const int32 Index : PieceJoints[PieceIndex])
	{
		const FConnection& Head = Connections[Index];

		/*
		 * A given joint conducts nothing, so can't deliver a thrust — the tier rule.
		 * GetJointRole still answers for a severed joint, so HasGiven is asked here.
		 */
		if (Head.HasGiven() || GetJointRole(Index, PieceIndex) != EJointRole::Head)
		{
			continue;
		}

		/*
		 * And the head joint must know where it is: its centroid says which side it's on, and
		 * an unmeasured face's zero means "nobody said", not a plane through the world origin.
		 */
		if (Head.InterfaceHalfExtentCm.IsZero())
		{
			continue;
		}

		const FVector TowardAbutmentCm = FVector::VectorPlaneProject(
			Head.InterfaceCentreCm - Pieces[PieceIndex].CentreOfMassCm, UnitNormal);

		/*
		 * On the eccentric side; a joint square on to the overhang is on neither. A positive
		 * test drops an exact zero, and a NaN, here rather than counting it an abutment.
		 */
		if (!(FVector::DotProduct(EccentricCm, TowardAbutmentCm) > 0.0))
		{
			continue;
		}

		const int32 Abutment = OtherEndOf(Head, PieceIndex);

		/*
		 * It must reach the ground on its own account, and both halves matter. PieceSupported
		 * is the walk from earth, covering Grounded and Supported and excluding anything
		 * falling or stranded; the second test is trap 3, telling a real arch from two bricks
		 * propping each other over open air — both Supported with an intact head joint, so
		 * only the support relation separates them.
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

		/*
		 * A neighbour re-seated onto a spanning group leans on us only because something
		 * beyond it carries. That mark, set only by ReseatSpannedGroups for a group abutted on
		 * both sides, means the thrust line runs on to a reaction rather than stopping in
		 * mid-air — the one fact separating a spanned opening from two propping bricks.
		 * Without it this reads as it did before groups existed.
		 */
		if (bAbutmentLeansOnUs && !PieceReseatedOnAnArch[Abutment])
		{
			continue;
		}

		/*
		 * A spanned group is checked by being pushed, so not here: ApplyArchingThrust puts the
		 * real horizontal force on both springings once the accumulation settles. Judging the
		 * same thrust again, by a different rule (H off span and cover vs off the deleted
		 * couple), would answer one question twice. The one-cell hole has no thrust pass of
		 * its own.
		 */
		if (PieceReseatedOnAnArch[Abutment])
		{
			return true;
		}

		/*
		 * So the relief must be earned, by the seat's own sliding capacity. Moving the thrust
		 * line to the kern edge deletes a couple of (1 - k)*|M|, and on a half-seated brick
		 * only a horizontal pair can supply it — a push through this head joint and its
		 * reaction as shear in the bed plane. The arm between them is measured (head centroid
		 * above seat centroid, along the seat normal so a tilted joint is in its own frame):
		 * for a standard brick and 1 cm joint that's 3.75 cm, putting demand at
		 * (e - h/6)/z = 1.0444 of the reaction, load cancelled out. Withheld, not applied: a
		 * real shear would move every one-cell arch twenty-odd times on an axis reading zero,
		 * so an earned arch stays bit-identical and an unearned one reads what it carries —
		 * for a cohesionless joint outside its kern, a tension it has no strength for.
		 * !(demand <= capacity) so a degenerate arm, area or capacity lands inside the refusal.
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
	/*
	 * Invalidate the readout per pass, not just per call. Only a pass reaching an
	 * answering arm (Stands or a certified Falls) refills the cache; every decline leaves
	 * without refilling, so clearing here means a declined pass can't serve a prior pass's
	 * below-cap reading now InspectPiece makes it player-visible. The per-call Reset in
	 * SolveAndBreak stays as the coarser guard.
	 */
	ConnectionReadoutCache.Reset();

	/*
	 * Scope by size first, the fail-closed boundary keeping synchronous LP authority off
	 * the flagship scenarios (PROMOTION_DESIGN.md §12 D6-c). Above the cap the gate
	 * declines and behaviour falls through to the per-joint sweep, which breaks and
	 * enumerates support as production did. Written as a positive decline test (> cap) so a
	 * degenerate cap lands fail-closed.
	 */
	if (NumPieces() > EquilibriumGateBlockCap)
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * A no-op without complete geometry, for the reason the bridge refuses one: the LP
	 * reasons about moments of weights against bearing rectangles, and with either missing
	 * there's nothing to take moments of. Both fuzz generators emit no geometry, so they
	 * stay on the router.
	 */
	if (!HasCompleteGeometry())
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * Ask the rigid-block LP whether the whole structure has an admissible equilibrium. The
	 * pose is feasibility at lambda = 1 (PROMOTION_DESIGN §12 D6-b): same Stands/Falls
	 * as lambda* but 5-16x cheaper, and on the infeasible arm its phase-1 dual is
	 * Farkas-verified as the collapse mechanism (slice 3a). A refusal fails closed: a
	 * declining bridge, a refusing or over-budget solver all arrive as Unanswerable and
	 * decline here rather than inventing a break out of an answer the LP couldn't give.
	 */
	RigidBlockOracle::FOracleProblem Problem;
	FString WhyNot;

	if (!RigidBlockOracle::BuildRigidBlockProblem(*this, Problem, WhyNot))
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * Record which physics this pose was built in the moment it exists — 2 for X-Z, 3 for
	 * volumetric. The bridge picks the cheapest sound pose, so a 3D-flagged build whose
	 * posed rows are all in-plane and share one Y reads 2 (a roof on walls at two Y reads
	 * 3). Stamped on the pose, not the call: a pass declining before or after the bridge
	 * leaves the previous reading.
	 */
	LastEquilibriumProblemDim = Problem.Dim == RigidBlockOracle::EOracleDim::Dim3D ? 3 : 2;

	Problem.bGravityIsLive = false;

	/*
	 * First-crack promotion. Below the cap the break authority writes uncracked peak-fibre
	 * bending rows for every bonded joint (f_t > 0), so it cracks at its elastic limit — 3x
	 * stricter than the plastic no-tension form — not only at the plastic margin. Dry
	 * (f_t = 0) joints write no such row and are bit-identical. The flag reaches only this
	 * below-cap pose; the router, fuzzes and oracle sweeps are untouched.
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
		 * The LP stands the whole structure, so every piece is genuinely held — even one the
		 * router could only strand for want of a rule to divide load round a knot (rows 10 and
		 * 19). The mechanism is empty, so nothing moves; writing the LP support (all bridged
		 * pieces Supported/Grounded) is the sole effect, making GetPieceSupport LP-authoritative
		 * below the cap. Not a whole-structure veto but the absence of any mechanism to release.
		 */
		ApplyLimitAnalysisSupport(Problem, Result);
		CacheMinViolationReadout(Problem);
		return EEquilibriumGateDisposition::AuthoritativeNoBreak;
	}

	/*
	 * The structure has no admissible equilibrium, and the mechanism names the loss.
	 * SolveRigidBlock either Farkas-verified the certificate or refused (Unanswerable,
	 * handled above), so a Falls here carries a certified mechanism; the guard below is
	 * fail-closed defence, declining rather than acting on an uncertified set.
	 */
	const RigidBlockOracle::FOracleMechanism& Mechanism = Result.Mechanism;

	if (!Mechanism.bPresent || !Mechanism.bIsCertified)
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * The mechanism is the sole break authority (PROMOTION_DESIGN.md §12 D7's 3b): a
	 * piece is released iff the mechanism moves it, a joint severed iff it opens or slides.
	 * Write the LP support first, then sever the intact joints it opens, mapped back through
	 * the bridge's ConnectionOfJoint. Index-ordered and single-threaded, so deterministic.
	 * Sever is latching without a second utilisation evaluation, stamped with this pass. A
	 * joint already gone is skipped, which makes the cascade terminate: once a body's joints
	 * are all severed the mechanism names no intact joint to sever and the loop ends.
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

	/*
	 * Cache the readout once, on the settled pass — the same last-wins answer, computed
	 * once. A pass that severs continues the cascade and re-settles, so its readout would
	 * only be overwritten; guarding on !bSeveredThisPass runs the min-violation LP solely on
	 * the terminal non-breaking pass, whose result is what last-wins already kept.
	 */
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
	 * The strain readout is a separate, additive solve (PROMOTION_DESIGN.md §3.5,
	 * SHED_PATH Phase A 6b). The verdict is already settled; this poses the min-violation LP
	 * once on a copy (bMinViolationReadout routes it to a formulation with equilibrium rows
	 * hard and every strength row a penalised slack) to read the closest-to-admissible force
	 * distribution. It writes only this cache, so no break, support flag or force can move.
	 * First-crack rows are on, matching the below-cap break authority, or a bonded bending
	 * joint would report the plastic utilisation it's never held to (4x too comfortable at
	 * e = 3h); at M = 0 the two coincide. Rebuilt each time; an absent readout (bPresent
	 * false) leaves the cache all-absent so GetConnectionReadout fails closed to the router.
	 */

	// Observability only: count each readout LP so a test can watch how many a cascade pays for.
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

	/*
	 * Key each oracle joint back to its production connection through the bridge's
	 * ConnectionOfJoint provenance — the same map BreakByEquilibrium uses — so the cache
	 * the overlay reads by connection index carries the LP's per-joint N, M, violation and
	 * utilisation. A joint with no provenance, or a connection out of range, is skipped.
	 */
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
	 * Make GetPieceSupport LP-authoritative below the cap (PROMOTION_DESIGN.md §12 D7's
	 * 3b, §3.7). The router strands a piece whose load returns round a cycle it can't
	 * divide; the LP has no routing to fail, so if it stands the structure that piece is
	 * carried. Overwrite the router's per-piece support with the LP verdict for every
	 * bridged piece: if the LP stands (no mechanism) each reads Supported, or Grounded if a
	 * foundation (rows 10 and 19 go router-stranded to LP-carried); if infeasible, the moved
	 * pieces read Falling and every other bridged piece stays Supported/Grounded. A grounded
	 * block writes no equilibrium rows so it never moves. Pieces the bridge excluded keep the
	 * router's answer. PieceOfBlock and Mechanism.Blocks share block order, read in lockstep.
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
	 * The per-joint capacity sweep — the router's break authority, once per pass. Every
	 * joint over its own capacity gives in the same pass (DESIGN.md §3), stamped with
	 * this pass. The sole authority above the cap and on any LP refusal; below the cap the
	 * gate answers and this doesn't run, so it never latches a joint the mechanism didn't
	 * name.
	 */
	bool bBroke = false;

	for (int32 Index = 0; Index < Connections.Num(); ++Index)
	{
		/*
		 * By reference, the one line where it matters: FConnection is copyable and its latch is
		 * a member, so `for (FConnection C : ...)` — one missing ampersand — would latch
		 * every overloaded joint on a temporary and report a structure that breaks nothing.
		 */
		FConnection& Connection = Connections[Index];

		/*
		 * A joint already given is skipped, and this skip is what makes the loop terminate.
		 * ApplyForce answers a given joint with zero without re-latching, but HasGiven is still
		 * true, so every earlier break would re-report and the cascade would never settle.
		 * Confirmed by mutation: deleting these four lines hangs the suite. The stamp is
		 * history; only the intact-to-given transition belongs to a pass.
		 */
		if (Connection.HasGiven())
		{
			continue;
		}

		/*
		 * The moment goes in beside the force, as GetConnectionUtilisation hands the same pair
		 * to the same evaluator — one question asked twice. Break on the force alone and a
		 * joint drawn at 1.25 holds forever. The composite depth goes in too, the same seam the
		 * other way: a relief, so omitting it snaps a corbel drawn at 0.37 (at 22.9). The
		 * strength is the weakest-link pairing (EffectiveJointStrength), so a wood-on-brick
		 * bearing reads its material crush, not the connection's capacity, and the place that
		 * severs agrees with the overlay. The copy decides, the real connection severs:
		 * ApplyForce runs on a copy carrying the paired strength, and the break is stamped on
		 * the real Connection via Sever(). Where neither face names a material it's bit-identical
		 * to the bare connection.
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
	 * Every joint over capacity gives in the same pass (DESIGN.md §3): each pass is a
	 * solve plus one sweep that breaks everything over capacity, stamped with the pass
	 * number. Ordering within a pass is arbitrary (simultaneous gives); ordering between
	 * passes is real, each break following the load the previous shed, and is the sequence a
	 * collapse plays back. Breaking only the worst joint per pass reaches the same state at
	 * a solve per joint, and invents a sequence where there is none.
	 */
	int32 BreakingPasses = 0;

	/*
	 * Invalidate the cached strain readout before this settle (SHED_PATH.md Phase A 6b).
	 * Refilled solve-on-settle by BreakByEquilibrium below the cap; clearing here means a
	 * re-solve (including one now declining to the router above the cap) can't return a
	 * previous below-cap reading.
	 */
	ConnectionReadoutCache.Reset();

	// Observability only: zero the readout-solve counter for this whole cascade (see the getter).
	MinViolationReadoutSolves = 0;

	/*
	 * Pass numbers are global, so this call continues from the highest stamp already
	 * written, not from 1: stamps record the order a collapse happened in (DESIGN.md §3),
	 * and a joint that gave after a brick was pulled must carry a strictly larger number.
	 * Unreachable until removal existed. The return value is a different question, per-call:
	 * BreakingPasses counts only this call's passes, and conflating the two is the obvious
	 * wrong fix Structure.BreakPassesContinueAcrossCalls catches. INDEX_NONE is -1 and every
	 * real stamp >= 1, so an unstamped joint can't raise the mark.
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
	 * The regional prover's seed — the disturbance the region grows from
	 * (REGIONAL_PROVER_PLAN.md §1). On the first pass the disturbance is the removal, so
	 * the seed is the live neighbours of every tombstoned piece (a tombstone stamps no pass,
	 * so it isn't reachable through ConnectionBreakPass). On later passes it's the live
	 * endpoints of joints stamped Pass - 1 — what shed its load last pass.
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

	/* The count of intact (not-yet-severed) joints — the regional prover's monotone progress witness:
	 * a pass that severs a joint made progress, and joints only ever decrease so the cascade
	 * ends. */
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

	/* Pieces the last solve is not holding up: Falling or Stranded, live and answered. */
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

	/*
	 * The report is filled as the cascade runs and decides nothing: every clock and count
	 * feeds GetLastSolveAndBreakReport, and the cascade reads none of it back.
	 */
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
		 * The equilibrium gate decides the pass, and below the block cap is the sole break
		 * authority (DESIGN.md §7 step 4, PROMOTION_DESIGN.md §6 Slice 3, §12 D7's
		 * 3b). It asks the rigid-block LP whether the whole structure has any admissible force
		 * system in equilibrium with self-weight — a state no per-joint number expresses — and
		 * on no severs exactly the joints the mechanism opens, rewriting the support arrays from
		 * the LP. When it declines (over cap, no geometry, LP refusal) the capacity sweep is the
		 * sole authority, as before the gate. When it answers, the sweep is demoted to the
		 * estimator it already is: the forces still sit in ConnectionForces so the overlay is
		 * unchanged, but it latches nothing.
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
			 * The regional prover's arm (REGIONAL_PROVER_PLAN.md §4, slice 4a). Above the cap
			 * the gate declined and the per-joint sweep is blind to a global mechanism — a body
			 * with a comfortable per-joint utilisation while the assembly has no admissible
			 * equilibrium. A grounded-boundary LP over the disturbance neighbourhood can prove that
			 * collapse and upgrade a stand to a fall, one-directional toward Falling only (grounding
			 * the boundary only adds support). Gated on HasCompleteGeometry() as BreakByEquilibrium,
			 * so the geometry-free fuzzes are a no-op.
			 */
			if (HasCompleteGeometry())
			{
				/*
				 * Progress is a severed joint, never the prover's Falling count. The prover re-poses its
				 * region each pass and re-fells a piece it already felled: a disconnected block trivially
				 * moves under no constraints, so keying on "a piece went Falling" would loop forever.
				 * Intact joints only decrease, so keying on a severed joint is monotone and bounded: at
				 * most one pass per connection. It's also a complete witness of a genuine felling — a
				 * rigid joint between a moved and a standing block can't stay closed (the 1e-6 mechanism
				 * tolerance severs every moved-vs-standing joint), so a prover-felled piece fully
				 * disconnects and SolveLoads reads it Falling on its own. (This is why the
				 * router-re-holds case can't exist — proven 2026-09-03.) The Falling override is
				 * re-applied every pass including the terminal one, so the settled state carries it.
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

		/*
		 * A pass that breaks nothing is the last, and isn't counted: its loads are the settled
		 * state over the joints that survived. Termination: joints never heal, so every counted
		 * pass removes at least one connection. (Not SolveLoads' own inner fixpoint, nested
		 * inside each of these passes.)
		 */
		if (!bBrokeThisPass)
		{
			break;
		}

		++BreakingPasses;
	}

	LastSolveAndBreakReport.BreakingPasses = BreakingPasses;
	LastSolveAndBreakReport.IntactJointsAfter = CountIntactJoints();
	LastSolveAndBreakReport.TotalMs = (FPlatformTime::Seconds() - CascadeStartSeconds) * 1000.0;

	/*
	 * Logged at Log level only when it matters to a player: a cascade that broke something,
	 * or one that cost more than a frame. A settle that changed nothing in a few ms is
	 * Verbose.
	 */
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
	/*
	 * The isolated test entry (REGIONAL_PROVER_PLAN.md slice 1). Settles the router
	 * baseline, then defers the region flood + grounded-boundary pose + Falling-only stitch
	 * to the shared ProveRegionalCollapse, the same machinery the real cascade drives (slice
	 * 4a). BreakPass defaults to 1, the stamp this entry wrote before the factoring.
	 */
	SolveLoads();
	return ProveRegionalCollapse(Seed, RegionBlockCap);
}

int32 FStructure::ProveRegionalCollapse(const TArray<int32>& Seed, int32 RegionBlockCap, int32 BreakPass)
{
	/*
	 * The regional collapse prover (REGIONAL_PROVER_PLAN.md §§1-4, review item 12).
	 * A grounded-boundary LP over a neighbourhood of the disturbance can upgrade a router
	 * stand to a proven fall, never the reverse: a region with its frontier pinned to earth
	 * is one-directional (grounding only adds support), so it proves collapse but never
	 * standing. The caller already settled the graph, so this does not SolveLoads — that
	 * would waste work and wipe the support state the stitch overrides.
	 */

	/*
	 * The joint-hop adjacency the flood walks, built as SolveLoads builds it: every intact
	 * joint touching each piece, ascending connection index. A given joint is out of the
	 * graph, so a severed neighbourhood doesn't re-reach across a break.
	 */
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
	 * The region and its grounded frontier, grown in place. AdmitToRegion moves a live
	 * candidate into Region only if region + induced grounded boundary still fits the block
	 * budget — bounding |R| alone lets the one-hop ring push the pose past budget
	 * (REGIONAL_PROVER_PLAN.md §1). Boundary is the region's exact one-hop frontier at
	 * every step; a rejected candidate stays in it, its grounded ring the region's tie to
	 * earth.
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
	 * Grow the region by a BFS in joint-hops from a frontier seed set, up to Budget blocks,
	 * returning the count admitted. A frontier seed that can't be admitted is dropped, not
	 * grounded (grounding the pieces growth pushes from would pin the collapse it chases); a
	 * BFS-discovered neighbour that can't be admitted becomes grounded boundary. BFS order is
	 * a function of the sorted frontier and the ascending adjacency, both fixed, so a
	 * deterministic frontier gives a deterministic region.
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

	/* Set equality without an operator== on TSet: same size and one contained in the other. */
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
	 * Phase 1: a modest initial region flooded from the disturbance seed, deliberately far
	 * below RegionBlockCap (REGIONAL_PROVER_PLAN.md §§1-3). The point of
	 * grow-on-contact: pose an LP the size of the local mechanism, not the cap. A cap-sized
	 * first pose made the flagship 3D scenarios impractical (~200-block LP every above-cap
	 * pass). Budget is the seed's footprint plus a small ring, floored at 16, never above
	 * the cap.
	 */
	const int32 InitialBudget = FMath::Min(RegionBlockCap, FMath::Max(16, Seed.Num() + 8));
	int32 EffectiveBudget = InitialBudget;

	GrowFrom(Seed, EffectiveBudget);

	/*
	 * Grow-on-contact (REGIONAL_PROVER_PLAN.md slice 3). After each solve: a certified fall
	 * whose mechanism is interior (every grounded neighbour a genuine foundation) is
	 * complete, so stitch it; a fall touching a cut-artifact grounded boundary may hide more
	 * collapse, so re-flood from the moved set at a larger budget (mechanism-directed, so a
	 * disconnected grounded island is dropped and the budget re-spent on the collapse); a
	 * pose that doesn't fall grows a bounded speculative search from the whole boundary.
	 * Growth is monotone and the budget doubles each grow, so a local mechanism settles in
	 * one or two solves and a cap-spanning one in a few. Termination: an interior mechanism,
	 * the budget at its ceiling, a grow that changes nothing, or the iteration bound.
	 * GetLastRegionalProblemBlockCount reports the final |R ∪ B|.
	 */
	RigidBlockOracle::FOracleProblem Problem;
	RigidBlockOracle::FOracleResult Result;
	bool bLastPoseFell = false;

	/* Observability for the pass report: reset per call, written per pose. */
	LastProverPoses = 0;
	LastProverLpPivots = 0;
	LastProverLpMs = 0.0;
	bLastProverFell = false;
	LastProverJointsSevered = 0;
	LastProverPoseBreakdown.Reset();

	const int32 MaxGrowIterations = Pieces.Num() + 4;

	for (int32 Iteration = 0; Iteration < MaxGrowIterations; ++Iteration)
	{
		/*
		 * Pose R + grounded boundary at feasibility (bGravityIsLive = false) with the below-cap
		 * first-crack rows — the authority BreakByEquilibrium poses. A bridge refusal fails
		 * closed: no region opinion, so the router baseline stands.
		 */
		Problem = RigidBlockOracle::FOracleProblem();
		FString WhyNot;

		if (!RigidBlockOracle::BuildRegionalProblem(*this, Region, Boundary, Problem, WhyNot))
		{
			return 0;
		}

		/*
		 * Record the posed problem size (|R ∪ grounded boundary|) the moment the pose exists.
		 * Each grow overwrites it, so GetLastRegionalProblemBlockCount reports the final pose;
		 * every grow bounds the budget at RegionBlockCap, so it witnesses the ring never
		 * overspends.
		 */
		LastRegionalProblemBlockCount = Problem.Blocks.Num();

		Problem.bGravityIsLive = false;
		Problem.bFirstCrackRows = true;

		const double LpStartSeconds = FPlatformTime::Seconds();

		Result = RigidBlockOracle::SolveRigidBlock(Problem);

		/*
		 * Compute this pose's wall-clock once and feed both the aggregate and the per-pose
		 * record, so the RegionalPoseBreakdown ms sum reconciles with LastProverLpMs.
		 */
		const double PoseMs = (FPlatformTime::Seconds() - LpStartSeconds) * 1000.0;

		++LastProverPoses;
		LastProverLpPivots += Result.SimplexIterations;
		LastProverLpMs += PoseMs;

		const bool bCertifiedFall =
			RigidBlockOracle::OutcomeOf(Result) == RigidBlockOracle::EOracleOutcome::Falls
			&& Result.Mechanism.bPresent && Result.Mechanism.bIsCertified;

		bLastPoseFell = bCertifiedFall;

		/*
		 * One record per pose, appended where every iteration reaches it once, so the increment
		 * above and this append stay in lockstep (RegionalPoseBreakdown.Num() == RegionalPoses).
		 */
		FProverPoseReport& Pose = LastProverPoseBreakdown.AddDefaulted_GetRef();
		Pose.Blocks = Problem.Blocks.Num();
		Pose.LpPivots = Result.SimplexIterations;
		Pose.LpMs = PoseMs;
		Pose.bFell = bCertifiedFall;

		/*
		 * The frontier the next grow pushes from, and its budget ceiling. A certified fall
		 * re-floods mechanism-directed (from the moved set) up to the full RegionBlockCap. A pose
		 * that doesn't fall grows a speculative search from the whole boundary, capped at
		 * RegionSpeculativeCeiling far below the cap: with no mechanism to size to, a blind flood
		 * must not balloon toward the cap (the most expensive pose is a large standing region). A
		 * chain small enough to fit the ceiling still floods to its free end and falls; a
		 * mechanism reachable only past the ceiling is an accepted miss the router already stands
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
			 * The contact test — does a moved block neighbour a cut-artifact grounded boundary
			 * block (in Boundary but not a genuine foundation)? A real foundation stays earth; only a
			 * flood-pinned block may hide collapse behind it. No contact means the mechanism is
			 * bounded by genuine foundations — interior, nothing more to reveal.
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
				/* Interior mechanism — bounded by genuine foundations. Done; stitch it below. */
				break;
			}

			/* Re-flood from the moved set so a disconnected standing component is pruned as budget grows. */
			GrowFrontier = MovedPieces.Array();
			bReFloodFromMechanism = true;
		}
		else
		{
			/*
			 * No fall yet — the modest region hasn't reached the mechanism. Grow a speculative
			 * search from the whole boundary and look again, bounded by the speculative ceiling. The
			 * only branch growing a non-mechanism direction, and it runs only while nothing has
			 * fallen, so it can't grow into a standing island beside a live collapse (that path
			 * always falls first and re-floods above).
			 */
			GrowFrontier = Boundary.Array();
			GrowCeiling = RegionSpeculativeCeiling;
		}

		/* Deterministic admission order regardless of TSet iteration order. */
		GrowFrontier.Sort();

		if (EffectiveBudget >= GrowCeiling)
		{
			/*
			 * At the applicable ceiling — cap-bound for a fall (stitch the sound partial, the
			 * router keeps the rest), or the speculative ceiling with no fall (an accepted miss).
			 */
			break;
		}

		if (bReFloodFromMechanism)
		{
			/*
			 * Re-flood sized to the mechanism plus one adjacency ring, not a blind doubling. The
			 * budget bounds region plus grounded boundary, so advancing the movable region one ring
			 * costs two joint-hops: the moved set with its neighbours is the region the pose must be
			 * free to move (MovableRing), one hop further is the grounded boundary that pins it
			 * (PinnedRing). Sizing to the movable ring alone would pin it as boundary and reproduce
			 * the same ring — a fixpoint short of the mechanism, hiding a deep collapse behind it.
			 * With the boundary hop the region advances one ring per iteration, so a deeper mechanism
			 * is revealed next iteration. Rebuild from the moved seeds so a disconnected grounded
			 * island falls out; a rebuild reproducing the region is a fixpoint — stitch the current
			 * mechanism.
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
			/*
			 * Speculative re-flood with no mechanism to size to: double the budget and search wider
			 * from the whole boundary, bounded by the speculative ceiling.
			 */
			EffectiveBudget = FMath::Min(GrowCeiling, EffectiveBudget * 2);

			if (GrowFrom(GrowFrontier, EffectiveBudget) == 0)
			{
				/* The speculative frontier admitted nothing even at the larger budget — a fixpoint. Stop. */
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
	 * Falling-only stitch. A grounded boundary block writes no equilibrium rows and never
	 * moves, so the moved blocks are all interior; map them back through PieceOfBlock and
	 * mark each Falling. Never Supported — a region LP is a collapse prover only, and
	 * crediting a stand above the cap is the false-stand direction case-21 forbids. Falling
	 * is written as SolveLoads writes it: not held up and not stranded.
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

	/*
	 * Sever the intact joints the mechanism opens, mapped back through ConnectionOfJoint —
	 * the same idiom BreakByEquilibrium uses. A joint already gone is skipped; this pass
	 * stamps the ones it severs.
	 */
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
	/*
	 * The oracle-block count the last regional prove posed (|region ∪ grounded boundary|),
	 * or INDEX_NONE if none has. The flood bounds it at RegionBlockCap, so it's the plan's
	 * stopping-gate witness.
	 */
	return LastRegionalProblemBlockCount;
}

int32 FStructure::GetLastEquilibriumProblemDim() const
{
	/*
	 * The dimension the last equilibrium-gate pose was built in (2 for X-Z, 3 for
	 * volumetric), or INDEX_NONE if none. Stamped from Problem.Dim when the bridge accepts,
	 * so a declined pass reports the last pose. Test-only; no production code branches on it.
	 */
	return LastEquilibriumProblemDim;
}

void FStructure::SetEquilibriumGateBlockCap(int32 MaxBlocks)
{
	/*
	 * Stores the cap the equilibrium gate consults: authoritative at or below it, fail-closed
	 * to the router above (PROMOTION_DESIGN.md §12 D6-c). Injectable so the
	 * scope-by-size tests can drive either side on a small fixture. A bare assignment, so it
	 * drives no behaviour on its own.
	 */
	EquilibriumGateBlockCap = MaxBlocks;
}

void FStructure::SetRegionBlockCap(int32 MaxBlocks)
{
	/*
	 * Stores the cap the regional prover's flood consults above the cap
	 * (REGIONAL_PROVER_PLAN.md §4): the largest |region ∪ grounded boundary| it may
	 * pose. Defaults to 200, matching the equilibrium gate — affordable because
	 * grow-from-modest poses a mechanism-sized region, so the cap is only the ceiling a large
	 * propagating collapse extends toward. A bare assignment; the cascade seam reads it.
	 */
	RegionalProverBlockCap = MaxBlocks;
}

void FStructure::SetPieceMaterial(int32 PieceIndex, const DestructionProfiles::FMaterialProfile* Material)
{
	/*
	 * Records what a piece is made of so a cross-material joint can reach its two faces'
	 * materials (via EffectiveBondedStrength). A bare store behind a range guard; an
	 * out-of-range handle is ignored, the same fail-closed shape as the other accessors.
	 */
	if (Pieces.IsValidIndex(PieceIndex))
	{
		Pieces[PieceIndex].Material = Material;
	}
}

void FStructure::SetThreeDimensional(bool bIsThreeDimensional)
{
	/*
	 * The 3D permission RigidBlockBridge branches on (THREED_DESIGN.md E3). When set,
	 * BuildRigidBlockProblem may pose this structure as Dim3D — real 3D joint geometry,
	 * out-of-plane Y normal accepted — but only when the posed problem actually leaves the
	 * X-Z plane; a flagged structure whose posed rows are all planar is posed 2D. When unset
	 * (default), the 2D X-Z pose and its Y-normal refusal stand.
	 */
	bThreeDimensional = bIsThreeDimensional;
}

bool FStructure::IsThreeDimensional() const
{
	return bThreeDimensional;
}

int32 FStructure::GetBreakPass(int32 ConnectionIndex) const
{
	/*
	 * An unknown connection didn't break, so it fails closed to the same answer as a joint
	 * that never gave.
	 */
	return ConnectionBreakPass.IsValidIndex(ConnectionIndex)
		? ConnectionBreakPass[ConnectionIndex]
		: INDEX_NONE;
}

int32 FStructure::GetBreakAuthority(int32 ConnectionIndex) const
{
	/*
	 * An unknown connection severed nothing, so it fails closed to INDEX_NONE. See the
	 * header for the code meanings (1 gate, 2 sweep, 3 prover).
	 */
	return ConnectionBreakAuthority.IsValidIndex(ConnectionIndex)
		? ConnectionBreakAuthority[ConnectionIndex]
		: INDEX_NONE;
}

EJointRole FStructure::GetJointRole(int32 ConnectionIndex, int32 PieceIndex) const
{
	/*
	 * The decision SolveLoads itself routes by (it calls this rather than carrying its own
	 * copy), so a readout of a tier and the tier the load took are the same answer. A
	 * connection's normal points toward PieceB, so it's turned to point at the asked piece:
	 * substantially up, the interface is beneath and bears it; substantially down, the bed
	 * joint is above and neither holds it up. That direction is the whole correction —
	 * routing that only asked whether two pieces were joined let a load path run upward. A
	 * normal that won't normalise is answered None, not given a tier (a NaN would fall into
	 * Head, a support tier). An unknown connection needs its own guard: the placeholder's
	 * normal is (0,0,1), which normalises, and its PieceB of INDEX_NONE once matched an
	 * unidentified piece and returned BedBeneath, the strongest support tier, for a joint and
	 * piece that don't exist. With the handle known good the piece check needs nothing of its
	 * own, since AddConnection refuses a joint whose ends aren't both valid.
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
	 * Zero for an out-of-range handle, and for a connection no solve reached — nothing in
	 * an ungrounded island is held up, so there's no static load path to report.
	 */
	return ConnectionForces.IsValidIndex(ConnectionIndex)
		? ConnectionForces[ConnectionIndex]
		: FVector::ZeroVector;
}

FVector FStructure::GetConnectionMoment(int32 ConnectionIndex) const
{
	/*
	 * Zero for an out-of-range handle and for a connection no solve reached, the same scope
	 * as GetConnectionForce. A moment is a load, not a verdict, so the fail-closed answer is
	 * the opposite polarity to GetConnectionUtilisation's Max(): zero says nothing levers
	 * this, and an invented moment would make a NaN of everything downstream.
	 */
	return ConnectionMoments.IsValidIndex(ConnectionIndex)
		? ConnectionMoments[ConnectionIndex]
		: FVector::ZeroVector;
}

double FStructure::GetConnectionCompositeDepthCm(int32 ConnectionIndex) const
{
	/*
	 * Zero for an out-of-range handle and for a connection no solve reached, the same scope
	 * as the two accessors above. A depth is a relief, not a load, so zero fails closed for
	 * the opposite reason GetConnectionMoment's does: no masonry credited means the joint
	 * reads its own bed patch and reports heavily loaded, and an invented depth would quietly
	 * relieve one.
	 */
	return ConnectionCompositeDepthCm.IsValidIndex(ConnectionIndex)
		? ConnectionCompositeDepthCm[ConnectionIndex]
		: 0.0;
}

double FStructure::GetConnectionUtilisation(int32 ConnectionIndex) const
{
	/*
	 * Delegated whole, never re-derived: FConnection::UtilisationUnder is the evaluator the
	 * break decision is made on, so composing it over the routed force keeps this from being
	 * a hand-copy and inherits the degenerate-normal guard. Force, moment and depth come off
	 * the accessors, not the arrays, which makes the identity Structure.h states true by
	 * construction; dropping the moment reads too optimistic, dropping the depth too
	 * pessimistic. The strength is the weakest-link pairing, so a wood-on-brick bearing reads
	 * its material crush; where neither face names a material this is bit-identical to the
	 * bare connection.
	 */
	FConnection Paired = GetConnection(ConnectionIndex);
	Paired.Strength = EffectiveJointStrength(ConnectionIndex);

	return Paired.UtilisationUnder(
		GetConnectionForce(ConnectionIndex),
		GetConnectionMoment(ConnectionIndex),
		GetConnectionCompositeDepthCm(ConnectionIndex));
}

FConnectionStrength FStructure::EffectiveJointStrength(int32 ConnectionIndex) const
{
	/*
	 * Pair the connection with its two faces' materials, weakest-link, only when both name
	 * one (via EffectiveBondedStrength: min(connection, matA, matB) per axis). When either is
	 * null — "nobody said" — the bare connection governs, the fail-safe default every
	 * material-free fixture keeps reading.
	 */
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
	/*
	 * The cached min-violation readout, or absent. BreakByEquilibrium fills it solve-on-settle
	 * below the cap; above the cap the gate declines, nothing solves it, and the cache stays
	 * empty so this reads absent and the overlay falls back to GetConnectionUtilisation.
	 * Absent too for an out-of-range handle and before any solve.
	 */
	return ConnectionReadoutCache.IsValidIndex(ConnectionIndex)
		? ConnectionReadoutCache[ConnectionIndex]
		: FConnectionReadout{};
}

int32 FStructure::GetMinViolationReadoutSolveCount() const
{
	// Observability only — see the header. Bare accessor, no logic.
	return MinViolationReadoutSolves;
}

bool FStructure::IsPieceSupported(int32 PieceIndex) const
{
	// An unknown piece is not being held up.
	return PieceSupported.IsValidIndex(PieceIndex) && PieceSupported[PieceIndex];
}

EPieceSupport FStructure::GetPieceSupport(int32 PieceIndex) const
{
	/*
	 * Derived from IsPieceSupported, not computed beside it: the two are one answer at two
	 * resolutions, so asking the coarser here makes IsPieceSupported(H) == (GetPieceSupport
	 * is Grounded or Supported) true by construction, including for an unknown handle and
	 * before any solve. Reading support first means that if a piece ever became both stranded
	 * and supported the answer would still agree with the boolean.
	 */
	if (IsPieceSupported(PieceIndex))
	{
		return GetPiece(PieceIndex).bIsGrounded ? EPieceSupport::Grounded : EPieceSupport::Supported;
	}

	/*
	 * Stranded is a claim about the solver, made only about a piece the last solve found in a
	 * knot. Everything else is Falling: a piece resting only on a knot, one with nothing
	 * beneath it, a removed piece once re-solved, and an unknown handle.
	 */
	return PieceStranded.IsValidIndex(PieceIndex) && PieceStranded[PieceIndex]
		? EPieceSupport::Stranded
		: EPieceSupport::Falling;
}

bool FStructure::HasSupportAnswer(int32 PieceIndex) const
{
	/*
	 * The array's own extent is the answer, which is why there's no "have we solved yet"
	 * flag: SolveLoads sizes PieceSupported to the piece count and nothing else writes it, so
	 * a flag would be a second copy — and would answer the wrong question (whether a solve
	 * happened, not whether it reached this handle). The same range check IsPieceSupported
	 * makes, asked here for its own sake.
	 */
	return PieceSupported.IsValidIndex(PieceIndex);
}
