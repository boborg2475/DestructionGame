// Copyright Epic Games, Inc. All Rights Reserved.

#include "DestructionGameGameMode.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "DestructionGameFlyingPawn.h"
#include "DestructionGamePlayerController.h"
#include "Engine/World.h"
#include "World/DestructionStructureSubsystem.h"

/*
 * File-local names carry a GameMode prefix. An anonymous namespace is private to a
 * TRANSLATION UNIT rather than to a file, and a unity build merges many files into one — so
 * two file-local names that collide are a hard compile error between files that never refer
 * to each other. See CURRENT_STATE.md.
 */
namespace
{
	/**
	 * The wall the game mode puts up on Play: 30 bricks across, 40 courses, flush ends.
	 *
	 * FLUSH RATHER THAN RAGGED, so the wall has square ends and so the scenario exercises
	 * the mixed-size case — half bats at alternating course ends mean two piece sizes and
	 * two masses rather than 1,220 copies of one box.
	 *
	 * NOTHING HERE IS DERIVED TWICE. The dimensions and the joint thickness are a spec;
	 * DestructionLayout::RunningBond turns them into boxes, masses, pairs and joints, and
	 * UDestructionStructureSubsystem::BuildRunningBond turns those into a wall standing in
	 * the world. This names no position, no mass and no joint of its own.
	 */
	DestructionLayout::FRunningBondSpec GameModeScenarioWallSpec()
	{
		DestructionLayout::FRunningBondSpec Spec;

		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = DestructionProfiles::ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = 40;
		Spec.BricksPerCourse = 30;
		Spec.End = DestructionLayout::EWallEnd::Flush;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		return Spec;
	}
}

ADestructionGameGameMode::ADestructionGameGameMode()
{
	DefaultPawnClass = ADestructionGameFlyingPawn::StaticClass();
	PlayerControllerClass = ADestructionGamePlayerController::StaticClass();
}

void ADestructionGameGameMode::BeginPlay()
{
	Super::BeginPlay();

	UWorld* const World = GetWorld();

	UDestructionStructureSubsystem* const Subsystem =
		World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;

	if (Subsystem == nullptr)
	{
		return;
	}

	/*
	 * A REFUSED BUILD LEAVES BuiltStructureId AT INDEX_NONE, which is what it already means:
	 * the game mode built no structure. BuildRunningBond spends no id on a refusal either, so
	 * there is no half-built wall to reconcile.
	 */
	BuiltStructureId = Subsystem->BuildRunningBond(GameModeScenarioWallSpec());

	/*
	 * AND THE WALL IS LEFT SOLVED, WHICH IS A SEPARATE ACT FROM BUILDING IT. A structure
	 * nobody has solved has no support answer for any piece, and EPieceSupport::Falling is
	 * what an ABSENT answer reads as — so an unsolved wall is indistinguishable from one in
	 * free fall to everything that asks. SolveAndPush is the wire that both solves and makes
	 * the world agree with the answer, and it releases nothing from a wall that is standing.
	 *
	 * An id of INDEX_NONE names no structure, and SolveAndPush already answers that with
	 * zero, so a refused build needs no branch of its own here.
	 */
	Subsystem->SolveAndPush(BuiltStructureId);
}
