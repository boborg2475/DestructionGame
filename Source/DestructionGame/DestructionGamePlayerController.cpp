// Copyright Epic Games, Inc. All Rights Reserved.


#include "DestructionGamePlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "UObject/ConstructorHelpers.h"
#include "Core/PieceActions.h"
#include "DestructionGameCameraManager.h"
#include "RequiredContent.h"
#include "World/DestructionStructureSubsystem.h"

ADestructionGamePlayerController::ADestructionGamePlayerController()
{
	// set the player camera manager class
	PlayerCameraManagerClass = ADestructionGameCameraManager::StaticClass();

	/*
	 * wire up the mapping contexts here rather than in a Blueprint, so the sandbox
	 * runs from C++ defaults alone — by the paths RequiredContent.h names, so this
	 * constructor and the required-content table cannot become two lists that disagree
	 */
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultContext(DestructionContent::DefaultMappingContextPath);
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookContext(DestructionContent::MouseLookMappingContextPath);

	DefaultMappingContexts.Add(DefaultContext.Object);
	DefaultMappingContexts.Add(MouseLookContext.Object);

	/* the piece menu's own input, by the same one spelling of its path */
	static ConstructorHelpers::FObjectFinder<UInputAction> InspectPieceActionAsset(DestructionContent::InspectPieceActionPath);

	InspectPieceAction = InspectPieceActionAsset.Object;
}

TArray<FPieceMenuRow> ADestructionGamePlayerController::InspectAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	UWorld* const World = GetWorld();

	UDestructionStructureSubsystem* const Subsystem =
		World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;

	/*
	 * NO WORLD OR NO SUBSYSTEM DISMISSES THE MENU RATHER THAN LEAVING IT UP. Every route
	 * out of this function that is not a live piece returns an empty list, which is what
	 * makes "the ray hit nothing" and "the ray hit a brick standing for a piece that has
	 * gone" the same code path — a handler that simply returned on a miss would leave the
	 * previous brick's menu on screen naming a brick the player is no longer pointing at.
	 */
	if (Subsystem == nullptr)
	{
		return TArray<FPieceMenuRow>();
	}

	/*
	 * THE WHOLE CHAIN IS ALREADY WRITTEN AND NONE OF IT IS REPEATED HERE. TracePiece fails
	 * closed on every step from the trace to the re-resolve, so a miss arrives as a default
	 * ref; PieceActionsFor resolves that ref again against the binding and answers an empty
	 * menu for one that names nothing. This is the wire between them, not a third opinion.
	 */
	const FPieceHit Hit = Subsystem->TracePiece(StartCm, EndCm);

	const FStructureBinding* const Binding = Subsystem->Find(Hit.Ref.StructureId);

	if (Binding == nullptr)
	{
		return TArray<FPieceMenuRow>();
	}

	return BuildPieceMenuRows(PieceActionsFor(*Binding, Hit.Ref), Hit.Ref);
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
