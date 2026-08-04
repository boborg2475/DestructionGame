// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"
#include "Core/PieceActions.h"
#include "Core/StructureBinding.h"
#include "Subsystems/WorldSubsystem.h"
#include "DestructionStructureSubsystem.generated.h"

/**
 * What a trace into the world found, if it found a piece at all.
 *
 * BOTH FIELDS FAIL CLOSED TOGETHER. A trace that hit the floor, hit a brick belonging to a
 * structure this subsystem no longer holds, hit a brick whose piece has since been removed,
 * or hit nothing at all, all produce the default: a ref naming nothing and no handle. The
 * ref is what a menu and a commit are built from; the handle is what the last step of the
 * chain resolved to, and keeping both is what lets a test see WHICH step failed closed.
 */
struct FPieceHit
{
	FPieceRef Ref;

	int32 PieceHandle = INDEX_NONE;
};

/**
 * The world's structures: one FStructureBinding per built wall, keyed by structure id.
 *
 * TUniquePtr RATHER THAN A FLAT ARRAY OF BINDINGS. Find hands out a pointer, and a
 * caller that cached one across a later Add would be reading freed storage after the
 * array grew. Boxing each binding makes the address stable structurally rather than by
 * convention.
 */
UCLASS()
class DESTRUCTIONGAME_API UDestructionStructureSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	/**
	 * Lay a running-bond wall, spawn one ABrickActor per box, and adopt the lot.
	 *
	 * THE SAME CALL THE SCENARIO WILL MAKE, so a test of it exercises the real seam
	 * rather than a parallel one written for testing.
	 *
	 * @return the new structure's id, or INDEX_NONE if nothing was built.
	 */
	int32 BuildRunningBond(const DestructionLayout::FRunningBondSpec& Spec);

	/** The binding for a structure id, or null. */
	FStructureBinding* Find(int32 StructureId);

	/**
	 * Solve one structure and push the answer onto the world: every piece the solver is
	 * no longer holding up is handed to physics.
	 *
	 * @return how many pieces THIS CALL released. Zero for a settled structure, and zero
	 *         for a structure id that names nothing.
	 */
	int32 SolveAndPush(int32 StructureId);

	/**
	 * Turn a world-space ray into the piece it hit.
	 *
	 * THE WHOLE CHAIN, AND EVERY STEP FAILS CLOSED: a visibility trace, then
	 * HitResult::GetActor, then a cast to ABrickActor, then the ref the brick carries, then
	 * the binding that ref names, then FStructureBinding::ResolvePiece. Anything that is not
	 * a live piece of a structure this subsystem holds comes back as a default FPieceHit.
	 *
	 * ECC_Visibility because that is the channel the player's own click will use; a channel
	 * invented for this would be a second answer to "can you see it".
	 */
	FPieceHit TracePiece(const FVector& StartCm, const FVector& EndCm);

	/**
	 * Commit an action against a piece and destroy whatever the commit orphaned.
	 *
	 * THIS IS WHERE FPieceActionResult::ActorToDestroy IS CONSUMED, and it is the only place
	 * it can be: RunPieceAction is deliberately world-free and hands the orphan back rather
	 * than destroying it, so without a caller that does the destroying a deleted brick's mesh
	 * stays standing in the world with no piece naming it.
	 *
	 * @return true if the action ran. False for a ref that names nothing, and false without
	 *         destroying anything.
	 */
	bool CommitPieceAction(const FPieceRef& Ref, const FPieceAction& Action);

	/**
	 * Commit an action against a whole selection: run them all, solve once, push once.
	 *
	 * THE BATCHING IS THE REQUIREMENT. RunPieceActions runs every action and then solves
	 * exactly one time; this destroys every orphan it hands back and then pushes that single
	 * answer onto the world. Looping CommitPieceAction instead would solve and push once per
	 * brick — the same final state at N times the cost, which at scenario scale is the
	 * difference between a click and a stutter.
	 *
	 * THE PUSH FOLLOWS THE SOLVE THAT SAW EVERY REMOVAL, and that ordering is load-bearing
	 * rather than tidy: FStructureBinding::ApplyResults refuses to release a piece the last
	 * solve has no answer for, so pushing against an answer computed before the last action
	 * ran leaves the pieces that action orphaned hanging in the air — the exact failure a
	 * player found in ten seconds, reintroduced by a batch.
	 *
	 * EVERY REF MUST NAME THE SAME STRUCTURE, which is what a selection built by clicking one
	 * wall is; refs naming a structure this subsystem does not hold contribute nothing and are
	 * refused piece by piece, exactly as the single-piece commit refuses them.
	 *
	 * @return how many pieces the action actually ran against. Zero for an empty selection and
	 *         for a selection none of whose refs resolve.
	 */
	int32 CommitPieceActionForAll(TArrayView<const FPieceRef> Refs, const FPieceAction& Action);

private:

	TMap<int32, TUniquePtr<FStructureBinding>> Structures;

	int32 NextStructureId = 0;
};
