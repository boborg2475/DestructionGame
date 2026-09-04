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
 * THE BUILD-MODE GHOST, PHOTOGRAPHED: THE GHOST TRACKING A PREVIEW, THEN THE REAL BRICK LANDING
 * WHERE THE GHOST WAS.
 *
 * =========================================================================================
 * WHY THIS FILE EXISTS — THE OWED VISUAL PROOF FOR UI-4a
 * =========================================================================================
 *
 * `Tests/BuildModeComponentTest.cpp` proves the drive loop world-free-ish: UBuildModeComponent
 * begins a build, drives a GHOST actor to the snap the subsystem's non-mutating PreviewBuildPiece
 * predicts, and on confirm grows the structure by one real ABrickActor at exactly that pose. Its
 * central claim — asserted on BOUNDS so the corner-pivot offset cannot make one agree while the
 * other is a half-brick out — is that the ghost's bounds sit EXACTLY where the committed brick
 * lands. That is arithmetic over transforms and boxes, which is why it runs in milliseconds and
 * which is exactly why it CANNOT PRODUCE A PICTURE. This is the picture: two frames of the same
 * loop, so a human can see the ghost's pose coincide with where the brick lands.
 *
 * =========================================================================================
 * THE TWO FRAMES
 * =========================================================================================
 *
 *   FRAME 1 "BuildGhost_Preview": a real grounded SEED brick stands, and the GHOST hovers at the
 *   running-bond next-course snap one cell up and over — the pose a click would commit. This is
 *   "the ghost shows where the click will land".
 *
 *   FRAME 2 "BuildGhost_Placed": the click has landed. The real brick now stands exactly where the
 *   ghost stood in frame 1, and the ghost has moved ONE cell further along the same course to the
 *   next snap. The proof is that the second real brick occupies the first frame's ghost pose.
 *
 * THE CAMERA DOES NOT MOVE BETWEEN THE TWO, deliberately: it is framed once over the union of every
 * pose that will ever appear, so a reader can lay the frames side by side and read the ghost of
 * frame 1 against the brick of frame 2 in the same pixels.
 *
 * =========================================================================================
 * IT DRIVES THE REAL COMPONENT AND THE REAL SUBSYSTEM, NOT A PARALLEL SPAWN LOOP
 * =========================================================================================
 *
 * `Tests/BuildDemoScreenshotTest.cpp` and `Tests/CorbelScreenshotTest.cpp` hand-roll a spawn loop
 * because their subject is a world-free FStructure that has to be stood up. This subject is the
 * COMPONENT ITSELF, so it is driven exactly as `Tests/BuildModeComponentTest.cpp` drives it: an
 * owner actor spawned in the shared game world, a UBuildModeComponent NewObject'd onto it and
 * RegisterComponent'd so GetWorld resolves, then BeginBuild / UpdatePreviewAt / ConfirmPlace. The
 * ghost and the real bricks are the component's and the subsystem's own actors; nothing here spawns
 * a brick by hand. What is reused verbatim from the sibling harnesses is only the CAMERA-AND-FILM
 * plumbing: the head-on frame computed from a bounding box, the fixed-timestep exposure warm-up, the
 * `Shot showui` exec routed through the game viewport client, and the PNG signature read by hand.
 *
 * =========================================================================================
 * IT BUILDS FIFTEEN METRES OFF THE SCENARIO WALL, LIKE THE SIBLING HARNESSES
 * =========================================================================================
 *
 * `ADestructionGameGameMode::BeginPlay` lays a 30 x 40 wall at the origin, and the component's snap
 * brain seeds and grows from wherever the requested cursor is. A build driven at the literal cursors
 * of the unit test — (0,0,0), then (11,0,7.5) — would grow the structure INSIDE that scenario wall,
 * brick through brick, and every frame would be a picture of the interference rather than of the
 * ghost. So every cursor here is translated `StageOriginYCm` down the Y axis, an axis gravity does
 * not act on and the running-bond snap carries through unchanged: the seed lands at (0, -1500, 0),
 * the ghost snaps to (11.25, -1500, 7.5), and the whole loop is arithmetically the one the unit test
 * measures — a pure translation of it, off the scenario wall's frame. Nothing here destroys that
 * wall, so a sibling `Visual.*` shot can run in the same process; the build's own bricks are torn
 * down through the subsystem at the end.
 *
 * =========================================================================================
 * WHAT IT ASSERTS — DELIBERATELY LIGHT, BECAUSE THE POINT IS THE IMAGE
 * =========================================================================================
 *
 * A harness that only takes a picture is green whatever is in the picture, so it asserts the modest
 * invariants that make each frame a picture of the right thing: the build reached the expected piece
 * count at each stage (1 after the seed, 2 after the commit), the ghost actor exists and is VISIBLE
 * in each frame, and each file landed as a real PNG — signature, IHDR, sane dimensions, and a byte
 * count no flat colour could reach. It asserts NOTHING about what the images look like; judging that,
 * and in particular whether the opaque grey hover-tinted ghost reads as distinct from the real
 * bricks, is a human's job.
 *
 * =========================================================================================
 * IT NEEDS A TICKING WORLD *AND* A REAL RHI, hence EAutomationTestFlags::NonNullRHI
 * =========================================================================================
 *
 * Without that flag the ordinary `-nullrhi` suite would run this, find no viewport, write no file
 * and GO GREEN. With it, the ordinary suite never mentions this test exists, so it must be RUN
 * EXPLICITLY. From PowerShell (Git Bash mangles the map path):
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.BuildGhostScreenshot"
 *     -TestExit="Automation Test Queue Empty"
 *
 * `-nullrhi` MUST BE ABSENT: `FApp::CanEverRender()` is false with it and `UGameEngine::Init` only
 * builds a window and a viewport under that, so there would be nothing to screenshot even if the
 * filter let the test through.
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

	/** Debug overlays off, so nothing is burned over the thing a human is being asked to judge. */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/**
	 * THE TIMINGS, WHICH ARE THE SIBLING HARNESSES'.
	 *
	 * `WarmUpFrames` and `SettleFrames` cover TSR's temporal history and auto-exposure; `SlateFrames`
	 * is the layout pass a rebuilt view needs; `WriteFrames` is because `ProcessScreenShots` writes at
	 * END OF DRAW, so moving on in the same frame as the request loses the file. `AdvanceFrames` is a
	 * short settle after the commit spawns the second brick, so its lit surface is present in frame 2;
	 * nothing here is released, so there is no fall to wait on.
	 */
	constexpr int32 WarmUpFrames = 120;
	constexpr int32 SettleFrames = 60;
	constexpr int32 SlateFrames = 3;
	constexpr int32 WriteFrames = 5;
	constexpr int32 AdvanceFrames = 30;

	/**
	 * The floor a real frame of a lit scene clears and a flat colour does not. The task's bar is
	 * 10 kB; a 1080p PNG of a few lit bricks runs far past it, so this is a floor and not a judgement.
	 */
	constexpr int64 MinimumScreenshotBytes = 10 * 1024;
	constexpr int32 MinimumScreenshotWidth = 640;
	constexpr int32 MinimumScreenshotHeight = 480;

	/**
	 * HOW THE CAMERA IS PLACED, COPIED FROM THE SIBLING HARNESSES AND FOR THEIR REASONS.
	 *
	 * The default `UCameraComponent` field of view is 90 degrees HORIZONTALLY, so at a standoff `s`
	 * the visible width is `2s` and the visible height at 16:9 is `2s * 1080/1920`. Inverting that for
	 * the union bounding box gives the standoff, and the margin keeps the bricks off the edges of the
	 * frame with some ground and sky to read them against.
	 */
	constexpr double FrameMargin = 1.25;
	constexpr double ViewportAspectHeightOverWidth = 1080.0 / 1920.0;

	/**
	 * THE CAMERA LOOKS ALONG -Y, WHICH IS HEAD-ON TO THIS WALL. The build is planar in X-Z at
	 * y = -1500 — the seed, the placed brick and the ghost all share it — so a view down the Y axis
	 * is square-on to the wall face. Yaw -90 puts the view along -Y and +X to the right, the same
	 * sense as every elevation in the design documents.
	 */
	constexpr double CameraYawDegrees = -90.0;

	/** Nothing is framed closer than this, so a half-metre of bricks does not fill the screen. */
	constexpr double MinimumStandoffCm = 120.0;

	/**
	 * FIFTEEN METRES DOWN THE Y AXIS FROM THE SCENARIO WALL, applied to every cursor the component
	 * is driven with. See the file header: a pure translation along an axis gravity does not act on,
	 * carried by the running-bond snap, so the loop photographed is arithmetically the one the unit
	 * test measures. The camera stands off along +Y from here and looks along -Y, so the scenario
	 * wall at y = 0 is behind the lens rather than in shot.
	 */
	constexpr double StageOriginYCm = -1500.0;

	/**
	 * THE THREE CURSORS, offset by StageOriginYCm. These are the unit test's (0,0,0) and (11,0,7.5),
	 * plus one cell further on for the second ghost. The seed is grounded and lands at its cursor; the
	 * next-course cursor is off-grid on purpose so the snap has to move it, and the ghost follows the
	 * snap to (11.25, -1500, 7.5); the third cursor is one 22.5 cm grid cell further along the course.
	 */
	const FVector SeedCursorCm(0.0, StageOriginYCm, 0.0);
	const FVector NextCourseCursorCm(11.0, StageOriginYCm, 7.5);
	const FVector FurtherCursorCm(33.75, StageOriginYCm, 7.5);

	/** HALF-extent of the full 21.5 x 10.25 x 6.5 brick — the component's default CurrentExtentCm. */
	const FVector HalfBrickCm(10.75, 5.125, 3.25);

	/**
	 * WHAT THE RUN BUILT, CARRIED BETWEEN LATENT COMMANDS.
	 *
	 * FILE-SCOPE STATE, for the reason the sibling harnesses give: a latent command carries only what
	 * its parameters carry, and the build, the two shots and the file check run frames apart.
	 */
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

	/** How many pieces the live build holds right now, or -1 if the structure has vanished. */
	inline int32 LivePieceCount(UWorld* World, int32 StructureId)
	{
		UDestructionStructureSubsystem* const Subsystem =
			World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;

		const FStructureBinding* const Binding =
			Subsystem != nullptr ? Subsystem->Find(StructureId) : nullptr;

		return Binding != nullptr ? Binding->NumPieces() : -1;
	}

	/**
	 * The ghost exists and is visible, and the build holds the count this stage expects. Asserted at
	 * each shot so a frame is only taken of the state it claims to show.
	 */
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

/**
 * Open the stage: check there is a world and a viewport to photograph into, and leave the scenario
 * wall standing.
 */
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
 * Attach the component, begin the build, seed a grounded brick, preview the next-course snap so the
 * ghost is hovering at it, and aim the camera head-on over the whole loop's footprint.
 *
 * THE DRIVE IS THE COMPONENT'S OWN, exactly as `Tests/BuildModeComponentTest.cpp` drives it: an owner
 * actor, a registered UBuildModeComponent, BeginBuild, a grounded seed via preview + confirm, then a
 * next-course preview that leaves the ghost standing at the snap. The camera is framed once over the
 * union of the seed, the placed pose and the second ghost pose, so it need not move between frames.
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

	/* --- the component, driven --------------------------------------------------------------- */

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

	/* A grounded seed brick at the offset origin: preview then confirm grows the structure to 1. */
	Comp->bBuildGrounded = true;
	Comp->UpdatePreviewAt(SeedCursorCm);
	Comp->ConfirmPlace();

	/*
	 * THE NEXT-COURSE PREVIEW LEAVES THE GHOST HOVERING AT THE SNAP. The cursor is off-grid; the snap
	 * moves it to the running-bond next-course pose (11.25, -1500, 7.5), and the ghost follows the
	 * snap. This is the state frame 1 photographs.
	 */
	Comp->bBuildGrounded = false;
	const FBuildPreview Preview = Comp->UpdatePreviewAt(NextCourseCursorCm);

	Test->TestTrue(
		TEXT("the next-course preview against the seed must be valid"),
		Preview.bValid);

	Test->TestEqual(
		*FString::Printf(
			TEXT("the seed confirm should grow the structure to 1 piece, it holds %d"),
			LivePieceCount(World, Record.StructureId)),
		LivePieceCount(World, Record.StructureId), 1);

	/* --- and the camera, framed over every pose that will ever appear ------------------------ */

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

/** FRAME 1: the seed stands and the ghost hovers at the snap. This is the picture of it. */
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

/**
 * Confirm the previewed pose so the real brick lands where the ghost was, then preview one cell
 * further so the ghost moves on. This is the state frame 2 photographs.
 */
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

	/* The ghost moves ONE cell further along the same course, to the next snap. */
	const FBuildPreview Preview = Comp->UpdatePreviewAt(FurtherCursorCm);

	Test->TestTrue(
		TEXT("the further preview against the two-brick structure must be valid"),
		Preview.bValid);

	Test->AddInfo(FString::Printf(
		TEXT("committed the second brick; the ghost now previews snap kind %d at (%.2f, %.2f, %.2f)"),
		static_cast<int32>(Preview.Kind), Preview.CentreCm.X, Preview.CentreCm.Y, Preview.CentreCm.Z));

	return true;
}

/**
 * FRAME 2: the real brick stands where the ghost was, and the ghost has moved on. This is the
 * picture of it.
 */
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
 * Take the build off the stage, leaving the scenario wall standing.
 *
 * THE COMPONENT AND THE SUBSYSTEM DESTROY THEIR OWN: DestroyComponent takes the ghost with it (its
 * EndPlay owns that), Destroy(StructureId) takes the real bricks, and the owner actor goes last.
 * Nothing sweeps the world, so the scenario wall a sibling `Visual.*` shot needs is untouched.
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

	/*
	 * THE OLD FILES GO FIRST, SYNCHRONOUSLY, BEFORE ANY LATENT COMMAND IS QUEUED. Everything
	 * downstream reads "the file exists" as "this run rendered a frame", and that reading is only true
	 * if the file cannot have survived from an earlier run.
	 */
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
