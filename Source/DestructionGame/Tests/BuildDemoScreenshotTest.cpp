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
 * The build-mode demo building, photographed standing: one head-on frame of what the placement
 * brain lays.
 *
 * WHY THIS FILE EXISTS. `BuildMode::BuildDemoBuilding` is the proof that the placement brain
 * composes: eight PlacePiece calls that assemble a two-course running-bond ClayBrick wall with a
 * Timber wall-plate bearing across the top, returned as a live `FBrickLayout` that stands under
 * `SolveLoads`. Every test that exercises it — `Core.BuildMode.*` — is world-free arithmetic over
 * `FStructure` and `FPieceBox`, which is why it runs in milliseconds and exactly why it cannot
 * produce a picture. The /goal asks for a small render of a placed structure, and this is it: the
 * same eight pieces those tests solve, stood up in a `UWorld` as `ABrickActor`s and photographed
 * once, head-on.
 *
 * IT MIRRORS `Tests/CorbelScreenshotTest.cpp` RATHER THAN REINVENTING A HARNESS. The corbel
 * harness is the project's pattern for standing an arbitrary `FStructure` + its `FPieceBox`es up
 * in the world and photographing it through the game viewport's `Shot showui` exec. The machinery
 * is reused here almost verbatim — the mesh-fills-the-box spawn transform, the viewport-client
 * screenshot request, the delete-first-so-existence-means-something rule, the PNG signature and
 * IHDR read by hand. What differs is the subject and the shape of the shot:
 *
 *   - The subject is built by `BuildDemoBuilding`, not by a corbel fixture. The layout goes in
 *     empty and comes out with eight jointed pieces, and the picture is of that.
 *   - There is one frame, not a pair. The corbel family photographs either side of a cascade
 *     because a corbel is condemned by its own geometry; the demo building stands, so there is
 *     nothing to settle and no "after". The bricks are spawned kinematic and never released.
 *
 * IT BUILDS FIFTEEN METRES OFF THE SCENARIO WALL, LIKE THE CORBEL HARNESS. `ADestructionGame
 * GameMode::BeginPlay` lays a wall at the origin and every automation test shares one world, so
 * the demo building is spawned `StageOriginYCm` down the Y axis to keep the two out of each
 * other's frame — a constant translation along an axis gravity does not act on, so the structure
 * photographed is arithmetically the one `Core.BuildMode` solves. Nothing here destroys the
 * scenario wall, so a sibling `Visual.*` shot can run in the same process.
 *
 * WHAT IT ASSERTS — deliberately light, because the point is the image. A harness that only takes
 * a picture is green whatever is in the picture, so it asserts the modest invariants that make
 * the frame a picture of the right thing: the layout built to eight pieces, an `ABrickActor`
 * stands for every live piece, and the file landed as a real PNG — signature, IHDR, sane
 * dimensions, and a byte count no flat colour could reach. It asserts nothing about what the
 * image looks like; judging that is a human's job.
 *
 * IT NEEDS A TICKING WORLD *AND* A REAL RHI, hence EAutomationTestFlags::NonNullRHI. Without that
 * flag the ordinary `-nullrhi` suite would run this, find no viewport, write no file and go
 * green. With it, the ordinary suite never mentions this test exists, so it must be run
 * explicitly. From PowerShell (Git Bash mangles the map path):
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.BuildDemoScreenshot"
 *     -TestExit="Automation Test Queue Empty"
 *
 * `-nullrhi` must be absent: `FApp::CanEverRender()` is false with it and `UGameEngine::Init`
 * only builds a window and a viewport under that, so there would be nothing to screenshot even
 * if the filter let the test through.
 */
namespace BuildDemoScreenshotSupport
{
	using namespace DestructionLayout;

	/** The file the one frame lands as, under FPaths::ScreenShotDir(). */
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

	/** Debug overlays off, so nothing is burned over the thing a human is being asked to judge. */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/**
	 * The timings, which are the corbel harness's.
	 *
	 * `WarmUpFrames` and `SettleFrames` cover TSR's temporal history and auto-exposure;
	 * `SlateFrames` is the layout pass a rebuilt view needs; `WriteFrames` is because
	 * `ProcessScreenShots` writes at end of draw, so moving on in the same frame as the request
	 * loses the file. There is no fall or cascade here — the structure stands — so the corbel
	 * harness's `FallFrames` has no counterpart.
	 */
	constexpr int32 WarmUpFrames = 120;
	constexpr int32 SettleFrames = 60;
	constexpr int32 SlateFrames = 3;
	constexpr int32 WriteFrames = 5;

	/**
	 * The floor a real frame of a lit scene clears and a flat colour does not. The task's bar is
	 * 10 kB; a 1080p PNG of eight lit bricks runs far past it, so this is a floor and not a judgement.
	 */
	constexpr int64 MinimumScreenshotBytes = 10 * 1024;
	constexpr int32 MinimumScreenshotWidth = 640;
	constexpr int32 MinimumScreenshotHeight = 480;

	/**
	 * How the camera is placed, copied from the corbel harness and for its reasons.
	 *
	 * The default `UCameraComponent` field of view is 90 degrees horizontally, so at a standoff
	 * `s` the visible width is `2s` and the visible height at 16:9 is `2s * 1080/1920`. Inverting
	 * that for the structure's bounding box gives the standoff, and the margin keeps the wall off
	 * the edges of the frame with some ground and sky to read it against.
	 */
	constexpr double FrameMargin = 1.25;
	constexpr double ViewportAspectHeightOverWidth = 1080.0 / 1920.0;

	/**
	 * The camera looks along -Y, which is head-on to this wall. The demo building is planar in
	 * X-Z at y = 0 — the grounded course, the staggered course and the plate all share it — so a
	 * view down the Y axis is square-on to the wall face with the plate reading across the top.
	 * Yaw -90 puts the view along -Y and +X to the right, the same sense as every elevation in
	 * the design documents.
	 */
	constexpr double CameraYawDegrees = -90.0;

	/** Nothing is framed closer than this, so a metre-wide wall does not fill the screen. */
	constexpr double MinimumStandoffCm = 120.0;

	/**
	 * Fifteen metres down the Y axis from the scenario wall, applied to the spawn and to nothing
	 * else. See the file header: a constant translation along an axis gravity does not act on, so
	 * the structure being photographed is arithmetically the structure the world-free tests read.
	 * The camera stands off along +Y from here and looks along -Y (see CameraYawDegrees), so the
	 * scenario wall at y = 0 is behind the lens rather than in shot.
	 */
	constexpr double StageOriginYCm = -1500.0;

	/**
	 * WHAT THE RUN BUILT, CARRIED BETWEEN LATENT COMMANDS.
	 *
	 * FILE-SCOPE STATE, for the reason the corbel harness gives: a latent command carries only what
	 * its parameters carry, and the build, the shot and the file check run frames apart.
	 */
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
	 * WHERE TO PUT A BRICK ACTOR AND HOW BIG TO MAKE IT, so that its MESH fills the box.
	 *
	 * COPIED FROM `Tests/CorbelScreenshotTest.cpp` FOR ITS REASONS: the subsystem's own recipe is
	 * file-local to `World/DestructionStructureSubsystem.cpp` with no header to reach it through. The
	 * scale is the box's size over the mesh's own local size, and the pivot is read off the mesh
	 * rather than assumed centred — SM_Cube's origin is a CORNER, so placing the actor at the box's
	 * centre would put every brick a half-size out on all three axes.
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

	/** One brick, sized, placed and weighed. Null if it could not be built. */
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

		/*
		 * No mesh, no brick. The mesh is a hard content reference resolved on the CDO, so
		 * deleting the asset leaves it null rather than failing to compile — and the sizing
		 * above divides by its bounds, which would make an infinite scale out of a missing asset.
		 */
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

	/**
	 * Ask for a screenshot, THROUGH THE VIEWPORT CLIENT AND NOT THROUGH GEngine.
	 *
	 * MEASURED, NOT PREFERRED — see `Tests/CorbelScreenshotTest.cpp`: a request routed through
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
}

/**
 * Open the stage: check there is a world and a viewport to photograph into, and leave the scenario
 * wall standing.
 */
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

	/*
	 * THE VIEWPORT IS ASSERTED HERE rather than left to show up as a missing file, because without one
	 * `HandleScreenshotCommand` returns having done nothing at all and the only symptom downstream
	 * reads identically to a renderer that failed.
	 */
	UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

	Test->TestNotNull(
		TEXT("there must be a game viewport for Slate to screenshot: -nullrhi must be absent"),
		Viewport);

	return true;
}

/**
 * Build the demo building world-free, stand it up in the world, and aim the camera head-on at the
 * whole of it.
 *
 * THE STRUCTURE IS BUILT BY `BuildDemoBuilding` AND THEN SPAWNED, in that order, because it is the
 * world-free half that `Core.BuildMode` already measures. The picture is of the same eight pieces
 * those tests solve, not of a second construction that happens to look similar.
 */
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

	/* --- the world-free half ------------------------------------------------------------- */

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

	/*
	 * SOLVED BUT NOT SETTLED. `SolveLoads` is non-destructive, so the frame is of the structure AS
	 * LAID — the state every world-free BuildMode test reports. Nothing is released; the bricks stay
	 * kinematic and stand.
	 */
	Record.Layout.Structure.SolveLoads();

	/* --- and the world half ------------------------------------------------------------- */

	Record.Actors.SetNum(Record.Layout.Structure.NumPieces());

	FBox Bounds(ForceInit);

	for (int32 Piece = 0; Piece < Record.Layout.Structure.NumPieces(); ++Piece)
	{
		if (Record.Layout.Structure.IsPieceRemoved(Piece))
		{
			continue;
		}

		/* THE OFFSET LIVES HERE AND NOWHERE ELSE — see StageOriginYCm and the file header. */
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

	/* --- and the camera ------------------------------------------------------------------ */

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

/** The structure is standing exactly as it was laid, and this is the picture of it. */
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

/** Take the demo building's bricks off the stage, leaving the scenario wall standing. */
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

/** The file landed and it is a real PNG. It was deleted before the run. */
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
	 * THE SIGNATURE AND THE IHDR, READ BY HAND. Eight signature bytes, then a four-byte chunk length,
	 * then "IHDR", then width and height as big-endian 32-bit integers. A byte count alone passes for
	 * a file of random bytes, and a decoder would be a dependency on the very rendering stack under
	 * test.
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

	/*
	 * THE OLD FILE GOES FIRST, SYNCHRONOUSLY, BEFORE ANY LATENT COMMAND IS QUEUED. Everything
	 * downstream reads "the file exists" as "this run rendered a frame", and that reading is only true
	 * if the file cannot have survived from an earlier run.
	 */
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
