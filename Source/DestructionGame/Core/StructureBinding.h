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
 * There is no actor-to-piece map, and that is the point rather than an incidental saving:
 * we spawn the bricks, so each can be handed its own identity at spawn time, where a
 * reverse hash from actor to handle would be a second structure to keep in step with the
 * first and invalidate on every removal.
 *
 * Both fields default to INDEX_NONE, and neither default may ever match: an actor that was
 * never told who it is must resolve to nothing rather than to piece zero of whatever
 * structure asked.
 */
struct FPieceRef
{
	/** Which structure this piece belongs to. */
	int32 StructureId = INDEX_NONE;

	/** Position in that structure's piece array. */
	int32 PieceIndex = INDEX_NONE;

	/**
	 * By value, because a ref is two integers and nothing else.
	 *
	 * The same piece is named by many different FPieceRef objects — one copied onto the
	 * brick at spawn, one out of a trace, one on a menu row — so anything asking "is this
	 * the piece I already have" must compare what they say, not where they live. Written
	 * once here so the selection, the presenter and everything after cannot grow their own
	 * slightly different version.
	 */
	bool operator==(const FPieceRef& Other) const
	{
		return StructureId == Other.StructureId && PieceIndex == Other.PieceIndex;
	}
};

/**
 * One piece's link to the world: the actor standing for it, where it was laid, and
 * whether it has already gone dynamic.
 *
 * Weak, not raw. The tombstone below handles the deliberate case — RemovePiece clears
 * the actor in the same call — but an actor can also be destroyed by a route this layer
 * never sees: a level transition, a lifespan expiring, something else calling Destroy. A
 * raw pointer would then dangle and look live; a weak one reads null, the fail-closed
 * answer every other accessor here gives.
 */
struct FPieceBinding
{
	/** Cleared when the piece is removed; null once whatever it named has gone. */
	TWeakObjectPtr<UObject> Actor;

	/**
	 * Where this piece was laid, and it is kept when the piece is removed.
	 *
	 * A box cannot dangle — it is a pair of vectors, not a reference — so clearing it
	 * would be actively worse: a zeroed FPieceBox is a well-formed zero-size box at the
	 * origin, which reads as real geometry rather than as absent. The box is a record of
	 * where the brick WAS, which is what debris, decals and a collapse replay want after
	 * the piece has gone. The actor pointer is the only field with a lifetime problem, so
	 * it is the only one removal touches.
	 */
	DestructionLayout::FPieceBox Box;

	/**
	 * Whether this piece has been handed to physics. One way, like FConnection's latch.
	 *
	 * Solving is re-runnable and non-destructive; releasing a brick is neither. Once a
	 * piece has gone dynamic it has moved, and FStructurePiece has no position — so
	 * re-freezing it would pin it wherever Chaos left it, and re-releasing it would
	 * re-apply the release to something already falling. Read and written in the same
	 * pass of ApplyResults for the reason FConnection latches inside ApplyForce: a check
	 * and a set that live apart drift.
	 */
	bool bReleased = false;
};

/**
 * A structure plus the world's half of it: one binding per piece handle, forever
 * index-parallel to the structure's piece array.
 *
 * Why this layer exists: FStructure is position-free and actor-free on purpose, which
 * keeps the whole solver suite under a second, but something has to own the structure's
 * lifetime, turn a piece handle into the brick the player is looking at, and push each
 * solve's answer back onto those bricks. This is deliberately still a plain struct with no
 * world and no UObject, so it is unit-testable exactly like FStructure.
 * UDestructionStructureSubsystem is the UWorldSubsystem that owns one and does the
 * actor-facing work.
 *
 * The two arrays cannot desync, because the desync is inexpressible: the structure is
 * private and GetStructure hands back a const reference even from a mutable binding, so
 * there is no route to FStructure::RemovePiece except through this type's own RemovePiece,
 * which removes the piece and clears the binding's actor in one function. Making the bad
 * state impossible to write is worth more than a test that catches it after the fact,
 * because the bad state is silent: a binding array one entry out of step resolves every
 * handle above the hole to the wrong brick, and nothing crashes.
 *
 * There is no second "is removed" flag here. FStructure already tombstones the slot and
 * IsPieceRemoved is the record; this type forwards to it rather than keeping its own copy,
 * so the two can never disagree.
 */
struct FStructureBinding
{
	/**
	 * Which structure this is, as an FPieceRef names it.
	 *
	 * INDEX_NONE by default, and a binding still carrying that default matches nothing, so
	 * an unidentified structure and an unidentified actor fail closed rather than finding
	 * each other.
	 */
	int32 StructureId = INDEX_NONE;

	/**
	 * Read-only access to the graph, for solving results and strain readouts.
	 *
	 * Const even from a mutable binding, and there is deliberately no non-const overload —
	 * see the note on the type. Every mutation of the graph has a counterpart in the binding
	 * array, so every mutation goes through a method here.
	 */
	const FStructure& GetStructure() const;

	/**
	 * Add a piece and its binding together, returning the one handle that names both.
	 *
	 * One call because two calls is one call away from a desync.
	 *
	 * The box is both halves of where the piece is: the binding keeps it, and its centre
	 * goes down to the solver as the piece's centre of mass, so a piece cannot be laid in
	 * one place and load its joints from another. A box whose centre is not finite places
	 * nothing, and the piece is added unplaced rather than with a centre nobody can use.
	 *
	 * Material travels with the piece, defaulting to "nobody said" so callers that assign
	 * no material are unchanged. It is what the graph pairs against a joint's connection to
	 * reach the weakest-link crush, so it must survive the trip into the binding alongside
	 * the mass — a piece whose material is dropped reads its joints off the bare connection,
	 * making cross-material bearings inert. The store is behind the same private door as the
	 * piece itself, so the two cannot fall out of step.
	 */
	int32 AddPiece(
		double MassKg,
		bool bIsGrounded,
		UObject* Actor,
		const DestructionLayout::FPieceBox& Box,
		const DestructionProfiles::FMaterialProfile* Material = nullptr);

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
	 * Fails closed, returning INDEX_NONE for: a ref naming another structure, a ref
	 * carrying either default, an index outside the handle range, and an index naming a
	 * piece that has been removed. A stale ref is the normal case rather than the exotic
	 * one — the actor outlives the piece by at least a frame — so the wrong answer here is
	 * a confident handle to somebody else's brick.
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
	 * Solve, break every joint over its own capacity, and re-solve until nothing more
	 * gives — leaving the graph settled and ready to be pushed.
	 *
	 * Forwarded, and the forward is the only reason this exists: GetStructure hands back a
	 * const reference even from a mutable binding, so there is no other route to
	 * FStructure::SolveAndBreak — which is what kept the cascade off the world wire entirely,
	 * a graph able to condemn a joint at 2.24 of capacity with nothing in World/ able to ask
	 * it to give.
	 *
	 * It discharges ApplyResults' ordering obligation by construction: the last thing
	 * FStructure::SolveAndBreak does is a complete solve that broke nothing — that pass is how
	 * it knows to stop — so the answer left behind is over exactly the joints and pieces that
	 * survived, and a push behind this pushes a settled answer, not a mid-cascade one.
	 *
	 * Destructive, unlike SolveLoads: joints never heal and the pass stamps are never
	 * rewritten, so this is not something a strain readout or a what-if may call.
	 *
	 * @return how many passes THIS CALL broke at least one joint in; zero for a structure
	 *         that settles as it stands. Per-call, unlike the stamps — see
	 *         FStructure::SolveAndBreak.
	 */
	int32 SolveAndBreak();

	/**
	 * COMPILE STUB FOR SLICE 2 (PROMOTION_DESIGN.md §12 D6-c) — forward the equilibrium gate's
	 * injectable block cap to the private FStructure, since the graph is otherwise reachable
	 * from here by no route. Stores the value; nothing reads it yet. dev-expert wires the gate
	 * to be authoritative at or below the cap and fail closed to the router above it.
	 */
	void SetEquilibriumGateBlockCap(int32 MaxBlocks);

	/**
	 * Flag this binding's structure 3D, forwarding to the private FStructure, which is
	 * reachable from here by no other route. Two callers: AdoptLayout, where a layout laid
	 * 3D (DestructionShed3D::Build sets it) must stay 3D once adopted, or the world bridge
	 * poses the shed in 2D and refuses its out-of-plane corners; and
	 * UDestructionStructureSubsystem::BeginBuild, which states it unconditionally for a
	 * player's build because a build grown from clicks adopts no layout to carry it.
	 * See FStructure::SetThreeDimensional.
	 */
	void SetThreeDimensional(bool bIsThreeDimensional);

	/**
	 * Push the last solve's answer onto the bindings: release every piece the solver
	 * is no longer holding up.
	 *
	 * Explicit, never a callback from inside the solver: a callback would put the world back
	 * inside the world-free layer, and fire during a solve that is allowed to be re-run and
	 * is explicitly non-destructive. Solving decides; this decides what to do about it, and
	 * the caller decides when.
	 *
	 * Release keys off GetPieceSupport, and is blind to why: Grounded and Supported stay put,
	 * Stranded and Falling both come down. The two are worth telling apart for a collapse
	 * test — Stranded means the solver could not route a knot, a limitation rather than
	 * physics — but that is a diagnostic about the solve, and nothing about the brick
	 * differs: either way there is no load path to the earth. A binding that branched on the
	 * reason would be claiming a stranded brick should hang in the air.
	 *
	 * One way: a piece already released is skipped, so it is neither released again nor
	 * frozen back, even if a later solve reports it supported — which removal genuinely can
	 * cause, by promoting a head joint a bed joint beneath had outranked.
	 *
	 * A removed piece is never released: its actor has already been cleared, so there is
	 * nothing left to hand to physics.
	 *
	 * Nor is a piece the last solve never answered for — before any solve at all, or added
	 * since. GetPieceSupport says Falling in both cases, correct of a diagnostic and
	 * disastrous as a command, so this asks FStructure::HasSupportAnswer first. Releasing is
	 * irreversible, so "not yet asked" must never be read as "not held up".
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
 * exactly what makes the desync inexpressible — and also means a wall already laid cannot
 * get in without a function like this one. The alternative is a hand-written loop re-adding
 * every piece and connection at the call site, the two-arrays-in-lockstep code the type
 * exists to outlaw.
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
