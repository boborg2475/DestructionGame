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
	 * Movable from spawn: Static can never simulate, and changing mobility at release would
	 * recreate the body, which Release must not do to a falling brick.
	 */
	Mesh->SetMobility(EComponentMobility::Movable);

	/*
	 * Placeholder mesh. Spawners size and place from the mesh's own bounds, so a real brick mesh
	 * only changes the path. A deleted asset is caught by the required-content test.
	 */
	static ConstructorHelpers::FObjectFinder<UStaticMesh> BrickMeshAsset(
		DestructionContent::BrickPlaceholderMeshPath);

	Mesh->SetStaticMesh(BrickMeshAsset.Object);

	// Separate assets so hover and selected look different; the player checks the selection before Delete.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> HoverMaterialAsset(
		DestructionContent::BrickHoverMaterialPath);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SelectedMaterialAsset(
		DestructionContent::BrickSelectedMaterialPath);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> InspectedMaterialAsset(
		DestructionContent::BrickInspectedMaterialPath);

	HoverMaterial = HoverMaterialAsset.Object;
	SelectedMaterial = SelectedMaterialAsset.Object;
	InspectedMaterial = InspectedMaterialAsset.Object;

	// Named properties, not a state-indexed array, whose null None slot the content sweep would misread.
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
	 * One overlay per joint-readout colour slot. Six finders, not a loop: a static FObjectFinder
	 * in a loop resolves only the first path, giving every slot the same material.
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
	 * The physics state is the record; no separate flag. The early return matters:
	 * SetSimulatePhysics on a simulating body recreates it at rest, freezing a falling brick
	 * mid-air. There is deliberately no way back (see Tests/BrickActorTest.cpp).
	 */
	if (Mesh->IsSimulatingPhysics())
	{
		return;
	}

	Mesh->SetSimulatePhysics(true);
}

void ABrickActor::SetHighlighted(EBrickHighlight NewHighlight)
{
	// Draws only what it is told; the controller decides the state. Keeps this idempotent.
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
	 * Neighbour states walk the slot mapping rather than six copy-pasted cases. Anything else,
	 * including None, wears nothing; World.Brick.HighlightWearsAMaterial catches a new enumerator
	 * missing its case.
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
	 * An overlay, never slot 0, so None is just null. UStaticMeshComponent has no default overlay,
	 * so null really means none.
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
