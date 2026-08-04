// Copyright Epic Games, Inc. All Rights Reserved.


#include "DestructionGamePlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "UObject/ConstructorHelpers.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Core/PieceActions.h"
#include "DestructionGameCameraManager.h"
#include "RequiredContent.h"
#include "World/BrickActor.h"
#include "World/DestructionStructureSubsystem.h"

/*
 * File-local names carry a PieceMenu prefix. An anonymous namespace is private to a
 * TRANSLATION UNIT rather than to a file, and a unity build merges many files into one —
 * so two file-local names that collide are a hard compile error between files that never
 * refer to each other. See CURRENT_STATE.md.
 */
namespace
{
	/** The subsystem holding this world's walls, or null. There is no world in a bare CDO. */
	UDestructionStructureSubsystem* PieceMenuSubsystemOf(const AActor& Actor)
	{
		UWorld* const World = Actor.GetWorld();

		return World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;
	}

	/**
	 * The brick standing for this ref, or null.
	 *
	 * THE REF IS RESOLVED RATHER THAN INDEXED, so a ref naming a piece that has gone — which
	 * is what every ref becomes the moment a commit runs — answers null instead of reaching
	 * a tombstoned slot. GetActor already answers null for INDEX_NONE, for a removed piece
	 * and for an actor destroyed by any route, so the cast is the only check left.
	 */
	ABrickActor* PieceMenuBrickForRef(UDestructionStructureSubsystem* Subsystem, const FPieceRef& Ref)
	{
		if (Subsystem == nullptr)
		{
			return nullptr;
		}

		const FStructureBinding* const Binding = Subsystem->Find(Ref.StructureId);

		if (Binding == nullptr)
		{
			return nullptr;
		}

		return Cast<ABrickActor>(Binding->GetActor(Binding->ResolvePiece(Ref)));
	}
}

/*
 * THE PRIORITY THE CONTEXTS ARE APPLIED AT, NAMED BECAUSE IT IS NOW USED TWICE. The menu
 * removes IMC_MouseLook and puts it back, and a restore at a different priority would change
 * which context wins a shared key without changing anything visible at the call site.
 *
 * The name is deliberately not a bare `MappingContextPriority`: a unity build merges many
 * .cpp files into one translation unit, so a file-scope constant here shares a namespace with
 * every other file in the module. See CURRENT_STATE.md.
 */
static constexpr int32 PieceMenuMappingContextPriority = 0;

/*
 * How far a ray cast from the cursor reaches, in cm (1 uu = 1 cm), i.e. 100 m.
 *
 * ONE REACH FOR BOTH HANDLERS, because they are the same ray: what a click would hit and what
 * the cursor is pointing at must be the same brick, and two constants is two ways for them to
 * stop being.
 *
 * IT LIVES IN THE HANDLERS' HALF, WHICH IS THE UNTESTED ONE, so it is a reach rather than a
 * tuned threshold: the game mode's wall is about 6.6 m across and 3 m tall, and a flying
 * observer is expected to be tens of metres off it. Nothing downstream depends on the value —
 * the trace either hits a brick or it does not, and a miss dismisses.
 */
static constexpr double PieceMenuCursorReachCm = 10000.0;

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

	/*
	 * the look context is remembered by name as well as applied, because the piece menu takes
	 * it away while it is up — the SAME pointer in both places, so there is nothing to drift
	 */
	MouseLookMappingContext = MouseLookContext.Object;

	DefaultMappingContexts.Add(DefaultContext.Object);
	DefaultMappingContexts.Add(MouseLookMappingContext);

	/* the piece menu's own input, by the same one spelling of its path */
	static ConstructorHelpers::FObjectFinder<UInputAction> InspectPieceActionAsset(DestructionContent::InspectPieceActionPath);

	InspectPieceAction = InspectPieceActionAsset.Object;

	/* and the one that keeps the highlight under the cursor, mapped in IMC_Default beside it */
	static ConstructorHelpers::FObjectFinder<UInputAction> HoverPieceActionAsset(DestructionContent::HoverPieceActionPath);

	HoverPieceAction = HoverPieceActionAsset.Object;
}

TArray<FPieceMenuRow> ADestructionGamePlayerController::InspectAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	TArray<FPieceMenuRow> Rows;

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	/*
	 * THE WHOLE CHAIN IS ALREADY WRITTEN AND NONE OF IT IS REPEATED HERE. TracePiece fails
	 * closed on every step from the trace to the re-resolve, so a miss arrives as a default
	 * ref; PieceActionsFor resolves every ref again against the binding and answers an empty
	 * menu for one that names nothing. This is the wire between them, not a third opinion.
	 */
	if (Subsystem != nullptr)
	{
		const FPieceHit Hit = Subsystem->TracePiece(StartCm, EndCm);

		/*
		 * A CLICK HAPPENS AT THE CURSOR, so this ray is also the answer to what is under it.
		 * Saying so here rather than waiting for the next mouse-move is what stops a brick
		 * staying lit after it has been clicked away from, or a cleared selection leaving
		 * the last brick pointed at still called out.
		 */
		SetHoveredPiece(Hit.Ref);

		/*
		 * CLICKING A BRICK TOGGLES IT, AND CLICKING PAST EVERYTHING CLEARS THE LOT. The
		 * selection is the durable state and the menu is a projection of it rebuilt below,
		 * which is why there is no branch here that shows or dismisses anything: an empty
		 * selection builds no rows, and an empty row list is already how a menu comes down.
		 */
		if (Hit.PieceHandle != INDEX_NONE)
		{
			PieceSelection.Toggle(Hit.Ref);

			RefreshPieceHighlight(Hit.Ref);
		}
		else
		{
			ClearPieceSelection();
		}

		/*
		 * ONE MENU FOR THE WHOLE SELECTION, AGAINST THE STRUCTURE ITS REFS NAME. A selection
		 * is built by clicking one wall, so the first ref names it and PieceActionsFor
		 * refuses the rest piece by piece if it ever does not.
		 */
		const TArrayView<const FPieceRef> Selected = PieceSelection.Refs();

		if (Selected.Num() > 0)
		{
			if (const FStructureBinding* const Binding = Subsystem->Find(Selected[0].StructureId))
			{
				Rows = BuildPieceMenuRows(PieceActionsFor(*Binding, Selected), Selected);
			}
		}
	}

	/*
	 * EVERY ROUTE OUT OF HERE PRESENTS, INCLUDING THE ONES THAT FOUND NOTHING — no world, no
	 * subsystem, a ray that hit the floor, a brick standing for a piece that has gone. That is
	 * what makes "the ray hit nothing" and "take the menu down" the same call rather than two,
	 * and it is the whole reason this is one ShowPieceMenu at the end instead of an early
	 * return per guard: a route that simply returned would leave the previous brick's menu on
	 * screen naming a brick the player is no longer pointing at, and a Delete on it removes it.
	 */
	ShowPieceMenu(Rows);

	return Rows;
}

FPieceRef ADestructionGamePlayerController::HoverAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	/*
	 * POINTING AT A BRICK IS NOT CHOOSING IT, so nothing here touches the selection, opens
	 * a menu or closes one — the only thing that changes is which brick is called out. No
	 * world and no subsystem is the same answer as a ray that hit nothing: a default ref,
	 * which lets go of whatever was called out before.
	 */
	const FPieceHit Hit = Subsystem != nullptr ? Subsystem->TracePiece(StartCm, EndCm) : FPieceHit();

	SetHoveredPiece(Hit.Ref);

	return Hit.Ref;
}

const FPieceSelection& ADestructionGamePlayerController::GetPieceSelection() const
{
	return PieceSelection;
}

EBrickHighlight ADestructionGamePlayerController::HighlightForPiece(const FPieceRef& Ref) const
{
	/*
	 * THE STRONGER STATE WINS WHERE THEY COINCIDE, and this order is the whole of that rule.
	 * A selected brick under the cursor stays Selected: a hover that overwrote it would make
	 * a chosen brick read as unchosen exactly when the player is looking at it, which is
	 * indistinguishable from having lost the selection.
	 */
	if (PieceSelection.Contains(Ref))
	{
		return EBrickHighlight::Selected;
	}

	if (HoveredPiece == Ref)
	{
		return EBrickHighlight::Hovered;
	}

	return EBrickHighlight::None;
}

void ADestructionGamePlayerController::RefreshPieceHighlight(const FPieceRef& Ref)
{
	if (ABrickActor* const Brick = PieceMenuBrickForRef(PieceMenuSubsystemOf(*this), Ref))
	{
		Brick->SetHighlighted(HighlightForPiece(Ref));
	}
}

void ADestructionGamePlayerController::SetHoveredPiece(const FPieceRef& Ref)
{
	/*
	 * THE BRICK BEING LEFT IS REFRESHED AS WELL AS THE ONE BEING POINTED AT, and it is
	 * refreshed rather than simply cleared — it may be selected, in which case it stays
	 * called out. Without the first of the two, every brick the cursor has ever crossed
	 * stays lit and the wall ends up entirely highlighted.
	 */
	const FPieceRef Previous = HoveredPiece;

	HoveredPiece = Ref;

	RefreshPieceHighlight(Previous);
	RefreshPieceHighlight(Ref);
}

void ADestructionGamePlayerController::ClearPieceSelection()
{
	/* Copied out first: Clear empties the very array these live in. */
	const TArray<FPieceRef> WasSelected(PieceSelection.Refs());

	PieceSelection.Clear();

	for (const FPieceRef& Ref : WasSelected)
	{
		RefreshPieceHighlight(Ref);
	}
}

bool ADestructionGamePlayerController::ShowPieceMenu(TArrayView<const FPieceMenuRow> Rows)
{
	/*
	 * SHOWING IS DEFINED AS DISMISSING AND THEN BUILDING, WHICH IS THE POINT RATHER THAN AN
	 * IMPLEMENTATION DETAIL. There is exactly one route out of "a menu is up", so replacing a
	 * menu, showing an empty one and closing one outright all take it — which is what makes the
	 * controls come back on every one of them without three copies of the restore. It is also
	 * what keeps the widget half honest, which is why the build below sits here and the removal
	 * sits beside the Reset in DismissPieceMenu: a second add with no matching remove leaks the
	 * previous menu on screen forever and no headless assertion can see that, but the
	 * model-level version of the same bug — holding two menus' rows — is asserted, and the two
	 * are only the same code path while show is written this way.
	 *
	 * Rows must not alias ShownPieceMenuRows: the dismiss below empties it. No caller does that
	 * today and nothing guards it; see CURRENT_STATE.md.
	 */
	DismissPieceMenu();

	if (Rows.Num() == 0)
	{
		return false;
	}

	ShownPieceMenuRows.Append(Rows.GetData(), Rows.Num());

	BuildPieceMenuWidget();

	SetPieceMenuControls(true);

	return true;
}

bool ADestructionGamePlayerController::DismissPieceMenu()
{
	if (!IsPieceMenuShown())
	{
		return false;
	}

	ShownPieceMenuRows.Reset();

	RemovePieceMenuWidget();

	SetPieceMenuControls(false);

	return true;
}

bool ADestructionGamePlayerController::IsPieceMenuShown() const
{
	/*
	 * THE ROWS ARE THE RECORD, AND THERE IS NO SECOND FLAG. An empty list dismisses, so
	 * "holding rows" and "a menu is up" are the same fact; a bool beside them would be a
	 * second copy of it, free to disagree.
	 */
	return ShownPieceMenuRows.Num() > 0;
}

TArrayView<const FPieceMenuRow> ADestructionGamePlayerController::GetShownPieceMenuRows() const
{
	return ShownPieceMenuRows;
}

bool ADestructionGamePlayerController::ChoosePieceMenuRow(int32 RowIndex)
{
	/*
	 * AN INDEX THAT NAMES NO ROW COMMITS NOTHING, AND IT IS REFUSED RATHER THAN CLAMPED. A
	 * FMath::Clamp here would turn every out-of-range choice into a commit of row 0 — the
	 * first entry of a menu run against a brick nobody clicked — which is the obvious wrong
	 * fix and is exactly what the refusal rows of World.Choose count entries into Run to
	 * catch. IsValidIndex is also what makes "choose row 0 when no menu is up" the same
	 * refusal, since an empty array has no valid index at all.
	 */
	if (!ShownPieceMenuRows.IsValidIndex(RowIndex))
	{
		return false;
	}

	/*
	 * THE CHOSEN ROW IS COPIED OUT BEFORE THE DISMISS, and that is load-bearing rather than
	 * tidy: DismissPieceMenu Reset()s the very array the rows live in, so a reference into
	 * ShownPieceMenuRows would be reading destroyed elements by the time it was committed.
	 * Both halves come from the ROW rather than from anything this controller remembered
	 * separately — Core/PieceMenu.h says why the row carries its own targets, and a
	 * presenter that committed the chosen row's action against a remembered selection would
	 * act on the wrong bricks with everything else looking perfect.
	 */
	const TArray<FPieceRef> Refs = ShownPieceMenuRows[RowIndex].Refs;
	const FPieceAction* const Action = ShownPieceMenuRows[RowIndex].Action;

	/*
	 * IT COMES DOWN FIRST, BY THE ONE ROUTE OUT OF "A MENU IS UP" — so the cursor and
	 * free-look are given back by the same restore every other route takes, and the commit
	 * below runs with nothing on screen naming the bricks it is about to remove.
	 */
	DismissPieceMenu();

	/*
	 * AND THE PICK GOES WITH IT, BEFORE THE COMMIT RATHER THAN AFTER. These bricks have just
	 * been acted on, so leaving them selected would carry them into the next click's menu —
	 * where they no longer resolve, and the intersection then offers nothing at all. Before,
	 * because the commit is what destroys them, and a brick has to still exist to be told it
	 * is no longer called out.
	 */
	ClearPieceSelection();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	if (Subsystem == nullptr)
	{
		return false;
	}

	/*
	 * ONE COMMIT FOR THE WHOLE SELECTION, WHICH IS WHAT MAKES IT ONE SOLVE. Looping the
	 * single-piece commit here would reach the same wall at N times the price — and would
	 * push N times, each against an answer that had seen only part of the batch.
	 */
	return Subsystem->CommitPieceActionForAll(Refs, *Action) > 0;
}

void ADestructionGamePlayerController::BuildPieceMenuWidget()
{
	UWorld* const World = GetWorld();

	UGameViewportClient* const Viewport = World != nullptr ? World->GetGameViewport() : nullptr;

	/*
	 * NO VIEWPORT MEANS NO WIDGET, AND THAT IS THE ORDINARY CASE IN A TEST rather than an
	 * error: a world built in code has no UGameViewportClient at all, so the presented rows —
	 * which are the record, not this — stand alone and everything asserted about a menu still
	 * holds with nothing drawn.
	 */
	if (Viewport == nullptr)
	{
		return;
	}

	TSharedRef<SVerticalBox> Buttons = SNew(SVerticalBox);

	for (int32 RowIndex = 0; RowIndex < ShownPieceMenuRows.Num(); ++RowIndex)
	{
		Buttons->AddSlot()
			.AutoHeight()
			[
				SNew(SButton)
				.Text(FText::FromString(ShownPieceMenuRows[RowIndex].Label))
				.OnClicked(FOnClicked::CreateUObject(
					this, &ADestructionGamePlayerController::OnPieceMenuRowClicked, RowIndex))
			];
	}

	PieceMenuWidget =
		SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			Buttons
		];

	Viewport->AddViewportWidgetContent(PieceMenuWidget.ToSharedRef());
}

void ADestructionGamePlayerController::RemovePieceMenuWidget()
{
	if (!PieceMenuWidget.IsValid())
	{
		return;
	}

	UWorld* const World = GetWorld();

	if (UGameViewportClient* const Viewport = World != nullptr ? World->GetGameViewport() : nullptr)
	{
		Viewport->RemoveViewportWidgetContent(PieceMenuWidget.ToSharedRef());
	}

	PieceMenuWidget.Reset();
}

FReply ADestructionGamePlayerController::OnPieceMenuRowClicked(int32 RowIndex)
{
	ChoosePieceMenuRow(RowIndex);

	return FReply::Handled();
}

void ADestructionGamePlayerController::SetPieceMenuControls(bool bMenuIsUp)
{
	bShowMouseCursor = bMenuIsUp;

	UEnhancedInputLocalPlayerSubsystem* const Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer());

	/*
	 * A CONTROLLER WITH NO LOCAL PLAYER HAS NO SUBSYSTEM TO TAKE A CONTEXT OFF, and presenting
	 * must still work rather than merely not crash — so this fails closed and leaves the
	 * cursor flag, which needs nothing, already set above.
	 */
	if (Subsystem == nullptr || MouseLookMappingContext == nullptr)
	{
		return;
	}

	if (bMenuIsUp)
	{
		Subsystem->RemoveMappingContext(MouseLookMappingContext);
	}
	else
	{
		Subsystem->AddMappingContext(MouseLookMappingContext, PieceMenuMappingContextPriority);
	}
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
				Subsystem->AddMappingContext(CurrentContext, PieceMenuMappingContextPriority);
			}
		}
	}

	/*
	 * ON Started, AND EXACTLY ONCE. With no explicit trigger on the action, Triggered fires
	 * every frame the button is held, so holding LMB would re-trace and re-present the menu
	 * sixty times a second; Completed is the release, which opens a menu on let-go. Opening a
	 * menu is a one-shot press. And a second binding for the same action runs the handler twice
	 * per click — which, now that a miss dismisses, is open-then-immediately-close.
	 */
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent))
	{
		if (InspectPieceAction != nullptr)
		{
			EnhancedInputComponent->BindAction(
				InspectPieceAction,
				ETriggerEvent::Started,
				this,
				&ADestructionGamePlayerController::OnInspectPiece);
		}

		/*
		 * HOVER BINDS ON Triggered, WHICH IS THE OPPOSITE OF THE LINE ABOVE AND IS THE POINT.
		 * Inspecting is a one-shot press; hovering is a continuous axis, and with no explicit
		 * trigger asset Enhanced Input actuates an axis action on every frame its value is
		 * non-zero — i.e. on exactly the frames the mouse moved, which are exactly the frames
		 * on which what is under the cursor can have changed. Started fires on the first frame
		 * of a gesture and not again until the mouse stops and restarts, so the highlight would
		 * update once per drag and be stale for the rest of it; Completed fires when the mouse
		 * STOPS, so the brick called out would always be the previous one. A still mouse costs
		 * no traces at all, because an unactuated axis fires nothing.
		 *
		 * AND EXACTLY ONCE, for the same reason as above: a second binding traces and
		 * re-highlights twice on every moved frame for an answer that was already correct.
		 */
		if (HoverPieceAction != nullptr)
		{
			EnhancedInputComponent->BindAction(
				HoverPieceAction,
				ETriggerEvent::Triggered,
				this,
				&ADestructionGamePlayerController::OnHoverPiece);
		}
	}
}

void ADestructionGamePlayerController::OnInspectPiece()
{
	FVector StartCm;
	FVector Direction;

	/*
	 * No viewport means no ray at all, and the out parameters are left untouched — so this
	 * returns rather than tracing along whatever was on the stack.
	 */
	if (!DeprojectMousePositionToWorld(StartCm, Direction))
	{
		return;
	}

	InspectAlongRay(StartCm, StartCm + Direction * PieceMenuCursorReachCm);
}

void ADestructionGamePlayerController::OnHoverPiece()
{
	FVector StartCm;
	FVector Direction;

	/*
	 * Same untestable inch as OnInspectPiece, and the same failure closed: no viewport means no
	 * ray, and the out parameters are left untouched, so this returns rather than tracing along
	 * whatever happened to be on the stack.
	 */
	if (!DeprojectMousePositionToWorld(StartCm, Direction))
	{
		return;
	}

	HoverAlongRay(StartCm, StartCm + Direction * PieceMenuCursorReachCm);
}
