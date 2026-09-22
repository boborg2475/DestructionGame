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
 * The staircase void photographed: one frame right after the cut, one five simulated seconds
 * later with the overhang unmoved.
 *
 * Warning: NonNullRHI keeps this out of the standard -nullrhi suite, so it can rot unnoticed. It
 * did once, asserting the overhang fell for five slices after arching slice 5 (2026-08-07) made
 * it stand. Run it deliberately (command below); CURRENT_STATE.md carries the same warning.
 *
 * It photographs the game mode's 30 x 40 wall, not the test fixture: RunningBond has no origin,
 * so a second wall would overlap the first. Plumbing (timings, file checks, material-log sweep)
 * is explained in Tests/PieceMenuScreenshotTest.cpp.
 *
 * The wall acts as a deep beam over the cut: the bottom rung reads 0.369 of capacity, not 22.93,
 * so nothing moves and the two frames look the same. Non-movement is a valid assertion (unlike
 * movement as proof of a break, DESIGN.md §4), but a released brick could jam in place, so
 * IsReleased is also checked. Elapsed world time is asserted so a frozen world cannot pass.
 *
 * The before-frame is taken with time dilation pinned at its 0.0001 floor, so it shows the cut
 * before anything could move, and its stillness is measured (under 1 mm). Keep the freeze: it is
 * what distinguishes "standing" from "not started falling yet".
 *
 * Also asserted: the game mode's wall, the 36 staircase bricks cut and their actors gone, no menu
 * in either shot, and a far-side control brick unmoved. Image content is for a human to judge.
 *
 * To run (PowerShell; Git Bash mangles the map path), after closing the editor and building:
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.StaircaseScreenshot"
 *     -TestExit="Automation Test Queue Empty"
 *
 * -nullrhi must be absent, or no viewport is created and there is nothing to screenshot.
 */
namespace StaircaseScreenshotSupport
{
	using namespace DestructionLayout;
	using namespace StaircaseWallTestSupport;

	/** Screenshot names in Saved/Screenshots. */
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
	 * `Shot showui` rather than HighResShot: the Slate path is the one proven to write a file
	 * (see Tests/PieceMenuScreenshotTest.cpp). No menu is up, so showui adds nothing to the image.
	 */
	const TCHAR* const BeforeScreenshotCommand =
		TEXT("Shot showui filename=StaircaseBefore -nosuffix");
	const TCHAR* const AfterScreenshotCommand =
		TEXT("Shot showui filename=StaircaseAfter -nosuffix");

	/** Debug overlays off so nothing is drawn over the image. */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/** 30 x 40 flush: 20 even courses of 30 plus 20 odd courses of 31 (a half bat at each end). */
	constexpr int32 ScenarioWallPieceCount = 1220;

	/**
	 * Camera placement. With a 90 degree horizontal FOV at 16:9, a 230 cm standoff frames 460 x
	 * 259 cm. Aiming at (90, 105) frames the void (to X = 123.75, courses 1 to 11), the ground, and
	 * about twenty courses above.
	 */
	constexpr double CameraStandoffCm = 230.0;
	constexpr double CameraAimXCm = 90.0;
	constexpr double CameraAimZCm = 105.0;
	constexpr double CameraYawDegrees = 90.0;

	/** A brick is 10.25 deep and the wall is centred on Y = 0, so +/-100 crosses it entirely. */
	constexpr double RayReachCm = 100.0;

	/** Control brick at X = 562.5 cm, far from the staircase; it must never move. */
	constexpr double FarSideXCm = 25 * StaircaseBrickPitchCm;
	constexpr int32 FarSideCourse = 12;

	/**
	 * Max movement before the before-frame, cm. Frozen, a falling brick moves about 5e-9 cm per
	 * frame; one real frame of falling is 0.14 cm at 60 Hz.
	 */
	constexpr double BeforeStillnessToleranceCm = 0.1;

	/** Max drift over the whole five seconds for a held brick. A released brick falls metres. */
	constexpr double DriftToleranceCm = 0.1;

	/**
	 * Minimum world time between the frames, so a stopped world cannot pass the non-movement
	 * checks. A floor, not an expectation: still three times the 0.32 s a brick needs to fall 50 cm.
	 */
	constexpr double MinimumElapsedSeconds = 0.5;

	/**
	 * Frame waits, as latent commands since -ExecCmds gives no delay. SettleFrames covers TSR
	 * history and auto-exposure; SlateFrames a layout pass; WriteFrames because the PNG is written
	 * at end of draw; FallFrames (5 s at 60 Hz) gives the wall every chance to come down.
	 */
	constexpr int32 SettleFrames = 120;
	constexpr int32 SlateFrames = 3;
	constexpr int32 WriteFrames = 5;
	constexpr int32 FallFrames = 300;

	/** Floors derived in Tests/PieceMenuScreenshotTest.cpp. */
	constexpr int64 MinimumScreenshotBytes = 32 * 1024;
	constexpr int32 MinimumScreenshotWidth = 640;
	constexpr int32 MinimumScreenshotHeight = 480;

	/** Logged as a warning, so the usual error grep misses it. */
	const TCHAR* const MaterialFailureMarker = TEXT("Failed to compile Material");

	/**
	 * Laid positions recorded by the aim command, which alone sees the intact wall, for the later
	 * commands to measure against. File-scope because latent commands share nothing else. Reset
	 * at the start of every run.
	 */
	struct FStaircaseScreenshotRecord
	{
		int32 StructureId = INDEX_NONE;

		/** The eleven corbelled bricks, bottom step first, and their laid positions. */
		TArray<int32> CorbelPieces;
		TArray<FVector> CorbelLaidAtCm;

		int32 FarSidePiece = INDEX_NONE;
		FVector FarSideLaidAtCm = FVector::ZeroVector;

		/** World time when the freeze was lifted. Negative until then. */
		double UnfrozenAtSeconds = -1.0;

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

	/** Every laid box of a binding, in handle order. */
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
	 * Request a screenshot through the viewport client: UEngine::Exec has no SHOT handler, so a
	 * request there writes nothing. Called inline so it lands in the commit's frame. A missing
	 * viewport is reported, since otherwise the only symptom is a missing file.
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

/** Aim the camera at the staircase corner and record the laid positions of the bricks that matter. */
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

	// Asserted separately so a missing viewport is not mistaken for a failed render.
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
 * Cut the staircase as a player would and request the before-shot in the same frame; a separate
 * latent command would run a frame later. The time freeze covers the rest, and
 * FStaircaseScreenshotVerifyBeforeCommand measures the result.
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
	 * Freeze time before cutting. Clamped to MinGlobalTimeDilation (0.0001 in BaseGame.ini), so a
	 * released brick falls about 5e-9 cm per frame. Set here so the settle frames ran at normal speed.
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

	// Point at each brick, then choose Delete once for the whole selection.
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

	// ChoosePieceMenuRow must dismiss the menu, or the panel would be in both pictures.
	Test->TestFalse(
		TEXT("no menu may be up when the before-frame is queued, or the panel is in the picture"),
		Controller->IsPieceMenuShown());

	RequestScreenshot(*Test, BeforeScreenshotCommand);

	Test->AddInfo(FString::Printf(
		TEXT("the void is cut and '%s' is queued in the same frame"), BeforeScreenshotCommand));

	return true;
}

/**
 * Check the corbel had moved under 1 mm at the before-frame, then restore time dilation and
 * record the world time so the after-frame can prove time passed.
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

		Record.UnfrozenAtSeconds = World != nullptr ? World->GetTimeSeconds() : -1.0;

		Test->AddInfo(FString::Printf(
			TEXT("time dilation restored to 1 at world time %.3f s; the overhang may now come down ")
			TEXT("if anything is going to make it"),
			Record.UnfrozenAtSeconds));
	}

	return true;
}

/**
 * The overhang stood; take the after-shot. (Until 2026-08-07 this required it to fall; see the
 * file header.)
 *
 * Asserts real time elapsed, each of the eleven corbel steps unmoved (printed per course), none
 * of them released (a released brick could jam in place), and the far-side control unmoved.
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

	// Elapsed time first: a wall that did not move because the clock stopped did not stand.
	const double NowSeconds = World != nullptr ? World->GetTimeSeconds() : -1.0;

	const double ElapsedSeconds = Record.UnfrozenAtSeconds >= 0.0 && NowSeconds >= 0.0
		? NowSeconds - Record.UnfrozenAtSeconds
		: -1.0;

	Test->AddInfo(FString::Printf(
		TEXT("%.3f s of world time passed between the two frames, over %d frames"),
		ElapsedSeconds, FallFrames + SettleFrames));

	Test->TestTrue(
		*FString::Printf(
			TEXT("the wall must have been given real time to fall in, or 'it did not move' means nothing: %.3f s elapsed and at least %g was required"),
			ElapsedSeconds, MinimumElapsedSeconds),
		ElapsedSeconds >= MinimumElapsedSeconds);

	// Printed for every course, pass or fail, so the log reads as a table.
	double WorstMovedCm = 0.0;

	for (int32 Step = 0; Step < Record.CorbelPieces.Num(); ++Step)
	{
		const int32 Piece = Record.CorbelPieces[Step];
		const FVector NowAtCm = ActorLocationOf(*Binding, Piece);
		const double MovedCm = FVector::Dist(NowAtCm, Record.CorbelLaidAtCm[Step]);

		WorstMovedCm = FMath::Max(WorstMovedCm, MovedCm);

		Test->AddInfo(FString::Printf(
			TEXT("corbel course %2d (piece %4d) moved %.6f cm and is at Z %.3f, laid at Z %.3f"),
			StaircaseLowestCorbelCourse + Step, Piece, MovedCm,
			NowAtCm.Z, Record.CorbelLaidAtCm[Step].Z));

		Test->TestTrue(
			*FString::Printf(
				TEXT("THE RULING: the overhang must STAND — corbel course %d (piece %d) must not have moved, it drifted %.6f cm and may drift %g"),
				StaircaseLowestCorbelCourse + Step, Piece, MovedCm, DriftToleranceCm),
			MovedCm < DriftToleranceCm);

		// Held up, not just jammed in place after release.
		Test->TestFalse(
			*FString::Printf(
				TEXT("corbel course %d (piece %d) is still being carried and must NOT have been released to physics"),
				StaircaseLowestCorbelCourse + Step, Record.CorbelPieces[Step]),
			Binding->IsReleased(Record.CorbelPieces[Step]));
	}

	Test->AddInfo(FString::Printf(
		TEXT("the worst-moved of the %d corbelled bricks travelled %.6f cm in %.3f s"),
		Record.CorbelPieces.Num(), WorstMovedCm, ElapsedSeconds));

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
 * Both files exist (deleted before the run), are real PNGs, and no material fell back to the
 * checkerboard.
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
		 * Signature and IHDR read by hand (8 signature bytes, 4-byte length, "IHDR", then big-endian
		 * width and height), avoiding a decoder from the stack under test.
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
	 * Log sweep, read from the file because material compiles happen at map load, before the test.
	 * FILEREAD_AllowWrite is required: the engine holds the log open for writing.
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

	// Delete old files first so their existence later proves this run wrote them.
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
	 * Settle the intact wall, cut and shoot in one frame, wait, settle, shoot again. Each view gets
	 * its own shader drain and settle after it is set up (see Tests/PieceMenuScreenshotTest.cpp).
	 * No settle between the cut and the first shot, since any frame there could show falling.
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
