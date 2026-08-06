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
	/**
	 * EVERY STATE A BRICK CAN BE IN, AND THE ASSET IT MUST REACH FOR — ONE ROW PER STATE.
	 *
	 * A TABLE RATHER THAN THREE HAND-WRITTEN SWITCHES, AND THAT CHANGE IS WHAT THE SIX NEIGHBOUR
	 * STATES FORCED. With four states a name switch, a slot switch and a hand-written transition
	 * script were readable; with ten they are three lists that have to be kept in step by hand,
	 * and the one that gets forgotten is the script — which is the list whose omissions are
	 * INVISIBLE, because SetHighlighted has a `default:` arm and a state nobody exercised draws
	 * plain rather than failing. Adding a state is now adding a row here, exactly as adding a
	 * material profile is adding a row.
	 *
	 * None CARRIES NO PATH, DELIBERATELY: it wears nothing at all, which is a different claim from
	 * "wears the wrong thing" and is asserted as such below.
	 */
	struct FBrickHighlightRow
	{
		EBrickHighlight State = EBrickHighlight::None;
		const TCHAR* Name = nullptr;

		/** The asset this state must wear, or null for the state that wears nothing. */
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
		{ EBrickHighlight::Neighbour5, TEXT("Neighbour5"), DestructionContent::BrickNeighbourMaterialPaths[5] }
	};

	/** How many states there are, which is how wide the expectation table has to be. */
	constexpr int32 BrickHighlightStateCount = UE_ARRAY_COUNT(BrickHighlightRows);

	/** Index into the table above, found in it rather than kept as a second copy of it. */
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
	 * THE TRANSITION SCRIPT, AND IT STILL COVERS EVERY ORDERED PAIR OF STATES — NOW ALL HUNDRED.
	 *
	 * A SCRIPT RATHER THAN TEN INDEPENDENT CHECKS because every bug in this area is a state left
	 * behind by the previous step: an overlay that is set on the way in and never cleared on the
	 * way out leaves the whole wall lit, and one-shot assertions would each pass on their own.
	 *
	 * GENERATED RATHER THAN WRITTEN OUT, AND THAT IS THE CHANGE THE SIX NEIGHBOURS FORCED. Four
	 * states made a sixteen-step Eulerian circuit that a person could read and check; ten states
	 * make a hundred-step one that nobody can check by eye, so a hand-written script would quietly
	 * become a script that covers MOST pairs — and the pairs it dropped would be invisible, because
	 * SetHighlighted has a `default:` arm and a state nobody exercised sets a NULL overlay and
	 * draws PLAIN. For the strongest states in the scene that is exactly backwards, and it is what
	 * this sweep exists to catch. Setting From and then To makes every ordered pair explicit, and
	 * the step between one pair and the next is a transition too, so the walk stays continuous.
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
 * TEN STATES MUST BE TEN DISTINGUISHABLE ANSWERS. None is nothing at all, and the other nine are
 * nine different materials — if any two shared one the enumerator that shared it would be
 * decoration, and a brick would read as chosen the moment the cursor crossed it, or the joint
 * numbers on screen would belong to any of the lit bricks.
 *
 * THE SIX NEIGHBOUR STATES ARE THE OTHER HALF OF THE JOINT READOUT, AND WITHOUT THEM THE PANEL IS
 * ONLY HALF BUILT. A joint row already draws a coloured swatch and names its far end in words, and
 * in a wall of 1,220 identical bricks a word is not enough to find one by — the colour is the tie,
 * and today nothing in the world wears it, so the tie does not exist. FInspectorJointRow::ColourSlot
 * decides WHICH colour each row is; this is the brick end of the same decision, and World.Select is
 * where the two are held against each other.
 *
 * AND THE `default:` ARM IS WHY THE SWEEP HAS TO NAME EVERY STATE RATHER THAN THE ONES SOMEBODY
 * REMEMBERED. SetHighlighted maps a state to an overlay with a switch that has a default arm, so
 * a state with no case of its own sets a NULL overlay and the brick draws PLAIN — which for the
 * STRONGEST state is exactly backwards, and is invisible to a sweep that stops at three. Six new
 * states arriving at once is precisely when a sweep stops at the ones somebody remembered, which
 * is why the state list, the asset per state and the transition walk are now ONE table.
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

	/*
	 * THE NINE MUST BE NINE DIFFERENT ASSETS, PAIRWISE. Two of them collapsing onto one look makes
	 * the enumerator that shares it decoration, and which pair collapses decides which lie the
	 * player is told: hover onto selected says a brick is chosen the moment the cursor crosses it,
	 * inspected onto selected says the joint numbers on screen belong to any of the bricks that are
	 * lit, and two neighbours onto one says two rows of the readout point at the same brick.
	 *
	 * PAIRWISE OVER THE WHOLE TABLE RATHER THAN THREE NAMED COMPARISONS, which is what makes adding
	 * a state a row: six neighbours arriving at once is 36 pairs, and the copy-paste that ships two
	 * of them identical is exactly the mistake a hand-written list of comparisons does not catch.
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
	 *
	 * SET EXPLICITLY RATHER THAN RELYING ON WHERE THE WALK STOPPED, AND THAT IS WHAT GENERATING
	 * THE SCRIPT CHANGED. The hand-written circuit happened to END on None, so reading the live
	 * overlay here said something; the generated sweep ends on whatever the last row of the table
	 * is, which today is Neighbour5 and tomorrow is whatever gets appended. Asserting on that is
	 * asserting on the GENERATOR. The claim worth keeping is the brick's, so it is driven to None
	 * on purpose and then read — and it is read off the live component AFTER the whole hundred-step
	 * sweep, which is the one thing the per-state table below cannot say: that table reports what
	 * None recorded on its FIRST visit, when the brick had barely been touched.
	 */
	Brick->SetHighlighted(EBrickHighlight::None);

	TestNull(
		*FString::Printf(
			TEXT("a brick back at None must wear no overlay at all, it wears '%s'"),
			*BrickHighlightDescribeOverlay(Brick)),
		Mesh->GetOverlayMaterial());

	/*
	 * EVERY STATE THAT IS NOT None WEARS AN OVERLAY, AND IT IS THE ONE ITS ROW NAMES.
	 *
	 * THIS IS THE ROW THE `default:` ARM SWALLOWS, SIX TIMES OVER. A state SetHighlighted has no
	 * case for takes the default and sets a NULL overlay, so the brick draws PLAIN — for Inspected
	 * that is weaker than merely selected, which is backwards, and for a Neighbour it is the
	 * readout pointing at a brick that never lights, which is the whole tie the panel is drawn to
	 * make. It fails here as "wears no overlay at all" rather than as a wrong asset.
	 *
	 * DERIVED FROM A NAMED ASSET, NOT MERELY NON-NULL. A brick that highlighted itself with a
	 * transient material nobody shipped would satisfy "something is set" and would silently stop
	 * working the day the real asset was deleted.
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
	 * AND THE NINE OVERLAYS ARE NINE DIFFERENT ANSWERS, PAIRWISE. Different ASSETS is asserted at
	 * the top of this test; this is the separate claim that the brick actually reached for a
	 * different one per state, which is what a switch with a shared arm gets wrong — and with six
	 * near-identical new arms, sharing one by copy-paste is the likeliest mistake there is.
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
