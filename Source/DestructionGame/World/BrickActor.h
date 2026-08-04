// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/StructureBinding.h"
#include "GameFramework/Actor.h"
#include "BrickActor.generated.h"

class UStaticMeshComponent;

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

	/** Who this brick is, as the solver names it. */
	const FPieceRef& GetPieceRef() const;

	/** Told once, at spawn. */
	void SetPieceRef(const FPieceRef& Ref);

private:

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Mesh;

	FPieceRef PieceRef;
};
