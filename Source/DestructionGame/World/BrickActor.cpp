// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/BrickActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

/*
 * The brick's mesh, resolved once onto the CDO.
 *
 * SM_Cube is a placeholder and is treated as one: nothing here assumes it is 100 uu on a
 * side or that its pivot is anywhere in particular. Whoever spawns a brick reads the
 * mesh's own bounds and sizes and places the actor from them, so a real brick mesh
 * dropped in here changes nothing but the asset path. See CURRENT_STATE.md on hard
 * content references from C++ — this one fails at CDO construction rather than at
 * compile time if the asset is ever deleted.
 */
namespace
{
	const TCHAR* const BrickPlaceholderMeshPath = TEXT("/Game/LevelPrototyping/Meshes/SM_Cube.SM_Cube");
}

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

	static ConstructorHelpers::FObjectFinder<UStaticMesh> BrickMeshAsset(BrickPlaceholderMeshPath);

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

const FPieceRef& ABrickActor::GetPieceRef() const
{
	return PieceRef;
}

void ABrickActor::SetPieceRef(const FPieceRef& Ref)
{
	PieceRef = Ref;
}
