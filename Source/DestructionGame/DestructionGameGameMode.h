// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/StructureBinding.h"
#include "Engine/TimerHandle.h"
#include "GameFramework/GameModeBase.h"
#include "World/DestructionScenarios.h"
#include "World/ScenarioLabel.h"
#include "DestructionGameGameMode.generated.h"

/**
 *  GameMode for the destruction sandbox.
 *  Spawns the free-flying observer pawn; carries no win/lose rules.
 */
UCLASS()
class DESTRUCTIONGAME_API ADestructionGameGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ADestructionGameGameMode();

	/**
	 * The structure this game mode built when play began, or INDEX_NONE if it built none.
	 *
	 * The id rather than the binding: the subsystem owns the structures and Find is the one
	 * route to one, so returning a binding would make this a second owner of its lifetime.
	 */
	int32 GetBuiltStructureId() const { return BuiltStructureId; }

	/**
	 * Which catalogue row this level chose, and how it came to choose it.
	 *
	 * State rather than a log line, since matching on log text is fragile — and the pair, never
	 * the index alone, since a URL naming a nonexistent scenario and one naming nothing at all
	 * land on the same row and must still be tellable apart.
	 */
	int32 GetSelectedScenarioRow() const { return SelectedScenarioRow; }

	DestructionScenarios::EScenarioSelection GetScenarioSelection() const
	{
		return SelectedScenarioHow;
	}

	/**
	 * What the player is told they are looking at, right now.
	 *
	 * Computed on demand rather than stored: half of it is a clock, the countdown to an armed
	 * cut, which changes every frame. DestructionScenarios::BuildScenarioLabel turns the row
	 * and the cut's remaining delay into words, world-free and testable.
	 */
	DestructionScenarios::FScenarioLabel GetScenarioLabel() const;

protected:

	/** Builds the selected scenario, frames it for the player, and arms its hold. */
	virtual void BeginPlay() override;

private:

	/**
	 * Run the level: take out the bricks the scenario names, and settle what is left.
	 *
	 * Fires once, when the hold expires, on every row rather than only a cutting one — seven of
	 * nine rows cut nothing and are condemned by their own geometry, and used to settle on the
	 * frame they were built. Goes through the same commit path the player's own delete takes, so
	 * what a level shows is reproducible by clicking.
	 */
	void RunScenario();

	/** Set by the begin-play build; INDEX_NONE until then and if the build is refused. */
	int32 BuiltStructureId = INDEX_NONE;

	/** The catalogue row begin-play selected; INDEX_NONE until then. */
	int32 SelectedScenarioRow = INDEX_NONE;

	/** How that row was reached. See DestructionScenarios::EScenarioSelection. */
	DestructionScenarios::EScenarioSelection SelectedScenarioHow =
		DestructionScenarios::EScenarioSelection::Default;

	/**
	 * The bricks the armed cut will take, resolved when the wall was laid.
	 *
	 * Refs rather than handles: this is held across seconds of play in which the player may
	 * have deleted the brick, which the commit path's re-resolve refuses on.
	 */
	TArray<FPieceRef> ScenarioCutRefs;

	/**
	 * The hold: how long the structure is left exactly as laid before the level runs.
	 *
	 * One clock, not two: it is what the cut is armed on and what a no-cut row settles on, so
	 * the label's countdown can never announce one moment while another fires.
	 *
	 * A timer rather than a tick — a game mode torn down mid-hold takes it with it, since
	 * UWorld::DestroyActor clears every timer for the actor and the timer manager dies with the
	 * world. Repeated world tests would otherwise crash on an outliving callback.
	 */
	FTimerHandle ScenarioHoldTimer;

	/**
	 * Whether that cut has already run, which turns the label's countdown into a report.
	 *
	 * A flag rather than a dead timer handle read backwards: the handle is invalid both after
	 * the cut fires and before anything armed it, so a never-started timer would otherwise read
	 * as already cut, telling the player the brick was out while they looked straight at it.
	 */
	bool bScenarioCutHasFired = false;
};
