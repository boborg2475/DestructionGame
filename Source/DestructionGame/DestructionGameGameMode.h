// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
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

protected:

	/** Builds the scenario wall, so pressing Play shows something. */
	virtual void BeginPlay() override;

private:

	/** Set by the begin-play build; INDEX_NONE until then and if the build is refused. */
	int32 BuiltStructureId = INDEX_NONE;
};
