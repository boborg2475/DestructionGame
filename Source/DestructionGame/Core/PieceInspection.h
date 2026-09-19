// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"

/**
 * What ONE joint on ONE piece is doing: the row of a debugger's per-joint breakout.
 *
 * Every field is something the solver already answers, read back rather than re-derived —
 * the whole discipline of this type. FStructure::GetConnectionUtilisation is one line
 * delegating to FConnection::UtilisationUnder specifically so a readout cannot become a
 * third hand-copy of the break decision; this project has already paid twice for a
 * duplicated derivation (the half-bat mass, and the break decision itself), and a second
 * copy agrees to nine decimal places forever and still differs in the last bit.
 */
struct FJointInspection
{
	/** Which connection this row is, so a caller can go back to the graph with it. */
	int32 ConnectionIndex = INDEX_NONE;

	/**
	 * The piece at the FAR end of this joint — the neighbour, not the piece being inspected.
	 *
	 * It keeps naming a piece that has been removed, deliberately: the joint that went with
	 * it is precisely what a player who just pulled a brick wants to see, and a row that
	 * dropped the handle would leave "this joint has gone" with nothing saying whose it was.
	 */
	int32 OtherPieceIndex = INDEX_NONE;

	/** What this joint is to the piece being inspected. FStructure's own decision. */
	EJointRole Role = EJointRole::None;

	/**
	 * What this joint carries, in UNREAL FORCE UNITS, exactly as GetConnectionForce gives it.
	 *
	 * Not converted to newtons here, and the reason is the one conversion boundary: 1 N is
	 * 100 uu, but the only named factor in this codebase is ForceUnitsPerMPaSqCm — 10,000,
	 * the newton factor times the cm2-to-mm2 factor — so dividing by 100 here would be a
	 * second, open-coded conversion, which CLAUDE.md forbids outright. It would also put this
	 * field in different units from the accessor it is read from, the one thing that makes
	 * "the breakout agrees with the graph" checkable at all.
	 *
	 * So the model layer stays in the units the model works in, and turning uu into
	 * something a human reads is the presenter's formatting decision, not an arithmetic one
	 * buried in Core.
	 */
	FVector ForceUu = FVector::ZeroVector;

	/**
	 * The bending moment this joint carries about its own centroid, in uu.cm, exactly as
	 * GetConnectionMoment gives it.
	 *
	 * Without it the row cannot explain the number beside it: utilisation is computed from
	 * the force and the moment, so a breakout carrying only the force shows a joint at 2.24x
	 * capacity next to 2667 uu with no arithmetic a reader can do between them.
	 *
	 * Raw uu.cm for the reason ForceUu is raw uu: the field is held against the accessor it
	 * came from with exact ==, only checkable while the two are in the same units.
	 *
	 * Zero is "no eccentricity" and is not a tolerance — see FStructure::GetConnectionMoment.
	 * A centred load, a piece nobody placed and a joint whose rectangle nobody measured all
	 * produce it exactly.
	 */
	FVector MomentUuCm = FVector::ZeroVector;

	/** How close to failing under that force. 0 unloaded, 1 at the limit, above 1 gives. */
	double Utilisation = 0.0;

	/**
	 * Whether this joint is still in the structure at all.
	 *
	 * Not inferable from the two fields above — rendering it as though it were is the
	 * misleading readout this flag exists to prevent. A joint that has given carries no
	 * force, so it reads 0 uu at 0 utilisation, identical to an intact joint with nothing on
	 * it. One is a hole in the wall and the other a healthy bed joint on a pad; a debugger
	 * that drew them the same way would be worse than none.
	 */
	bool bHasGiven = false;

	/**
	 * Which cascade pass gave this joint, or INDEX_NONE if no pass did.
	 *
	 * Paired with bHasGiven and only meaningful beside it — INDEX_NONE does not mean intact.
	 * See FStructure::GetBreakPass: the pair is a complete encoding of three states, and the
	 * middle one is the player's own move.
	 *
	 *     intact              bHasGiven false, INDEX_NONE
	 *     went with a piece   bHasGiven true,  INDEX_NONE
	 *     broke in pass N     bHasGiven true,  N >= 1
	 */
	int32 BreakPass = INDEX_NONE;
};

/**
 * Everything a debugger would show about ONE piece: its own state, and every joint on it.
 *
 * The first time the solver's answer is computed for a human rather than an assertion,
 * which is why it lives in Core with no world and no widget: the piece menu widget landed
 * under a recorded exception to the TDD gate, on condition that it contains no logic
 * whatsoever, and a readout is exactly the feature that would erode that. Every number on
 * this struct is decided somewhere a test can reach.
 */
struct FPieceInspection
{
	/**
	 * Whether the ref or handle named a piece that is actually in the structure.
	 *
	 * False is not the same as "no joints", which is why this is a field rather than an
	 * inference from an empty list: an isolated grounded pad is a real piece with nothing
	 * joined to it, and a ref that names nothing is not a piece at all.
	 */
	bool bIsPiece = false;

	/** The handle that was resolved, or INDEX_NONE. */
	int32 PieceIndex = INDEX_NONE;

	/**
	 * Whether the last solve actually answered for this piece — FStructure::HasSupportAnswer.
	 *
	 * The other half of Support, and without it the readout lies: EPieceSupport::Falling is
	 * both "nothing is holding this up" and "nobody has asked yet", deliberately, since zero
	 * has to be the fail-closed enumerator. A breakout showing a freshly built, never-solved
	 * wall as a column of falling bricks would be reporting a catastrophe that has not
	 * happened.
	 */
	bool bHasSupportAnswer = false;

	/** Why this piece is or is not being held up. Falling when there is no answer. */
	EPieceSupport Support = EPieceSupport::Falling;

	/**
	 * Every joint touching this piece, in ascending connection order, INCLUDING ones that
	 * have given.
	 *
	 * A scan, deliberately not an index: there is no adjacency index on FStructure, and
	 * building one is recorded as measured not worth it for the solve, while this is one
	 * piece on demand at a player's click, so a scan is the right cost.
	 *
	 * Ascending connection order because a debugger's list must not reshuffle between two
	 * looks at the same brick, and connection order is the only stable order there is.
	 */
	TArray<FJointInspection> Joints;
};

/**
 * The per-joint breakout for one piece of a structure.
 *
 * Fails closed on anything that is not a live piece — an out-of-range handle, INDEX_NONE,
 * and a piece that has been removed — answering bIsPiece false with no joints at all. The
 * removed case is worth stating: FStructure deliberately keeps the last solve's support
 * answer for a removed piece until something re-solves, right for a solver accessor with a
 * documented scope and wrong for a readout, where a stale Supported drawn beside a gone
 * brick is a confident answer about nothing.
 *
 * It never produces a row for a handle that names no joint, so the other fail-closed
 * answer in this area — GetConnectionUtilisation's TNumericLimits<double>::Max() for an
 * unknown connection — cannot reach a row here. Max() and a given joint's zero are the two
 * answers a readout must never draw alike, and this type keeps them apart by never showing
 * the first and by carrying bHasGiven for the second.
 */
FPieceInspection InspectPiece(const FStructure& Structure, int32 PieceIndex);

/**
 * The same, entered by the ref a click produces rather than by a handle.
 *
 * A thin resolve on top of the one above, exactly as the single-ref BuildPieceMenuRows is
 * a thin call onto the selection one: resolving here means "a ref naming another
 * structure" and "a ref naming a piece that has gone" take the same fail-closed path as
 * everything else.
 */
FPieceInspection InspectPiece(const FStructureBinding& Binding, const FPieceRef& Ref);
