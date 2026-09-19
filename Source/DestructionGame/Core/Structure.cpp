// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Structure.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/RigidBlock/RigidBlockBridge.h"
#include "HAL/PlatformTime.h"

/*
 * The solver's own log, so a break decision can be read out without a debugger: a Log
 * line per cascade that broke something or cost more than a frame, Verbose per pass.
 */
DEFINE_LOG_CATEGORY_STATIC(LogDestructionSolve, Log, All);

/*
 * Every name here carries a Solver prefix — not decoration. An anonymous namespace is
 * private to a translation unit, not a file, so a unity build merges every file's
 * anonymous namespace into one; a name colliding with a test file's support constants
 * (the tier cosine is legitimately spelled twice, to agree with GetJointRole bit for
 * bit) is a hard compile error, which already happened once, luck-dependent on how UBT
 * partitions the blob. Prefixed per CURRENT_STATE.md; grep still finds the lockstep
 * pair below since the distinctive part of each name is unchanged.
 */
namespace
{
	/**
	 * Unreal's gravity, 980 cm/s2.
	 *
	 * Mass is already kg and length already cm, so MassKg * 980 is a force in Unreal units
	 * directly — the 1 N = 100 uu conversion (DESIGN.md §3) is baked into the 980 rather than
	 * applied on top of it. Multiplying by 100 again here is the standard way to be wrong by
	 * exactly 100x.
	 */
	constexpr double SolverGravityCmPerSecondSquared = 980.0;

	/**
	 * Where the line between a bed joint and a head joint sits: cos(45 degrees).
	 *
	 * DESIGN.md §3 makes support two-tiered on whether the interface normal is "substantially
	 * vertical"; 45 degrees is the threshold that prefers neither tier and needs no material
	 * data, which matters since the tier is decided before any strength profile is consulted. A
	 * friction-based line (arctan mu) would be more physical but would make the load path depend
	 * on the connection profile — left to the non-gravity generalisation DESIGN.md §3 already
	 * flags as outstanding.
	 */
	constexpr double SolverBedJointCosine = 0.70710678118654752440;

	/**
	 * How deep an arch may be, as a fraction of the span it crosses: sqrt(3)/2, to three digits.
	 *
	 * BS 5977-1 specifies the equilateral triangle of loading over an opening — 60 degree base
	 * angles, height sqrt(3)/2 of the span — as the masonry that arches around rather than
	 * reaching the span. ARCHING_DESIGN.md adopts the angle only as a cap on arching depth, not
	 * to reduce the load — the strictly harsher reading, which is what keeps a dispersion angle
	 * out of the data.
	 *
	 * A modelling constant with a published source, not a material property, so it lives here
	 * beside SolverBedJointCosine rather than as a per-profile dispersion angle or a
	 * `bDevelopsArchAction` flag — the regression DESIGN.md §2 names by name.
	 *
	 * Spelled 0.866 rather than sqrt(3)/2: the published figure, differing from the exact value
	 * by 3e-5 relative, far inside anything this model can distinguish.
	 */
	constexpr double SolverArchingDepthPerSpan = 0.866;

	/**
	 * How deep a deep beam may be, as a multiple of the effective arm of the joint under it.
	 *
	 * Provisional, and a ruling rather than a derivation — COMPOSITE_DEPTH_DESIGN.md slice 3
	 * exists to settle this number and has not been made. What is not provisional is the form: a
	 * bound proportional to the joint's own e = |M|/|F| is the only one that satisfies both ends
	 * at once, and a shear-transfer budget provably cannot bound the depth at all because it is
	 * a floor (tau_max goes as 1/D, so a deeper section is easier to sustain, not harder).
	 *
	 * Where 3.464 comes from, stated as the post-hoc rationalisation it is: it is 2*sqrt(3),
	 * four times SolverArchingDepthPerSpan (BS 5977-1's triangle read over the mirror span 2e a
	 * cantilever is half of, then doubled) — that reuse is why this value was preferred, not why
	 * it is what it is. Published guidance points the other way: EN 1992-1-1 5.3.1(3) calls a
	 * member a deep beam under three times its depth, a validity limit of about 0.67, which
	 * fails the free end by 4x. The free-end ruling is what actually sets this number: a brick
	 * deleted at the end of a wall must not bring it down, which needs at least 2.465; the
	 * one-sided corbel property needs at most 3.822; 3.464 is the only value in that window with
	 * margin on both sides.
	 *
	 * (COMPOSITE_DEPTH_DESIGN.md writes this as "2 x 0.866" = 1.732, reproducing none of that
	 * document's own figures — its K = 1.56129/lambda^2 = 0.130117 identity and its
	 * 0.4493/0.5064 predictions are all 3.464. The value is 3.464; the prose has a slip between
	 * lambda and the lambda/2 its matched-corbel lemma also uses.)
	 *
	 * Spelled as a literal rather than 4 * SolverArchingDepthPerSpan — bit-identical, but the
	 * arching angle does not govern this, and deriving it in code would let a future
	 * lintel-angle correction silently re-rule the free end.
	 */
	constexpr double SolverCompositeDepthPerArm = 3.464;

	/**
	 * How far a joint's rectangle may disagree with its own area, as a fraction of it.
	 *
	 * Relative, because the quantity is an area: an absolute slack that is noise on a square
	 * centimetre is a different face on a square metre.
	 *
	 * Bracketed rather than pinned — anywhere in the band would do. Two derivations of one face
	 * can legitimately disagree in the last few bits (o_u x o_v against 4 x (o_u/2) x (o_v/2)
	 * round identically only because those factors are powers of two), so exact equality is too
	 * strict; but the rule must still catch a rectangle describing a different face, so it
	 * cannot be slack. A relative 1e-12 is re-derivation noise and is accepted; a relative 1e-6
	 * is not reachable by rounding a handful of multiplies and is refused. This sits mid-band,
	 * and Structure.GraphValidation asserts the two ends rather than this number, so moving it
	 * within the band breaks nothing.
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
	 * That, and nothing wider, is what "caught in an unroutable knot" means: a piece that is
	 * ultimately one of its own supports cannot have its load divided without a rule for going
	 * round a loop, which DESIGN.md §3 says we do not have.
	 *
	 * Deliberately not "the ordering could not reach this piece" — Kahn orders top-down, so a
	 * piece comes out unordered whenever a knot sits anywhere above it, stranding everything
	 * resting on the foundation below. Un-orderability is a solver artefact; being your own
	 * support is a fact about the structure.
	 *
	 * The walk stops at a grounded piece, since the earth absorbs what arrives and passes
	 * nothing on — walking through one would find a loop in the most ordinary shape there is,
	 * two bricks on the ground naming each other through their head joint.
	 *
	 * It walks LoadPaths rather than the raw support lists, so it sees exactly the edges the
	 * accumulation does. The starting piece is left unvisited so arriving back at it is detected
	 * rather than skipped.
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
	 * Written !(MassKg >= 0.0) rather than MassKg < 0.0 so a NaN lands inside the guard instead
	 * of slipping past it — every comparison against NaN is false. A mass nobody can make sense
	 * of must not enter the array, since the arithmetic downstream would launder it into
	 * plausible-looking loads. Zero is allowed deliberately; a massless piece is meaningful.
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
	 * A piece added to a structure is in it. The flag defaults false so the placeholder GetPiece
	 * hands back for an unknown handle reads as dead — this is the one place that turns it on.
	 */
	Piece.bIsInTheStructure = true;

	return Pieces.Add(Piece);
}

int32 FStructure::AddPiece(double MassKg, bool bIsGrounded, const FVector& CentreOfMassCm)
{
	/*
	 * A centre that is not finite is refused outright, before the two-argument door is opened,
	 * because that door adds — checked afterwards, the tombstone left behind would make
	 * "refused" indistinguishable from "added and then removed".
	 *
	 * It's the worse of the two nonsense inputs this overload can be handed. A mass that is not
	 * finite makes an obviously broken load; a centre that is not finite becomes a lever arm the
	 * moment SolveLoads subtracts a joint centroid from it, feeding the cross product that
	 * builds the moment through to ComputeUtilisation — where every comparison against NaN is
	 * false, so the joint reads as intact and a wall with one unplaceable brick would stand,
	 * confidently. Storing the centre and clearing the flag wouldn't help either: the piece
	 * would then read as "nobody said where it is", a healthy state it doesn't deserve.
	 *
	 * ContainsNaN is !FMath::IsFinite per component, so an infinity is caught alongside a NaN
	 * and a broken Y as surely as a broken X — which matters, since Y is the axis every bed
	 * joint in a running-bond wall bends about. StructureBinding's own check spells the same
	 * rule one layer up; this is the door, that one is belt and braces.
	 */
	if (CentreOfMassCm.ContainsNaN())
	{
		return INDEX_NONE;
	}

	/*
	 * The same door, with one more fact through it — the two-argument form is called rather than
	 * restating its guards here, which would risk two rules that agree only until somebody
	 * tightens one.
	 */
	const int32 Handle = AddPiece(MassKg, bIsGrounded);

	if (Handle == INDEX_NONE)
	{
		return INDEX_NONE;
	}

	/*
	 * Supplying a centre buys the piece the ability to load a joint eccentrically and nothing
	 * else. The flag keeps "at the origin" and "nobody said" apart: a defaulted zero on a wall
	 * laid off the origin would be a lever arm of metres, invented out of a field nobody wrote.
	 */
	Pieces[Handle].CentreOfMassCm = CentreOfMassCm;
	Pieces[Handle].bHasCentreOfMass = true;

	return Handle;
}

int32 FStructure::AddConnection(const FConnection& Connection)
{
	/*
	 * The structure owns the graph, so it's the only place that can tell a valid piece handle
	 * from a nonsense one. A joint from a piece to itself, or to a piece that doesn't exist, is
	 * not a load path — and nothing below has the piece array needed to notice.
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
	 * Area fails closed at construction rather than at solve time: the load split divides by the
	 * total supporting area, and a zero, negative or NaN area leaves nothing sensible to divide
	 * by. Guard written !(x > 0.0) so NaN is rejected by the same branch as zero. FConnection
	 * keeps its own area guard for callers that bypass the structure; this one is in addition to
	 * it.
	 */
	if (!(Connection.InterfaceAreaSqCm > 0.0) || !FMath::IsFinite(Connection.InterfaceAreaSqCm))
	{
		return INDEX_NONE;
	}

	/*
	 * A normal that will not normalise describes no interface plane, so there's no joint here to
	 * load. Normalize returns false for both a zero-length and a NaN normal, which is why one
	 * check covers both. A non-unit normal is a legitimate description of the same plane and is
	 * stored as given.
	 */
	FVector UnitNormal = Connection.InterfaceNormal;
	if (!UnitNormal.Normalize())
	{
		return INDEX_NONE;
	}

	/*
	 * The joint's own geometry — everything below is conditional on a rectangle having been
	 * supplied at all.
	 *
	 * Zero extents are not a degenerate joint, they're "no bending capacity was ever measured" —
	 * a healthy state, since with no moment the area alone answers a centred load bit for bit,
	 * the same way a zero friction coefficient reduces Mohr-Coulomb exactly rather than
	 * approximately. Checked unconditionally, the consistency rule below rejects 4 x 0 x 0
	 * against a real area and takes every geometry-free fixture with it, tilted ones included.
	 *
	 * Any component non-zero, not all three: a rectangle with one extent left at zero is the
	 * shape of a value somebody assembled by hand and stopped halfway, and it has to reach the
	 * consistency rule as the line it is rather than read as no geometry.
	 */
	if (!Connection.InterfaceHalfExtentCm.IsZero())
	{
		/*
		 * A centre is a world position with no sensible bound, so finiteness is the only rule
		 * for one. A wall laid off the origin puts every joint far from zero — the running-bond
		 * producer's first bed joint already lands at (5.625, 0, 7.0) — and a NaN here launders
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
		 * A rectangle may only be supplied on an axis-aligned normal. The in-plane frame is "the
		 * two world axes that are not the separation axis", which only names a frame when there
		 * is a separation axis; a normal 40 degrees off vertical has two candidates, and the
		 * choice between them silently picks a section modulus.
		 *
		 * MakeInterface sets exactly one component to +/-1 and leaves the other two at zero, so
		 * nothing the producer builds is refused here — a normal a millionth off an axis was not
		 * produced by rounding an exact axis vector, it meant something else. The raw normal is
		 * read rather than the normalised one, so a non-unit (0, 0, 5) is still the same plane.
		 *
		 * A tilted normal carrying no rectangle is untouched by this and stays a perfectly good
		 * geometry-free joint, which is what keeps every tilted fixture buildable.
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
		 * Every half-extent is non-negative and finite, which the area check below doesn't imply
		 * — two negative halves multiply into a plausible 4 x -5 x -5 = 100, so a guard on the
		 * product alone would accept an inside-out rectangle and hand the section modulus a
		 * negative lever arm that flips the sign of every stress downstream (the same trap
		 * PieceMassKg carries a row for, one level up). Written !(x >= 0.0) so NaN lands inside
		 * the guard, and IsFinite checked separately since +inf >= 0.0 is true.
		 *
		 * Zero on the separation axis, exactly, and this is area-blind on purpose: an extent
		 * there can agree with the area perfectly and still describe a box rather than an
		 * interface — a sign whoever wrote it had a different idea of which axes are in-plane.
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
		 * And the two must be the same face. An extent that disagrees with its area is the same
		 * class of fault as a normal that disagrees with its A/B pairing — a plausible number
		 * attached to the wrong geometry — except the area governs the load split while the
		 * rectangle governs the lever arm, and mortar's tensile strength is a hundredth of its
		 * compressive one, so a lever arm quietly out by a factor moves the governing axis, not
		 * just the number.
		 *
		 * Written !(diff <= tol) rather than diff > tol so a NaN difference lands inside the
		 * guard. The area is already positive and finite by this point, so the relative bound is
		 * a bound.
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
	 * The break-pass stamp is grown here, with the connection it belongs to, so the two arrays
	 * are parallel by construction rather than by a solve remembering to resize one. A joint
	 * that has never broken is INDEX_NONE from the moment it exists — the same answer as a
	 * handle that names no joint at all.
	 */
	ConnectionBreakPass.Add(INDEX_NONE);
	ConnectionBreakAuthority.Add(INDEX_NONE);

	return Connections.Add(Connection);
}

bool FStructure::RemovePiece(int32 PieceIndex)
{
	/*
	 * A handle that names no piece, and one whose piece has already gone, both remove nothing
	 * and say so. The second matters beyond tidiness: removal severs joints, and a second call
	 * going through would sever a joint the cascade had already stamped, or decrement a live
	 * count already dropped.
	 */
	if (IsPieceRemoved(PieceIndex))
	{
		return false;
	}

	/*
	 * Tombstone, never compaction. The slot stays where it is and no piece ever moves down into
	 * it, since every handle in the connection array is an index into this one — see the note on
	 * bIsInTheStructure for why a free list isn't the tidy-up it looks like.
	 */
	Pieces[PieceIndex].bIsInTheStructure = false;

	/*
	 * A joint holding a piece that's no longer there isn't a joint, so it leaves the structure
	 * with it — severing is the whole of removal's effect on the load model. SolveLoads already
	 * drops a given joint at the top of the tier decision, so this one line takes the piece out
	 * of the tier, the reachability walk, the load paths, the accumulation order and the split
	 * at once, through the path the cascade already uses rather than a second one beside it.
	 *
	 * By reference. FConnection is copyable and the latch is a member, so
	 *
	 *     for (FConnection Connection : Connections)
	 *
	 * — one missing ampersand — would sever a temporary and leave the real joints holding a
	 * piece that no longer exists.
	 *
	 * Nothing is stamped. ConnectionBreakPass records what failed under load, the sequence phase
	 * 5 plays back; a joint that went with its piece never snapped and must not appear there.
	 * HasGiven answers the separate question of whether it's still in the structure, and the
	 * pair encodes all three states with no sentinel — see GetBreakPass.
	 */
	for (FConnection& Connection : Connections)
	{
		if (Connection.PieceA == PieceIndex || Connection.PieceB == PieceIndex)
		{
			Connection.Sever();
		}
	}

	/*
	 * The forces the last solve computed are left as they are; removal is deliberately immediate
	 * rather than re-solving here, since what a structure carries is SolveLoads' answer to give
	 * — a caller takes out however many pieces it means to and then asks. Every joint severed
	 * above is guaranteed a zero by the next solve, which starts from zero and never reaches a
	 * given joint.
	 */
	return true;
}

bool FStructure::IsPieceRemoved(int32 PieceIndex) const
{
	/*
	 * An unknown handle reads as removed — the fail-closed direction, and the same answer the
	 * accessors either side of it give: a caller filtering with `if (IsPieceRemoved(H))
	 * continue;` skips a handle naming nothing rather than walking on into it as though it were
	 * live.
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
	 * Counted rather than cached, so it can't drift out of step with the tombstones it counts.
	 * NumPieces is the handle range; this is the other question, and the two diverging is the
	 * whole point of leaving the hole.
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
	 * A conjunction over what is still in the structure. A moment needs a point for the load to
	 * act at and a rectangle for the joint to resist it with, so either half missing anywhere
	 * means some joint here answers a centred load because it has to, not because the load is
	 * centred.
	 *
	 * A removed piece and a joint that has given are out of the graph entirely — they carry and
	 * route nothing — so a tombstone left by a piece nobody placed must not condemn a structure
	 * whose live half is fully described. A predicate walking the raw arrays would say false
	 * forever the first time a player pulled a brick.
	 *
	 * An empty structure reads true, the empty conjunction rather than a special case, which is
	 * what keeps this composable: adding a fully described piece to a complete structure leaves
	 * it complete.
	 */
	for (const FStructurePiece& Piece : Pieces)
	{
		if (Piece.bIsInTheStructure && !Piece.bHasCentreOfMass)
		{
			return false;
		}
	}

	/*
	 * Zero extents are the absence of a rectangle, the same reading AddConnection uses to decide
	 * whether there's one to validate at all. A face with no extent on any axis is not a
	 * degenerate joint — the area alone answers a centred load exactly — it's a joint whose
	 * bending capacity nobody measured, which is precisely what this predicate exists to make
	 * visible.
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
	 * Counted at the door, once per call, and nothing here reads it back. The fixpoint below
	 * iterates, but those iterations are one solve rather than several: what a caller is charged
	 * for is the call. SolveAndBreak makes several calls and is meant to, so this is a count and
	 * never a budget.
	 */
	++SolveCount;

	/*
	 * Profiled, not budgeted. The phase clocks below feed GetLastSolveLoadsProfile and nothing
	 * else; a Seconds() read costs tens of nanoseconds against a solve that costs milliseconds.
	 */
	LastSolveLoadsProfile = FSolveLoadsProfile();
	const double ProfileStartSeconds = FPlatformTime::Seconds();

	/*
	 * Which joints touch which piece, built once by walking the connections.
	 *
	 * The tier decision below wants a piece's own joints and nothing else, and used to find them
	 * by asking every connection about every piece — pieces x connections calls to GetJointRole,
	 * almost all answering None. The scenario wall is 1,220 pieces against ~3,500 joints, so
	 * that's 4.3 million calls per solve, and a cascade runs a solve per pass: a bottom-course
	 * delete measured 31 passes and about 1.25 s of visible lag. One walk over the joints costs
	 * what a single piece used to, and the tier loop then reads the six or so a brick actually
	 * has.
	 *
	 * Ascending connection index is part of the contract, not an incidental. Appending in index
	 * order to both endpoints leaves each list ordered exactly as the old connection-major sweep
	 * left it, so the support lists, the split and every floating-point sum built on them
	 * reproduce bit for bit. Sorting, filling out of order, or a hash container with different
	 * iteration order would each reorder an accumulation whose last bit decides breaks — the
	 * cascade fuzz has five joints settling at exactly 1.0 and one a single ulp below it.
	 *
	 * Reserving is not on that list — what breaks the contract is append order, not storage
	 * sizing, so a count-then-fill layout (count each piece's joints, prefix sum, then a second
	 * ascending pass with a per-piece cursor) would preserve this exactly while removing the
	 * ~3,600 small allocations a scenario-scale solve makes here.
	 *
	 * The bounds check is here because this side writes. AddConnection refuses a joint whose
	 * ends aren't both valid piece handles, which is what entitles GetJointRole to read a stored
	 * connection with no check of its own; an index chosen to select an element to append to has
	 * no such licence, since there the failure is a memory overwrite rather than a wrong tier.
	 * It's a bounds check on a write, not a defence of the whole path — step three still indexes
	 * Loaders by OtherEndOf with no check. Both are unreachable through the public doors; this
	 * is simply where a write is.
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
	 * Step one: which connections hold each piece up?
	 *
	 * Two-tiered, per DESIGN.md §3: a piece rests on the bed joints beneath it, and only a piece
	 * with none of those falls back to its head joints. A bed joint above bears nothing at all —
	 * that's something resting on this piece.
	 *
	 * This is the correction the whole solver turned on. Routing purely by graph distance to the
	 * ground let a short sideways path exclude a bed joint: a brick spanning a gap ended up the
	 * same distance from the earth as the brick resting on top of it, so the joint between them
	 * carried zero and the keystone bore none of the wall. Classification was direction-aware
	 * from the start; routing was blind, and routing decides where the load ends up.
	 */
	TArray<TArray<int32>> SupportConnections;
	SupportConnections.SetNum(Pieces.Num());

	/*
	 * And which of them have no seat at all, recorded here because this is the loop that knows —
	 * the fallback firing, which nothing downstream can tell apart from a piece with one seat by
	 * looking at the finished list alone. A hole one brick wide leaves nobody in this set; a
	 * wider one leaves the bricks in the middle, which is what ReseatSpannedGroups is for.
	 *
	 * Grounded pieces are not in it, and neither is a tombstone — the earth needs no seat, so a
	 * grounded piece is a good abutment for a group to push against rather than a member of one.
	 */
	TArray<bool> PieceHasNoSeat;
	PieceHasNoSeat.Init(false, Pieces.Num());

	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		TArray<int32> HeadConnections;

		for (const int32 Index : PieceJoints[PieceIndex])
		{
			/*
			 * A joint that has given is out of the structure and conducts nothing, and dropping
			 * it here — before the tier is even decided — is what makes that true everywhere at
			 * once: it leaves the support lists, and with them the reachability walk, the load
			 * paths, the accumulation order and the split.
			 *
			 * It has to leave the tier decision, not merely the load path. A bed joint wins the
			 * tier outright over any number of head joints, so a broken bed joint that still won
			 * would leave the piece with an empty support list — reporting it as falling with
			 * its intact head joint carrying zero, self-consistent, plausible-looking and wrong.
			 * The piece is meant to fall back onto the head joint and load it in shear.
			 *
			 * Its force stays at the zero every pass starts from, which is the zero
			 * redistribution is built on: the share it used to carry is now divided among the
			 * supports left.
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
		 * The fallback, and only the fallback: one bed joint beneath wins outright over any
		 * number of head joints, because a joint that can bear in compression is what is
		 * actually carrying the piece.
		 */
		if (SupportConnections[PieceIndex].Num() == 0)
		{
			PieceHasNoSeat[PieceIndex] =
				Pieces[PieceIndex].bIsInTheStructure && !Pieces[PieceIndex].bIsGrounded;

			SupportConnections[PieceIndex] = MoveTemp(HeadConnections);
		}
	}

	/*
	 * Step one and a half: a run of pieces with no seat between them spans the hole rather than
	 * hanging sideways off its edges.
	 *
	 * The only place in the solver that reads geometry to decide a route, and it sits above the
	 * tier rather than inside it — GetJointRole is untouched, and the tier of one joint is still
	 * a fact about one normal and one pairing. See ReseatSpannedGroups for the revision to
	 * MOMENTS_DESIGN's discipline line, and for why the whole pass is a no-op on a structure
	 * nobody placed.
	 *
	 * Before Loaders, before the fixpoint, and it's the last thing to touch SupportConnections —
	 * every step below reads that one list, which is what stops any of them forgetting the
	 * re-seat the way the old solver forgot to filter falling supports.
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
	 * The same relation read the other way: who rests on each piece. Both remaining steps walk
	 * the support relation backwards, and building it once is what lets them.
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
	 * Pieces caught in a knot the solver cannot route, reported as not held up at all.
	 *
	 * Kahn (step four) never makes the members of a cycle ready, so their joints keep a load of
	 * zero and the weight they carry never reaches the earth. Left there, IsPieceSupported would
	 * say "held up" while GetConnectionForce said "carrying nothing" — two answers to the same
	 * solve, contradicting each other with no signal. Reporting the piece unsupported makes them
	 * agree again, the fail-closed direction: DESIGN.md §3 already says a piece with no path to
	 * ground is unsupported. It does not claim the cycle is solved; dividing load round a loop
	 * needs a rule we don't have.
	 *
	 * This set only ever grows, which is what makes the fixpoint below terminate, and it is kept
	 * rather than discarded, as a member beside PieceSupported: it's the reason behind that
	 * array's answer, and GetPieceSupport hands it out. Rebuilt from scratch here, exactly like
	 * the rest of the solver's output, so a knot since dissolved — by a break or a removal —
	 * doesn't survive the next solve as a stale claim.
	 */
	PieceStranded.Init(false, Pieces.Num());

	/*
	 * And which pieces have overturned off their supports, kept beside the stranded set for the
	 * same reason: a body on two or more compression-only bearings whose centre of mass projects
	 * outside their union has no admissible equilibrium and must lose the earth, yet no
	 * per-joint number sees it — each seat reads a comfortable split. Detected during the
	 * accumulation below (once the load paths are known) and, like stranding, excludes the piece
	 * from the next pass's reachability walk so it comes out unsupported. A local, not a member:
	 * GetPieceSupport needs only PieceSupported and PieceStranded — an overturned piece is
	 * neither supported nor caught in a knot, so it falls through to Falling, which is what it
	 * is. Grows monotonically across passes, so the fixpoint still terminates.
	 */
	TArray<bool> PieceOverturned;
	PieceOverturned.Init(false, Pieces.Num());

	/*
	 * And which pieces a refused one-sided arch has released, kept beside the overturned set for
	 * the same reason and used the same way. A run of seatless pieces spanning a hole forms a
	 * group; when its one-sided abutment makes the opposition gate refuse the arch
	 * (PieceInRefusedArchGroup, filled by ReseatSpannedGroups), the run keeps its sign-blind
	 * head joints, and if those form a mutual-support cycle the stranding pass below would
	 * report the run Stranded. But a refused arch was the run's only hope of a path to earth, so
	 * the honest answer is Falling, not the false knot Stranded (DESIGN §8 case-21 scope: above
	 * the cap the brittle answer governs and a one-sided masonry cantilever falls). So when the
	 * cycle test fires on a refused-group member, it's released here rather than stranded —
	 * excluded from the next pass's walk exactly as an overturned piece is, so it and anything
	 * resting only on it come out unsupported and read Falling. Gated strictly on refused-group
	 * membership, which forms only under complete geometry, so a geometry-free mutually-propping
	 * pair keeps its genuine Stranded. Grows monotonically across passes, so the fixpoint still
	 * terminates.
	 */
	TArray<bool> PieceReleasedFromRefusedArch;
	PieceReleasedFromRefusedArch.Init(false, Pieces.Num());

	/*
	 * Reachability and the load split depend on each other, so the solve runs to a fixpoint
	 * rather than one pass. Stranding a knot changes which pieces reach the ground; that changes
	 * which supports the split may use; and that can strand a piece resting only on the one just
	 * stranded. Each pass either strands at least one more piece or is the last, so this runs at
	 * most NumPieces + 1 times. Every pass is a complete solve from scratch, so the final one's
	 * answer stands and the discarded passes leave nothing behind.
	 *
	 * Everything the passes work in is declared inside the loop and rebuilt at first use —
	 * hoisting would be correct today but would only invite a later pass to read what an earlier
	 * one wrote.
	 */
	const double FixpointStartSeconds = FPlatformTime::Seconds();

	for (;;)
	{
		++LastSolveLoadsProfile.FixpointIterations;
		LastSolveLoadsProfile.SupportedPerIteration.Add(0);
		LastSolveLoadsProfile.OverturnedPerIteration.Add(0);
		LastSolveLoadsProfile.StrandedPerIteration.Add(0);
		LastSolveLoadsProfile.ReleasedPerIteration.Add(0);

		/*
		 * Recomputed from scratch every pass, so re-solving — and re-trying after a stranding —
		 * gives the same answer rather than accumulating onto the last one.
		 */
		ConnectionForces.Init(FVector::ZeroVector, Connections.Num());
		ConnectionMoments.Init(FVector::ZeroVector, Connections.Num());
		ConnectionCompositeDepthCm.Init(0.0, Connections.Num());
		PieceSupported.Init(false, Pieces.Num());

		/*
		 * Step two: which pieces reach the ground?
		 *
		 * A breadth-first walk outward from every grounded piece at once, over support rather
		 * than raw connectivity. Being joined to a neighbour is not support, and neither is
		 * being joined to something that hangs off you — a piece glued under a grounded slab
		 * touches the earth and is not held up by it.
		 *
		 * Marking each piece once is what makes a cycle safe: two pieces can each hang from the
		 * other, and a walk that didn't track where it had been would sit there forever instead
		 * of correctly calling both unsupported.
		 *
		 * A stranded piece conducts nothing: the walk neither marks it nor continues through it,
		 * which is what lets a later pass see that whatever rested on it has lost its only path
		 * to the earth.
		 *
		 * A removed piece is not a root, the one line of removal that isn't free. Severing its
		 * joints takes it out of every edge in the relation, but a grounded piece seeds this
		 * walk on its own account rather than through any joint — so without the second conjunct
		 * a removed grounded piece would report itself held up by an earth it no longer rests
		 * on.
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
				 * An overturned body is excluded exactly as a stranded one is: the walk neither
				 * marks it supported nor crosses it, so a piece resting only on something that
				 * has toppled loses its own path to earth on the next pass. Empty on the first
				 * pass, so the walk is bit-identical until the accumulation below finds a body
				 * past tipping.
				 *
				 * A piece released from a refused arch is excluded on the same footing: a
				 * seatless run whose one-sided arch the opposition gate declined, then found by
				 * the stranding pass in a sign-blind head-joint cycle, has genuinely no load
				 * path — so from the next pass on it must not be reached through those head
				 * joints and drops clean through to Falling. Empty on the first pass and grows
				 * only when the stranding pass releases a refused-group member, so a refused
				 * cantilever reaching an abutment without a cycle is never released and keeps
				 * its Supported reading unchanged.
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
		 * Step three: a support that is itself falling is not a support.
		 *
		 * The split may only use supports with their own path to the earth. A share given to a
		 * piece that never reaches the ground is credited to something falling and stops there:
		 * the load never arrives, and — worse — the joint actually carrying it reports only its
		 * fraction, so a joint at 1.9x utilisation reads 0.95x and stands there forever.
		 *
		 * Filtering once, into the list every later step reads, is deliberate. The defect this
		 * replaced consulted PieceSupported when deciding who pushes load but not when deciding
		 * where it goes; one list nothing can forget to filter is what stops that coming back.
		 *
		 * Dropping falling supports is always safe: a supported ungrounded piece was only marked
		 * supported because the walk above reached it through a supported support, so at least
		 * one always survives and there is never nothing left to divide by. The load path of
		 * each piece: its supports that themselves reach the ground.
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
		 * A piece must receive everything above it before passing anything on, so the order is a
		 * topological sort of the support relation: a piece is ready once every piece resting on
		 * it has been dealt with. Distance to the ground is not that order — under the two-tier
		 * rule a spanning brick and the brick sitting on it are the same distance from the
		 * earth, and the second loads the first.
		 *
		 * Only supported, ungrounded pieces push load. A grounded piece terminates the flow —
		 * whatever reaches it is taken by the earth — and an unsupported piece isn't held up by
		 * anything, so there's no static load path to report.
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
		 * What arrived from above is a force and a moment, and a moment is only a number about a
		 * point — this one is about the receiving piece's own centre of mass, a bookkeeping
		 * choice rather than a physical one: transfer is transitive, so re-referencing joint to
		 * centre to joint gives bit for bit what re-referencing joint straight to joint gives,
		 * and a per-piece accumulator needs one point per piece rather than one per joint.
		 *
		 * Not the world origin, deliberately: arithmetically valid but numerically awful, since
		 * every entry would be the moment of the whole wall about a point a wall-length away,
		 * and each joint's own answer would come back as the difference of two huge numbers.
		 *
		 * Zero for a piece nobody placed, and nothing is ever added to one — with no centre
		 * there's no point to be about, and its joints already answer a centred load exactly as
		 * they did before moments existed.
		 */
		TArray<FVector> ReceivedMomentUuCm;
		ReceivedMomentUuCm.Init(FVector::ZeroVector, Pieces.Num());

		/*
		 * Set when this pass finds a body past tipping, so the fixpoint runs once more with it
		 * excluded — the same role bStrandedThisPass plays for a knot. A pass that only
		 * overturns (strands nothing) still isn't the last one.
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
			 * A supported ungrounded piece always has at least one support that reaches the
			 * ground — that's how it was reached — and AddConnection rejects any area that isn't
			 * finite and positive, so this can't be false. Written as a positive test so a NaN
			 * total lands on "cannot split" rather than producing a confident-looking share out
			 * of a division by nothing, and kept inside the loop below rather than skipped so
			 * the pieces underneath still become ready.
			 */
			const bool bCanSplit = TotalAreaSqCm > 0.0;

			/*
			 * A piece on exactly one support is statically determinate, and on several it is not
			 * — that's the whole of the moment rule, and the obvious alternative (every
			 * supporting joint carrying its own share crossed with its own lever arm) isn't an
			 * approximation of it but a different, wrong answer. On an ordinary running-bond
			 * brick with two symmetric bed patches the moments cancel across the pair, not on
			 * either one, so each joint would read sigma_b = 4.18e-3 against sigma_n = 1.269e-3
			 * and every bed joint in a standing wall would report about 0.029 in tension — a
			 * wall half-peeled everywhere, from a rule that looks like ordinary statics.
			 *
			 * On one support the reaction has nowhere to move to and the moment is exact. On
			 * several, the reactions rearrange until moment equilibrium is satisfied, and this
			 * design has no rule for dividing that, so the area split stands and the moment
			 * stays zero — exact wherever the centre of mass sits at the area-weighted centroid
			 * of the supports, which every symmetric running bond does, and unconservative
			 * otherwise. Recorded as such in MOMENTS_DESIGN.md rather than hidden.
			 *
			 * A piece nobody placed carries no moment at all, which is not a tolerance: with no
			 * eccentricity the bending term vanishes and every joint reads bit for bit what it
			 * read before moments existed — the only reason every geometry-free fixture still
			 * works.
			 *
			 * And a piece re-seated onto an arch is indeterminate however many edges it has
			 * left, the same rule rather than an exception to it. ReseatSpannedGroups only
			 * routes a group that something seated stands on both sides of, so the reaction to
			 * an unseated brick's weight is shared between two abutments; the one head joint
			 * left in its load path is the bookkeeping route for the vertical share, not a claim
			 * that the brick hangs off that joint alone. Treated as determinate it would carry
			 * its whole column across the 11.25 cm to that joint's centroid — MOMENTS_DESIGN
			 * case (b) multiplied by twenty-eight brick weights, about 11.6 of capacity — and
			 * the head joint would snap, so the arch would fail for a new reason having just
			 * been granted.
			 */
			const bool bLoadPathIsDeterminate = LoadPaths[Current].Num() == 1
				&& Pieces[Current].bHasCentreOfMass && !PieceReseatedOnAnArch[Current];

			/*
			 * Before the moment is zeroed for two or more supports, ask whether the body
			 * overturns. Zeroing is right for a piece whose centre of mass sits over its
			 * supports — reactions rearrange with no couple on either seat — but wrong for one
			 * whose centre of mass has left the region its supports can push up through with no
			 * tension to hold it down: there is no admissible equilibrium, and pretending each
			 * seat carries a comfortable share leaves a board floating on bearings it has
			 * toppled off (the shed's ridge, once its back gable is gone).
			 * PieceOverturnsOffItsSupports answers that — tension clause first, then centre of
			 * mass against the support union — and on yes the piece is marked so the next
			 * reachability walk drops it and it comes out Falling.
			 *
			 * Nothing else about this pass changes: the piece is still split and still reads
			 * Supported for the rest of this pass (its forces are discarded when the fixpoint
			 * re-runs), so every piece is bit-identical until a body past tipping is found.
			 * Gated on N >= 2 so single-support determinate pieces, already handled exactly by
			 * the moment branch, never reach it.
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
					 * Straight down, pointing at whichever end of the joint is being held up.
					 * Gravity doesn't change direction because a joint happens to be vertical —
					 * FConnection resolves this same vector as compression on a bed joint and
					 * shear on a head joint, against the joint's own normal.
					 *
					 * And it's no longer the whole of what a springing carries:
					 * ApplyArchingThrust runs once this accumulation has settled and adds a
					 * horizontal component at the two abutments of every spanned opening. Every
					 * joint no arch touches keeps exactly the vector built here, bit for bit.
					 *
					 * The sign is not free. ConnectionLoad.h's convention is that the force
					 * belonging to a connection acts on PieceB, the piece the normal points
					 * toward. When the loaded piece is PieceA, describing the same joint from
					 * PieceB means flipping the normal and taking the equal-and-opposite
					 * reaction, so the stored force points up. Get it wrong and a plainly
					 * compressed joint reads as tension — mortar's tensile limit is 0.1 MPa
					 * against 10 MPa compressive, so it would give at one percent of its real
					 * capacity.
					 */
					const double SignedZUU = Connection.PieceB == Current ? -ShareUU : ShareUU;

					/*
					 * Assignment, not accumulation: a connection is the support of at most one
					 * of its two endpoints among the pieces reached here, since two pieces each
					 * supporting the other form a cycle the ordering never makes ready — and
					 * which the pass below then strands outright.
					 */
					ConnectionForces[Index] = FVector(0.0, 0.0, SignedZUU);

					/*
					 * The same share as a physical force — straight down, whichever end the
					 * producer named first. Every moment below is built from this rather than
					 * the stored vector, so the declaration-order sign is applied once, where
					 * the answer is stored.
					 */
					const FVector ShareWeightUu(0.0, 0.0, -ShareUU);

					/*
					 * The joint also has to know where it is — the other half of the conjunction
					 * HasCompleteGeometry asks. A joint with no rectangle has no centroid
					 * either, and its zero means "nobody said" rather than the world origin;
					 * subtracting it from a placed piece would invent metres of eccentricity out
					 * of a field nobody wrote, meet a section modulus of zero, and make
					 * ComputeUtilisation read every joint of a half-described structure as
					 * failed.
					 *
					 * Zero extents are read as the absence of a rectangle here in exactly the
					 * words HasCompleteGeometry uses. A joint with no bending capacity measured
					 * is healthy, not degenerate — the area alone answers a centred load exactly
					 * — so it keeps reading bit for bit what it read before moments existed.
					 */
					const bool bJointKnowsItsFace = !Connection.InterfaceHalfExtentCm.IsZero();

					/*
					 * What this joint carries, about its own centroid: everything that arrived
					 * from above, carried down and re-referenced, plus this piece's own weight
					 * about the same point. Written physically, with the declaration-order sign
					 * put back on at the store below.
					 *
					 * (c_from - c_to) x F is ordinary Varignon, and it's what makes a moment
					 * mean anything as it travels: the received load keeps its own lever arm
					 * instead of being treated as though placed neatly on this piece's middle.
					 * The older rule was exact for a piece carrying only itself and silently
					 * wrong for a corbel carrying a wall — it vanished wherever a chain stacked
					 * squarely, which is why nothing that stacks squarely moves now.
					 *
					 * The moment rides alongside the force and does not change it: the split
					 * stays area-weighted and gravity still points straight down, rather than a
					 * longer or tilted force vector encoding the moment in the one number a
					 * readout uses to explain the load.
					 */
					FVector MomentAboutJointUuCm = FVector::ZeroVector;

					if (bLoadPathIsDeterminate && bJointKnowsItsFace)
					{
						MomentAboutJointUuCm = ReceivedMomentUuCm[Current]
							+ FVector::CrossProduct(
								Pieces[Current].CentreOfMassCm - Connection.InterfaceCentreCm,
								ShareWeightUu);

						/*
						 * A seat with something to push against arches rather than cantilevers.
						 * Delete one brick from a running-bond wall and the brick above keeps
						 * exactly one seat, overhangs it by 5.625 cm, and reads 1.63 of capacity
						 * in tension — so it goes, and the failure walks across the wall at
						 * 33.69 degrees, one step per course. Missing from that picture is the
						 * intact head joint into the hole: the two bricks either side lean on
						 * each other, the thrust line runs through the opening instead of
						 * peeling the seat open, and the same joint reads 0.0142 on the
						 * compression axis. ARCHING_DESIGN.md.
						 *
						 * The four gates split across two objects because they're two kinds of
						 * fact: that the load is compressive and outside the kern is arithmetic
						 * on one face, belonging to the joint; that this is a bed joint beneath
						 * a placed piece with an abutment on the overhanging side is a statement
						 * about the graph, belonging here.
						 *
						 * Cheap test first — the tier and the joint's own two gates cost a
						 * normalise and a handful of multiplies; only a joint that would
						 * actually be relieved pays for the walk over the piece's neighbours. In
						 * an intact wall every seat has e = 0 exactly, so nothing below the
						 * first condition is ever reached.
						 *
						 * The force handed over is the stored one, declaration-order sign and
						 * all, since "compressive" is a fact about the joint only that
						 * orientation can state. The moment goes in physically oriented, costing
						 * nothing — only the magnitude of each in-plane component is read.
						 */
						if (GetJointRole(Index, Current) == EJointRole::BedBeneath)
						{
							const double ArchingRelief = Connection.ArchingMomentScale(
								ConnectionForces[Index], MomentAboutJointUuCm);

							if (ArchingRelief < 1.0)
							{
								/*
								 * The couple the cap would delete is what something else has to
								 * supply, so it's handed over with the gates rather than left
								 * implicit. Capping the moment vector by k removes (1 - k) of
								 * it, and an arch is only an arch if the thrust replacing it can
								 * actually be delivered — HasArchingAbutment measures that
								 * against the seat's own sliding capacity and refuses to be an
								 * abutment where it can't. DESIGN.md §7 gap 4.
								 *
								 * The whole vector's magnitude is the conservative reading: a
								 * component about the seat's own normal would be torsion, which
								 * DESIGN.md §5.3 doesn't model and gravity on an axis-aligned
								 * rectangle never produces, but where the two spellings differ
								 * this one asks for more thrust, so the difference can only
								 * withhold a relief, never grant one.
								 *
								 * Inside the relief test, so an intact wall's seats are relieved
								 * by exactly 1 and never pay for the square root.
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
							 * And what resists what is left is not one bed patch. A stack of
							 * courses over a lost support acts as a deep beam: the plane taking
							 * the overturning moment is a vertical section through the bonded
							 * masonry standing over this joint, t*D^2/6, against the patch's own
							 * 179.48 cm3 — eleven courses is a factor of sixty-five, deciding
							 * whether a brick deleted at a free end takes the wall with it.
							 * ARCHING_DESIGN.md slice 5, and the user's own ruling.
							 *
							 * The depth is measured and the moment is not touched: composite
							 * action changes the section the moment is read against, never what
							 * the wall hands down, so the ladder below this joint carries
							 * exactly what it carried before and only this joint's utilisation
							 * moves. Scaling the moment instead would relieve every joint under
							 * this one too, by a factor nobody derived.
							 *
							 * A bed joint and a real moment is both the gate and the budget — a
							 * head joint has no masonry standing over it in this sense
							 * (MOMENTS_DESIGN case (b), which must not move), and a joint
							 * carrying no moment has nothing for a second section to resist, so
							 * an intact wall (every seat at e = 0) never reaches this line. And
							 * there has to be a stack: a piece with nothing resting on it is one
							 * unit, not a composite of anything — the topmost rung of every
							 * corbel reads its own bed patch for exactly this reason, since one
							 * course of depth is already shallower than the patch.
							 *
							 * The wall is not the only thing that caps the depth — the walk
							 * stops where the masonry stops and at any joint that has given, so
							 * a cut's bottom rung is bent by the cut but depthed by the wall:
							 * the same eleven-step cut read 0.223 under forty courses against
							 * 0.369 under thirteen, more load for less utilisation.
							 * COMPOSITE_DEPTH_DESIGN.md.
							 *
							 * So the second cap is the joint's own effective lever arm, e =
							 * |M|/|F|, a length the solver has already accumulated — no new
							 * field, no profile column, no per-material branch. For a k-step
							 * corbel e comes to about half the corbel's own depth, so lambda*e
							 * is roughly 1.73 times that depth, and the cap does not fire where
							 * a wall stops at the top of its corbel; it fires on the wall the
							 * game renders, the case no fixture had.
							 *
							 * The division is guarded in the direction that credits less: a
							 * force at or near zero makes the arm enormous or non-finite, and an
							 * infinite permitted depth is exactly the whole wall this exists to
							 * refuse. Tested positively so a NaN arm lands outside the relief
							 * and the joint keeps its own bed patch — reading intact when it
							 * should read failed is the expensive way to be wrong.
							 *
							 * But the arm may only trim the masonry above the cut, never the
							 * corbelling body itself, so the body's own depth is a floor under
							 * it. The corbelling courses generate the moment — bonded into one
							 * cantilevering body, needing no shear transfer to be engaged — so
							 * they resist with their full depth unconditionally. Masonry above
							 * the cut is different: it isn't bent by the corbel's moment and has
							 * to be dragged into the section by shear over a distance, which is
							 * what lambda*e bounds. Applying the arm to the whole depth taxed
							 * the corbel for its own height and made a shallower corbel read
							 * higher than a deeper one. And the floor can never credit a course
							 * above the cut, because the floor is the cut —
							 * CorbellingBodyDepthCm stops at the first non-corbelling course, an
							 * exact structural guarantee.
							 *
							 * Max is written as `greater than` so a NaN loses the floor:
							 * FMath::Max is `(B < A) ? A : B` (GenericPlatformMath.h), every
							 * comparison against NaN is false, so it discards a NaN first
							 * argument and would silently substitute a plausible number for a
							 * fault; spelled out, a body depth that isn't a number falls through
							 * to lambda*e, the higher of the two readings.
							 *
							 * The arm is both a limit on the walk and a cap on its answer.
							 * MasonryDepthAboveCm stops once it has enough, so it returns the
							 * first whole number of courses that reaches the limit and
							 * overshoots it by up to one — on the scenario corbel 202.5 cm
							 * against a permitted 200.77, 1.7% larger than the rule allows, in
							 * the permissive direction and invisible to a one-sided property.
							 * Slice 4 takes the same two steps with the arching angle. Written
							 * as `less than` rather than FMath::Min (slice 4's spelling too) for
							 * the same NaN reason: the walk itself cannot return a NaN, since
							 * its rise tests are `!(x > 0.0)` and unreadable geometry leaves it
							 * at zero, which withholds the relief — the direction to be wrong
							 * in.
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
					 * And it's handed on as the pair it is: a force through this joint, and
					 * exactly the moment stored on this joint, re-referenced to the receiving
					 * piece's own centre. What travels is what the joint reads — no second,
					 * private quantity — so a joint whose statics is indeterminate transmits a
					 * moment of zero about itself rather than nothing at all, and the load below
					 * it still knows it came through that patch, not through the middle of the
					 * piece it landed on.
					 *
					 * Both ends have to be placed: the joint supplies the point the moment is
					 * currently about, the support supplies the point it's being moved to; with
					 * either missing there's nothing to measure the transfer against, and
					 * inventing one would put the world origin into a lever arm.
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
				 * Every entry in a load path reaches the ground, so the only thing left to
				 * exclude here is the earth itself: a grounded piece absorbs what arrives and
				 * passes nothing on.
				 */
				if (--PendingLoaders[Support] == 0 && !Pieces[Support].bIsGrounded)
				{
					Ready.Add(Support);
				}
			}
		}

		/*
		 * Step five: strand the pieces actually caught in an unroutable knot; the next pass has
		 * to run without them, both because they must report as unsupported and because the
		 * shares they were allocated have to go somewhere real.
		 *
		 * A piece is in the knot when its own load comes back round to it, never merely because
		 * the ordering failed to reach it — those aren't the same set. Kahn runs top-down, so a
		 * piece is unordered whenever a knot sits anywhere above it. Stranding on
		 * un-orderability would therefore walk down to the first grounded piece and back up
		 * through everything resting on what it just stranded, reporting a brick bed-jointed to
		 * the earth as falling. DESIGN.md §3: a piece is unsupported only when it genuinely has
		 * no load path to the ground, and pieces beneath a knot keep their support and carry
		 * everything except the unroutable contribution.
		 *
		 * Stranding still travels upward, and that's the fixpoint's job rather than this loop's:
		 * a piece whose only support has just been stranded isn't reached by the next pass's
		 * walk from the ground, so it comes out unsupported there.
		 *
		 * A stranded piece is never marked supported again — the walk neither marks nor crosses
		 * one — so no piece can be stranded twice and every pass that doesn't break has shrunk
		 * the problem.
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
				 * A cycle on a refused-arch member is a fall, not a knot. The piece is only in
				 * this head-joint cycle because its one-sided arch was refused; that refusal was
				 * the loss of its only real load path, so it's released to Falling rather than
				 * reported Stranded. A cycle on any other piece is a genuine solver knot and
				 * stays Stranded — which is what keeps the geometry-free mutually-propping pair
				 * (no group ever forms, so it's not a refused-arch member) reading Stranded.
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
	 * Step six: an arch pushes sideways, and the springing has to carry that too.
	 *
	 * Outside the fixpoint because it reads its answer. The thrust is a fraction of the load the
	 * abutments' own seats have already been given, so it can only be computed once the
	 * accumulation has settled — and it feeds nothing back, since a horizontal force changes no
	 * support list, no split, no accumulation order and no moment. That's what lets the whole
	 * vertical answer stay bit-identical to one computed before arches existed.
	 */
	const double ArchingStartSeconds = FPlatformTime::Seconds();

	ApplyArchingThrust(PieceJoints, Arches);

	const double ProfileEndSeconds = FPlatformTime::Seconds();

	LastSolveLoadsProfile.FixpointMs = (ArchingStartSeconds - FixpointStartSeconds) * 1000.0;
	LastSolveLoadsProfile.ArchingMs = (ProfileEndSeconds - ArchingStartSeconds) * 1000.0;
	LastSolveLoadsProfile.TotalMs = (ProfileEndSeconds - ProfileStartSeconds) * 1000.0;

	/*
	 * Nothing is evaluated against a strength here. Solving computes what each joint carries and
	 * must leave every connection exactly as intact as it found it: FConnection::ApplyForce
	 * latches, so calling it would break joints as a side effect of asking what they carry and
	 * make a solve unrepeatable.
	 */
}

bool FStructure::PieceOverturnsOffItsSupports(int32 PieceIndex, const TArray<int32>& LoadPath) const
{
	const FStructurePiece& Piece = Pieces[PieceIndex];

	/*
	 * No centre of mass, no point to project — fail closed and keep today's stand. A piece
	 * nobody placed carries no overturning either; its joints already answer a centred load
	 * exactly, so it must read exactly as it did before this gate existed.
	 */
	if (!Piece.bHasCentreOfMass)
	{
		return false;
	}

	/*
	 * The tension clause, asked first and off the strength data itself. Any support in the load
	 * path whose strength can carry tension holds the lifting side down in withdrawal, so the
	 * body has an admissible equilibrium however far its centre of mass reaches past the
	 * compression bearings, and must not be overturned. This is what spares the porch overhang,
	 * tied back by a Screw, and the tension-tied board of the anti-regression fixture. Reading
	 * TensileStrengthMPa directly keeps the distinction in the material data: a new
	 * tension-capable connection type spares its bodies with no change here.
	 */
	for (const int32 Index : LoadPath)
	{
		if (Connections[Index].Strength.TensileStrengthMPa > 0.0)
		{
			return false;
		}
	}

	/*
	 * All supports are compression-only: the body stands only while its centre of mass projects
	 * onto the region its bearings can push up through. That region is the convex hull of the
	 * contact rectangles on the bed plane, and this tests the axis-aligned bounding box of those
	 * rectangles instead — a superset of the hull, so a centre of mass outside the box is
	 * outside the hull for certain. Felling on "outside the box" is therefore conservative in
	 * the direction that matters: it can only fail to fell a body genuinely outside a
	 * non-rectangular hull, never fell one genuinely inside. That keeps the anchor-safety of
	 * symmetric running bond exact — its centre of mass sits at the area-weighted centroid of
	 * its seats, inside the box, so the box test never fires — at the cost of under-felling some
	 * diagonal support arrangements no fixture exhibits (the co-linear ridge is the opposite
	 * case). A sound approximation of the hull test, not the hull.
	 *
	 * The projection is onto the horizontal bed plane — the X and Y of each face — since gravity
	 * is vertical and the bearings are horizontal seats; the height the centre of mass sits at
	 * can't change which seats reach under it.
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
		 * A joint that didn't measure its face has no rectangle, so the box is undefined and the
		 * body keeps today's reading — the same words HasCompleteGeometry uses for an unmeasured
		 * face, and the fail-closed rule that a body with no support geometry must not
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
	 * Outside the box, written as four positive comparisons. A negated conjunction, or
	 * FMath::Min/Max against the centre of mass, would each let a non-finite coordinate through
	 * as an overturn, since every comparison against NaN is false and `!inside` would read true.
	 * Four `<`/`>` tests, each false against a NaN, land such a piece on "inside" and keep its
	 * stand — the direction a piece that should stand must never be felled in.
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
	 * Sized before the gate, so every caller downstream may index it without asking whether this
	 * pass ran. An all-false array is exactly what "no group formed" means — the same answer a
	 * structure nobody placed gets. PieceInRefusedArchGroup is sized here too and stays
	 * all-false unless a group forms and its opposition gate refuses it, so a geometry-free
	 * structure (no group ever forms) leaves it empty, keeping a genuine geometry-free
	 * cycle-strand out of this reclassification.
	 */
	PieceReseatedOnAnArch.Init(false, Pieces.Num());
	PieceInRefusedArchGroup.Init(false, Pieces.Num());
	Arches.Reset();

	/*
	 * The geometry gate, load-bearing rather than defensive. Deciding that a run of bricks over
	 * a hole is an arch rather than a chain of hangers is a routing decision, and it can't be
	 * made without knowing where the hole is — which is why ARCHING_DESIGN revises
	 * MOMENTS_DESIGN's discipline line here rather than quietly bending it. With no positions
	 * there's no group, so the whole pass is a no-op and a geometry-free structure routes bit
	 * for bit as it always did.
	 *
	 * Both fuzz generators emit no geometry — 20,000 cases between them, the only property tests
	 * over routing this project has. An arch that could fire without positions would set every
	 * one of them against an oracle that has never heard of one, and they'd go dark quietly
	 * rather than failing.
	 */
	if (!HasCompleteGeometry())
	{
		return;
	}

	TArray<bool> Grouped;
	Grouped.Init(false, Pieces.Num());

	/*
	 * Reused across every group rather than allocated per group: hops from the nearest abutment,
	 * INDEX_NONE for a piece this pass has not reached. Only entries belonging to the group
	 * being worked are ever read, and each group writes its own before reading them.
	 */
	TArray<int32> HopsFromAbutment;
	HopsFromAbutment.Init(INDEX_NONE, Pieces.Num());

	/*
	 * What "contiguous" means, written once and asked four times: the piece across an intact
	 * head joint from this one, or INDEX_NONE where this joint is not one.
	 *
	 * Head joints and nothing else. A bed joint to a seatless piece is the tier that has already
	 * failed to hold anybody up, and following one would fuse the courses above and below a hole
	 * into a single group spanning nothing. A joint that has given conducts nothing at all, so
	 * it can't make two pieces one group either — GetJointRole keeps answering for a severed
	 * joint deliberately, so that has to be asked here.
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
		 * member touching one is a hop from the ground and seeds the walk inward.
		 *
		 * Each abutment once, however many members touch it. The direction test below is unmoved
		 * by a duplicate — two copies of one vector can only agree with each other — but the
		 * thrust divides itself among the abutments at each end, and an abutment counted twice
		 * would take twice its share.
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
		 * And the group only spans if something seated stands on both sides of it. One abutment
		 * is a cantilever however many bricks long it is, and granting it would hang a wall's
		 * whole free end off a joint with nothing to thrust into — the permissive failure
		 * ARCHING_DESIGN names, and why a wall's free vertical end keeps today's answer.
		 *
		 * Opposite sides is a negative dot product about the group's own centre, which needs no
		 * axis nominated and so says the same thing for a wall laid along X, along Y or at forty
		 * degrees to both. Written as a positive test, so a NaN anywhere in either direction
		 * leaves the group unabutted rather than spanning — a hole that quietly stops being a
		 * hole is the expensive way to be wrong here.
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
			 * The gate refuses a one-sided cantilever, correctly — but the run it declines is
			 * now left on its sign-blind head joints, where each middle member lists both
			 * neighbours as "supports", the run becomes a mutual-support chain and
			 * LoadReturnsToPiece strands it. That Stranded is a solver artefact: a refused arch
			 * was the run's only hope of a load path, so with it declined the honest answer is
			 * Falling. Record every member of this refused group so SolveLoads can exclude them
			 * from the reachability walk exactly as it excludes an overturned piece — they never
			 * reach the stranding branch and fall through to Falling. Gated strictly on group
			 * membership, which only forms under HasCompleteGeometry(), so a geometry-free knot
			 * is never touched.
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
		 * The re-seat is acyclic by construction. A member keeps only the head joints that take
		 * it strictly closer to an abutment, so every remaining edge runs from a longer path to
		 * a shorter one and no walk can return to where it started — what separates this from
		 * the naive arch ARCHING_DESIGN's trap 1 describes: making the neighbour a support
		 * outright puts two bricks over a hole in a two-node cycle, LoadReturnsToPiece strands
		 * the pair, and the wall comes down for a new reason.
		 *
		 * Ascending joint index survives, since PieceJoints is ascending and this filters it in
		 * place rather than sorting anything. The whole accumulation downstream is a
		 * floating-point sum whose last bit decides breaks, and its order is the order of these
		 * lists.
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
		 * And the opening is recorded as an arch, with its two ends told apart. What the thrust
		 * pass needs — and this loop is the only place that knows — is which abutments face each
		 * other across the hole: H is one number for the whole span, pushed out at both ends at
		 * once, and a pass that couldn't tell the ends apart would have nothing to make equal
		 * and opposite.
		 *
		 * The first abutment's own direction is the axis, nominating no world axis, so it says
		 * the same thing for a wall laid along X, along Y or at forty degrees to both. Sides
		 * fall out as the sign of a projection onto it, and an abutment square on to that axis
		 * is on neither side and is dropped — written as two positive tests so a NaN leaves it
		 * out rather than assigning it to whichever branch happens to be the else.
		 *
		 * A direction that won't normalise means an abutment sitting exactly on the group's own
		 * centre, which describes no span, so there's no arch here to thrust — the re-seat above
		 * stands either way, slice 2's answer, unaffected.
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
		 * Both ends or neither. bAbutsOnBothSides above already found a pair of abutments in
		 * opposition, so this can't be false — asked because the thrust pass may not be the
		 * thing that discovers a one-ended arch, trap 2 wearing the clothes of a refactor.
		 */
		if (Arch.Abutments[0].Num() > 0 && Arch.Abutments[1].Num() > 0)
		{
			/*
			 * And L is how far the two ends stand apart, one mean abutment centre per end. Slice
			 * 3 never needed it because `d_e = 0.866*L` cancelled the span out of the thrust
			 * ratio entirely; capping `d_e` by the cover puts it back, and the abutments' own
			 * positions give it with no new query — for a running-bond wall each springing keeps
			 * half a cell of bearing, so the two centres are the clear opening apart to the
			 * centimetre.
			 */
			EndCentreCm[0] /= static_cast<double>(Arch.Abutments[0].Num());
			EndCentreCm[1] /= static_cast<double>(Arch.Abutments[1].Num());

			Arch.SpanCm = (EndCentreCm[0] - EndCentreCm[1]).Size();

			/*
			 * The thrust axis is the abutment-to-abutment line, not the first abutment's own
			 * direction. TowardAbutmentCm[0] earned its keep above as the classification axis
			 * that split the abutments into two ends by the sign of a projection — a job that
			 * only needs an axis those abutments genuinely straddle, run before the end centres
			 * exist. But it's the wrong axis to push along: when a group re-seats onto
			 * perpendicular (corner) walls the first abutment sits diagonally inboard, so that
			 * raw vector carries an out-of-plane component and the thrust pass would shove each
			 * springing sideways out of the wall plane — a force a flat arch's thrust line
			 * cannot produce.
			 *
			 * The true thrust axis is the line from one end's mean abutment centre to the
			 * other's: the arch springs between the two abutment groups, and whatever
			 * out-of-plane offset each end carries is shared by both, so it cancels in their
			 * difference. EndCentreCm[0] is the +TowardEndZero side, so EndCentreCm[0] -
			 * EndCentreCm[1] points toward end 0 and keeps the sign convention the thrust pass
			 * expects. Zeroing Z projects it onto the horizontal seat plane the thrust acts in.
			 *
			 * For a planar wall both abutments share an out-of-plane coordinate and a course, so
			 * this difference is already axis-aligned along the wall run and normalises to the
			 * same (+/-1, 0, 0) the raw first-abutment vector did — the in-plane arches are
			 * unmoved.
			 *
			 * A difference that won't normalise means the two ends coincide horizontally,
			 * describing no span, so there's no arch here to thrust — the re-seat above stands
			 * either way, the same fail-closed answer the classification-axis guard gives at the
			 * top.
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
		 * The seats the arch delivers itself through, and what they already carry. The thrust
		 * arrives at the abutment and leaves through the same patch its weight does, which is
		 * why the springing plane is the critical one: demand is constant with depth while the
		 * friction resisting it grows with the weight above.
		 *
		 * The sign of each seat is recorded here rather than re-derived below. ConnectionLoad's
		 * convention is that a joint's force acts on PieceB, so a joint naming the abutment
		 * second stores the push as given and one naming it first stores the equal-and-opposite
		 * reaction. Get it backwards and the two ends of an arch pull together instead of
		 * pushing apart — a perfectly plausible-looking wall.
		 */
		TArray<int32> Seats[2];
		TArray<double> SeatSign[2];
		double SeatAreaSqCm[2] = { 0.0, 0.0 };

		/*
		 * How deep the arch may be if only the angle had a say, and also how far up the cover
		 * walk below has to bother looking: past this much masonry the angle governs and the
		 * exact cover changes no answer.
		 */
		const double AngleCappedDepthCm = SolverArchingDepthPerSpan * Arch.SpanCm;

		/*
		 * The thinnest cover either end stands under, and one number for the whole arch.
		 *
		 * A cover measured per abutment and applied per abutment is trap 2 wearing a new hat:
		 * the two ends of one opening would disagree about d_e, push each other by different
		 * amounts, and hand the structure a net horizontal force out of nowhere while every
		 * joint still read plausibly. Reducing the two measurements to one before anything is
		 * pushed makes the equal-and-opposite property structural rather than lucky.
		 *
		 * The thinnest rather than the mean, because thin cover is the direction that fails — an
		 * arch is only as good as its shallower haunch, and taking the deeper one would be the
		 * permissive reading of exactly the defect this slice exists to fix.
		 */
		double CoverCm = TNumericLimits<double>::Max();

		/*
		 * W is the whole load the arch puts on its abutments, springings' own columns included,
		 * and ARCHING_DESIGN is explicit that it isn't a triangle. Taking the re-seated group's
		 * load alone would leave out the two bricks the thrust is actually delivered through and
		 * under-report the thrust by roughly a cell's worth.
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
					 * A joint that has given conducts nothing, so it takes no thrust either —
					 * the same rule the tier decision applies. GetJointRole keeps answering for
					 * a severed joint deliberately, so it has to be asked here.
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
				 * Written as `not at least as deep` rather than as a `Min`, so a cover that
				 * comes back NaN is taken rather than discarded. Every comparison against NaN is
				 * false, so FMath::Min would quietly keep the running answer and the arch would
				 * end up credited with the good end's depth; this way the NaN reaches the guard
				 * below and the arch is left unthrust instead.
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
		 * Both ends have to be able to take it, or neither is pushed. An end whose abutments are
		 * grounded — resting on the earth rather than a bed joint — has no seat here to deliver
		 * into, and thrusting only the other end would give the structure a net horizontal force
		 * out of nowhere while every joint still read plausibly. ARCHING_DESIGN's trap 2, and
		 * the one thing no per-joint check could catch.
		 *
		 * Every guard is written as a positive test, so a NaN area or load leaves the arch
		 * unthrust rather than laundering into a plausible-looking sideways force.
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
		 * A span and a cover that mean nothing leave the arch unthrust, the same answer a
		 * degenerate area or load already gets above. Both guards are positive tests, so a NaN
		 * lands inside them; neither is reachable from a wall anyone laid, since an abutted
		 * group has two abutments a real distance apart and the spanning course is itself a
		 * course of cover.
		 */
		if (!(Arch.SpanCm > 0.0) || !FMath::IsFinite(Arch.SpanCm)
			|| !(CoverCm > 0.0) || !FMath::IsFinite(CoverCm))
		{
			continue;
		}

		/*
		 * The arching depth, a `min` rather than a replacement. BS 5977-1's angle says how deep
		 * an arch may be; the masonry actually standing over the opening says how deep it can
		 * be. Whichever is smaller is what there is to work with, so a deeply buried opening is
		 * governed by the angle and reads the same however much more wall is piled on it, while
		 * a shallow one is governed by what little it has.
		 *
		 * Held as d_e/L rather than d_e, which is arithmetic, not tidiness. The thrust only ever
		 * depends on the ratio — H = 3*W*L/(8*d_e) is 3*W/(8*(d_e/L)) — and where the angle
		 * governs, d_e/L is the constant itself, so that expression is character for character
		 * the one slice 3 shipped and every answer slice 3 pinned is bit-identical. Dividing
		 * 0.866*L back out of L instead loses the cancellation: IEEE multiplication isn't exact,
		 * so L/(0.866*L) is 0.866 only to within a rounding, and the dry-stone springing moves
		 * in its last three digits for no reason anybody chose.
		 *
		 * Written out rather than as FMath::Min because the comparison works against us: Min is
		 * `(A < B) ? A : B` (GenericPlatformMath.h), and every comparison against NaN is false,
		 * so a NaN cover in the first argument would be silently replaced by the angle's answer
		 * — the permissive direction, the exact defect this slice exists to remove. The guard
		 * above has already refused a NaN; this is written so it wouldn't matter if it hadn't.
		 */
		const double DepthPerSpan = CoverCm < AngleCappedDepthCm
			? CoverCm / Arch.SpanCm
			: SolverArchingDepthPerSpan;

		if (!(DepthPerSpan > 0.0))
		{
			continue;
		}

		/*
		 * H = W*L/(8r) with r = d_e/3, so H = 3*W/(8*(d_e/L)). The kern-limited rise is a fixed
		 * fraction of the depth rather than the span, which is what makes the cover matter at
		 * all: H climbs as 1/d_e while V doesn't move, so the thrust ratio H/V = 3L/(4*d_e)
		 * blows up as the masonry over an opening thins. Where the angle governs — a narrow hole
		 * under deep cover — d_e/L is 0.866, the span drops out of the ratio again and it
		 * settles at the constant 3/(4*0.866) = 0.866 slice 3 measured everywhere.
		 *
		 * One number for the whole arch, pushed out at both ends at once, which is what makes
		 * trap 2 exact rather than nearly exact.
		 */
		const double ThrustUu = 3.0 * TotalVerticalUu / (8.0 * DepthPerSpan);

		for (int32 End = 0; End < 2; ++End)
		{
			/*
			 * One direction, two signs, so the two ends sum to exactly zero rather than to a
			 * rounding of it: +H*D and -H*D cancel bit for bit on every component.
			 */
			const FVector EndThrustUu = (End == 0 ? ThrustUu : -ThrustUu) * Arch.TowardEndZero;

			for (int32 Which = 0; Which < Seats[End].Num(); ++Which)
			{
				const int32 Index = Seats[End][Which];

				/*
				 * Divided among an end's seats by interface area, which is the same rule the
				 * load split already uses — and with one seat, which is what a half-seated
				 * springing has, it is the whole of it exactly.
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
	 * What stands on this piece is whatever the graph says rests on it — BedAbove is that
	 * relation exactly — so this costs a walk over the six or so joints a brick has and needs no
	 * broadphase, no octree, no world. A joint that has given conducts nothing and holds nothing
	 * up, so it isn't a course of masonry either; GetJointRole keeps answering for a severed
	 * joint deliberately, so it has to be asked.
	 *
	 * The first by ascending joint index, a chain and not a traversal. Running bond puts two
	 * pieces over each brick and in a wall of uniform height both columns reach the same place;
	 * a stepped or gabled wall would have its answer decided by which one this took, and nothing
	 * tests that yet.
	 *
	 * Two callers, and the second is why this is a function. MasonryDepthAboveCm takes this step
	 * once per course, and SolveLoads asks it once to find out whether there's a stack over a
	 * joint at all — a piece with nothing on it is one unit rather than a composite, and one
	 * unit is not a deep beam. Two transcriptions of "what rests on this" would agree until the
	 * day a severed joint was handled in only one of them.
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
	 * The course the joint is under counts, and is the first of them rather than something
	 * resting on one — so the shallowest depth a wall can offer is one course and never zero.
	 * For the arch that's the spanning course, the first ring, so an opening cut in the top
	 * course of a wall still has a ring to arch in; for the composite section it's the corbelled
	 * brick itself, the only masonry over its own seat.
	 *
	 * Its depth is a course pitch, not a brick height. What the wall works through is the
	 * masonry from one bed plane to the next, mortar included, exactly the rise from the piece
	 * below the joint to the piece standing on it. Taking the unit's own height instead drops
	 * the joints and reads about 13% shallow on standard brickwork — plausible enough to survive
	 * a review and wrong on every row.
	 */
	const double FirstCourseRiseCm =
		Pieces[Piece].CentreOfMassCm.Z - Pieces[Seat].CentreOfMassCm.Z;

	if (!(FirstCourseRiseCm > 0.0))
	{
		return 0.0;
	}

	/*
	 * And the walk is bounded twice over. Past EnoughDepthCm no further course can change the
	 * caller's answer — 0.866*L for the arch, where the angle takes over, and lambda*|M|/|F| for
	 * the composite section, where the deep beam stops reaching. Both are at most
	 * ceil(EnoughDepthCm / course pitch) steps, so neither caller walks a whole wall it won't
	 * use: about five courses for a free end and about twenty-seven for the scenario corbel. The
	 * piece count is the second bound and is pure defence — a graph whose normals claim A is
	 * above B and B above A would otherwise walk forever, and a structure with complete geometry
	 * is the only thing that reaches here.
	 *
	 * Compared as a double, so a vanishing course pitch produces an enormous bound rather than
	 * an integer conversion nobody defined.
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
		 * Measured rise by rise rather than counted and multiplied, so courses of unequal depth
		 * add up to what they are instead of a multiple of the first one. A step that doesn't
		 * rise isn't a course, and stopping on it is the fail-closed direction: less cover is
		 * more thrust, and a joint reading intact when it should read failed is the expensive
		 * way to be wrong here.
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
	 * The body's first course is the piece standing on the joint, and it's not asked whether
	 * it's corbelling — it's the course whose overhang generated the moment being read, the cut
	 * by construction; the walk above it is what has to be justified. Measured as a course pitch
	 * from the seat rather than a unit height, for the reason MasonryDepthAboveCm gives: the
	 * wall works through the mortar as well as the brick.
	 */
	const double FirstCourseRiseCm =
		Pieces[Piece].CentreOfMassCm.Z - Pieces[Seat].CentreOfMassCm.Z;

	if (!(FirstCourseRiseCm > 0.0))
	{
		return 0.0;
	}

	/*
	 * Seated on exactly one course. Counted over the piece's own joints, excluding anything that
	 * has given and anything resting on a piece that has left the structure — the same two
	 * exclusions PieceRestingOn makes, for the same reason: neither is masonry the body can be
	 * bonded into.
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
	 * The piece count is the bound, and it's pure defence. Every step rises by a positive amount
	 * and no piece can be visited twice on a strictly rising chain, so a graph whose normals are
	 * consistent stops of its own accord; this is here so one whose normals aren't cannot walk
	 * forever.
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
		 * Measured rise by rise, and a step that doesn't rise isn't a course. Written !(x > 0.0)
		 * so geometry nobody measured leaves the body at the depth it has already earned rather
		 * than adding a NaN to it — a shallower body credits less section, the direction to be
		 * wrong in.
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
	 * The seat's own plane is what the sides are measured in, so a normal that won't normalise
	 * has no sides and can't abut anything. Nothing that reaches here can fail this — the caller
	 * has already had a tier and a moment out of the same joint — and it's here so the
	 * projections below are never fed a direction nobody chose.
	 */
	FVector UnitNormal = BedJoint.InterfaceNormal;

	if (!UnitNormal.Normalize())
	{
		return false;
	}

	/*
	 * Which way the piece overhangs: where its centre of mass sits relative to the centroid of
	 * the patch it has left, flattened into that patch's plane. On a bed joint the plane is
	 * horizontal and this is the 5.625 cm a half-seated running-bond brick leans by; the
	 * projection keeps the two courses' worth of height between the two points out of a
	 * comparison that's only ever about sideways.
	 */
	const FVector EccentricCm = FVector::VectorPlaneProject(
		Pieces[PieceIndex].CentreOfMassCm - BedJoint.InterfaceCentreCm, UnitNormal);

	/*
	 * How hard this seat may be pushed sideways, MPa, bought with the seat's own squeeze. The
	 * couple the cap deletes leaves through this patch as shear, so what limits it is the
	 * Mohr-Coulomb envelope ComputeUtilisation already measures every other sliding demand
	 * against — the bond, plus whatever friction the mean compressive stress is worth. Read off
	 * the force rather than the profile's name: a joint with no cohesion earns exactly what its
	 * friction earns, which is what makes dry stone unable to flat-arch without one line
	 * anywhere saying so.
	 *
	 * One conversion, the named one. The force is uu and the strengths are MPa, so the area
	 * carries the 10000 exactly once and the comparison below is stress against stress.
	 */
	const FConnectionLoad SeatLoad = DestructionForce::ClassifyForce(SeatForceUu, UnitNormal);

	const double SeatCompressionMPa = SeatLoad.Compression
		/ (BedJoint.InterfaceAreaSqCm * DestructionForce::ForceUnitsPerMPaSqCm);

	const double CohesionAndFrictionMPa = BedJoint.Strength.ShearCohesionMPa
		+ BedJoint.Strength.FrictionCoefficient * SeatCompressionMPa;

	/*
	 * The truncation is written out rather than as FMath::Min, and the order of the comparison
	 * is the whole reason. Min is `(A <= B) ? A : B` (GenericPlatformMath.h) and every
	 * comparison against NaN is false, so a NaN capacity handed to it as the first argument
	 * would be silently replaced by the profile's ceiling — which for an unset ceiling is
	 * TNumericLimits<double>::Max(), an arch afforded by arithmetic nobody can read. Asking
	 * whether the ceiling is the smaller one keeps a NaN, refusing the relief at the guard
	 * below. Unreachable today, since the caller has already had a finite normal stress out of
	 * this same joint; spelled so it wouldn't matter if it hadn't.
	 */
	const double SlidingCapacityMPa =
		BedJoint.Strength.MaxShearStrengthMPa < CohesionAndFrictionMPa
			? BedJoint.Strength.MaxShearStrengthMPa
			: CohesionAndFrictionMPa;

	for (const int32 Index : PieceJoints[PieceIndex])
	{
		const FConnection& Head = Connections[Index];

		/*
		 * A joint that has given conducts nothing, so it can't deliver a thrust either — the
		 * same rule the tier decision applies, for the same reason. GetJointRole still answers
		 * for a severed joint deliberately, so this has to be asked here.
		 */
		if (Head.HasGiven() || GetJointRole(Index, PieceIndex) != EJointRole::Head)
		{
			continue;
		}

		/*
		 * And the head joint has to know where it is. Its centroid says which side it's on, and
		 * an unmeasured face carries a zero meaning "nobody said" rather than a plane through
		 * the world origin — which on a wall laid anywhere else would answer the side question
		 * with the direction of the origin.
		 */
		if (Head.InterfaceHalfExtentCm.IsZero())
		{
			continue;
		}

		const FVector TowardAbutmentCm = FVector::VectorPlaneProject(
			Head.InterfaceCentreCm - Pieces[PieceIndex].CentreOfMassCm, UnitNormal);

		/*
		 * On the eccentric side, and a joint square on to the overhang is on neither side.
		 * Written as a positive test so an exact zero — and a NaN — falls out here rather than
		 * being counted as an abutment.
		 */
		if (!(FVector::DotProduct(EccentricCm, TowardAbutmentCm) > 0.0))
		{
			continue;
		}

		const int32 Abutment = OtherEndOf(Head, PieceIndex);

		/*
		 * It has to reach the ground on its own account, and both halves of that are needed.
		 * PieceSupported is the walk from the earth, so it covers Grounded and Supported
		 * together and excludes anything falling or stranded; the second test is trap 3, telling
		 * a real arch apart from two bricks propping each other over open air — both are
		 * Supported and both have an intact head joint to the other, so only the support
		 * relation separates them.
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
		 * And a neighbour re-seated onto a spanning group leans on us only because something
		 * beyond it is carrying. That mark is written by ReseatSpannedGroups and by nothing
		 * else, only for a group with a seated abutment on both sides — so where it's set the
		 * thrust line runs on through the group to a reaction rather than stopping in mid-air,
		 * the one fact separating a spanned opening from the two bricks propping each other the
		 * test above exists to refuse. Without it this reads exactly as it did before groups
		 * existed.
		 */
		if (bAbutmentLeansOnUs && !PieceReseatedOnAnArch[Abutment])
		{
			continue;
		}

		/*
		 * A spanned group is checked by being pushed, so it's not checked here. Where that mark
		 * is set, ApplyArchingThrust puts the real horizontal force on both springings once the
		 * accumulation settles, and the joint's own shear axis measures it like any other
		 * demand. Judging the same thrust a second time — and by a different rule, since that
		 * pass reads H off the span and the cover while this reads it off the deleted couple —
		 * would answer one question twice. The ONE-CELL hole is the case with no thrust pass of
		 * its own.
		 */
		if (PieceReseatedOnAnArch[Abutment])
		{
			return true;
		}

		/*
		 * So the relief has to be earned, and what earns it is the seat's own sliding capacity.
		 *
		 * Moving the thrust line in to the kern edge deletes a couple of `(1 - k)*|M|` from what
		 * this joint carries, and on the free body of a half-seated brick nothing can supply it
		 * except a horizontal pair — a push out through this head joint, and its equal and
		 * opposite reaction as shear in the bed plane below. The arm between the two is measured
		 * rather than assumed: the head joint's own centroid above the seat's own centroid,
		 * taken along the seat's normal so a tilted joint is measured in its own frame instead
		 * of Z. For a standard brick and a 1 cm joint that's 3.75 cm, putting the demand at `(e
		 * - h/6)/z = 1.0444` of the reaction — a fact about the bond geometry alone, load
		 * cancelled out of it, exactly as the spanned case's `3L/(4*d_e)` is.
		 *
		 * Withheld rather than applied, a deliberate choice. Pushing the thrust in as a real
		 * shear force is the other honest answer and belongs with a re-anchoring pass: it moves
		 * every one-cell arch in the project by twenty-odd times on an axis that was reading
		 * zero. Withholding leaves an earned arch bit-identical to what it read before this
		 * existed, and leaves an unearned one reading what it's actually carrying — for a
		 * cohesionless joint outside its kern, a tension it has no strength for at all.
		 *
		 * `!(demand <= capacity)` and not `demand > capacity`. Every comparison against NaN is
		 * false, so the negated form lands a degenerate arm, area or unreadable capacity inside
		 * the refusal and the relief is withheld — the expensive direction to be wrong in.
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
	 * Invalidate the readout per pass, not just per call. Only a pass that reaches an answering
	 * arm (Stands or a certified Falls) refills the cache through CacheMinViolationReadout;
	 * every decline return below (over cap, no geometry, an Unanswerable LP, an uncertified
	 * mechanism) leaves without refilling. Clearing here means a pass that declines can't leave
	 * a prior pass's below-cap reading behind, so an answered-then-declined cascade — which no
	 * owned fixture reaches today but which becomes player-visible now InspectPiece serves the
	 * readout — can't serve a stale number. The per-call Reset in SolveAndBreak stays as the
	 * coarser guard; this per-pass clear subsumes it for the within-call window.
	 */
	ConnectionReadoutCache.Reset();

	/*
	 * Scope by size first — the fail-closed boundary that keeps synchronous LP authority off the
	 * flagship scenarios (PROMOTION_DESIGN.md §12 D6-c). Above the block cap the gate declines
	 * and does nothing, so behaviour falls through to the per-joint capacity sweep (the router),
	 * which then both breaks and enumerates support exactly as production did before the gate.
	 * The cap is compared against the live block count, injectable through
	 * SetEquilibriumGateBlockCap for the scope-by-size test.
	 *
	 * Written as a positive test on the decline side (> cap), never NumPieces() <= cap, so a
	 * degenerate cap lands on the fail-closed branch rather than slipping past.
	 */
	if (NumPieces() > EquilibriumGateBlockCap)
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * A no-op without complete geometry, for the same load-bearing reason the bridge itself
	 * refuses one: the LP reasons about moments of weights at centroids against bearing
	 * rectangles, and with either missing there's nothing to take moments of. Both fuzz
	 * generators emit no geometry, so they're provably untouched and stay on the router.
	 */
	if (!HasCompleteGeometry())
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * Ask the rigid-block LP whether the whole structure has an admissible equilibrium. The pose
	 * is feasibility at lambda = 1 (bGravityIsLive = false, PROMOTION_DESIGN §12 D6-b): it
	 * returns the identical Stands/Falls boolean as lambda* but is 5-16x cheaper, and on the
	 * infeasible arm its phase-1 dual is extracted and Farkas-verified as the collapse mechanism
	 * (Slice 3a) — the named set of blocks that move and joints that open or slide.
	 *
	 * A refusal fails closed to the router. The bridge declining (an out-of-plane joint, a
	 * tombstone), the solver refusing or running over budget, all arrive as Unanswerable, and
	 * every one of them declines here rather than inventing a break — or a support answer — out
	 * of an answer the LP could not give.
	 */
	RigidBlockOracle::FOracleProblem Problem;
	FString WhyNot;

	if (!RigidBlockOracle::BuildRigidBlockProblem(*this, Problem, WhyNot))
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * Record which physics this pose was built in the moment it exists — 2 for the X-Z problem,
	 * 3 for the volumetric one. The bridge picks the cheapest sound pose, so a 3D-flagged build
	 * whose posed joints are all in-plane and whose posed rows share one Y reads 2 here (a roof
	 * bearing on walls at two different Y reads 3 even with every normal in-plane) — the whole
	 * observable GetLastEquilibriumProblemDim exists to serve. Stamped on the pose, not the
	 * call: a pass that declines before the bridge (over cap, no geometry) or after it (an
	 * Unanswerable LP) leaves the previous reading in place.
	 */
	LastEquilibriumProblemDim = Problem.Dim == RigidBlockOracle::EOracleDim::Dim3D ? 3 : 2;

	Problem.bGravityIsLive = false;

	/*
	 * First-crack promotion. Below the cap the break authority now writes the uncracked
	 * peak-fibre bending rows for every bonded joint (f_t > 0), so a bonded joint carrying
	 * tension-in-bending cracks at its elastic limit — 3x stricter than the plastic no-tension
	 * form — rather than only at the plastic margin. Dry (f_t = 0) joints write no such row and
	 * are bit-identical. The flag reaches only this below-cap production pose; the flagship
	 * router, the geometry-free fuzzes, and the oracle sweeps (which set it themselves) are
	 * untouched.
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
		 * The LP stands the whole structure, so every piece it carries is genuinely held — even
		 * a piece the router could only strand for want of a rule to divide load round a knot or
		 * across an unabutted opening (rows 10 and 19). The mechanism is empty, so nothing
		 * moves; writing the LP support (all bridged pieces Supported/Grounded) is the sole
		 * effect, and it's what makes GetPieceSupport LP-authoritative below the cap. Nothing
		 * breaks — this is not a whole-structure release veto but the absence of any mechanism
		 * to release.
		 */
		ApplyLimitAnalysisSupport(Problem, Result);
		CacheMinViolationReadout(Problem);
		return EEquilibriumGateDisposition::AuthoritativeNoBreak;
	}

	/*
	 * The structure has no admissible equilibrium, and the mechanism names the loss. On this arm
	 * SolveRigidBlock either Farkas-verified the certificate or refused (Unanswerable, handled
	 * above), so a Falls here carries a certified mechanism; the guard below is fail-closed
	 * defence, declining to the router rather than acting on an uncertified set.
	 */
	const RigidBlockOracle::FOracleMechanism& Mechanism = Result.Mechanism;

	if (!Mechanism.bPresent || !Mechanism.bIsCertified)
	{
		return EEquilibriumGateDisposition::DeclinedToRouter;
	}

	/*
	 * The mechanism is the sole break authority (PROMOTION_DESIGN.md §12 D7's 3b section): a
	 * piece is released iff the mechanism moves it, a joint severed iff it opens or slides.
	 * Write the LP support first (moved pieces Falling, the rest Supported/Grounded), then sever
	 * the intact joints the mechanism opens, mapped back to production connections through the
	 * bridge's ConnectionOfJoint provenance. Index-ordered and single-threaded, so the choice is
	 * deterministic. Sever is only the mechanism of latching without a second utilisation
	 * evaluation, stamped with this pass exactly as an over-capacity joint is.
	 *
	 * A joint already gone is skipped, and this is what makes the outer cascade terminate: once
	 * a body's joints are all severed the bridge no longer carries them (a given joint is out of
	 * the LP), so the free body still falls the LP but its mechanism names no intact joint left
	 * to sever — the pass breaks nothing and the loop below ends.
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
	 * Cache the readout once, on the settled pass — the same last-wins answer, computed once. A
	 * pass that severs anything continues the cascade (AuthoritativeBroke below), and the next
	 * pass re-settles on the severed assembly and recomputes the readout, so this pass's readout
	 * would only be overwritten. Guarding on !bSeveredThisPass runs the separate min-violation
	 * LP solely on the terminal non-breaking pass, whose result is precisely the one today's
	 * last-wins already kept — mirroring the per-pass ConnectionReadoutCache.Reset() reasoning
	 * above: only the pass that actually answers the settled structure leaves a reading behind.
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
	 * The strain readout is a separate, additive solve (PROMOTION_DESIGN.md §3.5, SHED_PATH.md
	 * Phase A 6b). The break authority above has already settled the verdict; this poses the
	 * min-violation LP once on a copy of the same assembly — bMinViolationReadout routes it to a
	 * different formulation (equilibrium rows hard, every strength row relaxed with a penalised
	 * slack) that reads the closest-to-admissible force distribution rather than the vacuous
	 * maximise-lambda one. It writes nothing but this cache, so no break decision, support flag
	 * or ConnectionForces can move — the readout is the estimator the overlay reads, never the
	 * verdict.
	 *
	 * First-crack rows are on, matching the below-cap break authority. BreakByEquilibrium poses
	 * the gate with bFirstCrackRows below the cap, so a bonded joint cracks at its uncracked
	 * peak-fibre limit — three times stricter in bending than the plastic no-tension form. The
	 * readout must assemble the same rows or a bonded bending joint reports the plastic
	 * utilisation it's never held to (four times too comfortable at e = 3h). At M = 0 first
	 * crack and plastic coincide, so the pure-axial readouts are unchanged; the divergence is
	 * observable only where the joint bends.
	 *
	 * The cache is rebuilt each time, sized to the connections, so a below-cap re-solve can't
	 * leave a stale entry behind. A readout the solver couldn't produce (bPresent false) leaves
	 * the cache all-absent rather than filling it with zeros, so GetConnectionReadout fails
	 * closed to the router exactly as the gate itself does on a refusal.
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
	 * ConnectionOfJoint provenance — the same map BreakByEquilibrium uses to sever the
	 * mechanism's joints — so the cache the overlay reads by connection index carries the LP's
	 * per-joint N, M, violation and utilisation. A joint with no provenance entry, or a
	 * connection out of range, is skipped rather than guessed.
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
	 * Make GetPieceSupport LP-authoritative below the cap (PROMOTION_DESIGN.md §12 D7's 3b
	 * section, §3.7). The router's downward flood strands a piece whose load returns to it round
	 * a cycle it has no rule to divide; the LP has no routing to fail, so if it stands the
	 * structure that piece is genuinely carried. Overwrite the router's per-piece support with
	 * the LP verdict for every piece the bridge included:
	 *
	 *   - the LP stands the structure (no mechanism): every bridged piece
	 *     is carried, so it reads Supported, or Grounded if it's a
	 *     foundation block — the router's Stranded is overridden (rows 10
	 *     and 19 go from router-stranded to LP-carried);
	 *   - the LP is infeasible (a mechanism is present): the pieces the
	 *     mechanism moves have lost the earth and read Falling; every other
	 *     bridged piece is still carried and reads Supported/Grounded.
	 *
	 * A grounded block writes no equilibrium rows, so its mechanism triple is exactly zero and
	 * bMoves is false — it always reads carried, and GetPieceSupport maps a carried grounded
	 * piece to Grounded. Pieces the bridge did not include (removed, or excluded) are left with
	 * the router's answer, which is correct: a removed piece has no support to assert.
	 *
	 * PieceOfBlock[b] and Mechanism.Blocks[b] are the same block-index order, so they're read in
	 * lock-step; a piece the LP carries is never left Stranded.
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
	 * The per-joint capacity sweep — the router's break authority, run once per pass. Every
	 * joint over its own capacity gives in the same pass (DESIGN.md §3), stamped with this pass.
	 * This is the sole break authority above the block cap and on any LP refusal; below the cap
	 * the equilibrium gate answers instead and this sweep doesn't run, so it never latches a
	 * joint the mechanism didn't name. Returns whether any joint gave.
	 */
	bool bBroke = false;

	for (int32 Index = 0; Index < Connections.Num(); ++Index)
	{
		/*
		 * By reference, and this is the one line where that matters. FConnection is copyable and
		 * its "has given" latch is a member of the object, so
		 *
		 *     for (FConnection C : Connections)
		 *
		 * — one missing ampersand — would latch every overloaded joint on a temporary, leave the
		 * real connections intact, and report a structure that breaks nothing under any load
		 * whatsoever.
		 */
		FConnection& Connection = Connections[Index];

		/*
		 * A joint that has already given is skipped rather than re-evaluated, and this skip is
		 * what makes the loop terminate at all. ApplyForce answers a given joint with zero
		 * without latching a second time, so calling it looks harmless — but HasGiven below asks
		 * about the joint, not about that call, and it's still true. Every joint broken in an
		 * earlier pass would re-report itself as breaking now: the stamp would be rewritten to
		 * the current pass, bBroke would be set every time, and a structure that settled long
		 * ago would cascade forever. Confirmed by mutation — deleting these four lines hangs the
		 * suite rather than merely failing it.
		 *
		 * The stamp is history; the latch is only the present. Only the transition from intact
		 * to given belongs to a pass.
		 */
		if (Connection.HasGiven())
		{
			continue;
		}

		/*
		 * The moment goes in beside the force, exactly as GetConnectionUtilisation hands the
		 * same pair to the same evaluator. The two are one question asked twice — what is this
		 * joint carrying — and a sweep that broke joints on the force alone would answer it
		 * differently from the readout the moment a load path missed a centroid: a joint drawn
		 * at 1.25 of capacity, holding forever. Both arrays are rebuilt by the solve above and
		 * indexed alike.
		 *
		 * And so does the composite depth, the same seam pointing the other way. It's a relief
		 * rather than a load, so a sweep that omitted it would break joints the readout draws as
		 * comfortable — a corbel shown at 0.37 and snapped at 22.9. Three arrays now, one solve,
		 * one evaluator.
		 *
		 * The strength is the weakest-link pairing, not the bare connection — the same
		 * EffectiveJointStrength the readout (GetConnectionUtilisation) and the LP bridge decide
		 * on. A cross-material bearing (wood post on brick footing) reads its material crush,
		 * not the connection's own capacity, so the one place that actually severs above the cap
		 * agrees with the overlay that draws the joint failed. Where neither face names a
		 * material EffectiveJointStrength returns the bare connection bit-for-bit, so every
		 * single-material fixture is unaffected.
		 *
		 * The copy decides; the real connection severs. The "has given" latch is a member of the
		 * object, so ApplyForce is called on a copy carrying the paired strength — used only to
		 * reach the verdict — and the actual break is stamped onto the real Connection via
		 * Sever(). Latching the copy and never touching the real joint is the same
		 * missing-ampersand hazard the loop header warns about, one level in: the structure
		 * would break nothing under any load.
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
	 * Every joint over capacity gives in the same pass (DESIGN.md §3). Each pass is a complete
	 * solve followed by one sweep that breaks everything the solve found over its own capacity,
	 * and the pass number is stamped on each joint that gives.
	 *
	 * Ordering within a pass is arbitrary and nothing may depend on it — two joints that give
	 * together are simultaneous and the array order they're visited in is an implementation
	 * detail. Ordering between passes is real: it's caused, each break following from the load
	 * the previous one shed, and it's the sequence a collapse is played back in.
	 *
	 * The alternative, breaking only the worst joint each pass, reaches the same settled state
	 * and costs a solve per joint to do it — and worse, it invents a sequence where there is
	 * none, reporting three independently overloaded joints as failing one after another.
	 */
	int32 BreakingPasses = 0;

	/*
	 * Invalidate the cached strain readout before this settle (SHED_PATH.md Phase A 6b). It's
	 * refilled solve-on-settle by BreakByEquilibrium below the cap; clearing it here means a
	 * re-solve — including one that now declines to the router above the cap — can't return the
	 * readout a previous below-cap settle cached, so a stale reading can never be served.
	 */
	ConnectionReadoutCache.Reset();

	// Observability only: zero the readout-solve counter for this whole cascade (see the getter).
	MinViolationReadoutSolves = 0;

	/*
	 * Pass numbers are global to the structure, so this call continues from the highest stamp
	 * already written rather than starting again at 1. Stamps are the record of the order a
	 * collapse happened in (DESIGN.md §3), and every consumer reads a shared number as "these
	 * gave simultaneously" — so a joint that gave after a player pulled a brick out has to carry
	 * a strictly larger number than everything that gave before it.
	 *
	 * This was unreachable until removal existed: a second cascade on a settled structure breaks
	 * nothing, so no two calls could both stamp. RemovePiece is the one operation that changes
	 * the graph between calls, and it's the MVP's own interaction.
	 *
	 * The return value is a different question and stays per-call. BreakingPasses below counts
	 * only the passes this call ran, which is what a caller polls to find out whether its
	 * removal did anything; only the stamps accumulate. Conflating the two — returning the
	 * high-water mark, or adding the previous count onto every stamp — is the obvious wrong fix
	 * and Structure.BreakPassesContinueAcrossCalls catches it.
	 *
	 * INDEX_NONE is -1 and every real stamp is at least 1, so an unstamped joint can't raise the
	 * mark and a structure that has never broken starts, correctly, at 1.
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
	 * (REGIONAL_PROVER_PLAN.md §1, physics-model call 4). On the first pass of this call the
	 * disturbance is the removal, so the seed is the live neighbours of every tombstoned piece:
	 * the joints RemovePiece severed are the graph edges the collapse propagates along, and a
	 * tombstone stamps no pass, so it isn't reachable through ConnectionBreakPass. On later
	 * passes the disturbance is the previous pass's breaks, so the seed is the live endpoints of
	 * the joints stamped Pass - 1 — the same "what shed its load last pass" the cascade already
	 * reads to sequence a collapse.
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
	 * The report is filled as the cascade runs, and decides nothing. Every clock and count here
	 * feeds GetLastSolveAndBreakReport so a reader can see which pass cost what and why; the
	 * cascade itself reads none of it back.
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
		 * The equilibrium gate decides the pass, and below the block cap it's the sole break
		 * authority (DESIGN.md §7 step 4, PROMOTION_DESIGN.md §6 Slice 3, §12 D7's 3b section).
		 * It asks the rigid-block LP whether the whole structure has any admissible force system
		 * in equilibrium with self-weight — a state no per-joint number can express, which is
		 * why a leaning stack read the same comfortable utilisation at every height and a
		 * two-load-path body reads safe past its tipping point — and on "no" it severs exactly
		 * the joints the collapse mechanism opens. It also rewrites the support arrays from the
		 * LP, so GetPieceSupport is LP-authoritative below the cap.
		 *
		 * When it declines (over the cap, no geometry, an LP refusal) the per-joint capacity
		 * sweep is the sole authority this pass, exactly as production behaved before the gate.
		 * When it answers, the capacity sweep is demoted to the estimator it already is: the
		 * forces it would read still sit in ConnectionForces from the solve above, so the
		 * utilisation overlay is unchanged, but it latches nothing — the mechanism is the only
		 * thing that breaks a joint.
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
			 * that reads a comfortable per-joint utilisation while the whole assembly has no
			 * admissible equilibrium. A grounded-boundary LP over the disturbance neighbourhood
			 * can prove that collapse the router missed and upgrade its stand to a proven fall —
			 * one-directional, toward Falling only (grounding the boundary only adds support, so
			 * an infeasible region is genuinely infeasible globally; it never credits a stand).
			 * Gated on HasCompleteGeometry() exactly as BreakByEquilibrium, so the geometry-free
			 * fuzzes are a provable no-op.
			 */
			if (HasCompleteGeometry())
			{
				/*
				 * Progress is a severed joint, and only a severed joint — never the prover's
				 * Falling count. The prover re-poses its region every pass and the LP re-fells a
				 * piece it already felled last pass: a piece the prover disconnected is a
				 * jointless block in the next pose, trivially "moving" under no constraints, so
				 * the LP names it again and the stitch re-marks it Falling. Keying the cascade
				 * on "a piece went Falling" would count that re-mark as progress and loop
				 * forever, posing an LP every pass. Intact joints only ever decrease, so keying
				 * on a severed joint is a monotone, bounded termination guarantee: at most one
				 * pass per connection.
				 *
				 * It's also a complete witness of a genuine felling. A rigid joint between a
				 * moved block and a block that stays put can't remain closed — the
				 * mechanism-extraction tolerance (1e-6 relative, RigidBlockOracle.cpp) severs
				 * every moved-vs-standing joint, since only the exact rotation centre is
				 * sub-tolerance and no finite-width contact sits there. So a prover-felled piece
				 * always fully disconnects from every standing piece, and SolveLoads, which
				 * grants Supported only along intact paths to ground, then reads it Falling on
				 * its own. (This is why the "router re-holds a felled-but-still-connected piece"
				 * case doesn't exist and can't be unit-tested — proven 2026-09-03; the guard is
				 * against the re-felling loop above, not that.) The only felling that severs
				 * nothing is a body already fully disconnected, so whenever the prover genuinely
				 * upgrades a router stand to a fall it severs a boundary joint — and the Falling
				 * override is re-applied on every pass including the terminal non-breaking one,
				 * so the settled state carries it even when a pass counts no break.
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
		 * A pass that breaks nothing is the last one, and isn't counted: the loads it computed
		 * are the settled state, over exactly the joints that survived.
		 *
		 * Termination: joints never heal, so every counted pass permanently removes at least one
		 * connection from the structure and there can be no more of them than there are
		 * connections. (Not to be confused with SolveLoads' own internal fixpoint, a smaller
		 * thing nested inside each of these passes.)
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
	 * Said out loud at Log level only when it matters to a player: a cascade that broke
	 * something, or one that cost more than a frame. A settle that changed nothing in a few
	 * milliseconds is Verbose.
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
	 * The isolated test entry (REGIONAL_PROVER_PLAN.md slice 1). It settles the router baseline,
	 * then defers the whole region flood + grounded-boundary pose + Falling-only stitch to the
	 * shared ProveRegionalCollapse — the identical machinery the real SolveAndBreak cascade
	 * drives (slice 4a). BreakPass defaults to 1 here, exactly the stamp this entry wrote before
	 * the factoring, so the slice-1/2/3 tests read unchanged.
	 */
	SolveLoads();
	return ProveRegionalCollapse(Seed, RegionBlockCap);
}

int32 FStructure::ProveRegionalCollapse(const TArray<int32>& Seed, int32 RegionBlockCap, int32 BreakPass)
{
	/*
	 * The regional collapse prover (REGIONAL_PROVER_PLAN.md §§1-4, review item 12). A
	 * grounded-boundary LP over a neighbourhood of the disturbance can upgrade a router stand to
	 * a proven fall — never the reverse. A region LP with its frontier pinned to the earth is a
	 * one-directional prover: it can prove collapse (grounding the boundary only adds support,
	 * so an infeasible region is genuinely infeasible globally) but never standing, so it only
	 * ever marks pieces Falling. The caller has already settled the graph (SolveAndBreak's
	 * per-pass SolveLoads, or the isolated entry's baseline), so this does not SolveLoads —
	 * re-solving would waste work and wipe the support state the stitch overrides.
	 */

	/*
	 * The joint-hop adjacency the flood walks, rebuilt exactly as SolveLoads builds it: every
	 * intact joint touching each piece, in ascending connection index. A given joint is out of
	 * the graph, so a severed neighbourhood doesn't re-reach across a break.
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
	 * The region and its grounded frontier, grown in place. AdmitToRegion moves a live candidate
	 * into Region only if the region united with the grounded boundary ring it would then induce
	 * still fits the running block budget — the budget is spent on R and the B that R carries
	 * (bounding |R| alone lets the one-hop ring push the posed problem past the budget, the
	 * plan's gate "region u grounded boundary <= budget", REGIONAL_PROVER_PLAN.md §1). Boundary
	 * is kept as the region's exact induced one-hop frontier at every step; a rejected candidate
	 * stays in that boundary — its grounded ring is the region's tie to the earth it hangs from.
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
	 * returning the number of blocks admitted. A frontier seed that can't be admitted (the
	 * budget is full) is dropped, not grounded — grounding the very pieces the growth pushes
	 * from would pin the collapse it chases; a BFS-discovered neighbour that can't be admitted
	 * becomes grounded boundary instead. The BFS order is a function of the (sorted) frontier
	 * array and the ascending-connection-index adjacency, both fixed, so a caller that hands a
	 * deterministically-ordered frontier gets a deterministic region.
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
	 * Phase 1 — a modest initial region flooded from the disturbance seed, deliberately far
	 * smaller than RegionBlockCap (REGIONAL_PROVER_PLAN.md §§1-3). This is the whole point of
	 * grow-on-contact: pose an LP the size of the local mechanism, not the size of the cap. A
	 * cap-sized first pose is what made the flagship 3D scenarios impractical (a ~200-block LP
	 * on every above-cap cascade pass); a modest region that grows only as the proved mechanism
	 * demands keeps every pose mechanism-sized even with a large cap standing as the ceiling.
	 * The budget is the seed's own footprint plus a small ring, floored at 16 so a single-piece
	 * seed still poses a meaningful neighbourhood, and never above the cap.
	 */
	const int32 InitialBudget = FMath::Min(RegionBlockCap, FMath::Max(16, Seed.Num() + 8));
	int32 EffectiveBudget = InitialBudget;

	GrowFrom(Seed, EffectiveBudget);

	/*
	 * Grow-on-contact (REGIONAL_PROVER_PLAN.md slice 3). After each solve:
	 *   - a certified fall whose mechanism is interior (no moved block
	 *     touches a cut-artifact grounded boundary — every grounded
	 *     neighbour is a genuine bIsGrounded foundation) is complete: the
	 *     true mechanism can't extend past real earth, so stitch it;
	 *   - a certified fall that does touch a cut-artifact grounded
	 *     boundary may hide more collapse behind that pinned block, so re-
	 *     flood the region from the moved set at a larger budget —
	 *     mechanism directed, so a disconnected standing component the
	 *     modest flood happened to admit (a grounded island) is dropped
	 *     (unreachable from the moved seeds) and the whole budget is
	 *     re-spent on the collapse. Re-flood, not merely grow-the-
	 *     frontier, is what prunes that dead weight;
	 *   - a pose that doesn't fall hasn't yet reached the mechanism (the
	 *     modest region may be too small to contain it), so grow a bounded
	 *     speculative search outward from the whole current boundary and
	 *     look again.
	 * Growth is monotone (freeing a cut-artifact grounded block can only
	 * let more move) and the budget doubles each grow, so a local
	 * mechanism settles in one or two solves and a cap-spanning one in a
	 * few. Termination: an interior mechanism; the budget already at its
	 * ceiling (cap-bound for a fall — stitch the sound partial, the router
	 * keeps the rest); a grow/re-flood that changes nothing (fixpoint); or
	 * the monotone iteration bound. GetLastRegionalProblemBlockCount
	 * reports the final pose's |R u B|.
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
		 * first-crack rows — the identical authority BreakByEquilibrium poses. A bridge refusal
		 * fails closed: no region opinion, so the router baseline stands untouched.
		 */
		Problem = RigidBlockOracle::FOracleProblem();
		FString WhyNot;

		if (!RigidBlockOracle::BuildRegionalProblem(*this, Region, Boundary, Problem, WhyNot))
		{
			return 0;
		}

		/*
		 * Record the posed problem size — |region u grounded boundary| — the moment the pose
		 * exists. Each grow pass overwrites it, so GetLastRegionalProblemBlockCount reports the
		 * final pose; every grow bounds the budget at RegionBlockCap, so it's the gate's witness
		 * the ring never overspends.
		 */
		LastRegionalProblemBlockCount = Problem.Blocks.Num();

		Problem.bGravityIsLive = false;
		Problem.bFirstCrackRows = true;

		const double LpStartSeconds = FPlatformTime::Seconds();

		Result = RigidBlockOracle::SolveRigidBlock(Problem);

		/*
		 * Compute this pose's wall-clock once and feed both the aggregate and the per-pose
		 * record, so the RegionalPoseBreakdown ms sum reconciles exactly with LastProverLpMs.
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
		 * One record per pose, appended here where every loop iteration reaches it exactly once
		 * — the increment above and this append stay in lockstep, so RegionalPoseBreakdown.Num()
		 * == RegionalPoses.
		 */
		FProverPoseReport& Pose = LastProverPoseBreakdown.AddDefaulted_GetRef();
		Pose.Blocks = Problem.Blocks.Num();
		Pose.LpPivots = Result.SimplexIterations;
		Pose.LpMs = PoseMs;
		Pose.bFell = bCertifiedFall;

		/*
		 * The frontier the next grow pushes from, and the budget ceiling it may grow to. A
		 * certified fall re-floods mechanism-directed (from the moved set) up to the full
		 * RegionBlockCap — real collapse is worth the whole budget. A pose that doesn't fall
		 * grows a speculative search from the whole boundary, capped at RegionSpeculativeCeiling
		 * far below RegionBlockCap: without a mechanism to size to, a blind flood must not
		 * balloon toward the cap. A large standing region is the single most expensive pose (a
		 * feasibility proof over hundreds of blocks), and a blind flood on a big structure hits
		 * exactly that — the speculative ceiling keeps a wandering search cheap, while a chain
		 * small enough to fit inside it still floods to its free end and falls (so the
		 * one-directional prover still upgrades those router over-holds). A local mechanism the
		 * search would only reach past the ceiling is an accepted miss — the router already
		 * stands it (REGIONAL_PROVER_PLAN.md §1, "a region too small to contain the mechanism
		 * ... acceptable").
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
			 * block (in Boundary but not a genuine bIsGrounded foundation)? A real foundation is
			 * earth and stays earth; only a block the flood pinned grounded may hide more
			 * collapse behind it. No contact means the mechanism is bounded by genuine
			 * foundations — interior, nothing more to reveal.
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
			 * search outward from the whole current boundary (no mechanism to follow) and look
			 * again, bounded by the speculative ceiling. This is the only branch that grows a
			 * non-mechanism direction, and it runs only while nothing has fallen, so it can't
			 * grow into a standing island that sits beside a live collapse (that path always
			 * falls first and re-floods above).
			 */
			GrowFrontier = Boundary.Array();
			GrowCeiling = RegionSpeculativeCeiling;
		}

		/* Deterministic admission order regardless of TSet iteration order. */
		GrowFrontier.Sort();

		if (EffectiveBudget >= GrowCeiling)
		{
			/*
			 * At the applicable ceiling — cap-bound for a fall (stitch the sound partial below,
			 * the router keeps the rest), or the speculative ceiling reached with no fall (an
			 * accepted miss, return 0).
			 */
			break;
		}

		if (bReFloodFromMechanism)
		{
			/*
			 * Re-flood sized to the mechanism plus exactly one adjacency ring of new
			 * exploration, rather than a blind doubling that balloons several times past it. The
			 * budget bounds region plus grounded boundary, so growing the movable region by one
			 * ring costs two joint-hops of budget: the moved set unioned with its immediate
			 * neighbours is the region the pose must be free to move (MovableRing), and one hop
			 * further out is the grounded boundary that pins it (PinnedRing). Sizing to the
			 * movable ring alone would pin that ring as boundary instead of admitting it to the
			 * region, and re-flooding from the same moved set would then reproduce the same
			 * pinned ring — a fixpoint short of the true mechanism, so a deep collapse behind
			 * the ring would never be revealed (its moved set never reaches the ring). With the
			 * boundary hop included the region advances one movable ring per iteration: a
			 * mechanism deeper than one ring is revealed by the next iteration, which re-floods
			 * from the new, larger moved set, so the region keeps expanding while collapse
			 * continues and still terminates at the cap or a fixpoint. Rebuild the region from
			 * the moved seeds so blocks unreachable from the collapse (a disconnected grounded
			 * island) fall out and the budget is re-spent on the mechanism's own component. A
			 * rebuild that reproduces the region is a fixpoint (or cap-bound) — nothing more to
			 * reach, so stitch the current mechanism.
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
			 * Speculative re-flood with no mechanism to size to: double the budget and search
			 * wider from the whole boundary, bounded by the speculative ceiling.
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
	 * Falling-only stitch. A grounded boundary block writes no equilibrium rows and never moves,
	 * so the moved blocks are all interior by construction; map them back to pieces through
	 * PieceOfBlock and mark each Falling. Never Supported — a region LP is a collapse prover
	 * only, and crediting a stand above the cap is exactly the false-stand direction case-21
	 * forbids. Falling is written as SolveLoads writes it: not held up and not stranded.
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
	 * Sever the intact joints the mechanism opens, mapped back to production connections through
	 * the bridge's ConnectionOfJoint provenance — the identical release/sever idiom
	 * BreakByEquilibrium uses. A joint already gone is skipped; this pass stamps the ones it
	 * severs.
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
	 * The number of oracle blocks the last SolveAndBreak_WithRegionalProver posed to
	 * BuildRegionalProblem — |region ∪ grounded boundary| — or INDEX_NONE if no regional prove
	 * has posed a problem yet. The flood bounds this at RegionBlockCap (see the pose in
	 * SolveAndBreak_WithRegionalProver), so it's the plan's stopping-gate witness.
	 */
	return LastRegionalProblemBlockCount;
}

int32 FStructure::GetLastEquilibriumProblemDim() const
{
	/*
	 * The dimension the last equilibrium-gate pose was built in — 2 for the X-Z problem, 3 for
	 * the volumetric one — or INDEX_NONE if no pose has been built. Stamped by
	 * BreakByEquilibrium from Problem.Dim the moment the bridge accepts, so a declined pass
	 * reports the last pose rather than the last call. Test-only observability: no production
	 * code may branch on it.
	 */
	return LastEquilibriumProblemDim;
}

void FStructure::SetEquilibriumGateBlockCap(int32 MaxBlocks)
{
	/*
	 * Stores the cap the equilibrium gate consults: the gate is authoritative at or below it and
	 * fails closed to the router above it (PROMOTION_DESIGN.md §12 D6-c). Injectable so the
	 * scope-by-size tests can drive either side of the boundary on a small fixture. A bare
	 * assignment — no branch, no arithmetic — so it drives no behaviour on its own.
	 */
	EquilibriumGateBlockCap = MaxBlocks;
}

void FStructure::SetRegionBlockCap(int32 MaxBlocks)
{
	/*
	 * Stores the cap the regional prover's flood consults in SolveAndBreak's above-cap decline
	 * arm (REGIONAL_PROVER_PLAN.md §4, physics-model call 2): the largest |region ∪ grounded
	 * boundary| the flood may pose. Defaults to 200, matching the equilibrium gate — affordable
	 * because grow-from-modest poses a mechanism-sized region, not a cap-sized one (a local
	 * over-hold poses a few blocks even at cap 200, and the speculative ceiling keeps a no-fall
	 * search cheap), so the cap is only ever the ceiling a genuinely large propagating collapse
	 * extends toward. A bare assignment — no branch, no arithmetic — so it drives no behaviour
	 * on its own; the cascade seam reads it.
	 */
	RegionalProverBlockCap = MaxBlocks;
}

void FStructure::SetPieceMaterial(int32 PieceIndex, const DestructionProfiles::FMaterialProfile* Material)
{
	/*
	 * Records what a piece is made of so a cross-material joint can reach its two faces'
	 * materials (router + oracle bridge, via EffectiveBondedStrength). A bare store behind a
	 * range guard — no branch on the value, no arithmetic. An out-of-range handle is ignored,
	 * the same fail-closed shape the other accessors take.
	 */
	if (Pieces.IsValidIndex(PieceIndex))
	{
		Pieces[PieceIndex].Material = Material;
	}
}

void FStructure::SetThreeDimensional(bool bIsThreeDimensional)
{
	/*
	 * The 3D permission `RigidBlockBridge` branches on (THREED_DESIGN.md E3). When set,
	 * `BuildRigidBlockProblem` may pose this structure as Dim3D — the real 3D joint geometry,
	 * the out-of-plane Y normal accepted — and does so only when the posed problem actually
	 * leaves the X-Z plane; a flagged structure whose posed rows are all planar is posed 2D
	 * (`GetLastEquilibriumProblemDim` reads which). When unset (the default), the 2D X-Z pose
	 * and its Y-normal refusal stand.
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
	 * An unknown connection is not a joint that broke, so it fails closed to the same answer as
	 * one that never gave.
	 */
	return ConnectionBreakPass.IsValidIndex(ConnectionIndex)
		? ConnectionBreakPass[ConnectionIndex]
		: INDEX_NONE;
}

int32 FStructure::GetBreakAuthority(int32 ConnectionIndex) const
{
	/*
	 * An unknown connection severed nothing, so it fails closed to INDEX_NONE — the same answer
	 * as a joint no authority ever gave. See the header for the code meanings (1 gate, 2 sweep,
	 * 3 prover).
	 */
	return ConnectionBreakAuthority.IsValidIndex(ConnectionIndex)
		? ConnectionBreakAuthority[ConnectionIndex]
		: INDEX_NONE;
}

EJointRole FStructure::GetJointRole(int32 ConnectionIndex, int32 PieceIndex) const
{
	/*
	 * This is the decision SolveLoads itself routes by — it calls this rather than carrying its
	 * own copy — which is what makes a readout of a tier and the tier the load actually took the
	 * same answer rather than two that agree until they don't.
	 *
	 * A connection is described by a normal pointing toward PieceB, so the first thing to do is
	 * turn it to point at the piece being asked about. Pointing substantially up at this piece,
	 * the interface is beneath it and bears it. Pointing substantially down, the bed joint is
	 * above — something resting on this piece, or something this piece is glued underneath — and
	 * neither holds it up. That direction is the whole correction: routing that only asked
	 * whether two pieces were joined let a load path run upward through a joint.
	 *
	 * A normal that won't normalise is answered None rather than given a tier. Normalize returns
	 * false for both a zero-length and a NaN normal, so the comparison below is never reached
	 * with a NaN in it — the guard is here rather than in the comparison because a NaN falling
	 * through would land in Head, a support tier.
	 *
	 * An unknown connection needs a guard of its own — the argument that it didn't was the bug.
	 * It used to read "GetConnection hands back a placeholder whose zero normal won't
	 * normalise", the reasoning GetConnectionUtilisation is entitled to and this isn't, since
	 * the two consume different fields of the placeholder: utilisation consumes the area, zero
	 * on a default FConnection, which ComputeUtilisation's own guard already fails closed on;
	 * this consumes the normal, and a default's normal is FVector::ZAxisVector — (0,0,1), which
	 * normalises perfectly. Carried across, the placeholder's PieceB of INDEX_NONE matched an
	 * unidentified PieceIndex and the tier came back BedBeneath: the strongest support tier
	 * there is, reported for a joint and a piece that don't exist.
	 *
	 * With the handle known good the piece check below needs nothing of its own. AddConnection
	 * refuses a joint whose ends aren't both valid piece handles, so a stored connection never
	 * names INDEX_NONE at either end and an unidentified piece can't find one to match — a door
	 * guard that exists and is tested, precisely what the placeholder-normal claim was not.
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
	 * Zero for an out-of-range handle, and zero for a connection that no solve has reached —
	 * nothing in an ungrounded island is being held up, so there is no static load path to
	 * report.
	 */
	return ConnectionForces.IsValidIndex(ConnectionIndex)
		? ConnectionForces[ConnectionIndex]
		: FVector::ZeroVector;
}

FVector FStructure::GetConnectionMoment(int32 ConnectionIndex) const
{
	/*
	 * Zero for an out-of-range handle, and zero for a connection no solve has reached — the same
	 * scope and shape as GetConnectionForce, because it's the same solver output rebuilt by the
	 * same solve.
	 *
	 * A moment is a load, not a verdict, so the fail-closed answer here is the opposite polarity
	 * to GetConnectionUtilisation's Max(): zero says nothing is levering this, the conservative
	 * reading for something that isn't a joint, and it's the utilisation that has to come back
	 * reading as failed. An invented enormous moment would make a NaN of everything finite
	 * downstream of it.
	 */
	return ConnectionMoments.IsValidIndex(ConnectionIndex)
		? ConnectionMoments[ConnectionIndex]
		: FVector::ZeroVector;
}

double FStructure::GetConnectionCompositeDepthCm(int32 ConnectionIndex) const
{
	/*
	 * Zero for an out-of-range handle and zero for a connection no solve has reached — the same
	 * scope and shape as the two accessors above, because it's the same solver output rebuilt by
	 * the same solve.
	 *
	 * A depth is a relief, not a load, so the fail-closed answer here is zero for the opposite
	 * reason GetConnectionMoment's is: no masonry credited means the joint reads its own bed
	 * patch and reports as heavily loaded, and it's the utilisation that must come back reading
	 * as failed for something that isn't a joint. An invented depth would quietly relieve one.
	 */
	return ConnectionCompositeDepthCm.IsValidIndex(ConnectionIndex)
		? ConnectionCompositeDepthCm[ConnectionIndex]
		: 0.0;
}

double FStructure::GetConnectionUtilisation(int32 ConnectionIndex) const
{
	/*
	 * Delegated whole, never re-derived: FConnection::UtilisationUnder is the one evaluator the
	 * break decision itself is made on, so composing it over the routed force keeps this
	 * accessor from becoming a hand-copy of that arithmetic — and it inherits the
	 * degenerate-normal obligation for free. An unknown handle needs no branch of its own
	 * either: GetConnection returns a placeholder whose area is zero, which ComputeUtilisation's
	 * own area guard already answers with TNumericLimits<double>::Max() rather than a healthy
	 * zero.
	 *
	 * Force, moment and composite depth all come off the accessors rather than off the arrays
	 * directly, which is what makes the identity Structure.h states — this answer equals
	 * UtilisationUnder of GetConnectionForce, GetConnectionMoment and
	 * GetConnectionCompositeDepthCm — true by construction rather than by three range checks
	 * agreeing; reading the arrays here would be the second copy the seam exists to rule out.
	 * Dropping the moment would read a joint too optimistic, dropping the depth too pessimistic
	 * — both a readout disagreeing with the cascade about the same joint. Zero for a handle no
	 * solve has reached, exactly as the force is: a load path with no eccentricity, not a
	 * tolerance.
	 *
	 * The strength evaluated against is the weakest-link material pairing rather than the bare
	 * connection, so a wood-on-brick bearing reads its material crush and not the connection's
	 * own capacity. A copy carries the effective strength onto the same geometry and routed
	 * force, so the identity above still holds joint by joint; where neither face names a
	 * material, EffectiveJointStrength returns the bare connection and this is bit-identical to
	 * reading GetConnection directly.
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
	 * Pair the connection with its two faces' materials, weakest-link, only when both faces name
	 * one. A joint's two pieces are FConnection::PieceA / PieceB, and each piece carries what
	 * it's made of on FStructurePiece::Material. When both are present the bearing is
	 * cross-material and its capacity is min(connection, matA, matB) per axis
	 * (DestructionForce::EffectiveBondedStrength); when either is null — "nobody said what this
	 * is made of" — the bare connection governs, the fail-safe default every fixture that
	 * assigns no material keeps reading.
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
	 * below the block cap (CacheMinViolationReadout, keyed by the bridge's ConnectionOfJoint
	 * provenance); above the cap the gate declines, nothing solves it, and the cache stays empty
	 * so this reads absent — the overlay then falls back to the router's
	 * GetConnectionUtilisation. Absent too for an out-of-range handle and before any solve,
	 * exactly as the other solver accessors are empty before they're filled.
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
	 * Derived from IsPieceSupported rather than computed beside it. The two are one answer at
	 * two resolutions, so asking the coarser one here is what makes
	 *
	 *     IsPieceSupported(H) == (GetPieceSupport(H) is Grounded or Supported)
	 *
	 * true by construction instead of by agreement — including for an unknown handle, where the
	 * range check inside it is the only one either accessor needs, and before any solve, where
	 * the arrays are empty and every handle is unknown.
	 *
	 * Only the reason for not being held up needs the second array, and the order is deliberate:
	 * a piece is never both stranded and supported, but reading support first means that if it
	 * ever became possible the answer would still agree with the boolean rather than
	 * contradicting it.
	 */
	if (IsPieceSupported(PieceIndex))
	{
		return GetPiece(PieceIndex).bIsGrounded ? EPieceSupport::Grounded : EPieceSupport::Supported;
	}

	/*
	 * Stranded is a claim about the solver, so it's only ever made about a piece the last solve
	 * actually found in a knot. Everything else falls through to Falling: a piece resting only
	 * on a knot, a piece with nothing beneath it at all, a removed piece once something has
	 * re-solved, and a handle that names no piece.
	 */
	return PieceStranded.IsValidIndex(PieceIndex) && PieceStranded[PieceIndex]
		? EPieceSupport::Stranded
		: EPieceSupport::Falling;
}

bool FStructure::HasSupportAnswer(int32 PieceIndex) const
{
	/*
	 * The array's own extent is the answer, which is why there's no "have we solved yet" flag
	 * anywhere near this. SolveLoads sizes PieceSupported to the piece count and nothing else
	 * writes it, so a flag beside it would be a second copy of something the array already
	 * carries — and a flag would answer the wrong question anyway: it says whether a solve
	 * happened, not whether it reached this handle, so a piece added since would sail past it.
	 *
	 * The same range check IsPieceSupported makes on the way to its own answer, asked here for
	 * its own sake rather than duplicated.
	 */
	return PieceSupported.IsValidIndex(PieceIndex);
}
