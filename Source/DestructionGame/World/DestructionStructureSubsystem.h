// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"
#include "Core/StructureBinding.h"
#include "Subsystems/WorldSubsystem.h"
#include "DestructionStructureSubsystem.generated.h"

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

private:

	TMap<int32, TUniquePtr<FStructureBinding>> Structures;

	int32 NextStructureId = 0;
};
