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
 * CARRY A STRUCTURE'S 3D FLAG from a built layout into the live played world, so a shed
 * authored as genuinely three-dimensional stays three-dimensional once it reaches a
 * FStructureBinding — the same class of AdoptLayout drop the already-green
 * CrossMaterialBearing.AdoptLayoutCarriesPieceMaterial test pins for the per-piece Material.
 *
 * BEHAVIOUR UNDER TEST, in one sentence: when a layout flagged 3D
 * (FStructure::SetThreeDimensional(true), as DestructionShed3D::Build sets) is adopted into a
 * live FStructureBinding, the adopted structure is STILL flagged 3D.
 *
 * WHY THIS IS THE RED. AdoptLayout (Core/StructureBinding.cpp) is the only public route from a
 * laid FBrickLayout into a live binding's FStructure. It replays every piece (mass, grounding,
 * actor, box, Material) and every connection into a FRESH Out structure — but it never copies
 * FStructure::bThreeDimensional. FStructure defaults that flag to false, so a 3D-flagged layout
 * becomes a 2D live structure the moment it is adopted. The live world path is
 * DestructionScenarios::Build -> UDestructionStructureSubsystem::BuildLayout -> AdoptLayout, so
 * every shed the game actually builds loses its 3D flag here. The bridge then poses the adopted
 * structure in 2D, refuses its out-of-plane (Y-normal) corner joints, and the shed misbehaves in
 * play — the exact signature the real-RHI render (DestructionGame.Visual.ScenarioLevelScreenshots)
 * caught: the shed3d overhang did NOT fall when the post was cut, though the world-free oracle on
 * the 3D-flagged structure fells it.
 *
 * THE MECHANISM WITNESS, and why it is the whole test. IsThreeDimensional() on the adopted
 * structure is BINARY and immune to solver jitter — true means the flag rode across, false means
 * it was dropped. That pins the fault unambiguously on AdoptLayout, exactly as the material test's
 * pointer-level witness (adopted Material == &Timber) pins its drop. It needs no world, no bridge
 * and no LP: it reads one bool off the graph AdoptLayout produced.
 *
 * WHY NOT ALSO DRIVE THE POST-CUT COLLAPSE HERE. The behavioural consequence (pull the post, the
 * overhang loses the earth) is ALREADY covered on the built Layout.Structure by
 * World.Scenarios.Shed3DRow ARM 2. Driving it through the 2D-broken binding would be red for a
 * TANGLED reason rather than the flag drop: with the structure posed 2D, the overhang can read
 * Stranded (a diagnostic about the solve, not a support answer), and Stranded is neither Grounded
 * nor Supported — so a "lost the earth" assertion could pass on the broken 2D structure and go
 * green on arrival, asserting nothing. The clean binary witness is the right minimal red; the
 * collapse-level coverage lives where it can be asserted without that ambiguity.
 *
 * THE POSITIVE CONTROL. Before adoption the built layout's OWN structure already reads
 * IsThreeDimensional() == true (DestructionShed3D::Build flags it), so the drop below is
 * unambiguously AdoptLayout's, not a mis-built fixture.
 *
 * NEEDS A TICKING WORLD: NO. DestructionShed3D::Build is arithmetic over boxes and a graph;
 * AdoptLayout is a replay; the assertion is one bool. Stand-in UObjects in the transient package
 * stand in for the brick actors, exactly as the material test and StructureBindingTest do —
 * AdoptLayout only holds the pointers.
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

	/* ------------------------------------------------------------------ *
	 * POSITIVE CONTROL — the built layout's OWN structure is already flagged
	 * 3D. If this failed the fixture would be wrong and the adoption result
	 * meaningless; passing it pins the drop below on AdoptLayout alone.
	 * ------------------------------------------------------------------ */
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

	/* ------------------------------------------------------------------ *
	 * THE MECHANISM WITNESS — the 3D flag itself. Today AdoptLayout replays
	 * pieces and connections into a fresh structure and never copies
	 * bThreeDimensional, which defaults false — so the adopted structure reads
	 * 2D where the layout read 3D. That is the drop, and the render's symptom.
	 * ------------------------------------------------------------------ */
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
