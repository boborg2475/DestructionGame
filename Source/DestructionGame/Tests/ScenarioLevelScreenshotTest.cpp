// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"
#include "DestructionGameGameMode.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "World/BrickActor.h"
#include "World/DestructionScenarios.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Joins every scenario level and photographs it twice: Held (as laid, solved, nothing released,
 * hold not expired) and Run (after the hold, cuts applied, three seconds to fall). A human judges
 * the images; this asserts the level matches the world-free catalogue.
 *
 * Nothing here builds anything: it opens the row's own .umap and the game mode's BeginPlay does
 * the rest, so the pictures are of the level a player joins. ViewpointFor is called, not copied.
 *
 * The clock is controlled so Held is not a race against render speed: a fixed 1/60 s time step
 * makes a frame count a duration, and world dilation is pinned at its floor (0.0001, BaseGame.ini)
 * from build until the exposure warm-up, so shader compilation costs no hold time. Exposure then
 * converges at full speed inside the 4 s hold (technique from Tests/StaircaseScreenshotTest.cpp).
 *
 * Asserts: selection by map name, counts match the catalogue, a brick per live piece, player at
 * ViewpointFor facing the structure, nothing moved or released while held, hold not expired, then
 * exactly the catalogue's release count after the run, and real PNGs. Displacement is never read as
 * a break (DESIGN.md §4); post-run movement is only printed.
 *
 * Needs a real RHI (NonNullRHI), so the ordinary -nullrhi suite skips it; run it explicitly.
 * From PowerShell (Git Bash mangles the map path):
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.ScenarioLevelScreenshots"
 *     -TestExit="Automation Test Queue Empty"
 *
 * -nullrhi must be absent: with it there is no viewport to screenshot.
 */
namespace ScenarioLevelShotSupport
{
	using namespace DestructionLayout;

	/** `Shot` and not `HighResShot`, and `showui` with it — see Tests/PieceMenuScreenshotTest.cpp. */
	inline FString ScenarioShotCommandFor(const FString& BaseName)
	{
		return FString::Printf(TEXT("Shot showui filename=%s -nosuffix"), *BaseName);
	}

	inline FString ScenarioShotPathFor(const FString& BaseName)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::ScreenShotDir() / BaseName + TEXT(".png"));
	}

	/** Debug overlays off, so nothing is drawn over the image. */
	const TCHAR* const ScenarioShotDisableScreenMessages = TEXT("DisableAllScreenMessages");

	/** File-name suffixes for a row's two frames. Not Before/After: most rows cut nothing. */
	const TCHAR* const ScenarioShotHeldSuffix = TEXT("_Held");
	const TCHAR* const ScenarioShotRunSuffix = TEXT("_Run");

	/** File name for a row's frame, derived from the row name so a new scenario needs no edit here. */
	inline FString ScenarioShotBaseName(const DestructionScenarios::FScenario& Row, const TCHAR* Suffix)
	{
		return TEXT("Scenario_") + Row.Name.ToString().Replace(TEXT("-"), TEXT("_")) + Suffix;
	}

	/**
	 * Folders a scenario map may live in, as Tests/ScenarioMapTest.cpp sweeps. FunctionalTests/ is
	 * deliberately absent: it is never cooked, so a scenario there would be missing from a build.
	 */
	const TCHAR* const ScenarioShotMapFolders[] =
	{
		TEXT("/Game/Maps/Scenarios/"),
		TEXT("/Game/Maps/"),
	};

	/** The long package name for a row's map, or empty with every candidate recorded in OutTried. */
	inline FString ScenarioShotFindMapPackage(const TCHAR* MapName, FString& OutTried)
	{
		OutTried.Reset();

		FString Found;

		for (const TCHAR* const Folder : ScenarioShotMapFolders)
		{
			const FString Candidate = FString(Folder) + MapName;

			if (!OutTried.IsEmpty())
			{
				OutTried += TEXT(", ");
			}

			OutTried += FString::Printf(TEXT("'%s.umap'"), *Candidate);

			if (Found.IsEmpty() && FPackageName::DoesPackageExist(Candidate))
			{
				Found = Candidate;
			}
		}

		return Found;
	}

	/**
	 * Timings in frames of 1/60 s world time. Frozen frames cost the hold nothing; the 1 s of
	 * exposure frames is the only warm-up cost against the 4 s hold. Write frames exist because
	 * screenshots are written at end of draw. Run frames (3.33 s) clear the hold with a second of
	 * slack. Fall frames are 180, not 300, because corbel-f-100 releases ~3,000 bodies at once;
	 * 3 s is still nine times what a brick needs to fall a visible 50 cm.
	 */
	constexpr double ScenarioShotFixedDeltaSeconds = 1.0 / 60.0;

	constexpr int32 ScenarioShotFrozenFrames = 30;
	constexpr int32 ScenarioShotSlateFrames = 3;
	constexpr int32 ScenarioShotExposureFrames = 60;
	constexpr int32 ScenarioShotWriteFrames = 5;
	constexpr int32 ScenarioShotRunFrames = 200;
	constexpr int32 ScenarioShotFallFrames = 180;

	/** Frames the map may take to load and build before the wait fails. */
	constexpr int32 ScenarioShotMapLoadFrameBudget = 3000;

	/** Minimum PNG size and dimensions; see Tests/PieceMenuScreenshotTest.cpp. */
	constexpr int64 ScenarioShotMinimumBytes = 32 * 1024;
	constexpr int32 ScenarioShotMinimumWidth = 640;
	constexpr int32 ScenarioShotMinimumHeight = 480;

	/** 1 mm: below the 0.14 cm a brick falls in one 60 Hz frame. */
	constexpr double ScenarioShotStillnessToleranceCm = 0.1;

	/** How near the pawn must be to the catalogue's viewpoint. */
	constexpr double ScenarioShotViewpointToleranceCm = 0.01;

	/** The aspect the game mode frames for. */
	constexpr double ScenarioShotAspectHeightOverWidth = 1080.0 / 1920.0;

	/** Print a double at full precision. */
	inline FString ScenarioShotBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/**
	 * What the world-free catalogue says this row is and does. Every world-side count is checked
	 * against it; it is independent of the map, game mode, subsystem, actors and timer.
	 */
	struct FScenarioShotOracle
	{
		bool bBuilt = false;

		int32 Pieces = 0;
		int32 Joints = 0;

		/** Pieces the row's own cut removes. */
		int32 CutPieces = 0;

		/** Passes in which `SolveAndBreak` broke a joint, after the cut. */
		int32 BreakPasses = 0;

		/** Pieces the settled solve stops holding up; exactly what `ApplyResults` releases. */
		int32 WouldRelease = 0;

		/** Bounding box as laid; the viewpoint is framed on it. */
		FBox BoundsCm = FBox(ForceInit);

		/** The bed joint under the arm's lowest outermost brick (corbel rows only). */
		int32 RootJoint = INDEX_NONE;
		int32 RootSeatPiece = INDEX_NONE;
		int32 RootArmPiece = INDEX_NONE;

		/** Recorded so the root joint's identity is asserted: grounded seat, ungrounded arm, bed role. */
		bool bRootSeatGrounded = false;
		bool bRootArmGrounded = true;

		EJointRole RootRoleFromArm = EJointRole::None;

		/** The root joint's utilisation as laid (solved, not settled). */
		double RootUtilisation = 0.0;
	};

	/**
	 * The root joint of a laid corbel: the bed joint under the arm's lowest outermost brick, where
	 * a corbel on an immovable base fails in tension.
	 *
	 * Found by groundedness and height, not by looking for a course that steps out: the corbel base
	 * is laid in an alternating bond, so a step search lands inside the base and reads 0. Derived
	 * from the structure because FScenario does not expose the grid spec. Fails closed to
	 * INDEX_NONE for a flat wall (only one grounded course).
	 */
	inline int32 ScenarioShotFindRootJoint(
		const FStructure& Structure,
		const TArray<FPieceBox>& Boxes,
		int32& OutSeatPiece,
		int32& OutArmPiece)
	{
		OutSeatPiece = INDEX_NONE;
		OutArmPiece = INDEX_NONE;

		/*
		 * Top grounded course is the base's top; the lowest ungrounded course is the arm's first.
		 * Courses are grouped by exact centre Z, since every producer lays a course at one height.
		 */
		double TopGroundedZCm = -TNumericLimits<double>::Max();
		double FirstArmZCm = TNumericLimits<double>::Max();

		int32 GroundedCourses = 0;

		TArray<double> GroundedZCm;

		for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
		{
			const double ZCm = Boxes[Piece].CentreCm.Z;

			if (!Structure.GetPiece(Piece).bIsGrounded)
			{
				FirstArmZCm = FMath::Min(FirstArmZCm, ZCm);

				continue;
			}

			TopGroundedZCm = FMath::Max(TopGroundedZCm, ZCm);

			if (!GroundedZCm.ContainsByPredicate(
					[ZCm](double Known) { return FMath::IsNearlyEqual(Known, ZCm, 1.0e-6); }))
			{
				GroundedZCm.Add(ZCm);
				++GroundedCourses;
			}
		}

		// One grounded course is a plain wall, not a corbel on a base.
		if (GroundedCourses < 2 || FirstArmZCm >= TNumericLimits<double>::Max())
		{
			return INDEX_NONE;
		}

		double SeatXCm = -TNumericLimits<double>::Max();
		double ArmXCm = -TNumericLimits<double>::Max();

		for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
		{
			const double ZCm = Boxes[Piece].CentreCm.Z;
			const double XCm = Boxes[Piece].CentreCm.X;

			if (FMath::IsNearlyEqual(ZCm, TopGroundedZCm, 1.0e-6) && XCm > SeatXCm)
			{
				SeatXCm = XCm;
				OutSeatPiece = Piece;
			}

			if (FMath::IsNearlyEqual(ZCm, FirstArmZCm, 1.0e-6) && XCm > ArmXCm)
			{
				ArmXCm = XCm;
				OutArmPiece = Piece;
			}
		}

		if (OutSeatPiece == INDEX_NONE || OutArmPiece == INDEX_NONE)
		{
			return INDEX_NONE;
		}

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			const FConnection& Connection = Structure.GetConnection(Joint);

			if ((Connection.PieceA == OutSeatPiece && Connection.PieceB == OutArmPiece)
				|| (Connection.PieceA == OutArmPiece && Connection.PieceB == OutSeatPiece))
			{
				return Joint;
			}
		}

		return INDEX_NONE;
	}

	/**
	 * Lay this row world-free, read it as laid, apply its cut, settle, and count what is unheld.
	 * Uses ApplyResults' release predicate written out, not called, so a bug in the world wire
	 * cannot agree with itself. The whole cut goes before one settle, as RunPieceActions does.
	 */
	inline FScenarioShotOracle ScenarioShotReadWorldFree(const DestructionScenarios::FScenario& Row)
	{
		FScenarioShotOracle Oracle;

		FBrickLayout Laid;
		TArray<int32> CutPieces;

		if (!DestructionScenarios::Build(Row, Laid, CutPieces))
		{
			return Oracle;
		}

		Oracle.bBuilt = true;
		Oracle.Pieces = Laid.Structure.NumPieces();
		Oracle.Joints = Laid.Structure.NumConnections();
		Oracle.CutPieces = CutPieces.Num();

		for (const FPieceBox& Box : Laid.Boxes)
		{
			Oracle.BoundsCm += FBox(Box.CentreCm - Box.ExtentCm, Box.CentreCm + Box.ExtentCm);
		}

		// Read as laid first (the held state); SolveLoads is non-destructive.
		Laid.Structure.SolveLoads();

		Oracle.RootJoint = ScenarioShotFindRootJoint(
			Laid.Structure, Laid.Boxes, Oracle.RootSeatPiece, Oracle.RootArmPiece);

		if (Oracle.RootJoint != INDEX_NONE)
		{
			Oracle.RootUtilisation = Laid.Structure.GetConnectionUtilisation(Oracle.RootJoint);

			Oracle.bRootSeatGrounded = Laid.Structure.GetPiece(Oracle.RootSeatPiece).bIsGrounded;
			Oracle.bRootArmGrounded = Laid.Structure.GetPiece(Oracle.RootArmPiece).bIsGrounded;

			Oracle.RootRoleFromArm =
				Laid.Structure.GetJointRole(Oracle.RootJoint, Oracle.RootArmPiece);
		}

		for (const int32 Piece : CutPieces)
		{
			Laid.Structure.RemovePiece(Piece);
		}

		Oracle.BreakPasses = Laid.Structure.SolveAndBreak();

		for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
		{
			if (Laid.Structure.IsPieceRemoved(Piece) || !Laid.Structure.HasSupportAnswer(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Laid.Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				++Oracle.WouldRelease;
			}
		}

		return Oracle;
	}

	/**
	 * One row's state, carried between latent commands (which carry only their parameters). Reset
	 * at the top of every row.
	 */
	struct FScenarioShotRecord
	{
		bool bJoined = false;

		/**
		 * The world up before travel. Open is serviced at end of tick, so the next frame still has
		 * the old world (for row 0, the startup map running a structure); the join waits for a
		 * different UWorld.
		 */
		TWeakObjectPtr<UWorld> WorldBeforeTravel;

		int32 StructureId = INDEX_NONE;

		FScenarioShotOracle Oracle;

		/** Each brick's laid position, for measuring stillness. */
		TArray<FVector> LaidAtCm;

		/** World time the level finished building, i.e. when the hold started. */
		double BuiltAtSeconds = 0.0;

		/** Solve count while held, to prove the level later ran. */
		int32 SolvesWhileHeld = 0;

		double HeldAtSeconds = 0.0;
		double RanAtSeconds = 0.0;

		double WorstHeldMovementCm = 0.0;
		double WorstRunMovementCm = 0.0;

		void Reset()
		{
			*this = FScenarioShotRecord();
		}
	};

	inline FScenarioShotRecord& ScenarioShotRecord()
	{
		static FScenarioShotRecord Record;
		return Record;
	}

	/** The row this index names, or null. */
	inline const DestructionScenarios::FScenario* ScenarioShotRow(int32 RowIndex)
	{
		return DestructionScenarios::Catalogue().IsValidIndex(RowIndex)
			? &DestructionScenarios::Catalogue()[RowIndex]
			: nullptr;
	}

	/** The game mode of whatever game world is up, or null. */
	inline ADestructionGameGameMode* ScenarioShotGameMode(UWorld* World)
	{
		return World != nullptr ? World->GetAuthGameMode<ADestructionGameGameMode>() : nullptr;
	}

	/** The cut text the level would show the player now, read from GetScenarioLabel, not reproduced. */
	inline FString ScenarioShotGameModeCutText(UWorld* World)
	{
		const ADestructionGameGameMode* const GameMode = World != nullptr
			? World->GetAuthGameMode<ADestructionGameGameMode>()
			: nullptr;

		return GameMode != nullptr
			? GameMode->GetScenarioLabel().CutText
			: FString(TEXT("<no game mode to ask>"));
	}

	/** The binding for the structure the game mode built, or null with the reason reported. */
	inline FStructureBinding* ScenarioShotBinding(
		FAutomationTestBase& Test, UWorld* World, int32 StructureId)
	{
		UDestructionStructureSubsystem* const Subsystem =
			World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;

		if (Subsystem == nullptr)
		{
			Test.AddError(TEXT("the world should own a UDestructionStructureSubsystem"));
			return nullptr;
		}

		FStructureBinding* const Binding = Subsystem->Find(StructureId);

		if (Binding == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("the game mode's structure id %d names no binding: there is nothing standing in ")
				TEXT("the world to photograph"),
				StructureId));
		}

		return Binding;
	}

	/** How many live pieces have been released to physics. */
	inline int32 ScenarioShotReleasedCount(const FStructureBinding& Binding)
	{
		int32 Released = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (!Binding.IsPieceRemoved(Piece) && Binding.IsReleased(Piece))
			{
				++Released;
			}
		}

		return Released;
	}

	/** How many pieces still have a live brick standing for them. */
	inline int32 ScenarioShotLiveBrickCount(const FStructureBinding& Binding)
	{
		int32 Bricks = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (IsValid(Cast<ABrickActor>(Binding.GetActor(Piece))))
			{
				++Bricks;
			}
		}

		return Bricks;
	}

	/** How many live pieces the last solve has no answer for. */
	inline int32 ScenarioShotUnansweredCount(const FStructureBinding& Binding)
	{
		int32 Unanswered = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (!Binding.IsPieceRemoved(Piece)
				&& !Binding.GetStructure().HasSupportAnswer(Piece))
			{
				++Unanswered;
			}
		}

		return Unanswered;
	}

	/** The worst distance any surviving brick has travelled since the level laid it. */
	inline double ScenarioShotWorstMovementCm(
		const FStructureBinding& Binding, const TArray<FVector>& LaidAtCm)
	{
		double Worst = 0.0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			const AActor* const Brick = Cast<AActor>(Binding.GetActor(Piece));

			if (Brick == nullptr || !LaidAtCm.IsValidIndex(Piece))
			{
				continue;
			}

			Worst = FMath::Max(Worst, FVector::Dist(Brick->GetActorLocation(), LaidAtCm[Piece]));
		}

		return Worst;
	}

	/** Set world time dilation; returns the value reached, or -1 with an error. */
	inline float ScenarioShotSetDilation(FAutomationTestBase& Test, UWorld* World, float Dilation)
	{
		AWorldSettings* const Settings = World != nullptr ? World->GetWorldSettings() : nullptr;

		if (Settings == nullptr)
		{
			Test.AddError(TEXT("the world has no AWorldSettings, so its clock cannot be controlled"));
			return -1.0f;
		}

		return Settings->SetTimeDilation(Dilation);
	}

	/** Request a screenshot via the viewport client; UEngine::Exec has no SHOT handler. */
	inline void ScenarioShotRequest(FAutomationTestBase& Test, const FString& Command)
	{
		UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

		if (Viewport == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("there is no game viewport to run '%s' against, so no frame can be written"),
				*Command));

			return;
		}

		Viewport->Exec(nullptr, *Command, *GLog);
	}
}

/** Pin a fixed time step so a frame count is a duration (restored at the end), and check for a viewport. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FScenarioShotOpenSessionCommand, FAutomationTestBase*, Test);

bool FScenarioShotOpenSessionCommand::Update()
{
	using namespace ScenarioLevelShotSupport;

	UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

	// Assert the viewport now; without one a missing file looks like a render failure.
	Test->TestNotNull(
		TEXT("there must be a game viewport for Slate to screenshot: -nullrhi must be absent"),
		Viewport);

	FApp::SetFixedDeltaTime(ScenarioShotFixedDeltaSeconds);
	FApp::SetUseFixedTimeStep(true);

	Test->AddInfo(FString::Printf(
		TEXT("the engine clock is pinned at %s s per frame for the whole of this test, so every ")
		TEXT("frame count below is a duration; it is restored at the end"),
		*ScenarioShotBits(FApp::GetFixedDeltaTime())));

	Test->TestTrue(
		TEXT("the harness must be able to pin the engine clock, or the four-second hold is a race ")
		TEXT("against however fast this machine draws"),
		FApp::UseFixedTimeStep());

	return true;
}

/** Restore the real-time clock. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FScenarioShotCloseSessionCommand, FAutomationTestBase*, Test);

bool FScenarioShotCloseSessionCommand::Update()
{
	FApp::SetUseFixedTimeStep(false);

	Test->AddInfo(TEXT("the engine clock is back on real time"));

	return true;
}

/** Travel to this row's own map. */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FScenarioShotOpenMapCommand, FAutomationTestBase*, Test, int32, RowIndex);

bool FScenarioShotOpenMapCommand::Update()
{
	using namespace ScenarioLevelShotSupport;

	ScenarioShotRecord().Reset();

	const DestructionScenarios::FScenario* const Row = ScenarioShotRow(RowIndex);

	if (Row == nullptr)
	{
		Test->AddError(FString::Printf(TEXT("the catalogue has no row %d"), RowIndex));
		return true;
	}

	FString Tried;

	const FString Package = ScenarioShotFindMapPackage(Row->MapName, Tried);

	if (Package.IsEmpty())
	{
		Test->AddError(FString::Printf(
			TEXT("row '%s' names the map '%s' and no .umap of that name is on disk, so there is no ")
			TEXT("level to join. Looked for: %s"),
			*Row->Name.ToString(), Row->MapName, *Tried));

		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world: a map must be named on the command line"));
		return true;
	}

	Test->AddInfo(FString::Printf(
		TEXT("=== ROW %d '%s' -> opening %s (the world is currently %s) ==="),
		RowIndex, *Row->Name.ToString(), *Package, *World->GetMapName()));

	ScenarioShotRecord().WorldBeforeTravel = World;

	// Always travel, even to the map already up: its hold may have expired long ago.
	GEngine->Exec(World, *FString::Printf(TEXT("Open %s"), *Package));

	return true;
}

/**
 * Wait for the level to finish building, freeze the world at once, and record what the level did
 * (row, structure, bricks, camera) and where every brick stands.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_THREE_PARAMETER(
	FScenarioShotJoinCommand, FAutomationTestBase*, Test, int32, RowIndex, int32, FramesWaited);

bool FScenarioShotJoinCommand::Update()
{
	using namespace ScenarioLevelShotSupport;

	const DestructionScenarios::FScenario* const Row = ScenarioShotRow(RowIndex);

	if (Row == nullptr)
	{
		return true;
	}

	FScenarioShotRecord& Record = ScenarioShotRecord();

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	ADestructionGameGameMode* const GameMode = ScenarioShotGameMode(World);

	// Must be a different world from before travel, or row 0 would accept the startup map.
	const bool bIsThisRow = World != nullptr
		&& World != Record.WorldBeforeTravel.Get()
		&& GameMode != nullptr
		&& GameMode->GetSelectedScenarioRow() == RowIndex
		&& GameMode->GetBuiltStructureId() != INDEX_NONE;

	if (!bIsThisRow)
	{
		if (++FramesWaited < ScenarioShotMapLoadFrameBudget)
		{
			return false;
		}

		Test->AddError(FString::Printf(
			TEXT("row '%s' never came up: after %d frames the world is %s, its game mode is %s and ")
			TEXT("the row it selected is %d"),
			*Row->Name.ToString(), FramesWaited,
			*GetNameSafe(World), *GetNameSafe(World != nullptr ? World->GetAuthGameMode() : nullptr),
			GameMode != nullptr ? GameMode->GetSelectedScenarioRow() : INDEX_NONE));

		return true;
	}

	// Freeze now. Clamped to MinGlobalTimeDilation (0.0001), so the warm-up costs microseconds.
	const float Frozen = ScenarioShotSetDilation(*Test, World, 0.0f);

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s': the world must actually freeze while the renderer warms up, or the hold is a ")
			TEXT("race; its dilation is %g"),
			*Row->Name.ToString(), Frozen),
		Frozen > 0.0f && Frozen <= 0.001f);

	Record.BuiltAtSeconds = World->GetTimeSeconds();

	// A stale world on the right row would pass every count below yet be past its hold.
	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' must have been JOINED rather than found already running: %s s of world time ")
			TEXT("had passed when it finished building, and its hold is %s s"),
			*Row->Name.ToString(), *ScenarioShotBits(Record.BuiltAtSeconds),
			*ScenarioShotBits(Row->HoldSeconds)),
		Record.BuiltAtSeconds < 1.0);

	// Row selection.

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' must be selected BY ITS OWN MAP NAME — that is the door a player uses, and a ")
			TEXT("map that opens onto somebody else's wall is worse than one that does not open. It ")
			TEXT("selected row %d and reports selection kind %d."),
			*Row->Name.ToString(), GameMode->GetSelectedScenarioRow(),
			static_cast<int32>(GameMode->GetScenarioSelection())),
		GameMode->GetScenarioSelection() == DestructionScenarios::EScenarioSelection::ByMapName);

	// Structure against the world-free catalogue.

	Record.Oracle = ScenarioShotReadWorldFree(*Row);

	if (!Record.Oracle.bBuilt)
	{
		Test->AddError(FString::Printf(
			TEXT("fixture: '%s' must build world-free, or there is nothing to hold the level against"),
			*Row->Name.ToString()));

		return true;
	}

	Record.StructureId = GameMode->GetBuiltStructureId();

	FStructureBinding* const Binding = ScenarioShotBinding(*Test, World, Record.StructureId);

	if (Binding == nullptr)
	{
		return true;
	}

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' must stand up the SAME structure the world-free catalogue lays: %d pieces and ")
			TEXT("%d joints against the catalogue's %d and %d. A lookalike would settle to a different ")
			TEXT("count and nothing about it would look wrong in the photograph."),
			*Row->Name.ToString(), Binding->NumPieces(),
			Binding->GetStructure().NumConnections(), Record.Oracle.Pieces, Record.Oracle.Joints),
		Binding->NumPieces() == Record.Oracle.Pieces
			&& Binding->GetStructure().NumConnections() == Record.Oracle.Joints);

	Record.LaidAtCm.SetNumZeroed(Binding->NumPieces());

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		if (const AActor* const Brick = Cast<AActor>(Binding->GetActor(Piece)))
		{
			Record.LaidAtCm[Piece] = Brick->GetActorLocation();
		}
	}

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s': a brick must stand for every one of its %d live pieces, or the photograph is ")
			TEXT("of a different structure from the one that was solved; %d bricks stand"),
			*Row->Name.ToString(), Binding->GetStructure().NumLivePieces(),
			ScenarioShotLiveBrickCount(*Binding)),
		ScenarioShotLiveBrickCount(*Binding) == Binding->GetStructure().NumLivePieces());

	Record.SolvesWhileHeld = Binding->GetStructure().NumSolves();

	// Player placement.

	const DestructionScenarios::FViewpoint Viewpoint = DestructionScenarios::ViewpointFor(
		Record.Oracle.BoundsCm, ScenarioShotAspectHeightOverWidth, Row->Framing);

	APlayerController* const Controller = World->GetFirstPlayerController();
	APawn* const Pawn = Controller != nullptr ? Controller->GetPawn() : nullptr;

	if (Pawn == nullptr)
	{
		Test->AddError(FString::Printf(
			TEXT("'%s': the level has no pawn for the player to look through, so there is no camera ")
			TEXT("and nothing this harness photographs is what a player would see"),
			*Row->Name.ToString()));

		return true;
	}

	const FVector CameraCm = Pawn->GetActorLocation();
	const FRotator CameraRotation = Controller->GetControlRotation();

	const FVector CentreCm = Record.Oracle.BoundsCm.GetCenter();
	const FVector SizeCm = Record.Oracle.BoundsCm.GetSize();

	Test->AddInfo(FString::Printf(
		TEXT("'%s': the structure spans %.1f x %.1f x %.1f cm centred on (%.1f, %.1f, %.1f); the ")
		TEXT("level put the player at (%.2f, %.2f, %.2f) facing (pitch %.1f, yaw %.1f), and ")
		TEXT("ViewpointFor says (%.2f, %.2f, %.2f) facing (pitch %.1f, yaw %.1f)"),
		*Row->Name.ToString(), SizeCm.X, SizeCm.Y, SizeCm.Z, CentreCm.X, CentreCm.Y, CentreCm.Z,
		CameraCm.X, CameraCm.Y, CameraCm.Z, CameraRotation.Pitch, CameraRotation.Yaw,
		Viewpoint.LocationCm.X, Viewpoint.LocationCm.Y, Viewpoint.LocationCm.Z,
		Viewpoint.Rotation.Pitch, Viewpoint.Rotation.Yaw));

	// ViewpointFor is called, not re-derived, so a wrong standoff cannot agree with itself.
	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' must stand the player where DestructionScenarios::ViewpointFor frames the ")
			TEXT("whole structure — the player is %s cm from it and may be at most %g. A level that ")
			TEXT("did not place the camera photographs whatever the map's own PlayerStart happened ")
			TEXT("to be pointing at."),
			*Row->Name.ToString(),
			*ScenarioShotBits(FVector::Dist(CameraCm, Viewpoint.LocationCm)),
			ScenarioShotViewpointToleranceCm),
		FVector::Dist(CameraCm, Viewpoint.LocationCm) < ScenarioShotViewpointToleranceCm);

	// Facing the structure, and not inside it.
	const FVector ToStructure = CentreCm - CameraCm;

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s': the structure must be IN FRONT of the camera — the camera looks along ")
			TEXT("(%.3f, %.3f, %.3f) and the structure lies along (%.3f, %.3f, %.3f)"),
			*Row->Name.ToString(),
			CameraRotation.Vector().X, CameraRotation.Vector().Y, CameraRotation.Vector().Z,
			ToStructure.X, ToStructure.Y, ToStructure.Z),
		FVector::DotProduct(CameraRotation.Vector(), ToStructure) > 0.0);

	Test->TestFalse(
		*FString::Printf(
			TEXT("'%s': the camera must not be INSIDE the structure, or the frame is a picture of the ")
			TEXT("inside of a brick"),
			*Row->Name.ToString()),
		Record.Oracle.BoundsCm.IsInsideOrOn(CameraCm));

	Record.bJoined = true;

	// Numbers printed beside the pictures.

	Test->AddInfo(FString::Printf(
		TEXT("ROW '%s' — %s"), *Row->Name.ToString(), Row->Title));

	Test->AddInfo(FString::Printf(
		TEXT("ROW '%s' — WHAT TO WATCH FOR: %s"), *Row->Name.ToString(), Row->Expectation));

	Test->AddInfo(FString::Printf(
		TEXT("ROW '%s' — %d pieces, %d joints, %d cut, hold %s s. The world-free catalogue settles ")
		TEXT("this row in %d breaking pass(es) and stops holding up %d piece(s)."),
		*Row->Name.ToString(), Record.Oracle.Pieces, Record.Oracle.Joints, Record.Oracle.CutPieces,
		*ScenarioShotBits(Row->HoldSeconds), Record.Oracle.BreakPasses, Record.Oracle.WouldRelease));

	if (Record.Oracle.RootJoint != INDEX_NONE)
	{
		Test->AddInfo(FString::Printf(
			TEXT("ROW '%s' — THE ROOT JOINT: joint %d, the bed joint under the arm's lowest ")
			TEXT("outermost brick (piece %d standing on piece %d), reads %s AS LAID. Over 1.0 is a ")
			TEXT("corbel the model says cannot hold itself up."),
			*Row->Name.ToString(), Record.Oracle.RootJoint, Record.Oracle.RootArmPiece,
			Record.Oracle.RootSeatPiece, *ScenarioShotBits(Record.Oracle.RootUtilisation)));

		// Prove it is the root joint, not a joint inside the base (an earlier finder read 0 there).
		Test->TestTrue(
			*FString::Printf(
				TEXT("'%s': the root joint must stand ON the immovable base and carry something that ")
				TEXT("is NOT part of it — seat piece %d grounded is %d, arm piece %d grounded is %d. ")
				TEXT("A joint between two base courses reads a confident number about nothing."),
				*Row->Name.ToString(), Record.Oracle.RootSeatPiece,
				Record.Oracle.bRootSeatGrounded ? 1 : 0, Record.Oracle.RootArmPiece,
				Record.Oracle.bRootArmGrounded ? 1 : 0),
			Record.Oracle.bRootSeatGrounded && !Record.Oracle.bRootArmGrounded);

		Test->TestEqual(
			*FString::Printf(
				TEXT("'%s': the root must be a BED joint seen from the arm above it, or the reading ")
				TEXT("taken there is about a different mechanism"),
				*Row->Name.ToString()),
			Record.Oracle.RootRoleFromArm, EJointRole::BedBeneath);

		Test->TestTrue(
			*FString::Printf(
				TEXT("'%s': the root joint must carry something finite and non-zero as laid — an arm ")
				TEXT("hangs off it — and it reads %s"),
				*Row->Name.ToString(), *ScenarioShotBits(Record.Oracle.RootUtilisation)),
			Record.Oracle.RootUtilisation > 0.0 && FMath::IsFinite(Record.Oracle.RootUtilisation));
	}
	else
	{
		Test->AddInfo(FString::Printf(
			TEXT("ROW '%s' — no course of this structure steps out past the one below it, so it has ")
			TEXT("no root joint to read: it is a flush wall rather than a corbel."),
			*Row->Name.ToString()));
	}

	return true;
}

/** Unfreeze the world so exposure and temporal history converge. */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FScenarioShotThawCommand, FAutomationTestBase*, Test, int32, RowIndex);

bool FScenarioShotThawCommand::Update()
{
	using namespace ScenarioLevelShotSupport;

	if (!ScenarioShotRecord().bJoined)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	const float Dilation = ScenarioShotSetDilation(*Test, World, 1.0f);

	Test->TestTrue(
		*FString::Printf(
			TEXT("the world must run at full speed for the %d exposure frames, or auto-exposure never ")
			TEXT("converges and the held frame is a picture of the dark; its dilation is %g"),
			ScenarioShotExposureFrames, Dilation),
		FMath::IsNearlyEqual(Dilation, 1.0f));

	return true;
}

/**
 * Photograph the structure as laid, before the hold expires. Stillness is measured, and
 * IsReleased is asserted beside it because a released brick jammed in place would also be still
 * (DESIGN.md §4).
 */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FScenarioShotHeldCommand, FAutomationTestBase*, Test, int32, RowIndex);

bool FScenarioShotHeldCommand::Update()
{
	using namespace ScenarioLevelShotSupport;

	const DestructionScenarios::FScenario* const Row = ScenarioShotRow(RowIndex);
	FScenarioShotRecord& Record = ScenarioShotRecord();

	if (Row == nullptr || !Record.bJoined)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	FStructureBinding* const Binding = ScenarioShotBinding(*Test, World, Record.StructureId);

	if (Binding == nullptr)
	{
		return true;
	}

	Record.HeldAtSeconds = World->GetTimeSeconds();

	const double IntoTheHoldSeconds = Record.HeldAtSeconds - Record.BuiltAtSeconds;

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s': THE HOLD MUST NOT HAVE EXPIRED when the held frame was written — %s s of its ")
			TEXT("%s s hold had gone. Past it the level has already run, and the frame would be a ")
			TEXT("picture of the answer captioned as the question."),
			*Row->Name.ToString(), *ScenarioShotBits(IntoTheHoldSeconds),
			*ScenarioShotBits(Row->HoldSeconds)),
		IntoTheHoldSeconds < Row->HoldSeconds);

	Record.WorstHeldMovementCm = ScenarioShotWorstMovementCm(*Binding, Record.LaidAtCm);

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s': 'held' must mean the structure was standing exactly as laid — the ")
			TEXT("worst-moved brick had travelled %s cm and may travel at most %g"),
			*Row->Name.ToString(), *ScenarioShotBits(Record.WorstHeldMovementCm),
			ScenarioShotStillnessToleranceCm),
		Record.WorstHeldMovementCm < ScenarioShotStillnessToleranceCm);

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s': NOT ONE of its %d pieces may have been handed to physics while it is held; ")
			TEXT("%d had been. A player joining this level has to see the structure whole."),
			*Row->Name.ToString(), Binding->NumPieces(), ScenarioShotReleasedCount(*Binding)),
		ScenarioShotReleasedCount(*Binding) == 0);

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' must still be WHOLE while it is held: %d of %d pieces live and %d bricks ")
			TEXT("standing. Holding is about what has not been released, but a level that removed ")
			TEXT("pieces instead would satisfy that and show the same empty air."),
			*Row->Name.ToString(), Binding->GetStructure().NumLivePieces(), Binding->NumPieces(),
			ScenarioShotLiveBrickCount(*Binding)),
		Binding->GetStructure().NumLivePieces() == Binding->NumPieces()
			&& ScenarioShotLiveBrickCount(*Binding) == Binding->NumPieces());

	// Held means solved but not settled; an unsolved structure would also release nothing.
	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' must already be SOLVED while it is held — every one of its %d live pieces must ")
			TEXT("have a support answer, because an absent answer reads as Falling; %d have none"),
			*Row->Name.ToString(), Binding->GetStructure().NumLivePieces(),
			ScenarioShotUnansweredCount(*Binding)),
		ScenarioShotUnansweredCount(*Binding) == 0);

	Test->AddInfo(FString::Printf(
		TEXT("ROW '%s' — HELD at %s s of world time, %s s into its %s s hold: %d of %d pieces live, ")
		TEXT("%d bricks standing, 0 released, worst movement %s cm. The level says: \"%s\""),
		*Row->Name.ToString(), *ScenarioShotBits(Record.HeldAtSeconds),
		*ScenarioShotBits(IntoTheHoldSeconds), *ScenarioShotBits(Row->HoldSeconds),
		Binding->GetStructure().NumLivePieces(), Binding->NumPieces(),
		ScenarioShotLiveBrickCount(*Binding), *ScenarioShotBits(Record.WorstHeldMovementCm),
		*ScenarioShotGameModeCutText(World)));

	ScenarioShotRequest(*Test, ScenarioShotCommandFor(ScenarioShotBaseName(*Row, ScenarioShotHeldSuffix)));

	return true;
}

/**
 * Photograph the level after it has run. The exact release count against the catalogue is
 * asserted; movement is only printed (DESIGN.md §4).
 */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FScenarioShotRunCommand, FAutomationTestBase*, Test, int32, RowIndex);

bool FScenarioShotRunCommand::Update()
{
	using namespace ScenarioLevelShotSupport;

	const DestructionScenarios::FScenario* const Row = ScenarioShotRow(RowIndex);
	FScenarioShotRecord& Record = ScenarioShotRecord();

	if (Row == nullptr || !Record.bJoined)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	FStructureBinding* const Binding = ScenarioShotBinding(*Test, World, Record.StructureId);

	if (Binding == nullptr)
	{
		return true;
	}

	Record.RanAtSeconds = World->GetTimeSeconds();

	const double SinceBuiltSeconds = Record.RanAtSeconds - Record.BuiltAtSeconds;

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s': the level must have been given past its %s s hold — %s s of world time had ")
			TEXT("passed since it was built. Every claim below is about what the level DID, and a ")
			TEXT("world whose clock stopped would satisfy none of them honestly."),
			*Row->Name.ToString(), *ScenarioShotBits(Row->HoldSeconds),
			*ScenarioShotBits(SinceBuiltSeconds)),
		SinceBuiltSeconds >= Row->HoldSeconds);

	// Settling is a solve, so a higher solve count proves the level ran even if nothing fell.
	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' MUST HAVE RUN once its hold expired: settling is a solve, so the graph must ")
			TEXT("have solved more than the %d times it had while held; it has solved %d"),
			*Row->Name.ToString(), Record.SolvesWhileHeld, Binding->GetStructure().NumSolves()),
		Binding->GetStructure().NumSolves() > Record.SolvesWhileHeld);

	const int32 Released = ScenarioShotReleasedCount(*Binding);
	const int32 LivePieces = Binding->GetStructure().NumLivePieces();

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' MUST RELEASE EXACTLY THE %d PIECE(S) THE WORLD-FREE CATALOGUE SAYS THE SAME ROW ")
			TEXT("SETTLES TO; the level released %d of its %d. A level and a headless reading that ")
			TEXT("disagree here are about two different structures."),
			*Row->Name.ToString(), Record.Oracle.WouldRelease, Released, Binding->NumPieces()),
		Released == Record.Oracle.WouldRelease);

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' names %d cut(s), so exactly that many pieces may have been REMOVED: %d of %d ")
			TEXT("are live"),
			*Row->Name.ToString(), Record.Oracle.CutPieces, LivePieces, Binding->NumPieces()),
		LivePieces == Binding->NumPieces() - Record.Oracle.CutPieces);

	Record.WorstRunMovementCm = ScenarioShotWorstMovementCm(*Binding, Record.LaidAtCm);

	// Printed, not asserted: helps a human read the frames.
	Test->AddInfo(FString::Printf(
		TEXT("ROW '%s' — RUN at %s s of world time, %d frames after its hold expired: %d of %d ")
		TEXT("pieces live, %d bricks standing, %d released against the catalogue's %d, worst ")
		TEXT("surviving brick moved %.3f cm (it had moved %s cm when the held frame was written). ")
		TEXT("The level says: \"%s\""),
		*Row->Name.ToString(), *ScenarioShotBits(Record.RanAtSeconds), ScenarioShotFallFrames,
		LivePieces, Binding->NumPieces(), ScenarioShotLiveBrickCount(*Binding), Released,
		Record.Oracle.WouldRelease, Record.WorstRunMovementCm,
		*ScenarioShotBits(Record.WorstHeldMovementCm),
		*ScenarioShotGameModeCutText(World)));

	ScenarioShotRequest(*Test, ScenarioShotCommandFor(ScenarioShotBaseName(*Row, ScenarioShotRunSuffix)));

	return true;
}

/** Every frame file exists and is a real PNG. All were deleted before the run. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FScenarioShotCheckFilesCommand, FAutomationTestBase*, Test);

bool FScenarioShotCheckFilesCommand::Update()
{
	using namespace ScenarioLevelShotSupport;

	for (const DestructionScenarios::FScenario& Row : DestructionScenarios::Catalogue())
	{
		// Build sandboxes are not photographed.
		if (Row.bBuildSandbox)
		{
			continue;
		}

		for (const TCHAR* const Suffix : { ScenarioShotHeldSuffix, ScenarioShotRunSuffix })
		{
			const FString BaseName = ScenarioShotBaseName(Row, Suffix);
			const FString Path = ScenarioShotPathFor(BaseName);

			const int64 SizeBytes = IFileManager::Get().FileSize(*Path);

			if (SizeBytes < 0)
			{
				Test->AddError(FString::Printf(
					TEXT("no screenshot was written to %s: the shot request never reached a draw, or ")
					TEXT("the file went somewhere else"),
					*Path));

				continue;
			}

			TArray<uint8> Bytes;

			if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 24)
			{
				Test->AddError(FString::Printf(
					TEXT("the screenshot at %s could not be read back, or is too short to carry a PNG ")
					TEXT("header (%d bytes)"),
					*Path, Bytes.Num()));

				continue;
			}

			/*
			 * PNG signature and IHDR read by hand: 8 signature bytes, 4-byte length, "IHDR", then
			 * big-endian width and height. Avoids depending on a decoder.
			 */
			static const uint8 PngSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

			const bool bIsPng = FMemory::Memcmp(Bytes.GetData(), PngSignature, 8) == 0
				&& FMemory::Memcmp(Bytes.GetData() + 12, "IHDR", 4) == 0;

			const auto BigEndian = [&Bytes](int32 At)
			{
				return (static_cast<int32>(Bytes[At]) << 24)
					| (static_cast<int32>(Bytes[At + 1]) << 16)
					| (static_cast<int32>(Bytes[At + 2]) << 8)
					| static_cast<int32>(Bytes[At + 3]);
			};

			const int32 Width = bIsPng ? BigEndian(16) : 0;
			const int32 Height = bIsPng ? BigEndian(20) : 0;

			Test->AddInfo(FString::Printf(
				TEXT("IMAGE %s.png is %lld bytes, %d x %d pixels, at %s"),
				*BaseName, SizeBytes, Width, Height, *Path));

			Test->TestTrue(
				*FString::Printf(
					TEXT("%s.png must be a real frame of a lit scene — a flat colour is under 10 kB — ")
					TEXT("so it must be at least %lld bytes; it is %lld"),
					*BaseName, ScenarioShotMinimumBytes, SizeBytes),
				SizeBytes >= ScenarioShotMinimumBytes);

			Test->TestTrue(
				*FString::Printf(
					TEXT("%s.png must begin with the PNG signature and an IHDR chunk, and declare real ")
					TEXT("dimensions; it is %d x %d"),
					*BaseName, Width, Height),
				bIsPng && Width >= ScenarioShotMinimumWidth && Height >= ScenarioShotMinimumHeight);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioLevelScreenshotTest,
	"DestructionGame.Visual.ScenarioLevelScreenshots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::NonNullRHI
		| EAutomationTestFlags::ProductFilter)

bool FScenarioLevelScreenshotTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioLevelShotSupport;

	ScenarioShotRecord().Reset();

	const TArray<DestructionScenarios::FScenario>& Rows = DestructionScenarios::Catalogue();

	// Floor on the row count, so an empty catalogue fails instead of passing silently.
	TestTrue(
		*FString::Printf(
			TEXT("fixture: the catalogue must carry at least the 9 rows the scenario slices left, or ")
			TEXT("this photographs nothing; it carries %d"),
			Rows.Num()),
		Rows.Num() >= 9);

	// Delete old files first, so "the file exists" means this run rendered it.
	for (const DestructionScenarios::FScenario& Row : Rows)
	{
		for (const TCHAR* const Suffix : { ScenarioShotHeldSuffix, ScenarioShotRunSuffix })
		{
			const FString Path = ScenarioShotPathFor(ScenarioShotBaseName(Row, Suffix));

			if (IFileManager::Get().FileExists(*Path))
			{
				IFileManager::Get().Delete(
					*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
			}

			if (IFileManager::Get().FileExists(*Path))
			{
				AddError(FString::Printf(
					TEXT("fixture: %s could not be deleted, so its existence afterwards would prove ")
					TEXT("nothing"),
					*Path));

				return true;
			}
		}
	}

	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(ScenarioShotDisableScreenMessages));
	ADD_LATENT_AUTOMATION_COMMAND(FScenarioShotOpenSessionCommand(this));

	/*
	 * Per row: travel, catch the build and freeze, drain shaders and settle Slate while frozen, thaw
	 * for exposure, shoot Held, run past the hold and let it fall, shoot Run. The shader drain must
	 * be inside the freeze: it waits on unbounded real time.
	 */
	for (int32 RowIndex = 0; RowIndex < Rows.Num(); ++RowIndex)
	{
		// A build sandbox lays nothing; skipped here and in the file check.
		if (Rows[RowIndex].bBuildSandbox)
		{
			AddInfo(FString::Printf(
				TEXT("SKIPPED '%s': a build sandbox lays nothing, so there is no structure to "
					"photograph held or run"),
				*Rows[RowIndex].Name.ToString()));

			continue;
		}

		ADD_LATENT_AUTOMATION_COMMAND(FScenarioShotOpenMapCommand(this, RowIndex));
		ADD_LATENT_AUTOMATION_COMMAND(FScenarioShotJoinCommand(this, RowIndex, 0));

		ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ScenarioShotSlateFrames));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ScenarioShotFrozenFrames));

		ADD_LATENT_AUTOMATION_COMMAND(FScenarioShotThawCommand(this, RowIndex));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ScenarioShotExposureFrames));

		ADD_LATENT_AUTOMATION_COMMAND(FScenarioShotHeldCommand(this, RowIndex));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ScenarioShotWriteFrames));

		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ScenarioShotRunFrames));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ScenarioShotFallFrames));

		ADD_LATENT_AUTOMATION_COMMAND(FScenarioShotRunCommand(this, RowIndex));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ScenarioShotWriteFrames));
	}

	ADD_LATENT_AUTOMATION_COMMAND(FScenarioShotCloseSessionCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FScenarioShotCheckFilesCommand(this));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
