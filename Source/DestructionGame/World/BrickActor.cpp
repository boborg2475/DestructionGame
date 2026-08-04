// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/BrickActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
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
	 * STORING THE STATE IS ALL THIS DOES, AND THAT IS DELIBERATE. What a brick LOOKS like
	 * is the same untestable inch as the menu widget — a code-built world has no renderer
	 * to ask — so every decision about WHICH state a brick is in lives out in the
	 * controller where it can be asserted, and whatever eventually reads this flag and
	 * swaps a material must add no decision of its own.
	 */
	Highlight = NewHighlight;
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
