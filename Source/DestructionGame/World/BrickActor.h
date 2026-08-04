// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/StructureBinding.h"
#include "GameFramework/Actor.h"
#include "BrickActor.generated.h"

class UStaticMeshComponent;

/**
 * How a brick is currently called out to the player.
 *
 * THREE STATES, NOT A BOOLEAN, because hover and selected render differently and mean
 * different things: hover is "this is what you would hit", selected is "this is what the
 * menu is about". One flag would make a hovered brick and a chosen one indistinguishable to
 * everything downstream, and the day they draw the same is the day a player deletes the
 * brick they were only pointing at.
 *
 * None IS THE ZERO ENUMERATOR, deliberately, so a zero-initialised brick is a plain one
 * rather than one claiming to be selected — the same reason EPieceSupport::Falling is zero.
 *
 * THE STATE IS TESTED AND THE MATERIAL IS NOT. What a brick LOOKS like is the same untestable
 * inch as the menu widget, so the untested half is kept to exactly "read this flag, set a
 * material" and every decision about WHICH state a brick is in lives out here where it can be
 * asserted.
 */
enum class EBrickHighlight : uint8
{
	None = 0,
	Hovered,
	Selected
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

	FPieceRef PieceRef;

	EBrickHighlight Highlight = EBrickHighlight::None;
};
