// Copyright Epic Games, Inc. All Rights Reserved.

#include "DestructionGameGameMode.h"
#include "DestructionGameFlyingPawn.h"
#include "DestructionGamePlayerController.h"

ADestructionGameGameMode::ADestructionGameGameMode()
{
	DefaultPawnClass = ADestructionGameFlyingPawn::StaticClass();
	PlayerControllerClass = ADestructionGamePlayerController::StaticClass();
}
