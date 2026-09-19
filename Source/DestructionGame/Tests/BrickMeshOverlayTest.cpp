// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Misc/App.h"
#include "World/BrickActor.h"

/*
 * WITH_EDITOR AS WELL AS WITH_DEV_AUTOMATION_TESTS, for the same reason
 * Tests/HighlightMaterialPaintTest.cpp needs it: a static mesh's Nanite SETTING is authoring
 * data, so UStaticMesh::GetNaniteSettings lives under WITH_EDITORONLY_DATA. The runtime half —
 * whether the Nanite pages actually got built — is a different question and is reported below
 * rather than asserted on, because it answers no in a headless run whatever the asset says.
 */
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md.
 */
namespace BrickMeshOverlayTestSupport
{
	/**
	 * One mesh the game draws a highlight OVERLAY on, and where it came from.
	 *
	 * Reached through the production path — the actor's own CDO — never by re-typing an asset
	 * path: a test that named SM_Cube by hand would go on checking an asset the brick no longer
	 * uses and stay green through exactly the swap this test exists to catch.
	 *
	 * One row today, because the rule is about a CAPABILITY an overlay-highlighted mesh must
	 * have rather than about this brick: debris, a ghost preview or a second highlighted actor
	 * is a row here, not a second test.
	 */
	struct FOverlayHighlightedMeshRow
	{
		const TCHAR* Where = nullptr;
		const UStaticMeshComponent* Component = nullptr;
		const UStaticMesh* Mesh = nullptr;
	};

	FOverlayHighlightedMeshRow BrickRow()
	{
		FOverlayHighlightedMeshRow Row;
		Row.Where = TEXT("ABrickActor::Mesh (the mesh SetHighlighted puts its overlay on)");

		const ABrickActor* const BrickDefault = GetDefault<ABrickActor>();

		Row.Component = BrickDefault != nullptr ? BrickDefault->GetMesh() : nullptr;
		Row.Mesh = Row.Component != nullptr ? Row.Component->GetStaticMesh() : nullptr;

		return Row;
	}

	FString DescribeMesh(const UStaticMesh* Mesh)
	{
		return Mesh != nullptr ? Mesh->GetPathName() : FString(TEXT("<none>"));
	}
}

/**
 * A mesh the game highlights with an overlay must be a mesh that can draw one — so it must not
 * be a Nanite mesh.
 *
 * WHAT WAS BROKEN, NOW FIXED — a regression net, not a red step. ABrickActor::SetHighlighted
 * called SetOverlayMaterial correctly for both states and a player still saw NOTHING when
 * pointing at or picking a brick: the brick's mesh, /Game/LevelPrototyping/Meshes/SM_Cube, had
 * Nanite enabled. The fix was one checkbox; both assertions below were red against the unfixed
 * asset.
 *
 * THE CAUSAL CHAIN, read out of the 5.8 engine source. Four scene proxies emit an overlay mesh
 * batch (found by grepping for the assignment `bOverlayMaterial = true`, not the material name):
 * FStaticMeshSceneProxy (StaticMeshSceneProxy.cpp:1469 and 1611), FSkeletalMeshSceneProxy
 * (SkeletalMeshSceneProxy.cpp:840), and the instanced and hierarchical-instanced static mesh
 * proxies (InstancedStaticMesh.cpp:1158, HierarchicalInstancedStaticMesh.cpp:1355/1487) — so ISM
 * bricks could still highlight if that optimisation ever lands. No Nanite proxy does:
 * UStaticMeshComponent::ShouldCreateNaniteProxy gives a Nanite-enabled mesh a Nanite::FSceneProxy
 * with no overlay member at all, no gate, no fallback and no log line — the overlay is simply
 * never drawn.
 *
 * ONE LATENT HAZARD, recorded and not acted on: an ISM'd brick whose overlay material lacks
 * bUsedWithInstancedStaticMeshes would be silently swapped for the default material — the same
 * class of failure, needing a row here (or in Content.HighlightMaterialsPaintSomething) the day
 * the first ISM lands.
 *
 * WHY THIS TEST IS WORTH MORE THAN THE FIX. Highlighting was built, wired and covered by three
 * other green tests (World.Brick.HighlightWearsAMaterial, World.Select,
 * Content.HighlightMaterialsPaintSomething), and all three stayed green while a property of an
 * UNRELATED asset defeated it entirely. ABrickActor's own constructor comment says a real brick
 * mesh "changes nothing but the asset path" — a Nanite-enabled replacement re-breaks highlighting
 * identically.
 *
 * TWO ASSERTIONS: the capability and its cause. UStaticMeshComponent::ShouldCreateNaniteProxy
 * (protected, reached via Nanite::FNaniteResourcesHelper::ShouldCreateNaniteProxy,
 * NaniteResourcesHelper.h:172) is `!UseNanite(ShaderPlatform) || !Component.HasValidNaniteData()`.
 * HasValidNaniteData() is public and runtime, so it is asserted — it measured TRUE against the
 * unfixed asset even with FApp::CanEverRender() FALSE under -nullrhi, because Nanite pages build
 * at load rather than at render. UseNanite(ShaderPlatform) is the machine's RHI and is
 * deliberately left untouched. bEnabled is asserted too, as the fail-closed backstop: it is what
 * travels in git, while the built pages are derived data a DDC-less machine could read false and
 * go green over an asset still authored as Nanite. Neither assertion alone can pass the test.
 *
 * The failure messages below carry this chain rather than just "bEnabled should be false": the
 * rule is not "Nanite is bad", it is that these meshes highlight via overlay and Nanite meshes
 * cannot draw one — Nanite bricks need a different highlight mechanism first.
 *
 * NEEDS A TICKING WORLD: no, and no world at all. A CDO exists from module load, so this reads an
 * asset pointer and a bool.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBrickMeshOverlayTest,
	"DestructionGame.Content.BrickMeshCanDrawTheHighlightOverlay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBrickMeshOverlayTest::RunTest(const FString& Parameters)
{
	using namespace BrickMeshOverlayTestSupport;

	const FOverlayHighlightedMeshRow Rows[] = { BrickRow() };

	for (const FOverlayHighlightedMeshRow& Row : Rows)
	{
		TestNotNull(
			*FString::Printf(TEXT("fixture: %s must resolve to a static mesh component"), Row.Where),
			Row.Component);

		TestNotNull(
			*FString::Printf(
				TEXT("%s must hold a static mesh — a highlight overlay needs something to sit on"),
				Row.Where),
			Row.Mesh);

		if (Row.Component == nullptr || Row.Mesh == nullptr)
		{
			continue;
		}

		const bool bNaniteEnabled = Row.Mesh->GetNaniteSettings().bEnabled;
		const bool bHasNaniteData = Row.Component->HasValidNaniteData();

		/*
		 * All three readings together, so the log settles the argument. CanEverRender is reported
		 * because of what it does NOT imply: against the unfixed asset it was false while
		 * HasValidNaniteData was true, so a reader cannot explain away either assertion by pointing
		 * at -nullrhi. All three read false today; printing them side by side tells a genuine
		 * regression apart from a machine whose DDC never built the pages.
		 */
		AddInfo(FString::Printf(
			TEXT("%s: '%s' — NaniteSettings.bEnabled=%s; HasValidNaniteData()=%s; FApp::CanEverRender()=%s"),
			Row.Where,
			*DescribeMesh(Row.Mesh),
			bNaniteEnabled ? TEXT("TRUE") : TEXT("false"),
			bHasNaniteData ? TEXT("TRUE") : TEXT("false"),
			FApp::CanEverRender() ? TEXT("true") : TEXT("false")));

		/* The capability, as directly as anything is reachable without a GPU — the other conjunct
		 * ShouldCreateNaniteProxy tests is the machine's RHI, deliberately left alone. */
		TestFalse(
			*FString::Printf(
				TEXT("%s: '%s' has BUILT NANITE DATA, so the renderer takes the Nanite path for it ")
				TEXT("and the highlight overlay is never drawn. HasValidNaniteData() is one of the ")
				TEXT("two conjuncts of UStaticMeshComponent::ShouldCreateNaniteProxy ")
				TEXT("(Rendering/NaniteResourcesHelper.h line 172); the other is UseNanite(ShaderPlatform), ")
				TEXT("which is the machine rather than the asset and is not asserted on here"),
				Row.Where, *DescribeMesh(Row.Mesh)),
			bHasNaniteData);

		/* And the cause, which is the half that travels in git: derived data can be missing on a
		 * machine that never built it, but this bool is in the .uasset. */
		TestFalse(
			*FString::Printf(
				TEXT("%s: '%s' has Nanite ENABLED, so it cannot draw the highlight overlay. ")
				TEXT("A Nanite-enabled static mesh is given a Nanite::FSceneProxy, and NO NANITE PROXY ")
				TEXT("EMITS AN OVERLAY MESH BATCH (NaniteSceneProxy.h has no Overlay reference at all), ")
				TEXT("whereas four non-Nanite proxies do: FStaticMeshSceneProxy, FSkeletalMeshSceneProxy, ")
				TEXT("and the INSTANCED and HIERARCHICAL INSTANCED static mesh proxies — so if these ")
				TEXT("bricks ever become ISMs for performance, overlays still work. What does not work ")
				TEXT("is Nanite: ABrickActor::SetHighlighted sets an overlay the renderer silently never ")
				TEXT("draws, with no warning and no log line, and every existing highlight test stays ")
				TEXT("green. Turn Nanite off on this mesh, or give the brick a mesh of its own that has ")
				TEXT("it off. If Nanite is genuinely wanted here, the highlight has to stop being an ")
				TEXT("overlay first — this assertion is about THAT dependency, not about Nanite"),
				Row.Where, *DescribeMesh(Row.Mesh)),
			bNaniteEnabled);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
