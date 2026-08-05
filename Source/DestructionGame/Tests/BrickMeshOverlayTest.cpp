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
	 * THE MESH IS REACHED THROUGH THE PRODUCTION PATH — the actor's own CDO — NEVER BY RE-TYPING
	 * AN ASSET PATH. The failure this test exists for is the placeholder cube being swapped for a
	 * real brick mesh; a test that named SM_Cube by hand would go on checking an asset the brick
	 * no longer uses and stay green through exactly that swap.
	 *
	 * A TABLE WITH ONE ROW TODAY, because the rule is about a CAPABILITY an overlay-highlighted
	 * mesh must have rather than about this brick: the day debris, a ghost preview or a second
	 * highlighted actor lands, it is a row here, not a second test.
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
 * A MESH THE GAME HIGHLIGHTS WITH AN OVERLAY MUST BE A MESH THAT CAN DRAW ONE — SO IT MUST NOT
 * BE A NANITE MESH.
 *
 * WHAT WAS BROKEN WHEN THIS WAS WRITTEN, AND IS FIXED NOW — SO THIS IS A REGRESSION NET AND NOT A
 * RED STEP ANY MORE. ABrickActor::SetHighlighted called UMeshComponent::SetOverlayMaterial with the
 * right asset for the right state, the two overlay materials were well-formed and visibly
 * different, the controller decided the state correctly, and a player still saw NOTHING when they
 * pointed at or picked a brick. The brick's mesh is /Game/LevelPrototyping/Meshes/SM_Cube, and that
 * asset had Nanite enabled. The fix was one checkbox on that asset; both assertions below were red
 * against it and are green against the fixed one.
 *
 * THE CAUSAL CHAIN, READ OUT OF THE 5.8 ENGINE SOURCE RATHER THAN FROM MEMORY — AND THE CLAIM THAT
 * MATTERS IS ABOUT NANITE, NOT ABOUT STATIC MESHES. OverlayMaterial is referenced in 52 files under
 * Engine/Source/Runtime, and FOUR scene proxies actually emit an overlay mesh batch — six sites,
 * found by grepping for the assignment `bOverlayMaterial = true` rather than for the material name:
 * FStaticMeshSceneProxy (Engine/Private/StaticMeshSceneProxy.cpp, the static path at line 1469 and
 * the dynamic one at 1611), FSkeletalMeshSceneProxy (SkeletalMeshSceneProxy.cpp:840),
 * FInstancedStaticMeshSceneProxy (InstancedStaticMesh.cpp:1158) and the hierarchical variant
 * (HierarchicalInstancedStaticMesh.cpp:1355 and 1487). What NO Nanite proxy does is emit one:
 * NaniteSceneProxy.h has zero Overlay references of any kind. UStaticMeshComponent::ShouldCreateNaniteProxy
 * decides which proxy a component gets, and a Nanite-enabled mesh gets a Nanite::FSceneProxy — which
 * has no overlay member at all. There is no gate, no fallback, no ensure and no log line: the
 * overlay is simply never drawn.
 *
 * THE DIRECTION OF THAT CORRECTION IS THE POINT. An earlier version of this comment, and of the
 * failure message below, said StaticMeshSceneProxy.cpp was the ONLY file in Runtime that touches
 * OverlayMaterial. It is not, and the difference is not pedantry: the obvious next optimisation for
 * a 1,220-brick wall is instanced static meshes, and "only FStaticMeshSceneProxy draws overlays"
 * tells the engineer holding that idea that ISM cannot highlight. IT CAN — both ISM and HISM emit
 * overlay batches. The false sentence also sat inside the message somebody reads at the exact
 * moment they are debugging this, which is the worst possible place to be wrong.
 *
 * ONE LATENT HAZARD ON THAT ROUTE, RECORDED AND DELIBERATELY NOT ACTED ON. If bricks ever do become
 * instanced static meshes, an overlay material without bUsedWithInstancedStaticMeshes is silently
 * swapped for the default material — the same class of failure as this one, a highlight defeated by
 * a property of an asset nobody was looking at, and it would need a row here (or in
 * Content.HighlightMaterialsPaintSomething) the day the first ISM lands.
 *
 * WHY THIS TEST IS WORTH MORE THAN THE FIX. The fix is one checkbox on one asset. The failure mode
 * is that highlighting — built, wired and covered by World.Brick.HighlightWearsAMaterial,
 * World.Select and Content.HighlightMaterialsPaintSomething, every one of them green — was defeated
 * entirely by a property of an UNRELATED asset, silently. ABrickActor's own constructor comment anticipates the
 * placeholder being replaced and says a real brick mesh "changes nothing but the asset path"; a
 * Nanite-enabled replacement re-breaks highlighting identically, and every one of those tests
 * stays green.
 *
 * TWO ASSERTIONS, AND THEY ARE THE CAPABILITY AND ITS CAUSE RATHER THAN ONE CLAIM TWICE. The
 * honest statement is the capability — "this mesh cannot render the overlay the highlight depends
 * on" — and the engine's own answer to that is UStaticMeshComponent::ShouldCreateNaniteProxy,
 * which is PROTECTED and reachable only through the public template
 * Nanite::FNaniteResourcesHelper::ShouldCreateNaniteProxy. Its body
 * (Rendering/NaniteResourcesHelper.h, line 172) is
 * `if (!UseNanite(ShaderPlatform) || !Component.HasValidNaniteData())`, and those two conjuncts
 * are very different propositions to a test:
 *
 *   HasValidNaniteData() IS PUBLIC AND RUNTIME, so it is asserted. It reads the built Nanite pages
 *   off the mesh's render data, which is as close to "the renderer will take the Nanite path" as
 *   anything reachable without a GPU. It is genuinely a capability reading rather than a proxy for
 *   one. It measured TRUE against the unfixed asset — and true even though FApp::CanEverRender() is
 *   FALSE in this -nullrhi run, because the pages are built at load rather than at render, which is
 *   worth writing down because assuming the opposite is the obvious mistake. Against the fixed
 *   asset all three readings are false, and the AddInfo below prints them so a future failure can
 *   be told apart from a machine that simply never built the pages.
 *
 *   UseNanite(ShaderPlatform) IS THE MACHINE, so nothing here touches it. Reaching through it
 *   would make the test's answer depend on the RHI of whoever ran it.
 *
 * AND THE AUTHORING FLAG IS ASSERTED AS WELL, AS THE FAIL-CLOSED BACKSTOP, because the runtime
 * reading can only ever be the WEAKER of the two. bEnabled is what is serialised into the .uasset
 * and what travels in git; the built pages are derived data, and a machine whose DDC never built
 * them would read HasValidNaniteData() false and go green over an asset that is still authored as
 * a Nanite mesh and still breaks the moment anyone else opens it. Neither assertion can make this
 * test pass on its own, so the pair fails closed in the direction this project always chooses.
 *
 * SO THE MESSAGES CARRY THE CHAIN. "bEnabled should be false" tells a future reader nothing about
 * why it is a rule, and the rule is not "Nanite is bad" — it is that THESE meshes are highlighted
 * by an overlay and Nanite meshes do not draw overlays. Anyone who wants Nanite bricks has to
 * replace the highlight mechanism first, and the failure says so.
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
		 * ALL THREE READINGS TOGETHER, SO THE LOG SETTLES THE ARGUMENT RATHER THAN THIS COMMENT.
		 * CanEverRender is reported because of what it does NOT imply: against the unfixed asset it
		 * was false while HasValidNaniteData was true, so the Nanite pages exist in a headless run
		 * and a reader cannot explain away either assertion by pointing at -nullrhi. All three read
		 * false today, and printing them side by side is what tells a genuine regression apart from
		 * a machine whose DDC never built the pages.
		 */
		AddInfo(FString::Printf(
			TEXT("%s: '%s' — NaniteSettings.bEnabled=%s; HasValidNaniteData()=%s; FApp::CanEverRender()=%s"),
			Row.Where,
			*DescribeMesh(Row.Mesh),
			bNaniteEnabled ? TEXT("TRUE") : TEXT("false"),
			bHasNaniteData ? TEXT("TRUE") : TEXT("false"),
			FApp::CanEverRender() ? TEXT("true") : TEXT("false")));

		/*
		 * THE CAPABILITY, STATED AS DIRECTLY AS ANYTHING REACHABLE WITHOUT A GPU. This is one of
		 * the two conjuncts ShouldCreateNaniteProxy itself tests; the other is the machine's RHI
		 * and is deliberately left alone.
		 */
		TestFalse(
			*FString::Printf(
				TEXT("%s: '%s' has BUILT NANITE DATA, so the renderer takes the Nanite path for it ")
				TEXT("and the highlight overlay is never drawn. HasValidNaniteData() is one of the ")
				TEXT("two conjuncts of UStaticMeshComponent::ShouldCreateNaniteProxy ")
				TEXT("(Rendering/NaniteResourcesHelper.h line 172); the other is UseNanite(ShaderPlatform), ")
				TEXT("which is the machine rather than the asset and is not asserted on here"),
				Row.Where, *DescribeMesh(Row.Mesh)),
			bHasNaniteData);

		/*
		 * AND THE CAUSE, WHICH IS THE HALF THAT TRAVELS IN GIT. Derived data can be missing on a
		 * machine that never built it; this bool is in the .uasset.
		 */
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
