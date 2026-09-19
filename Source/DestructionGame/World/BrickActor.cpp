// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/BrickActor.h"

#include "Components/StaticMeshComponent.h"
#include "Core/PieceMenu.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "RequiredContent.h"
#include "UObject/ConstructorHelpers.h"

EBrickHighlight BrickHighlightForNeighbourSlot(int32 ColourSlot)
{
	switch (ColourSlot)
	{
	case 0: return EBrickHighlight::Neighbour0;
	case 1: return EBrickHighlight::Neighbour1;
	case 2: return EBrickHighlight::Neighbour2;
	case 3: return EBrickHighlight::Neighbour3;
	case 4: return EBrickHighlight::Neighbour4;
	case 5: return EBrickHighlight::Neighbour5;
	}

	return EBrickHighlight::None;
}

EBrickHighlight BrickHighlightForLoadBand(EJointMarginBand Band)
{
	switch (Band)
	{
	case EJointMarginBand::Comfortable: return EBrickHighlight::LoadComfortable;
	case EJointMarginBand::Caution:     return EBrickHighlight::LoadCaution;
	case EJointMarginBand::Critical:    return EBrickHighlight::LoadCritical;
	}

	return EBrickHighlight::None;
}

ABrickActor::ABrickActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;

	/*
	 * Movable from spawn, never from Release. A Static-mobility component can never be made
	 * to simulate at all, and switching mobility at release time is an ordering trap — the
	 * body would have to be recreated, which is what Release must not do to a brick already
	 * falling. Kinematic is Movable-and-not-simulating, so an intact brick is already in the
	 * state a released one needs.
	 */
	Mesh->SetMobility(EComponentMobility::Movable);

	/*
	 * The brick's mesh, resolved once onto the CDO, by the path RequiredContent.h names.
	 *
	 * SM_Cube is a placeholder and is treated as one: nothing here assumes it is 100 uu on a
	 * side or where its pivot sits. Whoever spawns a brick reads the mesh's own bounds and
	 * sizes and places the actor from them, so a real brick mesh dropped in here changes
	 * nothing but the asset path. See CURRENT_STATE.md on hard content references from C++ —
	 * this fails at CDO construction, not compile time, if the asset is ever deleted, which
	 * is what the required-content table and its sweep exist to turn into a red test.
	 */
	static ConstructorHelpers::FObjectFinder<UStaticMesh> BrickMeshAsset(
		DestructionContent::BrickPlaceholderMeshPath);

	Mesh->SetStaticMesh(BrickMeshAsset.Object);

	/*
	 * The three highlight overlays, by the same one spelling of their paths. Separate assets
	 * on purpose: four states that are not four distinguishable looks make the enum
	 * decoration, and a brick that drew as chosen the moment the cursor crossed it takes away
	 * the one thing a player must be able to check before pressing Delete.
	 */
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> HoverMaterialAsset(
		DestructionContent::BrickHoverMaterialPath);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SelectedMaterialAsset(
		DestructionContent::BrickSelectedMaterialPath);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> InspectedMaterialAsset(
		DestructionContent::BrickInspectedMaterialPath);

	HoverMaterial = HoverMaterialAsset.Object;
	SelectedMaterial = SelectedMaterialAsset.Object;
	InspectedMaterial = InspectedMaterialAsset.Object;

	/*
	 * The load overlay's three bands, one finder each. Three named properties and not an
	 * array, for the reason the three above are named: these are indexed by a state rather
	 * than a number the model computed, and an array indexed by state carries a null in
	 * None's slot the required-content sweep cannot tell from a reference that stopped
	 * resolving.
	 */
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> LoadComfortableMaterialAsset(
		DestructionContent::BrickLoadComfortableMaterialPath);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> LoadCautionMaterialAsset(
		DestructionContent::BrickLoadCautionMaterialPath);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> LoadCriticalMaterialAsset(
		DestructionContent::BrickLoadCriticalMaterialPath);

	LoadComfortableMaterial = LoadComfortableMaterialAsset.Object;
	LoadCautionMaterial = LoadCautionMaterialAsset.Object;
	LoadCriticalMaterial = LoadCriticalMaterialAsset.Object;

	/*
	 * One overlay per colour slot of the joint readout, so a row of numbers and a brick in
	 * the world are the same colour.
	 *
	 * Six finders rather than a loop — the engine's rule, not a preference: an FObjectFinder
	 * must be static, resolving once for the CDO inside a constructor, so a static declared
	 * inside a loop would resolve the first path six times and hand every slot the same
	 * material. Each asset is added at the index its own path came from; two slots sharing an
	 * asset fail pairwise in World.Brick.HighlightWearsAMaterial rather than merely looking odd.
	 */
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Neighbour0MaterialAsset(
		DestructionContent::BrickNeighbourMaterialPaths[0]);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Neighbour1MaterialAsset(
		DestructionContent::BrickNeighbourMaterialPaths[1]);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Neighbour2MaterialAsset(
		DestructionContent::BrickNeighbourMaterialPaths[2]);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Neighbour3MaterialAsset(
		DestructionContent::BrickNeighbourMaterialPaths[3]);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Neighbour4MaterialAsset(
		DestructionContent::BrickNeighbourMaterialPaths[4]);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Neighbour5MaterialAsset(
		DestructionContent::BrickNeighbourMaterialPaths[5]);

	NeighbourMaterials.Add(Neighbour0MaterialAsset.Object);
	NeighbourMaterials.Add(Neighbour1MaterialAsset.Object);
	NeighbourMaterials.Add(Neighbour2MaterialAsset.Object);
	NeighbourMaterials.Add(Neighbour3MaterialAsset.Object);
	NeighbourMaterials.Add(Neighbour4MaterialAsset.Object);
	NeighbourMaterials.Add(Neighbour5MaterialAsset.Object);
}

UStaticMeshComponent* ABrickActor::GetMesh() const
{
	return Mesh;
}

void ABrickActor::Release()
{
	/*
	 * The physics state is the record, so there is no second copy of it to drift: a
	 * bReleased flag here would be a third answer to a question FPieceBinding and the body
	 * itself already know, and the body's is the one that matters.
	 *
	 * The early return is what makes a second call harmless rather than merely tolerated:
	 * SetSimulatePhysics on an already-simulating body recreates it, and a recreated body
	 * starts from rest — a brick a quarter second into its fall would freeze hanging in
	 * mid-air with nothing reporting anything wrong. There is deliberately no way back; see
	 * the compile-time detectors in Tests/BrickActorTest.cpp for why re-freezing is unsayable.
	 */
	if (Mesh->IsSimulatingPhysics())
	{
		return;
	}

	Mesh->SetSimulatePhysics(true);
}

void ABrickActor::SetHighlighted(EBrickHighlight NewHighlight)
{
	/*
	 * The state is stored and the overlay set from it — no decision here beyond that
	 * mapping. Which state a brick should be in is the controller's, decided and asserted
	 * there; this end only draws whatever it is told, consulting nothing else — not the
	 * prior state, not the selection, not a cursor. That is what keeps it idempotent and
	 * order-free.
	 */
	Highlight = NewHighlight;

	UMaterialInterface* Overlay = nullptr;

	switch (NewHighlight)
	{
	case EBrickHighlight::Hovered:
		Overlay = HoverMaterial;
		break;

	case EBrickHighlight::Selected:
		Overlay = SelectedMaterial;
		break;

	case EBrickHighlight::Inspected:
		Overlay = InspectedMaterial;
		break;

	/*
	 * The three load bands, named here rather than answered through BrickHighlightForLoadBand's
	 * inverse. The six neighbours below go through their slot mapping because a slot is a
	 * number and walking the mapping is cheaper than a hand-written inverse; a band is one of
	 * three states with a property each, the same shape as the three above.
	 */
	case EBrickHighlight::LoadComfortable:
		Overlay = LoadComfortableMaterial;
		break;

	case EBrickHighlight::LoadCaution:
		Overlay = LoadCautionMaterial;
		break;

	case EBrickHighlight::LoadCritical:
		Overlay = LoadCriticalMaterial;
		break;

	/*
	 * The six neighbour states are answered by asking the one slot mapping rather than six
	 * more arms: six near-identical cases is exactly where a copy-paste hands two slots one
	 * material, so this walks the existing mapping in the direction it does not already run,
	 * leaving one table rather than a table and its hand-written inverse.
	 *
	 * None, and any state that reaches the end of this, wears nothing — right for None, and
	 * wrong-looking for everything else: a new enumerator added without a case draws plain,
	 * and for a state stronger than Selected that is backwards — the brick the player is
	 * reading would be the only one in the wall not lit. World.Brick.HighlightWearsAMaterial
	 * sweeps every state so the omission fails as "wears no overlay" rather than being
	 * swallowed here.
	 */
	default:
		for (int32 Slot = 0; Slot < NeighbourMaterials.Num(); ++Slot)
		{
			if (BrickHighlightForNeighbourSlot(Slot) == NewHighlight)
			{
				Overlay = NeighbourMaterials[Slot];
				break;
			}
		}

		break;
	}

	/*
	 * An overlay, never slot 0. Highlighting is additive: the brick keeps its own material
	 * underneath, so None is a single null rather than a restore of something remembered.
	 * UStaticMeshComponent does not override GetDefaultOverlayMaterial, so null here really
	 * means "wearing nothing" rather than falling back to some asset's own default.
	 */
	Mesh->SetOverlayMaterial(Overlay);
}

EBrickHighlight ABrickActor::GetHighlight() const
{
	return Highlight;
}

const FPieceRef& ABrickActor::GetPieceRef() const
{
	return PieceRef;
}

void ABrickActor::SetPieceRef(const FPieceRef& Ref)
{
	PieceRef = Ref;
}
