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
};
