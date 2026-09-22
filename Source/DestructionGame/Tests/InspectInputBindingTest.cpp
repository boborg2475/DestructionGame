// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "RequiredContent.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace because unity builds merge anonymous ones. Not InspectPieceBindingTestSupport,
 * which belongs to the asset-side test.
 */
namespace InspectInputBindingTestSupport
{
	/** Every action-event binding on a component, as one line for the log. */
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
 * SetupInputComponent binds IA_InspectPiece to exactly one handler, on Started.
 *
 * Asserts the binding registry, not a simulated click: a real key press needs a viewport, and the
 * handler's DeprojectMousePositionToWorld is viewport-bound. The deprojection is untested anywhere,
 * so the handler should be only a deprojection plus one InspectAlongRay call.
 *
 * Exactly one binding, since two would run the handler twice per click. Started, since Triggered
 * fires every frame the button is held and Completed fires on release. The action is loaded by
 * path, not read off the controller, so binding the wrong asset fails. Needs a world with a real
 * ULocalPlayer (for SetupInputComponent) but never ticks it.
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

	// Precondition: DefaultInput.ini makes the input component a UEnhancedInputComponent.
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
