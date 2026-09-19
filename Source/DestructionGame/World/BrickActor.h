// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/StructureBinding.h"
#include "GameFramework/Actor.h"
#include "BrickActor.generated.h"

class UMaterialInterface;
class UStaticMeshComponent;

/* Core/PieceMenu.h's band, forward-declared so World does not drag the whole presenter in. */
enum class EJointMarginBand : uint8;

/**
 * How a brick is currently called out to the player.
 *
 * Ten states, not a boolean, because they render differently and mean different things:
 * hover is "this is what you would hit", selected is "this is what the menu is about",
 * inspected is "this is the brick the joint readout is describing", and the six below are
 * "this is the brick on the far end of joint row N". One flag would make a hovered brick and
 * a chosen one indistinguishable downstream, and the day they draw the same is the day a
 * player deletes the brick they were only pointing at.
 *
 * The strongest state wins where they coincide, order Inspected > Selected > Neighbour >
 * Hovered. A brick whose joint forces are on screen must be distinguishable from the five
 * others the player also picked. The order is a judgement, not a deduction, and is argued
 * (not inferred from the layout below) where it is decided —
 * ADestructionGamePlayerController::HighlightForPiece, pinned by
 * World.Select.ClickingTogglesTheSelectionAndHoverHighlights.
 *
 * None is the zero enumerator, deliberately, so a zero-initialised brick is a plain one
 * rather than one claiming to be selected — the same reason EPieceSupport::Falling is zero.
 *
 * Which material a state asks for is tested; what it looks like is not. A code-built world
 * has no renderer, but which asset the brick handed the component needs nothing else, so the
 * untested inch is exactly "does the shader look nice" and no wider. Which state a brick is
 * in still lives in the controller, where it can be asserted.
 */
enum class EBrickHighlight : uint8
{
	None = 0,
	Hovered,
	Selected,
	Inspected,

	/*
	 * Six more for "this is the brick on the far end of joint row N", one per colour slot of
	 * the readout — the states that tie a row of numbers to a brick a player can see.
	 *
	 * Six enumerators rather than one plus a slot index. SetHighlighted takes the state the
	 * brick should now be in and nothing else, which is what makes it idempotent and
	 * order-free; a slot carried alongside would be a second field free to disagree with the
	 * first — a brick reading Neighbour with slot 4 left over from the last time. GetHighlight
	 * would stop being the whole answer, and World.Brick.HighlightWearsAMaterial's "the overlay
	 * depends only on the state" would become unsayable.
	 *
	 * The numeric order is not the precedence order: which state wins where several apply is
	 * HighlightForPiece's decision; see World.Select for the order it pins.
	 */
	Neighbour0,
	Neighbour1,
	Neighbour2,
	Neighbour3,
	Neighbour4,
	Neighbour5,

	/*
	 * And three more for "this is how hard this piece's worst joint is working", one per band
	 * of EJointMarginBand — the load overlay's states: a wall tinted green through amber to red
	 * at its base, so the player can see where the load is before pulling anything out.
	 *
	 * The weakest states in the precedence (HighlightForPiece's decision): the overlay covers
	 * every live piece at once, so anything that says "this one" — cursor, selection, the
	 * readout's own colours — must beat it, or turning the overlay on would take away the one
	 * thing a player checks before pressing Delete.
	 *
	 * Still not read from the numeric order: these are last in the enum and last in the
	 * precedence, and that agreement is a coincidence nothing may read into.
	 */
	LoadComfortable,
	LoadCaution,
	LoadCritical
};

/**
 * The state a brick is in when it is the far end of the joint row wearing colour slot N.
 *
 * The one mapping between a slot and a state, shared rather than inverted: the controller has
 * a slot (FInspectorJointRow::ColourSlot) and needs the state; the brick has the state and
 * needs the material its slot names. Two functions would be two tables, and a slot mapped one
 * way here and another there is a joint row lit in a different row's colour.
 *
 * A switch rather than arithmetic on the enumerator: `Neighbour0 + Slot` would bake an
 * assumption about the enum's layout into every caller, and EBrickHighlight promises nothing
 * about layout except that None is zero.
 *
 * Fails closed to None, the right answer for INDEX_NONE and for anything past the end of the
 * palette: FInspectorJointRow::ColourSlot hands out INDEX_NONE rather than wrapping, so a row
 * past the end draws no colour instead of borrowing somebody else's.
 */
EBrickHighlight BrickHighlightForNeighbourSlot(int32 ColourSlot);

/**
 * The state a piece is in when the load overlay is on and its worst joint sits in this band.
 *
 * The one mapping between a band and a state, for the same reason BrickHighlightForNeighbourSlot
 * is the one mapping between a slot and a state: a second table anywhere would be an overlay
 * drawing amber for a joint the readout beside it calls comfortable.
 *
 * A switch rather than arithmetic on the enumerator, again because EBrickHighlight promises
 * nothing about layout except that None is zero.
 *
 * Fails closed to None on a band this build has never heard of: EJointMarginBand is a uint8
 * and a cast is all it takes to make one, and a piece with no answer must draw plain rather
 * than a plausible green — a false claim of nowhere near failing.
 */
EBrickHighlight BrickHighlightForLoadBand(EJointMarginBand Band);

/**
 * One brick in the world: the actor a piece handle points at.
 *
 * Lives in World/, not Core/. Core is world-free and actor-free, which is why the whole
 * solver suite runs in about a second; an AActor in there would drag Engine into every
 * arithmetic test.
 *
 * Does not tick — a scale decision, not an economy. The solver is pushed
 * (FStructureBinding::ApplyResults hands each solve's answer to the bricks), so nothing
 * about a brick runs every frame; a per-brick tick is what would make thousands of actors
 * expensive rather than merely numerous.
 */
UCLASS()
class DESTRUCTIONGAME_API ABrickActor : public AActor
{
	GENERATED_BODY()

public:

	ABrickActor();

	/** The brick's body. Movable from spawn, kinematic until Release. */
	UStaticMeshComponent* GetMesh() const;

	/** Hand this brick to physics. IDEMPOTENT, and there is deliberately no way back. */
	void Release();

	/**
	 * Call this brick out to the player, or stop calling it out.
	 *
	 * Idempotent and order-free: the argument is the state the brick should now be in, not a
	 * nudge, so nothing has to remember what it set last or unwind it in the right order.
	 *
	 * The overlay is additive, which is why it is an overlay: swapping slot 0 would mean
	 * remembering the brick's own material and restoring it; clearing an overlay is one null.
	 */
	void SetHighlighted(EBrickHighlight Highlight);

	/** How this brick is currently called out. None unless something said otherwise. */
	EBrickHighlight GetHighlight() const;

	/** Who this brick is, as the solver names it. */
	const FPieceRef& GetPieceRef() const;

	/** Told once, at spawn. */
	void SetPieceRef(const FPieceRef& Ref);

private:

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Mesh;

	/**
	 * What each called-out state wears, resolved once onto the CDO by the paths
	 * RequiredContent.h names.
	 *
	 * Three named properties rather than one array indexed by the enum, because None wears
	 * nothing: an array would carry a null in its first slot, and the required-content sweep
	 * that reads these back cannot tell a deliberate null from a reference that stopped
	 * resolving. Adding a further state means adding a property beside these.
	 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> HoverMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SelectedMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> InspectedMaterial;

	/**
	 * What each band of the load overlay wears, on the same terms as the three above: one
	 * property per state, since an array indexed by state would carry a null the
	 * required-content sweep could not tell from a broken reference.
	 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> LoadComfortableMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> LoadCautionMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> LoadCriticalMaterial;

	/**
	 * What each colour slot of the joint readout wears, indexed by the slot itself.
	 *
	 * An array here and named properties above, and the difference is what the index means:
	 * the properties above are indexed by a state, and None wears nothing, so an array there
	 * would carry an ambiguous null. These are indexed by a number the model computed, every
	 * entry of which names a real asset, so the array has no hole to misread.
	 */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> NeighbourMaterials;

	FPieceRef PieceRef;

	EBrickHighlight Highlight = EBrickHighlight::None;
};
