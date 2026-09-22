// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Core/BuildMode/DemoBuilding.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "DestructionGamePlayerController.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "World/BrickActor.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * One head-on screenshot of BuildMode::BuildDemoBuilding (a two-course ClayBrick wall with a
 * Timber plate), stood up as ABrickActors. Reuses the Tests/CorbelScreenshotTest.cpp harness;
 * one frame, since the building stands and nothing is released.
 *
 * Built StageOriginYCm off the scenario wall so the two stay out of each other's frame; the
 * offset is along an axis gravity ignores. Asserts only that eight pieces were built and spawned
 * and that a real PNG landed; judging the image is a human's job.
 *
 * Needs a real RHI (NonNullRHI), so the -nullrhi suite skips it. Run explicitly from PowerShell
 * (Git Bash mangles the map path):
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.BuildDemoScreenshot"
 *     -TestExit="Automation Test Queue Empty"
 *
 * `-nullrhi` must be absent, or there is no viewport to screenshot.
 */
namespace BuildDemoScreenshotSupport
{
	using namespace DestructionLayout;

	/** Screenshot file name, under FPaths::ScreenShotDir(). */
	const TCHAR* const ShotBaseName = TEXT("BuildDemo");

	/** The pieces `BuildDemoBuilding` lays: 4 grounded + 3 staggered ClayBrick + 1 Timber plate. */
	constexpr int32 ExpectedPieceCount = 8;

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

	/** Debug overlays off, so nothing is burned into the image. */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/**
	 * Frame waits, from the corbel harness: warm-up and settle cover TSR history and auto-exposure,
	 * Slate is one layout pass, and Write lets ProcessScreenShots write at end of draw.
	 */
	constexpr int32 WarmUpFrames = 120;
	constexpr int32 SettleFrames = 60;
	constexpr int32 SlateFrames = 3;
	constexpr int32 WriteFrames = 5;

	/** A lit scene clears 10 kB; a flat colour does not. */
	constexpr int64 MinimumScreenshotBytes = 10 * 1024;
	constexpr int32 MinimumScreenshotWidth = 640;
	constexpr int32 MinimumScreenshotHeight = 480;

	/**
	 * Camera framing: with a 90-degree horizontal FOV, a standoff s shows 2s wide and 2s * 9/16
	 * high; the margin leaves some ground and sky around the wall.
	 */
	constexpr double FrameMargin = 1.25;
	constexpr double ViewportAspectHeightOverWidth = 1080.0 / 1920.0;

	/** Yaw -90 looks along -Y, square-on to the wall (planar in X-Z), with +X to the right. */
	constexpr double CameraYawDegrees = -90.0;

	constexpr double MinimumStandoffCm = 120.0;

	/**
	 * 15 m down Y from the scenario wall, applied only to the spawn. The camera looks along -Y, so
	 * the scenario wall is behind it.
	 */
	constexpr double StageOriginYCm = -1500.0;

	/** What the run built, held at file scope because latent commands run frames apart. */
	struct FBuildDemoRecord
	{
		bool bBuilt = false;

		FBrickLayout Layout;

		/** One entry per piece handle. Null for a piece that was never spawned. */
		TArray<TWeakObjectPtr<ABrickActor>> Actors;

		int32 SpawnedActors = 0;

		void Reset()
		{
			*this = FBuildDemoRecord();
		}
	};

	inline FBuildDemoRecord& BuildDemoRecord()
	{
		static FBuildDemoRecord Record;
		return Record;
	}

	/**
	 * Transform that makes the brick mesh fill the box (copied from the corbel harness). The pivot
	 * is read off the mesh because SM_Cube's origin is a corner, not the centre.
	 */
	inline FTransform DemoBrickSpawnTransform(const UStaticMesh& BrickMesh, const FPieceBox& Box)
	{
		const FBox LocalBounds = BrickMesh.GetBoundingBox();

		const FVector Scale = (Box.ExtentCm * 2.0) / LocalBounds.GetSize();

		return FTransform(
			FRotator::ZeroRotator,
			Box.CentreCm - Scale * LocalBounds.GetCenter(),
			Scale);
	}

	/** Spawn one sized, placed, weighed brick. Null on failure. */
	inline ABrickActor* SpawnDemoBrick(UWorld& World, const FPieceBox& Box, double MassKg)
	{
		ABrickActor* Brick = World.SpawnActorDeferred<ABrickActor>(
			ABrickActor::StaticClass(), FTransform::Identity);

		if (Brick == nullptr)
		{
			return nullptr;
		}

		UStaticMeshComponent* const Mesh = Brick->GetMesh();
		UStaticMesh* const BrickMesh = Mesh != nullptr ? Mesh->GetStaticMesh() : nullptr;

		// A deleted mesh asset leaves this null, and sizing divides by its bounds.
		if (BrickMesh == nullptr)
		{
			Brick->Destroy();
			return nullptr;
		}

		Mesh->SetMassOverrideInKg(NAME_None, static_cast<float>(MassKg), true);

		Brick->FinishSpawning(DemoBrickSpawnTransform(*BrickMesh, Box));

		return Brick;
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

	/** Request a screenshot through the viewport client; UEngine::Exec has no Shot handler. */
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
}

/** Check there is a world and a viewport to photograph. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildDemoOpenStageCommand, FAutomationTestBase*, Test);

bool FBuildDemoOpenStageCommand::Update()
{
	using namespace BuildDemoScreenshotSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world: the map must be named on the command line"));
		return true;
	}

	Test->AddInfo(FString::Printf(TEXT("game world is %s"), *World->GetMapName()));

	// Asserted here, since a missing viewport otherwise looks like a failed render downstream.
	UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

	Test->TestNotNull(
		TEXT("there must be a game viewport for Slate to screenshot: -nullrhi must be absent"),
		Viewport);

	return true;
}

/** Build the demo building world-free, spawn it, and aim the camera head-on. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildDemoBuildCommand, FAutomationTestBase*, Test);

bool FBuildDemoBuildCommand::Update()
{
	using namespace BuildDemoScreenshotSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	FBuildDemoRecord& Record = BuildDemoRecord();
	Record.Reset();

	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world to build into"));
		return true;
	}

	const TArray<BuildMode::FPlacementResult> Placements = BuildMode::BuildDemoBuilding(Record.Layout);

	Test->TestEqual(
		*FString::Printf(
			TEXT("BuildDemoBuilding must place %d pieces (4 grounded + 3 staggered ClayBrick + 1 ")
			TEXT("Timber plate); it placed %d"),
			ExpectedPieceCount, Placements.Num()),
		Placements.Num(), ExpectedPieceCount);

	Test->TestEqual(
		*FString::Printf(
			TEXT("the built structure must hold %d pieces; it holds %d"),
			ExpectedPieceCount, Record.Layout.Structure.NumPieces()),
		Record.Layout.Structure.NumPieces(), ExpectedPieceCount);

	// Solved but not settled: the structure as laid. Nothing is released.
	Record.Layout.Structure.SolveLoads();

	Record.Actors.SetNum(Record.Layout.Structure.NumPieces());

	FBox Bounds(ForceInit);

	for (int32 Piece = 0; Piece < Record.Layout.Structure.NumPieces(); ++Piece)
	{
		if (Record.Layout.Structure.IsPieceRemoved(Piece))
		{
			continue;
		}

		// The stage offset is applied here only.
		FPieceBox StageBox = Record.Layout.Boxes[Piece];
		StageBox.CentreCm.Y += StageOriginYCm;

		ABrickActor* const Brick = SpawnDemoBrick(
			*World, StageBox, Record.Layout.Structure.GetPiece(Piece).MassKg);

		if (Brick == nullptr)
		{
			Test->AddError(FString::Printf(
				TEXT("piece %d could not be spawned, so the picture is missing a brick"), Piece));

			continue;
		}

		Record.Actors[Piece] = Brick;

		++Record.SpawnedActors;

		Bounds += FBox::BuildAABB(
			Record.Layout.Boxes[Piece].CentreCm, Record.Layout.Boxes[Piece].ExtentCm);
	}

	Test->TestEqual(
		*FString::Printf(
			TEXT("every one of the %d live pieces must have a brick standing for it, or the ")
			TEXT("photograph is of a different structure from the one that was solved; %d bricks stand"),
			Record.Layout.Structure.NumLivePieces(), Record.SpawnedActors),
		Record.SpawnedActors, Record.Layout.Structure.NumLivePieces());

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
		TEXT("%d pieces, %d joints, %d bricks spawned. The wall spans %.1f x %.1f cm centred on ")
		TEXT("(%.1f, %.1f); the camera stands off %.1f cm and frames %.1f x %.1f cm head-on."),
		Record.Layout.Structure.NumLivePieces(), Record.Layout.Structure.NumConnections(),
		Record.SpawnedActors, 2.0 * HalfSizeCm.X, 2.0 * HalfSizeCm.Z, CentreCm.X, CentreCm.Z,
		StandoffCm, 2.0 * StandoffCm, 2.0 * StandoffCm * ViewportAspectHeightOverWidth));

	return true;
}

/** Take the screenshot. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildDemoShootCommand, FAutomationTestBase*, Test);

bool FBuildDemoShootCommand::Update()
{
	using namespace BuildDemoScreenshotSupport;

	FBuildDemoRecord& Record = BuildDemoRecord();

	if (!Record.bBuilt)
	{
		return true;
	}

	RequestScreenshot(*Test, ShotCommandFor(FString(ShotBaseName)));

	return true;
}

/** Remove the demo bricks, leaving the scenario wall. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildDemoTearDownCommand, FAutomationTestBase*, Test);

bool FBuildDemoTearDownCommand::Update()
{
	using namespace BuildDemoScreenshotSupport;

	FBuildDemoRecord& Record = BuildDemoRecord();

	int32 Removed = 0;

	for (TWeakObjectPtr<ABrickActor>& Weak : Record.Actors)
	{
		if (ABrickActor* const Brick = Weak.Get())
		{
			Brick->Destroy();
			++Removed;
		}
	}

	Test->AddInfo(FString::Printf(
		TEXT("cleared %d of the %d demo bricks off the stage"), Removed, Record.SpawnedActors));

	Record.Reset();

	return true;
}

/** Check the file landed and is a real PNG (it was deleted before the run). */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FBuildDemoCheckFileCommand, FAutomationTestBase*, Test);

bool FBuildDemoCheckFileCommand::Update()
{
	using namespace BuildDemoScreenshotSupport;

	const FString Path = ScreenshotPathFor(FString(ShotBaseName));

	const int64 SizeBytes = IFileManager::Get().FileSize(*Path);

	if (SizeBytes < 0)
	{
		Test->AddError(FString::Printf(
			TEXT("no screenshot was written to %s: the shot request never reached a draw, or the ")
			TEXT("file went somewhere else"),
			*Path));

		return true;
	}

	TArray<uint8> Bytes;

	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 24)
	{
		Test->AddError(FString::Printf(
			TEXT("the screenshot at %s could not be read back, or is too short to carry a PNG header ")
			TEXT("(%d bytes)"),
			*Path, Bytes.Num()));

		return true;
	}

	/*
	 * PNG header read by hand: 8 signature bytes, a 4-byte length, "IHDR", then big-endian width
	 * and height. Avoids depending on a decoder.
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
		ShotBaseName, SizeBytes, Width, Height, *Path));

	Test->TestTrue(
		*FString::Printf(
			TEXT("%s.png must be a real frame of a lit scene — a flat colour is well under 10 kB — so ")
			TEXT("it must be at least %lld bytes; it is %lld"),
			ShotBaseName, MinimumScreenshotBytes, SizeBytes),
		SizeBytes >= MinimumScreenshotBytes);

	Test->TestTrue(
		*FString::Printf(
			TEXT("%s.png must begin with the PNG signature and an IHDR chunk, and declare real ")
			TEXT("dimensions; it is %d x %d"),
			ShotBaseName, Width, Height),
		bIsPng && Width >= MinimumScreenshotWidth && Height >= MinimumScreenshotHeight);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildDemoScreenshotTest,
	"DestructionGame.Visual.BuildDemoScreenshot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::NonNullRHI
		| EAutomationTestFlags::ProductFilter)

bool FBuildDemoScreenshotTest::RunTest(const FString& Parameters)
{
	using namespace BuildDemoScreenshotSupport;

	BuildDemoRecord().Reset();

	// Delete any old file first, so the file existing afterwards proves this run rendered.
	const FString Path = ScreenshotPathFor(FString(ShotBaseName));

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

	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(DisableScreenMessagesCommand));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FBuildDemoOpenStageCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WarmUpFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FBuildDemoBuildCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FBuildDemoShootCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FBuildDemoTearDownCommand(this));

	ADD_LATENT_AUTOMATION_COMMAND(FBuildDemoCheckFileCommand(this));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
