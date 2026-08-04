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

	/**
	 * Put these rows on screen as the piece menu, replacing whatever was up.
	 *
	 * ROWS ONLY, NO SEPARATE REF. FPieceMenuRow already carries the ref it commits against,
	 * and Core/PieceMenu.h says why that is the whole reason the row is a struct: a presenter
	 * that remembered the ref beside the rows is one stale field away from committing the
	 * right action against the wrong brick. A second ref parameter here would re-open exactly
	 * that, one layer up.
	 *
	 * AN EMPTY LIST DISMISSES rather than showing an empty box. Every miss route out of
	 * InspectAlongRay produces an empty list, so "the ray hit nothing" and "take the menu
	 * down" have to be the same call or the previous brick's menu stays on screen.
	 *
	 * @return whether a menu is now presented — false for an empty list.
	 */
	bool ShowPieceMenu(TArrayView<const FPieceMenuRow> Rows);

	/** Take the piece menu down. @return whether there was one to take down. */
	bool DismissPieceMenu();

	/** Whether a piece menu is currently presented. */
	bool IsPieceMenuShown() const;

	/** The rows currently presented, empty when no menu is up. */
	TArrayView<const FPieceMenuRow> GetShownPieceMenuRows() const;

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

	/**
	 * The always-on free-look context, named separately because the menu turns it off.
	 *
	 * IT IS ALSO IN DefaultMappingContexts, AND IT IS THE SAME POINTER — this is a name for
	 * one of the contexts that are applied, not a second one. IMC_MouseLook binds the raw
	 * Mouse2D axis unconditionally with no held button, so the camera follows the mouse all
	 * the time and there is no pointer; a menu drawn on top of that is unusable. IMC_Default
	 * is deliberately NOT named here and is never removed, because it carries IA_InspectPiece,
	 * which is how the player closes the menu by clicking somewhere else.
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputMappingContext> MouseLookMappingContext;

	/** Input mapping context setup */
	virtual void SetupInputComponent() override;

private:

	/**
	 * IA_InspectPiece's handler: turn the cursor into a world ray and inspect along it.
	 *
	 * A DEPROJECTION PLUS ONE CALL, AND NOTHING ELSE, BY DESIGN. DeprojectMousePositionToWorld
	 * needs a viewport and a headless run has none, so this is the one link in the chain no
	 * test can reach — which is exactly why everything that can be wrong in a way a player
	 * would notice lives behind InspectAlongRay instead. Anything added here grows the
	 * untested surface.
	 */
	void OnInspectPiece();

	/**
	 * Hand the controls to the menu, or give them back.
	 *
	 * ONE FUNCTION WITH ONE BRANCH RATHER THAN TWO, so the restore cannot quietly fall out of
	 * step with the apply — forgetting it leaves the player with a cursor and no camera, which
	 * is unrecoverable rather than merely untidy. A controller with no ULocalPlayer has no
	 * Enhanced Input subsystem to remove a context from, and that fails closed here.
	 */
	void SetPieceMenuControls(bool bMenuIsUp);

	/** The rows on screen right now. Empty means no menu. */
	TArray<FPieceMenuRow> ShownPieceMenuRows;
};
