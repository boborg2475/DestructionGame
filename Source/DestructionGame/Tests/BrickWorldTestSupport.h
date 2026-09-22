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
#include "Engine/EngineBaseTypes.h"
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
#include "Templates/Function.h"
#include "Templates/SubclassOf.h"
#include "Tests/AutomationCommon.h"
#include "World/BrickActor.h"
#include "World/DestructionStructureSubsystem.h"

#include <type_traits>
#include <utility>

/**
 * The world harness shared by every World.* test, so fixtures cannot drift apart. Its
 * numbers are derived here, never imported from the code under test.
 *
 * Named namespace, not anonymous, because unity builds merge translation units; free
 * functions are `inline` because several translation units include this header.
 *
 * The world is built in code rather than placed in a map: reviewable text, no
 * FunctionalTesting dependency. It rides on Epic's FTestWorldWrapper (Tests/AutomationCommon.h).
 */
namespace BrickWorldTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * The mesh's local size is read, never assumed to be 100 uu, and its pivot is a corner
	 * ((0,0,0) to (100,100,100), logged each run). An actor placed at a box's centre would
	 * sit a half-size out on every axis, so assertions are on world-space bounds.
	 */
	const TCHAR* const CubeMeshPath = TEXT("/Game/LevelPrototyping/Meshes/SM_Cube.SM_Cube");

	/**
	 * The floor's top face, cm. The wall's underside is at Z = 0, leaving 50 cm of air. The
	 * floor does not hold the wall up (intact bricks are kinematic); it gives released
	 * bricks something to land on.
	 */
	constexpr double FloorTopZCm = -50.0;

	/**
	 * Half-extents, cm, the floor must cover. Checked because the corner-pivoted slab once
	 * spanned only +X +Y and left half of every wall unfloored. A 30-brick course reaches
	 * X 663.25; a wall is 10.25 cm deep. The slab reaches 2000.
	 */
	constexpr double FloorMustCoverHalfXCm = 700.0;
	constexpr double FloorMustCoverHalfYCm = 50.0;

	/**
	 * A flush wall, 2 courses of 3 = 7 pieces including two half bats, so a spawner that
	 * gave every brick one size and mass would fail.
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
	 * A wall with a waist: the only shape where deleting one brick orphans anything. A wide
	 * running-bond wall correctly moves nothing (a brick spans two below, and orphans hang on
	 * head joints). Two bricks per course, ragged, so each odd course is a single brick:
	 *
	 *      course 3            [ 5 ]
	 *      course 2         [ 3 ][ 4 ]
	 *      course 1            [ 2 ]        the waist
	 *      course 0         [ 0 ][ 1 ]      grounded
	 *
	 * Ragged, because half bats would add nothing under test here.
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
	 * Mass in kg from cm and g/cm3, derived independently of PieceMassKg. No force
	 * conversion: 1 N = 100 uu applies to forces only (DESIGN.md §3); mass is unconverted.
	 */
	inline double MassKgFromBox(const FPieceBox& Box, double DensityGramsPerCubicCm)
	{
		const double VolumeCubicCm =
			(Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0);

		return VolumeCubicCm * DensityGramsPerCubicCm / 1000.0;
	}

	/**
	 * The wall's two masses at ClayBrick's 1.9 g/cm3: full brick 1432.4375 cm3, half bat
	 * (10.25 long) 682.90625 cm3. A fixture precondition, so an all-one-size wall fails.
	 */
	constexpr double FullBrickMassKg = 2.72163125;
	constexpr double HalfBatMassKg = 1.297521875;

	/** Bounds are exact arithmetic on a box mesh, so this is slack, not signal. */
	constexpr double BoundsToleranceCm = 0.05;

	/** A kinematic brick drifts exactly zero (measured; DESIGN.md §3), so this has ample headroom. */
	constexpr double DriftToleranceCm = 0.1;

	/**
	 * Drop that counts as fallen: above the 1 cm joint settle, below the 7.5 cm course.
	 * Only valid where there is clear air below; a released brick still resting on the
	 * wall correctly moves about a millimetre (see Tests/StructurePushTest.cpp).
	 */
	constexpr double FallenAtLeastCm = 5.0;

	/** Tests tick a fixed simulated time, never poll for settling: a poll turns failures into timeouts. */
	constexpr float PhysicsStepSeconds = 1.0f / 60.0f;

	/**
	 * FTestWorldWrapper plus two things a real game does. A URL, so the game mode sees
	 * `?Scenario=` options (the base uses a default FURL). And a hook between
	 * InitializeActorsForPlay and BeginPlay, where UEngine::LoadMap spawns the player's
	 * controller and pawn. The base body is transcribed because both additions fall inside it.
	 */
	struct FBrickTestWorldWrapper : public FTestWorldWrapper
	{
		/** The URL begin-play runs under. Default-constructed matches the base class. */
		FURL BeginPlayURL;

		/** Runs where UEngine::LoadMap spawns the play actor: after actors init, before begin-play. */
		TFunction<void(UWorld&)> BeforeBeginPlay;

		virtual bool BeginPlayInTestWorld() override
		{
			UWorld* const World = GetTestWorld();

			if (World == nullptr)
			{
				ReportFailure(TEXT("TestWorld does not exist in BeginPlayInTestWorld!"));
				return false;
			}

			if (World->HasBegunPlay())
			{
				ReportFailure(TEXT("TestWorld has already begun play in BeginPlayInTestWorld!"));
				return false;
			}

			CachedFrameCounter = GFrameCounter;

			if (World->GetGameInstance() != nullptr)
			{
				// Required to forward actor BeginPlay (per the base class).
				World->SetGameMode(BeginPlayURL);

				if (World->GetAuthGameMode() == nullptr)
				{
					ReportFailure(TEXT("BeginPlayInTestWorld failed to create GameMode"));
					return false;
				}

				if (World->GetWorldSettings() == nullptr)
				{
					ReportFailure(TEXT("BeginPlayInTestWorld failed to create WorldSettings"));
					return false;
				}
			}

			World->InitializeActorsForPlay(BeginPlayURL);

			if (BeforeBeginPlay)
			{
				BeforeBeginPlay(*World);
			}

			World->BeginPlay();

			if (World->GetGameInstance() != nullptr && World->GetGameState() == nullptr)
			{
				ReportFailure(TEXT("BeginPlayInTestWorld failed to create GameState"));
				return false;
			}

			return true;
		}
	};

	/** A world that ticks, a floor, and the subsystem under test. */
	struct FBrickTestWorld
	{
		FBrickTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		UDestructionStructureSubsystem* Subsystem = nullptr;
		UStaticMesh* Cube = nullptr;

		/**
		 * Build the world, begin play, lay a floor and find the subsystem. The game mode is
		 * named explicitly, defaulting to AGameModeBase: otherwise the ini's
		 * ADestructionGameGameMode would build a 1,220-brick scenario in every World.* test.
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
			 * Log the local size, and require simple collision: without it nothing can
			 * simulate and fall assertions would fail for an unrelated reason.
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

			// Gravity on: integration tests.
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
		 * A kinematic slab under the wall (Movable avoids the SetStaticMesh-on-static check),
		 * placed by its bounds so the pivot does not matter, as BrickSpawnTransform does.
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

			// Checked on bounds: pivot-agnostic, and it catches scale errors too.
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
	 * A player controller with a real ULocalPlayer, so the engine runs SetupInputComponent via
	 * the production SetPlayer path (SetupInputComponent itself is protected).
	 * AddLocalPlayer is used rather than CreateLocalPlayer, whose no-viewport ensure would
	 * fail the running test. Tests/PieceInspectTest.cpp deliberately uses a bare controller,
	 * so code needing a local player must fail closed.
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
	 * Compile-time detectors for members that must never exist. A released brick has moved,
	 * and FPieceBinding::Box is where it was laid, so re-freezing would teleport a fallen
	 * brick back into the wall.
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
