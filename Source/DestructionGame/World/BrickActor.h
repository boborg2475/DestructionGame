// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/StructureBinding.h"
#include "GameFramework/Actor.h"
#include "BrickActor.generated.h"

class UMaterialInterface;
class UStaticMeshComponent;

/**
 * How a brick is currently called out to the player.
 *
 * TEN STATES, NOT A BOOLEAN, because they render differently and mean different things:
 * hover is "this is what you would hit", selected is "this is what the menu is about",
 * inspected is "this is the brick the joint readout is describing", and the six below are
 * "this is the brick on the far end of joint row N". One flag would make a hovered brick and
 * a chosen one indistinguishable to everything downstream, and the day they draw the same is
 * the day a player deletes the brick they were only pointing at.
 *
 * THE STRONGEST STATE WINS WHERE THEY COINCIDE, AND THE ORDER IS Inspected > Selected >
 * Neighbour > Hovered. A brick whose joint forces are on screen has to be distinguishable from
 * the five others the player also picked, or the readout is ambiguous about which brick it
 * describes. The two ends of the middle pair are judgements rather than deductions and both are
 * argued where the order is decided — ADestructionGamePlayerController::HighlightForPiece, with
 * World.Select.ClickingTogglesTheSelectionAndHoverHighlights pinning it — because the ORDER is
 * not a property of the enum. Nothing here may be inferred from the numeric layout below.
 *
 * None IS THE ZERO ENUMERATOR, deliberately, so a zero-initialised brick is a plain one
 * rather than one claiming to be selected — the same reason EPieceSupport::Falling is zero.
 *
 * WHICH MATERIAL A STATE ASKS FOR IS TESTED; WHAT IT LOOKS LIKE IS NOT. Asking a renderer
 * whether a brick looks different needs a renderer, and a code-built world has none — but
 * which asset the brick handed the component needs nothing but the component, so the untested
 * inch is exactly "does the shader look nice" and no wider. Every decision about WHICH state a
 * brick is in still lives out in the controller where it can be asserted.
 */
enum class EBrickHighlight : uint8
{
	None = 0,
	Hovered,
	Selected,
	Inspected,

	/*
	 * AND SIX MORE FOR "THIS IS THE BRICK ON THE FAR END OF JOINT ROW N", one per colour slot of
	 * the readout. These are the states that tie a row of numbers to a brick a player can see.
	 *
	 * SIX ENUMERATORS RATHER THAN ONE PLUS A SLOT INDEX. SetHighlighted takes THE STATE THE BRICK
	 * SHOULD NOW BE IN and nothing else, which is what makes it idempotent and order-free; a slot
	 * carried alongside would be a second field free to disagree with the first — a brick reading
	 * Neighbour with slot 4 left over from the last time it was one. GetHighlight would stop being
	 * the whole answer, and World.Brick.HighlightWearsAMaterial's "the overlay depends only on the
	 * state" is the assertion that would become unsayable.
	 *
	 * THE NUMERIC ORDER IS NOT THE PRECEDENCE ORDER. Which state wins where several apply is
	 * HighlightForPiece's decision and lives there; see World.Select for the order it pins.
	 */
	Neighbour0,
	Neighbour1,
	Neighbour2,
	Neighbour3,
	Neighbour4,
	Neighbour5
};

/**
 * The state a brick is in when it is the far end of the joint row wearing colour slot N.
 *
 * THE ONE MAPPING BETWEEN A SLOT AND A STATE, AND IT IS SHARED RATHER THAN INVERTED. The
 * controller has a slot — FInspectorJointRow::ColourSlot, the readout's own answer — and needs
 * the state; the brick has the state and needs the material its slot names. Two functions would
 * be two tables, and a slot that mapped one way here and another way there is a joint row
 * pointing at a brick lit in a different row's colour, which is precisely the confusion the
 * palette exists to end.
 *
 * A SWITCH RATHER THAN ARITHMETIC ON THE ENUMERATOR. Neighbour0 + Slot would bake an assumption
 * about the enum's LAYOUT into every caller, and the layout is not something EBrickHighlight
 * promises — None being zero is the only ordering it commits to.
 *
 * FAILS CLOSED TO None, which is the right answer for INDEX_NONE and for anything past the end of
 * the palette: FInspectorJointRow::ColourSlot hands out INDEX_NONE rather than wrapping, exactly
 * so that a row past the end draws no colour instead of borrowing somebody else's.
 */
EBrickHighlight BrickHighlightForNeighbourSlot(int32 ColourSlot);

/**
 * One brick in the world: the actor a piece handle points at.
 *
 * THIS LIVES IN World/, NOT Core/. Core is world-free and actor-free, which is why the
 * whole solver suite runs in about a second; an AActor in there would drag Engine into
 * every arithmetic test.
 *
 * IT DOES NOT TICK, AND THAT IS A SCALE DECISION RATHER THAN AN ECONOMY. The solver is
 * pushed — FStructureBinding::ApplyResults hands each solve's answer to the bricks — so
 * nothing about a brick needs to run every frame, and a per-brick tick is what would make
 * thousands of actors expensive rather than merely numerous.
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
	 * IDEMPOTENT AND ORDER-FREE: the argument is the state the brick should now be in, not a
	 * nudge, so nothing has to remember what it set last or unwind it in the right order.
	 *
	 * THE OVERLAY IS ADDITIVE, WHICH IS WHY IT IS AN OVERLAY. Swapping slot 0 would mean
	 * remembering the brick's own material and putting it back, i.e. a second record of its
	 * appearance and one more thing to leave behind; clearing an overlay is a single null.
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
	 * THREE NAMED PROPERTIES RATHER THAN ONE ARRAY INDEXED BY THE ENUM, because None wears
	 * nothing: an array would carry a null in its first slot, and the required-content sweep
	 * that reads these back cannot tell a deliberate null from a reference that stopped
	 * resolving. Adding a further state is adding a property beside these.
	 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> HoverMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SelectedMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> InspectedMaterial;

	/**
	 * What each COLOUR SLOT of the joint readout wears, indexed by the slot itself.
	 *
	 * AN ARRAY HERE AND NAMED PROPERTIES ABOVE, AND THE DIFFERENCE IS WHAT THE INDEX MEANS. The
	 * three above are indexed by a STATE, and None is a state that wears nothing — so an array of
	 * them would carry a null in its first slot and the required-content sweep that reads these
	 * back could not tell a deliberate null from a reference that stopped resolving. These are
	 * indexed by a NUMBER the model computed, every entry of which names a real asset, so the
	 * array has no hole to misread and a name per slot would only need a switch to turn the
	 * number back into the name.
	 */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> NeighbourMaterials;

	FPieceRef PieceRef;

	EBrickHighlight Highlight = EBrickHighlight::None;
};
