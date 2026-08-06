// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"
#include "Core/PieceSelection.h"
#include "Core/StructureBinding.h"
#include "DestructionGameGameMode.h"
#include "DestructionGamePlayerController.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SCREENSHOT HARNESS. IT EXISTS BECAUSE NOTHING IN THIS PROJECT HAS EVER RENDERED A PIXEL.
 *
 * Every other run in this suite is -nullrhi, so the whole renderer is absent, and the project has
 * twice shipped a fully green suite over something invisible on screen — the Nanite bug and the
 * commit-path push bug, both recorded in CURRENT_STATE.md. This test opens the piece menu on a
 * real wall in a real viewport and writes TWO PNGs a HUMAN can look at: a wide one that shows the
 * panel in the context of the whole wall, and a close one framing a handful of bricks around the
 * inspected one, where the three highlight colours are actually big enough to tell apart.
 *
 * IT IS A HARNESS AND IT IS ALSO A TEST, AND THE SECOND HALF IS WHAT KEEPS IT HONEST. A harness
 * that only takes a picture is green whatever is in the picture. So it deletes both files first (a
 * stale PNG from last week is otherwise indistinguishable from a fresh one), then asserts what a
 * picture of nothing could not satisfy: the world, the wall, the viewport, three bricks actually
 * SELECTED by rays into the world, a brick genuinely inspected, a menu still up after the shots,
 * and files that begin with the PNG signature and declare real dimensions.
 *
 * WHAT IT DELIBERATELY DOES NOT ASSERT IS ANYTHING ABOUT THE IMAGE'S CONTENT. An absolute RGB
 * comparison is hostage to tone mapping, auto-exposure warm-up and driver differences, and would
 * flake; CURRENT_STATE.md already records that if a pixel test is ever written it must be a
 * DIFFERENCE test. Judging what the panel looks like is a human's job, and this exists to give
 * that human something to look at.
 *
 * IT NEEDS A TICKING WORLD *AND* A REAL RHI, WHICH IS WHY IT CARRIES EAutomationTestFlags::
 * NonNullRHI. FAutomationTestFramework::GetValidTestNames strips that feature bit when -nullrhi is
 * on the command line (AutomationTest.cpp:841-845) and then filters the test out (:867-871).
 * WITHOUT THAT FLAG THE ORDINARY HEADLESS SUITE WOULD RUN THIS, find no viewport, write no file
 * and go green — which is precisely the failure mode this test exists to stop.
 *
 * HOW TO RUN IT. Close the editor, build as documented in CLAUDE.md, then:
 *
 *   "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.PieceMenuScreenshot"
 *     -TestExit="Automation Test Queue Empty"
 *
 * -nullrhi MUST BE ABSENT. FApp::CanEverRender() is false with it, and UGameEngine::Init only
 * builds a window and a viewport under that (GameEngine.cpp:1807, 1814) — so there would be
 * nothing for Slate to screenshot even if the filter let the test through.
 *
 * WHY `Shot showui` AND NOT `HighResShot`. HighResShot renders scene-only through an
 * FDummyViewport / FCanvas (UnrealClient.cpp:1694-1717), and ProcessScreenShots only takes the
 * Slate path when FScreenshotRequest::ShouldShowUI() is true (GameViewportClient.cpp:2380-2418),
 * which HandleHighresScreenshotCommand never sets. The menu is AddViewportWidgetContent, i.e.
 * Slate — so HighResShot would capture the wall with NO MENU ON IT, and the one thing this test
 * exists to photograph would be the one thing missing. HandleScreenshotCommand does set it
 * (GameViewportClient.cpp:4284-4307).
 *
 * NO PRODUCTION CODE WAS NEEDED, AND THAT IS A PROPERTY OF THE SEAMS RATHER THAN LUCK.
 * InspectAlongRay takes a WORLD-SPACE RAY rather than a cursor, and SetInspectedPiece takes a ref
 * rather than a hover event, precisely so that everything a player can reach is reachable without
 * a viewport. An exec command, a cvar and synthetic Slate input were all considered and rejected:
 * each is strictly more machinery for no gain, and each would be untested production code.
 *
 * NO FUNCTIONAL TEST, NO MAP EDIT, NO FunctionalTesting DEPENDENCY. Everything here comes from
 * Runtime/Engine/Public/Tests/AutomationCommon.h, in the Engine module which is already a
 * dependency. CLAUDE.md's standing rule is about AFunctionalTest ACTORS SERIALIZED INTO A .umap
 * carrying a class pointer from a module Epic does not precompile for Shipping into a cooked
 * build; this file places no actor in any map, so that rule is not engaged. Lvl_Sandbox is loaded
 * from the command line exactly as a player's session loads it, unmodified.
 */
namespace PieceMenuScreenshotSupport
{
	/**
	 * WHERE THE PNGs LAND, DERIVED HERE RATHER THAN HARD-CODED.
	 *
	 * UGameEngine::Init sets GameScreenshotSaveDirectory.Path to FPaths::ScreenShotDir()
	 * (GameEngine.cpp:919), and CreateViewportScreenShotFilename prefixes a bare name with it
	 * (UnrealClient.cpp:302-308). ScreenShotDir() is Saved/Screenshots/<PlatformName>/ and
	 * PlatformName() is "WindowsEditor" for a WITH_EDITOR binary — which is what
	 * UnrealEditor-Cmd.exe is, even running -game.
	 *
	 * The extension is appended by RequestScreenshot because the name carries none, and there is
	 * no numeric suffix because the exec passes -nosuffix (UnrealClient.cpp:240-247). A suffix
	 * would put every run in its own file, which sounds tidy and would make the deletion below
	 * useless: the point of one fixed name is that its existence can only mean THIS run.
	 *
	 * THERE ARE TWO SHOTS AND THEY ANSWER DIFFERENT QUESTIONS. The wide one is the panel in
	 * context — the whole wall, the menu hanging off the right edge — and at that standoff the
	 * three picked bricks are about 3% of the frame, which is enough to see that a selection
	 * exists and nowhere near enough to judge the highlight colours against each other. The close
	 * one frames a handful of bricks around the inspected one, so the selection teal, the
	 * inspected magenta and an unpicked neighbour are all resolvable in the same picture. Two
	 * files rather than one bigger file, because the framing is the whole difference and no single
	 * framing does both jobs.
	 */
	const TCHAR* const WideScreenshotBaseName = TEXT("PieceMenu");
	const TCHAR* const CloseUpScreenshotBaseName = TEXT("PieceMenuCloseUp");

	inline FString ScreenshotPathFor(const TCHAR* BaseName)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::ScreenShotDir() / FString(BaseName) + TEXT(".png"));
	}

	/**
	 * The execs that take them. `showui` is the whole reason this is `Shot` — see the file header.
	 */
	const TCHAR* const WideScreenshotCommand = TEXT("Shot showui filename=PieceMenu -nosuffix");
	const TCHAR* const CloseUpScreenshotCommand =
		TEXT("Shot showui filename=PieceMenuCloseUp -nosuffix");

	/**
	 * BOTH FILES, IN ONE LIST, BECAUSE EVERY CLAIM MADE ABOUT ONE IS MADE ABOUT THE OTHER. The
	 * deletion before the run and the PNG check after it are the same two statements twice over,
	 * and a list is what stops the second shot quietly acquiring a weaker version of either.
	 */
	const TCHAR* const ScreenshotBaseNames[] = { WideScreenshotBaseName, CloseUpScreenshotBaseName };

	/**
	 * THE ON-SCREEN DEBUG MESSAGES GO OFF, AND THAT IS A PROPERTY OF THE PICTURE RATHER THAN A
	 * CONVENIENCE.
	 *
	 * A capture meant for judging a UI had "Preparing Shaders (2)" burned into the top-left of it,
	 * which is a debug overlay sitting over the very thing a human is being asked to look at.
	 * UEngine::DrawOnscreenDebugMessages gates the whole block on GAreScreenMessagesEnabled
	 * (UnrealEngine.cpp:13630), and HandleDisableAllScreenMessagesCommand is the one switch that
	 * clears it (:10594). It is issued FIRST, before anything is waited on, so no frame this test
	 * causes to be drawn can carry one.
	 *
	 * IT IS BELT AS WELL AS BRACES. The waits below are what make the picture SETTLED; this is
	 * what stops a message being drawn over a settled picture anyway.
	 */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/**
	 * THE WALL'S GEOMETRY, DERIVED FROM THE SPEC RATHER THAN READ BACK OFF THE SOLVER.
	 *
	 * ADestructionGameGameMode::BeginPlay lays a flush running bond of 21.5 x 10.25 x 6.5 cm
	 * bricks with a 1.0 cm joint, 30 per course and 40 courses. DestructionLayout::RunningBond
	 * turns that into a brick pitch of 21.5 + 1 = 22.5 cm along X and a course pitch of
	 * 6.5 + 1 = 7.5 cm up Z; an EVEN course is 30 full bricks with brick n centred at n x 22.5,
	 * and every box is centred on Y = 0 (Layout.cpp:79). Course c is centred at
	 * 6.5 / 2 + 7.5 x c.
	 *
	 * So the wall spans X -10.75 .. 663.25 (674 cm) and Z 0 .. 299, and its centre is
	 * (-10.75 + 663.25) / 2 = 326.25 across and 149.5 up. Those two numbers are where the camera
	 * points; nothing about the picture depends on them being exactly right, but a camera aimed
	 * at nothing would take a picture of the sky and the assertions below would still pass, so
	 * they are written out rather than guessed.
	 */
	constexpr double BrickPitchCm = 22.5;
	constexpr double CoursePitchCm = 7.5;

	constexpr double WallCentreXCm = 326.25;
	constexpr double WallCentreZCm = 149.5;

	/** 30 x 40 flush: 20 even courses of 30 plus 20 odd courses of 31 (a half bat at each end). */
	constexpr int32 ScenarioWallPieceCount = 1220;

	/**
	 * THE THREE BRICKS THAT GET PICKED, AND WHY THEY ARE WHERE THEY ARE.
	 *
	 * COURSE 4 RATHER THAN COURSE 0, because the sandbox map has a floor of its own and nobody
	 * has measured how thick it is. A horizontal ray at the bottom course's centre height of
	 * 3.25 cm is one unlucky slab away from hitting the floor instead of a brick; four courses up
	 * is 33.25 cm and clear of anything a floor could plausibly be. Course 4 is EVEN, so it is 30
	 * full bricks at n x 22.5 with no half bats to reason about.
	 *
	 * THREE ADJACENT BRICKS NEAR THE MIDDLE OF THE WALL, so the picture shows a selection rather
	 * than one brick, and shows it where a human looking at the image will actually look. Bricks
	 * 13, 14 and 15 of the course put the middle one at X = 315, near the wall's own centre.
	 */
	constexpr int32 PickedCourse = 4;
	constexpr int32 PickedBricks[] = { 13, 14, 15 };
	constexpr int32 PickedCount = UE_ARRAY_COUNT(PickedBricks);

	inline double PickedCentreXCm(int32 Brick) { return Brick * BrickPitchCm; }
	inline double PickedCentreZCm() { return 6.5 * 0.5 + CoursePitchCm * PickedCourse; }

	/**
	 * How far along Y a ray starts and ends, either side of the wall.
	 *
	 * A brick is 10.25 cm deep and the wall is centred on Y = 0, so +/- 100 cm is far outside it
	 * on both sides and the ray crosses the whole thickness. Along Y rather than X or Z, so
	 * nothing else in the wall is ever in the way — the same choice, for the same reason, as
	 * Tests/StructureIntegrationTest.cpp's IntegrationReachCm.
	 */
	constexpr double RayReachCm = 100.0;

	/**
	 * HOW NEAR A BOX HAS TO BE TO COUNT AS THE BRICK THAT WAS ASKED FOR.
	 *
	 * The pieces are found by SEARCHING the binding's boxes for the ones at the derived
	 * positions, rather than by computing a piece INDEX. Index arithmetic over alternating
	 * 30- and 31-brick courses is exactly the kind of derivation that is silently wrong and
	 * still points at a real brick somewhere else in the wall; a position search either finds a
	 * brick where the bond says one is, or fails loudly. Half a brick pitch is comfortably
	 * tighter than the gap to the next brick along and enormously looser than the exact
	 * arithmetic needs.
	 */
	constexpr double BoxMatchToleranceCm = BrickPitchCm * 0.5;

	/**
	 * WHERE THE CAMERA GOES, AND WHY THAT FAR BACK.
	 *
	 * The default UCameraComponent field of view is 90 degrees HORIZONTALLY, so at 600 cm the
	 * visible width is 2 x 600 x tan(45) = 1200 cm against a wall 674 cm long, and the visible
	 * height at 16:9 is 1200 x 1080 / 1920 = 675 cm against a wall 299 cm tall. The whole wall is
	 * in frame with margin on both axes.
	 *
	 * IT IS PLACED EXPLICITLY RATHER THAN LEFT TO PlayerStart, because nobody knows which way
	 * PlayerStart faces — its transform is inside a binary .umap and has never been read. A
	 * screenshot harness whose framing depends on an unread asset is a harness that photographs
	 * whatever it happens to photograph.
	 *
	 * Yaw 90 looks along +Y, which is straight at a wall lying in the X-Z plane at Y = 0.
	 */
	constexpr double CameraStandoffCm = 600.0;
	constexpr double CameraYawDegrees = 90.0;

	/**
	 * AND WHERE IT GOES FOR THE CLOSE-UP, DERIVED FROM HOW MANY BRICKS HAVE TO BE IN FRAME.
	 *
	 * Same 90-degree horizontal field of view, so the visible width is 2 x standoff x tan(45),
	 * i.e. twice the standoff. Eight brick pitches is 8 x 22.5 = 180 cm, so 90 cm of standoff puts
	 * eight bricks across the frame — the three picked ones, the inspected one among them, and
	 * unpicked neighbours either side to compare them against. The visible height at 16:9 is
	 * 180 x 1080 / 1920 = 101 cm, about thirteen courses, so the bed joints above and below are in
	 * shot too.
	 *
	 * IT IS AIMED AT THE INSPECTED BRICK RATHER THAN AT THE WALL'S CENTRE, because the whole point
	 * of the second shot is the brick whose readout the panel is showing. The wall centre is 11 cm
	 * up and 11 bricks along from it, which at this standoff is most of the frame away.
	 */
	constexpr double CloseUpStandoffCm = 90.0;

	/**
	 * THE TIMINGS, AND WHY THEY ARE LATENT COMMANDS RATHER THAN A LONGER -ExecCmds.
	 *
	 * Every -ExecCmds string is queued into GEngine->DeferredCommands during UEngine::Init and
	 * flushed on the FIRST TickDeferredCommands, so splitting the work across several of them
	 * buys no delay whatsoever. Waiting has to be done from inside the test.
	 *
	 * SettleFrames covers TSR's temporal history and auto-exposure's adaptation, both of which
	 * need a handful of frames before the image stops changing. SlateFrames is the
	 * prepass-and-arrange the newly built panel needs before it has a size and a position.
	 * WriteFrames is because ProcessScreenShots writes at END OF DRAW — exiting in the same frame
	 * as the request loses the file.
	 *
	 * SETTLING IS DONE AGAIN AFTER THE CAMERA IS AIMED, AND THAT ORDER IS THE FIX RATHER THAN THE
	 * NUMBER. A frame came back with "Preparing Shaders (2)" still on it despite
	 * FWaitForShadersToFinishCompilingInGame having already run — which is not a contradiction:
	 * that wait ran while the pawn was still at PlayerStart, so the work it drained was the work
	 * the FIRST view asked for. Pointing the camera at 1220 bricks and putting a Slate panel over
	 * them queues more, and nothing was waiting on that. So every shot is preceded by its own
	 * shader drain AND its own settle, taken after the view it is a picture of has been set up.
	 *
	 * AND THE COUNT IS RAISED FOURFOLD, WHICH COSTS SECONDS IN A TEST NOBODY RUNS IN A LOOP. This
	 * is the one test in the project that needs a real RHI and it is run by hand to produce
	 * something a human looks at; a settled picture is worth far more here than a fast one.
	 */
	constexpr int32 SettleFrames = 120;
	constexpr int32 SlateFrames = 3;
	constexpr int32 WriteFrames = 5;

	/**
	 * THE SMALLEST FILE THAT COULD BE A REAL FRAME, IN BYTES.
	 *
	 * DERIVED, NOT PICKED, AND IT IS DELIBERATELY A FLOOR RATHER THAN A JUDGEMENT. A 1920 x 1080
	 * PNG of a single flat colour compresses to well under 10 kB, because PNG's filters reduce a
	 * constant image to a run of zeroes; a lit scene with a brick wall, a sky and text on top of
	 * it is hundreds of kB. 32 kB therefore sits several times above "the renderer produced
	 * nothing" and one to two orders of magnitude below "the renderer produced a picture", which
	 * is the widest margin available. It is not a claim about what the picture LOOKS like — that
	 * is a human's job, and the actual size is reported so the human can see it.
	 */
	constexpr int64 MinimumScreenshotBytes = 32 * 1024;

	/** Big enough that a 1 x 1 or a truncated header fails; small enough not to pin a resolution. */
	constexpr int32 MinimumScreenshotWidth = 640;
	constexpr int32 MinimumScreenshotHeight = 480;

	/**
	 * THE WARNING THE PROJECT'S HABITUAL GREP MISSES ENTIRELY.
	 *
	 * A material whose shader fails to compile keeps its whole node graph and renders as
	 * WorldGridMaterial, the checkerboard — the exact "correct data, silent renderer
	 * substitution, green suite" shape CURRENT_STATE.md records against the Nanite bug and the
	 * overlay materials. And ShaderCompiler.cpp:2306-2315 logs it at WARNING, not Error, so
	 * `LogAutomationController: Error` never sees it and neither does the automation framework.
	 * Reading the log file is the only way to notice, so this test reads it.
	 */
	const TCHAR* const MaterialFailureMarker = TEXT("Failed to compile Material");

	/** The binding for the wall the game mode built, or null with the reason reported. */
	inline FStructureBinding* FindScenarioWall(FAutomationTestBase& Test, UWorld* World)
	{
		ADestructionGameGameMode* const GameMode =
			World != nullptr ? World->GetAuthGameMode<ADestructionGameGameMode>() : nullptr;

		if (GameMode == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("the loaded map should be running ADestructionGameGameMode, it is running %s"),
				*GetNameSafe(World != nullptr ? World->GetAuthGameMode() : nullptr)));

			return nullptr;
		}

		UDestructionStructureSubsystem* const Subsystem =
			World->GetSubsystem<UDestructionStructureSubsystem>();

		if (Subsystem == nullptr)
		{
			Test.AddError(TEXT("the world should own a UDestructionStructureSubsystem"));
			return nullptr;
		}

		const int32 StructureId = GameMode->GetBuiltStructureId();

		FStructureBinding* const Binding =
			StructureId != INDEX_NONE ? Subsystem->Find(StructureId) : nullptr;

		if (Binding == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("the game mode should have built the scenario wall on begin-play; its structure id is %d and Find returned nothing"),
				StructureId));

			return nullptr;
		}

		return Binding;
	}

	/**
	 * The piece whose box sits at this point, or INDEX_NONE. See BoxMatchToleranceCm for why the
	 * bricks are found by POSITION rather than by an index computed from the course pattern.
	 */
	inline int32 PieceAtBox(const FStructureBinding& Binding, const FVector& CentreCm)
	{
		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (Binding.IsPieceRemoved(Piece))
			{
				continue;
			}

			if (FVector::Dist(Binding.GetBinding(Piece).Box.CentreCm, CentreCm) <= BoxMatchToleranceCm)
			{
				return Piece;
			}
		}

		return INDEX_NONE;
	}
}

/**
 * Aim the camera at the wall, pick three bricks the way a player picks them, and single the
 * middle one out so the joint readout has something to say.
 *
 * IT ENTERS THROUGH THE CALLS THE PLAYER'S OWN CLICKS ENTER THROUGH — three rays into
 * InspectAlongRay and one ref into SetInspectedPiece — which is the same entry rule
 * Tests/StructureIntegrationTest.cpp holds itself to. The two inches in front of those, the
 * deprojection that turns a cursor into a ray and the button that supplies a row index, are the
 * ones no test can reach, and they are the reason this photographs the result instead of
 * asserting it.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FPieceMenuScreenshotOpenMenuCommand, FAutomationTestBase*, Test);

bool FPieceMenuScreenshotOpenMenuCommand::Update()
{
	using namespace PieceMenuScreenshotSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world: the map must be named on the command line"));
		return true;
	}

	Test->AddInfo(FString::Printf(TEXT("game world is %s"), *World->GetMapName()));

	/*
	 * THE VIEWPORT IS ASSERTED SEPARATELY, because without one HandleScreenshotCommand returns
	 * having done nothing at all (GameViewportClient.cpp:4286) and the only symptom downstream is
	 * a missing file — which reads identically to a renderer that failed. This says which.
	 */
	UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

	Test->TestNotNull(
		TEXT("there must be a game viewport for Slate to screenshot: -nullrhi must be absent"),
		Viewport);

	FStructureBinding* const Binding = FindScenarioWall(*Test, World);

	if (Binding == nullptr)
	{
		return true;
	}

	Test->TestEqual(
		TEXT("the scenario wall should be the 30 x 40 flush bond the game mode specifies"),
		Binding->NumPieces(), ScenarioWallPieceCount);

	ADestructionGamePlayerController* const Controller =
		Cast<ADestructionGamePlayerController>(World->GetFirstPlayerController());

	if (Controller == nullptr)
	{
		Test->AddError(FString::Printf(
			TEXT("the first player controller should be an ADestructionGamePlayerController, it is %s"),
			*GetNameSafe(World->GetFirstPlayerController())));

		return true;
	}

	/*
	 * THE CAMERA IS PLACED BEFORE ANYTHING IS PICKED, so the settle frames already spent were
	 * spent on this view rather than on PlayerStart's. The pawn's camera component has
	 * bUsePawnControlRotation set, so the control rotation is what aims it.
	 */
	APawn* const Pawn = Controller->GetPawn();

	if (Pawn == nullptr)
	{
		Test->AddError(TEXT("the player controller has no pawn to put the camera on"));
		return true;
	}

	const FVector CameraCm(WallCentreXCm, -CameraStandoffCm, WallCentreZCm);

	Pawn->SetActorLocation(CameraCm);
	Controller->SetControlRotation(FRotator(0.0, CameraYawDegrees, 0.0));

	Test->AddInfo(FString::Printf(
		TEXT("camera placed at (%g, %g, %g) looking along yaw %g at the wall centre"),
		CameraCm.X, CameraCm.Y, CameraCm.Z, CameraYawDegrees));

	/* THE PLAYER'S THREE CLICKS. */
	const double CentreZCm = PickedCentreZCm();

	for (const int32 Brick : PickedBricks)
	{
		const FVector CentreCm(PickedCentreXCm(Brick), 0.0, CentreZCm);

		const int32 Piece = PieceAtBox(*Binding, CentreCm);

		if (Piece == INDEX_NONE)
		{
			Test->AddError(FString::Printf(
				TEXT("the bond puts brick %d of course %d at (%g, %g, %g) and no piece of the wall is laid there"),
				Brick, PickedCourse, CentreCm.X, CentreCm.Y, CentreCm.Z));

			continue;
		}

		const TArray<FPieceMenuRow> Rows = Controller->InspectAlongRay(
			FVector(CentreCm.X, CentreCm.Y - RayReachCm, CentreCm.Z),
			FVector(CentreCm.X, CentreCm.Y + RayReachCm, CentreCm.Z));

		Test->AddInfo(FString::Printf(
			TEXT("pointing at brick %d of course %d, piece %d at (%g, %g, %g), offered %d row(s)"),
			Brick, PickedCourse, Piece, CentreCm.X, CentreCm.Y, CentreCm.Z, Rows.Num()));

		Test->TestTrue(
			*FString::Printf(
				TEXT("a ray into brick %d of course %d must open a menu; it offered %d rows"),
				Brick, PickedCourse, Rows.Num()),
			Rows.Num() > 0);
	}

	/*
	 * THE SELECTION IS THE MECHANISM THAT SAYS THE PICTURE HAS SOMETHING IN IT. A screenshot of a
	 * wall with no menu on it is a perfectly valid PNG of the wrong thing, and the file checks
	 * downstream cannot tell the two apart.
	 */
	const FPieceSelection& Selection = Controller->GetPieceSelection();

	Test->TestEqual(
		TEXT("three rays into three different bricks should leave three bricks selected"),
		Selection.Num(), PickedCount);

	if (Selection.Num() < 2)
	{
		return true;
	}

	/*
	 * AND THE MIDDLE ONE IS SINGLED OUT, which is what hovering its entry in the menu does. It is
	 * what makes the readout draw a support line and a per-joint breakout rather than only a
	 * count and three labels — i.e. it is what puts the thing nobody has ever seen into the
	 * picture.
	 */
	Controller->SetInspectedPiece(Selection.Refs()[1]);

	const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();

	Test->TestTrue(
		*FString::Printf(
			TEXT("the middle selected brick should be inspected so the readout has a breakout to draw; the inspector reports %d selected, inspected=%s, %d joint(s)"),
			Inspector.SelectedCount,
			Inspector.bHasInspectedPiece ? TEXT("true") : TEXT("false"),
			Inspector.Joints.Num()),
		Inspector.bHasInspectedPiece);

	Test->TestTrue(
		*FString::Printf(
			TEXT("the inspected brick should name the second brick picked, {%d,%d}; it names {%d,%d}"),
			Selection.Refs()[1].StructureId, Selection.Refs()[1].PieceIndex,
			Inspector.InspectedRef.StructureId, Inspector.InspectedRef.PieceIndex),
		Inspector.InspectedRef == Selection.Refs()[1]);

	Test->AddInfo(FString::Printf(
		TEXT("readout: '%s' / support '%s' / '%s' over %d joint(s)"),
		*Inspector.CountText, *Inspector.SupportText, *Inspector.JointsText,
		Inspector.Joints.Num()));

	Test->TestTrue(
		TEXT("a menu must be presented at the moment the shot is queued"),
		Controller->IsPieceMenuShown());

	return true;
}

/**
 * Walk the camera in to the brick the readout is about, and change nothing else.
 *
 * THE PAWN TRANSFORM IS THE ONLY DIFFERENCE BETWEEN THE TWO SHOTS, WHICH IS WHAT MAKES THE PAIR
 * WORTH HAVING. Nothing here picks, unpicks, inspects or dismisses: the selection, the inspected
 * brick and the menu are exactly the ones the wide shot was taken of, so the two pictures are one
 * state seen from two distances rather than two states. A close-up that re-picked its own bricks
 * could differ from the wide one in ways nobody would think to look for.
 *
 * IT ASSERTS THE MENU IS STILL UP RATHER THAN ASSUMING IT. Moving a pawn cannot take a menu down
 * — but if something else did, the close-up would be a picture of a wall, and the file checks
 * downstream cannot tell that from a picture of a wall with a panel on it.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FPieceMenuScreenshotMoveCameraInCommand, FAutomationTestBase*, Test);

bool FPieceMenuScreenshotMoveCameraInCommand::Update()
{
	using namespace PieceMenuScreenshotSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	ADestructionGamePlayerController* const Controller = World != nullptr
		? Cast<ADestructionGamePlayerController>(World->GetFirstPlayerController())
		: nullptr;

	APawn* const Pawn = Controller != nullptr ? Controller->GetPawn() : nullptr;

	if (Pawn == nullptr)
	{
		Test->AddError(
			TEXT("there is no pawn to move the camera in on, so no close-up can be framed"));

		return true;
	}

	/* The middle brick of the three, which is the one SetInspectedPiece singled out. */
	const FVector CameraCm(
		PickedCentreXCm(PickedBricks[1]), -CloseUpStandoffCm, PickedCentreZCm());

	Pawn->SetActorLocation(CameraCm);
	Controller->SetControlRotation(FRotator(0.0, CameraYawDegrees, 0.0));

	Test->AddInfo(FString::Printf(
		TEXT("close-up camera placed at (%g, %g, %g), %g cm off the inspected brick, framing about %g cm of wall"),
		CameraCm.X, CameraCm.Y, CameraCm.Z, CloseUpStandoffCm, 2.0 * CloseUpStandoffCm));

	Test->TestTrue(
		TEXT("the menu must still be up for the close-up to be a picture of the same state"),
		Controller->IsPieceMenuShown());

	return true;
}

/**
 * The files landed, they are real PNGs, and no material silently fell back to the checkerboard.
 *
 * THE FILE WAS DELETED BEFORE THE RUN, so its existence can only mean this run wrote it. Without
 * that, a stale PNG from a previous session passes every assertion here while the renderer
 * produced nothing at all — which is the same "green over something invisible" shape the whole
 * test exists to close, reintroduced by the test itself.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FPieceMenuScreenshotCheckFileCommand, FAutomationTestBase*, Test);

bool FPieceMenuScreenshotCheckFileCommand::Update()
{
	using namespace PieceMenuScreenshotSupport;

	for (const TCHAR* const BaseName : ScreenshotBaseNames)
	{
		const FString Path = ScreenshotPathFor(BaseName);

		Test->AddInfo(FString::Printf(TEXT("screenshot should be at %s"), *Path));

		const int64 SizeBytes = IFileManager::Get().FileSize(*Path);

		if (SizeBytes < 0)
		{
			Test->AddError(FString::Printf(
				TEXT("no screenshot was written to %s: the shot request never reached a draw, or the file went somewhere else"),
				*Path));

			continue;
		}

		Test->AddInfo(FString::Printf(TEXT("%s.png is %lld bytes"), BaseName, SizeBytes));

		Test->TestTrue(
			*FString::Printf(
				TEXT("a 1920 x 1080 frame of a lit scene is hundreds of kB and a flat colour is under 10 kB, so %s.png must be at least %lld bytes; it is %lld"),
				BaseName, MinimumScreenshotBytes, SizeBytes),
			SizeBytes >= MinimumScreenshotBytes);

		TArray<uint8> Bytes;

		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 24)
		{
			Test->AddError(FString::Printf(
				TEXT("the screenshot at %s could not be read back, or is too short to carry a PNG header (%d bytes)"),
				*Path, Bytes.Num()));

			continue;
		}

		/*
		 * THE SIGNATURE AND THE IHDR, READ BY HAND. Eight signature bytes, then a four-byte
		 * chunk length, then "IHDR", then width and height as big-endian 32-bit integers. It
		 * is a dozen lines and it is worth them: a byte count alone passes for a file of
		 * random bytes, and a decoder would be a dependency on the very rendering stack under
		 * test.
		 */
		static const uint8 PngSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

		const bool bIsPng = FMemory::Memcmp(Bytes.GetData(), PngSignature, 8) == 0
			&& FMemory::Memcmp(Bytes.GetData() + 12, "IHDR", 4) == 0;

		Test->TestTrue(
			*FString::Printf(
				TEXT("%s.png must begin with the PNG signature and an IHDR chunk; its first bytes are %02x %02x %02x %02x"),
				BaseName, Bytes[0], Bytes[1], Bytes[2], Bytes[3]),
			bIsPng);

		if (bIsPng)
		{
			const auto BigEndian = [&Bytes](int32 At)
			{
				return (static_cast<int32>(Bytes[At]) << 24)
					| (static_cast<int32>(Bytes[At + 1]) << 16)
					| (static_cast<int32>(Bytes[At + 2]) << 8)
					| static_cast<int32>(Bytes[At + 3]);
			};

			const int32 Width = BigEndian(16);
			const int32 Height = BigEndian(20);

			Test->AddInfo(FString::Printf(
				TEXT("%s.png is %d x %d pixels"), BaseName, Width, Height));

			Test->TestTrue(
				*FString::Printf(
					TEXT("%s.png must be a real frame rather than a stub; it is %d x %d and must be at least %d x %d"),
					BaseName, Width, Height, MinimumScreenshotWidth, MinimumScreenshotHeight),
				Width >= MinimumScreenshotWidth && Height >= MinimumScreenshotHeight);
		}
	}

	/*
	 * AND THE MENU IS STILL UP, READ AFTER THE SHOT. The open-menu command asserted it before the
	 * request was queued; this says nothing took it down in the frames between, so whatever is in
	 * the file was photographed with a menu on it.
	 */
	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	ADestructionGamePlayerController* const Controller = World != nullptr
		? Cast<ADestructionGamePlayerController>(World->GetFirstPlayerController())
		: nullptr;

	Test->TestTrue(
		TEXT("the menu must still have been up when the shot was drawn"),
		Controller != nullptr && Controller->IsPieceMenuShown());

	/*
	 * THE LOG SWEEP. Read from the file rather than captured live, because materials submit their
	 * shader compiles while the map loads — long before an automation test starts — so an output
	 * device installed here would miss exactly the failures it was installed for.
	 *
	 * FILEREAD_AllowWrite is not optional: the engine holds this file open for writing, and a
	 * reader that does not share write access cannot open it at all on Windows.
	 */
	if (GLog != nullptr)
	{
		GLog->Flush();
	}

	const FString EngineLogFilename = FPlatformOutputDevices::GetAbsoluteLogFilename();

	FString Log;

	if (!FFileHelper::LoadFileToString(Log, *EngineLogFilename, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite))
	{
		Test->AddWarning(FString::Printf(
			TEXT("could not read %s back, so no material-compilation sweep was made"), *EngineLogFilename));
	}
	else if (Log.Contains(MaterialFailureMarker))
	{
		TArray<FString> Lines;
		Log.ParseIntoArrayLines(Lines);

		for (const FString& Line : Lines)
		{
			if (Line.Contains(MaterialFailureMarker))
			{
				Test->AddError(FString::Printf(
					TEXT("a material fell back to WorldGridMaterial, so the picture is of the checkerboard: %s"),
					*Line.TrimStartAndEnd()));
			}
		}
	}
	else
	{
		Test->AddInfo(TEXT("no material failed to compile: nothing fell back to WorldGridMaterial"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuScreenshotTest,
	"DestructionGame.Visual.PieceMenuScreenshot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::NonNullRHI
		| EAutomationTestFlags::ProductFilter)

bool FPieceMenuScreenshotTest::RunTest(const FString& Parameters)
{
	using namespace PieceMenuScreenshotSupport;

	/*
	 * THE OLD FILES GO FIRST, AND SYNCHRONOUSLY, BEFORE ANY LATENT COMMAND IS QUEUED. Everything
	 * downstream reads "the file exists" as "this run rendered a frame", and that reading is only
	 * true if the file cannot have survived from an earlier run. Both shots, for the same reason:
	 * a close-up left over from the last session is exactly as misleading as a wide one.
	 */
	for (const TCHAR* const BaseName : ScreenshotBaseNames)
	{
		const FString Path = ScreenshotPathFor(BaseName);

		if (IFileManager::Get().FileExists(*Path))
		{
			IFileManager::Get().Delete(*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
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
	 * THE SEQUENCE, AND THE SHAPE OF IT IS "SET THE VIEW UP, THEN LET IT SETTLE, THEN SHOOT" —
	 * TWICE. The second shot differs from the first only in where the pawn is standing, so
	 * everything about the state being photographed is established once, by the open-menu command,
	 * and never touched again.
	 *
	 * SCREEN MESSAGES ARE TURNED OFF BEFORE ANYTHING IS WAITED ON, and each shot gets its OWN
	 * shader drain and its own settle — see the timings block for why one at the top was not
	 * enough. The write wait after each `Shot` is because ProcessScreenShots writes at end of draw.
	 */
	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(DisableScreenMessagesCommand));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FPieceMenuScreenshotOpenMenuCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(WideScreenshotCommand));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FPieceMenuScreenshotMoveCameraInCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(CloseUpScreenshotCommand));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FPieceMenuScreenshotCheckFileCommand(this));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
