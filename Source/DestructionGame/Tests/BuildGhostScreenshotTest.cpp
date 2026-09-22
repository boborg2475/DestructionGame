// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Profiles/MaterialProfiles.h"
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
#include "World/BuildModeComponent.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Screenshots of the build-mode ghost. BuildModeComponentTest proves by bounds that the ghost
 * sits where the committed brick lands; this produces the picture for a human to judge.
 * Frame 1 (BuildGhost_Preview): a seed brick, with the ghost at the next-course snap. Frame 2
 * (BuildGhost_Placed): a real brick where the ghost was, the ghost one cell further on. The
 * camera is fixed over every pose so the frames compare pixel for pixel.
 *
 * Drives the real UBuildModeComponent and subsystem, 15 m down -Y from the scenario wall at the
 * origin (seed at (0, -1500, 0), ghost snap at (11.25, -1500, 7.5)); the wall is left standing.
 * Asserts only piece counts, a visible ghost, and that each file is a real PNG.
 *
 * Needs a real RHI (NonNullRHI), so the -nullrhi suite skips it. Run from PowerShell (Git Bash
 * mangles the map path), without -nullrhi:
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.BuildGhostScreenshot"
 *     -TestExit="Automation Test Queue Empty"
 */
namespace BuildGhostScreenshotSupport
{
	using namespace DestructionLayout;

	/** The two frames' file base names, under FPaths::ScreenShotDir(). */
	const TCHAR* const PreviewBaseName = TEXT("BuildGhost_Preview");
	const TCHAR* const PlacedBaseName = TEXT("BuildGhost_Placed");

	/** `Shot` and not `HighResShot`, and `showui` with it — see Tests/CorbelScreenshotTest.cpp. */
	inline FString ShotCommandFor(const FString& BaseName)
	{
		return FString::Printf(TEXT("Shot showui filename=%s -nosuffix"), *BaseName);
	}

	inline FString ScreenshotPathFor(const FString& BaseName)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::ScreenShotDir() / BaseName + TEXT(".png"));
	}

	/** Debug overlays off, so nothing covers the image. */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/**
	 * Frame waits: warm-up and settle for TSR history and auto-exposure; Slate for layout; write
	 * because screenshots are written at end of draw; advance so the second brick is lit.
	 */
	constexpr int32 WarmUpFrames = 120;
	constexpr int32 SettleFrames = 60;
	constexpr int32 SlateFrames = 3;
	constexpr int32 WriteFrames = 5;
	constexpr int32 AdvanceFrames = 30;

	/** Minimum size of a real lit frame; a flat colour stays under 10 kB. */
	constexpr int64 MinimumScreenshotBytes = 10 * 1024;
	constexpr int32 MinimumScreenshotWidth = 640;
	constexpr int32 MinimumScreenshotHeight = 480;

	/** With a 90-degree FOV, standoff s shows 2s wide by 2s * 1080/1920 tall; margin pads the edges. */
	constexpr double FrameMargin = 1.25;
	constexpr double ViewportAspectHeightOverWidth = 1080.0 / 1920.0;

	/** Yaw -90 looks along -Y at the X-Z build plane, with +X to the right. */
	constexpr double CameraYawDegrees = -90.0;

	/** Minimum camera distance. */
	constexpr double MinimumStandoffCm = 120.0;

	/** Stage offset from the scenario wall, applied to every cursor; the wall ends up behind the camera. */
	constexpr double StageOriginYCm = -1500.0;

	/**
	 * Cursors: the seed; an off-grid next-course cursor that snaps to (11.25, -1500, 7.5); and one
	 * 22.5 cm cell further along.
	 */
	const FVector SeedCursorCm(0.0, StageOriginYCm, 0.0);
	const FVector NextCourseCursorCm(11.0, StageOriginYCm, 7.5);
	const FVector FurtherCursorCm(33.75, StageOriginYCm, 7.5);

	/** Half-extent of the 21.5 x 10.25 x 6.5 brick, the component's default. */
	const FVector HalfBrickCm(10.75, 5.125, 3.25);

	/** What the run built, shared between latent commands that run frames apart. */
	struct FBuildGhostRecord
	{
		bool bBuilt = false;

		TWeakObjectPtr<AActor> Owner;
		TWeakObjectPtr<UBuildModeComponent> Component;

		int32 StructureId = INDEX_NONE;

		void Reset()
		{
			*this = FBuildGhostRecord();
		}
	};

	inline FBuildGhostRecord& BuildGhostRecord()
	{
		static FBuildGhostRecord Record;
		return Record;
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

	/** Requests a screenshot via the viewport client; UEngine::Exec has no SHOT handler. */
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

	/** How many pieces the live build holds right now, or -1 if the structure has vanished. */
	inline int32 LivePieceCount(UWorld* World, int32 StructureId)
	{
		UDestructionStructureSubsystem* const Subsystem =
			World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;

		const FStructureBinding* const Binding =
			Subsystem != nullptr ? Subsystem->Find(StructureId) : nullptr;

		return Binding != nullptr ? Binding->NumPieces() : -1;
	}

	/** Checks the piece count and a visible ghost before each shot. */
	inline void CheckStageBeforeShot(
		FAutomationTestBase& Test, const TCHAR* Stage, int32 ExpectedPieces)
	{
		const FBuildGhostRecord& Record = BuildGhostRecord();

		const int32 Pieces = LivePieceCount(
			Record.Component.IsValid() ? Record.Component->GetWorld() : nullptr, Record.StructureId);

		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: the build must hold %d piece(s) at this frame, it holds %d"),
				Stage, ExpectedPieces, Pieces),
			Pieces, ExpectedPieces);

		AActor* const Ghost = Record.Component.IsValid() ? Record.Component->GetGhostActor() : nullptr;

		Test.TestNotNull(
			*FString::Printf(TEXT("%s: the component must have a ghost actor to photograph"), Stage),
			Ghost);

		if (Ghost != nullptr)
		{
			Test.TestFalse(
				*FString::Printf(
					TEXT("%s: the ghost must be VISIBLE — a valid preview shows where the click lands"),
					Stage),
				Ghost->IsHidden());
		}
	}
}

/** Checks there is a world and a viewport to photograph. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildGhostOpenStageCommand, FAutomationTestBase*, Test);

bool FBuildGhostOpenStageCommand::Update()
{
	using namespace BuildGhostScreenshotSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world: the map must be named on the command line"));
		return true;
	}

	Test->AddInfo(FString::Printf(TEXT("game world is %s"), *World->GetMapName()));

	// Asserted here; otherwise a missing viewport looks like a renderer failure.
	UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

	Test->TestNotNull(
		TEXT("there must be a game viewport for Slate to screenshot: -nullrhi must be absent"),
		Viewport);

	return true;
}

/**
 * Attaches the component, seeds a grounded brick, previews the next-course snap, and frames the
 * camera once over every pose that will appear.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildGhostBuildCommand, FAutomationTestBase*, Test);

bool FBuildGhostBuildCommand::Update()
{
	using namespace BuildGhostScreenshotSupport;
	using namespace DestructionProfiles;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	FBuildGhostRecord& Record = BuildGhostRecord();
	Record.Reset();

	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world to build into"));
		return true;
	}

	AActor* const Owner = World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		Test->AddError(TEXT("the component's owner actor failed to spawn"));
		return true;
	}
	Record.Owner = Owner;

	UBuildModeComponent* const Comp = NewObject<UBuildModeComponent>(Owner);
	if (Comp == nullptr)
	{
		Test->AddError(TEXT("the build-mode component failed to construct"));
		return true;
	}
	Comp->RegisterComponent();
	Record.Component = Comp;

	Comp->BeginBuild();
	Record.StructureId = Comp->GetStructureId();

	// Grounded seed brick: the structure grows to 1.
	Comp->UpdatePreviewAt(SeedCursorCm);
	Comp->ConfirmPlace();

	// Ghost hovers at the running-bond snap (11.25, -1500, 7.5), the state frame 1 shows.
	const FBuildPreview Preview = Comp->UpdatePreviewAt(NextCourseCursorCm);

	Test->TestTrue(
		TEXT("the next-course preview against the seed must be valid"),
		Preview.bValid);

	Test->TestEqual(
		*FString::Printf(
			TEXT("the seed confirm should grow the structure to 1 piece, it holds %d"),
			LivePieceCount(World, Record.StructureId)),
		LivePieceCount(World, Record.StructureId), 1);

	// Camera framed over every pose that will appear.

	FBox Bounds(ForceInit);
	Bounds += FBox::BuildAABB(SeedCursorCm, HalfBrickCm);
	Bounds += FBox::BuildAABB(FVector(11.25, StageOriginYCm, 7.5), HalfBrickCm);
	Bounds += FBox::BuildAABB(FurtherCursorCm, HalfBrickCm);

	const FVector CentreCm = Bounds.GetCenter();
	const FVector HalfSizeCm = Bounds.GetExtent();

	const double StandoffCm = FMath::Max(
		MinimumStandoffCm,
		FrameMargin * FMath::Max(
			HalfSizeCm.X, HalfSizeCm.Z / ViewportAspectHeightOverWidth));

	ADestructionGamePlayerController* const Controller = FindController(*Test, World);
	APawn* const Pawn = Controller != nullptr ? Controller->GetPawn() : nullptr;

	if (Pawn == nullptr)
	{
		Test->AddError(TEXT("the player controller has no pawn to put the camera on"));
		return true;
	}

	const FVector CameraCm(CentreCm.X, StageOriginYCm + StandoffCm, CentreCm.Z);

	Pawn->SetActorLocation(CameraCm);
	Controller->SetControlRotation(FRotator(0.0, CameraYawDegrees, 0.0));

	Record.bBuilt = true;

	Test->AddInfo(FString::Printf(
		TEXT("seed at (%.2f, %.2f, %.2f); ghost previews the snap kind %d at (%.2f, %.2f, %.2f). The ")
		TEXT("loop spans %.1f x %.1f cm centred on (%.1f, %.1f); the camera stands off %.1f cm and ")
		TEXT("frames %.1f x %.1f cm head-on along -Y."),
		SeedCursorCm.X, SeedCursorCm.Y, SeedCursorCm.Z, static_cast<int32>(Preview.Kind),
		Preview.CentreCm.X, Preview.CentreCm.Y, Preview.CentreCm.Z,
		2.0 * HalfSizeCm.X, 2.0 * HalfSizeCm.Z, CentreCm.X, CentreCm.Z, StandoffCm,
		2.0 * StandoffCm, 2.0 * StandoffCm * ViewportAspectHeightOverWidth));

	return true;
}

/** Frame 1: the seed stands and the ghost hovers at the snap. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildGhostShootPreviewCommand, FAutomationTestBase*, Test);

bool FBuildGhostShootPreviewCommand::Update()
{
	using namespace BuildGhostScreenshotSupport;

	if (!BuildGhostRecord().bBuilt)
	{
		return true;
	}

	CheckStageBeforeShot(*Test, TEXT("frame 1 (preview)"), /*ExpectedPieces*/ 1);

	RequestScreenshot(*Test, ShotCommandFor(FString(PreviewBaseName)));

	return true;
}

/** Commits the previewed brick, then previews one cell further (the state frame 2 shows). */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildGhostAdvanceCommand, FAutomationTestBase*, Test);

bool FBuildGhostAdvanceCommand::Update()
{
	using namespace BuildGhostScreenshotSupport;

	FBuildGhostRecord& Record = BuildGhostRecord();

	if (!Record.bBuilt)
	{
		return true;
	}

	UBuildModeComponent* const Comp = Record.Component.Get();
	if (Comp == nullptr)
	{
		Test->AddError(TEXT("the build-mode component vanished before the commit"));
		return true;
	}

	const FPieceRef Placed = Comp->ConfirmPlace();

	Test->TestTrue(
		*FString::Printf(
			TEXT("the commit should place a real second piece, ref {%d, 1}, got {%d, %d}"),
			Record.StructureId, Placed.StructureId, Placed.PieceIndex),
		Placed == FPieceRef{ Record.StructureId, 1 });

	Test->TestEqual(
		*FString::Printf(
			TEXT("the commit should grow the structure to 2 pieces, it holds %d"),
			LivePieceCount(Comp->GetWorld(), Record.StructureId)),
		LivePieceCount(Comp->GetWorld(), Record.StructureId), 2);

	const FBuildPreview Preview = Comp->UpdatePreviewAt(FurtherCursorCm);

	Test->TestTrue(
		TEXT("the further preview against the two-brick structure must be valid"),
		Preview.bValid);

	Test->AddInfo(FString::Printf(
		TEXT("committed the second brick; the ghost now previews snap kind %d at (%.2f, %.2f, %.2f)"),
		static_cast<int32>(Preview.Kind), Preview.CentreCm.X, Preview.CentreCm.Y, Preview.CentreCm.Z));

	return true;
}

/** Frame 2: the real brick stands where the ghost was, and the ghost has moved on. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildGhostShootPlacedCommand, FAutomationTestBase*, Test);

bool FBuildGhostShootPlacedCommand::Update()
{
	using namespace BuildGhostScreenshotSupport;

	if (!BuildGhostRecord().bBuilt)
	{
		return true;
	}

	CheckStageBeforeShot(*Test, TEXT("frame 2 (placed)"), /*ExpectedPieces*/ 2);

	RequestScreenshot(*Test, ShotCommandFor(FString(PlacedBaseName)));

	return true;
}

/**
 * Removes the ghost (via DestroyComponent), the bricks (via the subsystem) and the owner. The
 * scenario wall is untouched for sibling Visual.* shots.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildGhostTearDownCommand, FAutomationTestBase*, Test);

bool FBuildGhostTearDownCommand::Update()
{
	using namespace BuildGhostScreenshotSupport;

	FBuildGhostRecord& Record = BuildGhostRecord();

	UWorld* const World = Record.Component.IsValid()
		? Record.Component->GetWorld()
		: (Record.Owner.IsValid() ? Record.Owner->GetWorld() : nullptr);

	if (UBuildModeComponent* const Comp = Record.Component.Get())
	{
		Comp->DestroyComponent();
	}

	if (World != nullptr && Record.StructureId != INDEX_NONE)
	{
		if (UDestructionStructureSubsystem* const Subsystem =
				World->GetSubsystem<UDestructionStructureSubsystem>())
		{
			Subsystem->Destroy(Record.StructureId);
		}
	}

	if (AActor* const Owner = Record.Owner.Get())
	{
		Owner->Destroy();
	}

	Test->AddInfo(TEXT("cleared the build's ghost, bricks and owner off the stage"));

	Record.Reset();

	return true;
}

/** Both files landed and they are real PNGs. Both were deleted before the run. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildGhostCheckFilesCommand, FAutomationTestBase*, Test);

bool FBuildGhostCheckFilesCommand::Update()
{
	using namespace BuildGhostScreenshotSupport;

	for (const TCHAR* const BaseName : { PreviewBaseName, PlacedBaseName })
	{
		const FString Path = ScreenshotPathFor(FString(BaseName));

		const int64 SizeBytes = IFileManager::Get().FileSize(*Path);

		if (SizeBytes < 0)
		{
			Test->AddError(FString::Printf(
				TEXT("no screenshot was written to %s: the shot request never reached a draw, or the ")
				TEXT("file went somewhere else"),
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
		 * Signature and IHDR read by hand: 8 signature bytes, 4-byte length, "IHDR", then big-endian
		 * width and height. Avoids depending on a decoder from the stack under test.
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
				TEXT("%s.png must be a real frame of a lit scene — a flat colour is well under 10 kB — ")
				TEXT("so it must be at least %lld bytes; it is %lld"),
				BaseName, MinimumScreenshotBytes, SizeBytes),
			SizeBytes >= MinimumScreenshotBytes);

		Test->TestTrue(
			*FString::Printf(
				TEXT("%s.png must begin with the PNG signature and an IHDR chunk, and declare real ")
				TEXT("dimensions; it is %d x %d"),
				BaseName, Width, Height),
			bIsPng && Width >= MinimumScreenshotWidth && Height >= MinimumScreenshotHeight);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildGhostScreenshotTest,
	"DestructionGame.Visual.BuildGhostScreenshot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::NonNullRHI
		| EAutomationTestFlags::ProductFilter)

bool FBuildGhostScreenshotTest::RunTest(const FString& Parameters)
{
	using namespace BuildGhostScreenshotSupport;

	BuildGhostRecord().Reset();

	// Delete old files first, so an existing file proves this run rendered it.
	for (const TCHAR* const BaseName : { PreviewBaseName, PlacedBaseName })
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

	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(DisableScreenMessagesCommand));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FBuildGhostOpenStageCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WarmUpFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FBuildGhostBuildCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FBuildGhostShootPreviewCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FBuildGhostAdvanceCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(AdvanceFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FBuildGhostShootPlacedCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FBuildGhostTearDownCommand(this));

	ADD_LATENT_AUTOMATION_COMMAND(FBuildGhostCheckFilesCommand(this));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
