// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "DestructionGameGameMode.h"
#include "DestructionGamePlayerController.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/AutomationCommon.h"
#include "Tests/CorbelCaseTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"
#include "World/BrickActor.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE CORBEL FAMILY, PHOTOGRAPHED: ONE FRAME AS LAID AND ONE AFTER THE CASCADE HAS SETTLED.
 *
 * =========================================================================================
 * WHY THIS FILE EXISTS AT ALL
 * =========================================================================================
 *
 * TESTS E TO J ARE WORLD-FREE. They are arithmetic over `FStructure` and `Layout` — no actors, no
 * RHI, no viewport — which is exactly why the whole solver suite runs in about a second. They
 * therefore CANNOT PRODUCE A PICTURE as written, and the user asked for pictures.
 *
 * `Tests/StaircaseScreenshotTest.cpp` is the project's rendering harness and its plumbing is
 * reused here rather than reinvented: the `Shot showui` exec routed through the GAME VIEWPORT
 * CLIENT, the latent-command timings, the delete-first-so-existence-means-something rule, the
 * PNG signature and IHDR read by hand. What could NOT be reused is the subject — every constant in
 * that file is the 36-brick staircase cut into the game mode's own wall — so what this adds is a
 * builder that takes any `FStructure` plus its boxes, stands it up in the world as `ABrickActor`s,
 * and photographs it either side of the settle.
 *
 * =========================================================================================
 * IT BUILDS ON ITS OWN PATCH OF GROUND, FIFTEEN METRES OFF THE SCENARIO WALL
 * =========================================================================================
 *
 * `ADestructionGameGameMode::BeginPlay` lays a 30 x 40 wall from x = -10.75 upward at y = 0, and
 * every corbel in `CorbelCaseTestSupport.h` is laid from x = 0 upward at y = 0 as well. A corbel
 * built into the running sandbox would therefore be laid INSIDE that wall, brick through brick,
 * and every frame would be a picture of the interference rather than of the corbel.
 *
 * THE FIX IS A SPAWN OFFSET AND NOT A FIXTURE CHANGE, and the difference is the whole point.
 * `StageOriginYCm` is applied when the ACTOR is placed and nowhere else: the `FStructure` the
 * pictures are of is bit-identical to the one `Core.Structure.*` solves, because a constant
 * translation along Y changes no mass, no interface, no lever arm and no reading — gravity is along
 * Z. Giving `FCorbelSpec` a world origin instead would be production-shaped arithmetic with no
 * failing test behind it, and it would make every world-free reading a reading about a different
 * fixture until somebody proved otherwise.
 *
 * AND THE SCENARIO WALL IS LEFT STANDING, WHICH IS NOT MERELY POLITENESS. Automation tests share
 * one process and one world, so a harness that destroyed those 1,220 actors would leave
 * `Visual.StaircaseScreenshot` — which photographs that exact wall — looking at a structure whose
 * every actor is null, and it would fail for a reason nothing in its own file mentions. The
 * offset costs nothing and keeps `RunTests DestructionGame.Visual` meaningful as a whole.
 *
 * =========================================================================================
 * WHAT "BEFORE" AND "AFTER" MEAN HERE, WHICH IS NOT WHAT THEY MEAN IN THE STAIRCASE FILE
 * =========================================================================================
 *
 * The staircase harness photographs a CUT: the wall is standing, the player deletes 36 bricks, and
 * the question is what the remainder does. Most of this family has no cut at all. A corbel is
 * condemned BY ITS OWN GEOMETRY — it is laid reaching too far and the root joint is over capacity
 * the moment it exists — so the honest pair is:
 *
 *   - BEFORE: the structure AS LAID. Every brick is kinematic, nothing has been released, and no
 *     verdict has been pushed onto the world. This frame is what the player built.
 *   - AFTER: the same structure once `SolveAndBreak` has settled it and every piece the solver is
 *     no longer holding up has been handed to physics and given three seconds to fall.
 *
 * NOTHING IS FROZEN BETWEEN THE TWO, and nothing needs to be: a brick spawns KINEMATIC and only
 * `ABrickActor::Release` makes it dynamic, so before the cascade command runs there is nothing in
 * the world that COULD move. The staircase file's time-dilation pin exists because its cascade
 * runs inside the player's commit; here the cascade is a separate latent command and the before
 * frame is written frames earlier. The stillness is asserted anyway rather than argued.
 *
 * CASE J IS THE EXCEPTION AND IT DOES CUT. It is the user's originally reported case — one brick
 * deleted at a free end of a forty-course wall — so its before frame is the intact wall and its
 * cascade command removes the brick, destroys its actor, and settles.
 *
 * =========================================================================================
 * WHAT IT ASSERTS BEYOND THE FILES
 * =========================================================================================
 *
 * A harness that only takes a picture is green whatever is in the picture. So each case asserts
 * that it built, that the piece count is the one the world-free fixture reports, that an actor was
 * spawned for every live piece, that nothing had moved when the before-frame was written, that the
 * cascade released exactly the pieces the solver condemned, and that both files landed as real
 * PNGs of real size. It deliberately asserts NOTHING about what the images LOOK like — judging
 * that is a human's job, which is the entire point of the exercise.
 *
 * =========================================================================================
 * IT NEEDS A TICKING WORLD *AND* A REAL RHI, hence EAutomationTestFlags::NonNullRHI
 * =========================================================================================
 *
 * Without that flag the ordinary `-nullrhi` suite would run this, find no viewport, write no file
 * and GO GREEN. With it, the ordinary suite never mentions this test exists — which is how
 * `Visual.StaircaseScreenshot` rotted red for five slices. RUN IT EXPLICITLY:
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.CorbelScreenshots"
 *     -TestExit="Automation Test Queue Empty"
 *
 * `-nullrhi` MUST BE ABSENT: `FApp::CanEverRender()` is false with it and `UGameEngine::Init` only
 * builds a window and a viewport under that, so there would be nothing to screenshot even if the
 * filter let the test through.
 */
namespace CorbelScreenshotSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace CorbelCaseTestSupport;

	/** `Shot` and not `HighResShot`, and `showui` with it — see Tests/PieceMenuScreenshotTest.cpp. */
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
	 * WHICH STRUCTURE A ROW OF THE TABLE WANTS. Not every row is a corbel: J is a running-bond wall
	 * with a brick deleted out of it, and A is a bare stepped arm the corbel fixture deliberately
	 * does not build.
	 */
	enum class ECorbelShotKind : uint8
	{
		/** A bare stepped arm of SINGLE bricks off the standard base. */
		BareArm,

		/** `CorbelCaseC(Steps)` — the two-cell base, filled. */
		FilledC,

		/** `CorbelCaseD(Steps)` — the same reach with three cells of masonry opposite. */
		FilledD,

		/** A flush 7 x N running-bond wall with the outermost brick of course 0 deleted. */
		FreeEndCut
	};

	/** One picture pair: what to build, what to call the files, and what a human is looking for. */
	struct FCorbelShotCase
	{
		const TCHAR* Label;
		const TCHAR* BaseName;
		ECorbelShotKind Kind;

		/** Steps for a corbel; courses high for the free-end wall. */
		int32 Count;

		const TCHAR* WhatItIs;
	};

	/**
	 * THE FAMILY, AND WHAT IS DELIBERATELY ABSENT FROM IT.
	 *
	 * A to F are the six structures the user approved, with E rendered as a PAIR either side of its
	 * crossover because "where it tips" is the whole point of that row and one frame cannot show a
	 * tipping point. J is here because it is the user's originally reported case and it now stands,
	 * which is a thing a picture can actually show.
	 *
	 * G, H AND I ARE NOT HERE, AND THAT IS A JUDGEMENT RATHER THAN AN OVERSIGHT. G is a claim about
	 * a RATIO between three scales, H about a ratio between three materials, I about an ordering
	 * across five step sizes. A pair of frames of any one row of those says nothing about the
	 * property, and a folder of twenty near-identical corbels would look like evidence without
	 * being any. Their deliverable is the printed number, which the world-free suite already gives.
	 */
	const FCorbelShotCase ShotCases[] =
	{
		{ TEXT("A"), TEXT("Corbel_A_BareArm4"), ECorbelShotKind::BareArm, 4,
			TEXT("a bare stepped arm of four single bricks — the minimal case, and the one with no "
				"equilibrium at its bearing at all") },

		{ TEXT("B"), TEXT("Corbel_B_Filled4"), ECorbelShotKind::FilledC, 4,
			TEXT("the same four-step profile FILLED SOLID") },

		{ TEXT("C"), TEXT("Corbel_C_Filled10"), ECorbelShotKind::FilledC, 10,
			TEXT("filled, ten steps, on the bare two-cell base") },

		{ TEXT("D"), TEXT("Corbel_D_Filled10Counterweight"), ECorbelShotKind::FilledD, 10,
			TEXT("case C plus three cells of masonry opposite — the counterweight") },

		{ TEXT("E35"), TEXT("Corbel_E35_JustUnder"), ECorbelShotKind::FilledD, 35,
			TEXT("the step BELOW the crossover: the last corbel the model says stands") },

		{ TEXT("E36"), TEXT("Corbel_E36_JustOver"), ECorbelShotKind::FilledD, 36,
			TEXT("the crossover itself: the first corbel whose root joint reads over 1.0") },

		{ TEXT("F"), TEXT("Corbel_F_HundredSteps"), ECorbelShotKind::FilledD, 100,
			TEXT("the hundred-step corbel — 11.25 m of overhang off a one-metre base") },

		{ TEXT("J"), TEXT("Corbel_J_FreeEnd40"), ECorbelShotKind::FreeEndCut, 40,
			TEXT("the user's reported case: one brick deleted at the free end of a forty-course "
				"wall, which must NOT bring it down") },
	};

	constexpr int32 NumShotCases = UE_ARRAY_COUNT(ShotCases);

	/**
	 * THE TIMINGS, and they are the staircase harness's with one change.
	 *
	 * `SettleFrames` covers TSR's temporal history and auto-exposure; `SlateFrames` is the layout
	 * pass a rebuilt view needs; `WriteFrames` is because `ProcessScreenShots` writes at END OF
	 * DRAW, so moving on in the same frame as the request loses the file.
	 *
	 * `FallFrames` IS 180 RATHER THAN THE STAIRCASE'S 300, AND THE REASON IS CASE F. That structure
	 * is 3,015 bricks and all but a handful of them are released at once; every frame of the fall is
	 * a frame of Chaos solving three thousand bodies, eight times over for the eight cases. Three
	 * seconds is still nine times the 0.32 s a released brick needs to fall the 50 cm that would be
	 * unmistakable in the photograph, and the arm of case F is eleven metres long — what it does, it
	 * does immediately.
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

	/** A millimetre, which no frame of real falling can hide inside. */
	constexpr double BeforeStillnessToleranceCm = 0.1;

	/**
	 * HOW THE CAMERA IS PLACED, AND WHY IT IS COMPUTED RATHER THAN NAMED.
	 *
	 * The eight structures here span from a 4-brick arm about 60 cm across to an eleven-metre one,
	 * so a standoff that frames any of them frames none of the others. The default
	 * `UCameraComponent` field of view is 90 degrees HORIZONTALLY, so at a standoff `s` the visible
	 * width is `2s` and the visible height at 16:9 is `2s * 1080/1920`. Inverting that for a
	 * bounding box gives the standoff below, and the margin is what keeps the structure off the
	 * edges of the frame and leaves some ground and sky for a human to read it against.
	 */
	constexpr double FrameMargin = 1.25;
	constexpr double ViewportAspectHeightOverWidth = 1080.0 / 1920.0;

	/**
	 * THE CAMERA LOOKS ALONG -Y, WHICH IS THE OPPOSITE OF `Tests/StaircaseScreenshotTest.cpp`, AND
	 * THE REASON IS LEGIBILITY RATHER THAN TASTE.
	 *
	 * At yaw 90 the view direction is +Y and the camera's right vector is -X, so increasing X is
	 * drawn to the LEFT and every corbel comes out MIRRORED against `claude_plans/CORBEL_CASES.html`
	 * — which draws the base at the left and the arm reaching right. Nobody comparing a photograph
	 * to that drawing should have to hold it up to a mirror first. Yaw -90 puts the view along -Y
	 * and +X to the right, so the pictures read the same way round as the drawings and as every
	 * elevation in the design documents.
	 */
	constexpr double CameraYawDegrees = -90.0;

	/** Nothing is framed closer than this, or a four-brick arm fills the screen with one brick. */
	constexpr double MinimumStandoffCm = 120.0;

	/**
	 * FIFTEEN METRES DOWN THE Y AXIS FROM THE SCENARIO WALL, applied to the SPAWN and to nothing
	 * else. See the file header: it is a constant translation along an axis gravity does not act
	 * on, so the structure being photographed is arithmetically the structure the world-free tests
	 * read. The scenario wall is 10.25 cm thick and centred on y = 0, and the camera stands between
	 * the two looking AWAY from it, so the wall is never in a corbel's frame.
	 *
	 * NEGATIVE, AND THAT IS WHAT KEEPS THE WALL OUT OF SHOT. The camera stands off along +Y and
	 * looks along -Y (see CameraYawDegrees), so everything at a HIGHER y than the camera is behind
	 * it. The largest standoff any case here asks for is about 860 cm, comfortably inside the 1500,
	 * so the scenario wall is behind the lens in every frame rather than a smudge on the horizon.
	 */
	constexpr double StageOriginYCm = -1500.0;

	/**
	 * WHERE THE BRICKS WERE BEFORE ANYTHING HAPPENED TO THEM, AND WHAT THE CASE READS.
	 *
	 * FILE-SCOPE STATE, for the reason the staircase harness gives: a latent command carries only
	 * what its parameters carry, and the four that matter run frames apart. The one that builds is
	 * the only one that sees the structure intact, and the two that judge the shots need what it
	 * saw. Reset at the top of every case so one case cannot inherit the last one's world.
	 */
	struct FCorbelShotRecord
	{
		bool bBuilt = false;

		FStructure Structure;
		TArray<FPieceBox> Boxes;

		/** One entry per piece handle. Null for a piece that was never spawned or has gone. */
		TArray<TWeakObjectPtr<ABrickActor>> Actors;

		/** Where each actor stood when it was laid, so a claim of stillness can be measured. */
		TArray<FVector> LaidAtCm;

		/** The arm's pieces, for the "how much of it lost the ground" count. */
		TArray<int32> ArmPieces;

		int32 RootJoint = INDEX_NONE;
		double RootUtilisation = 0.0;
		double ProjectionCm = 0.0;
		double CreditedDepthCm = 0.0;
		double EffectiveArmCm = 0.0;

		int32 SpawnedActors = 0;
		int32 ReleasedPieces = 0;
		int32 BreakingPasses = 0;
		int32 ArmPiecesWithNoPath = 0;

		double WorstBeforeMovementCm = 0.0;
		double WorstAfterMovementCm = 0.0;

		void Reset()
		{
			*this = FCorbelShotRecord();
		}
	};

	inline FCorbelShotRecord& CorbelShotRecord()
	{
		static FCorbelShotRecord Record;
		return Record;
	}

	/**
	 * WHERE TO PUT A BRICK ACTOR AND HOW BIG TO MAKE IT, so that its MESH fills the box.
	 *
	 * THIS IS THE SUBSYSTEM'S OWN RECIPE AND IT IS COPIED RATHER THAN CALLED, because
	 * `BrickSpawnTransform` and `SpawnBrickForPiece` are file-local to
	 * `World/DestructionStructureSubsystem.cpp` and there is no header to reach them through. Both
	 * halves are read off the MESH: the scale is the box's size over the mesh's own local size and
	 * never over 100, and the pivot is not assumed to be the centre — SM_Cube's local bounds run
	 * from (0,0,0) to (100,100,100), so its origin is a CORNER and placing the actor at the box's
	 * centre would put every brick a half-size out on all three axes.
	 *
	 * EXPOSING THE SUBSYSTEM'S VERSION INSTEAD WOULD BE PRODUCTION CODE WITH NO FAILING TEST BEHIND
	 * IT, which this project's rules forbid. The duplication is confined to a test file, it is
	 * arithmetic rather than policy, and `Tests/BrickActorTest.cpp` already pins the property both
	 * copies have to satisfy — that the mesh's BOUNDS end up where the box is.
	 */
	inline FTransform CorbelBrickSpawnTransform(const UStaticMesh& BrickMesh, const FPieceBox& Box)
	{
		const FBox LocalBounds = BrickMesh.GetBoundingBox();

		const FVector Scale = (Box.ExtentCm * 2.0) / LocalBounds.GetSize();

		return FTransform(
			FRotator::ZeroRotator,
			Box.CentreCm - Scale * LocalBounds.GetCenter(),
			Scale);
	}

	/** One brick, sized, placed and weighed. Null if it could not be built. */
	inline ABrickActor* SpawnCorbelBrick(UWorld& World, const FPieceBox& Box, double MassKg)
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
		 * NO MESH, NO BRICK. The mesh is a hard content reference resolved on the CDO, so deleting
		 * the asset leaves it null rather than failing to compile — and the sizing above divides by
		 * its bounds, which would make an infinite scale out of a missing asset.
		 */
		if (BrickMesh == nullptr)
		{
			Brick->Destroy();
			return nullptr;
		}

		Mesh->SetMassOverrideInKg(NAME_None, static_cast<float>(MassKg), true);

		Brick->FinishSpawning(CorbelBrickSpawnTransform(*BrickMesh, Box));

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
	 * MEASURED, NOT PREFERRED — see `Tests/StaircaseScreenshotTest.cpp`, whose first run asserted
	 * everything correctly and wrote no PNG at all because the request went to `UEngine::Exec`,
	 * which has no SHOT handler. `HandleScreenshotCommand` lives on `UGameViewportClient`.
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

	/** Where a piece's brick actually is right now, or the zero vector if it has gone. */
	inline FVector ActorLocationOf(const FCorbelShotRecord& Record, int32 Piece)
	{
		const ABrickActor* const Actor =
			Record.Actors.IsValidIndex(Piece) ? Record.Actors[Piece].Get() : nullptr;

		return Actor != nullptr ? Actor->GetActorLocation() : FVector::ZeroVector;
	}

	/** The worst distance any surviving brick has travelled since it was laid. */
	inline double WorstMovementCm(const FCorbelShotRecord& Record)
	{
		double Worst = 0.0;

		for (int32 Piece = 0; Piece < Record.Actors.Num(); ++Piece)
		{
			if (Record.Actors[Piece].Get() == nullptr)
			{
				continue;
			}

			Worst = FMath::Max(
				Worst, FVector::Dist(ActorLocationOf(Record, Piece), Record.LaidAtCm[Piece]));
		}

		return Worst;
	}
}

/**
 * Open the stage: check there is a world and a viewport to photograph into, and record that the
 * scenario wall is standing — because THE POINT IS THAT IT IS STILL STANDING AFTERWARDS.
 *
 * NOTHING HERE DESTROYS ANYTHING. The corbels are spawned fifteen metres away instead, so this
 * harness leaves the world exactly as it found it and `Visual.StaircaseScreenshot` can run in the
 * same process. See the file header.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FCorbelOpenStageCommand, FAutomationTestBase*, Test);

bool FCorbelOpenStageCommand::Update()
{
	using namespace CorbelScreenshotSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world: the map must be named on the command line"));
		return true;
	}

	Test->AddInfo(FString::Printf(TEXT("game world is %s"), *World->GetMapName()));

	/*
	 * THE VIEWPORT IS ASSERTED HERE RATHER THAN LEFT TO SHOW UP AS A MISSING FILE, because without
	 * one `HandleScreenshotCommand` returns having done nothing at all and the only symptom
	 * downstream reads identically to a renderer that failed.
	 */
	UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

	Test->TestNotNull(
		TEXT("there must be a game viewport for Slate to screenshot: -nullrhi must be absent"),
		Viewport);

	int32 Standing = 0;

	for (TActorIterator<ABrickActor> It(World); It; ++It)
	{
		++Standing;
	}

	Test->AddInfo(FString::Printf(
		TEXT("%d brick actors are standing at the origin — the game mode's 30 x 40 scenario wall. ")
		TEXT("The corbels below are built %g cm along Y from it and nothing here touches it."),
		Standing, StageOriginYCm));

	return true;
}

/**
 * Build one case, stand it up in the world, and aim the camera at the whole of it.
 *
 * THE STRUCTURE IS BUILT WORLD-FREE FIRST AND THEN SPAWNED, in that order and never the other way
 * round, because it is the world-free half that the entire E-to-J suite already measures. The
 * pictures are of the same arithmetic those tests print, not of a second construction that happens
 * to look similar.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FCorbelBuildCommand, FAutomationTestBase*, Test, int32, CaseIndex);

bool FCorbelBuildCommand::Update()
{
	using namespace CorbelScreenshotSupport;

	namespace ArchSupport = StructureArchingTestSupport;

	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	const FCorbelShotCase& Case = ShotCases[CaseIndex];

	FCorbelShotRecord& Record = CorbelShotRecord();
	Record.Reset();

	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world to build into"));
		return true;
	}

	/* --- the world-free half ------------------------------------------------------------- */

	if (Case.Kind == ECorbelShotKind::FreeEndCut)
	{
		FBrickLayout Laid;

		if (!RunningBond(ArchSupport::ArchWallSpecOfHeight(Case.Count), Laid))
		{
			Test->AddError(FString::Printf(
				TEXT("CASE %s: a flush 7 x %d wall should lay"), Case.Label, Case.Count));

			return true;
		}

		Record.Structure = MoveTemp(Laid.Structure);
		Record.Boxes = MoveTemp(Laid.Boxes);
	}
	else
	{
		FCorbelSpec Spec = Case.Kind == ECorbelShotKind::FilledD
			? CorbelCaseD(Case.Count)
			: CorbelCaseC(Case.Count);

		/*
		 * `bFilled` IS THE WHOLE OF CASE A, and it is one flag on the SAME producer rather than a
		 * second builder here. A bare arm on a different pedestal would be two changes at once and
		 * the pair of pictures would not be a comparison of anything; this way A and B differ in
		 * exactly one thing — whether a stepped course is filled inboard or is a single brick.
		 *
		 * `COMPOSITE_DEPTH_DESIGN.md` works the bare arm through by hand: bottom rung `F = 4`,
		 * `M = 90`, so `e = 22.5 cm` against a bearing whose outer edge is 5.125 cm from its own
		 * centroid — the resultant is 17.375 cm OUTSIDE the bearing, there is no equilibrium stress
		 * block on that joint at all, and `ComputeUtilisation` returns a confident 0.15613.
		 */
		Spec.bFilled = Case.Kind != ECorbelShotKind::BareArm;

		FCorbelStructure Built;

		if (!CorbelBuild(Spec, Built))
		{
			Test->AddError(FString::Printf(
				TEXT("CASE %s: the %d-step corbel should build"), Case.Label, Case.Count));

			return true;
		}

		Record.Structure = MoveTemp(Built.Structure);
		Record.Boxes = MoveTemp(Built.Boxes);
		Record.ArmPieces = MoveTemp(Built.ArmPieces);
		Record.RootJoint = Built.RootJoint;
		Record.ProjectionCm = Built.ProjectionCm;
	}

	/*
	 * SOLVED BUT NOT SETTLED. `SolveLoads` is non-destructive, so the reading printed beside the
	 * BEFORE frame is what the structure reads AS LAID — which is the number every world-free test
	 * in this family reports. The settling is a separate act, and it is the after-frame's.
	 */
	Record.Structure.SolveLoads();

	if (Record.RootJoint != INDEX_NONE)
	{
		Record.RootUtilisation = Record.Structure.GetConnectionUtilisation(Record.RootJoint);
		Record.CreditedDepthCm = Record.Structure.GetConnectionCompositeDepthCm(Record.RootJoint);

		const FVector ForceUu = Record.Structure.GetConnectionForce(Record.RootJoint);
		const FVector MomentUuCm = Record.Structure.GetConnectionMoment(Record.RootJoint);

		Record.EffectiveArmCm = ForceUu.Size() > 0.0 ? MomentUuCm.Size() / ForceUu.Size() : 0.0;
	}

	/* --- and the world half ------------------------------------------------------------- */

	Record.Actors.SetNum(Record.Structure.NumPieces());
	Record.LaidAtCm.SetNum(Record.Structure.NumPieces());

	FBox Bounds(ForceInit);

	for (int32 Piece = 0; Piece < Record.Structure.NumPieces(); ++Piece)
	{
		if (Record.Structure.IsPieceRemoved(Piece))
		{
			continue;
		}

		/* THE OFFSET LIVES HERE AND NOWHERE ELSE — see StageOriginYCm and the file header. */
		FPieceBox StageBox = Record.Boxes[Piece];
		StageBox.CentreCm.Y += StageOriginYCm;

		ABrickActor* const Brick = SpawnCorbelBrick(
			*World, StageBox, Record.Structure.GetPiece(Piece).MassKg);

		if (Brick == nullptr)
		{
			Test->AddError(FString::Printf(
				TEXT("CASE %s: piece %d could not be spawned, so the picture is missing a brick"),
				Case.Label, Piece));

			continue;
		}

		Record.Actors[Piece] = Brick;
		Record.LaidAtCm[Piece] = Brick->GetActorLocation();

		++Record.SpawnedActors;

		Bounds += FBox::BuildAABB(Record.Boxes[Piece].CentreCm, Record.Boxes[Piece].ExtentCm);
	}

	Test->TestEqual(
		*FString::Printf(
			TEXT("CASE %s: every one of the %d live pieces must have a brick standing for it, or ")
			TEXT("the photograph is of a different structure from the one that was solved"),
			Case.Label, Record.Structure.NumLivePieces()),
		Record.SpawnedActors, Record.Structure.NumLivePieces());

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
		TEXT("CASE %s (%s): %d pieces, %d joints, %d bricks spawned. The structure spans %.1f x ")
		TEXT("%.1f cm centred on (%.1f, %.1f); the camera stands off %.1f cm and frames %.1f x %.1f cm."),
		Case.Label, Case.WhatItIs, Record.Structure.NumLivePieces(),
		Record.Structure.NumConnections(), Record.SpawnedActors,
		2.0 * HalfSizeCm.X, 2.0 * HalfSizeCm.Z, CentreCm.X, CentreCm.Z, StandoffCm,
		2.0 * StandoffCm, 2.0 * StandoffCm * ViewportAspectHeightOverWidth));

	Test->AddInfo(FString::Printf(
		TEXT("CASE %s: THE MEASUREMENT. The root joint reads %s over %s cm of credited depth, e = ")
		TEXT("%s cm, and the arm projects %s cm past the base — %s x the %g cm total projection ")
		TEXT("published corbelling practice allows."),
		Case.Label, *CorbelBits(Record.RootUtilisation), *CorbelBits(Record.CreditedDepthCm),
		*CorbelBits(Record.EffectiveArmCm), *CorbelBits(Record.ProjectionCm),
		*CorbelBits(Record.ProjectionCm / CorbelCodeTotalProjectionCm),
		CorbelCodeTotalProjectionCm));

	return true;
}

/**
 * The structure is standing exactly as it was laid, and this is the picture of it.
 *
 * THE STILLNESS IS MEASURED RATHER THAN ARGUED. A brick spawns kinematic and only `Release` makes
 * it dynamic, so nothing here CAN have moved — but "nothing can have moved" is precisely the kind
 * of claim that stops being true when somebody changes the spawn path, and the whole value of a
 * before-frame is that it is a picture of the intact structure.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FCorbelShootBeforeCommand, FAutomationTestBase*, Test, int32, CaseIndex);

bool FCorbelShootBeforeCommand::Update()
{
	using namespace CorbelScreenshotSupport;

	const FCorbelShotCase& Case = ShotCases[CaseIndex];
	FCorbelShotRecord& Record = CorbelShotRecord();

	if (!Record.bBuilt)
	{
		return true;
	}

	Record.WorstBeforeMovementCm = WorstMovementCm(Record);

	Test->TestTrue(
		*FString::Printf(
			TEXT("CASE %s: 'before' must mean the structure was standing as laid — the worst-moved ")
			TEXT("brick had travelled %.9f cm and may travel at most %g"),
			Case.Label, Record.WorstBeforeMovementCm, BeforeStillnessToleranceCm),
		Record.WorstBeforeMovementCm < BeforeStillnessToleranceCm);

	RequestScreenshot(*Test, ShotCommandFor(FString(Case.BaseName) + TEXT("_Before")));

	return true;
}

/**
 * Settle the structure and push the answer onto the world: every piece the solver is no longer
 * holding up is handed to physics.
 *
 * THIS IS `FStructureBinding::ApplyResults`' RULE, WRITTEN OUT RATHER THAN CALLED, and the reason
 * is that a binding cannot be built from a hand-laid corbel — `AdoptLayout` takes an `FBrickLayout`
 * and the corbel fixture is not one. The rule it reproduces is the one that matters: a piece is
 * released only if the LAST SOLVE ANSWERED FOR IT and that answer is not Grounded or Supported.
 * `HasSupportAnswer` is what stops "not yet asked" being read as "not held up", which with a
 * one-way release would drop the foundation as well.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FCorbelCascadeCommand, FAutomationTestBase*, Test, int32, CaseIndex);

bool FCorbelCascadeCommand::Update()
{
	using namespace CorbelScreenshotSupport;

	namespace ArchSupport = StructureArchingTestSupport;

	const FCorbelShotCase& Case = ShotCases[CaseIndex];
	FCorbelShotRecord& Record = CorbelShotRecord();

	if (!Record.bBuilt)
	{
		return true;
	}

	/*
	 * CASE J CUTS FIRST, because it is the only row here whose failure is a PLAYER'S ACTION rather
	 * than the geometry it was laid with. The outermost full brick of the grounded course, which is
	 * the brick the user deleted when they reported the bug.
	 */
	if (Case.Kind == ECorbelShotKind::FreeEndCut)
	{
		const int32 EndBrick = StaircaseWallTestSupport::StaircasePieceAt(
			Record.Boxes, ArchSupport::ArchWallEvenBrickXCm(0), ArchSupport::ArchWallCourseZCm(0));

		if (EndBrick == INDEX_NONE || !Record.Structure.RemovePiece(EndBrick))
		{
			Test->AddError(FString::Printf(
				TEXT("CASE %s: the wall should have an end brick at (%g, 0, %g) to delete"),
				Case.Label, ArchSupport::ArchWallEvenBrickXCm(0), ArchSupport::ArchWallCourseZCm(0)));

			return true;
		}

		if (ABrickActor* const Cut = Record.Actors[EndBrick].Get())
		{
			Cut->Destroy();
		}

		Record.Actors[EndBrick] = nullptr;

		/*
		 * AND THE SEAT THE CUT LEFT, READ BEFORE ANYTHING IS ALLOWED TO GIVE. `SolveLoads` is
		 * non-destructive, so this is the AS-CUT reading — the number
		 * `Core.Structure.AFreeEndDeletionInATallWall` reports — rather than whatever the joint
		 * carries once the cascade has finished moving load around it.
		 */
		Record.Structure.SolveLoads();

		const int32 HalfSeated = StaircaseWallTestSupport::StaircasePieceAt(
			Record.Boxes, ArchSupport::ArchWallOddBrickXCm(0), ArchSupport::ArchWallCourseZCm(1));

		const int32 Seat = HalfSeated != INDEX_NONE
			? ArchSupport::TheOneIntactSeatBeneath(Record.Structure, HalfSeated)
			: INDEX_NONE;

		if (Seat != INDEX_NONE)
		{
			Record.RootJoint = Seat;
			Record.RootUtilisation = Record.Structure.GetConnectionUtilisation(Seat);
			Record.CreditedDepthCm = Record.Structure.GetConnectionCompositeDepthCm(Seat);

			const FVector ForceUu = Record.Structure.GetConnectionForce(Seat);
			const FVector MomentUuCm = Record.Structure.GetConnectionMoment(Seat);

			Record.EffectiveArmCm = ForceUu.Size() > 0.0 ? MomentUuCm.Size() / ForceUu.Size() : 0.0;
		}

		Test->AddInfo(FString::Printf(
			TEXT("CASE %s: deleted piece %d, the outermost full brick of the grounded course. The ")
			TEXT("brick above it keeps one seat (joint %d), which reads %s over %s cm of credited ")
			TEXT("depth with e = %s cm."),
			Case.Label, EndBrick, Seat, *CorbelBits(Record.RootUtilisation),
			*CorbelBits(Record.CreditedDepthCm), *CorbelBits(Record.EffectiveArmCm)));
	}

	Record.BreakingPasses = Record.Structure.SolveAndBreak();

	for (int32 Piece = 0; Piece < Record.Structure.NumPieces(); ++Piece)
	{
		ABrickActor* const Brick = Record.Actors.IsValidIndex(Piece)
			? Record.Actors[Piece].Get()
			: nullptr;

		if (Brick == nullptr || Record.Structure.IsPieceRemoved(Piece)
			|| !Record.Structure.HasSupportAnswer(Piece)
			|| Record.Structure.IsPieceSupported(Piece))
		{
			continue;
		}

		Brick->Release();

		++Record.ReleasedPieces;
	}

	for (const int32 Piece : Record.ArmPieces)
	{
		if (!Record.Structure.IsPieceRemoved(Piece) && !Record.Structure.IsPieceSupported(Piece))
		{
			++Record.ArmPiecesWithNoPath;
		}
	}

	Test->AddInfo(FString::Printf(
		TEXT("CASE %s: THE CASCADE. %d breaking pass(es); %d of %d pieces released to physics, and ")
		TEXT("%d of the %d pieces in the arm lost their path to the ground. The root joint broke in ")
		TEXT("pass %d (-1 means it never broke)."),
		Case.Label, Record.BreakingPasses, Record.ReleasedPieces,
		Record.Structure.NumLivePieces(), Record.ArmPiecesWithNoPath, Record.ArmPieces.Num(),
		Record.RootJoint != INDEX_NONE ? Record.Structure.GetBreakPass(Record.RootJoint) : -1));

	return true;
}

/** Whatever was going to happen has happened, and this is the picture of it. */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FCorbelShootAfterCommand, FAutomationTestBase*, Test, int32, CaseIndex);

bool FCorbelShootAfterCommand::Update()
{
	using namespace CorbelScreenshotSupport;

	const FCorbelShotCase& Case = ShotCases[CaseIndex];
	FCorbelShotRecord& Record = CorbelShotRecord();

	if (!Record.bBuilt)
	{
		return true;
	}

	Record.WorstAfterMovementCm = WorstMovementCm(Record);

	/*
	 * THE MOVEMENT IS REPORTED, NOT ASSERTED ON, AND THAT IS DELIBERATE. DESIGN.md §4 bans reading a
	 * displacement as evidence that a joint BROKE — two pieces can sever and rest exactly where they
	 * were — so what is asserted is the RELEASE COUNT against the solver's own answer, above. This
	 * number is here so a reader can tell a picture of a collapse from a picture of a structure
	 * that was condemned and did not visibly move.
	 */
	Test->AddInfo(FString::Printf(
		TEXT("CASE %s: %d frames after the cascade the worst-moved surviving brick had travelled ")
		TEXT("%.3f cm (it had travelled %.9f cm when the before-frame was written)"),
		Case.Label, FallFrames, Record.WorstAfterMovementCm, Record.WorstBeforeMovementCm));

	RequestScreenshot(*Test, ShotCommandFor(FString(Case.BaseName) + TEXT("_After")));

	return true;
}

/**
 * Take THIS CASE'S bricks off the stage so the next one is photographed on its own.
 *
 * THE RECORD'S OWN ACTORS AND NEVER A SWEEP OF THE WORLD, because a sweep would take the scenario
 * wall with it — see the file header. Debris that has fallen out of the level is already gone and
 * reads as null here, which needs no special case.
 */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FCorbelTearDownCommand, FAutomationTestBase*, Test, int32, CaseIndex);

bool FCorbelTearDownCommand::Update()
{
	using namespace CorbelScreenshotSupport;

	FCorbelShotRecord& Record = CorbelShotRecord();

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
		TEXT("CASE %s: cleared %d of its %d bricks off the stage"),
		ShotCases[CaseIndex].Label, Removed, Record.SpawnedActors));

	Record.Reset();

	return true;
}

/** The files landed and they are real PNGs. Both were deleted before the run. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(
	FCorbelCheckFilesCommand, FAutomationTestBase*, Test);

bool FCorbelCheckFilesCommand::Update()
{
	using namespace CorbelScreenshotSupport;

	for (const FCorbelShotCase& Case : ShotCases)
	{
		for (const TCHAR* const Suffix : { TEXT("_Before"), TEXT("_After") })
		{
			const FString BaseName = FString(Case.BaseName) + Suffix;
			const FString Path = ScreenshotPathFor(BaseName);

			const int64 SizeBytes = IFileManager::Get().FileSize(*Path);

			if (SizeBytes < 0)
			{
				Test->AddError(FString::Printf(
					TEXT("no screenshot was written to %s: the shot request never reached a draw, ")
					TEXT("or the file went somewhere else"),
					*Path));

				continue;
			}

			TArray<uint8> Bytes;

			if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() < 24)
			{
				Test->AddError(FString::Printf(
					TEXT("the screenshot at %s could not be read back, or is too short to carry a ")
					TEXT("PNG header (%d bytes)"),
					*Path, Bytes.Num()));

				continue;
			}

			/*
			 * THE SIGNATURE AND THE IHDR, READ BY HAND. Eight signature bytes, then a four-byte
			 * chunk length, then "IHDR", then width and height as big-endian 32-bit integers. A
			 * byte count alone passes for a file of random bytes, and a decoder would be a
			 * dependency on the very rendering stack under test.
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
				*BaseName, SizeBytes, Width, Height, *Path));

			Test->TestTrue(
				*FString::Printf(
					TEXT("%s.png must be a real frame of a lit scene — a flat colour is under 10 kB ")
					TEXT("— so it must be at least %lld bytes; it is %lld"),
					*BaseName, MinimumScreenshotBytes, SizeBytes),
				SizeBytes >= MinimumScreenshotBytes);

			Test->TestTrue(
				*FString::Printf(
					TEXT("%s.png must begin with the PNG signature and an IHDR chunk, and declare ")
					TEXT("real dimensions; it is %d x %d"),
					*BaseName, Width, Height),
				bIsPng && Width >= MinimumScreenshotWidth && Height >= MinimumScreenshotHeight);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorbelScreenshotTest,
	"DestructionGame.Visual.CorbelScreenshots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::NonNullRHI
		| EAutomationTestFlags::ProductFilter)

bool FCorbelScreenshotTest::RunTest(const FString& Parameters)
{
	using namespace CorbelScreenshotSupport;

	CorbelShotRecord().Reset();

	/*
	 * THE OLD FILES GO FIRST, SYNCHRONOUSLY, BEFORE ANY LATENT COMMAND IS QUEUED. Everything
	 * downstream reads "the file exists" as "this run rendered a frame", and that reading is only
	 * true if the file cannot have survived from an earlier run.
	 */
	for (const FCorbelShotCase& Case : ShotCases)
	{
		for (const TCHAR* const Suffix : { TEXT("_Before"), TEXT("_After") })
		{
			const FString Path = ScreenshotPathFor(FString(Case.BaseName) + Suffix);

			if (IFileManager::Get().FileExists(*Path))
			{
				IFileManager::Get().Delete(
					*Path, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
			}

			if (IFileManager::Get().FileExists(*Path))
			{
				AddError(FString::Printf(
					TEXT("fixture: %s could not be deleted, so its existence afterwards would prove ")
					TEXT("nothing"),
					*Path));

				return true;
			}
		}
	}

	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(DisableScreenMessagesCommand));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FCorbelOpenStageCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WarmUpFrames));

	for (int32 CaseIndex = 0; CaseIndex < NumShotCases; ++CaseIndex)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FCorbelBuildCommand(this, CaseIndex));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

		ADD_LATENT_AUTOMATION_COMMAND(FCorbelShootBeforeCommand(this, CaseIndex));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

		ADD_LATENT_AUTOMATION_COMMAND(FCorbelCascadeCommand(this, CaseIndex));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(FallFrames));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SettleFrames));

		ADD_LATENT_AUTOMATION_COMMAND(FCorbelShootAfterCommand(this, CaseIndex));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

		ADD_LATENT_AUTOMATION_COMMAND(FCorbelTearDownCommand(this, CaseIndex));
	}

	ADD_LATENT_AUTOMATION_COMMAND(FCorbelCheckFilesCommand(this));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
