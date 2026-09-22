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

/** Uniquely named namespace: unity builds merge files (see CURRENT_STATE.md). */
namespace ShedMaterialColourTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * The shed's two colour materials. Written out rather than imported from RequiredContent.h so the
	 * test fails if the wiring points at a different asset.
	 */
	const TCHAR* const ShedBrickMaterialPath = TEXT("/Game/Materials/M_Shed_Brick.M_Shed_Brick");
	const TCHAR* const ShedTimberMaterialPath = TEXT("/Game/Materials/M_Shed_Timber.M_Shed_Timber");

	/** Piece depth on Y; irrelevant to the material readout. */
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

	/** Whether a material is the asset or an instance of it at any depth (as BrickHighlightMaterialTest). */
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
	 * Lay a grounded ClayBrick footing and a grounded Timber post on it, with materials and one box
	 * per piece (AdoptLayout needs them in step). Returns false if the interface cannot be formed.
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
 * A brick actor wears its piece's structural material on mesh element 0: M_Shed_Brick for
 * ClayBrick, M_Shed_Timber for Timber.
 *
 * Drives the real spawn path (UDestructionStructureSubsystem::BuildLayout), so it also fails if
 * the mapping exists but is not called at spawn. Base material, not the overlay, which highlights
 * use. Asserted on the asset via GetMaterial(0) and a derives-from walk, not pixels. Two actors
 * with different materials rule out a single-colour or wrong-piece wiring. Other materials are
 * out of scope. Needs a world (bricks are actors) but never ticks it.
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

	// Both colour assets must load.
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

	// Two-piece layout: ClayBrick footing and Timber post.
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

	// Control: the layout carries the materials before spawning.
	TestTrue(TEXT("CONTROL: the layout's footing must carry ClayBrick"),
		Layout.Structure.GetPiece(BrickPiece).Material == &ClayBrick);
	TestTrue(TEXT("CONTROL: the layout's post must carry Timber"),
		Layout.Structure.GetPiece(TimberPiece).Material == &Timber);

	// Spawn through the real production path.
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

	// The two actors must differ, ruling out a single-colour wiring.
	TestTrue(
		*FString::Printf(
			TEXT("the brick and timber actors must wear DIFFERENT base materials; brick wears '%s', timber '%s'"),
			*DescribeMaterial(BrickBase), *DescribeMaterial(TimberBase)),
		BrickBase != TimberBase);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
