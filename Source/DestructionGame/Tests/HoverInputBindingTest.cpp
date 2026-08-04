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
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. Note in particular that this is NOT InspectInputBindingTestSupport or
 * InspectPieceBindingTestSupport, which are the two files this one is modelled on, and every
 * helper here carries a Hover prefix so no name in it can be ambiguous against theirs. See
 * CURRENT_STATE.md; the `using namespace` lives inside each RunTest for the same reason.
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
	 * A FLOOR ON WHAT THE TWO CONTEXTS ALREADY MAP, so a sweep over nothing fails rather than
	 * passing in silence. Measured off the assets: IMC_Default carries eight mappings and
	 * IMC_MouseLook one (Mouse2D for IA_MouseLook). Six is comfortably under that and well over
	 * zero, which is all this number has to be — the same floor, and the same reasoning, as
	 * Tests/InspectPieceBindingTest.cpp's.
	 */
	constexpr int32 HoverExistingMappingFloor = 6;
}

/**
 * THE PLAYER CONTROLLER BINDS A HOVER INPUT ACTION TO A HANDLER WHEN IT SETS ITS INPUT UP,
 * EXACTLY ONCE, AND ON Triggered RATHER THAN ON Started.
 *
 * WHAT IS BROKEN TODAY. HoverAlongRay is written, covered end to end by World.Select, and
 * called by NOBODY. There is no mouse-move binding anywhere, so hovering is unreachable to a
 * player: every brick in the game reads EBrickHighlight::None forever unless it is clicked.
 * This is the wire, and it is the whole of what is missing on this side.
 *
 * THE TRIGGER EVENT IS THE DECISION THIS TEST EXISTS TO PIN, AND IT IS THE OPPOSITE OF
 * IA_InspectPiece'S. Inspecting is a one-shot press, so it binds Started — Triggered would
 * re-open the menu every frame LMB was held. Hover is not a press at all: it is a continuous
 * axis, and with no explicit trigger asset Enhanced Input actuates an axis action on every
 * frame its value is non-zero, i.e. on every frame the mouse is actually moving.
 *
 *   - Started fires on the FIRST frame of a movement gesture and not again until the mouse has
 *     stopped and started moving again. Hover would update once at the beginning of each drag
 *     and then be stale for the entire rest of it — which is precisely the frames in which what
 *     is under the cursor changes. THIS IS THE OBVIOUS WRONG ANSWER, because it is the one the
 *     file next door already uses.
 *   - Completed fires when the mouse STOPS. Hover would update one frame after the player
 *     finished pointing, so the brick called out would always be the previous one.
 *   - Triggered fires on every actuated frame and only on those, so a still mouse costs no
 *     traces at all. The property that made Triggered wrong for a held button — it fires on
 *     frames where nothing changed — is exactly what makes it right for an axis, where an
 *     actuated frame IS a frame where something changed.
 *
 * EXACTLY ONE BINDING, NOT AT LEAST ONE. Two bindings on the same action trace and re-highlight
 * twice per mouse-move frame; at 30 x 40 bricks that is a doubled line trace on every frame the
 * player moves the mouse, for an answer that was already correct. A count is the only thing that
 * sees it and "at least one" never would.
 *
 * THE ACTION IS LOADED BY PATH RATHER THAN READ OFF THE CONTROLLER, so a controller that bound
 * some other action asset fails here rather than agreeing with itself.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD — the engine only runs SetupInputComponent for a
 * controller with a real ULocalPlayer behind it — but it never ticks one, spawns no wall and
 * touches no physics.
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
	 * FIXTURE PRECONDITION: THE ENGINE REALLY DID RUN SetupInputComponent, and it made an
	 * ENHANCED one. DefaultInput.ini names UEnhancedInputComponent as the default input
	 * component class; if that ever changes, this is where it says so rather than reporting an
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
 * THE HOVER INPUT EXISTS, IS MAPPED IN IMC_Default, AND DOES NOT SWALLOW THE AXIS FREE-LOOK
 * NEEDS.
 *
 * IMC_Default RATHER THAN IMC_MouseLook, AND THAT IS THE LOAD-BEARING HALF. Both actions read
 * the same Mouse2D axis, so hanging hover off the existing IA_MouseLook looks like it saves an
 * asset — but SetPieceMenuControls REMOVES IMC_MouseLook for as long as a menu is up, so hover
 * would stop updating at exactly the moment the cursor appears and the player starts moving it
 * across bricks to pick more of them. IMC_Default is never removed, for the same reason
 * IA_InspectPiece lives in it: it is how a player interacts with a menu that is already open.
 *
 * AND THEREFORE THE KEY IS SHARED, WHICH IS THE HAZARD. UInputAction::bConsumeInput defaults to
 * TRUE, and a consumed key is withheld from every mapping below it in the applied stack — so a
 * hover action on Mouse2D that consumes it takes the mouse away from IA_MouseLook and the camera
 * simply stops turning, with nothing logged and no test naming the cause. The assertion is
 * therefore conditional on the sharing rather than on the key: whatever key hover ends up on, if
 * something else in either context also wants it, hover must not eat it.
 *
 * NEEDS A TICKING WORLD: no. Two assets and an action, loaded by path — no world, no player,
 * no input subsystem. Whether SetupInputComponent BINDS the action is the separate question
 * next door.
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
	 * ONE: IMC_Default MAPS IT AT ALL. An action asset that exists, is bound in C++ and is
	 * mapped to no key is the exact shape of a wire that looks finished and never fires.
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
	 * TWO: AND NOT IN THE FREE-LOOK CONTEXT. IMC_MouseLook is taken away while a menu is up, so
	 * a hover mapping placed there stops updating at exactly the moment the cursor appears.
	 */
	TestTrue(
		TEXT("the hover action belongs in IMC_Default, not in IMC_MouseLook — that context is removed while a menu is up, which is when the cursor is visible and moving over bricks"),
		!MouseLookContext->HasMappingForInputAction(HoverAction));

	/*
	 * THREE: IT MUST NOT EAT AN AXIS SOMETHING ELSE IS USING. Sharing Mouse2D with IA_MouseLook
	 * is expected and fine; consuming it is not, and the symptom is a camera that silently
	 * stops turning. Written over whatever the contexts contain rather than against a key
	 * somebody wrote down, so it keeps meaning the same thing if hover moves key.
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
