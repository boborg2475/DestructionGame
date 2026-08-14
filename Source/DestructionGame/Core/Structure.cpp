// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Structure.h"

#include "Core/Profiles/ConnectionProfiles.h"

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
	 * How deep an arch may be, as a fraction of the span it crosses: sqrt(3)/2, to three digits.
	 *
	 * BS 5977-1 specifies the equilateral TRIANGLE OF LOADING over an opening — 60 degree base
	 * angles, so a height of sqrt(3)/2 of the span — as the masonry that arches around rather
	 * than reaching the span. ARCHING_DESIGN.md adopts that angle ONLY as a cap on the arching
	 * depth and deliberately does not use it to reduce the load, which is the strictly harsher
	 * reading and is what keeps a dispersion angle out of the data.
	 *
	 * IT IS A MODELLING CONSTANT WITH A PUBLISHED SOURCE, NOT A MATERIAL PROPERTY, so it belongs
	 * here beside SolverBedJointCosine as one number in one place. A `bDevelopsArchAction` flag
	 * or a per-profile dispersion angle is the regression DESIGN.md §2 names by name: everything
	 * else the arch needs is already derivable from the strengths the profiles carry.
	 *
	 * SPELLED 0.866 RATHER THAN sqrt(3)/2 because that is the published figure and the two
	 * differ by 3e-5 relative — far inside anything this model can distinguish.
	 */
	constexpr double SolverArchingDepthPerSpan = 0.866;

	/**
	 * How deep a deep beam may be, as a multiple of the effective arm of the joint under it.
	 *
	 * PROVISIONAL, AND IT IS A RULING RATHER THAN A DERIVATION. COMPOSITE_DEPTH_DESIGN.md slice 3
	 * exists to settle this number and has not been made; until it is, every reading that depends
	 * on it is provisional too. What is NOT provisional is the FORM — a bound proportional to the
	 * joint's own e = |M|/|F| is the only one worked through that satisfies both ends at once, and
	 * a shear-transfer budget provably cannot bound the depth at all because it is a FLOOR (tau_max
	 * goes as 1/D, so a deeper section is easier to sustain, not harder).
	 *
	 * WHERE 3.464 COMES FROM, STATED AS THE POST-HOC RATIONALISATION IT IS. It is 2*sqrt(3), which
	 * is four times SolverArchingDepthPerSpan — BS 5977-1's equilateral triangle read over the
	 * mirror span 2e a cantilever is half of, then doubled again. That reuse is why this value was
	 * preferred over its neighbours; it is NOT why the value is what it is. The published guidance
	 * points the other way: EN 1992-1-1 5.3.1(3) calls a member a deep beam when its span is under
	 * three times its depth, which as a validity limit gives about 0.67 — and 0.67 fails the free
	 * end by a factor of four. THE FREE-END RULING IS WHAT ACTUALLY SETS THIS NUMBER. The user
	 * ruled that a brick deleted at the end of a wall must not bring the wall down, that needs at
	 * least 2.465, the one-sided corbel property needs at most 3.822, and 3.464 is the only value
	 * in that window with margin on both sides.
	 *
	 * (COMPOSITE_DEPTH_DESIGN.md writes this as "2 x 0.866". That is 1.732 and reproduces none of
	 * that document's own figures — its table, its K = 1.56129/lambda^2 = 0.130117 identity and
	 * its 0.4493 and 0.5064 predictions are all 3.464. The value is 3.464; the arithmetic in the
	 * prose is a slip between lambda and the lambda/2 its matched-corbel lemma also uses.)
	 *
	 * SPELLED AS A LITERAL RATHER THAN AS 4 * SolverArchingDepthPerSpan, even though the two are
	 * bit-identical, because the arching angle does not GOVERN this. Deriving it in code would
	 * mean a future correction to a lintel-loading angle silently re-ruled the free end.
	 */
	constexpr double SolverCompositeDepthPerArm = 3.464;

	/**
	 * The INTERIM OVERTURNING GUARD'S bed-joint bond strength, MPa — MEAN-basis, and READ
	 * FROM THE GENERAL PURPOSE MORTAR PROFILE rather than duplicated here.
	 *
	 * IT IS THE PROFILE'S TensileStrengthMPa, 0.70, and until the 2026-08-14 mean-strength
	 * re-anchor it could not be. Before that flip the profile carried the CHARACTERISTIC
	 * 0.10 — a 5%-fractile design floor no honest overturning verdict could be ruled at —
	 * so the guard held its own 0.6 literal, six times the profile's figure. Since the
	 * re-anchor the profile's 0.70 IS a measured mean, and a second copy of a strength is
	 * exactly the drift the profile library exists to end.
	 *
	 * THE SWAP LANDED BEHIND ITS OWN RED, under the project's non-waivable TDD rule: moving
	 * 0.6 to 0.70 shifts every overturning verdict by 16.7%, and for a while nothing in the
	 * suite could tell the two values apart — the leaning-stack bracket deliberately holds
	 * at both ends, and the specified 8/11 interim-guard corbel pair answers identically at
	 * both by construction. The fixture that discriminates them is
	 * `Core.Structure.TheOverturningGuardRestoresAtTheProfilesMeanBond`
	 * (Tests/OverturningGuardBasisTest.cpp): a nine-step bare corbel arm whose bearing
	 * stands at 0.70 (ratio 0.869) and is severed at 0.6 (1.014), paired with a ten-step arm
	 * that falls at either value, together bracketing this constant into
	 * (0.60854, 0.75975) MPa. A retune that moves the profile's bond outside that bracket
	 * fails that test rather than moving this quietly.
	 *
	 * NOT constexpr, AND THAT IS SAFE: `GeneralPurposeMortar` is an aggregate initialised
	 * entirely from literals, so it is CONSTANT-initialised during static initialisation and
	 * strongly happens-before any dynamic initialiser — including this one.
	 *
	 * The honest mean bracket for clay in general-purpose mortar is 0.6-1.0 MPa (UK NA to
	 * BS EN 1996-1-1 Table NA.6 characteristic f_xk1 of 0.5 / 0.4 / 0.3 by water
	 * absorption, mean tested bond ~2x characteristic again). 0.70 is the low-middle of it,
	 * measured by the Gooch campaigns the profile cites, so a body the guard lets stand
	 * stands at a defensible mean bond rather than at an optimistic one.
	 *
	 * MEASURED FROM BOTH SIDES rather than tuned: at the characteristic 0.10 the guard would
	 * condemn the eight-course leaning stack (edge demand 0.24 MPa) AND corbel A, whose free
	 * body overturns its bearing by ~18.5 N.m against the ~17.9 N.m a characteristic bond
	 * restores — breaking the user's bonded-corbel ruling. At the profile's 0.70 corbel A
	 * stands 6.8x on ~125.65 N.m restored, the eight-course stack 2.9x, and the thirty-course
	 * stack still overturns 6.8x — every margin the safe side of the 0.6 the guard used to
	 * carry (5.8x / 2.5x / 7.9x on ~107.7 N.m), which is why this was consistency work rather
	 * than a retune. The whole acceptance window is 18x wide
	 * (LeaningStackAcceptanceTest.cpp), so nothing about the value is delicate — only the
	 * discriminator above is deliberately close to the line, because that is its job.
	 *
	 * KNOWN LIMITATION, STATED RATHER THAN BRANCHED AROUND: the guard credits GENERAL PURPOSE
	 * MORTAR's mean to every bed joint it evaluates, whatever profile that joint actually
	 * carries — a dry-stone or fastened bridge body is still restored by a bond it does not
	 * have. Reading the profile fixes the duplication, not the per-joint blindness. No
	 * fixture cascades such a body, selecting the joint's own strength would be capability no
	 * test covers, and the guard dies at evolution step 4; if such a fixture arrives first,
	 * this must read the joint's own profile before it.
	 */
	const double SolverInterimOverturningMeanBondMPa =
		DestructionProfiles::GeneralPurposeMortar.TensileStrengthMPa;

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
	 * A CENTRE THAT IS NOT FINITE IS REFUSED OUTRIGHT, and it is refused BEFORE the
	 * two-argument door is opened, because that door ADDS. Checked afterwards, a piece
	 * nobody accepted would already have taken a slot, and the tombstone it left would
	 * make "refused" indistinguishable from "added and then removed".
	 *
	 * It is the worse of the two nonsense inputs this overload can be handed. A mass that
	 * is not finite makes an obviously broken load; a centre that is not finite becomes a
	 * lever arm the moment SolveLoads subtracts a joint centroid from it, that arm goes
	 * through the cross product that builds the moment, and the moment reaches
	 * ComputeUtilisation — where every comparison against a NaN is false, so the joint
	 * reads as INTACT. A wall laid with one unplaceable brick would stand, confidently.
	 * Storing the centre and clearing the flag would not do either: the piece would then
	 * read as "nobody said where it is", which is a healthy state.
	 *
	 * ContainsNaN is !FMath::IsFinite on each component in turn, so an infinity lands
	 * inside the guard alongside a NaN, and a broken Y is caught as surely as a broken X
	 * — which matters, because Y is the axis every bed joint in a running-bond wall bends
	 * about. StructureBinding's own check spells the same rule at the layer above; this
	 * one is the door, and that one is now belt and braces.
	 */
	if (CentreOfMassCm.ContainsNaN())
	{
		return INDEX_NONE;
	}

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
	 * WHICH JOINTS TOUCH WHICH PIECE, BUILT ONCE BY WALKING THE CONNECTIONS.
	 *
	 * The tier decision below wants a piece's own joints and nothing else, and it used to
	 * find them by asking every connection in the structure about every piece — pieces x
	 * connections calls to GetJointRole, of which all but the two naming the piece answer
	 * None. The scenario wall is 1,220 pieces against ~3,500 joints, so that is 4.3 million
	 * calls per solve, and a cascade runs a solve per pass: a bottom-course delete measured
	 * 31 passes and about a second and a quarter of visible lag. One walk over the joints
	 * costs what a single piece used to, and the tier loop then reads the six or so a brick
	 * actually has.
	 *
	 * ASCENDING CONNECTION INDEX IS PART OF THE CONTRACT AND NOT AN INCIDENTAL. Appending
	 * in index order to both endpoints leaves each list ordered exactly as the old
	 * connection-major sweep left it, so the support lists, the split and every
	 * floating-point sum built on them reproduce bit for bit. Sorting, filling out of
	 * order, or a hash container whose iteration order differs would each reorder an
	 * accumulation whose last bit decides breaks — the cascade fuzz has five joints
	 * settling at exactly 1.0 and one at a single ulp below it.
	 *
	 * RESERVING IS NOT ON THAT LIST. What breaks the contract is the ORDER things are
	 * appended in, not when the storage is sized, so a count-then-fill layout — one pass
	 * to count each piece's joints, a prefix sum, then a second ascending pass with a
	 * per-piece cursor — fills strictly ascending and preserves this exactly, while
	 * removing the roughly 3,600 small allocations a scenario-scale solve makes here.
	 * That is available whenever the allocation churn is worth measuring.
	 *
	 * AND THE BOUNDS CHECK IS HERE BECAUSE THIS SIDE WRITES. AddConnection refuses a joint
	 * whose ends are not both valid piece handles, which is what entitles GetJointRole to
	 * read a stored connection with no check of its own; an index chosen to select an
	 * element to append to has no such licence, because there the failure is a memory
	 * overwrite rather than a wrong tier. It is a bounds check on a write, not a defence
	 * of the whole path — step three still indexes Loaders by OtherEndOf with no check,
	 * so a stored connection naming an out-of-range piece would reach that instead. Both
	 * are unreachable through the public doors; this one is simply where a write is.
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

	/*
	 * AND WHICH OF THEM HAVE NO SEAT AT ALL, recorded here because this is the loop that
	 * knows. It is the fallback firing, and nothing downstream can tell that apart from a
	 * piece with one seat by looking at the finished list: both come back non-empty. A hole
	 * one brick wide leaves nobody in this set; a wider one leaves the bricks in the middle of
	 * it, which is what ReseatSpannedGroups is for.
	 *
	 * GROUNDED PIECES ARE NOT IN IT, and neither is a tombstone. The earth needs no seat, so a
	 * grounded piece is a perfectly good abutment for a group to push against rather than a
	 * member of one.
	 */
	TArray<bool> PieceHasNoSeat;
	PieceHasNoSeat.Init(false, Pieces.Num());

	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		TArray<int32> HeadConnections;

		for (const int32 Index : PieceJoints[PieceIndex])
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
			PieceHasNoSeat[PieceIndex] =
				Pieces[PieceIndex].bIsInTheStructure && !Pieces[PieceIndex].bIsGrounded;

			SupportConnections[PieceIndex] = MoveTemp(HeadConnections);
		}
	}

	/*
	 * Step one and a half: a run of pieces with no seat between them SPANS the hole rather
	 * than hanging sideways off its edges.
	 *
	 * THE ONLY PLACE IN THE SOLVER THAT READS GEOMETRY TO DECIDE A ROUTE, and it sits ABOVE
	 * the tier rather than inside it — GetJointRole is untouched, and the tier of one joint is
	 * still a fact about one normal and one pairing. See ReseatSpannedGroups for the revision
	 * to MOMENTS_DESIGN's discipline line, and for why the whole pass is a no-op on a
	 * structure nobody placed.
	 *
	 * BEFORE Loaders, BEFORE the fixpoint, and it is the last thing to touch SupportConnections
	 * — every step from here down reads that one list, which is what stops any of them
	 * forgetting the re-seat the way the old solver forgot to filter falling supports.
	 */
	TArray<bool> PieceReseatedOnAnArch;
	TArray<FSpannedArch> Arches;
	ReseatSpannedGroups(
		PieceJoints, PieceHasNoSeat, SupportConnections, PieceReseatedOnAnArch, Arches);

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
		ConnectionCompositeDepthCm.Init(0.0, Connections.Num());
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

		/*
		 * WHAT ARRIVED FROM ABOVE IS A FORCE AND A MOMENT, AND A MOMENT IS ONLY A NUMBER
		 * ABOUT A POINT. This one is about the RECEIVING PIECE'S OWN CENTRE OF MASS, which
		 * is a bookkeeping choice and not a physical one: transfer is transitive, so
		 * re-referencing joint to centre to joint gives bit for bit what re-referencing
		 * joint straight to joint gives, and a per-piece accumulator needs one point per
		 * piece rather than one per joint.
		 *
		 * NOT THE WORLD ORIGIN, deliberately: that is arithmetically valid and numerically
		 * awful, since every entry would be the moment of the whole wall about a point a
		 * wall-length away and each joint's own answer would come back as the difference of
		 * two huge numbers.
		 *
		 * ZERO FOR A PIECE NOBODY PLACED, and nothing is ever added to one — with no centre
		 * there is no point to be about, and the joints of such a piece already answer a
		 * centred load exactly as they did before moments existed.
		 */
		TArray<FVector> ReceivedMomentUuCm;
		ReceivedMomentUuCm.Init(FVector::ZeroVector, Pieces.Num());

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
			 *
			 * AND A PIECE RE-SEATED ONTO AN ARCH IS INDETERMINATE HOWEVER MANY EDGES IT HAS
			 * LEFT, which is the same rule rather than an exception to it. ReseatSpannedGroups
			 * only routes a group that something seated stands on BOTH sides of, so the reaction
			 * to an unseated brick's weight is shared between two abutments; the one head joint
			 * left in its load path is the bookkeeping route for the vertical share, not a claim
			 * that the brick hangs off that joint alone. Treated as determinate it would carry
			 * its whole column across the 11.25 cm to that joint's centroid — MOMENTS_DESIGN
			 * case (b) multiplied by twenty-eight brick weights, about 11.6 of capacity — and
			 * the head joint would snap, so the arch would fail for a NEW reason having just
			 * been granted.
			 */
			const bool bLoadPathIsDeterminate = LoadPaths[Current].Num() == 1
				&& Pieces[Current].bHasCentreOfMass && !PieceReseatedOnAnArch[Current];

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
					 * AND IT IS NO LONGER THE WHOLE OF WHAT A SPRINGING CARRIES.
					 * ApplyArchingThrust runs once this accumulation has settled and adds a
					 * HORIZONTAL component at the two abutments of every spanned opening —
					 * gravity is still what this loop routes, and the arch is a second,
					 * sideways load the same joint has to take in shear. Every joint no arch
					 * touches keeps exactly the vector built here, bit for bit.
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
					 * THE SAME SHARE AS A PHYSICAL FORCE — straight down, whichever end of
					 * the joint the producer named first. Every moment below is built from
					 * this rather than from the stored vector, so the accumulation reasons
					 * about one direction for gravity and the declaration-order sign is
					 * applied once, where the answer is stored.
					 */
					const FVector ShareWeightUu(0.0, 0.0, -ShareUU);

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

					/*
					 * WHAT THIS JOINT CARRIES, ABOUT ITS OWN CENTROID: everything that
					 * arrived from above, carried down and re-referenced, plus this piece's
					 * own weight about the same point. Written physically, with the
					 * declaration-order sign put back on at the store below — that force
					 * already carries it, so crossing with the stored vector would flip BOTH
					 * the lever arm and the force and come out the same size, which is the
					 * invariance the classification already has.
					 *
					 * (c_from - c_to) x F IS ORDINARY VARIGNON, and it is what makes a moment
					 * mean anything as it travels: the received load kept its own lever arm
					 * instead of being treated as though it had been placed neatly on this
					 * piece's middle. That older rule was exact for a piece carrying only
					 * itself and silently wrong for a corbel carrying a wall, and it vanished
					 * entirely wherever a chain stacked squarely — which is why nothing that
					 * stacks squarely moves now.
					 *
					 * THE MOMENT RIDES ALONGSIDE THE FORCE AND DOES NOT CHANGE IT. The split
					 * is still area-weighted and gravity still points straight down; a longer
					 * or tilted force vector would encode the moment in the one number a
					 * readout uses to explain the load, which is how a plausible quantity
					 * stops describing what is happening.
					 */
					FVector MomentAboutJointUuCm = FVector::ZeroVector;

					if (bLoadPathIsDeterminate && bJointKnowsItsFace)
					{
						MomentAboutJointUuCm = ReceivedMomentUuCm[Current]
							+ FVector::CrossProduct(
								Pieces[Current].CentreOfMassCm - Connection.InterfaceCentreCm,
								ShareWeightUu);

						/*
						 * AND A SEAT WITH SOMETHING TO PUSH AGAINST ARCHES RATHER THAN
						 * CANTILEVERING. Delete one brick from a running-bond wall and the
						 * brick above keeps exactly one seat, overhangs it by 5.625 cm, and
						 * reads 1.63 of capacity in tension — so it goes, leaving two more
						 * half-seated bricks in the course above and a failure that walks
						 * across the wall at 33.69 degrees, one step per course. What is
						 * missing from that picture is the intact head joint into the hole:
						 * the two bricks either side lean on each other, the thrust line
						 * runs through the opening instead of peeling the seat open, and the
						 * same joint reads 0.0142 on the COMPRESSION axis. ARCHING_DESIGN.md.
						 *
						 * THE FOUR GATES ARE SPLIT ACROSS TWO OBJECTS BECAUSE THEY ARE TWO
						 * KINDS OF FACT. That the load is compressive and outside the kern is
						 * arithmetic on one face and belongs to the joint; that this is a bed
						 * joint beneath a placed piece with an abutment on the overhanging
						 * side is a statement about the graph and belongs here.
						 *
						 * THE ORDER IS THE CHEAP TEST FIRST. The tier and the joint's own two
						 * gates cost a normalise and a handful of multiplies; only a joint
						 * that would actually be relieved pays for the walk over the piece's
						 * neighbours. In an intact wall every seat has e = 0 exactly, so the
						 * moment is zero, the relief is exactly 1, and nothing below the
						 * first condition is ever reached.
						 *
						 * THE FORCE HANDED OVER IS THE STORED ONE, declaration-order sign and
						 * all, because "compressive" is a fact about the joint and only the
						 * force oriented the way ClassifyForce demands can state it. The
						 * moment goes in physically oriented, which costs nothing: only the
						 * magnitude of each in-plane component is read.
						 */
						if (GetJointRole(Index, Current) == EJointRole::BedBeneath)
						{
							const double ArchingRelief = Connection.ArchingMomentScale(
								ConnectionForces[Index], MomentAboutJointUuCm);

							if (ArchingRelief < 1.0)
							{
								/*
								 * AND THE COUPLE THE CAP WOULD DELETE IS WHAT SOMETHING ELSE HAS
								 * TO SUPPLY, so it is handed over with the gates rather than
								 * left implicit. Capping the moment vector by k removes (1 - k)
								 * of it, and an arch is only an arch if the thrust that replaces
								 * it can actually be delivered — HasArchingAbutment measures
								 * that against the seat's own sliding capacity and refuses to be
								 * an abutment where it cannot. DESIGN.md §7 gap 4.
								 *
								 * THE WHOLE VECTOR'S MAGNITUDE, WHICH IS THE CONSERVATIVE
								 * READING. A horizontal pair supplies the in-plane part; a
								 * component about the seat's own normal would be torsion, which
								 * DESIGN.md §5.3 does not model and which gravity on an
								 * axis-aligned rectangle never produces. Where the two spellings
								 * differ this one asks for MORE thrust, so the difference can
								 * only ever withhold a relief, never grant one.
								 *
								 * INSIDE THE RELIEF TEST, which keeps the paragraph above honest:
								 * an intact wall's seats are relieved by exactly 1 and never pay
								 * for the square root.
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
							 * AND WHAT RESISTS WHAT IS LEFT IS NOT ONE BED PATCH. A stack of
							 * courses over a lost support acts as a DEEP BEAM: the plane taking
							 * the overturning moment is a vertical section through the bonded
							 * masonry standing over this joint, t*D^2/6, against the patch's own
							 * 179.48 cm3 — eleven courses is a factor of sixty-five, and it is
							 * what decides whether a brick deleted at a free end takes the wall
							 * with it. ARCHING_DESIGN.md slice 5, and the user's own ruling.
							 *
							 * THE DEPTH IS MEASURED AND THE MOMENT IS NOT TOUCHED. Composite
							 * action changes the SECTION the moment is read against, never what
							 * the wall hands down — so the ladder below this joint carries
							 * exactly what it carried before, and only the joint's own
							 * utilisation moves. Scaling the moment instead would relieve every
							 * joint under this one as well, by a factor nobody derived.
							 *
							 * A BED JOINT AND A REAL MOMENT, WHICH IS BOTH THE GATE AND THE
							 * BUDGET. A head joint is not a bed plane and has no masonry
							 * standing over it in this sense — MOMENTS_DESIGN case (b) is
							 * exactly that shape and must not move — and a joint carrying no
							 * moment has nothing for a second section to resist, so the walk is
							 * skipped and the answer is identical either way. Between them, an
							 * intact wall pays for none of this: every seat has e = 0 exactly,
							 * so no joint in it reaches this line at all.
							 *
							 * AND THERE HAS TO BE A STACK. A piece with nothing resting on it is
							 * ONE UNIT, and one unit is not a composite of anything — "a stack of
							 * courses over a lost support" is the mechanism, so the definition of
							 * it is the gate rather than a refinement of it. The topmost rung of
							 * every corbel in the suite is exactly that case and it reads its own
							 * bed patch, which is also what the arithmetic would have given: one
							 * course of depth is a SHALLOWER section than the patch, so the
							 * relief was never going to fire there anyway. What the gate actually
							 * buys is refusing it for a lone overhanging brick that happens to be
							 * taller than its own seat is deep, where the formula alone would
							 * offer help no deep beam is there to supply.
							 *
							 * AND THE WALL IS NOT THE ONLY THING THAT CAPS THE DEPTH. The walk
							 * stops where the masonry stops and at any joint that has GIVEN, so a
							 * raking cut, a missing course and a broken bond each shorten it — but
							 * asking it for everything there is says a corbel is helped by masonry
							 * it has no part in. The moment bending a cut's bottom rung is set by
							 * the CUT and the depth by the WALL, so the two stop cancelling the
							 * moment the wall is taller: the same eleven-step cut read 0.223 under
							 * forty courses against 0.369 under thirteen, MORE load for LESS
							 * utilisation. COMPOSITE_DEPTH_DESIGN.md.
							 *
							 * SO THE SECOND CAP IS THE JOINT'S OWN EFFECTIVE LEVER ARM, e = |M|/|F|,
							 * which is a length the solver has already accumulated — no new field,
							 * no profile column and no per-material branch. A deep beam over a hole
							 * reaches about as far up as the load it is carrying reaches out, and
							 * for a k-step corbel e comes to about half the corbel's own depth, so
							 * lambda*e is roughly 1.73 times that depth and the cap does NOT fire
							 * where a wall stops at the top of its corbel. It fires on the wall the
							 * game renders, which is the case no fixture had.
							 *
							 * AND THE DIVISION IS GUARDED, IN THE DIRECTION THAT CREDITS LESS. A
							 * force at or near zero makes the arm enormous or non-finite, and an
							 * infinite permitted depth is exactly the whole wall this is here to
							 * refuse. Tested positively, so a NaN arm — which every comparison
							 * rejects — lands outside the relief and the joint keeps its own bed
							 * patch. A joint reading as intact when it should read as failed is the
							 * expensive way to be wrong.
							 *
							 * BUT THE ARM MAY ONLY TRIM THE MASONRY ABOVE THE CUT, NEVER THE
							 * CORBELLING BODY ITSELF — SO THE BODY'S OWN DEPTH IS A FLOOR UNDER
							 * IT. The corbelling courses are what GENERATE the moment: they are
							 * bonded into one cantilevering body and need no shear transfer to be
							 * engaged, so they resist with their full depth unconditionally.
							 * Masonry above the cut is a different thing — it is not being bent by
							 * the corbel's moment and has to be dragged into the section by shear
							 * over a distance, which is what `lambda*e` bounds. Applying the arm
							 * to the whole depth taxed the corbel for its own height and made a
							 * SHALLOWER corbel read HIGHER than a deeper one, because the section
							 * shrank with the step faster than the moment grew with it.
							 *
							 * AND THE FLOOR CAN NEVER CREDIT A SINGLE COURSE OF THE WALL ABOVE THE
							 * CUT, BECAUSE THE FLOOR IS THE CUT. CorbellingBodyDepthCm stops at
							 * the first course that is not corbelling, which is the first course
							 * of the wall standing over the body — an exact structural guarantee,
							 * not a bound that happens to hold here.
							 *
							 * MAX, WRITTEN AS `GREATER THAN` SO THE FLOOR IS WHAT A NaN LOSES.
							 * FMath::Max is `(B < A) ? A : B` (GenericPlatformMath.h) and every
							 * comparison against a NaN is false, so it DISCARDS a NaN first
							 * argument and would silently substitute a plausible number for a
							 * fault; spelled out, a body depth that is not a number falls through
							 * to `lambda*e` and the joint reads what slice 1 gave it, which is the
							 * higher of the two readings.
							 *
							 * THE ARM IS A LIMIT ON THE WALK AND A CAP ON ITS ANSWER, AND IT HAS
							 * TO BE BOTH. MasonryDepthAboveCm stops once it has ENOUGH, so it
							 * returns the first whole number of courses that REACHES the limit and
							 * overshoots it by up to one — on the scenario corbel 202.5 cm against
							 * a permitted 200.77, a section 1.7% larger than the rule allows, in
							 * the permissive direction and invisible to a one-sided property.
							 * Slice 4 takes the same pair of steps with the arching angle, and for
							 * the same reason.
							 *
							 * WRITTEN AS `LESS THAN` RATHER THAN AS FMath::Min, which is slice 4's
							 * spelling as well: Min is `(A < B) ? A : B` (GenericPlatformMath.h),
							 * so it discards a NaN
							 * first argument and silently keeps the cap. The walk cannot return a
							 * NaN — its own rise tests are `!(x > 0.0)`, so geometry it cannot read
							 * leaves it at zero — and zero takes this branch and withholds the
							 * relief, which is the direction to be wrong in.
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
					 * AND IT IS HANDED ON AS THE PAIR IT IS: a force through this joint, and
					 * exactly the moment stored on this joint, re-referenced to the receiving
					 * piece's own centre. WHAT TRAVELS IS WHAT THE JOINT READS — there is no
					 * second, private quantity — so a joint whose statics is indeterminate
					 * transmits a moment of zero ABOUT ITSELF rather than nothing at all, and
					 * the load below it still knows it came through that patch and not
					 * through the middle of the piece it landed on.
					 *
					 * BOTH ENDS HAVE TO BE PLACED. The joint supplies the point the moment is
					 * currently about and the support supplies the point it is being moved to;
					 * with either missing there is nothing to measure the transfer against,
					 * and inventing one would put the world origin into a lever arm.
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
	 * Step six: AN ARCH PUSHES SIDEWAYS, and the springing has to carry that too.
	 *
	 * OUTSIDE THE FIXPOINT BECAUSE IT READS ITS ANSWER. The thrust is a fraction of the load
	 * the abutments' own seats have already been given, so it can only be computed once the
	 * accumulation has settled — and it feeds nothing back, since a horizontal force changes no
	 * support list, no split, no accumulation order and no moment. That is what lets the whole
	 * vertical answer of the structure stay bit-identical to one computed before arches existed.
	 */
	ApplyArchingThrust(PieceJoints, Arches);

	/*
	 * Nothing is evaluated against a strength here. Solving computes what each
	 * joint carries and must leave every connection exactly as intact as it found
	 * it: FConnection::ApplyForce latches, so calling it would break joints as a
	 * side effect of asking what they carry and make a solve unrepeatable.
	 */
}

void FStructure::ReseatSpannedGroups(
	const TArray<TArray<int32>>& PieceJoints,
	const TArray<bool>& PieceHasNoSeat,
	TArray<TArray<int32>>& SupportConnections,
	TArray<bool>& PieceReseatedOnAnArch,
	TArray<FSpannedArch>& Arches) const
{
	/*
	 * SIZED BEFORE THE GATE, so every caller downstream may index it without asking whether
	 * this pass ran. An all-false array is exactly what "no group formed" means, and it is the
	 * same answer a structure nobody placed gets.
	 */
	PieceReseatedOnAnArch.Init(false, Pieces.Num());
	Arches.Reset();

	/*
	 * THE GEOMETRY GATE, AND IT IS LOAD-BEARING RATHER THAN DEFENSIVE. Deciding that a run of
	 * bricks over a hole is an arch rather than a chain of hangers is a ROUTING decision, and it
	 * cannot be made without knowing where the hole is — which is why ARCHING_DESIGN revises
	 * MOMENTS_DESIGN's discipline line here rather than quietly bending it. What the revision
	 * buys is this one line: with no positions there is no group, so the whole pass is a no-op
	 * and a geometry-free structure routes bit for bit as it always did.
	 *
	 * BOTH FUZZ GENERATORS EMIT NO GEOMETRY — 20,000 cases between them, and the only property
	 * tests over routing this project has. An arch that could fire without positions would set
	 * every one of them against an oracle that has never heard of one, and they would go dark
	 * quietly rather than failing.
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
	 * WHAT "CONTIGUOUS" MEANS, WRITTEN ONCE AND ASKED FOUR TIMES: the piece across an INTACT
	 * HEAD JOINT from this one, or INDEX_NONE where this joint is not one.
	 *
	 * Head joints and nothing else. A bed joint to a seatless piece is the tier that has
	 * already failed to hold anybody up, and following one would fuse the courses above and
	 * below a hole into a single group spanning nothing. A joint that has given conducts
	 * nothing at all, so it cannot make two pieces one group either — GetJointRole keeps
	 * answering for a severed joint, deliberately, so that has to be asked here.
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
		 * THE ABUTMENTS: the seated pieces the group pushes against, one head joint away. A
		 * member touching one is a hop from the ground and seeds the walk inward.
		 *
		 * EACH ABUTMENT ONCE, however many members touch it. The direction test below is
		 * unmoved by a duplicate — two copies of one vector can only agree with each other —
		 * but the thrust divides itself among the abutments at each end, and an abutment
		 * counted twice would take twice its share of it.
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
		 * AND THE GROUP ONLY SPANS IF SOMETHING SEATED STANDS ON BOTH SIDES OF IT. One abutment
		 * is a cantilever however many bricks long it is, and granting it would hang a wall's
		 * whole free end off a joint that has nothing to thrust into — which is the permissive
		 * failure ARCHING_DESIGN names, and the reason a wall's free vertical end has to keep
		 * today's answer.
		 *
		 * OPPOSITE SIDES IS A NEGATIVE DOT PRODUCT ABOUT THE GROUP'S OWN CENTRE, which needs no
		 * axis to be nominated and so says the same thing for a wall laid along X, along Y or
		 * at forty degrees to both. Written as a positive test, so a NaN anywhere in either
		 * direction leaves the group unabutted rather than spanning: a hole that quietly stops
		 * being a hole is the expensive way to be wrong here.
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
		 * THE RE-SEAT, AND IT IS ACYCLIC BY CONSTRUCTION. A member keeps only the head joints
		 * that take it strictly CLOSER to an abutment, so every remaining edge runs from a
		 * longer path to a shorter one and no walk can return to where it started. That is what
		 * separates this from the naive arch ARCHING_DESIGN's trap 1 describes: making the
		 * neighbour a support outright puts two bricks over a hole in a two-node cycle,
		 * LoadReturnsToPiece strands the pair, and the wall comes down for a NEW reason.
		 *
		 * ASCENDING JOINT INDEX SURVIVES, because PieceJoints is ascending and this filters it
		 * in place rather than sorting anything. The whole accumulation downstream is a
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
		 * AND THE OPENING IS RECORDED AS AN ARCH, WITH ITS TWO ENDS TOLD APART. What the thrust
		 * pass needs and this loop is the only place that knows is which abutments face each
		 * other across the hole: H is one number for the whole span, pushed out at both ends at
		 * once, and a pass that could not tell the ends apart would have nothing to make equal
		 * and opposite.
		 *
		 * THE FIRST ABUTMENT'S OWN DIRECTION IS THE AXIS, which nominates no world axis and so
		 * says the same thing for a wall laid along X, along Y or at forty degrees to both. The
		 * sides then fall out as the sign of a projection onto it, and an abutment square on to
		 * that axis is on NEITHER side and is dropped — written as two positive tests so a NaN
		 * leaves it out rather than assigning it to whichever branch happens to be the else.
		 *
		 * A DIRECTION THAT WILL NOT NORMALISE MEANS AN ABUTMENT SITTING EXACTLY ON THE GROUP'S
		 * OWN CENTRE, which describes no span, so there is no arch here to thrust — the re-seat
		 * above stands either way, which is slice 2's answer and is unaffected.
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
		 * BOTH ENDS OR NEITHER. bAbutsOnBothSides above already found a pair of abutments in
		 * opposition, so this cannot be false — it is asked because the thrust pass may not be
		 * the thing that discovers a one-ended arch, which is trap 2 wearing the clothes of a
		 * refactor.
		 */
		if (Arch.Abutments[0].Num() > 0 && Arch.Abutments[1].Num() > 0)
		{
			/*
			 * AND L IS HOW FAR THE TWO ENDS STAND APART, one mean abutment centre per end.
			 * Slice 3 never needed it because `d_e = 0.866*L` cancelled the span out of the
			 * thrust ratio entirely; capping `d_e` by the cover puts it back, and the abutments'
			 * own positions give it with no new query — for a running-bond wall each springing
			 * keeps half a cell of bearing, so the two centres are the clear opening apart to
			 * the centimetre.
			 */
			EndCentreCm[0] /= static_cast<double>(Arch.Abutments[0].Num());
			EndCentreCm[1] /= static_cast<double>(Arch.Abutments[1].Num());

			Arch.SpanCm = (EndCentreCm[0] - EndCentreCm[1]).Size();

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
		 * THE SEATS THE ARCH DELIVERS ITSELF THROUGH, and what they are already carrying. The
		 * thrust arrives at the abutment and leaves through the same patch its weight does,
		 * which is why the springing plane is the critical one: the demand is constant with
		 * depth while the friction that resists it grows with the weight above.
		 *
		 * THE SIGN OF EACH SEAT IS RECORDED HERE RATHER THAN RE-DERIVED BELOW. ConnectionLoad's
		 * convention is that a joint's force is the force acting on PieceB, so a joint naming
		 * the abutment second stores the push as given and one naming it first stores the
		 * equal-and-opposite reaction. Get it backwards and the two ends of an arch pull
		 * together instead of pushing apart, which is a perfectly plausible-looking wall.
		 */
		TArray<int32> Seats[2];
		TArray<double> SeatSign[2];
		double SeatAreaSqCm[2] = { 0.0, 0.0 };

		/*
		 * HOW DEEP THE ARCH MAY BE IF ONLY THE ANGLE HAD A SAY, and it is also how far up the
		 * cover walk below has to bother looking: past this much masonry the angle governs and
		 * the exact cover changes no answer.
		 */
		const double AngleCappedDepthCm = SolverArchingDepthPerSpan * Arch.SpanCm;

		/*
		 * THE THINNEST COVER EITHER END STANDS UNDER, and ONE NUMBER FOR THE WHOLE ARCH.
		 *
		 * A cover measured per abutment and applied per abutment is trap 2 wearing a new hat:
		 * the two ends of one opening would disagree about d_e, push each other by different
		 * amounts, and hand the structure a net horizontal force out of nowhere while every
		 * joint still read plausibly. Reducing the two measurements to one before anything is
		 * pushed makes the equal-and-opposite property structural rather than lucky.
		 *
		 * THE THINNEST RATHER THAN THE MEAN, because thin cover is the direction that fails —
		 * an arch is only as good as its shallower haunch, and taking the deeper one would be
		 * the permissive reading of exactly the defect this slice exists to fix.
		 */
		double CoverCm = TNumericLimits<double>::Max();

		/*
		 * W IS THE WHOLE LOAD THE ARCH PUTS ON ITS ABUTMENTS, springings' own columns included,
		 * and ARCHING_DESIGN is explicit that it is not a triangle. Taking the re-seated group's
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
					 * a severed joint, deliberately, so it has to be asked here.
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
				 * WRITTEN AS `NOT AT LEAST AS DEEP` RATHER THAN AS A `Min`, so that a cover which
				 * came back NaN is taken rather than discarded. Every comparison against a NaN is
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
		 * BOTH ENDS HAVE TO BE ABLE TO TAKE IT, OR NEITHER IS PUSHED. An end whose abutments
		 * are grounded — resting on the earth rather than on a bed joint — has no seat here to
		 * deliver into, and thrusting only the other end would give the structure a net
		 * horizontal force out of nowhere while every joint still read plausibly. That is
		 * ARCHING_DESIGN's trap 2 and it is the one thing no per-joint check could catch.
		 *
		 * Every guard is written as a positive test, so a NaN area or a NaN load leaves the
		 * arch unthrust rather than laundering into a plausible-looking sideways force.
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
		 * A SPAN AND A COVER THAT MEAN NOTHING LEAVE THE ARCH UNTHRUST, which is the same answer
		 * a degenerate area or a degenerate load already gets a few lines up. Both guards are
		 * positive tests, so a NaN lands inside them; and neither is reachable from a wall
		 * anyone laid, since an abutted group has two abutments a real distance apart and the
		 * spanning course is itself a course of cover.
		 */
		if (!(Arch.SpanCm > 0.0) || !FMath::IsFinite(Arch.SpanCm)
			|| !(CoverCm > 0.0) || !FMath::IsFinite(CoverCm))
		{
			continue;
		}

		/*
		 * THE ARCHING DEPTH, AND IT IS A `min` RATHER THAN A REPLACEMENT. BS 5977-1's angle says
		 * how deep an arch may be; the masonry actually standing over the opening says how deep
		 * it can be. Whichever is smaller is what there is to work with, so a deeply buried
		 * opening is governed by the angle and reads the same however much more wall is piled on
		 * it, while a shallow one is governed by what little it has.
		 *
		 * HELD AS d_e/L RATHER THAN AS d_e, AND THAT IS ARITHMETIC AND NOT TIDINESS. The thrust
		 * only ever depends on the ratio — H = 3*W*L/(8*d_e) is 3*W/(8*(d_e/L)) — and where the
		 * ANGLE governs, d_e/L is the constant itself, so that expression is character for
		 * character the one slice 3 shipped and every answer slice 3 pinned is bit-identical.
		 * Dividing 0.866*L back out of L instead loses the cancellation: IEEE multiplication is
		 * not exact, so L/(0.866*L) is 0.866 only to within a rounding, and the dry-stone
		 * springing moves in its last three digits for no reason anybody chose.
		 *
		 * WRITTEN OUT RATHER THAN AS FMath::Min BECAUSE THE COMPARISON WORKS AGAINST US. Min is
		 * `(A < B) ? A : B` (GenericPlatformMath.h), and every comparison against a NaN is false,
		 * so a NaN cover in the first argument would
		 * be silently REPLACED by the angle's answer — the permissive direction, and the exact
		 * defect this slice exists to remove. The guard above has already refused a NaN, and
		 * this is written so it would not matter if it had not.
		 */
		const double DepthPerSpan = CoverCm < AngleCappedDepthCm
			? CoverCm / Arch.SpanCm
			: SolverArchingDepthPerSpan;

		if (!(DepthPerSpan > 0.0))
		{
			continue;
		}

		/*
		 * H = W*L/(8r) WITH r = d_e/3, SO H = 3*W/(8*(d_e/L)). The kern-limited rise is a fixed
		 * fraction of the DEPTH rather than of the span, which is what makes the cover matter at
		 * all: H climbs as 1/d_e while V does not move, so the thrust ratio H/V = 3L/(4*d_e)
		 * blows up as the masonry over an opening thins. Where the angle governs — a narrow hole
		 * under deep cover — d_e/L is 0.866, the span drops out of the ratio again and it settles
		 * at the constant 3/(4*0.866) = 0.866 slice 3 measured everywhere.
		 *
		 * ONE NUMBER FOR THE WHOLE ARCH, pushed out at both ends at once, which is what makes
		 * trap 2 exact rather than nearly exact.
		 */
		const double ThrustUu = 3.0 * TotalVerticalUu / (8.0 * DepthPerSpan);

		for (int32 End = 0; End < 2; ++End)
		{
			/*
			 * ONE DIRECTION, TWO SIGNS, so the two ends sum to exactly zero rather than to a
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
	 * WHAT STANDS ON THIS PIECE IS WHATEVER THE GRAPH SAYS RESTS ON IT — BedAbove is that
	 * relation exactly — so this costs a walk over the six or so joints a brick has and needs
	 * no broadphase, no octree and no world. A joint that has GIVEN conducts nothing and holds
	 * nothing up, so it is not a course of masonry either; GetJointRole keeps answering for a
	 * severed joint, deliberately, so it has to be asked.
	 *
	 * THE FIRST BY ASCENDING JOINT INDEX, WHICH IS A CHAIN AND NOT A TRAVERSAL. Running bond
	 * puts two pieces over each brick and in a wall of uniform height both columns reach the
	 * same place; a stepped or gabled wall would have its answer decided by which one this
	 * took, and nothing tests that yet.
	 *
	 * TWO CALLERS, AND THE SECOND IS WHY IT IS A FUNCTION. MasonryDepthAboveCm takes this step
	 * once per course, and SolveLoads asks it ONCE to find out whether there is a stack over a
	 * joint at all — a piece with nothing on it is one unit rather than a composite of several,
	 * and one unit is not a deep beam. Two transcriptions of "what rests on this" would agree
	 * until the day a severed joint was handled in one of them.
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
	 * THE COURSE THE JOINT IS UNDER COUNTS, AND IT IS THE FIRST OF THEM RATHER THAN SOMETHING
	 * RESTING ON ONE — so the shallowest depth a wall can offer is one course and never zero.
	 * For the arch that is the spanning course, the first ring, so an opening cut in the top
	 * course of a wall still has a ring to arch in; for the composite section it is the
	 * corbelled brick itself, which is the only masonry over its own seat.
	 *
	 * ITS DEPTH IS A COURSE PITCH AND NOT A BRICK HEIGHT. What the wall works through is the
	 * masonry from one bed plane to the next, mortar included, which is exactly the rise from
	 * the piece below the joint to the piece standing on it. Taking the unit's own height
	 * instead drops the joints and reads about 13% shallow on standard brickwork, which is a
	 * plausible enough number to survive a review and is wrong on every row.
	 */
	const double FirstCourseRiseCm =
		Pieces[Piece].CentreOfMassCm.Z - Pieces[Seat].CentreOfMassCm.Z;

	if (!(FirstCourseRiseCm > 0.0))
	{
		return 0.0;
	}

	/*
	 * AND THE WALK IS BOUNDED TWICE OVER. Past EnoughDepthCm no further course can change the
	 * caller's answer — 0.866*L for the arch, where the angle takes over, and lambda*|M|/|F| for
	 * the composite section, where the deep beam stops reaching. Both are at most
	 * ceil(EnoughDepthCm / course pitch) steps, so neither caller walks a whole wall it will not
	 * use: about five courses for a free end and about twenty-seven for the scenario corbel. The
	 * piece count is the second bound and is pure defence — a graph whose normals claim A is
	 * above B and B above A would otherwise walk for ever, and a structure with complete geometry
	 * is the only thing that reaches here.
	 *
	 * COMPARED AS A DOUBLE, so a vanishing course pitch produces an enormous bound rather than
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
		 * MEASURED RISE BY RISE RATHER THAN COUNTED AND MULTIPLIED, so that courses of unequal
		 * depth add up to what they are instead of to a multiple of the first one. A step that
		 * does not rise is not a course, and stopping on it is the fail-closed direction: less
		 * cover is more thrust, and a joint that reads as intact when it should read as failed
		 * is the expensive way to be wrong here.
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
	 * THE BODY'S FIRST COURSE IS THE PIECE STANDING ON THE JOINT, AND IT IS NOT ASKED WHETHER IT
	 * IS CORBELLING. It is the course whose overhang generated the moment being read, so it is
	 * the cut by construction; the walk above it is what has to be justified. Measured as a
	 * course pitch from the seat rather than as a unit height, for the reason MasonryDepthAboveCm
	 * gives — the wall works through the mortar as well as through the brick.
	 */
	const double FirstCourseRiseCm =
		Pieces[Piece].CentreOfMassCm.Z - Pieces[Seat].CentreOfMassCm.Z;

	if (!(FirstCourseRiseCm > 0.0))
	{
		return 0.0;
	}

	/*
	 * SEATED ON EXACTLY ONE COURSE. Counted over the piece's own joints, excluding anything that
	 * has GIVEN and anything resting on a piece that has left the structure — the same two
	 * exclusions PieceRestingOn makes, and for the same reason: neither is masonry the body can
	 * be bonded into.
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
	 * THE PIECE COUNT IS THE BOUND, AND IT IS PURE DEFENCE. Every step rises by a positive amount
	 * and no piece can be visited twice on a strictly rising chain, so a graph whose normals are
	 * consistent stops of its own accord; this is here so that one whose normals are not cannot
	 * walk for ever.
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
		 * MEASURED RISE BY RISE, AND A STEP THAT DOES NOT RISE IS NOT A COURSE. Written
		 * !(x > 0.0) so that geometry nobody measured leaves the body at the depth it has
		 * already earned rather than adding a NaN to it — a shallower body credits less
		 * section, which is the direction to be wrong in.
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
	 * THE SEAT'S OWN PLANE IS WHAT THE SIDES ARE MEASURED IN, so a normal that will not
	 * normalise has no sides and cannot abut anything. Nothing that reaches here can fail
	 * this — the caller has already had a tier and a moment out of the same joint — and it
	 * is here so that the projections below are never fed a direction nobody chose.
	 */
	FVector UnitNormal = BedJoint.InterfaceNormal;

	if (!UnitNormal.Normalize())
	{
		return false;
	}

	/*
	 * WHICH WAY THE PIECE OVERHANGS: where its centre of mass sits relative to the centroid
	 * of the patch it has left, flattened into that patch's plane. On a bed joint the plane
	 * is horizontal and this is the 5.625 cm a half-seated running-bond brick leans by; the
	 * projection is what keeps the two courses' worth of height between the two points out
	 * of a comparison that is only ever about sideways.
	 */
	const FVector EccentricCm = FVector::VectorPlaneProject(
		Pieces[PieceIndex].CentreOfMassCm - BedJoint.InterfaceCentreCm, UnitNormal);

	/*
	 * HOW HARD THIS SEAT MAY BE PUSHED SIDEWAYS, MPa, AND IT IS BOUGHT WITH THE SEAT'S OWN
	 * SQUEEZE. The couple the cap deletes leaves through this patch as shear, so what limits it
	 * is the Mohr-Coulomb envelope ComputeUtilisation already measures every other sliding
	 * demand against — the bond, plus whatever friction the mean compressive stress is worth.
	 * Read off the force rather than off the profile's name: a joint with no cohesion earns
	 * exactly what its friction earns, which is what makes dry stone unable to flat-arch without
	 * one line anywhere saying so.
	 *
	 * ONE CONVERSION, THE NAMED ONE. The force is uu and the strengths are MPa, so the area
	 * carries the 10000 exactly once and the comparison below is stress against stress.
	 */
	const FConnectionLoad SeatLoad = DestructionForce::ClassifyForce(SeatForceUu, UnitNormal);

	const double SeatCompressionMPa = SeatLoad.Compression
		/ (BedJoint.InterfaceAreaSqCm * DestructionForce::ForceUnitsPerMPaSqCm);

	const double CohesionAndFrictionMPa = BedJoint.Strength.ShearCohesionMPa
		+ BedJoint.Strength.FrictionCoefficient * SeatCompressionMPa;

	/*
	 * THE TRUNCATION IS WRITTEN OUT RATHER THAN AS FMath::Min, AND THE ORDER OF THE COMPARISON
	 * IS THE WHOLE REASON. Min is `(A <= B) ? A : B` (GenericPlatformMath.h) and every comparison
	 * against a NaN is false, so a NaN capacity handed to it as the first argument would be
	 * silently REPLACED by the profile's ceiling — which for an unset ceiling is
	 * TNumericLimits<double>::Max(), an arch afforded by arithmetic nobody can read. Asking
	 * whether the CEILING is the smaller one keeps a NaN, and a NaN capacity refuses the relief
	 * at the guard below. Unreachable today, since the caller has already had a finite normal
	 * stress out of this same joint; spelled so that it would not matter if it had not.
	 */
	const double SlidingCapacityMPa =
		BedJoint.Strength.MaxShearStrengthMPa < CohesionAndFrictionMPa
			? BedJoint.Strength.MaxShearStrengthMPa
			: CohesionAndFrictionMPa;

	for (const int32 Index : PieceJoints[PieceIndex])
	{
		const FConnection& Head = Connections[Index];

		/*
		 * A joint that has given conducts nothing, so it cannot deliver a thrust either —
		 * the same rule the tier decision applies, and for the same reason. GetJointRole
		 * still answers for a severed joint, deliberately, so this has to be asked here.
		 */
		if (Head.HasGiven() || GetJointRole(Index, PieceIndex) != EJointRole::Head)
		{
			continue;
		}

		/*
		 * AND THE HEAD JOINT HAS TO KNOW WHERE IT IS. Its centroid is what says which side
		 * it is on, and an unmeasured face carries a zero that means "nobody said" rather
		 * than a plane through the world origin — which on a wall laid anywhere else would
		 * answer the side question with the direction of the origin.
		 */
		if (Head.InterfaceHalfExtentCm.IsZero())
		{
			continue;
		}

		const FVector TowardAbutmentCm = FVector::VectorPlaneProject(
			Head.InterfaceCentreCm - Pieces[PieceIndex].CentreOfMassCm, UnitNormal);

		/*
		 * ON THE ECCENTRIC SIDE, and a joint square on to the overhang is not on either
		 * side. Written as a positive test so that an exact zero — and a NaN — falls out
		 * here rather than being counted as an abutment.
		 */
		if (!(FVector::DotProduct(EccentricCm, TowardAbutmentCm) > 0.0))
		{
			continue;
		}

		const int32 Abutment = OtherEndOf(Head, PieceIndex);

		/*
		 * IT HAS TO REACH THE GROUND ON ITS OWN ACCOUNT, and both halves of that are needed.
		 * PieceSupported is the walk from the earth, so it covers Grounded and Supported
		 * together and excludes anything falling or stranded; the second test is trap 3, and
		 * it is what tells a real arch apart from two bricks propping each other over open
		 * air. Both of those are Supported and both have an intact head joint to the other,
		 * so only the support relation separates them.
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
		 * AND A NEIGHBOUR RE-SEATED ONTO A SPANNING GROUP LEANS ON US ONLY BECAUSE SOMETHING
		 * BEYOND IT IS CARRYING. That mark is written by ReseatSpannedGroups and by nothing
		 * else, and it is written only for a group with a seated abutment on BOTH sides — so
		 * where it is set the thrust line runs on through the group to a reaction rather than
		 * stopping in mid-air, which is the one fact that separates a spanned opening from the
		 * two bricks propping each other that the test above exists to refuse. Without it this
		 * reads exactly as it did before groups existed.
		 */
		if (bAbutmentLeansOnUs && !PieceReseatedOnAnArch[Abutment])
		{
			continue;
		}

		/*
		 * A SPANNED GROUP IS CHECKED BY BEING PUSHED, SO IT IS NOT CHECKED HERE. Where that mark
		 * is set, ApplyArchingThrust puts the real horizontal force on both springings once the
		 * accumulation settles, and the joint's own shear axis measures it like any other demand.
		 * Judging the same thrust a second time — and by a different rule, since that pass reads
		 * H off the span and the cover while this reads it off the deleted couple — would answer
		 * one question twice. The ONE-CELL hole is the case with no thrust pass of its own.
		 */
		if (PieceReseatedOnAnArch[Abutment])
		{
			return true;
		}

		/*
		 * SO THE RELIEF HAS TO BE EARNED, AND WHAT EARNS IT IS THE SEAT'S OWN SLIDING CAPACITY.
		 *
		 * Moving the thrust line in to the kern edge deletes a couple of `(1 - k)*|M|` from what
		 * this joint carries, and on the free body of a half-seated brick nothing can supply it
		 * except a horizontal pair — a push out through this head joint, and its equal and
		 * opposite reaction as shear in the bed plane below. The arm between the two is MEASURED
		 * rather than assumed: the head joint's own centroid above the seat's own centroid, taken
		 * along the seat's normal so a tilted joint is measured in its own frame instead of in Z.
		 * For a standard brick and a 1 cm joint that is 3.75 cm, which puts the demand at
		 * `(e - h/6)/z = 1.0444` of the reaction — a fact about the bond geometry alone, with the
		 * load cancelled out of it, exactly as the spanned case's `3L/(4*d_e)` is.
		 *
		 * WITHHELD RATHER THAN APPLIED, AND THAT IS A CHOICE. Pushing the thrust in as a real
		 * shear force is the other honest answer and belongs with a re-anchoring pass: it moves
		 * every one-cell arch in the project by twenty-odd times on an axis that was reading
		 * zero. Withholding leaves an earned arch bit-identical to what it read before this
		 * existed, and leaves an unearned one reading what it is actually carrying — which for a
		 * cohesionless joint outside its kern is a tension it has no strength for at all.
		 *
		 * `!(demand <= capacity)` AND NOT `demand > capacity`. Every comparison against a NaN is
		 * false, so the negated form lands a degenerate arm, a degenerate area or a capacity that
		 * came back unreadable INSIDE the refusal and the relief is withheld. A joint reading as
		 * intact when it should read as failed is the expensive direction to be wrong in, and an
		 * arch granted on arithmetic nobody can interpret is precisely that.
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

bool FStructure::BreakOverturnedBodies(int32 Pass)
{
	/*
	 * A NO-OP WITHOUT COMPLETE GEOMETRY, exactly like ReseatSpannedGroups and for the same
	 * load-bearing reason: a free body is a weight at a centroid against a bearing rectangle,
	 * and with either missing there is nothing to take moments of. Both fuzz generators emit
	 * no geometry — 20,000 cases between them, the only property tests over routing — and the
	 * cascade fuzz's sharpest property is "no joint broke that was not over capacity on the
	 * graph its own pass was solved on", which a guard firing without geometry would set
	 * against an oracle that has never heard of overturning.
	 */
	if (!HasCompleteGeometry())
	{
		return false;
	}

	/*
	 * The intact joints touching each piece — the same walk SolveLoads makes, rebuilt here
	 * because this runs AFTER the capacity sweep has latched this pass's over-capacity joints,
	 * and a joint that has just given is not a path the flood below may cross.
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

	bool bBrokeAny = false;

	TArray<bool> Visited;
	TArray<int32> Frontier;

	for (int32 Index = 0; Index < Connections.Num(); ++Index)
	{
		const FConnection& Joint = Connections[Index];

		if (Joint.HasGiven())
		{
			continue;
		}

		/*
		 * ONLY A BED JOINT HAS A BEARING EDGE. The piece the joint is beneath is the body's
		 * first course; a head joint holds nothing up this way and MOMENTS_DESIGN case (b) —
		 * a brick hanging off one — must keep reading what it reads.
		 */
		int32 Upper = INDEX_NONE;

		if (GetJointRole(Index, Joint.PieceB) == EJointRole::BedBeneath)
		{
			Upper = Joint.PieceB;
		}
		else if (GetJointRole(Index, Joint.PieceA) == EJointRole::BedBeneath)
		{
			Upper = Joint.PieceA;
		}
		else
		{
			continue;
		}

		/*
		 * ONLY A BODY THE SOLVER IS CURRENTLY HOLDING UP. A body already falling has no
		 * bearing to overturn about, and condemning joints inside a region the cascade
		 * released in an EARLIER pass would rewrite the collapse sequence — and could
		 * dissolve a knot, moving the stranded counts the known-red wall rows pin —
		 * without changing who reaches the earth. Supported and not Grounded, which also
		 * drops removed pieces: after the solve at the top of this pass they read Falling.
		 *
		 * The protection is across passes, not within one: support state comes from the
		 * pass-top solve, while the flood graph excludes this same pass's capacity breaks
		 * — so a body whose only ground path was severed by THIS pass's sweep still reads
		 * Supported here, and its bridge bearing can pick up a same-pass stamp. That is
		 * consistent with the fixture convention that simultaneous failures share a pass,
		 * and no current fixture reaches it; the convention is unpinned either way.
		 */
		if (GetPieceSupport(Upper) != EPieceSupport::Supported)
		{
			continue;
		}

		const int32 Seat = OtherEndOf(Joint, Upper);

		/*
		 * THE MAXIMAL BONDED BODY, AND THE JOINT MUST BE ITS ONLY BEARING. Flood outward from
		 * the upper piece over every intact joint EXCEPT this one: reaching the seat means the
		 * body has a second path around its bearing and no free-body statement about this one
		 * edge is exact; reaching the earth means the "body" is holding itself up elsewhere.
		 * Either way the guard stands aside — every running-bond wall, filled corbel and
		 * spanned opening in the project aborts here within a few hops, which is what keeps
		 * this affordable and every existing verdict untouched. Only when the flood closes
		 * over an ungrounded set does the whole of that set's weight provably pass through
		 * this joint, and the check below becomes plain statics.
		 */
		Visited.Init(false, Pieces.Num());
		Frontier.Reset();

		Visited[Upper] = true;
		Frontier.Add(Upper);

		bool bBodyIsFree = !Pieces[Upper].bIsGrounded;

		for (int32 Head = 0; bBodyIsFree && Head < Frontier.Num(); ++Head)
		{
			for (const int32 Walk : PieceJoints[Frontier[Head]])
			{
				if (Walk == Index)
				{
					continue;
				}

				const int32 Neighbour = OtherEndOf(Connections[Walk], Frontier[Head]);

				if (Neighbour == INDEX_NONE || Visited[Neighbour])
				{
					continue;
				}

				if (Neighbour == Seat || Pieces[Neighbour].bIsGrounded)
				{
					bBodyIsFree = false;
					break;
				}

				Visited[Neighbour] = true;
				Frontier.Add(Neighbour);
			}
		}

		if (!bBodyIsFree)
		{
			continue;
		}

		/*
		 * THE FREE BODY: its weight and where that weight acts. Every member is live (the
		 * flood crosses only intact joints, and a removed piece keeps none) and placed (the
		 * geometry gate above). Written as a positive test so a body whose mass is zero or
		 * unreadable overturns nothing rather than dividing by it.
		 */
		double BodyMassKg = 0.0;
		FVector MassWeightedCentreKgCm = FVector::ZeroVector;

		for (const int32 Member : Frontier)
		{
			BodyMassKg += Pieces[Member].MassKg;
			MassWeightedCentreKgCm += Pieces[Member].MassKg * Pieces[Member].CentreOfMassCm;
		}

		if (!(BodyMassKg > 0.0) || !FMath::IsFinite(BodyMassKg))
		{
			continue;
		}

		const FVector BodyCentreCm = MassWeightedCentreKgCm / BodyMassKg;
		const double BodyWeightUu = BodyMassKg * SolverGravityCmPerSecondSquared;

		/*
		 * OVERTURNING ABOUT THE BEARING EDGE AGAINST WHAT RESTORES IT, one in-plane axis at a
		 * time. Under complete geometry a rectangle sits only on an axis-aligned normal
		 * (AddConnection refuses the rest), so a bed joint's plane is horizontal, gravity is
		 * square to it, and the in-plane axes are exactly the rectangle's two non-zero
		 * half-extents.
		 *
		 * THE EDGE IS THE RECTANGLE'S OWN BOUNDARY on the side the body's centroid sits:
		 * lever = |centroid - joint centroid| - half-extent, per axis. At or inside the edge
		 * the weight is standing on its bearing and restores; that is the "standing weight
		 * inside the edge" term, and it is why a plumb tooth of stack-bonded bricks — lever
		 * negative on both axes — can never fire this whatever its height.
		 *
		 * PAST THE EDGE the weight overturns, and what resists is the BOND: the bearing
		 * rectangle at the profile's mean flexural bond, as the elastic section modulus about
		 * the edge's own axis — (4/3) * h_other * h_axis^2, the same bd^2/6 every section in
		 * this project is. Corbel A measures the pair: ~18.5 N.m of overturning against
		 * ~125.65 N.m restored at 0.70 MPa over its 179.48 cm3 patch, standing 6.8x, while the
		 * thirty-course leaning stack overturns its 225.93 cm3 by 6.8x.
		 *
		 * EVERY COMPARISON IS A POSITIVE TEST, so a NaN lever or a NaN modulus fires nothing:
		 * the guard is a second referee that BREAKS, and inventing a break out of arithmetic
		 * nobody can read would be failing open in the direction that dismantles structures.
		 * The joint checks underneath keep their own fail-closed answers either way.
		 */
		bool bOverturns = false;

		for (int32 Axis = 0; Axis < 3 && !bOverturns; ++Axis)
		{
			const double HalfExtentCm = Joint.InterfaceHalfExtentCm[Axis];

			if (!(HalfExtentCm > 0.0))
			{
				continue;
			}

			const double LeverCm =
				FMath::Abs(BodyCentreCm[Axis] - Joint.InterfaceCentreCm[Axis]) - HalfExtentCm;

			if (!(LeverCm > 0.0))
			{
				continue;
			}

			/* The OTHER in-plane half-extent: the bearing's width across the tipping axis. */
			double AcrossCm = 0.0;

			for (int32 Other = 0; Other < 3; ++Other)
			{
				if (Other != Axis && Joint.InterfaceHalfExtentCm[Other] > 0.0)
				{
					AcrossCm = Joint.InterfaceHalfExtentCm[Other];
				}
			}

			const double ModulusCm3 = (4.0 / 3.0) * AcrossCm * HalfExtentCm * HalfExtentCm;

			const double OverturningUuCm = BodyWeightUu * LeverCm;
			const double RestoringUuCm = SolverInterimOverturningMeanBondMPa
				* DestructionForce::ForceUnitsPerMPaSqCm * ModulusCm3;

			if (OverturningUuCm > RestoringUuCm)
			{
				bOverturns = true;
			}
		}

		if (!bOverturns)
		{
			continue;
		}

		/*
		 * THE BEARING GIVES. Latched and stamped exactly as the capacity sweep stamps, because
		 * a bearing about which its body has no equilibrium has failed under load and belongs
		 * in the collapse sequence; Sever is only the mechanism of latching without a second
		 * utilisation evaluation. The body loses its one connection to everything else and the
		 * re-solve reports it falling, whole.
		 */
		Connections[Index].Sever();
		ConnectionBreakPass[Index] = Pass;
		bBrokeAny = true;
	}

	return bBrokeAny;
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
			 *
			 * AND SO DOES THE COMPOSITE DEPTH, WHICH IS THE SAME SEAM POINTING THE OTHER
			 * WAY. It is a relief rather than a load, so a sweep that omitted it would
			 * break joints the readout draws as comfortable — a corbel shown at 0.37 and
			 * snapped at 22.9. Three arrays now, one solve, one evaluator.
			 */
			Connection.ApplyForce(
				ConnectionForces[Index], ConnectionMoments[Index],
				ConnectionCompositeDepthCm[Index]);

			if (Connection.HasGiven())
			{
				ConnectionBreakPass[Index] = Pass;
				bBrokeThisPass = true;
			}
		}

		/*
		 * THE SECOND GROUND A PASS BREAKS ON: the interim overturning guard (DESIGN.md §7
		 * step 2, disposable, deleted at step 4). The capacity sweep asks each joint about
		 * its own stresses; this asks whether any bonded body has walked past the edge of
		 * the one bearing holding it — a state no per-joint number can express, which is
		 * why a leaning stack read the same comfortable utilisation at every height. Same
		 * pass, same stamp: the two grounds are simultaneous, not sequenced.
		 */
		if (BreakOverturnedBodies(Pass))
		{
			bBrokeThisPass = true;
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

FVector FStructure::GetConnectionMoment(int32 ConnectionIndex) const
{
	/*
	 * Zero for an out-of-range handle, and zero for a connection no solve has reached —
	 * the same scope and the same shape as GetConnectionForce, because it is the same
	 * solver output rebuilt by the same solve.
	 *
	 * A MOMENT IS A LOAD, NOT A VERDICT, so the fail-closed answer here is the opposite
	 * polarity to GetConnectionUtilisation's Max(): zero says nothing is levering this,
	 * which is the conservative reading for something that is not a joint, and it is the
	 * utilisation that has to come back reading as failed. An invented enormous moment
	 * would make a NaN of everything finite downstream of it.
	 */
	return ConnectionMoments.IsValidIndex(ConnectionIndex)
		? ConnectionMoments[ConnectionIndex]
		: FVector::ZeroVector;
}

double FStructure::GetConnectionCompositeDepthCm(int32 ConnectionIndex) const
{
	/*
	 * Zero for an out-of-range handle and zero for a connection no solve has reached — the same
	 * scope and the same shape as the two accessors above, because it is the same solver output
	 * rebuilt by the same solve.
	 *
	 * A DEPTH IS A RELIEF, NOT A LOAD, so the fail-closed answer here is zero for the OPPOSITE
	 * reason GetConnectionMoment's is: no masonry credited means the joint reads its own bed
	 * patch and reports as heavily loaded, and it is the utilisation that must come back reading
	 * as failed for something that is not a joint. An invented depth would quietly relieve one.
	 */
	return ConnectionCompositeDepthCm.IsValidIndex(ConnectionIndex)
		? ConnectionCompositeDepthCm[ConnectionIndex]
		: 0.0;
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
	 * AND ALL THREE PARTS COME OFF THE ACCESSORS RATHER THAN OFF THE ARRAYS, which is what
	 * makes the identity Structure.h states — this answer equals UtilisationUnder of
	 * GetConnectionForce, GetConnectionMoment and GetConnectionCompositeDepthCm — true by
	 * construction instead of by three range checks agreeing. Reading the arrays directly
	 * here would be the second copy the whole seam exists to rule out, and BOTH trailing
	 * parameters are DEFAULTED, so the drift would be silent at every call site that
	 * omitted one.
	 *
	 * THE DEPTH IS THE THIRD PART AND IT MOVES THE ANSWER IN THE OTHER DIRECTION, which is
	 * worth saying because the seam is easiest to break where it is least expected: dropping
	 * the moment makes a readout too optimistic, dropping the depth makes it too pessimistic,
	 * and both are the readout disagreeing with the cascade about the same joint.
	 *
	 * Zero for a handle no solve has reached, exactly as the force is, and zero is a
	 * load path with no eccentricity rather than a tolerance.
	 */
	return GetConnection(ConnectionIndex)
		.UtilisationUnder(
			GetConnectionForce(ConnectionIndex),
			GetConnectionMoment(ConnectionIndex),
			GetConnectionCompositeDepthCm(ConnectionIndex));
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
