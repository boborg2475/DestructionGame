// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "RequiredContent.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. Note in particular that this is NOT InspectPieceBindingTestSupport,
 * which is the ASSET-side test next door. See CURRENT_STATE.md; the `using namespace` lives
 * inside RunTest for the same reason.
 */
namespace InspectInputBindingTestSupport
{
	/** Every action-event binding on a component, printed, so the log records what was bound. */
	FString DescribeActionBindings(const UEnhancedInputComponent* Component)
	{
		if (Component == nullptr)
		{
			return TEXT("<no enhanced input component>");
		}

		FString Line;

		for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding
			: Component->GetActionEventBindings())
		{
			if (!Binding.IsValid())
			{
				continue;
			}

			Line += FString::Printf(
				TEXT("%s%s@%s"),
				Line.IsEmpty() ? TEXT("") : TEXT(", "),
				*GetNameSafe(Binding->GetAction()),
				*UE::Input::LexToString(Binding->GetTriggerEvent()));
		}

		return Line.IsEmpty() ? TEXT("<no action bindings>") : Line;
	}
}

/**
 * THE PLAYER CONTROLLER BINDS IA_InspectPiece TO A HANDLER WHEN IT SETS ITS INPUT UP, EXACTLY
 * ONCE, ON THE PRESS.
 *
 * WHAT IS BROKEN TODAY. IA_InspectPiece exists, IMC_Default maps it to LeftMouseButton
 * (Content.InspectPieceIsBound asserts both), the controller holds the action as a UPROPERTY
 * resolved by path, and InspectAlongRay is written and covered — but SetupInputComponent only
 * adds the mapping contexts. Nothing calls UEnhancedInputComponent::BindAction, so the key
 * press reaches nothing at all and the whole chain behind it is dead code from a player's
 * point of view. This is the wire, and it is the last thing between LMB and a menu.
 *
 * THE ASSERTION IS THE BINDING REGISTRY, NOT A SIMULATED CLICK, and that is the honest limit
 * rather than a shortcut. Delivering a real key press needs a viewport, a slate application
 * and a focused window; and the handler's first act is DeprojectMousePositionToWorld, which is
 * genuinely viewport-bound — which is precisely why the seam sits at the RAY and
 * InspectAlongRay takes one. So: is the action attached to something. That is binary, exact,
 * and it is the specific thing that is missing. THE DEPROJECTION ITSELF IS DELIBERATELY NOT
 * COVERED HERE OR ANYWHERE, and the handler should therefore be a deprojection plus one call
 * to InspectAlongRay with nothing else in it — anything more grows the untested surface.
 *
 * EXACTLY ONE BINDING, NOT AT LEAST ONE. Two bindings for the same action on the same trigger
 * is a handler that runs twice per click: the menu opens, and then opens again against a
 * second trace — or, once the menu also dismisses on a miss, opens and immediately closes.
 * A count is the only thing that sees that, and "at least one" never would.
 *
 * ON Started, NOT Triggered. With no explicit trigger on the action, Triggered fires EVERY
 * FRAME the button is held down, so holding LMB re-traces and re-presents the menu sixty times
 * a second; Completed is the release, which opens a menu on let-go. Started is the one-shot
 * press, and opening a menu is a one-shot event.
 *
 * THE ACTION IS LOADED BY PATH RATHER THAN READ OFF THE CONTROLLER, so a controller that
 * bound some other action asset fails here rather than agreeing with itself.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD — a controller is an actor, and the engine only runs
 * SetupInputComponent for a controller with a real ULocalPlayer behind it — but it never ticks
 * one, spawns no wall and touches no physics.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectInputBindingTest,
	"DestructionGame.World.Input.InspectActionReachesAHandler",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FInspectInputBindingTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace InspectInputBindingTestSupport;

	const UInputAction* const InspectAction = LoadObject<UInputAction>(
		nullptr, DestructionContent::InspectPieceActionPath);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::InspectPieceActionPath),
		InspectAction);

	if (InspectAction == nullptr)
	{
		return true;
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller =
		SpawnControllerWithLocalPlayer(*this, TestWorld.World);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	/*
	 * FIXTURE PRECONDITION: THE ENGINE REALLY DID RUN SetupInputComponent, and it made an
	 * ENHANCED one. DefaultInput.ini names UEnhancedInputComponent as the default input
	 * component class; if that ever changes, every BindAction in the project stops compiling
	 * against the enhanced overloads and this is where it says so rather than reporting an
	 * absent binding.
	 */
	UEnhancedInputComponent* const Input = Cast<UEnhancedInputComponent>(Controller->InputComponent);

	TestNotNull(
		*FString::Printf(
			TEXT("fixture: the controller's input component should be a UEnhancedInputComponent, it is %s"),
			*GetNameSafe(Controller->InputComponent)),
		Input);

	if (Input == nullptr)
	{
		TestWorld.End();
		return true;
	}

	/*
	 * WHAT IS BOUND, REPORTED ON EVERY RUN, so a failure is readable from the log rather than
	 * from a debugger — and so "bound to the wrong trigger event" reads as such.
	 */
	AddInfo(FString::Printf(TEXT("the controller bound: %s"), *DescribeActionBindings(Input)));

	int32 InspectBindings = 0;
	TArray<ETriggerEvent> InspectTriggers;

	for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Input->GetActionEventBindings())
	{
		if (Binding.IsValid() && Binding->GetAction() == InspectAction)
		{
			++InspectBindings;
			InspectTriggers.Add(Binding->GetTriggerEvent());
		}
	}

	TestEqual(
		FString::Printf(
			TEXT("SetupInputComponent must bind '%s' to exactly one handler; it bound %d [%s]"),
			DestructionContent::InspectPieceActionPath,
			InspectBindings,
			*DescribeActionBindings(Input)),
		InspectBindings, 1);

	if (InspectBindings != 1)
	{
		TestWorld.End();
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("the inspect binding must fire on the PRESS (Started), it fires on %s"),
			*UE::Input::LexToString(InspectTriggers[0])),
		InspectTriggers[0] == ETriggerEvent::Started);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
