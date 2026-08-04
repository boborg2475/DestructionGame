// Copyright Epic Games, Inc. All Rights Reserved.


#include "DestructionGamePlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "UObject/ConstructorHelpers.h"
#include "Core/PieceActions.h"
#include "DestructionGameCameraManager.h"
#include "RequiredContent.h"
#include "World/DestructionStructureSubsystem.h"

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
 * How far the inspect ray reaches, in cm (1 uu = 1 cm), i.e. 100 m.
 *
 * IT LIVES IN THE HANDLER'S HALF, WHICH IS THE UNTESTED ONE, so it is a reach rather than a
 * tuned threshold: the game mode's wall is about 6.6 m across and 3 m tall, and a flying
 * observer is expected to be tens of metres off it. Nothing downstream depends on the value —
 * the trace either hits a brick or it does not, and a miss dismisses.
 */
static constexpr double PieceMenuInspectReachCm = 10000.0;

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
}

TArray<FPieceMenuRow> ADestructionGamePlayerController::InspectAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	TArray<FPieceMenuRow> Rows;

	UWorld* const World = GetWorld();

	UDestructionStructureSubsystem* const Subsystem =
		World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;

	/*
	 * THE WHOLE CHAIN IS ALREADY WRITTEN AND NONE OF IT IS REPEATED HERE. TracePiece fails
	 * closed on every step from the trace to the re-resolve, so a miss arrives as a default
	 * ref; PieceActionsFor resolves that ref again against the binding and answers an empty
	 * menu for one that names nothing. This is the wire between them, not a third opinion.
	 */
	if (Subsystem != nullptr)
	{
		const FPieceHit Hit = Subsystem->TracePiece(StartCm, EndCm);

		if (const FStructureBinding* const Binding = Subsystem->Find(Hit.Ref.StructureId))
		{
			Rows = BuildPieceMenuRows(PieceActionsFor(*Binding, Hit.Ref), Hit.Ref);
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

bool ADestructionGamePlayerController::ShowPieceMenu(TArrayView<const FPieceMenuRow> Rows)
{
	/*
	 * SHOWING IS DEFINED AS DISMISSING AND THEN BUILDING, WHICH IS THE POINT RATHER THAN AN
	 * IMPLEMENTATION DETAIL. There is exactly one route out of "a menu is up", so replacing a
	 * menu, showing an empty one and closing one outright all take it — which is what makes the
	 * controls come back on every one of them without three copies of the restore. It is also
	 * what will keep the widget half honest when it lands: a second add with no matching remove
	 * leaks the previous menu on screen forever, and no headless assertion can see that, but
	 * the model-level version of the same bug — holding two menus' rows — is asserted, and the
	 * two are only the same code path while show is written this way.
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

	InspectAlongRay(StartCm, StartCm + Direction * PieceMenuInspectReachCm);
}
