// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "DestructionGameFlyingPawn.h"
#include "DestructionGamePlayerController.h"
#include "Engine/StaticMesh.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Materials/MaterialInterface.h"
#include "RequiredContent.h"
#include "UObject/UnrealType.h"
#include "World/BrickActor.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named namespace: a unity build merges files into one translation unit.
namespace RequiredContentTestSupport
{
	/** One hard content reference found on a CDO, with a name for failure messages. */
	struct FRequiredContentReference
	{
		FString Where;
		UObject* Object = nullptr;
	};

	/**
	 * Every reference of AssetType that Class itself declares, read off its CDO by reflection so a
	 * new reference is swept without editing this file. Supers are skipped: an engine base class may
	 * legitimately hold a null asset pointer.
	 */
	void CollectDeclaredAssetReferences(
		const UClass* Class,
		const UObject* DefaultObject,
		const UClass* AssetType,
		TArray<FRequiredContentReference>& Out)
	{
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Property = *It;

			if (Property->GetOwnerClass() != Class)
			{
				continue;
			}

			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				if (ObjectProperty->PropertyClass == nullptr
					|| !ObjectProperty->PropertyClass->IsChildOf(AssetType))
				{
					continue;
				}

				FRequiredContentReference Reference;
				Reference.Where = FString::Printf(TEXT("%s::%s"), *Class->GetName(), *Property->GetName());
				Reference.Object = ObjectProperty->GetObjectPropertyValue_InContainer(DefaultObject);
				Out.Add(Reference);

				continue;
			}

			const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);

			if (ArrayProperty == nullptr)
			{
				continue;
			}

			const FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(ArrayProperty->Inner);

			if (Inner == nullptr || Inner->PropertyClass == nullptr
				|| !Inner->PropertyClass->IsChildOf(AssetType))
			{
				continue;
			}

			FScriptArrayHelper Helper(
				ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(DefaultObject));

			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				FRequiredContentReference Reference;
				Reference.Where = FString::Printf(
					TEXT("%s::%s[%d]"), *Class->GetName(), *Property->GetName(), Index);
				Reference.Object = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index));
				Out.Add(Reference);
			}
		}
	}

	/*
	 * Floors, so a sweep that stops finding anything (renamed UPROPERTYs, a changed reflection
	 * API) fails instead of passing over nothing. Sweeps are per class and per asset type.
	 */
	constexpr int32 PawnInputActionFloor = 4;
	constexpr int32 ControllerMappingContextFloor = 2;

	/**
	 * Brick highlight materials: hover, selected, inspected and six joint-readout neighbour colours.
	 * A missing one breaks nothing visibly except that state never lighting up.
	 */
	constexpr int32 BrickHighlightMaterialFloor = 9;

	/** The controller's own input actions (inspect and hover), which neither sweep above reads. */
	constexpr int32 ControllerInputActionFloor = 2;

	/**
	 * Paths the module resolves today: six input actions, two mapping contexts, the brick mesh and
	 * nine highlight materials. A floor, so adding a reference does not require editing this.
	 */
	constexpr int32 RequiredContentPathFloor = 18;
}

/**
 * Every content path hard-referenced from C++ still resolves, and the table matches what the
 * constructors use. FObjectFinder resolves at construction, so a deleted or renamed asset leaves
 * a silent null (no input, no mesh, BuildRunningBond returns INDEX_NONE).
 *
 * Three claims: every path in RequiredContentPaths loads; the CDOs hold no nulls (reflection
 * sweep, floored); and every asset resolved onto a CDO is listed in the table, so the table
 * cannot drift into a parallel list. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRequiredContentResolvesTest,
	"DestructionGame.Content.RequiredAssetsResolve",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRequiredContentResolvesTest::RunTest(const FString& Parameters)
{
	using namespace RequiredContentTestSupport;

	const TArrayView<const TCHAR* const> Paths = DestructionContent::RequiredContentPaths();

	TestTrue(
		FString::Printf(
			TEXT("the required-content table must list at least the %d paths the module resolves by path today; it lists %d"),
			RequiredContentPathFloor, Paths.Num()),
		Paths.Num() >= RequiredContentPathFloor);

	// Well-formedness first, so a later failure is about content rather than the table.
	for (int32 Index = 0; Index < Paths.Num(); ++Index)
	{
		const TCHAR* const Path = Paths[Index];

		const bool bUsable = Path != nullptr && FCString::Strlen(Path) > 0;

		TestTrue(
			FString::Printf(TEXT("required-content row %d must carry a path, got %s"),
				Index, Path == nullptr ? TEXT("null") : TEXT("an empty string")),
			bUsable);

		if (!bUsable)
		{
			continue;
		}

		for (int32 Earlier = 0; Earlier < Index; ++Earlier)
		{
			if (Paths[Earlier] == nullptr)
			{
				continue;
			}

			TestTrue(
				*FString::Printf(TEXT("required-content row %d duplicates row %d ('%s') — each path is listed once"),
					Index, Earlier, Path),
				FCString::Strcmp(Path, Paths[Earlier]) != 0);
		}

		// StaticLoadObject, not a cached pointer: asks whether the asset is still on disk under this name.
		UObject* const Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, Path);

		TestNotNull(
			*FString::Printf(TEXT("required content '%s' must still resolve; it does not"), Path),
			Loaded);
	}

	TArray<FRequiredContentReference> References;

	const int32 PawnStart = References.Num();

	CollectDeclaredAssetReferences(
		ADestructionGameFlyingPawn::StaticClass(),
		GetDefault<ADestructionGameFlyingPawn>(),
		UInputAction::StaticClass(),
		References);

	const int32 PawnFound = References.Num() - PawnStart;

	TestTrue(
		FString::Printf(
			TEXT("the flying pawn should declare at least %d UInputAction references for this sweep to mean anything; found %d"),
			PawnInputActionFloor, PawnFound),
		PawnFound >= PawnInputActionFloor);

	const int32 ControllerStart = References.Num();

	CollectDeclaredAssetReferences(
		ADestructionGamePlayerController::StaticClass(),
		GetDefault<ADestructionGamePlayerController>(),
		UInputMappingContext::StaticClass(),
		References);

	const int32 ControllerFound = References.Num() - ControllerStart;

	TestTrue(
		FString::Printf(
			TEXT("the player controller should hold at least %d input mapping contexts for this sweep to mean anything; found %d"),
			ControllerMappingContextFloor, ControllerFound),
		ControllerFound >= ControllerMappingContextFloor);

	const int32 ControllerActionStart = References.Num();

	CollectDeclaredAssetReferences(
		ADestructionGamePlayerController::StaticClass(),
		GetDefault<ADestructionGamePlayerController>(),
		UInputAction::StaticClass(),
		References);

	const int32 ControllerActionsFound = References.Num() - ControllerActionStart;

	TestTrue(
		FString::Printf(
			TEXT("the player controller should declare at least %d UInputAction reference for the piece menu's input; found %d"),
			ControllerInputActionFloor, ControllerActionsFound),
		ControllerActionsFound >= ControllerInputActionFloor);

	const int32 BrickMaterialStart = References.Num();

	CollectDeclaredAssetReferences(
		ABrickActor::StaticClass(),
		GetDefault<ABrickActor>(),
		UMaterialInterface::StaticClass(),
		References);

	const int32 BrickMaterialsFound = References.Num() - BrickMaterialStart;

	TestTrue(
		FString::Printf(
			TEXT("the brick should declare at least %d UMaterialInterface references for its highlight states; found %d"),
			BrickHighlightMaterialFloor, BrickMaterialsFound),
		BrickMaterialsFound >= BrickHighlightMaterialFloor);

	// The brick mesh is set on the component, not a UPROPERTY, so the reflection sweep misses it.
	{
		const ABrickActor* BrickDefault = GetDefault<ABrickActor>();
		const UStaticMeshComponent* BrickMesh =
			BrickDefault != nullptr ? BrickDefault->GetMesh() : nullptr;

		FRequiredContentReference Reference;
		Reference.Where = TEXT("ABrickActor::Mesh's static mesh");
		Reference.Object = BrickMesh != nullptr ? BrickMesh->GetStaticMesh() : nullptr;
		References.Add(Reference);
	}

	for (const FRequiredContentReference& Reference : References)
	{
		TestNotNull(
			*FString::Printf(
				TEXT("%s resolved to nothing — the asset it names by path has been deleted, renamed or moved"),
				*Reference.Where),
			Reference.Object);

		if (Reference.Object == nullptr)
		{
			continue;
		}

		// Cross-check: fails when a hard reference is added without its table row.
		const FString ResolvedPath = Reference.Object->GetPathName();

		const bool bListed = Paths.ContainsByPredicate(
			[&ResolvedPath](const TCHAR* const Path)
			{
				return Path != nullptr && ResolvedPath.Equals(Path, ESearchCase::IgnoreCase);
			});

		TestTrue(
			*FString::Printf(
				TEXT("%s resolves to '%s', which is not listed in the required-content table — a hard reference nothing sweeps is a hard reference nobody notices breaking"),
				*Reference.Where, *ResolvedPath),
			bListed);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
