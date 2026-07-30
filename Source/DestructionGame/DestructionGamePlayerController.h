// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "DestructionGamePlayerController.generated.h"

class UInputMappingContext;

/**
 *  Player Controller for the destruction sandbox.
 *  Applies the input mapping contexts and overrides the Player Camera Manager class.
 */
UCLASS(config="Game")
class DESTRUCTIONGAME_API ADestructionGamePlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	/** Constructor */
	ADestructionGamePlayerController();

protected:

	/** Input Mapping Contexts applied on possession */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

	/** Input mapping context setup */
	virtual void SetupInputComponent() override;
};
