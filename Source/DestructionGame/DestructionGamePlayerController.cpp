// Copyright Epic Games, Inc. All Rights Reserved.


#include "DestructionGamePlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "UObject/ConstructorHelpers.h"
#include "DestructionGameCameraManager.h"

ADestructionGamePlayerController::ADestructionGamePlayerController()
{
	// set the player camera manager class
	PlayerCameraManagerClass = ADestructionGameCameraManager::StaticClass();

	// wire up the mapping contexts here rather than in a Blueprint, so the sandbox
	// runs from C++ defaults alone
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultContext(TEXT("/Game/Input/IMC_Default.IMC_Default"));
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookContext(TEXT("/Game/Input/IMC_MouseLook.IMC_MouseLook"));

	DefaultMappingContexts.Add(DefaultContext.Object);
	DefaultMappingContexts.Add(MouseLookContext.Object);
}

void ADestructionGamePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// only add IMCs for local player controllers
	if (IsLocalPlayerController())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
			{
				Subsystem->AddMappingContext(CurrentContext, 0);
			}
		}
	}
}
