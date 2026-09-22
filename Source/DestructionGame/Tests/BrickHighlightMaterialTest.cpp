// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "RequiredContent.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BrickActor.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace with BrickHighlight-prefixed helpers, so unity builds do not collide with
 * PieceMultiSelectTest.cpp's highlight helpers.
 */
namespace BrickHighlightMaterialTestSupport
{
	/**
	 * Every highlight state and the asset it must wear, one row each. One table drives the names,
	 * slots and transition walk, so a new state cannot be silently skipped (SetHighlighted's
	 * default arm would draw it plain). None has no path: it wears nothing.
	 */
	struct FBrickHighlightRow
	{
		EBrickHighlight State = EBrickHighlight::None;
		const TCHAR* Name = nullptr;

		/** The asset this state must wear, or null for None. */
		const TCHAR* Path = nullptr;
	};

	const FBrickHighlightRow BrickHighlightRows[] = {
		{ EBrickHighlight::None,       TEXT("None"),       nullptr },
		{ EBrickHighlight::Hovered,    TEXT("Hovered"),    DestructionContent::BrickHoverMaterialPath },
		{ EBrickHighlight::Selected,   TEXT("Selected"),   DestructionContent::BrickSelectedMaterialPath },
		{ EBrickHighlight::Inspected,  TEXT("Inspected"),  DestructionContent::BrickInspectedMaterialPath },
		{ EBrickHighlight::Neighbour0, TEXT("Neighbour0"), DestructionContent::BrickNeighbourMaterialPaths[0] },
		{ EBrickHighlight::Neighbour1, TEXT("Neighbour1"), DestructionContent::BrickNeighbourMaterialPaths[1] },
		{ EBrickHighlight::Neighbour2, TEXT("Neighbour2"), DestructionContent::BrickNeighbourMaterialPaths[2] },
		{ EBrickHighlight::Neighbour3, TEXT("Neighbour3"), DestructionContent::BrickNeighbourMaterialPaths[3] },
		{ EBrickHighlight::Neighbour4, TEXT("Neighbour4"), DestructionContent::BrickNeighbourMaterialPaths[4] },
		{ EBrickHighlight::Neighbour5, TEXT("Neighbour5"), DestructionContent::BrickNeighbourMaterialPaths[5] },

		/*
		 * The load overlay's bands, shown on every live piece at once. A band that fell through to
		 * the default arm would draw plain and look plausible.
		 */
		{ EBrickHighlight::LoadComfortable, TEXT("LoadComfortable"), DestructionContent::BrickLoadComfortableMaterialPath },
		{ EBrickHighlight::LoadCaution,     TEXT("LoadCaution"),     DestructionContent::BrickLoadCautionMaterialPath },
		{ EBrickHighlight::LoadCritical,    TEXT("LoadCritical"),    DestructionContent::BrickLoadCriticalMaterialPath }
	};

	constexpr int32 BrickHighlightStateCount = UE_ARRAY_COUNT(BrickHighlightRows);

	/** Row index for a state, or INDEX_NONE. */
	int32 BrickHighlightSlot(EBrickHighlight Highlight)
	{
		for (int32 Slot = 0; Slot < BrickHighlightStateCount; ++Slot)
		{
			if (BrickHighlightRows[Slot].State == Highlight)
			{
				return Slot;
			}
		}

		return INDEX_NONE;
	}

	const TCHAR* BrickHighlightName(EBrickHighlight Highlight)
	{
		const int32 Slot = BrickHighlightSlot(Highlight);

		return Slot == INDEX_NONE ? TEXT("<a state this test does not know about>")
			: BrickHighlightRows[Slot].Name;
	}

	/**
	 * Whether a material is the asset or an instance of it at any depth. Not equality, so a
	 * material instance or dynamic instance also passes. The depth cap guards against a cycle.
	 */
	bool BrickHighlightDerivesFrom(const UMaterialInterface* Candidate, const UMaterialInterface* Asset)
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

	/** The brick's current overlay path, for failure messages. */
	FString BrickHighlightDescribeOverlay(const ABrickActor* Brick)
	{
		if (Brick == nullptr || Brick->GetMesh() == nullptr)
		{
			return TEXT("<no mesh>");
		}

		const UMaterialInterface* const Overlay = Brick->GetMesh()->GetOverlayMaterial();

		return Overlay != nullptr ? Overlay->GetPathName() : FString(TEXT("<none>"));
	}

	/**
	 * A transition walk covering every ordered pair of states, generated from the table. A walk,
	 * not independent checks, because the bugs here are state left behind by the previous step.
	 */
	TArray<EBrickHighlight> BrickHighlightWalk()
	{
		TArray<EBrickHighlight> Walk;

		for (const FBrickHighlightRow& From : BrickHighlightRows)
		{
			for (const FBrickHighlightRow& To : BrickHighlightRows)
			{
				Walk.Add(From.State);
				Walk.Add(To.State);
			}
		}

		return Walk;
	}
}

/**
 * A brick wears a distinct overlay material per highlight state and removes it at None.
 *
 * The renderer cannot be checked headless, but the material the brick asks for can: the seam is
 * UMeshComponent::SetOverlayMaterial. An overlay, not a slot-0 swap, so clearing is a single null;
 * the final assertion holds slot 0 unchanged.
 *
 * Every non-None state must wear a different asset, each derived from a required-content path
 * (so deleting the asset fails the test). The neighbour states tie the joint readout's colour
 * swatches to bricks in the world (FInspectorJointRow::ColourSlot; World.Select checks the pair).
 *
 * Needs a world to spawn the brick, but never ticks it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBrickHighlightMaterialTest,
	"DestructionGame.World.Brick.HighlightWearsAMaterial",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBrickHighlightMaterialTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BrickHighlightMaterialTestSupport;

	// Loaded by path, not read off the brick, so a brick using the wrong material fails.
	UMaterialInterface* Assets[BrickHighlightStateCount] = {};

	for (int32 Slot = 0; Slot < BrickHighlightStateCount; ++Slot)
	{
		const FBrickHighlightRow& Row = BrickHighlightRows[Slot];

		if (Row.Path == nullptr)
		{
			continue;
		}

		Assets[Slot] = LoadObject<UMaterialInterface>(nullptr, Row.Path);

		TestNotNull(
			*FString::Printf(TEXT("a %s brick needs a material at '%s'; it does not resolve"),
				Row.Name, Row.Path),
			Assets[Slot]);
	}

	// The assets must be pairwise distinct, or two states look the same.
	for (int32 Left = 0; Left < BrickHighlightStateCount; ++Left)
	{
		if (BrickHighlightRows[Left].Path == nullptr)
		{
			continue;
		}

		for (int32 Right = Left + 1; Right < BrickHighlightStateCount; ++Right)
		{
			if (BrickHighlightRows[Right].Path == nullptr)
			{
				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("%s and %s must be different assets or the two states draw the same; both are '%s'"),
					BrickHighlightRows[Left].Name, BrickHighlightRows[Right].Name,
					Assets[Left] != nullptr ? *Assets[Left]->GetPathName() : TEXT("<none>")),
				Assets[Left] != Assets[Right]);
		}
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	ABrickActor* const Brick = TestWorld.World->SpawnActor<ABrickActor>();

	TestNotNull(TEXT("fixture: the test world should spawn an ABrickActor"), Brick);

	if (Brick == nullptr || Brick->GetMesh() == nullptr)
	{
		TestWorld.End();
		return true;
	}

	UStaticMeshComponent* const Mesh = Brick->GetMesh();

	// A freshly spawned brick reads None and wears no overlay.
	TestTrue(
		*FString::Printf(
			TEXT("as spawned, a brick should read None and wear no overlay; it reads %s wearing %s"),
			BrickHighlightName(Brick->GetHighlight()),
			*BrickHighlightDescribeOverlay(Brick)),
		Brick->GetHighlight() == EBrickHighlight::None && Mesh->GetOverlayMaterial() == nullptr);

	// Recorded to check at the end that highlighting never touched slot 0.
	UMaterialInterface* const OwnMaterial = Mesh->GetMaterial(0);

	/*
	 * Each state's overlay is captured on first visit; every later visit must match, which tests
	 * idempotence and order-independence.
	 */
	UMaterialInterface* Expected[BrickHighlightStateCount] = {};
	bool bSeen[BrickHighlightStateCount] = {};

	EBrickHighlight From = EBrickHighlight::None;

	for (const EBrickHighlight To : BrickHighlightWalk())
	{
		Brick->SetHighlighted(To);

		UMaterialInterface* const Overlay = Mesh->GetOverlayMaterial();

		const int32 Slot = BrickHighlightSlot(To);

		if (Slot == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("the walk visited a state this test has no row for; add it to BrickHighlightRows")));

			continue;
		}

		if (!bSeen[Slot])
		{
			Expected[Slot] = Overlay;
			bSeen[Slot] = true;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s -> %s: the overlay must depend only on the state, so it should be '%s'; it is '%s'"),
				BrickHighlightName(From), BrickHighlightName(To),
				Expected[Slot] != nullptr ? *Expected[Slot]->GetPathName() : TEXT("<none>"),
				*BrickHighlightDescribeOverlay(Brick)),
			Overlay == Expected[Slot]);

		// The stored state (read by the controller) must agree with the overlay (read by the renderer).
		TestTrue(
			*FString::Printf(
				TEXT("%s -> %s: GetHighlight should read %s, it reads %s"),
				BrickHighlightName(From), BrickHighlightName(To),
				BrickHighlightName(To), BrickHighlightName(Brick->GetHighlight())),
			Brick->GetHighlight() == To);

		From = To;
	}

	/*
	 * After the full sweep, None must remove the overlay entirely, or every brick the cursor
	 * crossed stays lit. Set explicitly, since the generated walk does not end on None.
	 */
	Brick->SetHighlighted(EBrickHighlight::None);

	TestNull(
		*FString::Printf(
			TEXT("a brick back at None must wear no overlay at all, it wears '%s'"),
			*BrickHighlightDescribeOverlay(Brick)),
		Mesh->GetOverlayMaterial());

	/*
	 * Every non-None state wears the overlay its row names. A state missing from SetHighlighted's
	 * switch falls to the default arm and fails here as "no overlay".
	 */
	for (int32 Slot = 0; Slot < BrickHighlightStateCount; ++Slot)
	{
		const FBrickHighlightRow& Row = BrickHighlightRows[Slot];

		if (Row.Path == nullptr)
		{
			TestNull(
				*FString::Printf(
					TEXT("a %s brick must wear NOTHING; it wears '%s'"),
					Row.Name,
					Expected[Slot] != nullptr ? *Expected[Slot]->GetPathName() : TEXT("<none>")),
				Expected[Slot]);

			continue;
		}

		TestNotNull(
			*FString::Printf(
				TEXT("a %s brick must wear an overlay — a state nothing draws is a state a player cannot see"),
				Row.Name),
			Expected[Slot]);

		TestTrue(
			*FString::Printf(
				TEXT("a %s brick must wear '%s' (or an instance of it); it wears '%s'"),
				Row.Name, Row.Path,
				Expected[Slot] != nullptr ? *Expected[Slot]->GetPathName() : TEXT("<none>")),
			BrickHighlightDerivesFrom(Expected[Slot], Assets[Slot]));
	}

	/*
	 * The worn overlays are pairwise distinct too (distinct assets were checked above); this
	 * catches a switch with a shared or copy-pasted arm.
	 */
	for (int32 Left = 0; Left < BrickHighlightStateCount; ++Left)
	{
		if (BrickHighlightRows[Left].Path == nullptr)
		{
			continue;
		}

		for (int32 Right = Left + 1; Right < BrickHighlightStateCount; ++Right)
		{
			if (BrickHighlightRows[Right].Path == nullptr)
			{
				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("%s and %s must not draw the same thing; both wear '%s'"),
					BrickHighlightRows[Left].Name, BrickHighlightRows[Right].Name,
					Expected[Left] != nullptr ? *Expected[Left]->GetPathName() : TEXT("<none>")),
				Expected[Left] != Expected[Right]);
		}
	}

	TestTrue(
		*FString::Printf(
			TEXT("highlighting must be an OVERLAY: the brick's own slot 0 material should still be '%s', it is '%s'"),
			OwnMaterial != nullptr ? *OwnMaterial->GetPathName() : TEXT("<none>"),
			Mesh->GetMaterial(0) != nullptr ? *Mesh->GetMaterial(0)->GetPathName() : TEXT("<none>")),
		Mesh->GetMaterial(0) == OwnMaterial);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
