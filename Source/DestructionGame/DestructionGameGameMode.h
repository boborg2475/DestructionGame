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

	/** Id of the structure built at BeginPlay, or INDEX_NONE. The subsystem owns the binding. */
	int32 GetBuiltStructureId() const { return BuiltStructureId; }

	/**
	 * The catalogue row this level chose, and how. Both are needed: an unknown scenario name and no
	 * name land on the same row.
	 */
	int32 GetSelectedScenarioRow() const { return SelectedScenarioRow; }

	DestructionScenarios::EScenarioSelection GetScenarioSelection() const
	{
		return SelectedScenarioHow;
	}

	/** The on-screen scenario label, computed each call since it includes a countdown. */
	DestructionScenarios::FScenarioLabel GetScenarioLabel() const;

protected:

	/** Builds the selected scenario, frames it for the player, and arms its hold. */
	virtual void BeginPlay() override;

private:

	/**
	 * Remove the scenario's cut bricks (if any) and settle. Fires once when the hold expires, on
	 * every row, through the same commit path as a player delete.
	 */
	void RunScenario();

	/** Set by the begin-play build; INDEX_NONE until then and if the build is refused. */
	int32 BuiltStructureId = INDEX_NONE;

	/** The catalogue row begin-play selected; INDEX_NONE until then. */
	int32 SelectedScenarioRow = INDEX_NONE;

	/** How that row was reached. */
	DestructionScenarios::EScenarioSelection SelectedScenarioHow =
		DestructionScenarios::EScenarioSelection::Default;

	/** Bricks the armed cut will remove. Refs, since the player may delete one during the hold. */
	TArray<FPieceRef> ScenarioCutRefs;

	/**
	 * The hold before RunScenario. One timer drives both the cut and the label's countdown. A timer
	 * rather than a tick, so it is cleared if the game mode is destroyed mid-hold.
	 */
	FTimerHandle ScenarioHoldTimer;

	/** Whether the cut has run. A flag because the timer handle is also invalid before arming. */
	bool bScenarioCutHasFired = false;
};
