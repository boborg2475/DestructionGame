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
 * Named namespace, and named differently from every other one in this module — an
 * anonymous namespace is private to a translation unit rather than to a file, and a
 * unity build merges many files into one. Every helper here carries a Hover prefix so
 * no name can be ambiguous against the two files (InspectInputBindingTestSupport,
 * InspectPieceBindingTestSupport) this one is modelled on.
 */
namespace HoverInputBindingTestSupport
{
	/** Every action-event binding on a component, printed, so the log records what was bound. */
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

	/** Every mapping in a context, printed, so the log records what was bound at the time. */
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
	 * A floor on what the two contexts already map, so a sweep over nothing fails rather
	 * than passing in silence. Measured off the assets (IMC_Default: eight mappings,
	 * IMC_MouseLook: one); six is comfortably under that and well over zero, which is all
	 * this needs to be — same floor and reasoning as InspectPieceBindingTest.cpp's.
	 */
	constexpr int32 HoverExistingMappingFloor = 6;
}

/**
 * The player controller binds a hover input action to a handler when it sets its input
 * up, exactly once, and on Triggered rather than on Started.
 *
 * What is broken today: HoverAlongRay is written, covered end to end by World.Select, and
 * called by nobody. There is no mouse-move binding anywhere, so every brick reads
 * EBrickHighlight::None forever unless clicked. This is the missing wire.
 *
 * The trigger event is the opposite of IA_InspectPiece's. Inspecting is a one-shot press,
 * so it binds Started — Triggered would re-open the menu every frame LMB was held. Hover
 * is a continuous axis instead, and with no explicit trigger asset Enhanced Input actuates
 * an axis action on every frame its value is non-zero (every frame the mouse moves):
 *
 *   - Started fires on the first frame of a movement gesture only, so hover would update
 *     once at the start of each drag and stay stale for the rest of it — the file next
 *     door's binding, and the obvious wrong answer here.
 *   - Completed fires when the mouse stops, so hover would always name the previous brick.
 *   - Triggered fires on every actuated frame and only those — a still mouse costs no
 *     traces, and a moving one updates every frame it should.
 *
 * Exactly one binding, not at least one: two bindings on the same action trace and
 * re-highlight twice per mouse-move frame, doubling every line trace at 30x40 bricks for an
 * answer that was already correct. Only a count catches that.
 *
 * The action is loaded by path rather than read off the controller, so a controller that
 * bound some other action asset fails here rather than agreeing with itself.
 *
 * Needs a world — SetupInputComponent only runs for a controller with a real ULocalPlayer
 * behind it — but never ticks one, spawns no wall, touches no physics.
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

	/*
	 * Fixture precondition: the engine really did run SetupInputComponent, and made an
	 * Enhanced one. DefaultInput.ini names UEnhancedInputComponent as the default; if that
	 * ever changes, this is where it says so rather than reporting an absent binding.
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
 * The hover input exists, is mapped in IMC_Default, and does not swallow the axis
 * free-look needs.
 *
 * IMC_Default rather than IMC_MouseLook is the load-bearing half. Both actions read the
 * same Mouse2D axis, so hanging hover off the existing IA_MouseLook looks like it saves
 * an asset — but free-look has never been continuously live. SetPieceMenuControls used to
 * remove IMC_MouseLook while a menu was up, which would have stalled a hover hung off it
 * the moment the cursor appeared; S6 deleted that function and chorded free-look on a held
 * right mouse button instead, leaving the same hole by a different route (a hover there
 * would only update while the player is holding RMB, the one moment they are not pointing
 * at a brick). IMC_Default is applied for the whole session and gated on nothing, the same
 * reason IA_InspectPiece lives in it.
 *
 * The key is therefore shared, which is the hazard: UInputAction::bConsumeInput defaults
 * to true, and a consumed key is withheld from every mapping below it in the applied stack
 * — so a hover action on Mouse2D that consumes it takes the mouse away from IA_MouseLook
 * and the camera silently stops turning. The assertion is conditional on the sharing
 * rather than on the key, so it holds whatever key hover ends up on.
 *
 * No ticking world needed: two assets and an action, loaded by path. Whether
 * SetupInputComponent binds the action is the separate question next door.
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

	/* What the two contexts carry, in the log, so the answer is checkable rather than recalled. */
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

	/*
	 * One: IMC_Default maps it at all. An asset bound in C++ but mapped to no key is a
	 * wire that looks finished and never fires.
	 */
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

	/*
	 * Two: and not in the free-look context. That context used to be taken away while a
	 * menu was up; since S6 its one mapping is chorded on a held right mouse button.
	 * Either way a hover mapping placed there is dead at the moment it is wanted.
	 */
	TestTrue(
		TEXT("the hover action belongs in IMC_Default, not in IMC_MouseLook — that context's one mapping is chorded on a held right mouse button, and the player is not holding it while moving the cursor over bricks"),
		!MouseLookContext->HasMappingForInputAction(HoverAction));

	/*
	 * Three: it must not eat an axis something else is using. Sharing Mouse2D with
	 * IA_MouseLook is fine; consuming it is not — the symptom is a camera that silently
	 * stops turning. Checked against whatever the contexts contain, so it still holds if
	 * hover moves key.
	 */
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
