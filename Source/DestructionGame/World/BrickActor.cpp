// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/BrickActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "RequiredContent.h"
#include "UObject/ConstructorHelpers.h"

ABrickActor::ABrickActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;

	/*
	 * MOVABLE FROM SPAWN, NEVER FROM RELEASE. A Static-mobility component can never be
	 * made to simulate at all, and switching mobility at the moment of release is an
	 * ordering trap — the body has to be recreated, which is exactly what Release must not
	 * do to a brick that is already falling. Kinematic is Movable-and-not-simulating, so
	 * an intact brick is already in the state a released one needs.
	 */
	Mesh->SetMobility(EComponentMobility::Movable);

	/*
	 * The brick's mesh, resolved once onto the CDO, by the path RequiredContent.h names.
	 *
	 * SM_Cube is a placeholder and is treated as one: nothing here assumes it is 100 uu on a
	 * side or that its pivot is anywhere in particular. Whoever spawns a brick reads the
	 * mesh's own bounds and sizes and places the actor from them, so a real brick mesh
	 * dropped in here changes nothing but the asset path. See CURRENT_STATE.md on hard
	 * content references from C++ — this one fails at CDO construction rather than at
	 * compile time if the asset is ever deleted, which is what the required-content table
	 * and its sweep exist to turn into a red test.
	 */
	static ConstructorHelpers::FObjectFinder<UStaticMesh> BrickMeshAsset(
		DestructionContent::BrickPlaceholderMeshPath);

	Mesh->SetStaticMesh(BrickMeshAsset.Object);

	/*
	 * The two highlight overlays, by the same one spelling of their paths. They are separate
	 * assets on purpose: three states that are not three distinguishable looks make the enum
	 * decoration, and a brick that drew as chosen the moment the cursor crossed it takes away
	 * the one thing a player must be able to check before pressing Delete.
	 */
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> HoverMaterialAsset(
		DestructionContent::BrickHoverMaterialPath);

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> SelectedMaterialAsset(
		DestructionContent::BrickSelectedMaterialPath);

	HoverMaterial = HoverMaterialAsset.Object;
	SelectedMaterial = SelectedMaterialAsset.Object;
}

UStaticMeshComponent* ABrickActor::GetMesh() const
{
	return Mesh;
}

void ABrickActor::Release()
{
	/*
	 * THE PHYSICS STATE IS THE RECORD, so there is no second copy of it to drift. A
	 * bReleased flag here would be a third answer to a question FPieceBinding already
	 * keeps and the body itself already knows, and the one that matters is the body's.
	 *
	 * AND THE EARLY RETURN IS WHAT MAKES A SECOND CALL HARMLESS RATHER THAN MERELY
	 * TOLERATED. SetSimulatePhysics on an already-simulating body recreates it, and a
	 * recreated body starts from rest — so a brick a quarter of a second into its fall
	 * would be left hanging still in mid-air, with nothing reporting anything wrong.
	 * There is deliberately no way back: see the compile-time detectors in
	 * Tests/BrickActorTest.cpp for why re-freezing is unsayable rather than unwise.
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
	 * THE STATE IS STORED AND THE OVERLAY IS SET FROM IT, AND THERE IS NO DECISION HERE
	 * BEYOND THAT MAPPING. Which state a brick should be in is the controller's, decided in
	 * one place and asserted there; this end only has to draw whatever it is told, so it
	 * consults nothing — not the state it was in before, not the selection, not a cursor.
	 * That is what keeps it idempotent and order-free rather than merely documented as such.
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

	default:
		break;
	}

	/*
	 * AN OVERLAY, NEVER SLOT 0. Highlighting is additive: the brick keeps its own material
	 * underneath, so None is a single null rather than a restore of something remembered.
	 * UStaticMeshComponent does not override GetDefaultOverlayMaterial, so null here really
	 * does mean "wearing nothing" rather than falling back to some asset's own default.
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
