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
 * FOUR STATES, NOT A BOOLEAN, because they render differently and mean different things:
 * hover is "this is what you would hit", selected is "this is what the menu is about", and
 * inspected is "this is the brick the joint readout is describing". One flag would make a
 * hovered brick and a chosen one indistinguishable to everything downstream, and the day
 * they draw the same is the day a player deletes the brick they were only pointing at.
 *
 * THE STRONGEST STATE WINS WHERE THEY COINCIDE, AND THE ORDER IS Inspected > Selected >
 * Hovered. A brick whose joint forces are on screen has to be distinguishable from the five
 * others the player also picked, or the readout is ambiguous about which brick it describes.
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
	Inspected
};

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

	FPieceRef PieceRef;

	EBrickHighlight Highlight = EBrickHighlight::None;
};
