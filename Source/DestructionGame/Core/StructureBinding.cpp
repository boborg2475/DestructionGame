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
	const DestructionLayout::FPieceBox& Box)
{
	const int32 Handle = Structure.AddPiece(MassKg, bIsGrounded);

	FPieceBinding Binding;
	Binding.Actor = Actor;
	Binding.Box = Box;

	Pieces.Add(Binding);

	return Handle;
}

int32 FStructureBinding::AddConnection(const FConnection& Connection)
{
	/*
	 * Straight through: connections name piece handles and nothing else, so there is no
	 * second half of a joint for this layer to keep in step. FStructure validates at the
	 * door and there is nothing to add to that.
	 */
	return Structure.AddConnection(Connection);
}

bool FStructureBinding::RemovePiece(int32 PieceIndex)
{
	/*
	 * THE GRAPH DECIDES WHETHER THIS IS A REMOVAL AT ALL, and the binding acts only if
	 * it was. FStructure::RemovePiece already answers false for a handle that names no
	 * piece and for one that has already gone, so asking it first means there is exactly
	 * one place that knows what a live piece is — a second check here would be a copy
	 * that can disagree, and the two disagreeing is precisely the desync this whole type
	 * exists to make inexpressible.
	 */
	if (!Structure.RemovePiece(PieceIndex))
	{
		return false;
	}

	/*
	 * The actor is the ONLY field removal touches. The box stays, because it is a record
	 * of where the brick was rather than a reference to anything that can die, and
	 * clearing it would leave a well-formed zero-size box at the world origin — which
	 * reads as real geometry rather than as absent. See FPieceBinding.
	 */
	Pieces[PieceIndex].Actor = nullptr;

	return true;
}

int32 FStructureBinding::NumPieces() const
{
	/*
	 * THE BINDING ARRAY'S OWN EXTENT, not a forward to the structure. The two being
	 * equal is the invariant, so answering with the structure's count would make every
	 * check of that invariant compare a number against itself and pass forever.
	 *
	 * Like FStructure::NumPieces this is the handle RANGE and never a live count:
	 * callers iterate 0..NumPieces() and resolve each handle.
	 */
	return Pieces.Num();
}

bool FStructureBinding::IsPieceRemoved(int32 PieceIndex) const
{
	/*
	 * Forwarded, never mirrored. The tombstone on FStructurePiece is the record of what
	 * has gone; a bool kept here beside it would be a second answer to the same
	 * question, and only one of them would be updated by whoever next touches removal.
	 */
	return Structure.IsPieceRemoved(PieceIndex);
}

const FPieceBinding& FStructureBinding::GetBinding(int32 PieceIndex) const
{
	/*
	 * A default placeholder for an unknown handle, matching FStructure::GetPiece — and
	 * every field of it is the fail-closed answer: no actor, no box, not released.
	 */
	static const FPieceBinding Placeholder;
	return Pieces.IsValidIndex(PieceIndex) ? Pieces[PieceIndex] : Placeholder;
}

UObject* FStructureBinding::GetActor(int32 PieceIndex) const
{
	/*
	 * THREE WAYS TO GET NULL, AND ONLY ONE OF THEM IS CODE HERE. An unknown handle gets
	 * the placeholder, a removed piece had its actor cleared by RemovePiece, and an
	 * actor destroyed by any other route — a level transition, a lifespan, anything
	 * calling Destroy — is answered null by the weak pointer itself, which is the whole
	 * reason the field is weak. TWeakObjectPtr::Get does not resolve an object that has
	 * been marked garbage, so the answer changes at the moment of destruction rather
	 * than at the next collect.
	 */
	return GetBinding(PieceIndex).Actor.Get();
}

bool FStructureBinding::IsReleased(int32 PieceIndex) const
{
	return GetBinding(PieceIndex).bReleased;
}

int32 FStructureBinding::ResolvePiece(const FPieceRef& Ref) const
{
	/*
	 * AN UNIDENTIFIED BINDING MATCHES NOTHING, and this is the first check rather than a
	 * clause of the second because equality alone gets it backwards: a binding still
	 * carrying the default INDEX_NONE would otherwise match a ref that never learned who
	 * it belonged to, and two unidentified things finding each other is the fail-OPEN
	 * direction. The inequality below then rejects a defaulted ref against a real
	 * binding, so both halves of "neither default may ever match" are closed.
	 */
	if (StructureId == INDEX_NONE || Ref.StructureId != StructureId)
	{
		return INDEX_NONE;
	}

	/*
	 * A BOUNDS CHECK ALONE IS NOT ENOUGH, which is why this asks IsPieceRemoved rather
	 * than IsValidIndex: a tombstoned slot is still a perfectly valid array index, and
	 * accepting one hands the caller a confident handle to a brick that is not in the
	 * graph. IsPieceRemoved answers true for an out-of-range handle as well, so the one
	 * call closes the range and the tombstone together — a stale ref is the normal case
	 * here, not the exotic one.
	 */
	if (IsPieceRemoved(Ref.PieceIndex))
	{
		return INDEX_NONE;
	}

	return Ref.PieceIndex;
}

void FStructureBinding::SolveLoads()
{
	/*
	 * Solving is the world-free half and stays that way: it computes, it releases
	 * nothing, and ApplyResults is the separate, explicit push. A callback from in here
	 * would fire during a solve that anything is allowed to re-run for a strain readout.
	 */
	Structure.SolveLoads();
}

int32 FStructureBinding::ApplyResults()
{
	int32 ReleasedCount = 0;

	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		FPieceBinding& Binding = Pieces[PieceIndex];

		/*
		 * ONE WAY, AND THIS IS THE SKIP THAT MAKES IT SO. A piece already handed to
		 * physics has MOVED, and nothing in this layer knows where it went — so it is
		 * neither released again nor frozen back, however the solver's answer has
		 * changed since. Removal genuinely can change it: pulling a piece out promotes a
		 * head joint that a bed joint beneath had outranked, and the piece above reads
		 * Supported on the very next solve.
		 */
		if (Binding.bReleased)
		{
			continue;
		}

		/* A removed piece has no actor left to hand to physics. */
		if (IsPieceRemoved(PieceIndex))
		{
			continue;
		}

		/*
		 * NO ANSWER IS NOT AN INSTRUCTION, AND THE POLARITY INVERTS AT THIS LINE.
		 * GetPieceSupport reports Falling for a handle the last solve never reached — a
		 * structure nothing has solved, or a brick laid onto a settled wall since — because
		 * down there Falling is a DIAGNOSTIC and enumerator zero is the answer that
		 * promises least. Up here it is a COMMAND: hand the brick to physics and latch it
		 * forever. The same default is now fail-OPEN, and it releases the whole structure,
		 * grounded foundation included, with no later solve able to put any of it back.
		 *
		 * So the graph is asked whether it HAS an answer before its answer is acted on,
		 * which is the shape RemovePiece already uses one function up: ask the thing that
		 * knows and act only if it said yes. Note this cannot be spelled "have we ever
		 * solved?" — that closes the empty case and leaves the short one, which is the
		 * ordinary path a player laying a brick takes.
		 */
		if (!Structure.HasSupportAnswer(PieceIndex))
		{
			continue;
		}

		/*
		 * RELEASE IS EXACTLY "NOT HELD UP", AND IS BLIND TO WHY. Grounded and Supported
		 * stay put; Stranded and Falling both come down, and are deliberately not told
		 * apart. The distinction is a diagnostic about the SOLVE — Stranded means the
		 * solver declined to divide load round a knot rather than that anything is
		 * carrying the piece — so branching on it here would amount to claiming a
		 * stranded brick should hang in the air.
		 */
		const EPieceSupport Support = Structure.GetPieceSupport(PieceIndex);

		if (Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported)
		{
			continue;
		}

		/*
		 * Read and written in the same pass, for the reason FConnection latches inside
		 * ApplyForce: a check and a set that live apart drift.
		 */
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
	 * REFUSED OUTRIGHT IF THE INPUT IS ALREADY OUT OF STEP, and nothing is written.
	 *
	 * RunningBond can genuinely produce such a layout: an infinite brick dimension passes
	 * its !(x > 0.0) spec guard, PieceMassKg answers with something FStructure::AddPiece
	 * refuses, and the box is appended regardless — so Boxes ends up one longer than the
	 * piece array. Copying that index-by-index PROPAGATES the desync into the one type
	 * whose entire contract is that its two arrays cannot desync, and the AddPiece-return
	 * guard already queued on FStructureBinding would NOT close it: the mass handed over
	 * is a real one belonging to the wrong piece, or a zero, both of which the door
	 * accepts. A type that makes a bad state inexpressible must not be the thing that
	 * launders a known-bad input into that shape, so the check is at the door.
	 *
	 * The actor list is the third parallel array and comes from a different place again —
	 * whoever spawned the bricks. Too few and a handle binds to nothing; too many and
	 * there are actors in the world no piece will ever name, which is the direction that
	 * looks fine.
	 *
	 * An empty layout is refused as well. RunningBond rejects every spec that would
	 * produce one, so accepting it here would make this the more permissive of the two,
	 * and a caller handed true would have a binding it can never see anything from and no
	 * way to tell that from success.
	 */
	if (PieceCount < 1 || Layout.Boxes.Num() != PieceCount || Actors.Num() != PieceCount)
	{
		return false;
	}

	/*
	 * A REPLAY, AND IT IS CORRECT ONLY BECAUSE A LAYOUT IS APPEND-ONLY. RunningBond adds
	 * pieces and connections and never removes one, so no slot is tombstoned, no handle is
	 * a hole, and handle i of the layout is handle i of the binding by construction — which
	 * is what makes copying by index sound and what makes NumPieces a live count here as
	 * well as a range. The moment a producer removes a piece or reuses a slot, that stops
	 * being true and this has to replay the holes too, or every connection above the first
	 * one names the wrong pieces.
	 */
	for (int32 PieceIndex = 0; PieceIndex < PieceCount; ++PieceIndex)
	{
		const FStructurePiece& Piece = Layout.Structure.GetPiece(PieceIndex);

		Out.AddPiece(
			Piece.MassKg, Piece.bIsGrounded, Actors[PieceIndex], Layout.Boxes[PieceIndex]);
	}

	/*
	 * Joints go over whole, pairing and normal together: a normal inconsistent with its
	 * A/B pairing is the one thing the produced graph cannot survive, and reading the
	 * connection back out as one value is what makes transposing them impossible here.
	 */
	for (int32 JointIndex = 0; JointIndex < Layout.Structure.NumConnections(); ++JointIndex)
	{
		Out.AddConnection(Layout.Structure.GetConnection(JointIndex));
	}

	return true;
}
