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

/**
 * NAMED NAMESPACE, and named differently from every other one in this module. An anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one — at which point two anonymous helpers of the same name are the same
 * declaration twice. See CURRENT_STATE.md; the `using namespace` lives inside RunTest for
 * the same reason.
 */
namespace RequiredContentTestSupport
{
	/** One hard content reference found on a CDO, with enough context to name it in a failure. */
	struct FRequiredContentReference
	{
		FString Where;
		UObject* Object = nullptr;
	};

	/**
	 * Every reference of a given asset type that OUR OWN class declares, read off its CDO.
	 *
	 * REFLECTION RATHER THAN A HAND-WRITTEN LIST, because the failure this test exists for is
	 * a reference nobody remembered to check. A fifth input action added to the pawn is swept
	 * for free; a hand-written list would have to be edited alongside it, and the edit is
	 * exactly what gets forgotten.
	 *
	 * PROPERTIES DECLARED ON THE CLASS ITSELF, NOT ON ITS SUPERS. These are the references
	 * this project owns and this project resolves. An engine base class may legitimately
	 * carry a null asset pointer of the same type — that is its business, not a broken
	 * reference of ours — and sweeping it would make this test fail for somebody else's
	 * defaults.
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

	/**
	 * THE FLOORS, so a sweep that stopped finding anything fails rather than passing in
	 * silence. A class whose UPROPERTYs were renamed, or a reflection API that changed shape
	 * under us, would otherwise turn this whole test into a sweep over nothing.
	 */
	constexpr int32 PawnInputActionFloor = 4;
	constexpr int32 ControllerMappingContextFloor = 2;

	/**
	 * AND THE BRICK'S HIGHLIGHT MATERIALS, which is a FOURTH sweep for the same reason the
	 * controller's input actions needed a third: the sweeps are per class AND per asset type,
	 * so the first UMaterialInterface to land anywhere in the module is swept by none of the
	 * existing three.
	 *
	 * It is the reference whose absence is the quietest of the lot. Deleting the material does
	 * not break a spawn, a solve or a click — the game runs exactly as it does now, and the only
	 * symptom is that picking six bricks looks identical to picking none.
	 *
	 * NINE, BECAUSE THE SIX NEIGHBOUR COLOURS ARE SIX MORE STATES A BRICK CAN BE IN. Ten highlight
	 * states need nine materials and None needs nothing: hover, selected, inspected, and one per
	 * colour slot of the joint readout, so that a row of numbers and the brick it names are the
	 * same colour. A brick that resolved eight of the nine leaves exactly one joint row pointing
	 * at a brick that never lights, which is the quietest failure in the quietest reference here.
	 */
	constexpr int32 BrickHighlightMaterialFloor = 9;

	/**
	 * AND THE CONTROLLER'S OWN INPUT ACTIONS, which is a THIRD sweep rather than a wider one.
	 *
	 * The two sweeps above are per class AND per asset type, so a UInputAction declared on the
	 * player controller is swept by neither: the pawn sweep reads the pawn, and the controller
	 * sweep reads only its UInputMappingContext properties. That gap is not hypothetical — the
	 * piece menu's input action is bound on the CONTROLLER, deliberately (it owns the mapping
	 * contexts, it outlives any pawn, and it is what has the cursor and the deprojection), so
	 * the first reference to land in it is exactly the one nothing was watching.
	 *
	 * TWO, because the controller now carries the hover input as well as the inspect one. A
	 * floor rather than an equality for the same reason as the others.
	 */
	constexpr int32 ControllerInputActionFloor = 2;

	/**
	 * The six input actions, two mapping contexts, one placeholder brick mesh and NINE highlight
	 * materials the module resolves by path today. A FLOOR rather than an equality: adding a
	 * hard reference is meant to be adding a row, and a test asserting an exact count would have
	 * to be edited alongside every one of them.
	 *
	 * The six neighbour materials are what took this from 12 to 18, and they are the case the
	 * cross-check below is for: they are resolved onto the brick's CDO, so a table that did not
	 * list them would leave six live hard references nothing sweeps.
	 */
	constexpr int32 RequiredContentPathFloor = 18;
}

/**
 * EVERY CONTENT PATH THIS MODULE HARD-REFERENCES FROM C++ STILL RESOLVES, AND THE TABLE OF
 * THEM IS THE SAME LIST THE CONSTRUCTORS ACTUALLY USE.
 *
 * WHY THIS TEST EXISTS. ConstructorHelpers::FObjectFinder resolves a path AT CONSTRUCTION,
 * not at compile time. Delete or rename an asset and every one of these pointers is quietly
 * null: the pawn stops responding to input, the mapping contexts are never added, a brick
 * spawns with no mesh and BuildRunningBond answers INDEX_NONE. Nothing crashes and nothing
 * says why. CURRENT_STATE.md records that hazard and asks for exactly this — the thing that
 * turns content deletion into a red test.
 *
 * THREE CLAIMS, AND THEY CLOSE DIFFERENT HOLES.
 *
 * THE TABLE LOADS. Every path in DestructionContent::RequiredContentPaths must resolve
 * through StaticLoadObject. This is the half that covers a path no CDO happens to hold —
 * a level, a mesh referenced from a scenario, anything resolved later than startup.
 *
 * THE CDOs ARE NOT HOLDING NULLS. Read by reflection off the classes' own declared
 * properties, so a reference added later is swept without editing this file, and floored so
 * a sweep that found nothing fails rather than passing. This is the direct statement of the
 * hazard: whatever the table says, the pawn's own MoveAction is either there or it is not.
 *
 * AND THE TWO ARE THE SAME LIST. Every asset actually resolved onto a CDO must appear in
 * the table, by its own GetPathName. Without this the table can drift into a parallel list
 * of paths that all happen to load while the reference the game uses is somewhere else
 * entirely — a green test guarding nothing.
 *
 * NEEDS A TICKING WORLD: no, and deliberately not. A CDO exists from module load and
 * StaticLoadObject needs no world, so this runs at arithmetic speed alongside the solver
 * tests rather than costing tens of milliseconds of world setup.
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

	/* Well-formedness first, so a later failure is about content rather than about the table. */
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

		/*
		 * THE ASSERTION THE RECORDED HAZARD ASKS FOR. StaticLoadObject rather than a cached
		 * pointer: this has to answer "is the asset still on disk under this name", which is
		 * precisely what a rename or a delete changes and what a CDO resolved at startup can
		 * no longer tell us about on its own.
		 */
		UObject* const Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, Path);

		TestNotNull(
			*FString::Printf(TEXT("required content '%s' must still resolve; it does not"), Path),
			Loaded);
	}

	/*
	 * WHAT THE CLASSES THEMSELVES ARE HOLDING. Collected by reflection so the sweep survives
	 * a fifth input action, and floored so it cannot become a sweep over nothing.
	 */
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

	/*
	 * AND THE BRICK MESH, WHICH IS NOT A UPROPERTY OF THE SAME SHAPE. It is set onto the
	 * mesh component's own asset pointer in the constructor, so the reflection sweep above
	 * would never see it — and it is the reference whose absence is the most silent of the
	 * lot: SpawnBrickForPiece answers null, BuildRunningBond answers INDEX_NONE, and the
	 * scenario simply builds no wall.
	 */
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

		/*
		 * THE CROSS-CHECK, AND IT IS WHAT KEEPS THE TABLE HONEST. A table of paths that all
		 * load is worth nothing if it is not the list the game actually resolves; this is the
		 * other direction, and it fails the day somebody adds a hard reference without adding
		 * its row.
		 */
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
