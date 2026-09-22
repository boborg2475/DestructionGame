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
 * Screenshot harness: opens the piece menu on the real wall in a real viewport and writes two PNGs
 * for a human to judge: a wide shot of the whole wall and a close-up where the highlight colours
 * can be told apart. The rest of the suite is -nullrhi, and has twice been green over on-screen
 * bugs (CURRENT_STATE.md).
 *
 * It deletes both files first, then asserts what a picture of nothing could not satisfy: world,
 * wall, viewport, three bricks selected by rays, one inspected, menu still up, and real PNGs. It
 * asserts nothing about image content (a pixel test would have to be a difference test).
 *
 * NonNullRHI keeps it out of the -nullrhi suite, which would otherwise run it and pass with no file.
 *
 * To run: close the editor, build as in CLAUDE.md, then:
 *
 *   "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.PieceMenuScreenshot"
 *     -TestExit="Automation Test Queue Empty"
 *
 * -nullrhi must be absent: with it no window or viewport is built.
 *
 * `Shot showui`, not `HighResShot`: HighResShot renders scene-only and never sets ShouldShowUI, so
 * the Slate menu would be missing from the image.
 *
 * Needs no production hooks: InspectAlongRay takes a world ray and SetInspectedPiece a ref. Uses
 * only AutomationCommon.h (Engine module); no functional test, no map edit, no FunctionalTesting
 * dependency.
 */
namespace PieceMenuScreenshotSupport
{
	/**
	 * PNG base names. Files land in FPaths::ScreenShotDir() (Saved/Screenshots/WindowsEditor/).
	 * -nosuffix keeps one fixed name, so after the pre-run delete its existence means this run.
	 * Wide shows the menu in context; the close-up makes the highlight colours distinguishable.
	 */
	const TCHAR* const WideScreenshotBaseName = TEXT("PieceMenu");
	const TCHAR* const CloseUpScreenshotBaseName = TEXT("PieceMenuCloseUp");

	inline FString ScreenshotPathFor(const TCHAR* BaseName)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::ScreenShotDir() / FString(BaseName) + TEXT(".png"));
	}

	/** The exec commands; `showui` is why this is `Shot` (see file header). */
	const TCHAR* const WideScreenshotCommand = TEXT("Shot showui filename=PieceMenu -nosuffix");
	const TCHAR* const CloseUpScreenshotCommand =
		TEXT("Shot showui filename=PieceMenuCloseUp -nosuffix");

	/** Both files, so the delete and the PNG check apply equally to each. */
	const TCHAR* const ScreenshotBaseNames[] = { WideScreenshotBaseName, CloseUpScreenshotBaseName };

	/** Turn off on-screen debug messages (e.g. "Preparing Shaders") first, so none is drawn over the image. */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/**
	 * Wall geometry derived from the game mode's spec: 21.5 x 10.25 x 6.5 cm bricks, 1 cm joints,
	 * 30 x 40, so pitches 22.5 cm (X) and 7.5 cm (Z), centred on Y = 0. Even-course brick n is at
	 * X = n x 22.5; course c at Z = 3.25 + 7.5c. The wall spans X -10.75..663.25 and Z 0..299, so
	 * its centre (where the camera aims) is 326.25, 149.5.
	 */
	constexpr double BrickPitchCm = 22.5;
	constexpr double CoursePitchCm = 7.5;

	constexpr double WallCentreXCm = 326.25;
	constexpr double WallCentreZCm = 149.5;

	/** 30 x 40 flush: 20 even courses of 30 plus 20 odd courses of 31 (a half bat at each end). */
	constexpr int32 ScenarioWallPieceCount = 1220;

	/**
	 * The picked bricks: 13-15 of course 4 (middle at X = 315, near the wall centre). Course 4
	 * (Z = 33.25) keeps the ray clear of the map's floor, and is an even course with no half bats.
	 */
	constexpr int32 PickedCourse = 4;
	constexpr int32 PickedBricks[] = { 13, 14, 15 };
	constexpr int32 PickedCount = UE_ARRAY_COUNT(PickedBricks);

	inline double PickedCentreXCm(int32 Brick) { return Brick * BrickPitchCm; }
	inline double PickedCentreZCm() { return 6.5 * 0.5 + CoursePitchCm * PickedCourse; }

	/** Ray half-length along Y, well outside the 10.25 cm wall; along Y so no other brick is in the way. */
	constexpr double RayReachCm = 100.0;

	/**
	 * Match tolerance for finding a brick by position. Bricks are found by position, not by index
	 * arithmetic over 30/31-brick courses, which can be silently wrong yet still hit a real brick.
	 */
	constexpr double BoxMatchToleranceCm = BrickPitchCm * 0.5;

	/**
	 * Wide camera. With a 90-degree horizontal FOV, 600 cm shows 1200 x 675 cm, framing the
	 * 674 x 299 cm wall with margin. Placed explicitly, not via PlayerStart (its facing is unknown).
	 * Yaw 90 looks along +Y at the wall.
	 */
	constexpr double CameraStandoffCm = 600.0;
	constexpr double CameraYawDegrees = 90.0;

	/**
	 * Close-up standoff: visible width is twice the standoff, so 90 cm shows 180 cm, eight bricks
	 * (the picked three plus unpicked neighbours). Aimed at the inspected brick.
	 */
	constexpr double CloseUpStandoffCm = 90.0;

	/**
	 * Frame waits, done as latent commands (all -ExecCmds flush on the first tick, so they give no
	 * delay). Settle covers TSR history and auto-exposure; Slate lets the new panel lay out; Write
	 * covers screenshots being written at end of draw. Each shot gets its own shader drain and
	 * settle after its view is set up, since aiming the camera queues new shader work.
	 */
	constexpr int32 SettleFrames = 120;
	constexpr int32 SlateFrames = 3;
	constexpr int32 WriteFrames = 5;

	/**
	 * Smallest plausible real frame. A flat-colour 1080p PNG is under 10 kB; a lit scene is hundreds
	 * of kB. 32 kB sits between the two. A floor, not a judgement of content.
	 */
	constexpr int64 MinimumScreenshotBytes = 32 * 1024;

	/** Big enough that a 1 x 1 or a truncated header fails; small enough not to pin a resolution. */
	constexpr int32 MinimumScreenshotWidth = 640;
	constexpr int32 MinimumScreenshotHeight = 480;

	/**
	 * Log line for a material that failed to compile and fell back to the WorldGridMaterial
	 * checkerboard. Logged as a warning, not an error, so only a log-file read catches it.
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

	/** The piece whose box sits at this point, or INDEX_NONE (see BoxMatchToleranceCm). */
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
 * Aim the camera, pick three bricks through InspectAlongRay as a player's clicks do, and inspect
 * the middle one via SetInspectedPiece.
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

	// Assert the viewport now; without one a missing file looks like a render failure.
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

	// The camera uses pawn control rotation, so the control rotation aims it.
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

	// The player's three clicks.
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

	// The selection proves the picture has something in it; the file checks cannot.
	const FPieceSelection& Selection = Controller->GetPieceSelection();

	Test->TestEqual(
		TEXT("three rays into three different bricks should leave three bricks selected"),
		Selection.Num(), PickedCount);

	if (Selection.Num() < 2)
	{
		return true;
	}

	// Inspect the middle brick (as hovering its menu entry does) so the readout shows its joints.
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
 * Move the camera in to the inspected brick and change nothing else, so both shots show one state.
 * Asserts the menu is still up.
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

	// The middle brick, which is the inspected one.
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
 * The files exist (they were deleted before the run), are real PNGs, and no material fell back to
 * the checkerboard.
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
		 * PNG signature and IHDR read by hand: 8 signature bytes, 4-byte length, "IHDR", then
		 * big-endian width and height. Avoids depending on a decoder.
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

	// The menu is still up after the shots.
	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	ADestructionGamePlayerController* const Controller = World != nullptr
		? Cast<ADestructionGamePlayerController>(World->GetFirstPlayerController())
		: nullptr;

	Test->TestTrue(
		TEXT("the menu must still have been up when the shot was drawn"),
		Controller != nullptr && Controller->IsPieceMenuShown());

	/*
	 * Sweep the log file, not a live capture: materials compile during map load, before the test.
	 * FILEREAD_AllowWrite is required because the engine holds the log open for writing.
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

	// Delete old files first, so "the file exists" means this run rendered it.
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
	 * Set up the view, settle, shoot; twice. The second shot differs only in pawn position. Each
	 * shot gets its own shader drain and settle (see the timings block).
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
