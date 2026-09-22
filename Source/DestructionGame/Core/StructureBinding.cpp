// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/StructureBinding.h"

const FStructure& FStructureBinding::GetStructure() const
{
	return Structure;
}

int32 FStructureBinding::AddPiece(
	double MassKg,
	bool bIsGrounded,
	UObject* Actor,
	const DestructionLayout::FPieceBox& Box,
	const DestructionProfiles::FMaterialProfile* Material)
{
	/*
	 * The box centre is the centre of mass; without it every piece loads its joints as if
	 * centred. A non-finite centre (ContainsNaN also catches infinity) is not passed down, since
	 * it would put NaN into lever arms; the piece stays unplaced instead, which
	 * FStructure::HasCompleteGeometry reports.
	 */
	const int32 Handle = Box.CentreCm.ContainsNaN()
		? Structure.AddPiece(MassKg, bIsGrounded)
		: Structure.AddPiece(MassKg, bIsGrounded, Box.CentreCm);

	/*
	 * FStructure refuses negative or non-finite mass. Appending a binding anyway would desync
	 * the two arrays for every later handle.
	 */
	if (Handle == INDEX_NONE)
	{
		return INDEX_NONE;
	}

	// Null material leaves the piece at its default.
	Structure.SetPieceMaterial(Handle, Material);

	FPieceBinding Binding;
	Binding.Actor = Actor;
	Binding.Box = Box;

	Pieces.Add(Binding);

	return Handle;
}

int32 FStructureBinding::AddConnection(const FConnection& Connection)
{
	// Connections name only piece handles, so there is nothing to mirror here.
	return Structure.AddConnection(Connection);
}

bool FStructureBinding::RemovePiece(int32 PieceIndex)
{
	// FStructure is the single authority on what a live piece is.
	if (!Structure.RemovePiece(PieceIndex))
	{
		return false;
	}

	// Only the actor is cleared; the box records where the brick was (see FPieceBinding).
	Pieces[PieceIndex].Actor = nullptr;

	return true;
}

int32 FStructureBinding::NumPieces() const
{
	/*
	 * The binding's own count, so checks that it equals the structure's are meaningful. A handle
	 * range, not a live count.
	 */
	return Pieces.Num();
}

bool FStructureBinding::IsPieceRemoved(int32 PieceIndex) const
{
	// Forwarded, not mirrored: FStructurePiece's tombstone is the only record.
	return Structure.IsPieceRemoved(PieceIndex);
}

const FPieceBinding& FStructureBinding::GetBinding(int32 PieceIndex) const
{
	// Unknown handle: a fail-closed placeholder (no actor, no box, not released).
	static const FPieceBinding Placeholder;
	return Pieces.IsValidIndex(PieceIndex) ? Pieces[PieceIndex] : Placeholder;
}

UObject* FStructureBinding::GetActor(int32 PieceIndex) const
{
	/*
	 * Null for an unknown handle, a removed piece, or an actor destroyed elsewhere. The weak
	 * pointer returns null as soon as the actor is marked garbage.
	 */
	return GetBinding(PieceIndex).Actor.Get();
}

bool FStructureBinding::IsReleased(int32 PieceIndex) const
{
	return GetBinding(PieceIndex).bReleased;
}

int32 FStructureBinding::ResolvePiece(const FPieceRef& Ref) const
{
	// A default binding id must not match a default ref id, so check it separately.
	if (StructureId == INDEX_NONE || Ref.StructureId != StructureId)
	{
		return INDEX_NONE;
	}

	// IsPieceRemoved rejects both out-of-range and tombstoned handles.
	if (IsPieceRemoved(Ref.PieceIndex))
	{
		return INDEX_NONE;
	}

	return Ref.PieceIndex;
}

void FStructureBinding::SolveLoads()
{
	// Computes only; ApplyResults is the separate push to the world.
	Structure.SolveLoads();
}

int32 FStructureBinding::SolveAndBreak()
{
	// Forward to the private structure; the piece-to-actor mapping is unaffected.
	return Structure.SolveAndBreak();
}

void FStructureBinding::SetEquilibriumGateBlockCap(int32 MaxBlocks)
{
	// Forward to the private structure (PROMOTION_DESIGN.md §12 D6-c).
	Structure.SetEquilibriumGateBlockCap(MaxBlocks);
}

void FStructureBinding::SetThreeDimensional(bool bIsThreeDimensional)
{
	// Forward to the private structure; AdoptLayout uses it to carry the 3D flag.
	Structure.SetThreeDimensional(bIsThreeDimensional);
}

int32 FStructureBinding::ApplyResults()
{
	int32 ReleasedCount = 0;

	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		FPieceBinding& Binding = Pieces[PieceIndex];

		/*
		 * Release is one-way: a released piece has moved, so it is never refrozen even if a later
		 * solve reads it Supported.
		 */
		if (Binding.bReleased)
		{
			continue;
		}

		if (IsPieceRemoved(PieceIndex))
		{
			continue;
		}

		/*
		 * GetPieceSupport defaults to Falling for a piece the last solve never reached (unsolved,
		 * or laid since). Here that default would release the whole structure permanently, so
		 * require an answer first. A "has ever solved" check would miss newly laid bricks.
		 */
		if (!Structure.HasSupportAnswer(PieceIndex))
		{
			continue;
		}

		// Stranded and Falling both release: Stranded means unrouted, not held up.
		const EPieceSupport Support = Structure.GetPieceSupport(PieceIndex);

		if (Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported)
		{
			continue;
		}

		Binding.bReleased = true;
		++ReleasedCount;
	}

	return ReleasedCount;
}

bool AdoptLayout(
	const DestructionLayout::FBrickLayout& Layout,
	TArrayView<UObject* const> Actors,
	FStructureBinding& Out)
{
	const int32 PieceCount = Layout.Structure.NumPieces();

	/*
	 * Refuse, writing nothing, if pieces, boxes and actors differ in count or are empty.
	 * RunningBond can emit one extra box (an infinite dimension passes its spec guard but the
	 * piece is refused), and copying by index would then desync every later handle.
	 */
	if (PieceCount < 1 || Layout.Boxes.Num() != PieceCount || Actors.Num() != PieceCount)
	{
		return false;
	}

	/*
	 * Copy by index, valid only because layouts are append-only (no tombstones). A producer that
	 * removes pieces would need the holes replayed too.
	 */
	for (int32 PieceIndex = 0; PieceIndex < PieceCount; ++PieceIndex)
	{
		const FStructurePiece& Piece = Layout.Structure.GetPiece(PieceIndex);

		// Carry the material too, or cross-material bearings lose their weakest-link crush.
		Out.AddPiece(
			Piece.MassKg, Piece.bIsGrounded, Actors[PieceIndex], Layout.Boxes[PieceIndex], Piece.Material);
	}

	// Copy joints whole, so each normal stays consistent with its A/B pairing.
	for (int32 JointIndex = 0; JointIndex < Layout.Structure.NumConnections(); ++JointIndex)
	{
		Out.AddConnection(Layout.Structure.GetConnection(JointIndex));
	}

	/*
	 * The 3D flag is structure-level state. Dropping it poses the shed in 2D, so its corner
	 * joints are refused and the overhang never falls when the post is cut.
	 */
	Out.SetThreeDimensional(Layout.Structure.IsThreeDimensional());

	return true;
}
