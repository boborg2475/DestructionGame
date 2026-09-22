// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "RequiredContent.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Uniquely named namespace for unity builds; helpers carry a Hover prefix to avoid clashing with
 * the Inspect* binding tests this file is modelled on.
 */
namespace HoverInputBindingTestSupport
{
	/** A component's action-event bindings as text, for the log. */
	FString DescribeHoverActionBindings(const UEnhancedInputComponent* Component)
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

	/** A context's mappings as text, for the log. */
	FString DescribeHoverContext(const UInputMappingContext* Context)
	{
		if (Context == nullptr)
		{
			return TEXT("<null context>");
		}

		FString Line;

		for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
		{
			Line += FString::Printf(
				TEXT("%s%s=%s"),
				Line.IsEmpty() ? TEXT("") : TEXT(", "),
				*GetNameSafe(Mapping.Action.Get()),
				*Mapping.Key.ToString());
		}

		return Line.IsEmpty() ? TEXT("<no mappings>") : Line;
	}

	/**
	 * Minimum mappings across both contexts, so a sweep over nothing fails. The assets have nine
	 * (eight + one); same floor as InspectPieceBindingTest.cpp.
	 */
	constexpr int32 HoverExistingMappingFloor = 6;
}

/**
 * SetupInputComponent binds the hover action to exactly one handler, on Triggered.
 *
 * Triggered, unlike IA_InspectPiece's Started: an axis actuates every frame the mouse moves.
 * Started would update only at the start of each movement, and Completed only after it stops.
 * Exactly one binding, since a duplicate doubles every per-frame trace. The action is loaded by
 * path so the controller can't agree with itself.
 *
 * Needs a world with a real ULocalPlayer (for SetupInputComponent), but no ticking.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoverInputBindingTest,
	"DestructionGame.World.Input.HoverActionReachesAHandler",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHoverInputBindingTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace HoverInputBindingTestSupport;

	const UInputAction* const HoverAction = LoadObject<UInputAction>(
		nullptr, DestructionContent::HoverPieceActionPath);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::HoverPieceActionPath),
		HoverAction);

	if (HoverAction == nullptr)
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

	// Precondition: an Enhanced input component exists (DefaultInput.ini's default class).
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

	AddInfo(FString::Printf(TEXT("the controller bound: %s"), *DescribeHoverActionBindings(Input)));

	int32 HoverBindings = 0;
	TArray<ETriggerEvent> HoverTriggers;

	for (const TUniquePtr<FEnhancedInputActionEventBinding>& Binding : Input->GetActionEventBindings())
	{
		if (Binding.IsValid() && Binding->GetAction() == HoverAction)
		{
			++HoverBindings;
			HoverTriggers.Add(Binding->GetTriggerEvent());
		}
	}

	TestEqual(
		FString::Printf(
			TEXT("SetupInputComponent must bind '%s' to exactly one handler; it bound %d [%s]"),
			DestructionContent::HoverPieceActionPath,
			HoverBindings,
			*DescribeHoverActionBindings(Input)),
		HoverBindings, 1);

	if (HoverBindings != 1)
	{
		TestWorld.End();
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("the hover binding must fire on EVERY actuated frame (Triggered) because a mouse axis is continuous, not a press; it fires on %s"),
			*UE::Input::LexToString(HoverTriggers[0])),
		HoverTriggers[0] == ETriggerEvent::Triggered);

	TestWorld.End();

	return true;
}

/**
 * The hover action exists, is mapped in IMC_Default, and does not consume an axis free-look uses.
 *
 * IMC_Default, not IMC_MouseLook: free-look is chorded on a held right button (S6), so a hover
 * mapped there would only update while RMB is held. Sharing Mouse2D with IA_MouseLook is the
 * hazard: bConsumeInput defaults to true, and a consumed key is withheld from lower mappings, so
 * the camera would stop turning. The check keys on sharing, not on a specific key.
 *
 * Loads assets only; no world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHoverPieceIsBoundTest,
	"DestructionGame.Content.HoverPieceIsBound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHoverPieceIsBoundTest::RunTest(const FString& Parameters)
{
	using namespace HoverInputBindingTestSupport;

	const UInputAction* const HoverAction = LoadObject<UInputAction>(
		nullptr, DestructionContent::HoverPieceActionPath);

	TestNotNull(
		*FString::Printf(
			TEXT("hovering needs an input action at '%s'; it does not resolve"),
			DestructionContent::HoverPieceActionPath),
		HoverAction);

	const UInputMappingContext* const DefaultContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::DefaultMappingContextPath);

	const UInputMappingContext* const MouseLookContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::MouseLookMappingContextPath);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::DefaultMappingContextPath),
		DefaultContext);

	TestNotNull(
		*FString::Printf(TEXT("fixture: '%s' should resolve"),
			DestructionContent::MouseLookMappingContextPath),
		MouseLookContext);

	if (DefaultContext == nullptr || MouseLookContext == nullptr || HoverAction == nullptr)
	{
		return true;
	}

	// Log both contexts' mappings.
	AddInfo(FString::Printf(TEXT("IMC_Default maps: %s"), *DescribeHoverContext(DefaultContext)));
	AddInfo(FString::Printf(TEXT("IMC_MouseLook maps: %s"), *DescribeHoverContext(MouseLookContext)));

	TArray<FEnhancedActionKeyMapping> AllMappings;
	AllMappings.Append(DefaultContext->GetMappings());
	AllMappings.Append(MouseLookContext->GetMappings());

	TestTrue(
		FString::Printf(
			TEXT("fixture: the two contexts should already carry at least %d mappings for this sweep to mean anything; they carry %d"),
			HoverExistingMappingFloor, AllMappings.Num()),
		AllMappings.Num() >= HoverExistingMappingFloor);

	// 1. IMC_Default maps it to a key; otherwise the C++ binding never fires.
	TArray<FKey> HoverKeys;

	for (const FEnhancedActionKeyMapping& Mapping : DefaultContext->GetMappings())
	{
		if (Mapping.Action.Get() == HoverAction)
		{
			HoverKeys.Add(Mapping.Key);
		}
	}

	TestTrue(
		FString::Printf(
			TEXT("IMC_Default must map the hover action to at least one key; it maps it to %d [%s]"),
			HoverKeys.Num(), *DescribeHoverContext(DefaultContext)),
		HoverKeys.Num() >= 1);

	// 2. Not in IMC_MouseLook, whose mapping only works while RMB is held.
	TestTrue(
		TEXT("the hover action belongs in IMC_Default, not in IMC_MouseLook — that context's one mapping is chorded on a held right mouse button, and the player is not holding it while moving the cursor over bricks"),
		!MouseLookContext->HasMappingForInputAction(HoverAction));

	// 3. If it shares a key with another action, it must not consume it.
	for (const FKey& HoverKey : HoverKeys)
	{
		for (const FEnhancedActionKeyMapping& Mapping : AllMappings)
		{
			if (Mapping.Action.Get() == HoverAction || Mapping.Key != HoverKey)
			{
				continue;
			}

			TestFalse(
				*FString::Printf(
					TEXT("the hover action shares %s with %s, so it must not consume input — a consumed key is withheld from every mapping below it and free-look simply stops"),
					*HoverKey.ToString(),
					*GetNameSafe(Mapping.Action.Get())),
				HoverAction->bConsumeInput);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
