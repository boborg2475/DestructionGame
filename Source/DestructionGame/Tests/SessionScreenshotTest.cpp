// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/PieceMenu.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/SessionToolbar.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"
#include "DestructionGamePlayerController.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "World/BrickActor.h"
#include "World/BuildModeComponent.h"
#include "World/DestructionScenarios.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Screenshots of the interactive session on Lvl_Build. SessionControllerTest proves the mechanism
 * under -nullrhi; this proves the strip, ghost and details window are actually drawn. Every action
 * goes through the controller's own seams (OnToolbarButton, PointerAlongRay, PrimaryAlongRay,
 * SetInspectedPiece, ChoosePieceMenuRow); the component and subsystem are only read.
 *
 * Frames:
 *   1 Session_Build: a 5-brick wall with a timber plate on top, laid with the Screw chip (then back
 *     to Auto), and a ghost hovering past the wall.
 *   2 Session_Destroy: details window open on the course-1 brick under the plate; its rows show
 *     mortar, perpend and screw. Readouts say "not solved yet" because laying never solves.
 *   3 Session_Deleted: that brick deleted and the cascade settled.
 *   4 Session_LoadOverlay: the load overlay on over the settled wall (after the delete, which solves).
 *   5 Session_Run: a jointless Free brick in mid-air, released by Run and fallen.
 *   6 Session_Corner: plot cleared, an L-wall laid with the Rotate chip, rotated ghost at the end.
 *   7 Session_Ghost: one seed brick and a ghost shown with no click, turned by the Rotate chip.
 *
 * Frames 1-5 share one camera, placed once through the game mode's own framing (ViewpointFor,
 * ThreeQuarter) but moved in, since the plot's default framing makes the wall ~7% of the frame.
 * Frames 6 and 7 each reframe once for their own subject.
 *
 * Each frame asserts the state it depicts (mode, piece count, menu, ghost visibility, plus one
 * mechanism reading). Never displacement: the fallen brick's travel is only reported. Nothing is
 * asserted about pixels; that is for a human.
 *
 * NonNullRHI: without it the -nullrhi suite would find no viewport, write nothing and pass. Run it
 * explicitly from PowerShell (Git Bash mangles the map path):
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject"
 *     /Game/Maps/Scenarios/Lvl_Build
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.SessionScreenshots"
 *     -TestExit="Automation Test Queue Empty"
 *
 * -nullrhi must be absent (no viewport is built under it), and the map must be on the command line
 * (Lvl_Build selects the build catalogue row).
 */
namespace SessionScreenshotSupport
{
	using namespace DestructionSession;

	/** The seven frames' file base names, under FPaths::ScreenShotDir(). */
	const TCHAR* const BuildBaseName = TEXT("Session_Build");
	const TCHAR* const DestroyBaseName = TEXT("Session_Destroy");
	const TCHAR* const DeletedBaseName = TEXT("Session_Deleted");
	const TCHAR* const LoadOverlayBaseName = TEXT("Session_LoadOverlay");
	const TCHAR* const RunBaseName = TEXT("Session_Run");
	const TCHAR* const CornerBaseName = TEXT("Session_Corner");
	const TCHAR* const GhostBaseName = TEXT("Session_Ghost");

	/** All seven, so the pre-run deletion and post-run PNG check apply to each alike. */
	const TCHAR* const ScreenshotBaseNames[] = {
		BuildBaseName, DestroyBaseName, DeletedBaseName, LoadOverlayBaseName, RunBaseName,
		CornerBaseName, GhostBaseName };

	/**
	 * `Shot showui`, not HighResShot: HighResShot renders scene-only and would omit the Slate strip
	 * and menu this test is about. See Tests/PieceMenuScreenshotTest.cpp.
	 */
	inline FString ShotCommandFor(const FString& BaseName)
	{
		return FString::Printf(TEXT("Shot showui filename=%s -nosuffix"), *BaseName);
	}

	inline FString ScreenshotPathFor(const FString& BaseName)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::ScreenShotDir() / BaseName + TEXT(".png"));
	}

	/** Debug overlays off, so nothing covers the UI being judged. */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/**
	 * Timings, shared with the sibling harnesses. Warm-up/settle cover TSR history and auto-exposure;
	 * SlateFrames lets a rebuilt widget get laid out; WriteFrames because the PNG is written at end of
	 * draw. FallFrames (3 s, vs 0.28 s for a 37.5 cm fall) is waited after the delete and after Run.
	 */
	constexpr int32 WarmUpFrames = 120;
	constexpr int32 SettleFrames = 60;
	constexpr int32 SlateFrames = 3;
	constexpr int32 WriteFrames = 5;
	constexpr int32 FallFrames = 180;

	/** See Tests/PieceMenuScreenshotTest.cpp: derived as a floor, not picked as a judgement. */
	constexpr int64 MinimumScreenshotBytes = 32 * 1024;
	constexpr int32 MinimumScreenshotWidth = 640;
	constexpr int32 MinimumScreenshotHeight = 480;

	/*
	 * The grid, spelled out rather than imported. A brick is 21.5 x 10.25 x 6.5 cm on a 1 cm joint:
	 * 22.5 across, 11.25 half stagger, 7.5 up. A brick on course n centres at n * 7.5 + 3.25; a plate
	 * (5.0 cm half-height) at n * 7.5 + 5.0.
	 */

	constexpr double BrickPlaneCourse0Cm = 3.25;
	constexpr double BrickPlaneCourse1Cm = 10.75;

	/** Course 2's plane for a plate. */
	constexpr double PlatePlaneCourse2Cm = 20.0;

	/** Course 5 for a brick; its underside is 37.5 cm up. */
	constexpr int32 FloatingCourse = 5;
	constexpr double FloatingPlaneZCm = 40.75;

	/** Palette half extents, written out because the framing box is built from them. */
	const FVector HalfBrickCm(10.75, 5.125, 3.25);
	const FVector HalfPlateCm(33.75, 5.125, 5.0);

	/**
	 * Wall cursors, all on Y = 0. Course 0 steps one pitch so each takes the same-course snap at zero
	 * offset; course 1 sits on the half stagger, bedding onto two bricks. The plate is centred.
	 */
	constexpr double Course0CursorsXCm[] = { 0.0, 22.5, 45.0 };
	constexpr double Course1CursorsXCm[] = { 11.25, 33.75 };
	constexpr double PlateCursorXCm = 22.5;

	/**
	 * The brick inspected and then deleted: the first course-1 brick (index 3). Chosen because the
	 * plate bears on it with a screw whichever tied snap the plate takes, so its readout names three
	 * profiles (mortar beds, perpend, screw) rather than one.
	 */
	constexpr int32 InspectedPieceIndex = 3;
	constexpr double InspectedCentreXCm = 11.25;

	/**
	 * Frame 1's ghost cursor. Every plate snap is occupied, so the ghost falls back to Free at the
	 * cursor and must not overlap the real plate. The plate's right end is at 45 or 67.5 depending on
	 * which tied snap it took; a ghost at 112.5 starts at 78.75, clear by 11.25 cm.
	 */
	constexpr double GhostCursorXCm = 112.5;

	/**
	 * The floating Free brick (frame 5). Free forms no joints, and 120 cm is 75 cm from the nearest
	 * brick, outside the 30 cm snap radius. Its 37.5 cm underside cannot read grounded.
	 */
	constexpr double FloatingCursorXCm = 120.0;

	/** Laying rays start this far above the plane point and point straight down (pick limit is 1 km). */
	constexpr double RayHeightCm = 200.0;

	inline FVector LayRayStart(double XCm, double PlaneZCm)
	{
		return FVector(XCm, 0.0, PlaneZCm + RayHeightCm);
	}

	inline FVector LayRayEnd(double XCm, double PlaneZCm)
	{
		return FVector(XCm, 0.0, PlaneZCm);
	}

	/** The same ray off Y = 0, for the L-wall's Y leg. */
	inline FVector LayRayStartXY(double XCm, double YCm, double PlaneZCm)
	{
		return FVector(XCm, YCm, PlaneZCm + RayHeightCm);
	}

	inline FVector LayRayEndXY(double XCm, double YCm, double PlaneZCm)
	{
		return FVector(XCm, YCm, PlaneZCm);
	}

	/*
	 * Frame 6's L-wall: the cursors and expected counts from Core.BuildMode.CornerWallStands, whose
	 * header derives each one, laid here through the player's click.
	 *
	 * The order matters: joints come only from candidates the placed piece emits (CR-2a finding ix),
	 * so the corner brick at (56.25, 0) must be laid after the Y leg's brick at (61.875, 16.875).
	 * Reversed, the wall has 18 connections and 11 mortar instead of 19 and 12.
	 */
	struct FCornerStep
	{
		double XCm;
		double YCm;

		int32 Course;

		/** Whether the piece lies along Y (the Rotate chip). */
		bool bRotated;
	};

	const FCornerStep CornerSteps[] = {
		/* Course 0, X leg. */
		{  0.000,  0.000, 0, false },
		{ 22.500,  0.000, 0, false },
		{ 45.000,  0.000, 0, false },

		/* The rotated return off the X leg's +X end. */
		{ 61.875,  5.000, 0, true },

		/* Course 0, Y leg. */
		{ 61.875, 28.000, 0, true },
		{ 61.875, 50.500, 0, true },

		/* Course 1, over the X leg. */
		{ 11.250,  0.000, 1, false },
		{ 33.750,  0.000, 1, false },

		/* Y leg's first course-1 brick, before the lap (see order note above). */
		{ 61.875, 16.875, 1, true },

		/* The corner brick lapping over the return, bonding both legs. */
		{ 56.250,  0.000, 1, false },

		{ 61.875, 39.375, 1, true },
	};

	constexpr int32 CornerExpectedPieces = 11;
	constexpr int32 CornerExpectedConnections = 19;
	constexpr int32 CornerExpectedMortar = 12;
	constexpr int32 CornerExpectedPerpend = 7;

	/** Indices 0-5 are on the earth; later pieces reach it only through their beds. */
	constexpr int32 CornerLastGroundedPiece = 5;

	/** Frame 6's ghost: the next Y-leg course-1 brick, rotated (39.375 + 22.5). */
	constexpr double CornerGhostXCm = 61.875;
	constexpr double CornerGhostYCm = 61.875;

	/**
	 * The L's bounds for frame 6's reframe; the frame-1 camera would show the corner edge-on.
	 *
	 *   the X leg           X -10.75 .. 55.75,  Y  -5.125 ..  5.125
	 *   the course-1 lap    X  45.50 .. 67.00,  Y  -5.125 ..  5.125
	 *   the Y leg           X  56.75 .. 67.00,  Y  -5.125 .. 61.375
	 *   the rotated ghost   X  56.75 .. 67.00,  Y  51.125 .. 72.625
	 *   and Z 0 .. 14 (two courses of brick on their joints)
	 */
	inline FBox CornerStageBoundsCm()
	{
		return FBox(FVector(-10.75, -5.125, 0.0), FVector(67.0, 72.625, 14.0));
	}

	/*
	 * Frame 7, numbers from World.Session.CursorRefreshDrivesTheGhostFromARay: a seed brick at the
	 * origin, then a ray through (11.25, 3), 3 cm off the wall line so the ghost must be solved, not
	 * echoed. One seed only, so the ghost is easy to find and has few poses to pick from.
	 */

	constexpr double GhostSeedCursorXCm = 0.0;

	constexpr double GhostRayXCm = 11.25;
	constexpr double GhostRayYCm = 3.0;

	inline FVector GhostRayOriginCm()
	{
		return FVector(GhostRayXCm, GhostRayYCm, BrickPlaneCourse0Cm + RayHeightCm);
	}

	inline FVector GhostRayDirection()
	{
		return FVector(0.0, 0.0, -1.0);
	}

	/**
	 * Ghost footprints before and after Rotate (a quarter turn about Z swaps X and Y). The bounds
	 * size is the reading that proves the chip reached the ghost, not just lit.
	 */
	const FVector UprightGhostSizeCm(21.5, 10.25, 6.5);
	const FVector RotatedGhostSizeCm(10.25, 21.5, 6.5);

	/** The upright ghost before Rotate: the next-course running-bond snap. Asserted as a fixture. */
	const FVector UprightGhostCentreCm(11.25, 0.0, 10.75);

	/** Same tolerance as the sibling session tests. */
	constexpr double GhostBoundsToleranceCm = 0.05;

	/**
	 * Frame 7's box: the seed plus slack for the turned ghost. The rotated pose is the solver's and is
	 * only reported, so the box is not pinned to it. Measured:
	 *
	 *   the seed brick      X -10.75 .. 10.75,  Y  -5.125 ..  5.125,  Z 0 .. 6.5
	 *   the turned ghost    X  11.75 .. 22.00,  Y  -5.125 .. 16.375,  Z 0 .. 6.5
	 *
	 * Shrinking it changes nothing: ViewpointFor floors the standoff at ScenariosMinimumStandoffCm
	 * (120 cm), and this camera already sits at that floor.
	 */
	inline FBox GhostStageBoundsCm()
	{
		return FBox(FVector(-13.0, -9.0, 0.0), FVector(25.0, 19.0, 9.0));
	}

	/**
	 * The destroy ray runs along Y, not down: a real trace from above would hit the plate first.
	 * +/- 100 cm clears the 10.25 cm-deep wall on both sides.
	 */
	constexpr double InspectReachCm = 100.0;

	/**
	 * The box frames 1-5 are framed over. Stated, not measured, because the camera is placed before
	 * anything is laid. The union of every pose those frames show:
	 *
	 *   the wall            X -10.75 .. 55.75,  Z 0 .. 14
	 *   the timber plate    X -22.50 .. 67.50,  Z 15 .. 25   (either centred snap, see GhostCursorXCm)
	 *   the ghost           X  78.75 .. 146.25, Z 15 .. 25
	 *   the floating brick  X 109.25 .. 130.75, Z 37.5 .. 44
	 *   and where it lands  X 109.25 .. 130.75, Z 0 .. 6.5
	 *
	 * Y is the bricks' 10.25 cm depth.
	 */
	inline FBox SessionStageBoundsCm()
	{
		return FBox(
			FVector(-22.5, -5.125, 0.0),
			FVector(GhostCursorXCm + HalfPlateCm.X, 5.125, FloatingPlaneZCm + HalfBrickCm.Z));
	}

	/** Height over width at 1920 x 1080, as the game mode frames. */
	constexpr double FrameAspectHeightOverWidth = 1080.0 / 1920.0;

	/** State carried between latent commands, which run frames apart. */
	struct FSessionShotRecord
	{
		bool bStaged = false;

		TWeakObjectPtr<ADestructionGamePlayerController> Controller;

		int32 StructureId = INDEX_NONE;

		/**
		 * The Free brick's index and its actor's location at release. Actor, not box centre: the
		 * actor's pivot is its corner, and mixing the two would misreport the fall by 11 cm.
		 */
		int32 FloatingPieceIndex = INDEX_NONE;
		FVector FloatingActorAtReleaseCm = FVector::ZeroVector;

		void Reset()
		{
			*this = FSessionShotRecord();
		}
	};

	inline FSessionShotRecord& SessionShotRecord()
	{
		static FSessionShotRecord Record;
		return Record;
	}

	/** The controller the player is driving, or null with the reason reported. */
	inline ADestructionGamePlayerController* FindController(
		FAutomationTestBase& Test, UWorld* World)
	{
		ADestructionGamePlayerController* const Controller = World != nullptr
			? Cast<ADestructionGamePlayerController>(World->GetFirstPlayerController())
			: nullptr;

		if (Controller == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("the first player controller should be an ADestructionGamePlayerController, it is %s"),
				*GetNameSafe(World != nullptr ? World->GetFirstPlayerController() : nullptr)));
		}

		return Controller;
	}

	/**
	 * Request a screenshot through the viewport client. UEngine::Exec has no Shot handler and silently
	 * writes nothing (see Tests/CorbelScreenshotTest.cpp).
	 */
	inline void RequestScreenshot(FAutomationTestBase& Test, const FString& Command)
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

	/** The player's live build, or null. Read only. */
	inline FStructureBinding* FindBuild(UWorld* World, int32 StructureId)
	{
		UDestructionStructureSubsystem* const Subsystem =
			World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;

		return Subsystem != nullptr && StructureId != INDEX_NONE
			? Subsystem->Find(StructureId)
			: nullptr;
	}

	const TCHAR* ModeName(ESessionMode Mode)
	{
		switch (Mode)
		{
		case ESessionMode::Build:   return TEXT("Build");
		case ESessionMode::Destroy: return TEXT("Destroy");
		default:                    return TEXT("<unknown mode>");
		}
	}

	const TCHAR* PieceName(EBuildPieceKind Kind)
	{
		switch (Kind)
		{
		case EBuildPieceKind::Brick:        return TEXT("Brick");
		case EBuildPieceKind::TimberPlate:  return TEXT("TimberPlate");
		case EBuildPieceKind::TimberLintel: return TEXT("TimberLintel");
		default:                            return TEXT("<unknown piece>");
		}
	}

	/** The whole session state on one line, so a failure reads without a debugger. */
	FString StateBits(const FSessionToolbarState& State)
	{
		return FString::Printf(
			TEXT("{mode %s, piece %s, placement %s, course %d, hasStructure %d}"),
			ModeName(State.Mode), PieceName(State.Piece),
			State.Placement == EPlacementMode::Free ? TEXT("Free") : TEXT("Snap"),
			State.Course, State.bHasStructure ? 1 : 0);
	}

	/** Whether the strip for this state offers the button enabled. */
	bool ButtonIsEnabled(const FSessionToolbarState& State, EToolbarButtonId Id)
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		const FToolbarButton* const Button = Buttons.FindByPredicate(
			[Id](const FToolbarButton& Candidate) { return Candidate.Id == Id; });

		return Button != nullptr && Button->bEnabled;
	}

	/** The index of the presented row with this label, or INDEX_NONE. */
	int32 FindMenuRow(TArrayView<const FPieceMenuRow> Rows, const TCHAR* Label)
	{
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			if (Rows[Index].Label == FString(Label))
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	FString DescribeMenuRows(TArrayView<const FPieceMenuRow> Rows)
	{
		if (Rows.Num() == 0)
		{
			return TEXT("<no menu>");
		}

		FString Line;

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'->{%d,%d}"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Rows[Index].Label, Rows[Index].Ref.StructureId, Rows[Index].Ref.PieceIndex);
		}

		return Line;
	}

	/** Asserts the state a frame depicts (mode, piece count, menu, ghost visibility) as the shot is queued. */
	inline void CheckStageBeforeShot(
		FAutomationTestBase& Test,
		const TCHAR* Stage,
		ESessionMode ExpectedMode,
		int32 ExpectedPieces,
		bool bExpectMenu,
		bool bExpectGhostVisible)
	{
		const FSessionShotRecord& Record = SessionShotRecord();

		ADestructionGamePlayerController* const Controller = Record.Controller.Get();

		if (Controller == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the controller vanished before the shot"), Stage));

			return;
		}

		const FSessionToolbarState& State = Controller->GetSessionToolbarState();

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the session must be in %s mode for this frame to show the strip it claims; it "
					 "is %s"),
				Stage, ModeName(ExpectedMode), *StateBits(State)),
			State.Mode == ExpectedMode);

		const FStructureBinding* const Binding = FindBuild(Controller->GetWorld(), Record.StructureId);

		const int32 Pieces = Binding != nullptr ? Binding->NumPieces() : -1;

		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: the build must hold %d laid piece(s) at this frame, it holds %d"),
				Stage, ExpectedPieces, Pieces),
			Pieces, ExpectedPieces);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the piece menu must be %s in this frame; it shows [%s]"),
				Stage, bExpectMenu ? TEXT("UP") : TEXT("DOWN"),
				*DescribeMenuRows(Controller->GetShownPieceMenuRows())),
			Controller->IsPieceMenuShown() == bExpectMenu);

		// The ghost has no controller-level accessor, so it is read off the component.
		const UBuildModeComponent* const Build = Controller->GetBuildComponent();

		const AActor* const Ghost = Build != nullptr ? Build->GetGhostActor() : nullptr;

		if (bExpectGhostVisible)
		{
			Test.TestNotNull(
				*FString::Printf(TEXT("%s: there must be a ghost actor to photograph"), Stage), Ghost);

			if (Ghost != nullptr)
			{
				Test.TestFalse(
					*FString::Printf(
						TEXT("%s: the ghost must be VISIBLE — the frame claims the player can see where "
							 "the next click lands"),
						Stage),
					Ghost->IsHidden());
			}
		}
		else if (Ghost != nullptr)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: NO GHOST MAY BE IN THIS FRAME — a gold brick hanging over a wall the player "
						 "is demolishing is the most confusing thing this UI can do"),
					Stage),
				Ghost->IsHidden());
		}
	}
}

/** Find the world, viewport and controller, and place the frames 1-5 camera. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotOpenStageCommand, FAutomationTestBase*, Test);

bool FSessionShotOpenStageCommand::Update()
{
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();
	Record.Reset();

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world: /Game/Maps/Scenarios/Lvl_Build must be named on "
							"the command line"));

		return true;
	}

	Test->AddInfo(FString::Printf(TEXT("game world is %s"), *World->GetMapName()));

	// Asserted here: without a viewport the shot silently does nothing, which reads like a render failure.
	UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

	Test->TestNotNull(
		TEXT("there must be a game viewport for Slate to screenshot: -nullrhi must be absent"),
		Viewport);

	ADestructionGamePlayerController* const Controller = FindController(*Test, World);

	if (Controller == nullptr)
	{
		return true;
	}

	Record.Controller = Controller;

	// Fixture: the game mode opens Lvl_Build in Build mode; anything else means the wrong map.
	const DestructionSession::FSessionToolbarState& State = Controller->GetSessionToolbarState();

	Test->TestTrue(
		*FString::Printf(
			TEXT("fixture: the build plot must have opened the session in BUILD mode — if it did not, "
				 "the map on the command line is not Lvl_Build. The state is %s"),
			*StateBits(State)),
		State.Mode == DestructionSession::ESessionMode::Build);

	/* --- the camera, in the game mode's own framing -------------------------------------------- */

	APawn* const Pawn = Controller->GetPawn();

	if (Pawn == nullptr)
	{
		Test->AddError(TEXT("the player controller has no pawn to put the camera on"));
		return true;
	}

	const FBox StageCm = SessionStageBoundsCm();

	const DestructionScenarios::FViewpoint Viewpoint = DestructionScenarios::ViewpointFor(
		StageCm, FrameAspectHeightOverWidth, DestructionScenarios::EScenarioFraming::ThreeQuarter);

	Pawn->SetActorLocation(Viewpoint.LocationCm);
	Controller->SetControlRotation(Viewpoint.Rotation);

	Test->AddInfo(FString::Printf(
		TEXT("stage spans %.2f x %.2f x %.2f cm centred on (%.2f, %.2f, %.2f); the camera is moved in "
			 "from the game mode's plot framing to (%.2f, %.2f, %.2f) at rotation (%.2f, %.2f, %.2f)"),
		2.0 * StageCm.GetExtent().X, 2.0 * StageCm.GetExtent().Y, 2.0 * StageCm.GetExtent().Z,
		StageCm.GetCenter().X, StageCm.GetCenter().Y, StageCm.GetCenter().Z,
		Viewpoint.LocationCm.X, Viewpoint.LocationCm.Y, Viewpoint.LocationCm.Z,
		Viewpoint.Rotation.Pitch, Viewpoint.Rotation.Yaw, Viewpoint.Rotation.Roll));

	Record.bStaged = true;

	return true;
}

/** Lay the wall and plate through PrimaryAlongRay (the player's click), and leave the ghost hovering. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotBuildCommand, FAutomationTestBase*, Test);

bool FSessionShotBuildCommand::Update()
{
	using namespace DestructionSession;
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();

	if (!Record.bStaged)
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller = Record.Controller.Get();

	if (Controller == nullptr)
	{
		Test->AddError(TEXT("the controller vanished before the build"));
		return true;
	}

	/* --- course 0: three bricks on the earth ------------------------------------------------- */

	for (const double XCm : Course0CursorsXCm)
	{
		const bool bPlaced = Controller->PrimaryAlongRay(
			LayRayStart(XCm, BrickPlaneCourse0Cm), LayRayEnd(XCm, BrickPlaneCourse0Cm));

		Test->TestTrue(
			*FString::Printf(
				TEXT("a click at x = %g on course 0 must lay a brick; it reported %d"),
				XCm, bPlaced ? 1 : 0),
			bPlaced);
	}

	Record.StructureId = Controller->GetSessionStructureId();

	Test->TestTrue(
		*FString::Printf(
			TEXT("three laid bricks must give the session a structure to name; it names %d"),
			Record.StructureId),
		Record.StructureId != INDEX_NONE);

	/* --- course 1: two bricks staggered onto them --------------------------------------------- */

	Test->TestTrue(
		TEXT("Course up is always live in Build mode"),
		Controller->OnToolbarButton(EToolbarButtonId::CourseUp));

	for (const double XCm : Course1CursorsXCm)
	{
		const bool bPlaced = Controller->PrimaryAlongRay(
			LayRayStart(XCm, BrickPlaneCourse1Cm), LayRayEnd(XCm, BrickPlaneCourse1Cm));

		Test->TestTrue(
			*FString::Printf(
				TEXT("a click at x = %g on course 1 must lay a brick; it reported %d"),
				XCm, bPlaced ? 1 : 0),
			bPlaced);
	}

	/* --- and a timber plate bearing across the top -------------------------------------------- */

	Test->TestTrue(
		TEXT("a piece button is always live in Build mode"),
		Controller->OnToolbarButton(EToolbarButtonId::PieceTimberPlate));

	Test->TestTrue(
		TEXT("and Course up again, onto course 2"),
		Controller->OnToolbarButton(EToolbarButtonId::CourseUp));

	/*
	 * Screw only the plate; the bricks stay on Auto. Three profiles in one structure is what lets
	 * frame 2 show that the chip worked.
	 */
	Test->TestTrue(
		TEXT("the Screw chip must be clickable — a joint choice has no precondition"),
		Controller->OnToolbarButton(EToolbarButtonId::JointScrew));

	Test->TestTrue(
		*FString::Printf(
			TEXT("and the session must record it before the plate is laid; it reads %d"),
			static_cast<int32>(Controller->GetSessionToolbarState().Joint)),
		Controller->GetSessionToolbarState().Joint == EJointChoice::Screw);

	{
		const bool bPlaced = Controller->PrimaryAlongRay(
			LayRayStart(PlateCursorXCm, PlatePlaneCourse2Cm),
			LayRayEnd(PlateCursorXCm, PlatePlaneCourse2Cm));

		Test->TestTrue(
			*FString::Printf(
				TEXT("a click at x = %g on course 2 must lay the timber plate; it reported %d"),
				PlateCursorXCm, bPlaced ? 1 : 0),
			bPlaced);
	}

	// Back to Auto, so frame 1 shows the default strip and frame 5's brick is laid under Auto.
	Test->TestTrue(
		TEXT("the Auto chip must be clickable"),
		Controller->OnToolbarButton(EToolbarButtonId::JointAuto));

	Test->TestTrue(
		*FString::Printf(
			TEXT("and the session must be back on Auto for the frame; it reads %d"),
			static_cast<int32>(Controller->GetSessionToolbarState().Joint)),
		Controller->GetSessionToolbarState().Joint == EJointChoice::Auto);

	/* --- what the wall actually is, asserted before it is photographed ------------------------ */

	const FStructureBinding* const Binding = FindBuild(Controller->GetWorld(), Record.StructureId);

	if (Binding == nullptr)
	{
		Test->AddError(FString::Printf(
			TEXT("the build structure %d vanished while it was being laid"), Record.StructureId));

		return true;
	}

	Test->TestEqual(
		FString::Printf(
			TEXT("six clicks must give six pieces — five bricks and a plate; it holds %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 6);

	/*
	 * Connections prove the pieces are bonded, not just stacked. Expected 9 (2 heads on course 0,
	 * 2 + 3 into course 1, 2 plate bearings); asserted as >= 8 because the plate's tied snap is an
	 * ordering detail. An unbonded plate would read 6 or 7.
	 */
	Test->TestTrue(
		*FString::Printf(
			TEXT("THE PIECES MUST BE BONDED: 2 head joints on course 0, 5 bed/head joints into course 1 "
				 "and at least 2 bearings under the plate, so at least 8 connections. It holds %d"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections() >= 8);

	/*
	 * Both plate bearings must be screws, read off the graph that frame 2's readout presents. Both,
	 * because an override on only the first joint is the defect JointOverrideRidesThroughPlacement
	 * covers. All five fields, since sibling profiles share some.
	 */
	{
		const int32 PlatePiece = Binding->NumPieces() - 1;

		int32 Bearings = 0;
		int32 ScrewedBearings = 0;

		for (int32 Index = 0; Index < Binding->GetStructure().NumConnections(); ++Index)
		{
			const FConnection& Conn = Binding->GetStructure().GetConnection(Index);

			if (Conn.PieceA != PlatePiece && Conn.PieceB != PlatePiece)
			{
				continue;
			}

			++Bearings;

			const FConnectionStrength& S = Conn.Strength;
			const FConnectionStrength& Want = DestructionProfiles::Screw;

			const bool bIsScrew = S.CompressiveStrengthMPa == Want.CompressiveStrengthMPa
				&& S.ShearCohesionMPa == Want.ShearCohesionMPa
				&& S.TensileStrengthMPa == Want.TensileStrengthMPa
				&& S.FrictionCoefficient == Want.FrictionCoefficient
				&& S.MaxShearStrengthMPa == Want.MaxShearStrengthMPa;

			ScrewedBearings += bIsScrew ? 1 : 0;

			Test->AddInfo(FString::Printf(
				TEXT("plate joint #%d (%d-%d): {c %g, coh %g, t %g, mu %g, cap %g}"),
				Index, Conn.PieceA, Conn.PieceB,
				S.CompressiveStrengthMPa, S.ShearCohesionMPa, S.TensileStrengthMPa,
				S.FrictionCoefficient, S.MaxShearStrengthMPa));
		}

		Test->TestEqual(
			FString::Printf(
				TEXT("fixture: the plate must bear on BOTH course-1 bricks, it formed %d joint(s)"),
				Bearings),
			Bearings, 2);

		Test->TestEqual(
			FString::Printf(
				TEXT("AND BOTH OF THEM MUST CARRY THE SCREW THE PLAYER CHOSE: %d of %d do. A plate "
					 "screwed at one end and resting on friction at the other is the defect the "
					 "every-joint claim exists for, and the details window in frame 2 is reading these "
					 "very connections"),
				ScrewedBearings, Bearings),
			ScrewedBearings, 2);
	}

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const FVector CentreCm = Binding->GetBinding(Piece).Box.CentreCm;
		const FVector ExtentCm = Binding->GetBinding(Piece).Box.ExtentCm;

		Test->AddInfo(FString::Printf(
			TEXT("piece %d laid at (%.2f, %.2f, %.2f), half extent (%.2f, %.2f, %.2f), grounded %d"),
			Piece, CentreCm.X, CentreCm.Y, CentreCm.Z, ExtentCm.X, ExtentCm.Y, ExtentCm.Z,
			Binding->GetStructure().GetPiece(Piece).bIsGrounded ? 1 : 0));
	}

	Test->AddInfo(FString::Printf(
		TEXT("the laid build holds %d piece(s) and %d connection(s)"),
		Binding->NumPieces(), Binding->GetStructure().NumConnections()));

	/* --- the ghost, where the next click would land -------------------------------------------- */

	Controller->PointerAlongRay(
		LayRayStart(GhostCursorXCm, PlatePlaneCourse2Cm),
		LayRayEnd(GhostCursorXCm, PlatePlaneCourse2Cm));

	if (const UBuildModeComponent* const Build = Controller->GetBuildComponent())
	{
		if (const AActor* const Ghost = Build->GetGhostActor())
		{
			Test->AddInfo(FString::Printf(
				TEXT("the ghost stands at (%.2f, %.2f, %.2f), hidden %d"),
				Ghost->GetActorLocation().X, Ghost->GetActorLocation().Y, Ghost->GetActorLocation().Z,
				Ghost->IsHidden() ? 1 : 0));
		}
	}

	/* --- the strip this frame claims to be showing -------------------------------------------- */

	const FSessionToolbarState& State = Controller->GetSessionToolbarState();

	Test->TestTrue(
		*FString::Printf(
			TEXT("frame 1's strip must read Build, Plate, course 2, with a structure to command; "
				 "the state is %s"),
			*StateBits(State)),
		State.Mode == ESessionMode::Build
			&& State.Piece == EBuildPieceKind::TimberPlate
			&& State.Course == 2
			&& State.bHasStructure);

	Test->TestTrue(
		*FString::Printf(
			TEXT("and Clear build must be drawn LIVE over a plot with six pieces on it; the state is %s"),
			*StateBits(State)),
		ButtonIsEnabled(State, EToolbarButtonId::ClearBuild));

	return true;
}

/** Frame 1: Build mode, the wall laid, the ghost hovering. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShootBuildCommand, FAutomationTestBase*, Test);

bool FSessionShootBuildCommand::Update()
{
	using namespace SessionScreenshotSupport;

	if (!SessionShotRecord().bStaged)
	{
		return true;
	}

	CheckStageBeforeShot(
		*Test, TEXT("frame 1 (build)"), DestructionSession::ESessionMode::Build,
		/*ExpectedPieces*/ 6, /*bExpectMenu*/ false, /*bExpectGhostVisible*/ true);

	RequestScreenshot(*Test, ShotCommandFor(FString(BuildBaseName)));

	return true;
}

/**
 * Switch to Destroy, hover and click the inspected brick, then SetInspectedPiece (what hovering its
 * menu entry does) so the readout draws the per-joint breakout rather than a bare count.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotInspectCommand, FAutomationTestBase*, Test);

bool FSessionShotInspectCommand::Update()
{
	using namespace DestructionSession;
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();

	if (!Record.bStaged)
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller = Record.Controller.Get();

	if (Controller == nullptr)
	{
		Test->AddError(TEXT("the controller vanished before the inspect"));
		return true;
	}

	Test->TestTrue(
		TEXT("the Destroy tab is always live"),
		Controller->OnToolbarButton(EToolbarButtonId::ModeDestroy));

	const FVector InspectStart(InspectedCentreXCm, -InspectReachCm, BrickPlaneCourse1Cm);
	const FVector InspectEnd(InspectedCentreXCm, InspectReachCm, BrickPlaneCourse1Cm);

	Controller->PointerAlongRay(InspectStart, InspectEnd);

	Controller->PrimaryAlongRay(InspectStart, InspectEnd);

	const TArrayView<const FPieceMenuRow> Rows = Controller->GetShownPieceMenuRows();

	Test->TestTrue(
		*FString::Printf(
			TEXT("A CLICK IN DESTROY MODE MUST OPEN THE PIECE MENU on the brick the ray hit — that "
				 "window IS this frame's subject. It shows [%s]"),
			*DescribeMenuRows(Rows)),
		Controller->IsPieceMenuShown());

	const int32 DeleteRow = FindMenuRow(Rows, TEXT("Delete"));

	Test->TestTrue(
		*FString::Printf(
			TEXT("and it must offer Delete against the COURSE-1 BRICK UNDER THE PLATE, {%d,%d} — an "
				 "overhead ray would have hit the plate instead, which is why this one goes along Y. It "
				 "shows [%s]"),
			Record.StructureId, InspectedPieceIndex, *DescribeMenuRows(Rows)),
		DeleteRow != INDEX_NONE
			&& Rows.IsValidIndex(DeleteRow)
			&& Rows[DeleteRow].Ref.StructureId == Record.StructureId
			&& Rows[DeleteRow].Ref.PieceIndex == InspectedPieceIndex);

	if (DeleteRow == INDEX_NONE || !Rows.IsValidIndex(DeleteRow))
	{
		return true;
	}

	Controller->SetInspectedPiece(Rows[DeleteRow].Ref);

	const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();

	Test->TestTrue(
		*FString::Printf(
			TEXT("the clicked brick must be singled out so the readout has a breakout to draw; the "
				 "inspector reports %d selected, inspected=%s, %d joint(s)"),
			Inspector.SelectedCount,
			Inspector.bHasInspectedPiece ? TEXT("true") : TEXT("false"),
			Inspector.Joints.Num()),
		Inspector.bHasInspectedPiece);

	Test->AddInfo(FString::Printf(
		TEXT("readout: '%s' / support '%s' / '%s' over %d joint(s)"),
		*Inspector.CountText, *Inspector.SupportText, *Inspector.JointsText, Inspector.Joints.Num()));

	/*
	 * The rows must name both the screw and mortar, since the window is the only place the joint
	 * choice is visible. Counted, not matched: Presenter.JointRowNamesTheProfile owns the wording.
	 */
	{
		const auto RowSays = [](const FString& Text, const TCHAR* Word)
		{
			return Text.Contains(FString(Word), ESearchCase::IgnoreCase);
		};

		int32 ScrewRows = 0;
		int32 MortarRows = 0;

		for (const FInspectorJointRow& Row : Inspector.Joints)
		{
			Test->AddInfo(FString::Printf(TEXT("joint row: '%s'"), *Row.Text));

			ScrewRows += RowSays(Row.Text, TEXT("screw")) ? 1 : 0;
			MortarRows += RowSays(Row.Text, TEXT("mortar")) || RowSays(Row.Text, TEXT("perpend"))
				? 1 : 0;
		}

		Test->TestEqual(
			FString::Printf(
				TEXT("the inspected brick must show EXACTLY ONE screwed row — the plate's bearing over "
					 "it — and %d did"),
				ScrewRows),
			ScrewRows, 1);

		Test->TestTrue(
			*FString::Printf(
				TEXT("and at least one mortared row beside it, or the frame shows one word and proves "
					 "nothing about the choice; %d did"),
				MortarRows),
			MortarRows >= 1);
	}

	const FSessionToolbarState& State = Controller->GetSessionToolbarState();

	Test->TestTrue(
		*FString::Printf(
			TEXT("frame 2's strip must offer Run structure LIVE over the player's six-piece build; the "
				 "state is %s"),
			*StateBits(State)),
		ButtonIsEnabled(State, EToolbarButtonId::RunStructure));

	return true;
}

/** Frame 2: Destroy mode, the details window open on the inspected brick. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShootDestroyCommand, FAutomationTestBase*, Test);

bool FSessionShootDestroyCommand::Update()
{
	using namespace SessionScreenshotSupport;

	if (!SessionShotRecord().bStaged)
	{
		return true;
	}

	CheckStageBeforeShot(
		*Test, TEXT("frame 2 (destroy)"), DestructionSession::ESessionMode::Destroy,
		/*ExpectedPieces*/ 6, /*bExpectMenu*/ true, /*bExpectGhostVisible*/ false);

	RequestScreenshot(*Test, ShotCommandFor(FString(DestroyBaseName)));

	return true;
}

/** Choose Delete; the delete's own solve settles whatever it condemns. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotDeleteCommand, FAutomationTestBase*, Test);

bool FSessionShotDeleteCommand::Update()
{
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();

	if (!Record.bStaged)
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller = Record.Controller.Get();

	if (Controller == nullptr)
	{
		Test->AddError(TEXT("the controller vanished before the delete"));
		return true;
	}

	const TArrayView<const FPieceMenuRow> Rows = Controller->GetShownPieceMenuRows();

	const int32 DeleteRow = FindMenuRow(Rows, TEXT("Delete"));

	if (DeleteRow == INDEX_NONE)
	{
		Test->AddError(FString::Printf(
			TEXT("there is no Delete row to choose; the menu shows [%s]"), *DescribeMenuRows(Rows)));

		return true;
	}

	const bool bChose = Controller->ChoosePieceMenuRow(DeleteRow);

	Test->TestTrue(
		*FString::Printf(
			TEXT("choosing Delete must report that it committed; it reported %d"), bChose ? 1 : 0),
		bChose);

	const FStructureBinding* const Binding = FindBuild(Controller->GetWorld(), Record.StructureId);

	if (Binding == nullptr)
	{
		Test->AddError(TEXT("the build structure vanished under the delete"));
		return true;
	}

	Test->TestTrue(
		*FString::Printf(
			TEXT("THE INSPECTED BRICK MUST BE GONE: IsPieceRemoved(%d) reports %d"),
			InspectedPieceIndex, Binding->IsPieceRemoved(InspectedPieceIndex) ? 1 : 0),
		Binding->IsPieceRemoved(InspectedPieceIndex));

	// The cascade releases condemned pieces rather than removing them, so exactly one is removed.
	Test->TestEqual(
		FString::Printf(
			TEXT("and only that one: five of the six pieces must still be live; %d are"),
			Binding->GetStructure().NumLivePieces()),
		Binding->GetStructure().NumLivePieces(), 5);

	int32 Released = 0;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		Released += Binding->IsReleased(Piece) ? 1 : 0;
	}

	Test->AddInfo(FString::Printf(
		TEXT("the delete's own solve released %d of the %d remaining piece(s) to physics"),
		Released, Binding->GetStructure().NumLivePieces()));

	return true;
}

/** Frame 3: the hole where the inspected brick was, and whatever settled. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShootDeletedCommand, FAutomationTestBase*, Test);

bool FSessionShootDeletedCommand::Update()
{
	using namespace SessionScreenshotSupport;

	if (!SessionShotRecord().bStaged)
	{
		return true;
	}

	CheckStageBeforeShot(
		*Test, TEXT("frame 3 (deleted)"), DestructionSession::ESessionMode::Destroy,
		/*ExpectedPieces*/ 6, /*bExpectMenu*/ false, /*bExpectGhostVisible*/ false);

	RequestScreenshot(*Test, ShotCommandFor(FString(DeletedBaseName)));

	return true;
}

/**
 * Turn the load overlay on and check each standing brick wears
 * BrickHighlightForLoadBand(WorstJointBandForPiece(...)), composed from production rather than pinned,
 * since the band is the solver's answer. Released pieces are skipped; that rule is
 * LoadOverlayIgnoresReleasedPieces' to test.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotLoadOverlayCommand, FAutomationTestBase*, Test);

bool FSessionShotLoadOverlayCommand::Update()
{
	using namespace DestructionSession;
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();

	if (!Record.bStaged)
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller = Record.Controller.Get();

	if (Controller == nullptr)
	{
		Test->AddError(TEXT("the controller vanished before the load overlay"));
		return true;
	}

	Test->TestTrue(
		TEXT("the Load overlay chip must be live over the player's settled build"),
		ButtonIsEnabled(Controller->GetSessionToolbarState(), EToolbarButtonId::ToggleLoadOverlay));

	const bool bToggled = Controller->OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay);

	Test->TestTrue(
		*FString::Printf(
			TEXT("clicking Load overlay must report that it landed; it reported %d"), bToggled ? 1 : 0),
		bToggled);

	Test->TestTrue(
		TEXT("and the session must record it as on, because the chip in this frame is drawn lit from "
			 "that flag"),
		Controller->GetSessionToolbarState().bLoadOverlay);

	const FStructureBinding* const Binding = FindBuild(Controller->GetWorld(), Record.StructureId);

	if (Binding == nullptr)
	{
		Test->AddError(TEXT("the build structure vanished before the overlay frame"));
		return true;
	}

	int32 Tinted = 0;
	int32 Standing = 0;

	FString Line;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		if (Binding->IsPieceRemoved(Piece) || Binding->IsReleased(Piece))
		{
			continue;
		}

		++Standing;

		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

		if (Brick == nullptr)
		{
			continue;
		}

		const EJointMarginBand Band = WorstJointBandForPiece(Binding->GetStructure(), Piece);
		const EBrickHighlight Expected = BrickHighlightForLoadBand(Band);
		const EBrickHighlight Worn = Brick->GetHighlight();

		Tinted += Worn == EBrickHighlight::LoadComfortable
			|| Worn == EBrickHighlight::LoadCaution
			|| Worn == EBrickHighlight::LoadCritical ? 1 : 0;

		Line += FString::Printf(
			TEXT("%s%d:%d"), Line.IsEmpty() ? TEXT("") : TEXT(", "), Piece, static_cast<int32>(Worn));

		Test->TestEqual(
			FString::Printf(
				TEXT("piece %d must wear the state its worst joint's band maps to (%d); it wears %d"),
				Piece, static_cast<int32>(Expected), static_cast<int32>(Worn)),
			static_cast<int32>(Worn), static_cast<int32>(Expected));
	}

	Test->AddInfo(FString::Printf(
		TEXT("with the overlay on, %d of the %d standing pieces wear a load state — the wall reads [%s]"),
		Tinted, Standing, *Line));

	Test->TestTrue(
		*FString::Printf(
			TEXT("AND SOMETHING MUST ACTUALLY BE COLOURED, or this is a photograph of an untinted wall "
				 "agreeing with an overlay that computes nothing. %d of %d"),
			Tinted, Standing),
		Tinted >= 1);

	return true;
}

/** Frame 4: the settled wall tinted by load, with the chip lit. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShootLoadOverlayCommand, FAutomationTestBase*, Test);

bool FSessionShootLoadOverlayCommand::Update()
{
	using namespace SessionScreenshotSupport;

	if (!SessionShotRecord().bStaged)
	{
		return true;
	}

	CheckStageBeforeShot(
		*Test, TEXT("frame 4 (load overlay)"), DestructionSession::ESessionMode::Destroy,
		/*ExpectedPieces*/ 6, /*bExpectMenu*/ false, /*bExpectGhostVisible*/ false);

	RequestScreenshot(*Test, ShotCommandFor(FString(LoadOverlayBaseName)));

	return true;
}

/**
 * Turn the overlay off again. Asserts no load state anywhere rather than every brick None, because
 * hover and selection outrank the overlay (World.Session.LoadOverlayYieldsToHover).
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotLoadOverlayOffCommand, FAutomationTestBase*, Test);

bool FSessionShotLoadOverlayOffCommand::Update()
{
	using namespace DestructionSession;
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();

	if (!Record.bStaged)
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller = Record.Controller.Get();

	if (Controller == nullptr)
	{
		Test->AddError(TEXT("the controller vanished before the overlay was taken off"));
		return true;
	}

	Test->TestTrue(
		TEXT("the second click on the chip must land too"),
		Controller->OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay));

	Test->TestFalse(
		TEXT("and the session must record the overlay as off"),
		Controller->GetSessionToolbarState().bLoadOverlay);

	const FStructureBinding* const Binding = FindBuild(Controller->GetWorld(), Record.StructureId);

	if (Binding == nullptr)
	{
		Test->AddError(TEXT("the build structure vanished under the overlay toggle"));
		return true;
	}

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		if (Binding->IsPieceRemoved(Piece))
		{
			continue;
		}

		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

		if (Brick == nullptr)
		{
			continue;
		}

		const EBrickHighlight Worn = Brick->GetHighlight();

		Test->TestFalse(
			*FString::Printf(
				TEXT("NO BRICK MAY STILL BE WEARING A BAND IN THE FRAMES THAT FOLLOW: piece %d wears %d. A "
					 "tint left behind would change frame 5 into a picture of a state no click produced"),
				Piece, static_cast<int32>(Worn)),
			Worn == EBrickHighlight::LoadComfortable
				|| Worn == EBrickHighlight::LoadCaution
				|| Worn == EBrickHighlight::LoadCritical);
	}

	return true;
}

/**
 * Lay a Free brick in mid-air, then Run. A piece that cannot stand is needed: over a standing build,
 * Run releases nothing, which is indistinguishable from a Run wired to nothing.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotRunCommand, FAutomationTestBase*, Test);

bool FSessionShotRunCommand::Update()
{
	using namespace DestructionSession;
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();

	if (!Record.bStaged)
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller = Record.Controller.Get();

	if (Controller == nullptr)
	{
		Test->AddError(TEXT("the controller vanished before the run"));
		return true;
	}

	Test->TestTrue(
		TEXT("the Build tab is always live"),
		Controller->OnToolbarButton(EToolbarButtonId::ModeBuild));

	Test->TestTrue(
		TEXT("and so is the Brick chip — the floating piece must be a brick, not the plate the wall "
			 "was topped with"),
		Controller->OnToolbarButton(EToolbarButtonId::PieceBrick));

	Test->TestTrue(
		TEXT("Free placement is always live in Build mode"),
		Controller->OnToolbarButton(EToolbarButtonId::PlacementFree));

	for (int32 Step = 2; Step < FloatingCourse; ++Step)
	{
		Test->TestTrue(
			*FString::Printf(TEXT("Course up from %d must land"), Step),
			Controller->OnToolbarButton(EToolbarButtonId::CourseUp));
	}

	Test->TestEqual(
		FString::Printf(
			TEXT("fixture: the session must be on course %d; it reads %d"),
			FloatingCourse, Controller->GetSessionToolbarState().Course),
		Controller->GetSessionToolbarState().Course, FloatingCourse);

	const FStructureBinding* const Before = FindBuild(Controller->GetWorld(), Record.StructureId);

	if (Before == nullptr)
	{
		Test->AddError(TEXT("the build structure vanished before the free placement"));
		return true;
	}

	const int32 ConnectionsBefore = Before->GetStructure().NumConnections();

	Record.FloatingPieceIndex = Before->NumPieces();

	const bool bPlaced = Controller->PrimaryAlongRay(
		LayRayStart(FloatingCursorXCm, FloatingPlaneZCm),
		LayRayEnd(FloatingCursorXCm, FloatingPlaneZCm));

	Test->TestTrue(
		*FString::Printf(
			TEXT("a Free click at x = %g on course %d must lay a brick in mid-air; it reported %d"),
			FloatingCursorXCm, FloatingCourse, bPlaced ? 1 : 0),
		bPlaced);

	const FStructureBinding* const Binding = FindBuild(Controller->GetWorld(), Record.StructureId);

	if (Binding == nullptr || Binding->NumPieces() != Record.FloatingPieceIndex + 1)
	{
		Test->AddError(FString::Printf(
			TEXT("the free placement did not produce piece %d"), Record.FloatingPieceIndex));

		return true;
	}

	const FVector FloatingCentreCm = Binding->GetBinding(Record.FloatingPieceIndex).Box.CentreCm;

	if (const AActor* const Actor = Cast<AActor>(Binding->GetActor(Record.FloatingPieceIndex)))
	{
		Record.FloatingActorAtReleaseCm = Actor->GetActorLocation();
	}

	// Fixture: a grounded or bonded brick would stand, and Run would prove nothing.
	Test->TestTrue(
		*FString::Printf(
			TEXT("fixture: the Free brick's underside is at %g cm, so it must NOT read grounded, or "
				 "nothing in this build can fall"),
			FloatingPlaneZCm - HalfBrickCm.Z),
		!Binding->GetStructure().GetPiece(Record.FloatingPieceIndex).bIsGrounded);

	Test->TestEqual(
		FString::Printf(
			TEXT("fixture: and it must form NO joint — the build held %d connection(s) before it and "
				 "holds %d after"),
			ConnectionsBefore, Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), ConnectionsBefore);

	Test->TestTrue(
		*FString::Printf(
			TEXT("fixture: and nothing may have let go of it before Run; IsReleased(%d) reports %d"),
			Record.FloatingPieceIndex, Binding->IsReleased(Record.FloatingPieceIndex) ? 1 : 0),
		!Binding->IsReleased(Record.FloatingPieceIndex));

	Test->AddInfo(FString::Printf(
		TEXT("the free brick's box is centred at (%.2f, %.2f, %.2f) — its actor stands at "
			 "(%.2f, %.2f, %.2f) — with %d connection(s) in the build"),
		FloatingCentreCm.X, FloatingCentreCm.Y, FloatingCentreCm.Z,
		Record.FloatingActorAtReleaseCm.X, Record.FloatingActorAtReleaseCm.Y,
		Record.FloatingActorAtReleaseCm.Z, Binding->GetStructure().NumConnections()));

	/* --- Run it from Destroy mode ------------------------------------------------------------- */

	Test->TestTrue(
		TEXT("the Destroy tab is always live"),
		Controller->OnToolbarButton(EToolbarButtonId::ModeDestroy));

	const bool bRan = Controller->OnToolbarButton(EToolbarButtonId::RunStructure);

	Test->TestTrue(
		*FString::Printf(
			TEXT("clicking Run structure on a live build must report that it landed; it reported %d"),
			bRan ? 1 : 0),
		bRan);

	const FStructureBinding* const After = FindBuild(Controller->GetWorld(), Record.StructureId);

	if (After == nullptr)
	{
		Test->AddError(TEXT("the build structure vanished under Run"));
		return true;
	}

	// IsReleased, never displacement (DESIGN §4).
	Test->TestTrue(
		*FString::Printf(
			TEXT("RUN MUST SETTLE THE BUILD: the brick with nothing under it has to be handed to "
				 "physics. IsReleased(%d) reports %d"),
			Record.FloatingPieceIndex, After->IsReleased(Record.FloatingPieceIndex) ? 1 : 0),
		After->IsReleased(Record.FloatingPieceIndex));

	return true;
}

/** Frame 5: the freed brick after three seconds. Its travel is reported, not asserted. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShootRunCommand, FAutomationTestBase*, Test);

bool FSessionShootRunCommand::Update()
{
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();

	if (!Record.bStaged)
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller = Record.Controller.Get();

	if (Controller != nullptr && Record.FloatingPieceIndex != INDEX_NONE)
	{
		if (const FStructureBinding* const Binding =
				FindBuild(Controller->GetWorld(), Record.StructureId))
		{
			if (const AActor* const Actor = Cast<AActor>(Binding->GetActor(Record.FloatingPieceIndex)))
			{
				Test->AddInfo(FString::Printf(
					TEXT("%d frames after Run the freed brick's actor is at (%.2f, %.2f, %.2f), having "
						 "travelled %.2f cm from (%.2f, %.2f, %.2f) — it was released with its underside "
						 "%.2f cm above the earth"),
					FallFrames,
					Actor->GetActorLocation().X, Actor->GetActorLocation().Y,
					Actor->GetActorLocation().Z,
					FVector::Dist(Actor->GetActorLocation(), Record.FloatingActorAtReleaseCm),
					Record.FloatingActorAtReleaseCm.X, Record.FloatingActorAtReleaseCm.Y,
					Record.FloatingActorAtReleaseCm.Z,
					FloatingPlaneZCm - HalfBrickCm.Z));
			}
		}
	}

	CheckStageBeforeShot(
		*Test, TEXT("frame 5 (run)"), DestructionSession::ESessionMode::Destroy,
		/*ExpectedPieces*/ 7, /*bExpectMenu*/ false, /*bExpectGhostVisible*/ false);

	RequestScreenshot(*Test, ShotCommandFor(FString(RunBaseName)));

	return true;
}

/**
 * Frame 6's build: clear the plot, reframe, and lay an L-wall with the Rotate chip. Players can only
 * turn a piece through that chip, so a probe click gates the build with one named error if it is
 * missing.
 *
 * Asserts CornerWallStands' readings through the binding: 11 pieces, 19 connections, 12 mortar
 * (10 beds, 2 quoins) and 7 perpends, then every piece Grounded or Supported. The profile mix is what
 * distinguishes a real corner from one whose quoins came back as perpends.
 *
 * Uses SolveLoads, which is non-destructive; SolveAndBreak would settle the wall before the shot.
 * The 3D flag is set by BeginBuild, and SolveLoads does not read it anyway.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotCornerCommand, FAutomationTestBase*, Test);

bool FSessionShotCornerCommand::Update()
{
	using namespace DestructionSession;
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();

	if (!Record.bStaged)
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller = Record.Controller.Get();

	if (Controller == nullptr)
	{
		Test->AddError(TEXT("the controller vanished before the corner build"));
		return true;
	}

	Test->TestTrue(
		TEXT("the Build tab is always live"),
		Controller->OnToolbarButton(EToolbarButtonId::ModeBuild));

	/* --- gate: is there a rotate chip? ------------------------------------------------------- */

	if (!Controller->OnToolbarButton(EToolbarButtonId::RotatePiece))
	{
		Test->AddError(
			TEXT("CR-2b: OnToolbarButton(RotatePiece) was REFUSED, so there is no way for a player to "
				 "turn a piece and no L-wall can be laid. The model refuses a button it does not draw, "
				 "which is exactly what a missing chip looks like from here. Frame 6 will be a picture "
				 "of an empty plot until the chip exists"));

		return true;
	}

	/* Toggle back, so the table below starts from upright. */
	Test->TestTrue(
		TEXT("and the same chip must turn it off again — a setting the player cannot unset is not a "
			 "setting"),
		Controller->OnToolbarButton(EToolbarButtonId::RotatePiece));

	Test->TestFalse(
		*FString::Printf(
			TEXT("the corner build must start from an UPRIGHT session; it reads %s"),
			*StateBits(Controller->GetSessionToolbarState())),
		Controller->GetSessionToolbarState().bRotated);

	/* --- a fresh plot ------------------------------------------------------------------------ */

	Test->TestTrue(
		TEXT("Clear build must be live over the settled plot"),
		Controller->OnToolbarButton(EToolbarButtonId::ClearBuild));

	Test->TestTrue(
		TEXT("Snap placement is always live in Build mode — frame 5 left the session on Free, and an "
			 "L laid Free would form no joint at all"),
		Controller->OnToolbarButton(EToolbarButtonId::PlacementSnap));

	Test->TestTrue(
		TEXT("and the Brick chip, because frame 5's brick is the piece this L is built from"),
		Controller->OnToolbarButton(EToolbarButtonId::PieceBrick));

	/* Frame 5 climbed to course 5; the L starts on the earth. */
	while (Controller->GetSessionToolbarState().Course > 0)
	{
		const int32 Before = Controller->GetSessionToolbarState().Course;

		Test->TestTrue(
			*FString::Printf(TEXT("Course down from %d must land"), Before),
			Controller->OnToolbarButton(EToolbarButtonId::CourseDown));

		if (Controller->GetSessionToolbarState().Course >= Before)
		{
			Test->AddError(TEXT("Course down did not lower the course; refusing to loop"));
			break;
		}
	}

	Test->TestEqual(
		FString::Printf(
			TEXT("fixture: the L starts on the grounded course; the session reads %d"),
			Controller->GetSessionToolbarState().Course),
		Controller->GetSessionToolbarState().Course, 0);

	/* --- reframe for this frame -------------------------------------------------------------- */

	if (APawn* const Pawn = Controller->GetPawn())
	{
		const FBox CornerCm = CornerStageBoundsCm();

		const DestructionScenarios::FViewpoint Viewpoint = DestructionScenarios::ViewpointFor(
			CornerCm, FrameAspectHeightOverWidth, DestructionScenarios::EScenarioFraming::ThreeQuarter);

		Pawn->SetActorLocation(Viewpoint.LocationCm);
		Controller->SetControlRotation(Viewpoint.Rotation);

		Test->AddInfo(FString::Printf(
			TEXT("frame 6 reframes over the L's %.2f x %.2f x %.2f cm bounds: camera at "
				 "(%.2f, %.2f, %.2f), rotation (%.2f, %.2f, %.2f)"),
			2.0 * CornerCm.GetExtent().X, 2.0 * CornerCm.GetExtent().Y, 2.0 * CornerCm.GetExtent().Z,
			Viewpoint.LocationCm.X, Viewpoint.LocationCm.Y, Viewpoint.LocationCm.Z,
			Viewpoint.Rotation.Pitch, Viewpoint.Rotation.Yaw, Viewpoint.Rotation.Roll));
	}
	else
	{
		Test->AddError(TEXT("the player controller has no pawn to reframe the corner on"));
	}

	/* --- the eleven clicks ------------------------------------------------------------------- */

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(CornerSteps); ++Index)
	{
		const FCornerStep& Step = CornerSteps[Index];

		while (Controller->GetSessionToolbarState().Course < Step.Course)
		{
			Test->TestTrue(
				*FString::Printf(TEXT("step %d: Course up must land"), Index),
				Controller->OnToolbarButton(EToolbarButtonId::CourseUp));
		}

		if (Controller->GetSessionToolbarState().bRotated != Step.bRotated)
		{
			Test->TestTrue(
				*FString::Printf(
					TEXT("step %d: the rotate chip must land — the L needs it five times, once at each "
						 "change of direction"),
					Index),
				Controller->OnToolbarButton(EToolbarButtonId::RotatePiece));

			Test->TestEqual(
				*FString::Printf(
					TEXT("step %d: and the session must read %s after it"),
					Index, Step.bRotated ? TEXT("ROTATED") : TEXT("upright")),
				Controller->GetSessionToolbarState().bRotated, Step.bRotated);
		}

		const double PlaneZCm = Step.Course == 0 ? BrickPlaneCourse0Cm : BrickPlaneCourse1Cm;

		const bool bPlaced = Controller->PrimaryAlongRay(
			LayRayStartXY(Step.XCm, Step.YCm, PlaneZCm),
			LayRayEndXY(Step.XCm, Step.YCm, PlaneZCm));

		Test->TestTrue(
			*FString::Printf(
				TEXT("step %d: a click at (%g, %g) on course %d, %s, must lay a brick; it reported %d"),
				Index, Step.XCm, Step.YCm, Step.Course,
				Step.bRotated ? TEXT("rotated") : TEXT("upright"), bPlaced ? 1 : 0),
			bPlaced);

		if (Index == 0)
		{
			/* The cleared plot opened a new binding; the first brick gives it an id. */
			Record.StructureId = Controller->GetSessionStructureId();

			Test->TestTrue(
				*FString::Printf(
					TEXT("the first brick of the L must give the session a structure to name; it names "
						 "%d"),
					Record.StructureId),
				Record.StructureId != INDEX_NONE);
		}
	}

	/* --- what the L is, before the shot ------------------------------------------------------ */

	FStructureBinding* const Binding = FindBuild(Controller->GetWorld(), Record.StructureId);

	if (Binding == nullptr)
	{
		Test->AddError(FString::Printf(
			TEXT("the L-wall's structure %d vanished while it was being laid"), Record.StructureId));

		return true;
	}

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const FVector CentreCm = Binding->GetBinding(Piece).Box.CentreCm;
		const FVector ExtentCm = Binding->GetBinding(Piece).Box.ExtentCm;

		Test->AddInfo(FString::Printf(
			TEXT("L piece %d laid at (%.3f, %.3f, %.3f), half extent (%.3f, %.3f, %.3f), grounded %d"),
			Piece, CentreCm.X, CentreCm.Y, CentreCm.Z, ExtentCm.X, ExtentCm.Y, ExtentCm.Z,
			Binding->GetStructure().GetPiece(Piece).bIsGrounded ? 1 : 0));
	}

	Test->TestEqual(
		FString::Printf(
			TEXT("eleven clicks must give eleven pieces; the L holds %d"), Binding->NumPieces()),
		Binding->NumPieces(), CornerExpectedPieces);

	Test->TestEqual(
		FString::Printf(
			TEXT("AND NINETEEN CONNECTIONS: 10 beds, 2 quoins and 7 heads. Eleven boxes standing in an "
				 "L with no joints between them are the same photograph and a different building. It "
				 "holds %d"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), CornerExpectedConnections);

	/*
	 * The profile mix. A quoin is full mortar across a horizontal normal, the contact pre-CR-2a
	 * inference called a perpend. Counted both ways so errors cannot cancel.
	 */
	{
		const auto Matches = [](const FConnectionStrength& Got, const FConnectionStrength& Want)
		{
			return Got.CompressiveStrengthMPa == Want.CompressiveStrengthMPa
				&& Got.ShearCohesionMPa == Want.ShearCohesionMPa
				&& Got.TensileStrengthMPa == Want.TensileStrengthMPa
				&& Got.FrictionCoefficient == Want.FrictionCoefficient
				&& Got.MaxShearStrengthMPa == Want.MaxShearStrengthMPa;
		};

		int32 Mortar = 0;
		int32 Perpend = 0;
		int32 Unrecognised = 0;

		for (int32 Index = 0; Index < Binding->GetStructure().NumConnections(); ++Index)
		{
			const FConnectionStrength& S = Binding->GetStructure().GetConnection(Index).Strength;

			if (Matches(S, DestructionProfiles::GeneralPurposeMortar))
			{
				++Mortar;
			}
			else if (Matches(S, DestructionProfiles::GeneralPurposeMortarPerpend))
			{
				++Perpend;
			}
			else
			{
				++Unrecognised;
			}
		}

		Test->TestEqual(
			FString::Printf(
				TEXT("TWELVE BONDED JOINTS — ten beds and TWO QUOINS. A corner whose quoins came back "
					 "as perpends is a wall the player cannot tell apart in this frame and that comes "
					 "down under a push. %d did"),
				Mortar),
			Mortar, CornerExpectedMortar);

		Test->TestEqual(
			FString::Printf(
				TEXT("and seven WEAK perpend heads — the control for the claim above: same materials, "
					 "same vertical faces, and the only difference is that those two bricks run the "
					 "same way. %d did"),
				Perpend),
			Perpend, CornerExpectedPerpend);

		Test->TestEqual(
			FString::Printf(
				TEXT("and nothing carrying a profile this build did not choose; %d did"), Unrecognised),
			Unrecognised, 0);
	}

	/*
	 * Exact support per piece: the six on the earth read Grounded, the five above Supported through
	 * their beds. A missing bed reads Falling or Stranded.
	 */
	Binding->SolveLoads();

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const EPieceSupport Want = Piece <= CornerLastGroundedPiece
			? EPieceSupport::Grounded
			: EPieceSupport::Supported;

		Test->TestEqual(
			*FString::Printf(
				TEXT("L piece %d must read %s after the solve; it reads %d"),
				Piece,
				Piece <= CornerLastGroundedPiece ? TEXT("Grounded") : TEXT("Supported"),
				static_cast<int32>(Binding->GetStructure().GetPieceSupport(Piece))),
			static_cast<int32>(Binding->GetStructure().GetPieceSupport(Piece)),
			static_cast<int32>(Want));
	}

	/* --- the rotated ghost at the next Y-leg brick ------------------------------------------- */

	Controller->PointerAlongRay(
		LayRayStartXY(CornerGhostXCm, CornerGhostYCm, BrickPlaneCourse1Cm),
		LayRayEndXY(CornerGhostXCm, CornerGhostYCm, BrickPlaneCourse1Cm));

	Test->TestTrue(
		*FString::Printf(
			TEXT("the frame's ghost must be a ROTATED one — a header-on brick beside three stretchers "
				 "is the whole picture. The session reads %s"),
			*StateBits(Controller->GetSessionToolbarState())),
		Controller->GetSessionToolbarState().bRotated);

	if (const UBuildModeComponent* const Build = Controller->GetBuildComponent())
	{
		if (const AActor* const Ghost = Build->GetGhostActor())
		{
			Test->AddInfo(FString::Printf(
				TEXT("the corner ghost stands at (%.2f, %.2f, %.2f), hidden %d"),
				Ghost->GetActorLocation().X, Ghost->GetActorLocation().Y, Ghost->GetActorLocation().Z,
				Ghost->IsHidden() ? 1 : 0));
		}
	}

	return true;
}

/** Frame 6: the L-wall with a rotated ghost at the end of its second leg. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShootCornerCommand, FAutomationTestBase*, Test);

bool FSessionShootCornerCommand::Update()
{
	using namespace SessionScreenshotSupport;

	if (!SessionShotRecord().bStaged)
	{
		return true;
	}

	CheckStageBeforeShot(
		*Test, TEXT("frame 6 (corner)"), DestructionSession::ESessionMode::Build,
		/*ExpectedPieces*/ CornerExpectedPieces, /*bExpectMenu*/ false,
		/*bExpectGhostVisible*/ true);

	RequestScreenshot(*Test, ShotCommandFor(FString(CornerBaseName)));

	return true;
}

/**
 * Frame 7's stage: clear the plot, lay one seed brick, and show a ghost without clicking (the owner's
 * 2026-09-16 playtest ask).
 *
 * The ghost is driven through RefreshBuildPreviewFromRay, the testable half of the per-tick cursor
 * refresh (deprojection needs a viewport). PointerAlongRay would exercise the old path instead. No
 * PrimaryAlongRay after the seed, so the piece count is asserted unchanged at the end.
 *
 * Rotate is then pressed with the ghost standing, and its bounds size is re-read to prove the turn
 * reached it. The rotated centre is reported, not asserted; Core.BuildMode pins it.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotGhostCommand, FAutomationTestBase*, Test);

bool FSessionShotGhostCommand::Update()
{
	using namespace DestructionSession;
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();

	if (!Record.bStaged)
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller = Record.Controller.Get();

	if (Controller == nullptr)
	{
		Test->AddError(TEXT("the controller vanished before the ghost frame"));
		return true;
	}

	Test->TestTrue(
		TEXT("the Build tab is always live"),
		Controller->OnToolbarButton(EToolbarButtonId::ModeBuild));

	/* --- a fresh plot ------------------------------------------------------------------------ */

	Test->TestTrue(
		TEXT("Clear build must be live over the L"),
		Controller->OnToolbarButton(EToolbarButtonId::ClearBuild));

	Test->TestTrue(
		TEXT("and the Brick chip, because the seed and the ghost are both bricks"),
		Controller->OnToolbarButton(EToolbarButtonId::PieceBrick));

	Test->TestTrue(
		TEXT("and Snap placement, or the ghost would sit at the cursor verbatim and prove nothing "
			 "about a solved pose"),
		Controller->OnToolbarButton(EToolbarButtonId::PlacementSnap));

	/* Frame 6 ends on course 1, rotated. The seed goes down upright, on the earth. */
	while (Controller->GetSessionToolbarState().Course > 0)
	{
		const int32 Before = Controller->GetSessionToolbarState().Course;

		Test->TestTrue(
			*FString::Printf(TEXT("Course down from %d must land"), Before),
			Controller->OnToolbarButton(EToolbarButtonId::CourseDown));

		if (Controller->GetSessionToolbarState().Course >= Before)
		{
			Test->AddError(TEXT("Course down did not lower the course; refusing to loop"));
			break;
		}
	}

	if (Controller->GetSessionToolbarState().bRotated)
	{
		Test->TestTrue(
			TEXT("the rotate chip must land — frame 6 left the session turned and the seed is a "
				 "stretcher"),
			Controller->OnToolbarButton(EToolbarButtonId::RotatePiece));
	}

	Test->TestFalse(
		*FString::Printf(
			TEXT("fixture: the seed must be laid UPRIGHT, so the turn below is a change and not the "
				 "state it started in; the session reads %s"),
			*StateBits(Controller->GetSessionToolbarState())),
		Controller->GetSessionToolbarState().bRotated);

	Test->TestEqual(
		FString::Printf(
			TEXT("fixture: the seed goes on the grounded course; the session reads %d"),
			Controller->GetSessionToolbarState().Course),
		Controller->GetSessionToolbarState().Course, 0);

	/* --- reframe for this frame -------------------------------------------------------------- */

	if (APawn* const Pawn = Controller->GetPawn())
	{
		const FBox GhostCm = GhostStageBoundsCm();

		const DestructionScenarios::FViewpoint Viewpoint = DestructionScenarios::ViewpointFor(
			GhostCm, FrameAspectHeightOverWidth, DestructionScenarios::EScenarioFraming::ThreeQuarter);

		Pawn->SetActorLocation(Viewpoint.LocationCm);
		Controller->SetControlRotation(Viewpoint.Rotation);

		Test->AddInfo(FString::Printf(
			TEXT("frame 7 reframes over the seed-and-ghost %.2f x %.2f x %.2f cm box: camera at "
				 "(%.2f, %.2f, %.2f), rotation (%.2f, %.2f, %.2f)"),
			2.0 * GhostCm.GetExtent().X, 2.0 * GhostCm.GetExtent().Y, 2.0 * GhostCm.GetExtent().Z,
			Viewpoint.LocationCm.X, Viewpoint.LocationCm.Y, Viewpoint.LocationCm.Z,
			Viewpoint.Rotation.Pitch, Viewpoint.Rotation.Yaw, Viewpoint.Rotation.Roll));
	}
	else
	{
		Test->AddError(TEXT("the player controller has no pawn to reframe the ghost frame on"));
	}

	/* --- 1: the seed brick, the only click in this frame ------------------------------------- */

	{
		const bool bPlaced = Controller->PrimaryAlongRay(
			LayRayStart(GhostSeedCursorXCm, BrickPlaneCourse0Cm),
			LayRayEnd(GhostSeedCursorXCm, BrickPlaneCourse0Cm));

		Test->TestTrue(
			*FString::Printf(
				TEXT("the seed brick must land at x = %g on course 0; the click reported %d"),
				GhostSeedCursorXCm, bPlaced ? 1 : 0),
			bPlaced);
	}

	Record.StructureId = Controller->GetSessionStructureId();

	Test->TestTrue(
		*FString::Printf(
			TEXT("the cleared plot opened a new binding and the seed must name it; it names %d"),
			Record.StructureId),
		Record.StructureId != INDEX_NONE);

	const FStructureBinding* const Binding = FindBuild(Controller->GetWorld(), Record.StructureId);

	if (Binding == nullptr)
	{
		Test->AddError(FString::Printf(
			TEXT("the ghost frame's structure %d vanished under the seed"), Record.StructureId));

		return true;
	}

	Test->TestEqual(
		FString::Printf(
			TEXT("fixture: one click, one piece; the plot holds %d"), Binding->NumPieces()),
		Binding->NumPieces(), 1);

	const UBuildModeComponent* const Build = Controller->GetBuildComponent();

	if (Build == nullptr)
	{
		Test->AddError(TEXT("the controller has no build component, so there is no ghost to photograph"));
		return true;
	}

	/* --- 2: the cursor puts a ghost up with no click ----------------------------------------- */

	{
		const bool bRefreshed = Controller->RefreshBuildPreviewFromRay(
			GhostRayOriginCm(), GhostRayDirection());

		Test->TestTrue(
			*FString::Printf(
				TEXT("A CURSOR REFRESH IN BUILD MODE MUST PUT A GHOST UP WITH NO CLICK BEHIND IT — that "
					 "is the owner's ask and the whole subject of this frame. It reported %d"),
				bRefreshed ? 1 : 0),
			bRefreshed);

		const AActor* const Ghost = Build->GetGhostActor();

		Test->TestNotNull(TEXT("and there must be a ghost actor to photograph"), Ghost);

		if (Ghost == nullptr)
		{
			return true;
		}

		Test->TestFalse(
			TEXT("and it must be VISIBLE before the chip is touched — a preview nobody can see is not a "
				 "preview"),
			Ghost->IsHidden());

		const FBox Bounds = Ghost->GetComponentsBoundingBox(/*bNonColliding*/ true);

		Test->AddInfo(FString::Printf(
			TEXT("the un-turned ghost's bounds are centred (%.3f, %.3f, %.3f), size (%.3f, %.3f, %.3f)"),
			Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z,
			Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z));

		Test->TestTrue(
			*FString::Printf(
				TEXT("fixture: and it must stand at the RUNNING-BOND snap beside the seed, (11.25, 0, "
					 "10.75) — the ray was dropped 3 cm off that line, so a ghost sitting at the cursor "
					 "would mean nothing solved it. It is at (%.3f, %.3f, %.3f)"),
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(UprightGhostCentreCm, GhostBoundsToleranceCm));

		Test->TestTrue(
			*FString::Printf(
				TEXT("fixture: and it must be an UPRIGHT brick, 21.5 x 10.25 x 6.5, so the turn below is "
					 "a change; it is (%.3f, %.3f, %.3f)"),
				Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z),
			Bounds.GetSize().Equals(UprightGhostSizeCm, GhostBoundsToleranceCm));
	}

	/* --- 3: Rotate turns the ghost where it stands ------------------------------------------- */

	{
		Test->TestTrue(
			TEXT("the rotate chip must land"),
			Controller->OnToolbarButton(EToolbarButtonId::RotatePiece));

		Test->TestTrue(
			*FString::Printf(
				TEXT("and the session must read ROTATED for the frame; it reads %s"),
				*StateBits(Controller->GetSessionToolbarState())),
			Controller->GetSessionToolbarState().bRotated);

		const AActor* const Ghost = Build->GetGhostActor();

		Test->TestNotNull(TEXT("the ghost must survive the chip"), Ghost);

		if (Ghost == nullptr)
		{
			return true;
		}

		Test->TestFalse(
			TEXT("THE GHOST MUST STILL BE VISIBLE AFTER THE CHIP — a setting that puts the preview out "
				 "reads as the click being eaten"),
			Ghost->IsHidden());

		const FBox Bounds = Ghost->GetComponentsBoundingBox(/*bNonColliding*/ true);

		Test->AddInfo(FString::Printf(
			TEXT("the turned ghost's bounds are centred (%.3f, %.3f, %.3f), size (%.3f, %.3f, %.3f)"),
			Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z,
			Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z));

		Test->TestTrue(
			*FString::Printf(
				TEXT("AND IT MUST BE THE TURNED FOOTPRINT, 10.25 x 21.5 x 6.5 — X and Y swapped, Z "
					 "untouched. A chip that lit while the ghost kept its old footprint is the owner's "
					 "'the click did nothing', and it is invisible in a photograph of a box. It is "
					 "(%.3f, %.3f, %.3f)"),
				Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z),
			Bounds.GetSize().Equals(RotatedGhostSizeCm, GhostBoundsToleranceCm));
	}

	/* --- 4: nothing was committed ------------------------------------------------------------ */

	{
		const FStructureBinding* const After = FindBuild(Controller->GetWorld(), Record.StructureId);

		if (After == nullptr)
		{
			Test->AddError(TEXT("the ghost frame's structure vanished under the preview"));
			return true;
		}

		Test->TestEqual(
			FString::Printf(
				TEXT("THE PLOT MUST STILL HOLD EXACTLY THE ONE SEED: a preview places nothing, and a "
					 "second brick here would make this a picture of a click rather than of a ghost. It "
					 "holds %d piece(s)"),
				After->NumPieces()),
			After->NumPieces(), 1);

		Test->TestEqual(
			FString::Printf(
				TEXT("and one LIVE piece with it, so nothing was laid and quietly removed either; %d are "
					 "live"),
				After->GetStructure().NumLivePieces()),
			After->GetStructure().NumLivePieces(), 1);
	}

	return true;
}

/** Frame 7: one seed brick and a turned ghost beside it, placed by no click. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShootGhostCommand, FAutomationTestBase*, Test);

bool FSessionShootGhostCommand::Update()
{
	using namespace SessionScreenshotSupport;

	if (!SessionShotRecord().bStaged)
	{
		return true;
	}

	CheckStageBeforeShot(
		*Test, TEXT("frame 7 (ghost)"), DestructionSession::ESessionMode::Build,
		/*ExpectedPieces*/ 1, /*bExpectMenu*/ false, /*bExpectGhostVisible*/ true);

	RequestScreenshot(*Test, ShotCommandFor(FString(GhostBaseName)));

	return true;
}

/** Clear the build through the toolbar, leaving the empty plot begin-play produced. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotTearDownCommand, FAutomationTestBase*, Test);

bool FSessionShotTearDownCommand::Update()
{
	using namespace DestructionSession;
	using namespace SessionScreenshotSupport;

	FSessionShotRecord& Record = SessionShotRecord();

	ADestructionGamePlayerController* const Controller = Record.Controller.Get();

	if (Controller != nullptr)
	{
		Controller->OnToolbarButton(EToolbarButtonId::ModeBuild);

		const bool bCleared = Controller->OnToolbarButton(EToolbarButtonId::ClearBuild);

		Test->AddInfo(FString::Printf(
			TEXT("cleared the plot through the toolbar (%d), leaving the session in %s mode on an empty "
				 "build"),
			bCleared ? 1 : 0, ModeName(Controller->GetSessionToolbarState().Mode)));
	}

	Record.Reset();

	return true;
}

/** All seven files landed and they are real PNGs. All seven were deleted before the run. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FSessionShotCheckFilesCommand, FAutomationTestBase*, Test);

bool FSessionShotCheckFilesCommand::Update()
{
	using namespace SessionScreenshotSupport;

	for (const TCHAR* const BaseName : ScreenshotBaseNames)
	{
		const FString Path = ScreenshotPathFor(FString(BaseName));

		const int64 SizeBytes = IFileManager::Get().FileSize(*Path);

		if (SizeBytes < 0)
		{
			Test->AddError(FString::Printf(
				TEXT("no screenshot was written to %s: the shot request never reached a draw, or the "
					 "file went somewhere else"),
				*Path));

			continue;
		}

		TArray<uint8> Bytes;

		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 24)
		{
			Test->AddError(FString::Printf(
				TEXT("the screenshot at %s could not be read back, or is too short to carry a PNG header "
					 "(%d bytes)"),
				*Path, Bytes.Num()));

			continue;
		}

		/*
		 * Signature and IHDR read by hand: 8 signature bytes, 4-byte length, "IHDR", then big-endian
		 * width and height. A decoder would depend on the rendering stack under test.
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
			BaseName, SizeBytes, Width, Height, *Path));

		Test->TestTrue(
			*FString::Printf(
				TEXT("%s.png must be a real frame of a lit scene with a UI over it — a flat colour is "
					 "well under 10 kB — so it must be at least %lld bytes; it is %lld"),
				BaseName, MinimumScreenshotBytes, SizeBytes),
			SizeBytes >= MinimumScreenshotBytes);

		Test->TestTrue(
			*FString::Printf(
				TEXT("%s.png must begin with the PNG signature and an IHDR chunk, and declare real "
					 "dimensions; it is %d x %d"),
				BaseName, Width, Height),
			bIsPng && Width >= MinimumScreenshotWidth && Height >= MinimumScreenshotHeight);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionScreenshotsTest,
	"DestructionGame.Visual.SessionScreenshots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::NonNullRHI
		| EAutomationTestFlags::ProductFilter)

bool FSessionScreenshotsTest::RunTest(const FString& Parameters)
{
	using namespace SessionScreenshotSupport;

	SessionShotRecord().Reset();

	// Delete old files first, so an existing file afterwards means this run wrote it.
	for (const TCHAR* const BaseName : ScreenshotBaseNames)
	{
		const FString Path = ScreenshotPathFor(FString(BaseName));

		if (IFileManager::Get().FileExists(*Path))
		{
			IFileManager::Get().Delete(
				*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		}

		if (IFileManager::Get().FileExists(*Path))
		{
			AddError(FString::Printf(
				TEXT("fixture: %s could not be deleted, so its existence afterwards would prove nothing"),
				*Path));

			return true;
		}
	}

	/*
	 * For each frame: set up, settle, shoot, wait for the write. The delete and Run are each followed
	 * by a fall wait and a settle.
	 */
	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(DisableScreenMessagesCommand));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotOpenStageCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WarmUpFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotBuildCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShootBuildCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotInspectCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShootDestroyCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotDeleteCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(FallFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShootDeletedCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotLoadOverlayCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShootLoadOverlayCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotLoadOverlayOffCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotRunCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(FallFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShootRunCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	// Frames 6 and 7 come last because each clears the plot.
	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotCornerCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShootCornerCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotGhostCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShootGhostCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotTearDownCommand(this));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotCheckFilesCommand(this));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
