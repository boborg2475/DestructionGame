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
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. Every helper carries a BrickHighlight prefix so nothing here can be
 * ambiguous against Tests/PieceMultiSelectTest.cpp's highlight helpers, which are about the
 * same enum from the controller's side. See CURRENT_STATE.md.
 */
namespace BrickHighlightMaterialTestSupport
{
	const TCHAR* BrickHighlightName(EBrickHighlight Highlight)
	{
		switch (Highlight)
		{
		case EBrickHighlight::Hovered:  return TEXT("Hovered");
		case EBrickHighlight::Selected: return TEXT("Selected");
		default:                        return TEXT("None");
		}
	}

	/** Index into the per-state expectation table. Kept beside the enum, not derived from it. */
	int32 BrickHighlightSlot(EBrickHighlight Highlight)
	{
		switch (Highlight)
		{
		case EBrickHighlight::Hovered:  return 1;
		case EBrickHighlight::Selected: return 2;
		default:                        return 0;
		}
	}

	/**
	 * Whether a material IS a given asset, or is an instance of it however deep.
	 *
	 * NOT AN EQUALITY, DELIBERATELY. What has to be true is that the brick asked for the right
	 * ASSET for its state; whether the thing it hands the renderer is that material, a material
	 * instance of it, or a dynamic instance created so a colour can be animated later is an
	 * implementation choice, and an equality here would outlaw two of the three for no reason.
	 * The parent walk is bounded because a material instance chain cannot contain a cycle — the
	 * editor refuses one — and the depth cap is belt and braces against a cooked asset that
	 * somehow does.
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

	/** The overlay a brick is currently wearing, named for a failure message. */
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
	 * THE TRANSITION SCRIPT, AND IT COVERS ALL NINE ORDERED PAIRS OF STATES.
	 *
	 * A brick starts at None, so the twelve calls below walk N->H, H->H, H->S, S->S, S->N, N->N,
	 * N->H, H->N, N->S, S->H, H->S, S->N — every from/to pair including all three self
	 * transitions. A SCRIPT RATHER THAN THREE INDEPENDENT CHECKS because every bug in this area
	 * is a state left behind by the previous step: an overlay that is set on the way in and
	 * never cleared on the way out leaves the whole wall lit, and three one-shot assertions
	 * would each pass on their own.
	 */
	const EBrickHighlight BrickHighlightScript[] = {
		EBrickHighlight::Hovered,
		EBrickHighlight::Hovered,
		EBrickHighlight::Selected,
		EBrickHighlight::Selected,
		EBrickHighlight::None,
		EBrickHighlight::None,
		EBrickHighlight::Hovered,
		EBrickHighlight::None,
		EBrickHighlight::Selected,
		EBrickHighlight::Hovered,
		EBrickHighlight::Selected,
		EBrickHighlight::None
	};
}

/**
 * A BRICK WEARS A MATERIAL FOR THE STATE IT IS IN, ONE PER STATE, AND TAKES IT OFF AGAIN.
 *
 * WHAT IS BROKEN TODAY. EBrickHighlight is decided correctly by the controller, stored correctly
 * by the brick, and READ BY NOBODY — SetHighlighted assigns a field and stops. So selecting six
 * bricks looks exactly like selecting none, and the one thing a player must be able to check
 * before pressing Delete is which bricks are actually going.
 *
 * WHY THIS IS TESTABLE AT ALL, AND WHERE THE UNTESTED INCH MOVES TO. "Does the brick look
 * different" needs a renderer and a code-built world has none. But "which material did the brick
 * ASK FOR for this state" needs nothing but the component it set it on, and that is the half
 * where the bugs are: the wrong state drawing the wrong asset, a hover overlay never cleared, the
 * two states collapsing onto one look. Putting the seam at UMeshComponent::SetOverlayMaterial
 * shrinks what nothing can check from "the whole feature" to "does the shader look nice".
 *
 * AN OVERLAY RATHER THAN A MATERIAL SWAP, and the last assertion is what holds that. Replacing
 * slot 0 means remembering what was there and putting it back, which is a second record of the
 * brick's own appearance and one more thing to leave behind; an overlay is additive and clearing
 * it is a single null.
 *
 * THREE STATES MUST BE THREE DISTINGUISHABLE ANSWERS. None is nothing at all, and Hovered and
 * Selected are two different materials — if they were the same asset the enum would be
 * decoration, and a brick would read as chosen the moment the cursor crossed it.
 *
 * DERIVED FROM A NAMED ASSET, NOT MERELY NON-NULL. A brick that highlighted itself with a
 * transient material nobody shipped would satisfy "something is set" and would silently stop
 * working the day the real asset was deleted. Anchoring each state to a required-content path is
 * what turns that deletion into a red test.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD, because a brick is an actor and this drives the real
 * spawn path rather than a bare NewObject — but it never ticks one, lays no wall and touches no
 * physics.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBrickHighlightMaterialTest,
	"DestructionGame.World.Brick.HighlightWearsAMaterial",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBrickHighlightMaterialTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BrickHighlightMaterialTestSupport;

	/*
	 * THE ASSETS ARE LOADED BY PATH RATHER THAN READ BACK OFF THE BRICK, so a brick that
	 * resolved some other material agrees with nothing but itself and fails here.
	 */
	UMaterialInterface* const HoverMaterial = LoadObject<UMaterialInterface>(
		nullptr, DestructionContent::BrickHoverMaterialPath);

	UMaterialInterface* const SelectedMaterial = LoadObject<UMaterialInterface>(
		nullptr, DestructionContent::BrickSelectedMaterialPath);

	TestNotNull(
		*FString::Printf(TEXT("a hovered brick needs a material at '%s'; it does not resolve"),
			DestructionContent::BrickHoverMaterialPath),
		HoverMaterial);

	TestNotNull(
		*FString::Printf(TEXT("a selected brick needs a material at '%s'; it does not resolve"),
			DestructionContent::BrickSelectedMaterialPath),
		SelectedMaterial);

	TestTrue(
		*FString::Printf(
			TEXT("the two highlight materials must be different assets or the two states draw the same; both are '%s'"),
			HoverMaterial != nullptr ? *HoverMaterial->GetPathName() : TEXT("<none>")),
		HoverMaterial != SelectedMaterial);

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

	/*
	 * A BRICK NOBODY HAS CALLED OUT WEARS NOTHING. None is the zero enumerator precisely so a
	 * freshly spawned brick is a plain one, and an overlay present before anything asked for it
	 * would light the whole wall from the moment it was built.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("as spawned, a brick should read None and wear no overlay; it reads %s wearing %s"),
			BrickHighlightName(Brick->GetHighlight()),
			*BrickHighlightDescribeOverlay(Brick)),
		Brick->GetHighlight() == EBrickHighlight::None && Mesh->GetOverlayMaterial() == nullptr);

	/*
	 * THE BRICK'S OWN MATERIAL IS RECORDED SO IT CAN BE CHECKED AGAINST AT THE END. Highlighting
	 * must be additive: a swap of slot 0 would mean remembering and restoring what was there,
	 * which is a second record of the brick's appearance and one more thing to get out of step.
	 */
	UMaterialInterface* const OwnMaterial = Mesh->GetMaterial(0);

	/*
	 * THE PER-STATE EXPECTATION, CAPTURED ON FIRST SIGHT RATHER THAN WRITTEN DOWN. Every later
	 * visit to the same state must produce the SAME answer, which is what makes SetHighlighted's
	 * documented idempotence and order-freedom falsifiable: an implementation that toggled, or
	 * that only cleared when coming from one particular state, passes a per-state check and
	 * fails this one.
	 */
	UMaterialInterface* Expected[3] = { nullptr, nullptr, nullptr };
	bool bSeen[3] = { true, false, false };

	EBrickHighlight From = EBrickHighlight::None;

	for (const EBrickHighlight To : BrickHighlightScript)
	{
		Brick->SetHighlighted(To);

		UMaterialInterface* const Overlay = Mesh->GetOverlayMaterial();

		const int32 Slot = BrickHighlightSlot(To);

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

		/*
		 * AND THE STORED STATE STILL AGREES WITH THE MATERIAL. The flag is what the controller
		 * and World.Select read; the overlay is what the renderer reads. The moment they can
		 * disagree there are two answers to "is this brick called out".
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s -> %s: GetHighlight should read %s, it reads %s"),
				BrickHighlightName(From), BrickHighlightName(To),
				BrickHighlightName(To), BrickHighlightName(Brick->GetHighlight())),
			Brick->GetHighlight() == To);

		From = To;
	}

	/*
	 * NONE IS NOTHING AT ALL, AND THAT IS THE ASSERTION THE TRAIL DEPENDS ON. Setting the state
	 * back to None has to remove the overlay rather than swap it for a neutral one — anything
	 * non-null there is a brick still drawing an effect, and every brick the cursor has crossed
	 * would keep it.
	 */
	TestNull(
		*FString::Printf(
			TEXT("a brick back at None must wear no overlay at all, it wears '%s'"),
			*BrickHighlightDescribeOverlay(Brick)),
		Mesh->GetOverlayMaterial());

	TestNotNull(
		TEXT("a HOVERED brick must wear an overlay — a state nothing draws is a state a player cannot see"),
		Expected[BrickHighlightSlot(EBrickHighlight::Hovered)]);

	TestNotNull(
		TEXT("a SELECTED brick must wear an overlay — this is the one a player checks before pressing Delete"),
		Expected[BrickHighlightSlot(EBrickHighlight::Selected)]);

	TestTrue(
		*FString::Printf(
			TEXT("Hovered and Selected must not draw the same thing; both wear '%s'"),
			Expected[1] != nullptr ? *Expected[1]->GetPathName() : TEXT("<none>")),
		Expected[1] != Expected[2]);

	TestTrue(
		*FString::Printf(
			TEXT("a hovered brick must wear '%s' (or an instance of it); it wears '%s'"),
			DestructionContent::BrickHoverMaterialPath,
			Expected[1] != nullptr ? *Expected[1]->GetPathName() : TEXT("<none>")),
		BrickHighlightDerivesFrom(Expected[1], HoverMaterial));

	TestTrue(
		*FString::Printf(
			TEXT("a selected brick must wear '%s' (or an instance of it); it wears '%s'"),
			DestructionContent::BrickSelectedMaterialPath,
			Expected[2] != nullptr ? *Expected[2]->GetPathName() : TEXT("<none>")),
		BrickHighlightDerivesFrom(Expected[2], SelectedMaterial));

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
