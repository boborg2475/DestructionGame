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
	/** Floor on existing mappings (eight plus one today) so the collision sweep cannot pass vacuously. */
	constexpr int32 ExistingMappingFloor = 6;

	/** Every mapping in a context, for the log. */
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
 * The piece-menu input action exists, is mapped in IMC_Default, and its key is used by no other
 * action in either context (a mouse button may already be held to look). A hand-authored binding
 * that is never asserted can vanish on re-save. No world; whether a press reaches the handler is
 * not asserted here.
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

	// One: IMC_Default maps it at all.
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

	// Two: every key it uses is free, checked against whatever the contexts contain.
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

	// Three: not in the look context, or opening the menu would depend on look being active.
	TestTrue(
		TEXT("the inspect-piece action belongs in IMC_Default, not in the free-look context"),
		!MouseLookContext->HasMappingForInputAction(InspectAction));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
