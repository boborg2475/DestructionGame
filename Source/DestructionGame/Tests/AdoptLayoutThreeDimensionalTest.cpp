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
 * A layout's 3D flag must survive AdoptLayout into a live FStructureBinding, so a shed
 * authored as three-dimensional (FStructure::SetThreeDimensional(true), as
 * DestructionShed3D::Build sets) stays 3D once it reaches the played world — the same
 * class of drop CrossMaterialBearing.AdoptLayoutCarriesPieceMaterial pins for Material.
 *
 * WHY THIS IS THE RED. AdoptLayout (Core/StructureBinding.cpp) is the only route from a
 * laid FBrickLayout into a live FStructure: it replays every piece and connection into a
 * FRESH Out structure but never copies bThreeDimensional, which defaults false. Every
 * shed the game builds (DestructionScenarios::Build -> BuildLayout -> AdoptLayout) loses
 * its 3D flag here, so the bridge poses it in 2D and refuses its out-of-plane corner
 * joints — the exact symptom the real-RHI render caught: the shed3d overhang did not
 * fall when the post was cut, though the world-free oracle on the 3D-flagged structure
 * fells it.
 *
 * THE MECHANISM WITNESS. IsThreeDimensional() on the adopted structure is binary and
 * immune to solver jitter, so it pins the fault unambiguously on AdoptLayout — the same
 * way the material test's pointer-level witness pins its own drop.
 *
 * WHY NOT ALSO DRIVE THE POST-CUT COLLAPSE HERE. That consequence is already covered on
 * the built Layout.Structure by World.Scenarios.Shed3DRow ARM 2. Through the 2D-broken
 * binding it would be red for a tangled reason: a 2D-posed overhang can read Stranded,
 * which is neither Grounded nor Supported, so a "lost the earth" assertion could pass on
 * the broken structure and assert nothing.
 *
 * THE POSITIVE CONTROL below confirms the built layout's own structure already reads
 * IsThreeDimensional() == true, so the drop is unambiguously AdoptLayout's.
 *
 * NEEDS A TICKING WORLD: NO. DestructionShed3D::Build is arithmetic over boxes and a
 * graph; AdoptLayout is a replay; the assertion is one bool. Stand-in UObjects in the
 * transient package stand in for brick actors, as CrossMaterialBearing does.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace AdoptLayoutThreeDimensionalTestSupport
{
	using namespace DestructionLayout;

	/** A rooted stand-in for a brick actor: AdoptLayout only holds it, so any UObject serves. */
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

/**
 * A structure's 3D flag survives AdoptLayout, so a 3D-authored shed stays 3D in the live world.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAdoptLayoutCarriesThreeDimensionalFlagTest,
	"DestructionGame.Core.StructureBinding.AdoptLayoutCarriesThreeDimensionalFlag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAdoptLayoutCarriesThreeDimensionalFlagTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace AdoptLayoutThreeDimensionalTestSupport;

	/* ---- BUILD THE 3D SHED LAYOUT — the cleanest source of a genuinely 3D, 3D-flagged structure. ---- */
	FBrickLayout Layout;
	const bool bBuilt = DestructionShed3D::Build(DestructionShed3D::FShed3DSpec{}, Layout);

	TestTrue(TEXT("FIXTURE: the 3D shed must build"), bBuilt);

	if (!bBuilt)
	{
		return false;
	}

	/* POSITIVE CONTROL — the layout's own structure must already read 3D, or the fixture
	 * is wrong and the result below is meaningless. */
	TestTrue(
		TEXT("CONTROL: the built layout's own structure must already be flagged 3D before adoption"),
		Layout.Structure.IsThreeDimensional());

	/* Shape sanity — AdoptLayout refuses a layout whose arrays are out of step. */
	TestEqual(TEXT("FIXTURE: one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestTrue(TEXT("FIXTURE: the shed must have laid some pieces"),
		Layout.Structure.NumPieces() > 0);

	/* ---- ADOPT THROUGH THE PLAY PATH. ---- */
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

	/* The adopted graph must at least be the right shape before its flag means anything. */
	TestEqual(TEXT("adoption should carry every piece"),
		Binding.GetStructure().NumPieces(), Layout.Structure.NumPieces());

	/* THE MECHANISM WITNESS — the 3D flag itself. AdoptLayout replays pieces and
	 * connections into a fresh structure and never copies bThreeDimensional (defaults
	 * false), so the adopted structure reads 2D where the layout read 3D. */
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
