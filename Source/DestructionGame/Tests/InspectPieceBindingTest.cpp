// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "RequiredContent.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md; the `using namespace` lives inside RunTest for
 * the same reason.
 */
namespace InspectPieceBindingTestSupport
{
	/**
	 * A FLOOR ON WHAT THE TWO CONTEXTS ALREADY MAP, so a collision sweep over nothing fails
	 * rather than passing in silence.
	 *
	 * Measured off the assets as they stand: IMC_Default carries eight mappings — four
	 * keyboard directions plus Gamepad_Left2D for IA_Move, Gamepad_Right2D for IA_Look, and
	 * SpaceBar plus Gamepad_FaceButton_Bottom for IA_Jump — and IMC_MouseLook carries one,
	 * Mouse2D for IA_MouseLook. Six is comfortably under that and well over zero, which is
	 * the only thing this number has to be: an equality would have to be edited every time a
	 * binding is added, which is the edit that gets forgotten.
	 */
	constexpr int32 ExistingMappingFloor = 6;

	/** Every mapping in a context, printed, so the log records what was bound at the time. */
	FString DescribeContext(const UInputMappingContext* Context)
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
}

/**
 * THE INPUT THAT OPENS THE PIECE MENU EXISTS, IS MAPPED IN IMC_Default, AND ITS KEY IS NOT
 * ALREADY DOING SOMETHING ELSE.
 *
 * WHY A TEST AND NOT JUST AN ASSET. CURRENT_STATE.md's instruction for this slice is "do not
 * hard-code the input that opens it — bind it through the existing Enhanced Input assets and
 * leave the binding checkable rather than assuming a mouse button". Checkable means exactly
 * this: something that reads the shipped context and says what is in it. A binding added by
 * hand in the editor and never asserted is a binding that silently stops existing the next
 * time somebody re-saves the asset.
 *
 * THE COLLISION SWEEP IS THE HALF THAT NEEDED CHECKING FIRST. The obvious key for "click a
 * brick" is a mouse button, and the obvious hazard is that one is already held to look
 * around — so this asserts, generically, that no key mapped to the inspect action is mapped
 * to any OTHER action in either context. It is written over both contexts and over whatever
 * they contain rather than against a list of keys somebody wrote down, so it keeps working
 * when a binding is added. (What the assets actually carry today is printed on every run, so
 * the answer is in the log rather than in anyone's memory: IMC_MouseLook binds Mouse2D, the
 * mouse AXIS, and no mouse BUTTON is bound anywhere in either context.)
 *
 * IMC_Default RATHER THAN A NEW CONTEXT. Both contexts are already hard-referenced by
 * ADestructionGamePlayerController's constructor and both are already rows of the
 * required-content table, so adding a mapping to one is the change with the least new
 * surface; a third context would be a third asset to add, to reference, to list and to
 * remember to push.
 *
 * NEEDS A TICKING WORLD: no. Two assets and an action, loaded by path — no world, no player,
 * no input subsystem. Whether a KEY PRESS reaches the handler is a different question and
 * needs a possessed local player; it is deliberately not asserted here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FInspectPieceBindingTest,
	"DestructionGame.Content.InspectPieceIsBound",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FInspectPieceBindingTest::RunTest(const FString& Parameters)
{
	using namespace InspectPieceBindingTestSupport;

	const UInputAction* const InspectAction = LoadObject<UInputAction>(
		nullptr, DestructionContent::InspectPieceActionPath);

	TestNotNull(
		*FString::Printf(
			TEXT("the piece menu needs an input action at '%s'; it does not resolve"),
			DestructionContent::InspectPieceActionPath),
		InspectAction);

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

	if (DefaultContext == nullptr || MouseLookContext == nullptr)
	{
		return true;
	}

	/*
	 * WHAT IS ALREADY BOUND, REPORTED ON EVERY RUN. This is the "check before you choose a
	 * mouse button" evidence, kept in the log rather than in a comment that goes stale.
	 */
	AddInfo(FString::Printf(TEXT("IMC_Default maps: %s"), *DescribeContext(DefaultContext)));
	AddInfo(FString::Printf(TEXT("IMC_MouseLook maps: %s"), *DescribeContext(MouseLookContext)));

	TArray<FEnhancedActionKeyMapping> AllMappings;
	AllMappings.Append(DefaultContext->GetMappings());
	AllMappings.Append(MouseLookContext->GetMappings());

	TestTrue(
		FString::Printf(
			TEXT("fixture: the two contexts should already carry at least %d mappings for a collision sweep to mean anything; they carry %d"),
			ExistingMappingFloor, AllMappings.Num()),
		AllMappings.Num() >= ExistingMappingFloor);

	if (InspectAction == nullptr)
	{
		return true;
	}

	/*
	 * ONE: IMC_Default MAPS IT AT ALL. An action asset that exists and is mapped nowhere is
	 * the exact shape of a binding that looks done and does nothing.
	 */
	TArray<FKey> InspectKeys;

	for (const FEnhancedActionKeyMapping& Mapping : DefaultContext->GetMappings())
	{
		if (Mapping.Action.Get() == InspectAction)
		{
			InspectKeys.Add(Mapping.Key);
		}
	}

	TestTrue(
		FString::Printf(
			TEXT("IMC_Default must map the inspect-piece action to at least one key; it maps it to %d"),
			InspectKeys.Num()),
		InspectKeys.Num() >= 1);

	/*
	 * TWO: EVERY KEY IT USES IS FREE. Written over whatever the contexts contain rather than
	 * against a hand-written list of taken keys, so the day IA_MouseLook grows a held mouse
	 * button this fails instead of quietly overlapping it.
	 */
	for (const FKey& InspectKey : InspectKeys)
	{
		for (const FEnhancedActionKeyMapping& Mapping : AllMappings)
		{
			if (Mapping.Action.Get() == InspectAction || Mapping.Key != InspectKey)
			{
				continue;
			}

			AddError(FString::Printf(
				TEXT("the inspect-piece action is mapped to %s, which is already mapped to %s — a key doing two jobs is a click that also moves the camera"),
				*InspectKey.ToString(),
				*GetNameSafe(Mapping.Action.Get())));
		}
	}

	/*
	 * THREE: AND NOT IN THE LOOK CONTEXT. IMC_MouseLook is the always-on free-look context;
	 * putting a menu key in it would tie opening the menu to whether looking is active.
	 */
	TestTrue(
		TEXT("the inspect-piece action belongs in IMC_Default, not in the free-look context"),
		!MouseLookContext->HasMappingForInputAction(InspectAction));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
