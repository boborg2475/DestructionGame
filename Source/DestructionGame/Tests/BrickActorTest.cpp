// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "PhysicsEngine/BodyInstance.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

static_assert(
	!BrickWorldTestSupport::TBrickHasFreeze<ABrickActor>::value,
	"ABrickActor must have no Freeze(): a released brick has moved, nothing in this layer "
	"knows where it went, and FPieceBinding::Box records where it was LAID — so re-freezing "
	"it pins it wherever Chaos left it or teleports it back into the wall.");

static_assert(
	!BrickWorldTestSupport::TBrickHasUnrelease<ABrickActor>::value,
	"ABrickActor must have no Unrelease(): release is a one-way latch, exactly as "
	"FPieceBinding::bReleased and FConnection's giving latch are.");

static_assert(
	!BrickWorldTestSupport::TBrickHasSetSimulating<ABrickActor>::value,
	"ABrickActor must have no SetSimulating(bool): a boolean setter is Freeze() wearing a "
	"parameter, and it makes the bad call expressible again.");

/**
 * A built wall exists in the world at the layout's size and place, and each brick names its own
 * piece from both directions.
 *
 * Asserts GetComponentsBoundingBox, not GetActorLocation: bounds are pivot-agnostic and catch a
 * 100x scale error that a location check misses. The trace checks the reverse: whatever is at
 * piece i's spot resolves back to piece i, which catches correctly placed actors with wrong refs.
 *
 * Mass is read as the override because a kinematic body may have no solver mass; the other test
 * checks it reaches the body once dynamic. Needs a world with a physics scene, though never ticked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBrickActorSpawnedWallMatchesItsLayoutTest,
	"DestructionGame.World.Brick.SpawnedWallMatchesItsLayout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBrickActorSpawnedWallMatchesItsLayoutTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;

	const FRunningBondSpec Spec = WallSpec();

	// A reference layout built independently of the subsystem, to compare the spawned actors against.
	FBrickLayout Reference;
	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

	TestEqual(
		FString::Printf(TEXT("fixture: a flush 2 x 3 wall should be %d pieces, got %d"),
			WallPieceCount, Reference.Structure.NumPieces()),
		Reference.Structure.NumPieces(), WallPieceCount);

	TestEqual(
		FString::Printf(TEXT("fixture: the reference layout must carry one box per piece, got %d"),
			Reference.Boxes.Num()),
		Reference.Boxes.Num(), Reference.Structure.NumPieces());

	/*
	 * Both masses must be present or the per-piece mass check is free. Pinned to 1.9 g/cm3 over a
	 * full brick and a half bat.
	 */
	{
		TArray<double> MassesSeen;
		for (int32 Piece = 0; Piece < Reference.Structure.NumPieces(); ++Piece)
		{
			MassesSeen.AddUnique(MassKgFromBox(Reference.Boxes[Piece], Spec.DensityGramsPerCubicCm));
		}

		TestEqual(
			FString::Printf(TEXT("fixture: the flush wall should hold two distinct masses, got %d"),
				MassesSeen.Num()),
			MassesSeen.Num(), 2);

		/*
		 * With a tolerance: MassKgFromBox is deliberately volume-first, which lands one ulp below
		 * production's density-first 2.72163125, so an exact compare fails on the full brick.
		 */
		auto SeenNear = [&MassesSeen](double ExpectedKg)
		{
			return MassesSeen.ContainsByPredicate(
				[ExpectedKg](double Seen) { return FMath::Abs(Seen - ExpectedKg) < 1.0e-9; });
		};

		TestTrue(
			FString::Printf(TEXT("fixture: a full brick should weigh %.9g kg at 1.9 g/cm3"), FullBrickMassKg),
			SeenNear(FullBrickMassKg));

		TestTrue(
			FString::Printf(TEXT("fixture: a half bat should weigh %.9g kg at 1.9 g/cm3"), HalfBatMassKg),
			SeenNear(HalfBatMassKg));
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);

	TestTrue(
		FString::Printf(TEXT("BuildRunningBond should return a real structure id, got %d"), StructureId),
		StructureId != INDEX_NONE);

	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("Find(%d) should hand back the binding it just built"), StructureId),
		Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("the binding should carry the id it was handed out under, got %d"),
			Binding->StructureId),
		Binding->StructureId, StructureId);

	TestEqual(
		FString::Printf(TEXT("the built wall should span %d piece handles, got %d"),
			WallPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), WallPieceCount);

	TestEqual(
		FString::Printf(TEXT("the built wall should carry the reference wall's %d joints, got %d"),
			Reference.Structure.NumConnections(), Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), Reference.Structure.NumConnections());

	for (int32 Piece = 0; Piece < Reference.Structure.NumPieces() && Piece < Binding->NumPieces(); ++Piece)
	{
		const FPieceBox& Box = Reference.Boxes[Piece];

		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			continue;
		}

		// Bricks never tick: a per-brick tick is what makes thousands of actors expensive.
		TestTrue(
			FString::Printf(TEXT("brick %d must not tick: the solver is pushed, never polled"), Piece),
			!Brick->PrimaryActorTick.bCanEverTick);

		UStaticMeshComponent* Mesh = Brick->GetMesh();

		TestNotNull(
			*FString::Printf(TEXT("brick %d should own a static mesh component"), Piece),
			Mesh);

		if (Mesh == nullptr)
		{
			continue;
		}

		// Movable from spawn: Static mobility cannot simulate, and changing it at release is an ordering trap.
		TestTrue(
			FString::Printf(TEXT("brick %d must be Movable from spawn, mobility is %d"),
				Piece, static_cast<int32>(Mesh->Mobility.GetValue())),
			Mesh->Mobility == EComponentMobility::Movable);

		TestTrue(
			FString::Printf(TEXT("brick %d must be kinematic until something releases it"), Piece),
			!Mesh->IsSimulatingPhysics());

		const FBox Bounds = Brick->GetComponentsBoundingBox(/*bNonColliding*/ true);

		TestTrue(
			FString::Printf(
				TEXT("brick %d should occupy its own box: centre (%g, %g, %g) expected, got (%g, %g, %g)"),
				Piece, Box.CentreCm.X, Box.CentreCm.Y, Box.CentreCm.Z,
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(Box.CentreCm, BoundsToleranceCm));

		TestTrue(
			FString::Printf(
				TEXT("brick %d should be the size its box says: half-extent (%g, %g, %g) expected, got (%g, %g, %g)"),
				Piece, Box.ExtentCm.X, Box.ExtentCm.Y, Box.ExtentCm.Z,
				Bounds.GetExtent().X, Bounds.GetExtent().Y, Bounds.GetExtent().Z),
			Bounds.GetExtent().Equals(Box.ExtentCm, BoundsToleranceCm));

		// Read the override: GetMass reads the solver, where a kinematic body may have no mass (0 == 0).
		const double ExpectedMassKg = MassKgFromBox(Box, Spec.DensityGramsPerCubicCm);
		const FBodyInstance* Body = Mesh->GetBodyInstance();

		TestNotNull(
			*FString::Printf(TEXT("brick %d should have a body instance"), Piece),
			Body);

		if (Body != nullptr)
		{
			TestTrue(
				FString::Printf(TEXT("brick %d should have its mass overridden rather than inferred from a volume"),
					Piece),
				Body->bOverrideMass);

			TestTrue(
				FString::Printf(TEXT("brick %d should weigh %.9g kg, got an override of %.9g"),
					Piece, ExpectedMassKg, static_cast<double>(Body->GetMassOverride())),
				FMath::Abs(static_cast<double>(Body->GetMassOverride()) - ExpectedMassKg) < 1.0e-4);
		}

		// ResolvePiece fails closed on foreign refs, so resolving to this handle also proves the structure id.
		TestEqual(
			FString::Printf(TEXT("brick %d should carry its own structure id, got %d"),
				Piece, Brick->GetPieceRef().StructureId),
			Brick->GetPieceRef().StructureId, StructureId);

		TestEqual(
			FString::Printf(TEXT("brick %d should carry its own piece index, got %d"),
				Piece, Brick->GetPieceRef().PieceIndex),
			Brick->GetPieceRef().PieceIndex, Piece);

		TestEqual(
			FString::Printf(TEXT("brick %d's own ref should resolve back to handle %d, got %d"),
				Piece, Piece, Binding->ResolvePiece(Brick->GetPieceRef())),
			Binding->ResolvePiece(Brick->GetPieceRef()), Piece);
	}

	/*
	 * A trace through each box's centre, along Y across the wall's thickness, resolves to that
	 * piece. Visibility channel, as the piece context menu uses.
	 */
	for (int32 Piece = 0; Piece < Reference.Boxes.Num(); ++Piece)
	{
		const FPieceBox& Box = Reference.Boxes[Piece];

		const FVector Start(Box.CentreCm.X, Box.CentreCm.Y - 100.0, Box.CentreCm.Z);
		const FVector End(Box.CentreCm.X, Box.CentreCm.Y + 100.0, Box.CentreCm.Z);

		FHitResult Hit;
		const bool bHit = TestWorld.World->LineTraceSingleByChannel(
			Hit, Start, End, ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(BrickTrace), true));

		TestTrue(
			FString::Printf(TEXT("a visibility trace through piece %d's centre should hit something"), Piece),
			bHit);

		ABrickActor* HitBrick = Cast<ABrickActor>(Hit.GetActor());

		TestNotNull(
			*FString::Printf(TEXT("the trace at piece %d should hit an ABrickActor, hit %s"),
				Piece, *GetNameSafe(Hit.GetActor())),
			HitBrick);

		if (HitBrick != nullptr)
		{
			TestEqual(
				FString::Printf(TEXT("the trace at piece %d should resolve to piece %d, resolved to %d"),
					Piece, Piece, Binding->ResolvePiece(HitBrick->GetPieceRef())),
				Binding->ResolvePiece(HitBrick->GetPieceRef()), Piece);
		}
	}

	TestWorld.End();

	return true;
}

/**
 * A brick is kinematic until released, and release is one way and idempotent.
 *
 * "Nothing moved" is shown to bite by spawning with physics on. "It fell" is a legitimate
 * displacement assertion here (the claim is movement, not a break; DESIGN.md §4), with a
 * threshold derived from the mortar gap and course pitch.
 *
 * Idempotence is asserted as velocity surviving a second Release: toggling simulation off and on
 * would recreate the body at zero velocity. The fixture first checks the brick is moving.
 *
 * Piece 0 is in the bottom course, with clear air to the floor 50 cm below; a higher brick would
 * settle on the course beneath. Two seconds at a fixed 60 Hz step.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBrickActorKinematicUntilReleasedTest,
	"DestructionGame.World.Brick.KinematicUntilReleased",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBrickActorKinematicUntilReleasedTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;

	const FRunningBondSpec Spec = WallSpec();

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the wall should span %d handles, got %d"),
			WallPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), WallPieceCount);

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
	}

	if (Bricks.Num() != WallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	// One second with gravity on: nothing may move.
	TestWorld.TickSeconds(1.0);

	for (int32 Piece = 0; Piece < Bricks.Num(); ++Piece)
	{
		const double DriftCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		TestTrue(
			FString::Printf(
				TEXT("intact brick %d must not drift in a second of gravity; it moved %.6f cm"),
				Piece, DriftCm),
			DriftCm < DriftToleranceCm);

		TestTrue(
			FString::Printf(TEXT("intact brick %d must still be kinematic after a second"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	constexpr int32 Released = 0;

	ABrickActor* Falling = Bricks[Released];
	UStaticMeshComponent* FallingMesh = Falling->GetMesh();

	if (FallingMesh == nullptr)
	{
		AddError(TEXT("the brick to release has no mesh component"));
		TestWorld.End();
		return true;
	}

	const FVector ReleasedFrom = Falling->GetActorLocation();

	Falling->Release();

	TestTrue(
		TEXT("a released brick must be simulating physics"),
		FallingMesh->IsSimulatingPhysics());

	// The spawn-time mass reached the solver; GetMass is only meaningful once dynamic.
	TestTrue(
		FString::Printf(TEXT("the released brick should weigh %.9g kg in the solver, got %.9g"),
			FullBrickMassKg, static_cast<double>(FallingMesh->GetMass())),
		FMath::Abs(static_cast<double>(FallingMesh->GetMass()) - FullBrickMassKg) < 1.0e-3);

	// Long enough to be moving, short enough not to reach the floor.
	TestWorld.TickSeconds(0.25);

	const FVector VelocityBefore = FallingMesh->GetPhysicsLinearVelocity();

	// Precondition for the idempotence row: a quarter second of free fall is about -245 cm/s.
	TestTrue(
		FString::Printf(TEXT("fixture: the released brick should be falling by now, velocity Z is %.3f cm/s"),
			VelocityBefore.Z),
		VelocityBefore.Z < -100.0);

	Falling->Release();

	TestTrue(
		TEXT("a second Release must leave the brick simulating"),
		FallingMesh->IsSimulatingPhysics());

	// No tick between reads: an idempotent Release leaves velocity equal; recreating the body zeroes it.
	const FVector VelocityAfter = FallingMesh->GetPhysicsLinearVelocity();

	TestTrue(
		FString::Printf(
			TEXT("a second Release must not disturb the fall: velocity was (%.3f, %.3f, %.3f), now (%.3f, %.3f, %.3f)"),
			VelocityBefore.X, VelocityBefore.Y, VelocityBefore.Z,
			VelocityAfter.X, VelocityAfter.Y, VelocityAfter.Z),
		VelocityAfter.Equals(VelocityBefore, 1.0e-3));

	TestWorld.TickSeconds(0.75);

	const double FellCm = ReleasedFrom.Z - Falling->GetActorLocation().Z;

	AddInfo(FString::Printf(
		TEXT("the released brick fell %.3f cm in one second, from Z %.3f to Z %.3f (floor top is at %.1f)"),
		FellCm, ReleasedFrom.Z, Falling->GetActorLocation().Z, FloorTopZCm));

	TestTrue(
		FString::Printf(
			TEXT("the released brick should have fallen more than %.1f cm in a second; it moved %.3f cm"),
			FallenAtLeastCm, FellCm),
		FellCm > FallenAtLeastCm);

	// Releasing one brick releases only that brick.
	for (int32 Piece = 0; Piece < Bricks.Num(); ++Piece)
	{
		if (Piece == Released)
		{
			continue;
		}

		const double DriftCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		TestTrue(
			FString::Printf(
				TEXT("brick %d was not released and must not have moved; it drifted %.6f cm"),
				Piece, DriftCm),
			DriftCm < DriftToleranceCm);

		TestTrue(
			FString::Printf(TEXT("brick %d was not released and must still be kinematic"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
