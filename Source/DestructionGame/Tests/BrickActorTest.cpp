// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/StructureBinding.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"
#include "Tests/AutomationCommon.h"
#include "World/BrickActor.h"
#include "World/DestructionStructureSubsystem.h"

#include <type_traits>
#include <utility>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named differently from every other one in this
 * directory. An anonymous namespace is private to a TRANSLATION UNIT rather than to a
 * file, and a unity build merges many files into one. See CURRENT_STATE.md; the
 * `using namespace` for this one lives inside each RunTest body for the same reason.
 *
 * THE WORLD IS BUILT IN CODE, NOT PLACED IN A MAP. A functional test is a map edit as
 * well as a code change — discovery goes through an asset tag written onto the .umap on
 * editor save — so every such test is a binary diff nobody can review plus an editor
 * session to create it. A code-built world stays reviewable text, needs no
 * FunctionalTesting dependency, and runs under the existing DestructionGame filter with
 * no change to the documented command. Watchable demo maps are a separate, deliberate
 * artefact rather than the mechanism every test rides on.
 *
 * FTestWorldWrapper is Epic's own code-built harness (Engine/Public/Tests/
 * AutomationCommon.h, and NOT inside its WITH_AUTOMATION_TESTS guard): CreateWorld +
 * InitializeActorsForPlay + BeginPlay + a manual World->Tick, which is exactly the
 * 50-80 lines this file would otherwise carry. Using it rather than transcribing it
 * keeps the setup one thing rather than two.
 */
namespace BrickWorldTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * THE LOCAL SIZE OF THE MESH IS READ, NEVER ASSUMED TO BE 100.
	 *
	 * SM_Cube happens to be authored at 100 uu today, so "scale = dimension / 100" is
	 * right by accident. The day a real brick mesh lands, that constant is silently
	 * wrong and the wall merely looks a bit off — which is why production derives the
	 * scale from GetBoundingBox().GetSize() and why this file asserts on world-space
	 * BOUNDS rather than on a scale factor.
	 *
	 * AND ITS PIVOT IS AT A CORNER, MEASURED RATHER THAN ASSUMED: min (0, 0, 0) to max
	 * (100, 100, 100), reported below on every run. So an actor placed at a box's centre
	 * puts the brick a half-size out on ALL THREE axes — (10.75, 5.125, 3.25) cm for a
	 * full brick — and GetActorLocation would agree with the layout while the wall sat
	 * visibly off its own grid. This is exactly why the assertion below is on bounds.
	 */
	const TCHAR* const CubeMeshPath = TEXT("/Game/LevelPrototyping/Meshes/SM_Cube.SM_Cube");

	/**
	 * Where the floor's top face sits, in cm.
	 *
	 * The wall's bottom course has its underside at Z = 0, so a released brick has 50 cm
	 * of clear air beneath it. THE FLOOR IS NOT WHAT HOLDS THE WALL UP — every intact
	 * brick is kinematic and would hang in the air without it — it is there so this is a
	 * real contact scene with something to land on, and so a released brick stops rather
	 * than falling forever.
	 */
	constexpr double FloorTopZCm = -50.0;

	/**
	 * A FLUSH wall, which is the mixed-size case: 2 courses of 3 gives 3 + 4 = 7 pieces,
	 * two of them half bats. A ragged wall would be seven copies of one box and one mass,
	 * and a spawner that handed every brick the same size and weight would pass.
	 */
	FRunningBondSpec WallSpec()
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = 2;
		Spec.BricksPerCourse = 3;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	constexpr int32 WallPieceCount = 7;

	/**
	 * MASS FROM GEOMETRY, DERIVED HERE RATHER THAN IMPORTED.
	 *
	 * Density is g/cm3 and dimensions are cm, so cm3 x g/cm3 is grams and grams / 1000 is
	 * kilograms. NO FORCE CONVERSION BELONGS HERE: DESIGN.md §3's 1 N = 100 uu is a
	 * property of forces, and mass goes into Unreal unconverted. Calling
	 * DestructionLayout::PieceMassKg instead would make this agree with production
	 * however wrong production was; the multiplication order is deliberately the other
	 * one (volume first, density last) for the same reason, and the comparison is made
	 * with a tolerance so the one-ulp difference that order costs does not matter.
	 */
	double MassKgFromBox(const FPieceBox& Box, double DensityGramsPerCubicCm)
	{
		const double VolumeCubicCm =
			(Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0);

		return VolumeCubicCm * DensityGramsPerCubicCm / 1000.0;
	}

	/**
	 * The two masses the wall must contain, traced to a published density.
	 *
	 * 1.9 g/cm3 is ClayBrick's, cited in Core/Profiles/MaterialProfiles.cpp. A full brick
	 * is 21.5 x 10.25 x 6.5 = 1432.4375 cm3, a half bat (21.5 - 1) / 2 = 10.25 long is
	 * 682.90625 cm3. These are asserted as a FIXTURE PRECONDITION so that a wall that
	 * came out all one size fails here rather than making the per-piece mass check free.
	 */
	constexpr double FullBrickMassKg = 2.72163125;
	constexpr double HalfBatMassKg = 1.297521875;

	/** Bounds are exact arithmetic on a box mesh, so this is slack, not signal. */
	constexpr double BoundsToleranceCm = 0.05;

	/**
	 * A kinematic brick drifts EXACTLY ZERO — the two-brick spike measured 0.000000 cm,
	 * against 0.62-0.70 cm for the Chaos-constraint alternative (DESIGN.md §3). So this
	 * threshold has enormous headroom and is not a tuning knob.
	 */
	constexpr double DriftToleranceCm = 0.1;

	/**
	 * How far a released brick must have fallen to count as having fallen.
	 *
	 * DERIVED, NOT PICKED. The mortar joint is 1 cm, so a brick merely settling into its
	 * own gap moves at most 1 cm; the course pitch is 7.5 cm, so a brick that dropped a
	 * whole course has moved more than that. 5 cm is five times the settle and comfortably
	 * inside one course. The actual drop here is about 50 cm (the clear air above the
	 * floor), or the free-fall 490 cm if it were to miss the floor entirely, so the
	 * assertion is nowhere near its margin either way.
	 */
	constexpr double FallenAtLeastCm = 5.0;

	/**
	 * A FIXED NUMBER OF SIMULATED SECONDS, NEVER A "HAS IT SETTLED" POLL. Settling is
	 * non-deterministic, and a poll turns a real failure into a timeout, which reports far
	 * worse than an assertion does.
	 */
	constexpr float PhysicsStepSeconds = 1.0f / 60.0f;

	/** A world that ticks, a floor, and the subsystem under test. */
	struct FBrickTestWorld
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		UDestructionStructureSubsystem* Subsystem = nullptr;
		UStaticMesh* Cube = nullptr;

		bool Begin(FAutomationTestBase& Test)
		{
			Cube = LoadObject<UStaticMesh>(nullptr, CubeMeshPath);

			if (Cube == nullptr)
			{
				Test.AddError(FString::Printf(TEXT("fixture: could not load %s"), CubeMeshPath));
				return false;
			}

			/*
			 * VERIFIED RATHER THAN ASSUMED, both halves. The local size is reported so the
			 * "SM_Cube is 100 uu" claim in DESIGN.md is checkable from the log rather than
			 * from memory; and a mesh with no SIMPLE collision primitive cannot simulate at
			 * all, so a released brick would sit there and the fall assertion below would
			 * fail for a reason that has nothing to do with Release.
			 */
			const FVector LocalSizeCm = Cube->GetBoundingBox().GetSize();

			Test.AddInfo(FString::Printf(
				TEXT("fixture: %s local bounds are %g x %g x %g uu, min (%g, %g, %g), max (%g, %g, %g)"),
				CubeMeshPath, LocalSizeCm.X, LocalSizeCm.Y, LocalSizeCm.Z,
				Cube->GetBoundingBox().Min.X, Cube->GetBoundingBox().Min.Y, Cube->GetBoundingBox().Min.Z,
				Cube->GetBoundingBox().Max.X, Cube->GetBoundingBox().Max.Y, Cube->GetBoundingBox().Max.Z));

			const int32 SimpleCollisionElements =
				Cube->GetBodySetup() != nullptr ? Cube->GetBodySetup()->AggGeom.GetElementCount() : 0;

			Test.TestTrue(
				FString::Printf(
					TEXT("fixture: %s must carry simple collision or nothing built from it can simulate; it has %d primitives"),
					CubeMeshPath, SimpleCollisionElements),
				SimpleCollisionElements > 0);

			if (!Wrapper.CreateTestWorld(EWorldType::Game))
			{
				Wrapper.ForwardErrorMessages(&Test);
				return false;
			}

			World = Wrapper.GetTestWorld();

			if (!Wrapper.BeginPlayInTestWorld())
			{
				Wrapper.ForwardErrorMessages(&Test);
				return false;
			}

			/* Gravity on: this is an integration test, and the whole point is falling. */
			Test.TestTrue(
				FString::Printf(TEXT("fixture: the test world should have real gravity, got %g"),
					World->GetGravityZ()),
				World->GetGravityZ() < -900.0f);

			SpawnFloor(Test);

			Subsystem = World->GetSubsystem<UDestructionStructureSubsystem>();

			Test.TestNotNull(
				TEXT("fixture: the world should own a UDestructionStructureSubsystem"),
				Subsystem);

			return Subsystem != nullptr;
		}

		/**
		 * A slab under the wall, placed by its own bounds rather than by an assumed pivot.
		 *
		 * Movable-but-not-simulating, i.e. kinematic, which is what every intact brick is
		 * too — and it sidesteps the "illegal call to SetStaticMesh on a static component"
		 * check that setting a mesh on an already-registered Static component trips.
		 */
		void SpawnFloor(FAutomationTestBase& Test)
		{
			const FVector ScaleCm(40.0, 40.0, 1.0);
			const double TopOffsetCm = Cube->GetBoundingBox().Max.Z * ScaleCm.Z;

			const FTransform Transform(
				FRotator::ZeroRotator,
				FVector(0.0, 0.0, FloorTopZCm - TopOffsetCm),
				ScaleCm);

			AStaticMeshActor* Floor = World->SpawnActorDeferred<AStaticMeshActor>(
				AStaticMeshActor::StaticClass(), Transform);

			if (Floor == nullptr)
			{
				Test.AddError(TEXT("fixture: the floor failed to spawn"));
				return;
			}

			Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
			Floor->GetStaticMeshComponent()->SetStaticMesh(Cube);
			Floor->FinishSpawning(Transform);
		}

		void TickSeconds(double Seconds)
		{
			const int32 Steps = FMath::RoundToInt(Seconds / static_cast<double>(PhysicsStepSeconds));

			for (int32 Step = 0; Step < Steps; ++Step)
			{
				Wrapper.TickTestWorld(PhysicsStepSeconds);
			}
		}

		void End()
		{
			Wrapper.DestroyTestWorld(false);
			World = nullptr;
			Subsystem = nullptr;
		}
	};

	/** The actor for a piece handle, or null with the reason reported. */
	ABrickActor* BrickAt(FAutomationTestBase& Test, FStructureBinding& Binding, int32 Piece)
	{
		ABrickActor* Brick = Cast<ABrickActor>(Binding.GetActor(Piece));

		Test.TestNotNull(
			*FString::Printf(TEXT("piece %d should be bound to an ABrickActor, got %s"),
				Piece, *GetNameSafe(Binding.GetActor(Piece))),
			Brick);

		return Brick;
	}

	/**
	 * DETECTORS FOR MEMBERS THAT MUST NEVER EXIST, checked at COMPILE time because that is
	 * the only place they can be checked: the failure mode is somebody adding a Freeze and
	 * calling it, and no runtime assertion can catch code that was never written.
	 *
	 * WHY RE-FREEZING IS UNSAYABLE RATHER THAN MERELY UNWISE. A released brick has been
	 * handed to Chaos and has MOVED. FStructurePiece has no position, and FPieceBinding::Box
	 * is deliberately where the brick was LAID — so anything that recomputed a transform
	 * from the box would teleport a fallen brick back into the wall it fell out of, and the
	 * structure would then claim to be held up by a brick lying on the floor. Same trick as
	 * FStructureBinding::GetStructure having no non-const overload.
	 */
	template <typename T, typename = void>
	struct TBrickHasFreeze : std::false_type {};

	template <typename T>
	struct TBrickHasFreeze<T, std::void_t<decltype(std::declval<T&>().Freeze())>> : std::true_type {};

	template <typename T, typename = void>
	struct TBrickHasUnrelease : std::false_type {};

	template <typename T>
	struct TBrickHasUnrelease<T, std::void_t<decltype(std::declval<T&>().Unrelease())>> : std::true_type {};

	template <typename T, typename = void>
	struct TBrickHasSetSimulating : std::false_type {};

	template <typename T>
	struct TBrickHasSetSimulating<T, std::void_t<decltype(std::declval<T&>().SetSimulating(true))>>
		: std::true_type {};
}

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
