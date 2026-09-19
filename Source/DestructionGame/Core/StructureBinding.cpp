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
	 * The box's centre is the piece's centre of mass, and it goes down with the mass. A
	 * brick is a homogeneous rectangular solid whose mass came from this same box, so the
	 * two are one fact and Layout::RunningBond already derives them together. Keeping the
	 * box up here and forwarding only the mass loses the centre invisibly: an unplaced piece
	 * loads its joints exactly as a centred one does, so every wall the game builds through
	 * the binding would answer every corbel as though its weight acted through the middle of
	 * its support.
	 *
	 * A centre that is not one is not handed down. Box.CentreCm is whatever the caller had,
	 * and a piece nobody could place carries a non-finite one; storing that would launder a
	 * NaN into the lever arm the moment anything subtracts a joint centroid from it — a
	 * plausible-looking load rather than an obvious fault. The two-argument door leaves the
	 * piece honestly unplaced instead, and FStructure::HasCompleteGeometry is what makes
	 * that askable rather than silent. ContainsNaN is !IsFinite on each component, so an
	 * infinity lands inside the guard alongside a NaN.
	 */
	const int32 Handle = Box.CentreCm.ContainsNaN()
		? Structure.AddPiece(MassKg, bIsGrounded)
		: Structure.AddPiece(MassKg, bIsGrounded, Box.CentreCm);

	/*
	 * A refused piece must not land in the binding either. FStructure::AddPiece answers
	 * INDEX_NONE for a mass that is negative or non-finite — its `!(MassKg >= 0.0)` guard
	 * catches a NaN, against which every comparison is false. Appending the FPieceBinding
	 * regardless would grow the binding's Pieces array while the structure's stayed put, so
	 * GetActor(i) would name a different actor than GetPiece(i) for every handle above the
	 * hole — the exact silent desync this type exists to make inexpressible. The refusal is
	 * relayed here before anything is stored.
	 */
	if (Handle == INDEX_NONE)
	{
		return INDEX_NONE;
	}

	/*
	 * The material goes down through the same door as the piece, so what a brick is made of
	 * cannot be laid in one layer and lost in the next. SetPieceMaterial fails closed on an
	 * out-of-range handle, and AddPiece never fails silently, so tagging the handle just
	 * returned is unconditional; a null pointer clears to the "nobody said" default a piece
	 * already starts at, leaving a material-free caller's behaviour untouched.
	 */
	Structure.SetPieceMaterial(Handle, Material);

	FPieceBinding Binding;
	Binding.Actor = Actor;
	Binding.Box = Box;

	Pieces.Add(Binding);

	return Handle;
}

int32 FStructureBinding::AddConnection(const FConnection& Connection)
{
	/* Straight through: connections name piece handles only, so there is no second half
	 * of a joint for this layer to keep in step. FStructure validates at the door. */
	return Structure.AddConnection(Connection);
}

bool FStructureBinding::RemovePiece(int32 PieceIndex)
{
	/*
	 * The graph decides whether this is a removal at all, and the binding acts only if it
	 * was. FStructure::RemovePiece already answers false for a handle that names no piece and
	 * for one that has already gone, so asking it first means there is exactly one place that
	 * knows what a live piece is — a second check here would be a copy that can disagree.
	 */
	if (!Structure.RemovePiece(PieceIndex))
	{
		return false;
	}

	/*
	 * The actor is the only field removal touches. The box stays: it is a record of where
	 * the brick was rather than a reference to anything that can die, and clearing it would
	 * leave a well-formed zero-size box at the origin, reading as real geometry rather than
	 * absent. See FPieceBinding.
	 */
	Pieces[PieceIndex].Actor = nullptr;

	return true;
}

int32 FStructureBinding::NumPieces() const
{
	/*
	 * The binding array's own extent, not a forward to the structure: the two being equal
	 * is the invariant, so answering with the structure's count would make every check of
	 * that invariant compare a number against itself and pass forever.
	 *
	 * Like FStructure::NumPieces this is the handle range, never a live count: callers
	 * iterate 0..NumPieces() and resolve each handle.
	 */
	return Pieces.Num();
}

bool FStructureBinding::IsPieceRemoved(int32 PieceIndex) const
{
	/* Forwarded, never mirrored: the tombstone on FStructurePiece is the record of what
	 * has gone, and a bool kept here beside it would be a second answer to the same
	 * question that only one of them gets updated. */
	return Structure.IsPieceRemoved(PieceIndex);
}

const FPieceBinding& FStructureBinding::GetBinding(int32 PieceIndex) const
{
	/* A default placeholder for an unknown handle, matching FStructure::GetPiece — every
	 * field of it is the fail-closed answer: no actor, no box, not released. */
	static const FPieceBinding Placeholder;
	return Pieces.IsValidIndex(PieceIndex) ? Pieces[PieceIndex] : Placeholder;
}

UObject* FStructureBinding::GetActor(int32 PieceIndex) const
{
	/*
	 * Three ways to get null, and only one is code here: an unknown handle gets the
	 * placeholder, a removed piece had its actor cleared by RemovePiece, and an actor
	 * destroyed by any other route — a level transition, a lifespan, anything calling
	 * Destroy — is answered null by the weak pointer itself, the whole reason the field is
	 * weak. TWeakObjectPtr::Get does not resolve an object marked garbage, so the answer
	 * changes at the moment of destruction rather than at the next collect.
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
	 * An unidentified binding matches nothing, checked first rather than folded into the
	 * second because equality alone gets it backwards: a binding still carrying the default
	 * INDEX_NONE would otherwise match a ref that never learned who it belonged to, the
	 * fail-open direction. The inequality below then rejects a defaulted ref against a real
	 * binding, closing both halves of "neither default may ever match".
	 */
	if (StructureId == INDEX_NONE || Ref.StructureId != StructureId)
	{
		return INDEX_NONE;
	}

	/*
	 * A bounds check alone is not enough, which is why this asks IsPieceRemoved rather than
	 * IsValidIndex: a tombstoned slot is still a valid array index, and accepting one hands
	 * the caller a confident handle to a brick that is not in the graph. IsPieceRemoved
	 * answers true for an out-of-range handle too, so one call closes the range and the
	 * tombstone together — a stale ref is the normal case here, not the exotic one.
	 */
	if (IsPieceRemoved(Ref.PieceIndex))
	{
		return INDEX_NONE;
	}

	return Ref.PieceIndex;
}

void FStructureBinding::SolveLoads()
{
	/* Solving is the world-free half and stays that way: it computes, releases nothing,
	 * and ApplyResults is the separate, explicit push. */
	Structure.SolveLoads();
}

int32 FStructureBinding::SolveAndBreak()
{
	/*
	 * Straight through, and the forward is the point: this layer has nothing to add to the
	 * cascade, and the graph is private, so without a door here nothing that owns a world can
	 * reach it. The binding is untouched — a joint giving changes what the structure carries,
	 * not which actor stands for which piece.
	 */
	return Structure.SolveAndBreak();
}

void FStructureBinding::SetEquilibriumGateBlockCap(int32 MaxBlocks)
{
	/*
	 * Slice 2 compile stub — forwards to the private FStructure, the only route to it. The cap
	 * scopes the equilibrium gate's authority by structure size (PROMOTION_DESIGN.md §12 D6-c);
	 * nothing reads it yet.
	 */
	Structure.SetEquilibriumGateBlockCap(MaxBlocks);
}

void FStructureBinding::SetThreeDimensional(bool bIsThreeDimensional)
{
	/* Forwards to the private FStructure, the only route to it — see the header. AdoptLayout
	 * uses this to carry a laid layout's 3D flag across into the live binding. */
	Structure.SetThreeDimensional(bIsThreeDimensional);
}

int32 FStructureBinding::ApplyResults()
{
	int32 ReleasedCount = 0;

	for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
	{
		FPieceBinding& Binding = Pieces[PieceIndex];

		/*
		 * One way, and this is the skip that makes it so: a piece already handed to physics
		 * has moved, and nothing in this layer knows where it went, so it is neither released
		 * again nor frozen back however the solver's answer has changed since. Removal
		 * genuinely can change it: pulling a piece out promotes a head joint a bed joint
		 * beneath had outranked, and the piece above reads Supported next solve.
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
		 * Release is exactly "not held up", and is blind to why: Grounded and Supported stay
		 * put, Stranded and Falling both come down, deliberately not told apart. The
		 * distinction is a diagnostic about the solve — Stranded means the solver declined to
		 * divide load round a knot, not that anything is carrying the piece — so branching on
		 * it here would amount to claiming a stranded brick should hang in the air.
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
	 * Refused outright if the input is already out of step, and nothing is written.
	 *
	 * RunningBond can genuinely produce such a layout: an infinite brick dimension passes its
	 * !(x > 0.0) spec guard, PieceMassKg answers with something FStructure::AddPiece refuses,
	 * and the box is appended regardless — so Boxes ends up one longer than the piece array.
	 * Copying that index-by-index propagates the desync into the one type whose contract is
	 * that its two arrays cannot desync, and the AddPiece-return guard already queued on
	 * FStructureBinding would not close it: the mass handed over is a real one belonging to
	 * the wrong piece, or a zero, both of which the door accepts.
	 *
	 * The actor list is the third parallel array and comes from a different place again —
	 * whoever spawned the bricks. Too few and a handle binds to nothing; too many and there
	 * are actors in the world no piece will ever name, the direction that looks fine.
	 *
	 * An empty layout is refused as well. RunningBond rejects every spec that would produce
	 * one, so accepting it here would make this the more permissive of the two, and a caller
	 * handed true would have a binding it can never see anything from.
	 */
	if (PieceCount < 1 || Layout.Boxes.Num() != PieceCount || Actors.Num() != PieceCount)
	{
		return false;
	}

	/*
	 * A replay, correct only because a layout is append-only: RunningBond adds pieces and
	 * connections and never removes one, so no slot is tombstoned and handle i of the layout
	 * is handle i of the binding by construction — what makes copying by index sound. The
	 * moment a producer removes a piece or reuses a slot, this has to replay the holes too, or
	 * every connection above the first one names the wrong pieces.
	 */
	for (int32 PieceIndex = 0; PieceIndex < PieceCount; ++PieceIndex)
	{
		const FStructurePiece& Piece = Layout.Structure.GetPiece(PieceIndex);

		/*
		 * The material rides across with the mass: dropping it here, as this replay once did,
		 * leaves the adopted joint reading its bare connection instead of the weakest-link
		 * crush of its two faces, making a cross-material bearing inert in the played world.
		 */
		Out.AddPiece(
			Piece.MassKg, Piece.bIsGrounded, Actors[PieceIndex], Layout.Boxes[PieceIndex], Piece.Material);
	}

	/* Joints go over whole, pairing and normal together: a normal inconsistent with its
	 * A/B pairing is the one thing the produced graph cannot survive. */
	for (int32 JointIndex = 0; JointIndex < Layout.Structure.NumConnections(); ++JointIndex)
	{
		Out.AddConnection(Layout.Structure.GetConnection(JointIndex));
	}

	/*
	 * The 3D flag rides across with the graph. A layout laid genuinely three-dimensional
	 * (DestructionShed3D::Build flags it) carries that as structure-level state, not on any
	 * piece or joint, so the per-piece and per-joint replays above cannot bring it over —
	 * dropping it here, as this replay once did, leaves the world bridge posing the shed in
	 * 2D, refusing its out-of-plane corner joints, so the overhang never falls when the post
	 * is cut.
	 */
	Out.SetThreeDimensional(Layout.Structure.IsThreeDimensional());

	return true;
}
