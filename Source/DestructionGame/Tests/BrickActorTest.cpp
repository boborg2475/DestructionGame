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
 * A BUILT WALL EXISTS IN THE WORLD, AT THE SIZE AND PLACE THE LAYOUT SAID, AND EACH BRICK
 * CAN NAME ITS OWN PIECE FROM BOTH DIRECTIONS.
 *
 * THE ASSERTION IS GetComponentsBoundingBox, NOT GetActorLocation, and the choice is the
 * whole test. World bounds versus the layout's own box is pivot-agnostic — if SM_Cube's
 * origin is at its base rather than its centre, every brick sits half a height too high
 * and a location assertion passes happily — and it catches a 100x scale error in the same
 * comparison, because a brick scaled by dimension/1 instead of dimension/100 has bounds a
 * hundred times too big while its location is perfect. Deriving the scale as
 * DesiredSizeCm / MeshLocalSizeCm rather than DesiredSizeCm / 100 is what keeps that true
 * the day a real brick mesh replaces the cube.
 *
 * THE TRACE IS THE SAME CLAIM FROM THE OTHER SIDE. Bounds say "the brick for piece i is
 * where piece i is"; a trace into that spot says "whatever is there resolves back to piece
 * i". A wall whose actors were spawned in the right places but handed the wrong refs
 * passes the first and fails the second, and that is exactly the bug that makes the player
 * click one brick and a different one fall.
 *
 * THE MASS IS READ AS THE OVERRIDE, not as the solver's mass, because it is asserted AT
 * SPAWN and a kinematic body may have no mass in the solver at all. That the override
 * actually reaches the physics body is the other test's job, once the brick is dynamic.
 *
 * NEEDS A TICKING WORLD: yes, though this test never ticks it — spawning actors and
 * tracing needs a world with a physics scene, which is the same harness.
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

	/*
	 * THE REFERENCE LAYOUT IS BUILT HERE, SEPARATELY. The subsystem is expected to call
	 * RunningBond itself; laying the same wall a second time in the test gives something
	 * to compare the spawned actors against that did not come out of the subsystem, so a
	 * spawner that invented its own geometry has something to disagree with.
	 */
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
	 * BOTH MASSES HAVE TO BE PRESENT OR THE PER-PIECE MASS CHECK IS FREE, and both are
	 * pinned to published figures rather than to whatever the producer said: 1.9 g/cm3
	 * (ClayBrick, EN-cited in MaterialProfiles.cpp) over 21.5 x 10.25 x 6.5 cm and over
	 * the 10.25-long half bat.
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
		 * COMPARED WITH A TOLERANCE, AND THE ULP IS THE REASON. CURRENT_STATE.md records
		 * that density-first, `1.9 x 21.5 x 10.25 x 6.5 / 1000`, lands exactly on
		 * 2.72163125 while volume-first lands one ulp low at 2.7216312499999997 — and
		 * MassKgFromBox above is deliberately volume-first, so that it is not a
		 * transcription of production's order. An exact Contains here therefore fails on
		 * the full brick and passes on the half bat, which is a fixture artefact and not a
		 * fact about any wall. (It really does: this was written with == first.)
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

		/*
		 * THE SOLVER IS PUSHED, NEVER POLLED. A per-brick tick is what makes thousands of
		 * actors expensive rather than merely numerous, and nothing about a brick needs to
		 * run every frame — CURRENT_STATE.md's building-scale note is the arithmetic.
		 */
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

		/*
		 * MOVABLE FROM SPAWN. A Static-mobility component can never be made to simulate,
		 * and changing mobility at release is an ordering trap — so the assertion is made
		 * here, before anything has been released, rather than after.
		 */
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

		/*
		 * MASS AT SPAWN, READ AS THE CONFIGURED OVERRIDE. UPrimitiveComponent::GetMass
		 * reads the solver, and a kinematic body may have no mass there at all — so a
		 * GetMass assertion here risks being 0 == 0. The override is what
		 * SetMassOverrideInKg wrote, it is readable while kinematic, and the other test
		 * checks that it actually reaches the body once the brick is dynamic.
		 */
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

		/*
		 * THE BRICK CARRIES ITS OWN IDENTITY, and it resolves. FStructureBinding::ResolvePiece
		 * already fails closed on a foreign, stale or out-of-range ref, so a ref that comes
		 * back as the handle we asked about is a ref that named the right structure too.
		 */
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
	 * AND A TRACE LANDS ON THE PIECE THAT IS THERE, for every brick in the wall.
	 *
	 * The trace runs along Y, across the wall's thickness, through each box's own centre —
	 * so nothing else in the wall is in the way and the answer is unambiguous. This is the
	 * query the piece context menu will make when the player clicks a brick, which is why
	 * the channel is Visibility rather than something invented for the test.
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
 * A BRICK IS KINEMATIC UNTIL IT IS RELEASED, AND RELEASE IS ONE WAY AND IDEMPOTENT.
 *
 * THREE CLAIMS, AND EACH HAS A NAMED WAY OF BEING GREEN FOR NOTHING:
 *
 * "Nothing moved" is green because nothing was ever going to move — an assertion that a
 * pile of actors with no physics stayed still. It is proved to bite by spawning the wall
 * with SetSimulatePhysics(true), at which point the bricks fall and every row fails.
 *
 * "It fell" is the outcome assertion, and it is the one that has to be an OUTCOME:
 * DESIGN.md §4 is explicit that displacement is never a valid BREAK assertion, because
 * two pieces can sever and stay resting exactly in place. This is the other direction —
 * the claim IS that the brick moved, and the threshold is derived from the mortar gap and
 * the course pitch rather than picked.
 *
 * "Release is idempotent" is UNFALSIFIABLE if the assertion is only "still simulating",
 * because a second SetSimulatePhysics(true) on an already-simulating body is harmless.
 * The assertion that bites is that the VELOCITY SURVIVES: mutate Release to
 * SetSimulatePhysics(false) followed by SetSimulatePhysics(true) and the body is recreated
 * with zero velocity, so a brick that had been falling for a quarter of a second is
 * suddenly hanging still. That mutation is the proof this row earns its runtime, and the
 * fixture asserts the brick is genuinely moving first so the comparison is not 0 against 0.
 *
 * WHY PIECE 0. It is in the bottom course, so there is nothing but clear air between it
 * and the floor 50 cm below. Releasing a brick higher up would land it on the kinematic
 * course beneath after about one mortar joint of travel, which is precisely the settle the
 * threshold exists to be distinguishable from.
 *
 * NEEDS A TICKING WORLD: yes, and it is the reason this file exists at all. Two seconds of
 * simulated time at 60 Hz, on a fixed step, never on a settle poll.
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

	/* ONE SECOND OF PHYSICS WITH GRAVITY ON, AND NOTHING MAY MOVE. */
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

	/*
	 * AND THE MASS SET AT SPAWN REACHED THE SOLVER. This is the half the other test cannot
	 * make: GetMass reads the physics body, which is only meaningful once the body is
	 * dynamic. Reading it here rather than at spawn is deliberate; reading it at spawn and
	 * finding 0 == 0 would be worse than not reading it.
	 */
	TestTrue(
		FString::Printf(TEXT("the released brick should weigh %.9g kg in the solver, got %.9g"),
			FullBrickMassKg, static_cast<double>(FallingMesh->GetMass())),
		FMath::Abs(static_cast<double>(FallingMesh->GetMass()) - FullBrickMassKg) < 1.0e-3);

	/* Long enough to be plainly moving, short enough not to have reached the floor. */
	TestWorld.TickSeconds(0.25);

	const FVector VelocityBefore = FallingMesh->GetPhysicsLinearVelocity();

	/*
	 * THE FIXTURE'S PRECONDITION FOR THE IDEMPOTENCE ROW. A quarter second of free fall is
	 * about -245 cm/s; if the brick is not actually moving, comparing the velocity before
	 * and after a second Release proves nothing at all.
	 */
	TestTrue(
		FString::Printf(TEXT("fixture: the released brick should be falling by now, velocity Z is %.3f cm/s"),
			VelocityBefore.Z),
		VelocityBefore.Z < -100.0);

	Falling->Release();

	TestTrue(
		TEXT("a second Release must leave the brick simulating"),
		FallingMesh->IsSimulatingPhysics());

	/*
	 * THE ROW THE IDEMPOTENCE CLAIM ACTUALLY RESTS ON. Nothing ticked between the two
	 * reads, so a Release that derives its state from the body and returns early leaves
	 * this exactly equal; a Release that re-creates the body loses the velocity entirely.
	 */
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

	/*
	 * AND RELEASING ONE BRICK RELEASES ONLY THAT BRICK. Everything else is still kinematic
	 * and still exactly where it was laid — the wall does not come down because one brick
	 * was handed to physics, and Release must not be reaching anything it was not called on.
	 */
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
