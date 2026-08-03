// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "UObject/WeakObjectPtr.h"

/**
 * What a spawned brick carries so it can name itself back to the solver.
 *
 * THERE IS NO ACTOR-TO-PIECE MAP, and that is the point of this struct rather than an
 * incidental saving. We spawn the bricks, so each one can be handed its own identity
 * at spawn time; a reverse hash from actor to handle would be a second structure to
 * keep in step with the first, and every entry in it would be a thing to invalidate
 * when a piece was removed or an actor destroyed. Half the dangling problem simply
 * does not exist if the answer travels on the actor.
 *
 * BOTH FIELDS DEFAULT TO INDEX_NONE, and neither default may ever match: an actor
 * that was never told who it is must resolve to nothing rather than to piece zero of
 * whatever structure asked.
 */
struct FPieceRef
{
	/** Which structure this piece belongs to. */
	int32 StructureId = INDEX_NONE;

	/** Position in that structure's piece array. */
	int32 PieceIndex = INDEX_NONE;
};

/**
 * One piece's link to the world: the actor standing for it, where it was laid, and
 * whether it has already gone dynamic.
 *
 * WEAK, NOT RAW. The tombstone below handles the deliberate case — a piece removed
 * through FStructureBinding::RemovePiece has its actor cleared in the same call — but
 * an actor can also be destroyed by a route this layer never sees: a level transition,
 * a lifespan expiring, something else calling Destroy. A raw pointer would then be a
 * dangling read that looks like a live actor; a weak one reads null, which is the
 * answer every other accessor in this subsystem fails to.
 */
struct FPieceBinding
{
	/** Cleared when the piece is removed; null once whatever it named has gone. */
	TWeakObjectPtr<UObject> Actor;

	/**
	 * Where this piece was laid, and it is KEPT WHEN THE PIECE IS REMOVED.
	 *
	 * A box cannot dangle — it is a pair of vectors, not a reference — so nothing is
	 * made safe by clearing it, and clearing it would be actively worse: a zeroed
	 * FPieceBox is a perfectly well-formed box of zero size sitting at the world
	 * origin, which reads as real geometry rather than as absent. The box is a record
	 * of where the brick WAS, which is exactly what debris, decals and a collapse
	 * replay want after the piece has gone. The actor pointer is the only field with
	 * a lifetime problem, so it is the only one removal touches.
	 */
	DestructionLayout::FPieceBox Box;

	/**
	 * Whether this piece has been handed to physics. ONE WAY, like FConnection's latch.
	 *
	 * Solving is re-runnable and non-destructive; releasing a brick is neither. Once a
	 * piece has gone dynamic it has MOVED, and FStructurePiece has no position — so
	 * re-freezing it would pin it wherever Chaos happened to leave it, and re-releasing
	 * it would re-apply whatever the release does to something already falling. The
	 * flag is read and written in the same pass of ApplyResults for the same reason
	 * FConnection latches inside ApplyForce: a check and a set that live apart drift.
	 */
	bool bReleased = false;
};

/**
 * A structure plus the world's half of it: one binding per piece handle, forever
 * index-parallel to the structure's piece array.
 *
 * WHY THIS LAYER EXISTS. FStructure is position-free and actor-free on purpose, which
 * is what keeps the whole solver suite under a second. But something has to own the
 * structure's lifetime, turn a piece handle into the brick the player is looking at,
 * and push each solve's answer back onto those bricks. That is this, and it is
 * deliberately still a PLAIN STRUCT with no world and no UObject of its own, so it is
 * unit-testable exactly like FStructure. A UWorldSubsystem will own one of these and
 * do the actor-facing work; that is the next slice, not this one.
 *
 * THE TWO ARRAYS CANNOT DESYNC, BECAUSE THE DESYNC IS INEXPRESSIBLE. The structure is
 * private and GetStructure hands back a CONST reference even from a mutable binding,
 * so there is no route to FStructure::RemovePiece except through this type's own
 * RemovePiece — which removes the piece, clears the binding's actor and does both in
 * one function. Nothing outside is ever handed a mutable FStructure&. Making the bad
 * state impossible to write is worth more than a test that catches it after the fact,
 * because the bad state is silent: a binding array one entry out of step resolves
 * every handle above the hole to the wrong brick, and nothing crashes.
 *
 * THERE IS NO SECOND "IS REMOVED" FLAG HERE. FStructure already tombstones the slot
 * and IsPieceRemoved is the record; this type forwards to it rather than keeping its
 * own copy, so the two can never disagree. Clearing the actor is the only extra thing
 * removal does.
 */
struct FStructureBinding
{
	/**
	 * Which structure this is, as an FPieceRef names it.
	 *
	 * INDEX_NONE by default and a binding still carrying that default MATCHES NOTHING,
	 * so an unidentified structure and an unidentified actor fail closed rather than
	 * finding each other.
	 */
	int32 StructureId = INDEX_NONE;

	/**
	 * Read-only access to the graph, for solving results and strain readouts.
	 *
	 * CONST EVEN FROM A MUTABLE BINDING, and there is deliberately no non-const
	 * overload — see the note on the type. Every mutation of the graph has a
	 * counterpart in the binding array, so every mutation goes through a method here.
	 */
	const FStructure& GetStructure() const;

	/**
	 * Add a piece and its binding together, returning the one handle that names both.
	 *
	 * One call because two calls is one call away from a desync.
	 */
	int32 AddPiece(
		double MassKg,
		bool bIsGrounded,
		UObject* Actor,
		const DestructionLayout::FPieceBox& Box);

	/** Forwards to FStructure::AddConnection, which validates at the door. */
	int32 AddConnection(const FConnection& Connection);

	/**
	 * Take a piece out of the structure AND clear its binding's actor, in one call.
	 *
	 * The only public removal path there is. @return true if a live piece was removed.
	 */
	bool RemovePiece(int32 PieceIndex);

	/** The valid handle RANGE, never a live count — see FStructure::NumPieces. */
	int32 NumPieces() const;

	/** Forwarded, so nothing keeps a second copy of the answer. */
	bool IsPieceRemoved(int32 PieceIndex) const;

	/** Out-of-range handles get a default-constructed placeholder. */
	const FPieceBinding& GetBinding(int32 PieceIndex) const;

	/**
	 * The actor standing for this piece, or null.
	 *
	 * Null for an unknown handle, for a piece that has been removed, and for an actor
	 * that has been destroyed by any route at all.
	 */
	UObject* GetActor(int32 PieceIndex) const;

	/** Whether this piece has already gone dynamic. False for an unknown handle. */
	bool IsReleased(int32 PieceIndex) const;

	/**
	 * Turn an actor's own {StructureId, PieceIndex} back into a piece handle.
	 *
	 * FAILS CLOSED, returning INDEX_NONE for: a ref naming another structure, a ref
	 * carrying either default, an index outside the handle range, and an index naming
	 * a piece that has been removed. A stale ref is the normal case rather than the
	 * exotic one — the actor outlives the piece by at least a frame — so the wrong
	 * answer here is a confident handle to somebody else's brick.
	 */
	int32 ResolvePiece(const FPieceRef& Ref) const;

	/**
	 * Recompute the graph, releasing nothing.
	 *
	 * Forwarded, and the forward is the point: solving stays non-destructive and
	 * re-runnable, so anything may ask what the structure carries. ApplyResults is what
	 * turns the answer into work on the world, when the caller says so.
	 */
	void SolveLoads();

	/**
	 * Push the last solve's answer onto the bindings: release every piece the solver
	 * is no longer holding up.
	 *
	 * EXPLICIT, NEVER A CALLBACK FROM INSIDE THE SOLVER. A callback would put the world
	 * back inside the world-free layer, and it would fire during a solve that is
	 * allowed to be re-run and is explicitly non-destructive. Solving decides; this
	 * decides what to do about it, and the caller decides when.
	 *
	 * RELEASE KEYS OFF GetPieceSupport, AND IS BLIND TO WHY. Grounded and Supported
	 * stay put; Stranded and Falling both come down. The two are worth telling apart
	 * for a collapse test — Stranded means the solver could not route a knot, which is
	 * a limitation rather than physics — but that is a diagnostic about the SOLVE, and
	 * nothing about the brick differs: either way there is no load path to the earth
	 * and it is not being held up. A binding that branched on the reason would be
	 * claiming a stranded brick should hang in the air.
	 *
	 * ONE WAY. A piece already released is skipped, so it is neither released again nor
	 * frozen back — even if a later solve reports it supported, which a removal
	 * genuinely can cause by promoting a head joint that was previously outranked by a
	 * bed joint beneath.
	 *
	 * A removed piece is never released: its actor has already been cleared, so there
	 * is nothing left to hand to physics.
	 *
	 * AND NEITHER IS A PIECE THE LAST SOLVE NEVER ANSWERED FOR — before any solve at all,
	 * or added since the last one. GetPieceSupport says Falling in both cases, which is
	 * correct of a diagnostic and disastrous as a command, so this asks
	 * FStructure::HasSupportAnswer first. Releasing is irreversible, so "not yet asked"
	 * must never be read as "not held up".
	 *
	 * @return how many pieces THIS CALL released, which is zero for a settled
	 *         structure and is what a caller polls to learn whether anything moved.
	 */
	int32 ApplyResults();

private:
	FStructure Structure;

	/** One binding per piece handle. Index-parallel to the structure's pieces, always. */
	TArray<FPieceBinding> Pieces;
};

/**
 * Put a built layout into a binding: the only route there is from FBrickLayout to
 * FStructureBinding.
 *
 * The binding owns its FStructure privately and hands out no mutable reference, which is
 * exactly what makes the desync inexpressible — and which also means a wall that has
 * already been laid cannot get in without a function like this one. The alternative
 * available today is a hand-written loop re-adding every piece and every connection at
 * the call site, which is the two-arrays-in-lockstep code the type exists to outlaw.
 *
 * Actors is index-parallel to the layout's pieces: Actors[i] stands for piece handle i.
 *
 * @return true if the whole layout was adopted. See Tests/StructureBindingTest.cpp,
 *         DestructionGame.Core.StructureBinding.AdoptLayout, for what it refuses.
 */
bool AdoptLayout(
	const DestructionLayout::FBrickLayout& Layout,
	TArrayView<UObject* const> Actors,
	FStructureBinding& Out);
