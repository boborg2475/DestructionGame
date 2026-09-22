// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "RequiredContent.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, unique to this file: a unity build merges files, so an anonymous namespace
 * would collide with another file's (see CURRENT_STATE.md). The `using namespace` lives inside
 * RunTest for the same reason.
 */
namespace InspectPieceBindingTestSupport
{
	/**
	 * A floor on what the two contexts already map, so a collision sweep over nothing fails rather
	 * than passing silently. IMC_Default carries eight mappings and IMC_MouseLook one; six is under
	 * that and over zero, which is all it needs to be. An equality would need editing every time a
	 * binding is added.
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
 * The input that opens the piece menu exists, is mapped in IMC_Default, and its key is not
 * already doing something else.
 *
 * A test, not just an asset, because a binding added by hand and never asserted silently stops
 * existing the next time the asset is re-saved. CURRENT_STATE.md's instruction is to bind it
 * through the existing Enhanced Input assets and leave the binding checkable.
 *
 * The collision sweep is the half that needed checking first: the obvious key is a mouse button,
 * and the hazard is that one is already held to look around. It asserts generically that no key
 * mapped to the inspect action is mapped to any other action in either context, so it keeps
 * working when a binding is added. What the assets carry is printed on every run.
 *
 * IMC_Default rather than a new context: both contexts are already referenced and listed, so
 * adding a mapping to one is the least new surface.
 *
 * No ticking world: two assets and an action, loaded by path. Whether a key press reaches the
 * handler needs a possessed local player and is deliberately not asserted here.
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

	// What is already bound, reported on every run: evidence kept in the log, not a stale comment.
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
	 * One: IMC_Default maps it at all. An action that exists but is mapped nowhere looks done and
	 * does nothing.
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
	 * Two: every key it uses is free. Written over whatever the contexts contain, so the day
	 * IA_MouseLook grows a held mouse button this fails instead of overlapping it.
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
	 * Three: and not in the look context. IMC_MouseLook is always-on free-look; a menu key there
	 * would tie opening the menu to whether looking is active.
	 */
	TestTrue(
		TEXT("the inspect-piece action belongs in IMC_Default, not in the free-look context"),
		!MouseLookContext->HasMappingForInputAction(InspectAction));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
