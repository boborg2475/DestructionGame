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
 * Every scenario level, joined and photographed twice: one frame as the
 * player finds it, one after the level has run.
 *
 * This is the validation step, not new behaviour. The user asked to "join
 * the level, see the wall in front of the player start and observe what
 * happens to it" — everything under that sentence is already green
 * headlessly (World.Scenarios.CorbelRows, World.Scenario.
 * GameModeHoldsTheStructureAsLaid, Content.ScenarioMapsExist), but none of
 * that produces a picture, and this project's rule is that a change isn't
 * validated until there are screenshots to look at. So this hands a human
 * eighteen frames and the numbers to read them by; the judging is theirs.
 *
 * The difference from Tests/CorbelScreenshotTest.cpp, which is the whole
 * point: that harness hand-rolls its own spawn loop and camera placement
 * because there was no public door onto a hand-laid structure when it was
 * written. There is now, so nothing here builds anything — this opens the
 * row's own .umap, and ADestructionGameGameMode::BeginPlay does all of it:
 * selects the row from the map name, lays it through
 * DestructionScenarios::Build, stands it up through
 * UDestructionStructureSubsystem::BuildLayout, places the player per
 * DestructionScenarios::ViewpointFor, and arms the row's own HoldSeconds. The
 * pictures are of the level the player joins, not a test-assembled
 * lookalike, so a duplicated camera or spawn recipe can't creep back in. The
 * only production reach here is a call to ViewpointFor, not a copy of it —
 * re-deriving the arithmetic would agree with a wrong answer.
 *
 * The two frames, and what makes "Held" honest:
 *
 *   - Held: the structure exactly as laid — solved, nothing released, every
 *     brick still kinematic, hold not yet expired. For seven of the nine
 *     rows this frame didn't exist before the hold slice: a corbel is
 *     condemned by its own geometry, so before the hold the level settled on
 *     the frame it was built, over before the player's first frame drew.
 *   - Run: after the hold expires, whatever cuts the row names have been
 *     applied, the structure has settled, and whatever was going to fall has
 *     had three seconds to fall.
 *
 * The clock is controlled, or the Held frame is a race: the hold is four
 * world seconds and a 1080p frame of 3,015 bricks isn't free, so on a slow
 * machine the level could run while the renderer was still settling exposure
 * and "held" would picture the collapse. Two harness-side controls close
 * that, neither touching production: FApp::SetUseFixedTimeStep pins every
 * frame to 1/60 s of world time, making a frame count a duration; and the
 * world's time dilation is pinned at its floor from the instant the level
 * finishes building until the exposure warm-up starts, so the unbounded
 * shader-compilation wait after a map load costs no world time
 * (Tests/StaircaseScreenshotTest.cpp established the technique; 0.0001 is
 * BaseGame.ini's own MinGlobalTimeDilation). The warm-up TSR's temporal
 * history and auto-exposure need is then spent at full speed — a frozen
 * world has a delta of ~1.6 microseconds and eye adaptation would never
 * converge — inside the hold, which the fixed time step makes affordable.
 * How much of the hold had gone when the shutter opened is measured and
 * asserted, not argued.
 *
 * What it asserts, and what it deliberately does not: a harness that only
 * takes a picture is green whatever is in it, so each row asserts the map
 * selected its own row by name, that it built, that piece/joint counts match
 * the world-free catalogue, that a brick actor stands for every live piece,
 * that the player was placed per ViewpointFor and is looking at the
 * structure, that nothing had moved or been released/removed when the held
 * frame was written, that the hold hadn't expired — then that the level ran,
 * released exactly the pieces the catalogue says it settles to, and both
 * files landed as real PNGs of real size. It asserts nothing about what the
 * images look like; judging that is the human's job.
 *
 * It never reads a displacement as evidence a joint broke — DESIGN.md §4
 * forbids it, since two pieces can sever and rest exactly where they were —
 * so the release count against the catalogue's own answer is the mechanism,
 * and post-run movement is printed so a reader can tell a picture of a
 * collapse from one of a structure that was condemned but didn't visibly
 * move. The converse has no such hole: an unmoved brick hasn't been
 * released, which is why the held frame's stillness is measured against a
 * 0.1 cm tolerance and asserted.
 *
 * It needs a ticking world and a real RHI, hence
 * EAutomationTestFlags::NonNullRHI. Without that flag the ordinary -nullrhi
 * suite would run this, find no viewport, write no file, and go green — how
 * Visual.StaircaseScreenshot rotted red for five slices. With it, the
 * ordinary suite never mentions this test, so it must be run explicitly.
 * From PowerShell (Git Bash mangles the map path):
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.ScenarioLevelScreenshots"
 *     -TestExit="Automation Test Queue Empty"
 *
 * -nullrhi must be absent: FApp::CanEverRender() is false with it and
 * UGameEngine::Init only builds a window and a viewport otherwise, so there
 * would be nothing to screenshot even if the filter let the test through.
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

	/** Debug overlays off, so nothing is burned over the thing a human is being asked to judge. */
	const TCHAR* const ScenarioShotDisableScreenMessages = TEXT("DisableAllScreenMessages");

	/**
	 * The two frames of a row, as file-name suffixes. Held and Run rather
	 * than Before and After, because the pair is not a cut — seven of the
	 * nine rows remove nothing, so "before what?" has no answer for them;
	 * what every row has is a hold and the moment it expires.
	 */
	const TCHAR* const ScenarioShotHeldSuffix = TEXT("_Held");
	const TCHAR* const ScenarioShotRunSuffix = TEXT("_Run");

	/**
	 * The file name for a row's frame: the row's own name with its dashes
	 * made legal. Derived from the row, never listed — a table of eighteen
	 * file names is one to forget to edit when a scenario is added, the
	 * failure Content.ScenarioMapsExist exists for one layer up.
	 */
	inline FString ScenarioShotBaseName(const DestructionScenarios::FScenario& Row, const TCHAR* Suffix)
	{
		return TEXT("Scenario_") + Row.Name.ToString().Replace(TEXT("-"), TEXT("_")) + Suffix;
	}

	/**
	 * Where a scenario's map may live, the same pair Tests/ScenarioMapTest.cpp
	 * sweeps. /Game/Maps/Scenarios/ holds the scenario levels — one .umap per
	 * catalogue row, each a byte copy of the sandbox map with the game mode
	 * doing all the work. /Game/Maps/ is where Lvl_Sandbox already is: the
	 * gameplay map the game ships, and moving it would change what pressing
	 * Play opens.
	 *
	 * Content/Maps/FunctionalTests/ is deliberately absent — CLAUDE.md
	 * reserves it for functional-test maps and excludes it from the cook, so
	 * a scenario found there would work in the editor and be missing from a build.
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
	 * The timings, in frames — a frame is 1/60 s of world time because the
	 * clock is pinned.
	 *
	 * ScenarioShotFrozenFrames and the shader drain either side of it are
	 * spent with the world's time dilation at its floor, so they cost the
	 * hold nothing however long the map took to warm up.
	 * ScenarioShotExposureFrames is spent at full speed and is the only part
	 * of the warm-up that costs the hold: one second of it, against a
	 * four-second hold, leaves three seconds of margin on the frame that must
	 * be taken before the level runs.
	 *
	 * ScenarioShotWriteFrames exists because ProcessScreenShots writes at end
	 * of draw, so moving on in the same frame as the request loses the file.
	 *
	 * ScenarioShotRunFrames is 200 (3.33 s), what takes the world past a 4 s
	 * hold given the ~1.1 s already spent, with a second of slack.
	 * ScenarioShotFallFrames is 180 rather than 300 for the reason
	 * Tests/CorbelScreenshotTest.cpp gives — corbel-f-100's structure is 3,015
	 * bricks and all but a handful release at once, so every fall frame is
	 * Chaos solving three thousand bodies. Three seconds is still nine times
	 * the 0.32 s a released brick needs to fall the 50 cm that would be
	 * unmistakable in a photograph, and an eleven-metre arm does what it's
	 * going to do immediately.
	 */
	constexpr double ScenarioShotFixedDeltaSeconds = 1.0 / 60.0;

	constexpr int32 ScenarioShotFrozenFrames = 30;
	constexpr int32 ScenarioShotSlateFrames = 3;
	constexpr int32 ScenarioShotExposureFrames = 60;
	constexpr int32 ScenarioShotWriteFrames = 5;
	constexpr int32 ScenarioShotRunFrames = 200;
	constexpr int32 ScenarioShotFallFrames = 180;

	/** How many frames the map may take to load and build before the wait gives up and says so. */
	constexpr int32 ScenarioShotMapLoadFrameBudget = 3000;

	/** See Tests/PieceMenuScreenshotTest.cpp: derived as a floor, not picked as a judgement. */
	constexpr int64 ScenarioShotMinimumBytes = 32 * 1024;
	constexpr int32 ScenarioShotMinimumWidth = 640;
	constexpr int32 ScenarioShotMinimumHeight = 480;

	/**
	 * A millimetre, which no frame of real falling can hide inside. A frame
	 * of ordinary falling is 0.14 cm at 60 Hz, so the tolerance sits below
	 * the smallest movement a single frame of a genuine collapse could
	 * produce — and with dilation pinned for most of the warm-up, the real
	 * headroom is six orders of magnitude wider still.
	 */
	constexpr double ScenarioShotStillnessToleranceCm = 0.1;

	/** How near the pawn has to be to the viewpoint the catalogue computed. */
	constexpr double ScenarioShotViewpointToleranceCm = 0.01;

	/** The aspect the game mode frames for, restated so this file can print what it is comparing. */
	constexpr double ScenarioShotAspectHeightOverWidth = 1080.0 / 1920.0;

	/** Print a double so a comparison that failed in the last bit is readable as one. */
	inline FString ScenarioShotBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/**
	 * What the world-free catalogue says this row is and does — the oracle
	 * every world-side count is held against. Not an independent solver; what
	 * it's independent of is the world wire: the map, game mode, subsystem,
	 * spawned actors and timer. A level and a headless reading that disagree
	 * here are about two different structures, the one thing a photograph
	 * can't show.
	 */
	struct FScenarioShotOracle
	{
		bool bBuilt = false;

		int32 Pieces = 0;
		int32 Joints = 0;

		/** Pieces the row's own cut takes out. Zero for eight of the nine rows. */
		int32 CutPieces = 0;

		/** Passes `SolveAndBreak` broke at least one joint in, once the cut has been applied. */
		int32 BreakPasses = 0;

		/** Pieces the settled solve stops holding up — exactly what `ApplyResults` releases. */
		int32 WouldRelease = 0;

		/** The structure's bounding box as laid, which is what the viewpoint is framed on. */
		FBox BoundsCm = FBox(ForceInit);

		/** The bed joint under the arm's lowest outermost brick, for the corbel rows only. */
		int32 RootJoint = INDEX_NONE;
		int32 RootSeatPiece = INDEX_NONE;
		int32 RootArmPiece = INDEX_NONE;

		/**
		 * What makes the root joint the root joint, recorded so it can be
		 * asserted rather than assumed: it stands on the immovable base,
		 * carries something not part of it, and is a bed joint seen from the
		 * arm above. Without these three the reading below is a plausible
		 * number about whichever joint the search happened to reach first.
		 */
		bool bRootSeatGrounded = false;
		bool bRootArmGrounded = true;

		EJointRole RootRoleFromArm = EJointRole::None;

		/** What that joint reads AS LAID — solved and not settled, which is the held state. */
		double RootUtilisation = 0.0;
	};

	/**
	 * The root joint of a laid corbel: the bed joint under the arm's lowest
	 * outermost brick — the one place a corbel on an immovable base can fail,
	 * since a rigid body can't rotate about a fixed base without separating
	 * from it, and separation on a bed plane is that joint opening in tension.
	 *
	 * Found from groundedness and height, not a step outward, and that
	 * distinction is the whole of this comment. The first version looked for
	 * the lowest course whose outermost brick stood further out than the
	 * course below — sounds like the definition of a corbel, and is wrong,
	 * because DestructionCorbel::Build lays its base in an alternating bond:
	 * odd base courses shift by one step, so the very first joint the search
	 * met was between two courses of the immovable base. It named the same
	 * joint 12 for E35, E36 and F, read a confident 0, and every assertion
	 * here was satisfied — a plausible number about nothing. The rows below
	 * now assert the arm is not grounded and the seat is, so the mistake
	 * can't be silent a second time.
	 *
	 * Derived from the structure rather than the spec, which is why it's
	 * written out here at all: Tests/CorbelScenarioTest.cpp finds the same
	 * joint by computing where the grid says the two bricks are, from a base
	 * cell count and a left origin declared per row. Nothing on FScenario
	 * exposes either — a corbel row carries its producer as a closure — so a
	 * second copy of that table here would be a fixture to drift.
	 *
	 * A flat wall fails closed, by the same test: a running-bond wall grounds
	 * one course, every corbel-family structure grounds its whole base
	 * (three). So a structure with a single grounded course has no immovable
	 * base to cantilever off, no root joint, and this answers INDEX_NONE
	 * rather than naming an arbitrary bed joint as though it meant something.
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
		 * The base's top course is the highest course every one of whose
		 * bricks is grounded; the arm's first course is immediately above it.
		 * Courses are gathered by the Z of a brick's centre on the nose:
		 * every producer here lays a course at one exact height, and rounding
		 * to a grid would be a second opinion about where a course goes.
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

		// One grounded course is a wall standing on the earth, not an arm reaching off a plinth.
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
	 * Lay this row world-free, read it as laid, apply its cut, settle it, and
	 * count what is left unheld.
	 *
	 * The same predicate FStructureBinding::ApplyResults uses, written out
	 * rather than called: Grounded and Supported stay put, Stranded and
	 * Falling both come down, and a piece the last solve never answered for
	 * is never released. Written out because the point of the comparison is
	 * that the level and the catalogue agree — a helper shared with the world
	 * wire would agree with that wire however wrong it was.
	 *
	 * The cut is applied before the settle, and all of it before one settle —
	 * the order RunPieceActions runs in: every named brick goes, then exactly
	 * one cascade behind the last of them, so the push that follows sees an
	 * answer that saw every removal. A settle per brick would be a different
	 * experiment.
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

		/*
		 * As laid first, the held state. SolveLoads is non-destructive, so
		 * the reading printed beside the held frame is the one the
		 * world-free suite reports for this row, not whatever the joint
		 * carries after the cascade moved load around it.
		 */
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
	 * What one row's run learned, carried between latent commands.
	 *
	 * File-scope state, for the reason the two screenshot harnesses before
	 * this one give: a latent command carries only what its parameters
	 * carry, and the commands that matter run hundreds of frames apart. The
	 * one that catches the level being built is the only one that sees it
	 * intact, and the two that judge the frames need what it saw. Reset at
	 * the top of every row so one row can't inherit the last one's world.
	 */
	struct FScenarioShotRecord
	{
		bool bJoined = false;

		/**
		 * The world that was up before the travel, and why the harness has
		 * to hold one. Open is serviced at the end of a tick, so the frame
		 * after it is issued still has the old world in it — and for the
		 * first row that old world is the startup map, Lvl_Sandbox, running
		 * row 0 with a structure built. The join wait once accepted it,
		 * froze a world about to be thrown away, and photographed the real
		 * one unfrozen and already past its hold. Requiring a different
		 * UWorld is what makes the wait a wait.
		 */
		TWeakObjectPtr<UWorld> WorldBeforeTravel;

		int32 StructureId = INDEX_NONE;

		FScenarioShotOracle Oracle;

		/** Where each brick stood when the level laid it, so stillness can be measured. */
		TArray<FVector> LaidAtCm;

		/** World time the instant the level finished building, which is when the hold started. */
		double BuiltAtSeconds = 0.0;

		/** The graph's solve count while held, so "the level ran" is a claim about the solver. */
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

	/**
	 * What the level would be telling the player at this moment, in its own
	 * words. ADestructionGameGameMode::GetScenarioLabel composes it from the
	 * row it recorded building and the hold timer's own remainder, so the
	 * line printed beside a frame is the line the level itself would put on
	 * screen. Read here rather than reproduced, since a second copy of the
	 * wording is a caption that could disagree with the level.
	 */
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

	/** How many live pieces have been handed to physics. */
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

	/** Pin the world's clock, or say why it could not be pinned. Answers the dilation it reached. */
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

	/**
	 * Ask for a screenshot, through the viewport client and not through
	 * GEngine. Measured, not preferred — see Tests/StaircaseScreenshotTest.cpp,
	 * whose first run asserted everything correctly and wrote no PNG at all
	 * because the request went to UEngine::Exec, which has no SHOT handler.
	 * HandleScreenshotCommand lives on UGameViewportClient.
	 */
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

/**
 * Pin the engine's clock so a frame count is a duration, and check there is
 * something to photograph into.
 *
 * The fixed time step is the harness's, not the game's, and is restored at
 * the end. Without it the world seconds spent warming up the renderer is a
 * function of how fast the machine draws 3,015 bricks, and the four-second
 * hold is a race the slow machine loses silently, photographing a collapse
 * and calling it "held".
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FScenarioShotOpenSessionCommand, FAutomationTestBase*, Test);

bool FScenarioShotOpenSessionCommand::Update()
{
	using namespace ScenarioLevelShotSupport;

	UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

	/*
	 * The viewport is asserted here rather than left to show up as a missing
	 * file: without one, HandleScreenshotCommand returns having done
	 * nothing, and the only downstream symptom reads identically to a
	 * renderer that failed.
	 */
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

/** Put the clock back exactly as it was found. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FScenarioShotCloseSessionCommand, FAutomationTestBase*, Test);

bool FScenarioShotCloseSessionCommand::Update()
{
	FApp::SetUseFixedTimeStep(false);

	Test->AddInfo(TEXT("the engine clock is back on real time"));

	return true;
}

/** Travel to this row's own map, which is the only thing this harness ever asks the game to do. */
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

	/*
	 * The engine's own Open, always, even for the map already up. The
	 * startup map has been running since the process began — through shader
	 * compilation and every other test in the queue — so its hold expired
	 * long ago and its "held" frame would picture a level that had already
	 * run. Travelling unconditionally gives every row the same freshly begun
	 * world.
	 */
	GEngine->Exec(World, *FString::Printf(TEXT("Open %s"), *Package));

	return true;
}

/**
 * Wait for the level to finish building, and freeze the world the instant it
 * has.
 *
 * Everything recorded here is read from the level rather than given to it:
 * the row was selected by the map name, the structure laid by
 * DestructionScenarios::Build, the bricks spawned by
 * UDestructionStructureSubsystem::BuildLayout, and the camera placed by the
 * game mode. This command's whole job is to notice that it happened, stop
 * the clock so the renderer can warm up inside the hold, and write down
 * where every brick was standing while it still is.
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

	/*
	 * A different world from the one the travel was issued from — that
	 * clause is the whole of this wait. Without it the first row accepts the
	 * startup map (Lvl_Sandbox, already running row 0, up for ten seconds),
	 * freezes a world about to be discarded, and photographs the real one
	 * unfrozen and long past its hold.
	 */
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

	/*
	 * The clock stops here, on the first frame the level exists.
	 * AWorldSettings clamps this to MinGlobalTimeDilation, which
	 * BaseGame.ini sets to 0.0001, so the shader drain and Slate settle
	 * below advance the hold by microseconds however long they take in real time.
	 */
	const float Frozen = ScenarioShotSetDilation(*Test, World, 0.0f);

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s': the world must actually freeze while the renderer warms up, or the hold is a ")
			TEXT("race; its dilation is %g"),
			*Row->Name.ToString(), Frozen),
		Frozen > 0.0f && Frozen <= 0.001f);

	Record.BuiltAtSeconds = World->GetTimeSeconds();

	/*
	 * And the world is a fresh one. A stale world that happened to be
	 * running the right row would pass every count below and be
	 * photographed long after its hold expired — the one failure mode a
	 * picture of an intact structure can't distinguish from success.
	 */
	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' must have been JOINED rather than found already running: %s s of world time ")
			TEXT("had passed when it finished building, and its hold is %s s"),
			*Row->Name.ToString(), *ScenarioShotBits(Record.BuiltAtSeconds),
			*ScenarioShotBits(Row->HoldSeconds)),
		Record.BuiltAtSeconds < 1.0);

	/* --- the row the LEVEL chose, and how ------------------------------------------------- */

	Test->TestTrue(
		*FString::Printf(
			TEXT("'%s' must be selected BY ITS OWN MAP NAME — that is the door a player uses, and a ")
			TEXT("map that opens onto somebody else's wall is worse than one that does not open. It ")
			TEXT("selected row %d and reports selection kind %d."),
			*Row->Name.ToString(), GameMode->GetSelectedScenarioRow(),
			static_cast<int32>(GameMode->GetScenarioSelection())),
		GameMode->GetScenarioSelection() == DestructionScenarios::EScenarioSelection::ByMapName);

	/* --- what the world-free catalogue says this row is ----------------------------------- */

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

	/* --- and where the level put the player ---------------------------------------------- */

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

	/*
	 * The player is where the catalogue says, and ViewpointFor is called
	 * rather than copied — the claim is that the level framed the
	 * structure, so re-deriving the standoff here would agree with a wrong
	 * answer instead of failing against it.
	 */
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

	/*
	 * And it is looking at the structure rather than standing inside it.
	 * Two of the three ways a valid PNG can picture nothing — a camera
	 * buried in the masonry, a wall behind the lens — are exactly these two
	 * rows; the third, a wall out of frame, is what the standoff above is for.
	 */
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

	/* --- and the numbers a human needs beside the pictures -------------------------------- */

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

		/*
		 * And it is the root joint rather than the first bed joint the
		 * search reached. The first version of the finder named a joint
		 * inside the immovable base — the corbel's base is laid in an
		 * alternating bond, so its second course steps out too — printed a
		 * confident 0 for every corbel in the family, and nothing here
		 * objected. These three rows stop a plausible number from standing
		 * in for a reading about nothing.
		 */
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

/** Let the world run again, so the exposure and the temporal history settle at a real frame rate. */
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
 * The structure is standing exactly as the level laid it, the hold has not
 * expired, and this is the picture of it.
 *
 * The stillness is measured rather than argued. A brick spawns kinematic and
 * only Release makes it dynamic, so nothing here can have moved — but
 * "nothing can have moved" is precisely the kind of claim that stops being
 * true when somebody changes the spawn path, and the whole value of a held
 * frame is that it's a picture of the intact structure.
 *
 * And the non-movement isn't the only claim, because it couldn't be:
 * DESIGN.md §4 bans reading a displacement as evidence a joint broke. The
 * converse is sound (an unmoved brick hasn't been released), but a released
 * brick jammed on its neighbours would satisfy it, so IsReleased is asserted
 * beside it — the mechanism reading of "held as laid", a countable binary
 * fact rather than a measurement.
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

	/*
	 * And held is solved-and-not-settled rather than unsolved. A level
	 * could hold by never solving at all, and every release count would
	 * read zero because nothing ever asked — the wrong hold, and one this
	 * suite has been bitten by from the other end: an absent support answer
	 * reads as Falling, so an unsolved structure and one in free fall are
	 * one answer to every readout.
	 */
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
 * The hold expired, the level ran, whatever was going to fall has fallen,
 * and this is the picture of it.
 *
 * The release count is the assertion and the movement only printed.
 * DESIGN.md §4 forbids reading a displacement as evidence a joint broke —
 * two pieces can sever and rest exactly where they were — so what's held
 * against the world-free catalogue is the exact number of pieces the settle
 * stopped holding up; an inequality would pass against a level that dropped
 * everything it owns.
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

	/*
	 * The level ran, a separate claim from anything having fallen, and the
	 * only claim available on a row that correctly releases nothing.
	 * Settling is a solve, so the graph's own solve count going up says the
	 * hold expired and something happened.
	 */
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

	/*
	 * Printed, not asserted — the number that lets a reader tell a picture
	 * of a collapse from one of a structure that was condemned and didn't
	 * visibly move, a judgement only a human looking at the two frames can
	 * make.
	 */
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

/** The eighteen files landed and they are real PNGs. Every one was deleted before the run. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FScenarioShotCheckFilesCommand, FAutomationTestBase*, Test);

bool FScenarioShotCheckFilesCommand::Update()
{
	using namespace ScenarioLevelShotSupport;

	for (const DestructionScenarios::FScenario& Row : DestructionScenarios::Catalogue())
	{
		// A build sandbox was never photographed — an empty plot has nothing to photograph.
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
			 * The signature and the IHDR, read by hand: eight signature
			 * bytes, a four-byte chunk length, "IHDR", then width and height
			 * as big-endian 32-bit integers. A byte count alone passes for a
			 * file of random bytes, and a decoder would depend on the very
			 * rendering stack under test.
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

	/*
	 * A floor on the sweep, so a catalogue that emptied — or a lookup that
	 * started answering nothing — fails here rather than looping over no
	 * rows, writing no files, and passing in silence. Nine is what the
	 * scenario slices left.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("fixture: the catalogue must carry at least the 9 rows the scenario slices left, or ")
			TEXT("this photographs nothing; it carries %d"),
			Rows.Num()),
		Rows.Num() >= 9);

	/*
	 * The old files go first, synchronously, before any latent command is
	 * queued. Everything downstream reads "the file exists" as "this run
	 * rendered a frame", true only if the file can't have survived from an
	 * earlier run.
	 */
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
	 * The sequence, per row: travel to its map, catch the level the frame it
	 * finishes building and freeze the world, drain the shaders and settle
	 * Slate for free, thaw and let exposure converge inside the hold,
	 * photograph the held structure, then run the world past the hold and
	 * far enough beyond it for anything that was going to fall to have fallen.
	 *
	 * The shader drain is inside the freeze, the only place it can go: it
	 * waits on real time with no bound after a map load, so a drain outside
	 * the freeze would spend the hold on shader compilation and photograph a
	 * level that had already run.
	 */
	for (int32 RowIndex = 0; RowIndex < Rows.Num(); ++RowIndex)
	{
		/*
		 * A build sandbox is not photographed: the row lays no structure and
		 * the player fills the plot, so both frames would picture empty
		 * ground. Skipped here and in the file check below, which reads "the
		 * file exists" as "this row rendered".
		 */
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
