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
	 * THE ID RATHER THAN THE BINDING, because the subsystem owns the structures and Find is
	 * the one route to one. Handing back a binding here would make the game mode a second
	 * owner of something whose lifetime it does not control.
	 */
	int32 GetBuiltStructureId() const { return BuiltStructureId; }

	/**
	 * WHICH CATALOGUE ROW THIS LEVEL CHOSE, and HOW it came to choose it.
	 *
	 * STATE RATHER THAN A LOG LINE. A warning and an on-screen label are both thin reads of
	 * these two, and matching on log TEXT is fragile — so this pair is the assertable shape
	 * and whatever is printed is downstream of it.
	 *
	 * THE PAIR, NEVER THE INDEX ALONE. A URL naming a scenario that does not exist and a URL
	 * naming nothing at all both land on the same row, and a player who mistyped has to be
	 * able to tell those apart from outside.
	 */
	int32 GetSelectedScenarioRow() const { return SelectedScenarioRow; }

	DestructionScenarios::EScenarioSelection GetScenarioSelection() const
	{
		return SelectedScenarioHow;
	}

	/**
	 * WHAT THE PLAYER IS TOLD THEY ARE LOOKING AT, right now.
	 *
	 * COMPUTED ON DEMAND RATHER THAN STORED, because half of it is a clock: the countdown to an
	 * armed cut changes every frame, and a stored copy would need something to refresh it. The
	 * game mode owns the two facts a label cannot derive — which row it RECORDED building, and
	 * how much of the cut's delay is left — and DestructionScenarios::BuildScenarioLabel turns
	 * those into the words, world-free, where a test can read them.
	 */
	DestructionScenarios::FScenarioLabel GetScenarioLabel() const;

protected:

	/** Builds the selected scenario, frames it for the player, and arms its hold. */
	virtual void BeginPlay() override;

private:

	/**
	 * RUN THE LEVEL: take out the bricks the scenario names, and settle what is left.
	 *
	 * ONCE, WHEN THE HOLD EXPIRES, AND ON EVERY ROW RATHER THAN ONLY ON A CUTTING ONE. Seven of
	 * the nine rows cut nothing at all — a corbel is condemned by its own geometry — so a moment
	 * that only ever fired for a cut left those levels with nothing to happen at, which is why
	 * they used to settle on the frame they were built and be over before the player's first
	 * frame was drawn.
	 *
	 * THROUGH THE COMMIT PATH THE PLAYER'S OWN DELETE TAKES, so what a level shows is
	 * reproducible by clicking: one settle, one push, and the orphaned bricks destroyed.
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
	 * REFS RATHER THAN HANDLES, because a handle is a momentary answer and this one is held
	 * across seconds of play in which the player may have deleted the very brick — which is
	 * exactly what the commit path's re-resolve refuses on. Plain ints, no UObject, so there
	 * is nothing here for the garbage collector to keep alive.
	 */
	TArray<FPieceRef> ScenarioCutRefs;

	/**
	 * The hold: how long the structure is left exactly as laid before the level runs.
	 *
	 * ONE CLOCK, NOT TWO. It is what the cut is armed on AND what a no-cut row settles on, so
	 * there is a single answer to "when does this level do its thing" — and the label's
	 * countdown, which reads this handle's own remainder, can never announce one moment while
	 * another one fires.
	 *
	 * A TIMER RATHER THAN A TICK, and a game mode torn down mid-hold takes it with it:
	 * UWorld::DestroyActor clears every timer for the actor being destroyed, and the timer
	 * manager itself dies with the world. The world tests build and tear down worlds
	 * repeatedly, so a callback that outlived its game mode would show up as a crash rather
	 * than as a failure.
	 */
	FTimerHandle ScenarioHoldTimer;

	/**
	 * Whether that cut has already run, which is what turns the label's countdown into a report.
	 *
	 * A FLAG RATHER THAN A DEAD TIMER HANDLE READ BACKWARDS. The handle is invalid both after the
	 * cut has fired and before anything armed it, so a level whose timer never started — a row
	 * with no cut, or a delay the timer manager refused — would read as one that had already cut,
	 * and the player would be told the brick was out while looking straight at it.
	 */
	bool bScenarioCutHasFired = false;
};
