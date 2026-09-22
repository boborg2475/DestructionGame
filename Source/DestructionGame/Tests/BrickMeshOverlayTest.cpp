// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Misc/App.h"
#include "World/BrickActor.h"

// WITH_EDITOR because UStaticMesh::GetNaniteSettings is editor-only data (as in HighlightMaterialPaintTest.cpp).
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

// Uniquely named namespace: unity builds merge translation units (CURRENT_STATE.md).
namespace BrickMeshOverlayTestSupport
{
	/**
	 * A mesh the game draws a highlight overlay on, reached through the actor's CDO rather than
	 * a typed asset path so a mesh swap is caught. Other overlay-highlighted meshes become rows.
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
 * A mesh highlighted with an overlay must not be Nanite, because Nanite::FSceneProxy never draws
 * overlays (silently; no log). Regression net: SM_Cube had Nanite enabled and highlighting was
 * invisible while three other highlight tests stayed green.
 *
 * Non-Nanite static, skeletal, ISM and HISM proxies all emit overlay batches (UE 5.8 source), so
 * ISM bricks would still highlight. Latent hazard, not acted on: an ISM brick's overlay material
 * would need bUsedWithInstancedStaticMeshes.
 *
 * Asserts HasValidNaniteData() (the runtime conjunct of ShouldCreateNaniteProxy; true for the
 * broken asset even under -nullrhi) and bEnabled (what is stored in git, in case the DDC never
 * built pages). UseNanite(ShaderPlatform) is the machine's RHI and is not asserted.
 * No world needed: reads the CDO.
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
		 * Log all three so a real regression can be told from a machine whose DDC never built the
		 * pages. CanEverRender is false under -nullrhi and does not explain away either assertion.
		 */
		AddInfo(FString::Printf(
			TEXT("%s: '%s' — NaniteSettings.bEnabled=%s; HasValidNaniteData()=%s; FApp::CanEverRender()=%s"),
			Row.Where,
			*DescribeMesh(Row.Mesh),
			bNaniteEnabled ? TEXT("TRUE") : TEXT("false"),
			bHasNaniteData ? TEXT("TRUE") : TEXT("false"),
			FApp::CanEverRender() ? TEXT("true") : TEXT("false")));

		// The capability, as directly as reachable without a GPU.
		TestFalse(
			*FString::Printf(
				TEXT("%s: '%s' has BUILT NANITE DATA, so the renderer takes the Nanite path for it ")
				TEXT("and the highlight overlay is never drawn. HasValidNaniteData() is one of the ")
				TEXT("two conjuncts of UStaticMeshComponent::ShouldCreateNaniteProxy ")
				TEXT("(Rendering/NaniteResourcesHelper.h line 172); the other is UseNanite(ShaderPlatform), ")
				TEXT("which is the machine rather than the asset and is not asserted on here"),
				Row.Where, *DescribeMesh(Row.Mesh)),
			bHasNaniteData);

		// The cause, stored in the .uasset rather than derived data.
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
