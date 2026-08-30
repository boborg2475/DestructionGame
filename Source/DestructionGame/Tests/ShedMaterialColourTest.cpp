// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BrickActor.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md.
 */
namespace ShedMaterialColourTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * THE TWO COLOURS A SHED IS MADE OF, AND WHICH STRUCTURAL MATERIAL EACH BELONGS TO.
	 *
	 * The paths are WRITTEN OUT HERE RATHER THAN IMPORTED from a production constant, on purpose:
	 * this test's whole claim is "a brick actor must wear THIS asset for THIS material", so naming
	 * the asset itself is what makes the test fail if the wiring points somewhere else — importing
	 * DestructionContent::... would make the assertion agree with whatever the game happened to
	 * resolve. Dev declares these same two paths in RequiredContent.h in the green step; this file
	 * is the independent statement they must match.
	 *
	 * The `.M_Shed_Brick` suffix is the object name inside the package, the same shape every other
	 * material path in RequiredContent.h takes.
	 */
	const TCHAR* const ShedBrickMaterialPath = TEXT("/Game/Materials/M_Shed_Brick.M_Shed_Brick");
	const TCHAR* const ShedTimberMaterialPath = TEXT("/Game/Materials/M_Shed_Timber.M_Shed_Timber");

	/** Every piece is this deep on Y; the value is immaterial to a material readout. */
	constexpr double WytheWidthCm = 9.8;
	constexpr double FaceLengthCm = 10.0;
	constexpr double JointThicknessCm = 1.0;

	constexpr double FootingMassKg = 50.0;
	constexpr double PostMassKg = 60.0;

	FPieceBox MakeBox(double CentreZ, double SizeZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(FaceLengthCm, WytheWidthCm, SizeZ) * 0.5;
		Box.CentreCm = FVector(0.0, 0.0, CentreZ);
		return Box;
	}

	/**
	 * Whether a material IS a given asset, or is an instance of it however deep.
	 *
	 * NOT AN EQUALITY, DELIBERATELY — the same choice Tests/BrickHighlightMaterialTest.cpp makes.
	 * What has to be true is that the brick asked for the right ASSET for its material; whether the
	 * thing it hands the renderer is that material or an instance of it is an implementation choice,
	 * and an equality here would outlaw the instance for no reason. The walk is bounded because a
	 * material-instance chain cannot contain a cycle.
	 */
	bool DerivesFrom(const UMaterialInterface* Candidate, const UMaterialInterface* Asset)
	{
		if (Candidate == nullptr || Asset == nullptr)
		{
			return false;
		}

		const UMaterialInterface* Walk = Candidate;

		for (int32 Depth = 0; Walk != nullptr && Depth < 16; ++Depth)
		{
			if (Walk == Asset)
			{
				return true;
			}

			const UMaterialInstance* const Instance = Cast<UMaterialInstance>(Walk);

			Walk = Instance != nullptr ? Instance->Parent : nullptr;
		}

		return false;
	}

	FString DescribeMaterial(const UMaterialInterface* Material)
	{
		return Material != nullptr ? Material->GetPathName() : FString(TEXT("<none>"));
	}

	/**
	 * Lay a grounded ClayBrick footing and a grounded Timber post bearing on it, tag each with its
	 * material, and append one box per piece so the layout's two arrays stay in step (AdoptLayout
	 * refuses a desynced one). Returns false if the interface could not be formed.
	 *
	 * BOTH GROUNDED so nothing solves or falls — BuildLayout spawns and adopts but never solves, so
	 * grounding is belt and braces; the material an actor wears at spawn is what this test reads.
	 */
	bool BuildTwoMaterialLayout(FBrickLayout& OutLayout, int32& OutBrickPiece, int32& OutTimberPiece)
	{
		const FPieceBox FootBox = MakeBox(/*Z*/ 10.0, /*SizeZ*/ 20.0);
		const FPieceBox PostBox = MakeBox(/*Z*/ 31.0, /*SizeZ*/ 20.0);

		OutBrickPiece = OutLayout.Structure.AddPiece(FootingMassKg, /*bIsGrounded*/ true, FootBox.CentreCm);
		OutTimberPiece = OutLayout.Structure.AddPiece(PostMassKg, /*bIsGrounded*/ true, PostBox.CentreCm);

		OutLayout.Boxes.Add(FootBox);
		OutLayout.Boxes.Add(PostBox);

		OutLayout.Structure.SetPieceMaterial(OutBrickPiece, &ClayBrick);
		OutLayout.Structure.SetPieceMaterial(OutTimberPiece, &Timber);

		FConnection Joint;
		if (!MakeInterface(OutBrickPiece, FootBox, OutTimberPiece, PostBox, JointThicknessCm, Unbreakable, Joint))
		{
			return false;
		}

		return OutLayout.Structure.AddConnection(Joint) != INDEX_NONE;
	}
}

/**
 * A BRICK ACTOR WEARS ITS PIECE'S STRUCTURAL MATERIAL AS ITS BASE COLOUR: a ClayBrick piece's
 * actor draws M_Shed_Brick on mesh element 0, a Timber piece's actor draws M_Shed_Timber.
 *
 * WHAT IS BROKEN TODAY. SpawnBrickForPiece (World/DestructionStructureSubsystem.cpp) sizes,
 * places, weighs and names each brick, but it NEVER sets element 0 from the piece's material — so
 * every brick keeps the grey SM_Cube default whatever it is made of, and a shed of brick walls and
 * timber roof reads as one undifferentiated grey box. The user showed a photo of a real brick shed
 * and asked for the basic colour; this is the "colour by structural material" half of that.
 *
 * THE SEAM IS THE REAL SPAWN PATH, NOT A HELPER. This drives UDestructionStructureSubsystem::
 * BuildLayout — the general door the scenario builders go through — so it fails if the mapping is
 * missing OR if the mapping exists and nothing calls it at spawn. That second gap is the exact
 * "green in the world-free halves, wrong at the live join" class the render step keeps catching
 * (CURRENT_STATE.md records AdoptLayout silently dropping bThreeDimensional the same way); a test
 * that only exercised an ABrickActor seam by hand could not see it.
 *
 * WHY BASE MATERIAL, NOT THE OVERLAY. The highlight machinery (hover / selected / neighbour) uses
 * the OVERLAY material — Mesh->SetOverlayMaterial — precisely so a brick keeps its own look
 * underneath. Colour by material is that look underneath: element 0, read with GetMaterial(0). A
 * wiring that painted the overlay instead would leave the wall grey until the cursor crossed it.

 * ASSERTED ON THE ASSET, NOT ON PIXELS. The claim is which material the actor handed the component,
 * checked with GetMaterial(0) and a derives-from walk (so a material instance is allowed), against
 * the two assets loaded by their own paths. "Does it look brick-red" needs a renderer a code-built
 * world has none of; "which asset did the brick ask for" needs nothing but the component.
 *
 * TWO DIFFERENT MATERIALS ON TWO ACTORS, WHICH IS THE DISCRIMINATOR. A wiring that set every brick
 * to one colour — or that read the wrong piece's material — passes a single-material check and
 * fails this: the brick actor wears M_Shed_Brick, the timber actor wears M_Shed_Timber, and the
 * two are asserted to differ so neither can be the other by accident.
 *
 * OTHER MATERIALS ARE OUT OF SCOPE. Only ClayBrick and Timber matter for the shed; a piece of some
 * third material may keep the default, and no case here pins that.
 *
 * NEEDS A TICKING WORLD: NO. It needs a WORLD, because a brick is an actor and this drives the real
 * spawn path — but it lays no wall it must watch fall, solves nothing and ticks nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBrickWearsItsStructuralMaterialColourTest,
	"DestructionGame.World.Brick.WearsItsStructuralMaterialColour",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBrickWearsItsStructuralMaterialColourTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace ShedMaterialColourTestSupport;
	using namespace DestructionProfiles;

	/* ---- The two colour assets must exist and be loadable, or nothing below means anything. ---- */
	UMaterialInterface* const ShedBrickMaterial = LoadObject<UMaterialInterface>(nullptr, ShedBrickMaterialPath);
	UMaterialInterface* const ShedTimberMaterial = LoadObject<UMaterialInterface>(nullptr, ShedTimberMaterialPath);

	TestNotNull(
		*FString::Printf(TEXT("fixture: the brick colour material must load from '%s'"), ShedBrickMaterialPath),
		ShedBrickMaterial);
	TestNotNull(
		*FString::Printf(TEXT("fixture: the timber colour material must load from '%s'"), ShedTimberMaterialPath),
		ShedTimberMaterial);

	if (ShedBrickMaterial == nullptr || ShedTimberMaterial == nullptr)
	{
		return true;
	}

	TestTrue(
		TEXT("fixture: the two shed colours must be different assets or the discriminator is meaningless"),
		ShedBrickMaterial != ShedTimberMaterial);

	/* ---- Build a two-piece layout: a grounded ClayBrick footing and a grounded Timber post. ---- */
	FBrickLayout Layout;
	int32 BrickPiece = INDEX_NONE;
	int32 TimberPiece = INDEX_NONE;

	if (!BuildTwoMaterialLayout(Layout, BrickPiece, TimberPiece))
	{
		AddError(TEXT("FIXTURE: the producer must form the bed joint between footing and post"));
		return true;
	}

	TestEqual(TEXT("FIXTURE: two pieces — the ClayBrick footing and the Timber post"),
		Layout.Structure.NumPieces(), 2);
	TestEqual(TEXT("FIXTURE: one box per piece, or BuildLayout/AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	/* Positive control: the layout carries the materials before it is ever stood up. */
	TestTrue(TEXT("CONTROL: the layout's footing must carry ClayBrick"),
		Layout.Structure.GetPiece(BrickPiece).Material == &ClayBrick);
	TestTrue(TEXT("CONTROL: the layout's post must carry Timber"),
		Layout.Structure.GetPiece(TimberPiece).Material == &Timber);

	/* ---- Stand it up through the real production spawn path. ---- */
	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildLayout(Layout);

	TestTrue(
		*FString::Printf(TEXT("BuildLayout should stand up this well-formed layout; it returned %d"), StructureId),
		StructureId != INDEX_NONE);

	FStructureBinding* const Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(TEXT("the subsystem should hold the structure BuildLayout built"), Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	ABrickActor* const BrickActor = BrickAt(*this, *Binding, BrickPiece);
	ABrickActor* const TimberActor = BrickAt(*this, *Binding, TimberPiece);

	if (BrickActor == nullptr || TimberActor == nullptr
		|| BrickActor->GetMesh() == nullptr || TimberActor->GetMesh() == nullptr)
	{
		TestWorld.End();
		return true;
	}

	UMaterialInterface* const BrickBase = BrickActor->GetMesh()->GetMaterial(0);
	UMaterialInterface* const TimberBase = TimberActor->GetMesh()->GetMaterial(0);

	AddInfo(FString::Printf(
		TEXT("the ClayBrick actor wears '%s' on element 0; the Timber actor wears '%s'"),
		*DescribeMaterial(BrickBase), *DescribeMaterial(TimberBase)));

	/* ------------------------------------------------------------------ *
	 * THE BEHAVIOUR. Each actor's base material (element 0) is its piece's
	 * structural colour. Today both read the SM_Cube default, so both of
	 * these fail as "wears the grey default, not the shed colour" — the
	 * missing wiring, not a broken fixture.
	 * ------------------------------------------------------------------ */
	TestTrue(
		*FString::Printf(
			TEXT("a ClayBrick piece's actor must wear '%s' (or an instance of it) on element 0; it wears '%s'"),
			ShedBrickMaterialPath, *DescribeMaterial(BrickBase)),
		DerivesFrom(BrickBase, ShedBrickMaterial));

	TestTrue(
		*FString::Printf(
			TEXT("a Timber piece's actor must wear '%s' (or an instance of it) on element 0; it wears '%s'"),
			ShedTimberMaterialPath, *DescribeMaterial(TimberBase)),
		DerivesFrom(TimberBase, ShedTimberMaterial));

	/*
	 * AND THE TWO ARE DIFFERENT ON THE TWO ACTORS. A wiring that painted every brick one colour, or
	 * that read the wrong piece's material, passes each check above against that one colour and
	 * fails here.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("the brick and timber actors must wear DIFFERENT base materials; brick wears '%s', timber '%s'"),
			*DescribeMaterial(BrickBase), *DescribeMaterial(TimberBase)),
		BrickBase != TimberBase);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
