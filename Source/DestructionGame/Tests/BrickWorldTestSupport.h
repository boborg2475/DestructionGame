// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Components/StaticMeshComponent.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/StructureBinding.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "Tests/AutomationCommon.h"
#include "World/BrickActor.h"
#include "World/DestructionStructureSubsystem.h"

#include <type_traits>
#include <utility>

/**
 * THE WORLD HARNESS EVERY World.* TEST SHARES. It lives in a header rather than in one
 * test file because the second world test needed it and copying it would have made two
 * fixtures that drift — two floors at two heights, two settle thresholds, two answers to
 * "how long is a tick". Everything here is a fixture, not production: the numbers are
 * derived in the comments and none of them is imported from the code under test.
 *
 * NAMED NAMESPACE, not anonymous, and named differently from every other one in this
 * directory. An anonymous namespace is private to a TRANSLATION UNIT rather than to a
 * file, and a unity build merges many files into one. See CURRENT_STATE.md; the
 * `using namespace` for this one lives inside each RunTest body for the same reason.
 * Free functions are `inline` because this header is now included by more than one
 * translation unit and a non-unity build would otherwise fail at link.
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
	inline FRunningBondSpec WallSpec()
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
	inline double MassKgFromBox(const FPieceBox& Box, double DensityGramsPerCubicCm)
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
	inline ABrickActor* BrickAt(FAutomationTestBase& Test, FStructureBinding& Binding, int32 Piece)
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
