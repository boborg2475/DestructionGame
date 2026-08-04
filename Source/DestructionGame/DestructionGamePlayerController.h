// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/PieceMenu.h"
#include "Core/PieceSelection.h"
#include "GameFramework/PlayerController.h"
#include "Input/Reply.h"
#include "DestructionGamePlayerController.generated.h"

class SWidget;
class UInputAction;
class UInputMappingContext;

/* Declared with its underlying type in World/BrickActor.h, which only the .cpp needs. */
enum class EBrickHighlight : uint8;

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
	 * Call out whatever this ray hits, so the player can see what they would hit.
	 *
	 * SEPARATE FROM InspectAlongRay BECAUSE HOVERING IS NOT CLICKING. This changes nothing but
	 * which brick is called out: no selection is touched, no menu opens and none closes. The
	 * same seam is drawn at the same place for the same reason — the cursor-to-ray half needs a
	 * viewport and this half needs only a world.
	 *
	 * A SELECTED BRICK STAYS SELECTED WHEN IT IS HOVERED. The two states are distinct on
	 * purpose, and the stronger one wins: a brick the menu is about must not start reading as
	 * merely-pointed-at because the cursor drifted over it.
	 *
	 * @return the piece now hovered, or a default ref when the ray hit nothing.
	 */
	FPieceRef HoverAlongRay(const FVector& StartCm, const FVector& EndCm);

	/**
	 * The bricks the player has picked, which is what the menu is about.
	 *
	 * THE COUNT A MENU REPORTS IS Num() ON THIS, and the rows carry the same refs, because the
	 * menu is a projection of this set rebuilt whenever it changes rather than a second record
	 * of it. A MENU IS UP EXACTLY WHEN THIS IS NON-EMPTY.
	 */
	const FPieceSelection& GetPieceSelection() const;

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

	/**
	 * Choose a presented row: commit it and take the menu down.
	 *
	 * AN INDEX INTO THE PRESENTED ROWS IS THE WHOLE INPUT, which is what keeps the widget
	 * half to a single call. A button knows which entry it is and nothing else.
	 *
	 * @return whether the chosen row's action actually committed.
	 */
	bool ChoosePieceMenuRow(int32 RowIndex);

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

	/**
	 * Put one button per presented row on screen, and take them off again.
	 *
	 * THIS IS THE ONE INCH OF THE LOOP NO TEST CAN REACH, and it is deliberately kept to
	 * exactly that: a code-built test world has no UGameViewportClient at all, so
	 * AddViewportWidgetContent has nothing to add to and no headless assertion can see a
	 * button, a click, or a leak. Everything a menu could get wrong — which rows, which ref,
	 * which action, the dismissal, the controls restore, an index naming no row — is already
	 * decided by ChoosePieceMenuRow and the presenter, both of which are covered. So there is
	 * no logic here to be wrong: a button per row, its label off the row, and a click that
	 * calls ChoosePieceMenuRow with its own index and nothing else.
	 *
	 * The build sits beside the Append in ShowPieceMenu and the removal beside the Reset in
	 * DismissPieceMenu, so it inherits the dismiss-then-build discipline that already makes
	 * the controls restore correct — which is what keeps a second add with no matching remove,
	 * the one leak that would go unseen, on the same code path as the asserted one.
	 */
	void BuildPieceMenuWidget();
	void RemovePieceMenuWidget();

	/** A button's click: choose the row it stands for. Nothing else belongs here. */
	FReply OnPieceMenuRowClicked(int32 RowIndex);

	/**
	 * What state this brick should now be called out in, and putting it there.
	 *
	 * ONE FUNCTION DECIDES, AND EVERY ROUTE ASKS IT, which is what makes "the stronger state
	 * wins" true everywhere rather than at the call sites somebody remembered: a selected
	 * brick under the cursor reads Selected, so it cannot flicker back to merely-pointed-at
	 * the moment the player looks at it. Both halves are read back off the selection and the
	 * hovered ref rather than remembered per brick, so there is no third record to drift.
	 *
	 * A ref naming no brick refreshes nothing, which is what makes the pieces a commit has
	 * just destroyed harmless to pass in.
	 */
	EBrickHighlight HighlightForPiece(const FPieceRef& Ref) const;
	void RefreshPieceHighlight(const FPieceRef& Ref);

	/** Point at this piece, or at nothing, letting whatever was pointed at before go. */
	void SetHoveredPiece(const FPieceRef& Ref);

	/**
	 * Pick nothing, and stop calling out everything that was picked.
	 *
	 * THE REFS ARE COPIED OUT FIRST, because Clear empties the very array they live in — the
	 * same trap ChoosePieceMenuRow copies its row for. A cleared set that left bricks lit
	 * tells the player they still have a selection.
	 */
	void ClearPieceSelection();

	/** The rows on screen right now. Empty means no menu. */
	TArray<FPieceMenuRow> ShownPieceMenuRows;

	/** The bricks picked so far. Empty means no menu, because the menu is built from it. */
	FPieceSelection PieceSelection;

	/**
	 * The one brick under the cursor, or a default ref for none.
	 *
	 * ONE REF RATHER THAN A SET, because there is one cursor. It is not a second record of
	 * anything: which bricks are PICKED is the selection's, and this is only what is being
	 * pointed at — the two are combined by HighlightForPiece and nowhere else.
	 */
	FPieceRef HoveredPiece;

	/** The widget those rows are drawn as, valid only while a menu is up. */
	TSharedPtr<SWidget> PieceMenuWidget;
};
