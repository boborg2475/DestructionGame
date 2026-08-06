// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Components/StaticMeshComponent.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/StructureBinding.h"
#include "DestructionGamePlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "Templates/SubclassOf.h"
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
	 * HOW MUCH GROUND EVERY WALL IN THIS SUITE NEEDS UNDER IT, as a half-extent in cm.
	 *
	 * This exists because the floor was WRONG and nothing noticed. SpawnFloor used to place
	 * SM_Cube at (0, 0, ...) and scale it by 40, which reads as a 40 m slab straddling the
	 * origin — except that SM_Cube's pivot is a CORNER (0,0,0)..(100,100,100), so the slab
	 * actually spanned X 0..4000 and Y 0..4000. A RunningBond wall spans Y -5.125..+5.125,
	 * so HALF of every wall in every World.* test had no floor beneath it, and the leftmost
	 * brick of every wall (X from -10.75) hung off the edge as well. It affected nothing
	 * only by luck — released orphans landed on the kinematic bricks below them — and two
	 * test files worked around it rather than fixing it.
	 *
	 * The numbers are what the walls actually need. Along X, a 30-brick course reaches from
	 * -BrickSizeCm.X / 2 = -10.75 to 29 x 22.5 + 10.75 = 663.25, so 700 covers the widest
	 * wall this suite lays with room to spare. Along Y a wall is one brick deep, 10.25 cm
	 * centred on zero, so 50 is nearly ten times what is needed. Both are half-extents, and
	 * both are far inside the 2000 the slab now reaches, so this is a floor on the coverage
	 * rather than a fit to it.
	 */
	constexpr double FloorMustCoverHalfXCm = 700.0;
	constexpr double FloorMustCoverHalfYCm = 50.0;

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
	 * A WALL WITH A WAIST, which is the only shape in which deleting ONE brick genuinely
	 * orphans anything — and the reason it lives here is that four test files now need one.
	 *
	 * A running-bond wall is redundant on purpose: a full brick spans two below it, so pulling
	 * one support out leaves the other one carrying; and DESIGN.md §3's tier rule lets a piece
	 * with no bed joint left beneath it fall back onto its HEAD joints, so in a wide wall the
	 * orphan merely hangs off its neighbour. Both are right physics, and both make a wall in
	 * which deleting a brick correctly moves nothing at all — the RIGHT answer, and a useless
	 * fixture for anything about bricks coming down or about a re-solve being visible from
	 * outside.
	 *
	 * Two bricks per course is the narrowest bond RunningBond will lay, and ragged means the
	 * odd course is one brick short, so every odd course is a single brick that everything
	 * above it reaches the ground through. Three courses give
	 *
	 *      course 2         [ 3 ][ 4 ]      head joint 3-4 between them
	 *      course 1            [ 2 ]        THE WAIST
	 *      course 0         [ 0 ][ 1 ]      grounded
	 *
	 * and four put one more piece, resting on 3 and 4, on top of that:
	 *
	 *      course 3            [ 5 ]
	 *      course 2         [ 3 ][ 4 ]
	 *      course 1            [ 2 ]        THE WAIST
	 *      course 0         [ 0 ][ 1 ]      grounded
	 *
	 * RAGGED RATHER THAN FLUSH, because a flush end adds half bats whose only job is to be a
	 * second piece size — that is Tests/BrickActorTest.cpp's concern, and here it would add
	 * pieces and head joints without changing anything under test. WallSpec above is the flush
	 * one, and the two are deliberately different fixtures rather than one with a flag.
	 *
	 * PARAMETERISED ON HEIGHT ONLY, because height is the only thing the four callers disagree
	 * about: a test that ticks physics wants the piece above the pair, and one that does not
	 * would only pay spawn time for it.
	 */
	inline FRunningBondSpec NarrowWaistWallSpec(int32 CoursesHigh)
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = CoursesHigh;
		Spec.BricksPerCourse = 2;
		Spec.End = EWallEnd::Ragged;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

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
	 *
	 * AND IT ONLY MEANS ANYTHING WHERE THERE IS CLEAR AIR TO FALL THROUGH. Both derivations
	 * above assume the released brick has nothing beneath it — 50 cm of it over the floor, or
	 * the 8.5 cm the narrow-waist wall leaves when its waist is pulled. A released brick that
	 * is still resting on standing wall is a perfectly correct outcome and moves a millimetre,
	 * so applying this to one is a claim about geometry that is simply false: the foot of a
	 * staircase collapse keeps 10.25 of its 21.5 cm on the course below and measures 1.310 cm.
	 * A collapse whose released set has no clear air under it needs a different measure —
	 * Tests/StructurePushTest.cpp's settle test works one through.
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

		/**
		 * Build the world, begin play, put a floor under it and find the subsystem.
		 *
		 * THE GAME MODE IS NAMED RATHER THAN INHERITED, and the default is Epic's plain one.
		 * FTestWorldWrapper::BeginPlayInTestWorld calls UWorld::SetGameMode, which resolves
		 * WorldSettings->DefaultGameMode first and only then falls back to the project's
		 * GlobalDefaultGameMode — and that ini setting is ADestructionGameGameMode, so every
		 * world test in this suite has silently been running the real game mode. Once the
		 * game mode builds a scenario on begin-play, that would mean every World.* test
		 * spawning a 1,220-brick wall it never asked for.
		 *
		 * So a test that is not about the game mode gets AGameModeBase and no scenario, and
		 * a test that IS about it passes ADestructionGameGameMode::StaticClass() explicitly
		 * — which also makes that test say what it is exercising rather than depending on an
		 * ini file three directories away.
		 */
		bool Begin(
			FAutomationTestBase& Test,
			TSubclassOf<AGameModeBase> GameModeClass = AGameModeBase::StaticClass())
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

			if (AWorldSettings* Settings = World->GetWorldSettings())
			{
				Settings->DefaultGameMode = GameModeClass;
			}
			else
			{
				Test.AddError(TEXT("fixture: the test world has no WorldSettings to name a game mode on"));
				return false;
			}

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
		 *
		 * THE PLACEMENT IS THE SAME ARITHMETIC PRODUCTION ALREADY USES, and it had to
		 * become so: the earlier version put the actor's ORIGIN at (0, 0, ...), which for a
		 * corner-pivoted mesh puts the slab's whole 40 m footprint into the +X +Y quadrant.
		 * Subtracting the scaled local bounds centre puts the slab's BOUNDS where they were
		 * meant to be, whatever the pivot happens to be — the same fix, and the same reason,
		 * as BrickSpawnTransform in World/DestructionStructureSubsystem.cpp.
		 *
		 * The Z was right by accident before and is right on purpose now: the target centre
		 * is half the slab's own thickness below the top face, so the top face lands on
		 * FloorTopZCm either way.
		 */
		void SpawnFloor(FAutomationTestBase& Test)
		{
			const FVector ScaleCm(40.0, 40.0, 1.0);

			const FBox LocalBounds = Cube->GetBoundingBox();
			const FVector SlabSizeCm = LocalBounds.GetSize() * ScaleCm;

			const FVector TargetCentreCm(0.0, 0.0, FloorTopZCm - SlabSizeCm.Z * 0.5);

			const FTransform Transform(
				FRotator::ZeroRotator,
				TargetCentreCm - ScaleCm * LocalBounds.GetCenter(),
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

			/*
			 * AND IT IS CHECKED, ON BOUNDS, because the defect this replaces was invisible
			 * for exactly as long as nobody looked at where the slab actually was. Bounds
			 * rather than location is the same choice Tests/BrickActorTest.cpp makes and for
			 * the same reason: it is pivot-agnostic and catches a scale error in the same
			 * comparison.
			 */
			const FBox FloorBoundsCm = Floor->GetComponentsBoundingBox();

			const bool bCoversTheWalls =
				FloorBoundsCm.Min.X <= -FloorMustCoverHalfXCm
				&& FloorBoundsCm.Max.X >= FloorMustCoverHalfXCm
				&& FloorBoundsCm.Min.Y <= -FloorMustCoverHalfYCm
				&& FloorBoundsCm.Max.Y >= FloorMustCoverHalfYCm
				&& FMath::IsNearlyEqual(FloorBoundsCm.Max.Z, FloorTopZCm, BoundsToleranceCm);

			Test.TestTrue(
				*FString::Printf(
					TEXT("fixture: the floor must span at least X +/-%g, Y +/-%g with its top at Z %g; it spans X %g..%g, Y %g..%g, top Z %g"),
					FloorMustCoverHalfXCm, FloorMustCoverHalfYCm, FloorTopZCm,
					FloorBoundsCm.Min.X, FloorBoundsCm.Max.X,
					FloorBoundsCm.Min.Y, FloorBoundsCm.Max.Y,
					FloorBoundsCm.Max.Z),
				bCoversTheWalls);
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

	/**
	 * A player controller with a REAL ULocalPlayer behind it, which is what makes the engine
	 * run SetupInputComponent — and therefore the only way to see what the controller bound.
	 *
	 * WHY NOT JUST CALL SetupInputComponent. It is protected, and reaching it by a test-side
	 * subclass would need a UCLASS, which UHT only processes in headers. APlayerController::
	 * SetPlayer is the production route: it marks the controller local and calls
	 * InitInputSystem, which spawns the UEnhancedInputComponent named by DefaultInput.ini and
	 * then calls SetupInputComponent. Driving the real path is also what keeps this fixture
	 * honest — a binding that only appears when a test pokes it is not a binding.
	 *
	 * WHY NOT UGameInstance::CreateLocalPlayer. It contains `ensure(IsDedicatedServerInstance())`
	 * on the no-viewport path, and a fired ensure is counted by the automation framework as a
	 * failure of whatever test is running. AddLocalPlayer is the half below that ensure and is
	 * viewport-safe: ULocalPlayer::PlayerAdded takes a null UGameViewportClient and still
	 * initialises the subsystem collection, which is where the Enhanced Input local player
	 * subsystem comes from.
	 *
	 * A CONTROLLER WITHOUT ONE IS A DIFFERENT AND ALSO VALID FIXTURE — Tests/PieceInspectTest.cpp
	 * spawns a bare one deliberately, so anything that needs a local player must fail closed
	 * rather than crash.
	 */
	inline ADestructionGamePlayerController* SpawnControllerWithLocalPlayer(
		FAutomationTestBase& Test,
		UWorld* World)
	{
		if (World == nullptr || GEngine == nullptr || GEngine->LocalPlayerClass == nullptr)
		{
			Test.AddError(TEXT("fixture: no world, or the engine names no ULocalPlayer class"));
			return nullptr;
		}

		UGameInstance* const GameInstance = World->GetGameInstance();

		if (GameInstance == nullptr)
		{
			Test.AddError(TEXT("fixture: the test world has no UGameInstance to add a local player to"));
			return nullptr;
		}

		ADestructionGamePlayerController* const Controller =
			World->SpawnActor<ADestructionGamePlayerController>();

		if (Controller == nullptr)
		{
			Test.AddError(TEXT("fixture: the game's player controller failed to spawn"));
			return nullptr;
		}

		ULocalPlayer* const LocalPlayer =
			NewObject<ULocalPlayer>(GEngine, GEngine->LocalPlayerClass);

		GameInstance->AddLocalPlayer(
			LocalPlayer, FGenericPlatformMisc::GetPlatformUserForUserIndex(0));

		Controller->SetPlayer(LocalPlayer);

		Test.TestTrue(
			TEXT("fixture: SetPlayer should have marked the controller a local player controller"),
			Controller->IsLocalPlayerController());

		return Controller;
	}

	/** The Enhanced Input subsystem for a controller's local player, or null. */
	inline UEnhancedInputLocalPlayerSubsystem* InputSubsystemOf(APlayerController* Controller)
	{
		return Controller != nullptr
			? ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Controller->GetLocalPlayer())
			: nullptr;
	}

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
