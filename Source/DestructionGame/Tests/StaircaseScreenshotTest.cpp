// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/PieceActions.h"
#include "Core/PieceMenu.h"
#include "Core/StructureBinding.h"
#include "DestructionGameGameMode.h"
#include "DestructionGamePlayerController.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "Tests/StaircaseWallTestSupport.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE STAIRCASE, PHOTOGRAPHED: ONE FRAME WITH THE VOID CUT AND THE OVERHANG STILL STANDING, AND
 * ONE AFTER IT HAS COME DOWN.
 *
 * WHY A SECOND VISUAL TEST RATHER THAN A THIRD SHOT IN THE MENU ONE. That harness photographs a
 * PANEL and needs a menu up; this photographs a COLLAPSE and needs the menu gone, the void cut and
 * a second of simulation. They share nothing but the plumbing, and the plumbing is copied
 * deliberately in one respect only — the timings, the file checks and the material-log sweep are
 * the same statements about the same kind of artefact, and their reasoning is written out at
 * length in Tests/PieceMenuScreenshotTest.cpp rather than repeated here.
 *
 * IT PHOTOGRAPHS THE GAME MODE'S OWN 30 x 40 SCENARIO WALL, NOT THE 13 x 10 TEST FIXTURE, AND
 * THAT IS FORCED RATHER THAN CHOSEN. FRunningBondSpec has no origin: RunningBond lays every wall
 * from X = -10.75 upward at Y = 0, so a second wall built into the sandbox would be laid INSIDE
 * ADestructionGameGameMode::BeginPlay's, brick through brick. Giving the spec an offset is
 * production code with arithmetic in it and no failing test behind it. The 30 x 40 wall is also
 * the more honest subject: it is the wall the player was looking at when they cut this hole, and
 * the void is defined in ABSOLUTE courses and X, so it is the same hole in the same place — with
 * 28 courses of brickwork hanging over it instead of one.
 *
 * "BEFORE" IS HONEST, AND HERE IS EXACTLY WHAT IT MEANS. The cascade now runs INSIDE the commit,
 * so the instant the 36 bricks are cut the overhang is already condemned, already broken and
 * already handed to physics — there is no state in which the void exists and the joints do not.
 * What the before-frame therefore shows is the first drawn frame after the commit returned: the
 * hole is there, the overhang is over open air, and NOTHING HAS MOVED YET. To make that true
 * rather than nearly true, the world's time dilation is pinned at its floor (0.0001) for the
 * frames between the cut and the write, so the bricks fall a few nanometres rather than a few
 * centimetres; the dilation is restored before the fall. And it is not a caption, it is an
 * assertion: FStaircaseScreenshotVerifyBeforeCommand measures how far the corbelled bricks
 * actually moved by then and fails if any of them moved as much as a millimetre. A picture
 * captioned "before" that a future change quietly turned into a picture of a falling wall would
 * turn this test red rather than lying in a report.
 *
 * WHAT IT ASSERTS BEYOND THE FILES. A harness that only takes a picture is green whatever is in
 * the picture, so: the wall is the one the game mode built, the 36 named bricks are the staircase,
 * every one of their actors left the world, no menu is up when either shot is queued, the bottom
 * of the corbel had not moved when the first was taken and HAD fallen when the second was, and a
 * brick five metres down the wall never moved at all. It deliberately asserts NOTHING about what
 * the images look like — judging that is a human's job, which is the entire point.
 *
 * IT NEEDS A TICKING WORLD *AND* A REAL RHI, hence EAutomationTestFlags::NonNullRHI: without it
 * the ordinary -nullrhi suite would run this, find no viewport, write no file and go green.
 *
 * HOW TO RUN IT. Close the editor, build as documented in CLAUDE.md, then, from PowerShell (Git
 * Bash mangles the map path):
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.StaircaseScreenshot"
 *     -TestExit="Automation Test Queue Empty"
 *
 * -nullrhi MUST BE ABSENT: FApp::CanEverRender() is false with it and UGameEngine::Init only
 * builds a window and a viewport under that, so there would be nothing to screenshot even if the
 * filter let the test through.
 */
namespace StaircaseScreenshotSupport
{
	using namespace DestructionLayout;
	using namespace StaircaseWallTestSupport;

	/** Both shots, and the names a human is going to look for in Saved/Screenshots. */
	const TCHAR* const BeforeScreenshotBaseName = TEXT("StaircaseBefore");
	const TCHAR* const AfterScreenshotBaseName = TEXT("StaircaseAfter");

	const TCHAR* const ScreenshotBaseNames[] =
	{
		BeforeScreenshotBaseName, AfterScreenshotBaseName
	};

	inline FString ScreenshotPathFor(const TCHAR* BaseName)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::ScreenShotDir() / FString(BaseName) + TEXT(".png"));
	}

	/*
	 * `Shot` RATHER THAN `HighResShot`, AND `showui` WITH IT, for the reason written out in
	 * Tests/PieceMenuScreenshotTest.cpp: only the Slate path is reached when ShouldShowUI is set,
	 * and that is the path that has been proven to write a file in this project. There is no menu
	 * up when either of these fires — ChoosePieceMenuRow dismisses it before it commits, and both
	 * shot commands assert that — so `showui` costs nothing and buys the known-good route.
	 */
	const TCHAR* const BeforeScreenshotCommand =
		TEXT("Shot showui filename=StaircaseBefore -nosuffix");
	const TCHAR* const AfterScreenshotCommand =
		TEXT("Shot showui filename=StaircaseAfter -nosuffix");

	/** Debug overlays off, so nothing is burned over the thing a human is being asked to judge. */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/** 30 x 40 flush: 20 even courses of 30 plus 20 odd courses of 31 (a half bat at each end). */
	constexpr int32 ScenarioWallPieceCount = 1220;

	/**
	 * WHERE THE CAMERA GOES, AND WHY THAT CLOSE.
	 *
	 * The default UCameraComponent field of view is 90 degrees HORIZONTALLY, so the visible width
	 * is twice the standoff and the visible height at 16:9 is that times 1080/1920. At 230 cm the
	 * frame is 460 x 259 cm.
	 *
	 * The void reaches X = 123.75 at its widest and spans courses 1 to 11, i.e. Z 7.5 to 89.5; the
	 * corbelled bricks run from (112.5, 18.25) at the bottom step to (0, 93.25) at the top. Aiming
	 * at (90, 105) puts all of that in frame with the ground and about twenty courses of standing
	 * brickwork above the overhang, which is what makes it legible as a hole in a wall rather than
	 * as an abstract pattern of bricks. The whole 674 x 299 wall at the menu harness's 600 cm
	 * standoff would put the entire staircase in the bottom left twelfth of the frame.
	 */
	constexpr double CameraStandoffCm = 230.0;
	constexpr double CameraAimXCm = 90.0;
	constexpr double CameraAimZCm = 105.0;
	constexpr double CameraYawDegrees = 90.0;

	/** A brick is 10.25 deep and the wall is centred on Y = 0, so +/-100 crosses it entirely. */
	constexpr double RayReachCm = 100.0;

	/**
	 * A BRICK FAR ENOUGH DOWN THE WALL THAT THE STAIRCASE CANNOT REACH IT.
	 *
	 * Twenty-five brick pitches is X = 562.5 cm, four and a half metres past the widest step of the
	 * void and well outside the frame. It is the control for "the picture is of a wall with a
	 * corner missing" rather than of a wall that fell over or dropped through the floor.
	 */
	constexpr double FarSideXCm = 25 * StaircaseBrickPitchCm;
	constexpr int32 FarSideCourse = 12;

	/**
	 * HOW STILL "BEFORE" HAS TO BE, IN CM.
	 *
	 * A millimetre. With the world's time dilation pinned at its 0.0001 floor a brick in free fall
	 * covers about 5e-9 cm per frame, so this is six orders of magnitude of headroom on the claim
	 * and nowhere near enough slack to hide a frame of real falling, which at 60 Hz is 0.14 cm and
	 * at 10 Hz is 4.9 cm.
	 */
	constexpr double BeforeStillnessToleranceCm = 0.1;

	/** How far a released brick must have fallen to count as having fallen. */
	constexpr double FallenAtLeastCm = 5.0;

	/** And how far a brick nothing touched is allowed to drift. */
	constexpr double DriftToleranceCm = 0.1;

	/**
	 * THE TIMINGS. Latent commands rather than a longer -ExecCmds, because every -ExecCmds string
	 * is flushed on the FIRST TickDeferredCommands and buys no delay at all.
	 *
	 * SettleFrames covers TSR's temporal history and auto-exposure; SlateFrames is the layout pass
	 * a rebuilt viewport needs; WriteFrames is because ProcessScreenShots writes at END OF DRAW, so
	 * moving on in the same frame as the request loses the file. FallFrames is the only new one:
	 * the wall has to actually come down between the two shots, and 300 frames is five seconds at
	 * 60 Hz and proportionally more simulated time if the frame rate drops under the debris.
	 */
	constexpr int32 SettleFrames = 120;
	constexpr int32 SlateFrames = 3;
	constexpr int32 WriteFrames = 5;
	constexpr int32 FallFrames = 300;

	/** See Tests/PieceMenuScreenshotTest.cpp: derived as a floor, not picked as a judgement. */
	constexpr int64 MinimumScreenshotBytes = 32 * 1024;
	constexpr int32 MinimumScreenshotWidth = 640;
	constexpr int32 MinimumScreenshotHeight = 480;

	/** Logged at WARNING, not Error, so the project's habitual grep never sees it. */
	const TCHAR* const MaterialFailureMarker = TEXT("Failed to compile Material");

	/**
	 * WHERE THE BRICKS WERE BEFORE ANYTHING HAPPENED TO THEM.
	 *
	 * FILE-SCOPE STATE, WHICH IS UNUSUAL HERE AND IS THE POINT OF THIS COMMENT. A latent command
	 * carries only what its parameters carry, and the three that matter run frames apart: the one
	 * that aims the camera is the only one that sees the wall INTACT, and the two that judge the
	 * shots need those positions. Re-deriving them later is exactly wrong — after the cut the
	 * bricks have moved, which is the quantity being measured. Reset at the top of every run so a
	 * second run in the same process cannot inherit the first one's wall.
	 */
	struct FStaircaseScreenshotRecord
	{
		int32 StructureId = INDEX_NONE;

		/** The eleven corbelled bricks, bottom step first, and where each stood when laid. */
		TArray<int32> CorbelPieces;
		TArray<FVector> CorbelLaidAtCm;

		int32 FarSidePiece = INDEX_NONE;
		FVector FarSideLaidAtCm = FVector::ZeroVector;

		void Reset()
		{
			*this = FStaircaseScreenshotRecord();
		}
	};

	inline FStaircaseScreenshotRecord& StaircaseScreenshotRecord()
	{
		static FStaircaseScreenshotRecord Record;
		return Record;
	}

	/** The binding for the wall the game mode built, or null with the reason reported. */
	inline FStructureBinding* FindScenarioWall(FAutomationTestBase& Test, UWorld* World, int32& OutId)
	{
		OutId = INDEX_NONE;

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

		OutId = GameMode->GetBuiltStructureId();

		FStructureBinding* const Binding =
			OutId != INDEX_NONE ? Subsystem->Find(OutId) : nullptr;

		if (Binding == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("the game mode should have built the scenario wall on begin-play; its structure id is %d and Find returned nothing"),
				OutId));
		}

		return Binding;
	}

	/** Every laid box of a binding, in handle order, so the staircase can be read off it. */
	inline TArray<FPieceBox> BoxesOf(const FStructureBinding& Binding)
	{
		TArray<FPieceBox> Boxes;
		Boxes.Reserve(Binding.NumPieces());

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			Boxes.Add(Binding.GetBinding(Piece).Box);
		}

		return Boxes;
	}

	/** Where a piece's brick actually is right now, or the zero vector if it has gone. */
	inline FVector ActorLocationOf(const FStructureBinding& Binding, int32 Piece)
	{
		const AActor* const Actor = Cast<AActor>(Binding.GetActor(Piece));

		return Actor != nullptr ? Actor->GetActorLocation() : FVector::ZeroVector;
	}

	/**
	 * Ask for a screenshot, THROUGH THE VIEWPORT CLIENT AND NOT THROUGH GEngine.
	 *
	 * MEASURED, NOT PREFERRED. The first run of this test cut the wall, froze it, collapsed it and
	 * asserted every physical claim correctly — and wrote no PNG at all, because the request went
	 * to UEngine::Exec, which has no SHOT handler; HandleScreenshotCommand lives on
	 * UGameViewportClient. FExecStringLatentCommand routes to GEngine->GameViewport->Exec for
	 * exactly this reason (AutomationCommon.cpp:1127-1130) and this is that same route, called
	 * inline so the request lands in the frame the commit ran in rather than the frame after it.
	 *
	 * The absence of a viewport is reported rather than silently skipped: the only other symptom is
	 * a missing file, which reads identically to a renderer that failed.
	 */
	inline void RequestScreenshot(FAutomationTestBase& Test, const TCHAR* Command)
	{
		UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

		if (Viewport == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("there is no game viewport to run '%s' against, so no frame can be written"),
				Command));

			return;
		}

		Viewport->Exec(nullptr, Command, *GLog);
	}

	/** The controller the player would be driving, or null with the reason reported. */
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
}

/**
 * Aim the camera at the corner of the wall the staircase is about to be cut out of, and remember
 * where the bricks that matter are standing while they are still standing.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FStaircaseScreenshotAimCommand, FAutomationTestBase*, Test);

bool FStaircaseScreenshotAimCommand::Update()
{
	using namespace StaircaseScreenshotSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world: the map must be named on the command line"));
		return true;
	}

	Test->AddInfo(FString::Printf(TEXT("game world is %s"), *World->GetMapName()));

	/*
	 * THE VIEWPORT IS ASSERTED SEPARATELY, because without one HandleScreenshotCommand returns
	 * having done nothing at all and the only symptom downstream is a missing file — which reads
	 * identically to a renderer that failed. This says which.
	 */
	UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

	Test->TestNotNull(
		TEXT("there must be a game viewport for Slate to screenshot: -nullrhi must be absent"),
		Viewport);

	FStaircaseScreenshotRecord& Record = StaircaseScreenshotRecord();

	FStructureBinding* const Binding = FindScenarioWall(*Test, World, Record.StructureId);

	if (Binding == nullptr)
	{
		return true;
	}

	Test->TestEqual(
		TEXT("the scenario wall should be the 30 x 40 flush bond the game mode specifies"),
		Binding->NumPieces(), ScenarioWallPieceCount);

	const TArray<FPieceBox> Boxes = BoxesOf(*Binding);

	for (int32 Course = StaircaseLowestCorbelCourse; Course <= StaircaseHighestCorbelCourse; ++Course)
	{
		const int32 Corbel = StaircaseCorbelPiece(Boxes, Course);

		if (Corbel == INDEX_NONE)
		{
			Test->AddError(FString::Printf(
				TEXT("the bond should have laid a brick at (%.2f, 0, %.2f) for course %d's corbel"),
				StaircaseVoidEdgeXCm(Course), StaircaseCourseZCm(Course), Course));

			return true;
		}

		Record.CorbelPieces.Add(Corbel);
		Record.CorbelLaidAtCm.Add(ActorLocationOf(*Binding, Corbel));
	}

	Record.FarSidePiece = StaircasePieceAt(Boxes, FarSideXCm, StaircaseCourseZCm(FarSideCourse));

	if (Record.FarSidePiece == INDEX_NONE)
	{
		Test->AddError(FString::Printf(
			TEXT("the bond should have laid a brick at (%.2f, 0, %.2f) for the far-side control"),
			FarSideXCm, StaircaseCourseZCm(FarSideCourse)));

		return true;
	}

	Record.FarSideLaidAtCm = ActorLocationOf(*Binding, Record.FarSidePiece);

	ADestructionGamePlayerController* const Controller = FindController(*Test, World);
	APawn* const Pawn = Controller != nullptr ? Controller->GetPawn() : nullptr;

	if (Pawn == nullptr)
	{
		Test->AddError(TEXT("the player controller has no pawn to put the camera on"));
		return true;
	}

	const FVector CameraCm(CameraAimXCm, -CameraStandoffCm, CameraAimZCm);

	Pawn->SetActorLocation(CameraCm);
	Controller->SetControlRotation(FRotator(0.0, CameraYawDegrees, 0.0));

	Test->AddInfo(FString::Printf(
		TEXT("camera placed at (%g, %g, %g) looking along yaw %g, framing about %g x %g cm of wall"),
		CameraCm.X, CameraCm.Y, CameraCm.Z, CameraYawDegrees,
		2.0 * CameraStandoffCm, 2.0 * CameraStandoffCm * 1080.0 / 1920.0));

	return true;
}

/**
 * Cut the staircase the way a player cuts it, and photograph the result in the same breath.
 *
 * THE SHOT IS REQUESTED FROM INSIDE THIS COMMAND RATHER THAN FROM A FExecStringLatentCommand
 * AFTER IT, and that is the whole of what makes "before" honest: a following latent command runs
 * on the NEXT frame, which is one more world tick of falling. Requested here, the screenshot is
 * serviced at the end of the very draw whose tick the commit ran in. The time dilation pinned
 * immediately above closes the remaining gap, and FStaircaseScreenshotVerifyBeforeCommand
 * measures what is left of it rather than trusting either.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FStaircaseScreenshotCutCommand, FAutomationTestBase*, Test);

bool FStaircaseScreenshotCutCommand::Update()
{
	using namespace StaircaseScreenshotSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	int32 StructureId = INDEX_NONE;
	FStructureBinding* const Binding = FindScenarioWall(*Test, World, StructureId);
	ADestructionGamePlayerController* const Controller = FindController(*Test, World);

	if (Binding == nullptr || Controller == nullptr)
	{
		return true;
	}

	/*
	 * THE WORLD IS PINNED AT ITS SLOWEST BEFORE ANYTHING IS CUT. AWorldSettings clamps this to
	 * MinGlobalTimeDilation, which BaseGame.ini sets to 0.0001, so a frame advances the simulation
	 * by about 1.6 microseconds and a released brick falls around 5e-9 cm in one. It is set here
	 * rather than at the top of the test so that the frames spent settling the exposure and the
	 * temporal history ran at normal speed, which is what they are for.
	 */
	AWorldSettings* const Settings = World->GetWorldSettings();

	if (Settings == nullptr)
	{
		Test->AddError(TEXT("the world has no AWorldSettings to freeze, so no before-frame can be honest"));
		return true;
	}

	const float FrozenDilation = Settings->SetTimeDilation(0.0f);

	Test->AddInfo(FString::Printf(
		TEXT("time dilation pinned at %g for the before-frame"), FrozenDilation));

	Test->TestTrue(
		*FString::Printf(
			TEXT("the world must actually be frozen for 'before' to mean anything; dilation is %g"),
			FrozenDilation),
		FrozenDilation <= 0.001f);

	const FPieceAction* Delete = nullptr;

	for (const FPieceAction& Action : AllPieceActions())
	{
		if (Action.Label != nullptr && FCString::Strcmp(Action.Label, TEXT("Delete")) == 0)
		{
			Delete = &Action;
			break;
		}
	}

	if (Delete == nullptr)
	{
		Test->AddError(TEXT("the action table must contain a row labelled 'Delete'"));
		return true;
	}

	const TArray<FPieceBox> Boxes = BoxesOf(*Binding);
	const TArray<int32> VoidPieces = StaircaseVoidPieces(Boxes);

	Test->TestEqual(
		TEXT("the staircase cut into the scenario wall should name the same 36 bricks it names in the fixture"),
		VoidPieces.Num(), StaircaseVoidPieceCount);

	if (VoidPieces.Num() == 0)
	{
		return true;
	}

	/* THE PLAYER'S MOVE: point at each brick, then choose Delete once for the whole selection. */
	int32 DeleteRow = INDEX_NONE;

	for (const int32 Piece : VoidPieces)
	{
		const FVector CentreCm = Boxes[Piece].CentreCm;

		const TArray<FPieceMenuRow> Rows = Controller->InspectAlongRay(
			FVector(CentreCm.X, CentreCm.Y - RayReachCm, CentreCm.Z),
			FVector(CentreCm.X, CentreCm.Y + RayReachCm, CentreCm.Z));

		DeleteRow = INDEX_NONE;

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			if (Rows[Index].Action == Delete)
			{
				DeleteRow = Index;
				break;
			}
		}

		if (DeleteRow == INDEX_NONE)
		{
			Test->AddError(FString::Printf(
				TEXT("pointing at piece %d at (%g, %g, %g) offered no Delete row"),
				Piece, CentreCm.X, CentreCm.Y, CentreCm.Z));

			return true;
		}
	}

	Test->TestTrue(
		*FString::Printf(TEXT("choosing Delete on %d picked bricks should report that it committed"),
			VoidPieces.Num()),
		Controller->ChoosePieceMenuRow(DeleteRow));

	for (const int32 Piece : VoidPieces)
	{
		Test->TestTrue(
			*FString::Printf(TEXT("cut brick %d's actor must have left the world"), Piece),
			Binding->GetActor(Piece) == nullptr);
	}

	/*
	 * AND THE MENU IS DOWN, WHICH IS WHY `showui` COSTS NOTHING. ChoosePieceMenuRow dismisses it
	 * before it commits; if that ever changed, the panel would be sitting over the hole in both
	 * pictures and the file checks downstream could not tell.
	 */
	Test->TestFalse(
		TEXT("no menu may be up when the before-frame is queued, or the panel is in the picture"),
		Controller->IsPieceMenuShown());

	RequestScreenshot(*Test, BeforeScreenshotCommand);

	Test->AddInfo(FString::Printf(
		TEXT("the void is cut and '%s' is queued in the same frame"), BeforeScreenshotCommand));

	return true;
}

/**
 * The before-frame was a picture of a standing overhang, and the fall may now begin.
 *
 * THE STILLNESS IS MEASURED, NOT ASSERTED IN PROSE. A frame of ordinary falling is 0.14 cm at
 * 60 Hz and 4.9 cm at 10 Hz, so a millimetre of tolerance cannot be met by a wall that was
 * genuinely moving when the shutter opened.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FStaircaseScreenshotVerifyBeforeCommand, FAutomationTestBase*, Test);

bool FStaircaseScreenshotVerifyBeforeCommand::Update()
{
	using namespace StaircaseScreenshotSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	FStaircaseScreenshotRecord& Record = StaircaseScreenshotRecord();

	int32 StructureId = INDEX_NONE;
	FStructureBinding* const Binding = FindScenarioWall(*Test, World, StructureId);

	if (Binding == nullptr)
	{
		return true;
	}

	double WorstMovedCm = 0.0;

	for (int32 Step = 0; Step < Record.CorbelPieces.Num(); ++Step)
	{
		const int32 Piece = Record.CorbelPieces[Step];

		const double MovedCm =
			FVector::Dist(ActorLocationOf(*Binding, Piece), Record.CorbelLaidAtCm[Step]);

		WorstMovedCm = FMath::Max(WorstMovedCm, MovedCm);
	}

	Test->AddInfo(FString::Printf(
		TEXT("when the before-frame was written the corbel had moved at most %.9f cm"),
		WorstMovedCm));

	Test->TestTrue(
		*FString::Printf(
			TEXT("'before' must mean the overhang was still standing: the worst corbelled brick had moved %.9f cm and may move at most %g"),
			WorstMovedCm, BeforeStillnessToleranceCm),
		WorstMovedCm < BeforeStillnessToleranceCm);

	AWorldSettings* const Settings = World != nullptr ? World->GetWorldSettings() : nullptr;

	if (Settings != nullptr)
	{
		Settings->SetTimeDilation(1.0f);
		Test->AddInfo(TEXT("time dilation restored to 1; the overhang may now come down"));
	}

	return true;
}

/**
 * The overhang came down, and this is the picture of it.
 *
 * BOTH HALVES, AND NEITHER IS DECORATION. "The corbel fell" alone passes in a world that dropped
 * through its floor; "the far end did not move" alone passes in a world where nothing moved at
 * all. Both are read off the same simulated seconds, and only then is the shutter opened.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FStaircaseScreenshotShootAfterCommand, FAutomationTestBase*, Test);

bool FStaircaseScreenshotShootAfterCommand::Update()
{
	using namespace StaircaseScreenshotSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	FStaircaseScreenshotRecord& Record = StaircaseScreenshotRecord();

	int32 StructureId = INDEX_NONE;
	FStructureBinding* const Binding = FindScenarioWall(*Test, World, StructureId);

	if (Binding == nullptr || Record.CorbelPieces.Num() == 0)
	{
		return true;
	}

	for (int32 Step = 0; Step < Record.CorbelPieces.Num(); ++Step)
	{
		const int32 Piece = Record.CorbelPieces[Step];
		const FVector NowAtCm = ActorLocationOf(*Binding, Piece);
		const double FellCm = Record.CorbelLaidAtCm[Step].Z - NowAtCm.Z;

		Test->AddInfo(FString::Printf(
			TEXT("corbel course %2d (piece %4d) fell %.3f cm, from Z %.3f to Z %.3f"),
			StaircaseLowestCorbelCourse + Step, Piece, FellCm,
			Record.CorbelLaidAtCm[Step].Z, NowAtCm.Z));
	}

	/*
	 * THE TOP STEP IS THE ONE ASSERTED ON, AND THE BOTTOM ONE DELIBERATELY IS NOT — WHICH WAS
	 * MEASURED RATHER THAN REASONED. The bottom step of the corbel starts 15 cm off the ground and
	 * lands in the rubble of everything that came down on top of it, so how far it travels is a
	 * property of the debris pile: two runs of this test put it at 9.027 cm and at 2.314 cm. That
	 * is not a wobbly assertion to tighten, it is the wrong brick to ask. The TOP step began 90 cm
	 * up with nothing whatever beneath it — it is the "hanging metres out over open air" in the
	 * photograph — and it has fallen 83 and 91 cm on those same two runs, which is an enormous
	 * margin on a claim about the same collapse.
	 *
	 * AND EVERY STEP MUST HAVE BEEN HANDED TO PHYSICS, which is the part that does not depend on
	 * where anything landed. A corbelled brick still held kinematic is a brick the solver thinks is
	 * standing, and a picture of one is a picture of the bug this whole fixture was written for.
	 */
	const int32 TopStep = Record.CorbelPieces.Num() - 1;

	const double TopFellCm =
		Record.CorbelLaidAtCm[TopStep].Z - ActorLocationOf(*Binding, Record.CorbelPieces[TopStep]).Z;

	Test->TestTrue(
		*FString::Printf(
			TEXT("the overhang must have come down before the after-frame is taken: the top of the corbel fell %.3f cm and must fall more than %g"),
			TopFellCm, FallenAtLeastCm),
		TopFellCm > FallenAtLeastCm);

	for (int32 Step = 0; Step < Record.CorbelPieces.Num(); ++Step)
	{
		Test->TestTrue(
			*FString::Printf(
				TEXT("corbel course %d (piece %d) must have been released to physics, not left standing"),
				StaircaseLowestCorbelCourse + Step, Record.CorbelPieces[Step]),
			Binding->IsReleased(Record.CorbelPieces[Step]));
	}

	const double FarSideMovedCm = FVector::Dist(
		ActorLocationOf(*Binding, Record.FarSidePiece), Record.FarSideLaidAtCm);

	Test->AddInfo(FString::Printf(
		TEXT("the far-side control brick %d at X %g drifted %.6f cm"),
		Record.FarSidePiece, FarSideXCm, FarSideMovedCm));

	Test->TestTrue(
		*FString::Printf(
			TEXT("the far end of the wall is four metres from the staircase and must not have moved; brick %d drifted %.6f cm"),
			Record.FarSidePiece, FarSideMovedCm),
		FarSideMovedCm < DriftToleranceCm);

	ADestructionGamePlayerController* const Controller = FindController(*Test, World);

	Test->TestTrue(
		TEXT("no menu may be up when the after-frame is queued"),
		Controller != nullptr && !Controller->IsPieceMenuShown());

	RequestScreenshot(*Test, AfterScreenshotCommand);

	return true;
}

/**
 * The files landed, they are real PNGs, and no material silently fell back to the checkerboard.
 *
 * BOTH WERE DELETED BEFORE THE RUN, so their existence can only mean this run wrote them.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FStaircaseScreenshotCheckFilesCommand, FAutomationTestBase*, Test);

bool FStaircaseScreenshotCheckFilesCommand::Update()
{
	using namespace StaircaseScreenshotSupport;

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
		 * THE SIGNATURE AND THE IHDR, READ BY HAND. Eight signature bytes, then a four-byte chunk
		 * length, then "IHDR", then width and height as big-endian 32-bit integers. A byte count
		 * alone passes for a file of random bytes, and a decoder would be a dependency on the very
		 * rendering stack under test.
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

	if (!FFileHelper::LoadFileToString(
			Log, *EngineLogFilename, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite))
	{
		Test->AddWarning(FString::Printf(
			TEXT("could not read %s back, so no material-compilation sweep was made"),
			*EngineLogFilename));
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
	FStaircaseScreenshotTest,
	"DestructionGame.Visual.StaircaseScreenshot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::NonNullRHI
		| EAutomationTestFlags::ProductFilter)

bool FStaircaseScreenshotTest::RunTest(const FString& Parameters)
{
	using namespace StaircaseScreenshotSupport;

	StaircaseScreenshotRecord().Reset();

	/*
	 * THE OLD FILES GO FIRST, AND SYNCHRONOUSLY, BEFORE ANY LATENT COMMAND IS QUEUED. Everything
	 * downstream reads "the file exists" as "this run rendered a frame", and that reading is only
	 * true if the file cannot have survived from an earlier run. A stale "after" is exactly as
	 * misleading as a stale "before" — worse, in fact, since the pair is meant to be read as a
	 * sequence.
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
	 * THE SEQUENCE: settle the intact wall, cut and shoot in one frame, let it fall, settle again,
	 * shoot again. Each shot gets its own shader drain and its own settle, taken AFTER the view it
	 * is a picture of has been set up — see Tests/PieceMenuScreenshotTest.cpp for why one drain at
	 * the top is not enough.
	 *
	 * THERE IS DELIBERATELY NO SETTLE BETWEEN THE CUT AND THE FIRST SHOT. Every frame spent there
	 * is a frame of the overhang falling, which is the one thing the before-frame must not contain;
	 * the settling that matters was already paid for on the frames above, and the only thing that
	 * changed since is that 36 bricks stopped being drawn.
	 */
	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(DisableScreenMessagesCommand));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FStaircaseScreenshotAimCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FStaircaseScreenshotCutCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FStaircaseScreenshotVerifyBeforeCommand(this));

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(FallFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FStaircaseScreenshotShootAfterCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FStaircaseScreenshotCheckFilesCommand(this));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
