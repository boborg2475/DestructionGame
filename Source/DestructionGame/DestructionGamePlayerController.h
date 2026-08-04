// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/PieceMenu.h"
#include "GameFramework/PlayerController.h"
#include "DestructionGamePlayerController.generated.h"

class UInputAction;
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

	/**
	 * Inspect whatever this ray hits: open the piece menu for it, or dismiss the menu.
	 *
	 * A RAY IN RATHER THAN A CLICK, and the split is deliberate. Turning the cursor into a
	 * world ray needs a viewport, which a headless test does not have; everything AFTER
	 * that — trace, resolve, build the menu, present or dismiss it — is the part that can
	 * be wrong in a way a player would notice, and it needs only a world. So the input
	 * handler deprojects and calls this, and this is what a test drives.
	 *
	 * @return the rows now presented. EMPTY MEANS THE MENU WAS DISMISSED, which is the
	 *         answer for a ray that hit the floor, hit nothing, or hit a brick standing for
	 *         a piece that has gone — and it must actually dismiss rather than leave the
	 *         previous piece's menu on screen naming a brick the player is no longer
	 *         pointing at.
	 */
	TArray<FPieceMenuRow> InspectAlongRay(const FVector& StartCm, const FVector& EndCm);

protected:

	/** Input Mapping Contexts applied on possession */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

	/**
	 * The input that opens the piece context menu.
	 *
	 * ON THE CONTROLLER RATHER THAN THE PAWN: the controller already owns the mapping
	 * contexts, it outlives any pawn, and it is what carries the cursor this action needs.
	 * Resolved by the path RequiredContent.h names, so this and the required-content table
	 * cannot become two lists that disagree.
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> InspectPieceAction;

	/** Input mapping context setup */
	virtual void SetupInputComponent() override;
};
