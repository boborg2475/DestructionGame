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
 * THE INTERACTIVE SESSION, PHOTOGRAPHED: BUILD A WALL, INSPECT A BRICK, DELETE IT, RUN THE PLOT.
 *
 * =========================================================================================
 * WHY THIS FILE EXISTS
 * =========================================================================================
 *
 * `Tests/SessionControllerTest.cpp` proves the session's MECHANISM — one door into the state, the
 * mode dispatching a ray, Run releasing what cannot stand — and every one of its runs is `-nullrhi`,
 * so not one of them has ever caused a pixel to exist. CURRENT_STATE.md records the project shipping
 * a fully green suite over something invisible on screen twice (the Nanite bug, the commit-path push
 * bug), and the session's whole subject is a SCREEN: a toolbar strip, a gold ghost, a details window
 * over a brick. This is the committed proof that those four things are actually drawn, on the level a
 * player opens to build on.
 *
 * IT DRIVES THE REAL CONTROLLER THROUGH ITS OWN SEAMS, AND THAT IS THE POINT RATHER THAN A STYLE.
 * `World->GetFirstPlayerController()` cast to `ADestructionGamePlayerController` is the controller the
 * game mode opened the session on, and every action here is `OnToolbarButton`, `PointerAlongRay`,
 * `PrimaryAlongRay`, `SetInspectedPiece` or `ChoosePieceMenuRow` — the same calls the mouse handlers
 * and the strip's chips make. Nothing touches `UBuildModeComponent` or the subsystem to CHANGE
 * anything; they are read (the ghost's visibility, the binding's pieces) because a picture has to be
 * asserted to be a picture of the state it claims. A harness that drove the component directly would
 * photograph a loop the player cannot reach.
 *
 * =========================================================================================
 * THE SIX FRAMES
 * =========================================================================================
 *
 *   FRAME 1 "Session_Build": the plot as the level opens — Build mode, the strip up — with a small
 *   wall laid through `PrimaryAlongRay`: three bricks on course 0, two staggered onto them on course
 *   1, and a timber plate bearing across the top on course 2. THE PLATE IS SCREWED DOWN: the `Screw`
 *   joint chip is clicked before it is laid and put back to `Auto` afterwards, so the bricks keep
 *   their inferred mortar and the plate's two bearings carry the fastener the player picked — the
 *   UI-6 choice, committed. A gold GHOST hovers at the cursor, one plate-length past the wall. The
 *   strip reads Build lit, Plate lit, Auto lit again, Course 3 (index 2 — the readout is
 *   one-based), Clear build live.
 *
 *   FRAME 2 "Session_Destroy": Destroy mode (ghost gone), a real trace into the COURSE-1 BRICK UNDER
 *   THE PLATE, hovered then clicked, so the piece menu / details window is open on it with its
 *   Delete row and its joint readout, and the strip's Run structure is live. THAT BRICK IS THE ONE
 *   WORTH PHOTOGRAPHING BECAUSE ITS ROWS NAME THREE DIFFERENT PROFILES: two mortared beds under it,
 *   a perpend head joint beside it, and the SCREWED bearing the plate is fastened to it with — which
 *   is the only place in the game a player can see which joint they chose. THE READOUT SAYS "not
 *   solved yet" AND 0.0 N ON EVERY JOINT, AND THAT IS THE TRUTH RATHER THAN A BROKEN PICTURE: laying
 *   a piece never solves (the live-feedback-off default this build was laid under), so the numbers
 *   arrive only once something asks for them — Run, or a delete.
 *
 *   FRAME 3 "Session_Deleted": the Delete row chosen. That brick is gone and whatever the delete's
 *   own solve condemns has settled.
 *
 *   FRAME 4 "Session_LoadOverlay": the Destroy strip's `Load overlay` chip clicked ON over that same
 *   settled wall — every piece wearing the band of its worst joint, and the chip lit. TAKEN AFTER THE
 *   DELETE RATHER THAN BEFORE IT, because the delete is what SOLVES: laying never does, so the same
 *   click made one frame earlier would be photographing the overlay's own first solve rather than a
 *   settled wall's. The chip is clicked OFF again immediately afterwards, and that is asserted, so
 *   frame 5 is the picture it has always been.
 *
 *   FRAME 5 "Session_Run": back in Build for one FREE brick three courses up in mid-air with no
 *   joints, then Destroy and Run structure — the brick is released and has fallen to the ground.
 *
 *   FRAME 6 "Session_Corner" (CR-2b): the plot CLEARED and an L-WALL laid on it — a three-brick leg
 *   along X, a ROTATED brick returning at its end, a leg along Y, and a staggered course lapped over
 *   all three — every rotation made with the strip's own `Rotate` chip, with a rotated ghost left
 *   hovering at the end of the second leg. It is the only picture in this project of a building that
 *   turns a corner, and it exists because until that chip landed a player could not hand the game a
 *   turned box at all: CR-2a's whole corner vocabulary was code nobody could reach.
 *
 * THE CAMERA DOES NOT MOVE BETWEEN FRAMES 1 AND 5. It is placed ONCE, before the first frame, so the
 * five pictures can be laid side by side and read against each other in the same pixels. FRAME 6
 * REFRAMES, exactly once, and its own command says why: the L is a different building in a different
 * footprint — 61 cm along Y where the wall was 10 cm deep — and the frame-1 camera would show the
 * corner edge-on, which is the one thing a picture of a corner may not be.
 *
 * =========================================================================================
 * THE CAMERA IS THE GAME MODE'S FRAMING, MOVED IN ONCE
 * =========================================================================================
 *
 * `ADestructionGameGameMode::BeginPlay` frames the build plot on an INVENTED 3 m x 3 m x 1 m box,
 * because an empty plot has no bounds — which stands the player 484 cm off the origin and puts a
 * 66 cm wall across about 7% of the frame, far too small to judge a ghost's colour or a bearing joint
 * by. So the pawn is moved in ONCE, before frame 1, through the SAME production framing function the
 * game mode uses (`DestructionScenarios::ViewpointFor`, `ThreeQuarter`: azimuth 40 degrees, elevation
 * 30 degrees, margin 1.25) over the union of every pose that will appear in any of the four frames.
 * The angle a player sees the plot from is therefore unchanged; only the distance is.
 *
 * =========================================================================================
 * WHAT IT ASSERTS — DELIBERATELY MODEST, BECAUSE THE POINT IS THE IMAGE
 * =========================================================================================
 *
 * A harness that only takes a picture is green whatever is in the picture, so each frame asserts the
 * invariants that make it a picture of the thing it claims: the session's mode, the build's piece
 * count, whether a menu is up, whether the ghost is visible, and the one mechanism reading that frame
 * is about — 6 pieces and at least 8 connections for the laid wall, `IsPieceMenuShown` for the
 * inspector, `IsPieceRemoved` for the delete, `IsReleased` for the Run, and for the corner the
 * profile MIX (12 mortar, 7 perpend) plus every piece Grounded or Supported. NEVER DISPLACEMENT: the
 * fallen brick's travel is REPORTED so a human can read it beside the picture, and the claim that Run
 * did its job is `IsReleased`, exactly as `World.Session.RunStructureSettlesTheBuild` has it.
 *
 * It asserts NOTHING about what the images look like. Whether the strip is legible, whether the ghost
 * reads as gold against clay-red, whether the details window is over the right brick — that is a
 * human's job, and this exists to give that human something to look at.
 *
 * =========================================================================================
 * IT NEEDS A TICKING WORLD *AND* A REAL RHI, hence EAutomationTestFlags::NonNullRHI
 * =========================================================================================
 *
 * A ticking world, because the delete's cascade and the Run's release are handed to Chaos and the
 * frames are taken after they have fallen. A real RHI, because there is otherwise nothing to
 * screenshot: without the flag the ordinary `-nullrhi` suite would run this, find no viewport, write
 * no file and GO GREEN. With it, the ordinary suite never mentions this test exists, so it must be
 * RUN EXPLICITLY. From PowerShell (Git Bash mangles the map path):
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject"
 *     /Game/Maps/Scenarios/Lvl_Build
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.SessionScreenshots"
 *     -TestExit="Automation Test Queue Empty"
 *
 * `-nullrhi` MUST BE ABSENT: `FApp::CanEverRender()` is false with it and `UGameEngine::Init` only
 * builds a window and a viewport under that, so there would be nothing to screenshot even if the
 * filter let the test through. THE MAP MUST BE ON THE COMMAND LINE: `Lvl_Build` is what selects the
 * `build` catalogue row, which is what opens the session in Build mode on an empty plot.
 */
namespace SessionScreenshotSupport
{
	using namespace DestructionSession;

	/** The six frames' file base names, under FPaths::ScreenShotDir(). */
	const TCHAR* const BuildBaseName = TEXT("Session_Build");
	const TCHAR* const DestroyBaseName = TEXT("Session_Destroy");
	const TCHAR* const DeletedBaseName = TEXT("Session_Deleted");
	const TCHAR* const LoadOverlayBaseName = TEXT("Session_LoadOverlay");
	const TCHAR* const RunBaseName = TEXT("Session_Run");
	const TCHAR* const CornerBaseName = TEXT("Session_Corner");

	/**
	 * ALL SIX IN ONE LIST, because every claim made about one is made about the other five — the
	 * deletion before the run and the PNG check after it are the same two statements six times over,
	 * and a list is what stops the sixth shot quietly acquiring a weaker version of either.
	 */
	const TCHAR* const ScreenshotBaseNames[] = {
		BuildBaseName, DestroyBaseName, DeletedBaseName, LoadOverlayBaseName, RunBaseName,
		CornerBaseName };

	/**
	 * `Shot` and not `HighResShot`, and `showui` with it.
	 *
	 * HighResShot renders SCENE-ONLY through an FDummyViewport, and this whole test is about Slate:
	 * the toolbar strip and the piece menu are `AddViewportWidgetContent` widgets, so HighResShot
	 * would photograph the bricks with NO UI ON THEM — the one thing being proved would be the one
	 * thing missing. See Tests/PieceMenuScreenshotTest.cpp, which measured it.
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

	/** Debug overlays off, so nothing is burned over the UI a human is being asked to judge. */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/**
	 * THE TIMINGS, WHICH ARE THE SIBLING HARNESSES'.
	 *
	 * `WarmUpFrames` and `SettleFrames` cover TSR's temporal history and auto-exposure; `SlateFrames`
	 * is the layout pass a rebuilt strip or a newly built menu panel needs before it has a size and a
	 * position; `WriteFrames` is because `ProcessScreenShots` writes at END OF DRAW, so moving on in
	 * the same frame as the request loses the file. `FallFrames` is Tests/CorbelScreenshotTest.cpp's
	 * 180 — three seconds, against the 0.28 s the freed brick needs to fall its 37.5 cm — and it is
	 * waited out after BOTH mutations that hand bodies to physics: the delete's cascade and the Run.
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
	 * =====================================================================================
	 * THE GRID, SPELLED OUT RATHER THAN IMPORTED (the rule Tests/SessionControllerTest.cpp keeps).
	 * =====================================================================================
	 *
	 * A brick is 21.5 x 10.25 x 6.5 cm on a 1 cm joint, so the coordinating grid is 22.5 across,
	 * 11.25 for the half stagger and 7.5 up. Course 0 rests a brick ON the earth, centred at 3.25;
	 * course n centres it at n * 7.5 + 3.25. A timber plate is 5.0 cm half-height, so the same course
	 * puts a plate at n * 7.5 + 5.0 — which is why the build plane is derived from the piece as well
	 * as from the course.
	 */

	/** Course 0's plane for a brick: 0 * 7.5 + 3.25. */
	constexpr double BrickPlaneCourse0Cm = 3.25;

	/** Course 1's plane for a brick: 1 * 7.5 + 3.25. */
	constexpr double BrickPlaneCourse1Cm = 10.75;

	/** Course 2's plane for a PLATE: 2 * 7.5 + 5.0. */
	constexpr double PlatePlaneCourse2Cm = 20.0;

	/** Course 5's plane for a brick: 5 * 7.5 + 3.25 = 40.75, so its underside is 37.5 cm up. */
	constexpr int32 FloatingCourse = 5;
	constexpr double FloatingPlaneZCm = 40.75;

	/** The half extents the palette derives, written out because the framing box is built from them. */
	const FVector HalfBrickCm(10.75, 5.125, 3.25);
	const FVector HalfPlateCm(33.75, 5.125, 5.0);

	/**
	 * THE CURSORS THE WALL IS LAID AT, all on Y = 0.
	 *
	 * Course 0 at 0, 22.5 and 45: the first lands verbatim (an empty plot offers only the Free
	 * fallback) and the next two are exactly one brick plus one head joint along, so each takes the
	 * same-course snap at zero offset. Course 1 at 11.25 and 33.75: the running bond's half stagger,
	 * so each straddles the two below it and beds onto both. The plate is asked for at 22.5, the
	 * middle of the wall.
	 */
	constexpr double Course0CursorsXCm[] = { 0.0, 22.5, 45.0 };
	constexpr double Course1CursorsXCm[] = { 11.25, 33.75 };
	constexpr double PlateCursorXCm = 22.5;

	/**
	 * THE BRICK THE DETAILS WINDOW IS OPENED ON, AND THE ONE THEN DELETED: the FIRST COURSE-1 BRICK,
	 * the fourth piece laid, so index 3 — at x = 11.25, one course up.
	 *
	 * CHOSEN BECAUSE IT CARRIES A SCREWED BEARING, which is the whole point of the frame. The plate
	 * is laid with the `Joint screw` chip down, and it bears on BOTH course-1 bricks whichever of
	 * the two tied centred snaps it takes (see GhostCursorXCm: centred at 11.25 it spans -22.5..45,
	 * centred at 33.75 it spans 0..67.5, and both bricks sit inside either span). So this brick's
	 * readout shows its two mortared beds, its perpend head joint and ONE SCREW — four rows that
	 * name three different profiles, which is a picture of the joint-choice feature rather than of a
	 * readout that prints the same word four times.
	 *
	 * IT USED TO BE THE MIDDLE BOTTOM BRICK (index 1, x = 22.5), whose four joints are all mortar.
	 */
	constexpr int32 InspectedPieceIndex = 3;
	constexpr double InspectedCentreXCm = 11.25;

	/**
	 * WHERE THE GHOST HOVERS IN FRAME 1, AND WHY IT IS THIS FAR OUT.
	 *
	 * A plate is selected and the wall's whole top is already taken, so EVERY plate snap in range is
	 * dropped for occupancy — the centred pose over either course-1 brick, and both edge-flush poses,
	 * all interpenetrate the plate just laid. What is left is the solver's Free fallback: the cursor
	 * honoured verbatim. That is a true picture of today's behaviour (and of the logged
	 * `bRequestedPoseOccupied` deferral — the UI cannot yet say "Free won because everything was
	 * occupied"), but it means the ghost is drawn exactly where the ray meets the plane, so the cursor
	 * has to be put somewhere the gold ghost does not INTERSECT the real plate.
	 *
	 * 112.5 cm clears it whichever centred snap the plate took. The plate is asked for at 22.5, which
	 * is equidistant from the two course-1 bricks at 11.25 and 33.75, so the tie falls to whichever
	 * the solver emitted first — the plate lands at one of those two and its right end is therefore at
	 * 45 or 67.5. A ghost centred at 112.5 starts at 112.5 - 33.75 = 78.75, clear of the further of
	 * the two by 11.25 cm.
	 */
	constexpr double GhostCursorXCm = 112.5;

	/**
	 * AND WHERE THE FREE BRICK FLOATS IN FRAME 4: 120 cm along, five courses up.
	 *
	 * Free placement honours the cursor verbatim and forms no joint at all, and 120 cm is in any case
	 * 75 cm from the nearest brick — well outside the 30 cm snap radius — so it is jointless for two
	 * independent reasons. Its underside at 37.5 cm is thirty-seven times the 1 cm joint the grounded
	 * rule allows, so it cannot read grounded by rounding either.
	 */
	constexpr double FloatingCursorXCm = 120.0;

	/**
	 * How far above the build plane a laying ray starts.
	 *
	 * The ray END is aimed AT the plane point and the ORIGIN put 200 cm above it, so the direction is
	 * straight down and the intersection is unambiguous. 200 cm is well inside the component's 1 km
	 * maximum pick distance.
	 */
	constexpr double RayHeightCm = 200.0;

	inline FVector LayRayStart(double XCm, double PlaneZCm)
	{
		return FVector(XCm, 0.0, PlaneZCm + RayHeightCm);
	}

	inline FVector LayRayEnd(double XCm, double PlaneZCm)
	{
		return FVector(XCm, 0.0, PlaneZCm);
	}

	/** The same ray, off the Y = 0 line — the L-wall's second leg runs along Y. */
	inline FVector LayRayStartXY(double XCm, double YCm, double PlaneZCm)
	{
		return FVector(XCm, YCm, PlaneZCm + RayHeightCm);
	}

	inline FVector LayRayEndXY(double XCm, double YCm, double PlaneZCm)
	{
		return FVector(XCm, YCm, PlaneZCm);
	}

	/*
	 * =====================================================================================
	 * FRAME 6 — THE L-WALL, AND WHERE ITS ELEVEN CURSORS COME FROM
	 * =====================================================================================
	 *
	 * The cursors and the expected answers are `Core.BuildMode.CornerWallStands`' own table, laid
	 * here through the PLAYER's click instead of through `BuildMode::PlacePiece`. That test's header
	 * works every one of them against the solver — the return asked for 0.625 cm off its pose so the
	 * one flush choice that turns the corner wins outright, the two Y-leg bricks asked for 0.125 cm
	 * short of the pitch so the same-course pose beats the next-course one, and course 1 asked for
	 * AT the running bond where nothing can outrank an offset of zero.
	 *
	 * THE ORDER IS CornerWallStands' ORDER AND THAT IS LOAD-BEARING, NOT COSMETIC. CURRENT_STATE's
	 * CR-2a finding (ix) records that joints come only from candidates the PLACED piece emits, so a
	 * brick laid after a neighbour it should bond to forms no joint to it. The course-1 corner brick
	 * at (56.25, 0) takes its quoin from a corner-return pose of the Y leg's course-1 brick at
	 * (61.875, 16.875) — which therefore has to be standing FIRST. Laid the other way round the L is
	 * still a wall, but it is an eighteen-connection wall, and the twelve-mortar count below would be
	 * eleven.
	 */
	struct FCornerStep
	{
		double XCm;
		double YCm;

		/** 0 or 1 — the only two courses this wall has. */
		int32 Course;

		/** Whether the piece lies along Y: the `Rotate` chip's own flag. */
		bool bRotated;
	};

	const FCornerStep CornerSteps[] = {
		/* Course 0, the X leg — three stretchers on the earth. */
		{  0.000,  0.000, 0, false },
		{ 22.500,  0.000, 0, false },
		{ 45.000,  0.000, 0, false },

		/* The corner: a ROTATED brick returning off the X leg's +X end. This is the chip's reason. */
		{ 61.875,  5.000, 0, true },

		/* Course 0, the Y leg — the same running bond, stepping along Y. */
		{ 61.875, 28.000, 0, true },
		{ 61.875, 50.500, 0, true },

		/* Course 1, staggered over the X leg. */
		{ 11.250,  0.000, 1, false },
		{ 33.750,  0.000, 1, false },

		/* The Y leg's first course-1 brick — BEFORE the lap, see the order note above. */
		{ 61.875, 16.875, 1, true },

		/* The corner brick: the stretcher a bricklayer laps OVER the return, bonding both legs. */
		{ 56.250,  0.000, 1, false },

		/* And the Y leg's second. */
		{ 61.875, 39.375, 1, true },
	};

	constexpr int32 CornerExpectedPieces = 11;
	constexpr int32 CornerExpectedConnections = 19;
	constexpr int32 CornerExpectedMortar = 12;
	constexpr int32 CornerExpectedPerpend = 7;

	/** The six laid on the earth; everything after index 5 reaches it only through its beds. */
	constexpr int32 CornerLastGroundedPiece = 5;

	/**
	 * WHERE THE GHOST HOVERS IN FRAME 6: the next brick along the Y leg, still rotated.
	 *
	 * The leg's course-1 bricks step 22.5 cm along Y, so 39.375 + 22.5 = 61.875 is where the next one
	 * goes. A rotated ghost there is the picture the frame is for — a header-on brick, visibly turned
	 * ninety degrees to the three stretchers in the same shot.
	 */
	constexpr double CornerGhostXCm = 61.875;
	constexpr double CornerGhostYCm = 61.875;

	/**
	 * THE L'S OWN BOUNDS, and the ONE reframe in this harness.
	 *
	 * The other five frames share a camera on purpose — they are five readings of one plot and are
	 * meant to be laid side by side. This one is a different building in a different footprint: the
	 * L reaches 61 cm along Y where the wall was 10 cm deep, so the frame-1 camera would show it
	 * edge-on and foreshortened, which is the one thing a picture of a CORNER may not be. Framed
	 * through the same production function (`ViewpointFor`, `ThreeQuarter`), so the angle is still
	 * the one the game puts a player at.
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

	/**
	 * THE DESTROY RAY GOES ALONG Y, NOT DOWN FROM ABOVE, AND THAT IS A CORRECTNESS CHOICE.
	 *
	 * It is a REAL line trace against real collision, and by frame 2 the timber plate spans the whole
	 * top of the wall — so a ray dropped from above onto the middle bottom brick's centre would hit
	 * the PLATE first and the menu would come up on the wrong piece. A brick is 10.25 cm deep centred
	 * on Y = 0, so +/- 100 cm along Y is far outside it on both sides and the ray crosses the whole
	 * thickness with nothing else in the way. Same reach, for the same reason, as the piece-menu and
	 * session tests.
	 */
	constexpr double InspectReachCm = 100.0;

	/**
	 * THE ONE BOX EVERY FRAME IS FRAMED OVER, and it is stated rather than measured.
	 *
	 * The camera is placed BEFORE anything is laid — so the warm-up frames are spent on the view the
	 * first picture is taken of rather than on the game mode's — which means the bounds cannot be read
	 * off the structure. They are written out instead, as the union of every pose that will ever
	 * appear in any of the four frames:
	 *
	 *   the wall            X -10.75 .. 55.75,  Z 0 .. 14
	 *   the timber plate    X -22.50 .. 67.50,  Z 15 .. 25   (either centred snap, see GhostCursorXCm)
	 *   the ghost           X  78.75 .. 146.25, Z 15 .. 25
	 *   the floating brick  X 109.25 .. 130.75, Z 37.5 .. 44
	 *   and where it lands  X 109.25 .. 130.75, Z 0 .. 6.5
	 *
	 * Y is the bricks' own 10.25 cm depth. The three-quarter framing sizes its standoff from the
	 * bounding SPHERE of this box, so a little slack costs a little distance and nothing else.
	 */
	inline FBox SessionStageBoundsCm()
	{
		return FBox(
			FVector(-22.5, -5.125, 0.0),
			FVector(GhostCursorXCm + HalfPlateCm.X, 5.125, FloatingPlaneZCm + HalfBrickCm.Z));
	}

	/** The aspect the game mode frames with: the viewport's height over its width at 1920 x 1080. */
	constexpr double FrameAspectHeightOverWidth = 1080.0 / 1920.0;

	/**
	 * WHAT THE RUN BUILT, CARRIED BETWEEN LATENT COMMANDS.
	 *
	 * FILE-SCOPE STATE, for the reason the sibling harnesses give: a latent command carries only what
	 * its parameters carry, and the build, the four shots and the file check run frames apart.
	 */
	struct FSessionShotRecord
	{
		bool bStaged = false;

		TWeakObjectPtr<ADestructionGamePlayerController> Controller;

		int32 StructureId = INDEX_NONE;

		/**
		 * The index the Free brick took, and where its ACTOR stood when Run let go of it.
		 *
		 * THE ACTOR'S LOCATION AND NOT THE PIECE'S BOX CENTRE, because the travel below is measured
		 * against the same actor afterwards and a brick actor's pivot is its CORNER — comparing the
		 * two would report a fall 11 cm longer than the brick actually made.
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
	 * Ask for a screenshot, THROUGH THE VIEWPORT CLIENT AND NOT THROUGH GEngine.
	 *
	 * MEASURED, NOT PREFERRED — see Tests/CorbelScreenshotTest.cpp: a request routed through
	 * `UEngine::Exec` asserts everything correctly and writes no PNG, because there is no SHOT handler
	 * there. `HandleScreenshotCommand` lives on `UGameViewportClient`.
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

	/** The player's live build, or null. Read, never driven — the controller is what is driven. */
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

	/** Whether the strip this state draws offers this button LIVE. Asked of the production model. */
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

	/**
	 * What every frame claims about the state it is a picture of, asserted at the moment the shot is
	 * queued.
	 *
	 * FOUR READINGS, EACH BINARY OR A COUNT. The mode decides which strip is drawn; the piece count
	 * says the build is the one the frame describes; the menu says whether a details window should be
	 * in the picture; the ghost's visibility says whether a gold brick should be. None of them is a
	 * judgement about pixels — they are what makes the file a picture of the right thing rather than a
	 * valid PNG of the wrong one.
	 */
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

		/*
		 * THE GHOST IS READ OFF THE COMPONENT BECAUSE THERE IS NO OTHER WAY TO SEE IT. It is the one
		 * thing in these pictures with no controller-level accessor, and "a gold brick is or is not in
		 * this frame" is exactly the claim a picture can be wrong about.
		 */
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

/**
 * Open the stage: a world, a viewport, the controller the game mode opened the session on — and the
 * camera, moved in once and never again.
 */
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

	/*
	 * THE VIEWPORT IS ASSERTED HERE rather than left to show up as a missing file, because without one
	 * `HandleScreenshotCommand` returns having done nothing at all and the only symptom downstream
	 * reads identically to a renderer that failed.
	 */
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

	/*
	 * THE LEVEL MUST HAVE OPENED IN BUILD MODE, AND IT IS A FIXTURE CLAIM RATHER THAN A SETUP STEP.
	 * `ADestructionGameGameMode::BeginPlay` puts the build plot's player in Build mode through the
	 * controller's own door; if it has not, the map on the command line is not the build plot and
	 * every frame below would be a picture of somebody else's level.
	 */
	const DestructionSession::FSessionToolbarState& State = Controller->GetSessionToolbarState();

	Test->TestTrue(
		*FString::Printf(
			TEXT("fixture: the build plot must have opened the session in BUILD mode — if it did not, "
				 "the map on the command line is not Lvl_Build. The state is %s"),
			*StateBits(State)),
		State.Mode == DestructionSession::ESessionMode::Build);

	/* --- the camera, moved in once, in the game mode's own framing ---------------------------- */

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

/**
 * Lay the wall a click at a time, bear a plate across it, and leave the ghost hovering.
 *
 * EVERY PLACEMENT IS `PrimaryAlongRay`, WHICH IS THE PLAYER'S CLICK. The three course-0 cursors are
 * exactly on the bond so each takes its same-course snap; the two course-1 cursors are exactly on the
 * half stagger so each straddles the two below it; the plate is asked for at the middle of the wall
 * and the solver's centred snap pulls it onto whichever course-1 brick it ties on.
 */
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
	 * AND THE PLATE IS SCREWED DOWN — the joint chip clicked BEFORE the piece it applies to.
	 *
	 * THE BRICKS ARE LEFT ON AUTO ON PURPOSE. The choice applies to the NEXT placement only, so a
	 * wall laid under Auto and a plate laid under Screw is one structure carrying three different
	 * profiles — mortared beds, the inferred weak perpends, and two screwed bearings — which is what
	 * makes frame 2's readout show the setting rather than one word repeated. A session that screwed
	 * everything would photograph the same picture whether the chip worked or not.
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

	/*
	 * BACK TO AUTO, so the strip in frame 1 is the one a player sees by default and the Free brick
	 * frame 5 lays is not photographed under a setting this frame happened to leave down.
	 */
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
	 * THE CONNECTION COUNT IS THE MECHANISM READING OF "IT IS A WALL AND THE PLATE BEARS ON IT".
	 *
	 * Six boxes stacked with no joints between them look identical in a photograph and behave nothing
	 * like a structure, so the picture is worth nothing without this. The bond gives 2 head joints on
	 * course 0, 2 beds under the first course-1 brick, 2 beds and a head under the second, and 2
	 * bearings under the plate: 9. The floor is 8 rather than 9 because which of the two tied centred
	 * snaps the plate takes is an ordering detail (see GhostCursorXCm) and a floor is the honest claim
	 * — but it is a floor well above the 6 or 7 a wall with a plate merely resting on one brick would
	 * read, so it cannot go green over a plate that bears on nothing.
	 */
	Test->TestTrue(
		*FString::Printf(
			TEXT("THE PIECES MUST BE BONDED: 2 head joints on course 0, 5 bed/head joints into course 1 "
				 "and at least 2 bearings under the plate, so at least 8 connections. It holds %d"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections() >= 8);

	/*
	 * AND THE PLATE'S BEARINGS ARE SCREWS — BOTH OF THEM, READ OFF THE GRAPH.
	 *
	 * THE MECHANISM READING BEHIND FRAME 2'S WORDS. The details window is a presenter over these
	 * connections, so if the chip never reached the door the readout would name mortar and the frame
	 * would be a perfectly sharp picture of the wrong structure. TWO of them because one override
	 * written onto the first joint leaves the far end of a screwed plate resting on friction, which
	 * is `World.BuildMode.JointOverrideRidesThroughPlacement`'s own claim and is worth a sentence
	 * here because this is the only place the whole click-to-committed-physics path runs at once.
	 *
	 * ALL FIVE FIELDS, because the library is siblings by construction and one field would admit a
	 * neighbour.
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

	/* --- and the ghost, left hovering where the next click would land -------------------------- */

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

/** FRAME 1: Build mode, the wall laid, the ghost hovering. */
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
 * Switch to Destroy, point at the middle bottom brick and click it, so its details window is up.
 *
 * THE HOVER AND THE CLICK ARE BOTH MADE, in that order, because they are two different seams and a
 * player makes both: `PointerAlongRay` calls the brick out under the cursor and `PrimaryAlongRay` is
 * what opens the menu on it. `SetInspectedPiece` is the third — it is what hovering an entry in the
 * menu does, and it is what makes the readout draw the support line and the per-joint breakout rather
 * than a count and a label. Without it the details window in the picture would be the empty one.
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

	/* Pointing first, which is what a player's cursor does on its way to the click. */
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

	/*
	 * SINGLE THE BRICK OUT, which is what hovering its entry in the menu does — and it is what puts
	 * the joint readout into the picture instead of a bare count.
	 */
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
	 * THE ROWS IN THIS PICTURE NAME THREE DIFFERENT PROFILES, AND THAT IS WHAT MAKES IT A PICTURE OF
	 * THE JOINT CHOICE.
	 *
	 * The brick under the plate wears mortared beds, a perpend head joint and the ONE SCREW the
	 * player chose when they laid the plate — so a human looking at the frame can read the setting
	 * they clicked back off the window, which is the only place the choice is ever visible (a screwed
	 * plate and a dry-bedded one sit in exactly the same pixels). Asserted as COUNTS rather than as a
	 * sentence: `Presenter.JointRowNamesTheProfile` owns the wording, and what this frame needs is
	 * that both words are in the picture at once.
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

	/* And the Destroy strip's own command, which this frame claims to be showing live. */
	const FSessionToolbarState& State = Controller->GetSessionToolbarState();

	Test->TestTrue(
		*FString::Printf(
			TEXT("frame 2's strip must offer Run structure LIVE over the player's six-piece build; the "
				 "state is %s"),
			*StateBits(State)),
		ButtonIsEnabled(State, EToolbarButtonId::RunStructure));

	return true;
}

/** FRAME 2: Destroy mode, the details window open on the middle bottom brick. */
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

/**
 * Choose the Delete row: the brick leaves the wall, and the delete path's own solve settles whatever
 * that condemns.
 */
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

	/*
	 * AND ONLY THAT ONE IS REMOVED. The cascade RELEASES pieces it condemns rather than removing them,
	 * so a delete that took half the wall out of the graph would read here — and "whatever settles
	 * settles" is about bodies falling, not about pieces vanishing.
	 */
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

/** FRAME 3: the hole where the middle bottom brick was, and whatever settled. */
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
 * Switch the load overlay ON over the settled wall, and check the world actually wears the bands.
 *
 * THE PICTURE IS THE POINT AND THE ASSERTION IS WHAT MAKES IT A PICTURE OF THE RIGHT THING. Every
 * headless test of this feature runs `-nullrhi`, so not one of them has ever caused a coloured brick to
 * exist; what this adds is the frame. The claim made beside it is the one the frame cannot make for
 * itself — that each brick wears exactly `BrickHighlightForLoadBand(WorstJointBandForPiece(...))`,
 * composed here from the two production functions rather than written down as a literal, because which
 * band a given brick of this wall lands in is a solver answer this harness has no business pinning.
 *
 * A RELEASED PIECE IS SKIPPED. The delete's own settle may have let go of bodies, and a brick handed to
 * physics is no longer part of the structure the overlay describes — `World.Session`'s own
 * `LoadOverlayIgnoresReleasedPieces` is where that rule lives, and repeating it here would make this
 * harness fail for that test's reason.
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

/** FRAME 4: the settled wall tinted by where its load is, with the chip lit. */
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
 * And the overlay OFF again, so the frames after it are the pictures they have always been.
 *
 * THE CLAIM IS "NO LOAD STATE ANYWHERE" RATHER THAN "EVERY BRICK IS None", DELIBERATELY. The hover and
 * the selection are the overlay's superiors in `HighlightForPiece`, and a brick that happens to be
 * claimed by one of them is not evidence of a stale tint — asserting `None` here would be asserting
 * that taking the overlay off also drops the hover, which is the opposite of the precedence
 * `World.Session.LoadOverlayYieldsToHover` pins.
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
 * Lay one FREE brick in mid-air with nothing under it, then Run the plot so it is let go.
 *
 * THE FLOATING BRICK IS THE WHOLE FIXTURE, for the reason `World.Session.RunStructureSettlesTheBuild`
 * gives: a Run over a build that already stands releases nothing, and "nothing happened" is exactly
 * what a Run button wired to no solver produces. A piece that CANNOT stand is the one observable
 * difference between a Run that ran and a Run that did not — and in a photograph it is the difference
 * between a brick in the air and a brick on the ground.
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

	/* Course 2 up to course 5: three steps, each of which must land. */
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

	/*
	 * THE TWO FIXTURE PRECONDITIONS, AND BOTH ARE NEEDED. A brick that turned out to be grounded, or
	 * bonded to something, would STAND — and the frame would be a picture of a brick in the air with a
	 * green test underneath it saying Run worked.
	 */
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

	/* --- and Run it, from Destroy mode, through the one door ---------------------------------- */

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

	/*
	 * `IsReleased`, NEVER DISPLACEMENT. DESIGN §4 is explicit, and it bites here: the brick is about
	 * to be photographed on the ground, so the temptation to read its Z is exactly the mistake. Where
	 * it ends up is Chaos's business; whether Run handed it over is the binding's own record.
	 */
	Test->TestTrue(
		*FString::Printf(
			TEXT("RUN MUST SETTLE THE BUILD: the brick with nothing under it has to be handed to "
				 "physics. IsReleased(%d) reports %d"),
			Record.FloatingPieceIndex, After->IsReleased(Record.FloatingPieceIndex) ? 1 : 0),
		After->IsReleased(Record.FloatingPieceIndex));

	return true;
}

/**
 * FRAME 5: the freed brick, after three seconds of falling.
 *
 * HOW FAR IT TRAVELLED IS REPORTED AND NOT ASSERTED. It is the number a reader needs to tell a
 * picture of a brick that fell from a picture of a brick that was released and did not move — and it
 * is an observation, not the claim, because the claim is `IsReleased` and was made before the wait.
 */
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
 * FRAME 6's BUILD: clear the plot, reframe the camera, and lay an L-WALL with the rotate chip.
 *
 * =====================================================================================
 * WHY THIS FRAME EXISTS AND WHY IT IS RED UNTIL THE CHIP DOES
 * =====================================================================================
 *
 * CR-2a taught the solver a corner return and `Core.BuildMode.CornerWallStands` proves the wall it
 * builds — but through `BuildMode::PlacePiece`, which takes an extent as an argument. A PLAYER has
 * no way to hand the game a turned box: every placement's footprint comes from the palette, so
 * until there is a chip that swaps X and Y the whole corner vocabulary is code nobody can reach.
 * This frame is the proof that they can, and it is the only picture in this project of a building
 * that turns a corner.
 *
 * SO IT IS GATED ON THE CHIP, AT THE TOP, AND SAYS SO. `PrimaryAlongRay` with a rotated ghost is
 * impossible before `EToolbarButtonId::RotatePiece` exists — the model refuses a button it does not
 * draw — so a probe click is made first and the whole build is abandoned with one named error if it
 * comes back false. That keeps the failure readable as "the control is missing" rather than as
 * eleven snap poses landing in the wrong places.
 *
 * =====================================================================================
 * WHAT IT ASSERTS BESIDE THE PICTURE
 * =====================================================================================
 *
 * The same mechanism readings `CornerWallStands` makes, taken through the binding: eleven pieces,
 * nineteen connections, twelve full-mortar joints (ten beds and two quoins) and seven weak perpend
 * heads, then every piece Grounded or Supported after a solve. The profile MIX is what makes the
 * picture a picture of a CORNER — a wall whose quoins came back as perpends looks identical in every
 * pixel and is a different building.
 *
 * `SolveLoads` IS NON-DESTRUCTIVE AND IS THE RIGHT CALL HERE. It is re-runnable by contract
 * (`FStructureBinding::SolveLoads`, "ApplyResults is what turns the answer into work on the world"),
 * so reading support kinds off it changes nothing in the frame — unlike `SolveAndBreak`, which the
 * Run command uses and which would settle the wall before it is photographed.
 *
 * THE 3D FLAG IS NO LONGER SET BY HAND HERE. `UDestructionStructureSubsystem::BeginBuild` states it
 * for every player's build, so the L this frame photographs arrives flagged and the harness has
 * nothing to state. It changed nothing this frame reads in any case — `SolveLoads` does not read the
 * flag at all (the router is dimension-agnostic, routing load down bed joints and reading each normal
 * directly), and the one reader, the LP bridge, is consulted only by the below-cap break gate inside
 * `SolveAndBreak`, which this frame deliberately never runs. What the flag really buys is the
 * player's `Run structure`, which is `World.Session.CornerBuildIsJudgedByTheLP`'s business rather
 * than this picture's.
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

	/* --- THE GATE: is there a rotate chip at all? --------------------------------------------- */

	if (!Controller->OnToolbarButton(EToolbarButtonId::RotatePiece))
	{
		Test->AddError(
			TEXT("CR-2b: OnToolbarButton(RotatePiece) was REFUSED, so there is no way for a player to "
				 "turn a piece and no L-wall can be laid. The model refuses a button it does not draw, "
				 "which is exactly what a missing chip looks like from here. Frame 6 will be a picture "
				 "of an empty plot until the chip exists"));

		return true;
	}

	/* And straight back, so the table below owns every rotation from a known upright start. */
	Test->TestTrue(
		TEXT("and the same chip must turn it off again — a setting the player cannot unset is not a "
			 "setting"),
		Controller->OnToolbarButton(EToolbarButtonId::RotatePiece));

	Test->TestFalse(
		*FString::Printf(
			TEXT("the corner build must start from an UPRIGHT session; it reads %s"),
			*StateBits(Controller->GetSessionToolbarState())),
		Controller->GetSessionToolbarState().bRotated);

	/* --- a fresh plot: the five frames before this one leave a wall and a fallen brick on it --- */

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

	/* --- the camera, moved ONCE for this frame ------------------------------------------------ */

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

		/* The course, stepped one chip click at a time — this wall only ever goes up. */
		while (Controller->GetSessionToolbarState().Course < Step.Course)
		{
			Test->TestTrue(
				*FString::Printf(TEXT("step %d: Course up must land"), Index),
				Controller->OnToolbarButton(EToolbarButtonId::CourseUp));
		}

		/* And the rotation, which is one click whenever the next piece lies the other way. */
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

		/*
		 * The brick's own course plane: course 0 rests it on the earth at 3.25, course 1 one pitch
		 * above at 10.75. Spelled from the two named constants rather than multiplied out, so the
		 * arithmetic stays where the file's own grid note put it.
		 */
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
			/* The cleared plot opened a NEW binding; the first brick is what gives it an id. */
			Record.StructureId = Controller->GetSessionStructureId();

			Test->TestTrue(
				*FString::Printf(
					TEXT("the first brick of the L must give the session a structure to name; it names "
						 "%d"),
					Record.StructureId),
				Record.StructureId != INDEX_NONE);
		}
	}

	/* --- what the L actually is, before it is photographed ------------------------------------ */

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
	 * THE PROFILE MIX, COUNTED — and it is what makes this a picture of a CORNER rather than of a
	 * wall that happens to bend. The quoin is a mortar joint across a HORIZONTAL normal, which is
	 * exactly the contact the pre-CR-2a inference called a weak perpend; counted both ways so a
	 * wrong profile somewhere cannot cancel against a right one elsewhere.
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
	 * AND IT STANDS, NOT BY BEING PINNED TO THE EARTH. The exact support kind per piece: the six laid
	 * on the ground read Grounded and the five above them reach the earth ONLY through the beds the
	 * player's own clicks formed, so a bed that never formed reads Falling or Stranded here rather
	 * than quietly making a prettier picture.
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

	/* --- and the ghost, still rotated, where the next brick of the Y leg would go -------------- */

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

/** FRAME 6: the L-wall, turned corner and all, with a rotated ghost at the end of its second leg. */
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
 * Take the build off the plot, through the toolbar, and leave the level as the game mode left it.
 *
 * `Clear build` RATHER THAN A SWEEP OF THE WORLD, because it is the session's own command and it is
 * what a player would press: it cancels the binding, destroys its bricks and opens a fresh empty plot.
 * The level itself laid nothing — that is what a build sandbox is — so an empty plot in Build mode is
 * exactly the state begin-play produced.
 */
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

/** All six files landed and they are real PNGs. All six were deleted before the run. */
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
		 * THE SIGNATURE AND THE IHDR, READ BY HAND. Eight signature bytes, then a four-byte chunk
		 * length, then "IHDR", then width and height as big-endian 32-bit integers. A byte count alone
		 * passes for a file of random bytes, and a decoder would be a dependency on the very rendering
		 * stack under test.
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

	/*
	 * THE OLD FILES GO FIRST, SYNCHRONOUSLY, BEFORE ANY LATENT COMMAND IS QUEUED. Everything
	 * downstream reads "the file exists" as "this run rendered a frame", and that reading is only true
	 * if the file cannot have survived from an earlier run.
	 */
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
	 * THE SEQUENCE: set the state up, let the view settle, shoot — five times, off ONE camera placed
	 * before the first warm-up. Screen messages go off before anything is waited on; each shot is
	 * followed by its write wait because ProcessScreenShots writes at end of draw; and each of the two
	 * mutations that hands bodies to physics is followed by a fall wait and a settle.
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

	/*
	 * AND FRAME 6 LAST, because it is the one that CLEARS the plot: everything before it is five
	 * readings of one wall, and an L laid on top of that wall would snap onto it.
	 */
	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotCornerCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShootCornerCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotTearDownCommand(this));

	ADD_LATENT_AUTOMATION_COMMAND(FSessionShotCheckFilesCommand(this));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
