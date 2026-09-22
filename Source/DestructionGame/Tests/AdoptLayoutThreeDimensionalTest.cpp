// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"

#include "Components/StaticMeshComponent.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A layout's 3D flag must survive AdoptLayout into a live FStructureBinding (as
 * AdoptLayoutCarriesPieceMaterial does for Material). AdoptLayout replays into a fresh
 * structure; without copying bThreeDimensional (default false) every played shed is posed in
 * 2D and its out-of-plane corners are refused, so the shed3d overhang did not fall when its post
 * was cut.
 *
 * Asserts IsThreeDimensional() directly: binary and free of solver jitter. The post-cut collapse
 * is covered by World.Scenarios.Shed3DRow; here a 2D-posed overhang could read Stranded and make
 * a collapse assertion vacuous. A positive control confirms the built layout is already 3D.
 *
 * World-free; transient UObjects stand in for brick actors. Named namespace for unity builds.
 */
namespace AdoptLayoutThreeDimensionalTestSupport
{
	using namespace DestructionLayout;

	/** A rooted stand-in for a brick actor; AdoptLayout only holds it. */
	UObject* MakeStandIn()
	{
		UObject* StandIn = NewObject<UStaticMeshComponent>(GetTransientPackage());
		StandIn->AddToRoot();
		return StandIn;
	}

	void ReleaseStandIns(const TArray<UObject*>& StandIns)
	{
		for (UObject* StandIn : StandIns)
		{
			if (StandIn != nullptr)
			{
				StandIn->RemoveFromRoot();
			}
		}
	}
}

/** A structure's 3D flag survives AdoptLayout. See the file header. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAdoptLayoutCarriesThreeDimensionalFlagTest,
	"DestructionGame.Core.StructureBinding.AdoptLayoutCarriesThreeDimensionalFlag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAdoptLayoutCarriesThreeDimensionalFlagTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace AdoptLayoutThreeDimensionalTestSupport;

	// The 3D shed is the simplest genuinely 3D, 3D-flagged layout.
	FBrickLayout Layout;
	const bool bBuilt = DestructionShed3D::Build(DestructionShed3D::FShed3DSpec{}, Layout);

	TestTrue(TEXT("FIXTURE: the 3D shed must build"), bBuilt);

	if (!bBuilt)
	{
		return false;
	}

	// Positive control: the layout itself is already 3D.
	TestTrue(
		TEXT("CONTROL: the built layout's own structure must already be flagged 3D before adoption"),
		Layout.Structure.IsThreeDimensional());

	TestEqual(TEXT("FIXTURE: one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestTrue(TEXT("FIXTURE: the shed must have laid some pieces"),
		Layout.Structure.NumPieces() > 0);

	// Adopt through the play path.
	TArray<UObject*> StandIns;
	StandIns.Reserve(Layout.Structure.NumPieces());
	for (int32 PieceIndex = 0; PieceIndex < Layout.Structure.NumPieces(); ++PieceIndex)
	{
		StandIns.Add(MakeStandIn());
	}

	FStructureBinding Binding;
	Binding.StructureId = 11;

	const bool bAdopted = AdoptLayout(Layout, StandIns, Binding);

	TestTrue(TEXT("AdoptLayout should adopt this well-formed 3D layout"), bAdopted);

	if (!bAdopted)
	{
		ReleaseStandIns(StandIns);
		return false;
	}

	TestEqual(TEXT("adoption should carry every piece"),
		Binding.GetStructure().NumPieces(), Layout.Structure.NumPieces());

	AddInfo(FString::Printf(
		TEXT("ADOPTED IsThreeDimensional = %s (layout had true)"),
		Binding.GetStructure().IsThreeDimensional() ? TEXT("true") : TEXT("false")));

	TestTrue(
		TEXT("the ADOPTED structure must still be flagged 3D — AdoptLayout must copy the 3D flag, else the "
			"world bridge poses the shed in 2D, drops its Y-normal corners, and the overhang will not fall"),
		Binding.GetStructure().IsThreeDimensional());

	ReleaseStandIns(StandIns);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
