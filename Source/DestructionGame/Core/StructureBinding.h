// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "UObject/WeakObjectPtr.h"

/**
 * A spawned brick's identity, handed to it at spawn so no actor-to-piece map is needed.
 * Both fields default to INDEX_NONE, so an unassigned actor resolves to nothing, not piece 0.
 */
struct FPieceRef
{
	int32 StructureId = INDEX_NONE;

	/** Position in that structure's piece array. */
	int32 PieceIndex = INDEX_NONE;

	/** Compares by value; one shared definition so callers do not each write their own. */
	bool operator==(const FPieceRef& Other) const
	{
		return StructureId == Other.StructureId && PieceIndex == Other.PieceIndex;
	}
};

/**
 * One piece's link to the world: its actor, where it was laid, and whether it has gone dynamic.
 * The actor is weak because it can be destroyed by routes this layer never sees; a weak
 * pointer then reads null rather than dangling.
 */
struct FPieceBinding
{
	/** Cleared when the piece is removed; null once the actor has gone. */
	TWeakObjectPtr<UObject> Actor;

	/**
	 * Where this piece was laid; kept after removal as a record for debris and replays. A zeroed
	 * box would read as real geometry at the origin, so it is not cleared.
	 */
	DestructionLayout::FPieceBox Box;

	/**
	 * Whether this piece has been handed to physics. One way: re-freezing would pin it where
	 * Chaos left it. Read and set in the same ApplyResults pass so check and set cannot drift.
	 */
	bool bReleased = false;
};

/**
 * A structure plus its world bindings, one per piece handle and always index-parallel to the
 * structure's pieces. A plain struct (no world, no UObject) so it is unit-testable;
 * UDestructionStructureSubsystem owns one and does the actor work.
 *
 * Desync is impossible to express: the structure is private and only exposed const, so every
 * mutation goes through this type and updates both arrays together. An off-by-one binding
 * array would silently resolve handles to the wrong bricks. Removal state is forwarded to
 * FStructure::IsPieceRemoved rather than duplicated.
 */
struct FStructureBinding
{
	/** Which structure this is, as an FPieceRef names it. INDEX_NONE matches nothing. */
	int32 StructureId = INDEX_NONE;

	/** Read-only graph access. No non-const overload, so all mutation goes through this type. */
	const FStructure& GetStructure() const;

	/**
	 * Add a piece and its binding in one call, returning the shared handle. The box centre
	 * becomes the solver's centre of mass; a non-finite centre adds the piece unplaced. Material
	 * (optional) is stored with the piece so cross-material bearings see it.
	 */
	int32 AddPiece(
		double MassKg,
		bool bIsGrounded,
		UObject* Actor,
		const DestructionLayout::FPieceBox& Box,
		const DestructionProfiles::FMaterialProfile* Material = nullptr);

	/** Forwards to FStructure::AddConnection, which validates at the door. */
	int32 AddConnection(const FConnection& Connection);

	/** Remove a piece and clear its binding's actor together. The only public removal path. Returns true if a live piece was removed. */
	bool RemovePiece(int32 PieceIndex);

	/** The valid handle range, not a live count (see FStructure::NumPieces). */
	int32 NumPieces() const;

	bool IsPieceRemoved(int32 PieceIndex) const;

	/** Out-of-range handles get a default-constructed placeholder. */
	const FPieceBinding& GetBinding(int32 PieceIndex) const;

	/** The piece's actor, or null for an unknown handle, a removed piece, or a destroyed actor. */
	UObject* GetActor(int32 PieceIndex) const;

	/** Whether this piece has gone dynamic. False for an unknown handle. */
	bool IsReleased(int32 PieceIndex) const;

	/**
	 * Map an actor's ref back to a piece handle. Fails closed (INDEX_NONE) for another
	 * structure, a default field, an out-of-range index, or a removed piece. Stale refs are
	 * normal, since actors outlive their pieces by at least a frame.
	 */
	int32 ResolvePiece(const FPieceRef& Ref) const;

	/** Recompute the graph, releasing nothing. Non-destructive and re-runnable. */
	void SolveLoads();

	/**
	 * Solve, break every over-capacity joint, and repeat until nothing gives. The only route to
	 * FStructure::SolveAndBreak, since GetStructure is const. Its final pass breaks nothing, so
	 * ApplyResults afterwards pushes a settled answer. Destructive: not for readouts or what-ifs.
	 *
	 * @return passes in this call that broke at least one joint; zero if the structure settles.
	 */
	int32 SolveAndBreak();

	/**
	 * Slice 2 compile stub (PROMOTION_DESIGN.md §12 D6-c): forwards the equilibrium gate's block
	 * cap to FStructure. Stored but not yet read.
	 */
	void SetEquilibriumGateBlockCap(int32 MaxBlocks);

	/**
	 * Flag the structure 3D (see FStructure::SetThreeDimensional). Called by AdoptLayout so an
	 * adopted 3D layout stays 3D, and by UDestructionStructureSubsystem::BeginBuild for player
	 * builds, which adopt no layout.
	 */
	void SetThreeDimensional(bool bIsThreeDimensional);

	/**
	 * Release every piece the last solve no longer holds up. Explicit, never a solver callback,
	 * so the solver stays world-free and re-runnable.
	 *
	 * Keys off GetPieceSupport: Stranded and Falling both release, since neither has a path to
	 * the earth. One way: released pieces are skipped even if later reported supported. Removed
	 * pieces are skipped. Pieces the last solve never answered for are skipped
	 * (FStructure::HasSupportAnswer), because release is irreversible and GetPieceSupport
	 * reports them Falling.
	 *
	 * @return pieces released by this call; zero for a settled structure.
	 */
	int32 ApplyResults();

private:
	FStructure Structure;

	/** One binding per piece handle, index-parallel to the structure's pieces. */
	TArray<FPieceBinding> Pieces;
};

/**
 * Adopt a built layout into a binding: the only route from FBrickLayout to FStructureBinding.
 * Actors[i] stands for piece handle i.
 *
 * @return true if the whole layout was adopted. Refusals are covered by
 *         DestructionGame.Core.StructureBinding.AdoptLayout.
 */
bool AdoptLayout(
	const DestructionLayout::FBrickLayout& Layout,
	TArrayView<UObject* const> Actors,
	FStructureBinding& Out);
