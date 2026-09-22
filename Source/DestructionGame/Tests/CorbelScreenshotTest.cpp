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
 * Photographs the corbel family: one frame as laid, one after the cascade settles.
 *
 * The corbel tests (E to J) are world-free, so this builds each FStructure as ABrickActors and
 * reuses Tests/StaircaseScreenshotTest.cpp's plumbing (Shot via the game viewport client, delete-
 * first, hand-read PNG header).
 *
 * Structures spawn at StageOriginYCm, 15 m off the scenario wall, to avoid interpenetrating it. The
 * offset is applied only to actors, so the FStructure is identical to what the world-free tests
 * solve. The scenario wall is left intact because Visual.StaircaseScreenshot shares the world.
 *
 * Before = as laid (bricks spawn kinematic, so nothing can move; still asserted). After = after
 * SolveAndBreak, with unsupported pieces released and given 3 s to fall. Case J also deletes one
 * brick first. Asserts build, actor count, stillness, release count and real PNGs; not what the
 * images look like, which is for a human.
 *
 * Needs a real RHI (NonNullRHI), so the -nullrhi suite skips it. Run explicitly, without -nullrhi:
 *
 *   & "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
 *     "<project>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox
 *     -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
 *     -nosplash -NoSound -unattended -nopause -log
 *     -ExecCmds="Automation RunTests DestructionGame.Visual.CorbelScreenshots"
 *     -TestExit="Automation Test Queue Empty"
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

	/** Debug overlays off. */
	const TCHAR* const DisableScreenMessagesCommand = TEXT("DisableAllScreenMessages");

	/** Which structure a row builds. */
	enum class ECorbelShotKind : uint8
	{
		/** A bare stepped arm of single bricks off the standard base. */
		BareArm,

		/** `CorbelCaseC(Steps)` — the two-cell base, filled. */
		FilledC,

		/** `CorbelCaseD(Steps)` — the same reach with three cells of masonry opposite. */
		FilledD,

		/** A flush 7 x N running-bond wall with the outermost brick of course 0 deleted. */
		FreeEndCut
	};

	/** One picture pair: what to build, file name, and a description. */
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
	 * A to F, with E shot either side of its crossover, plus J (the user's reported case). G, H and I
	 * are omitted on purpose: they are claims about ratios and orderings a single picture cannot show.
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
	 * Staircase harness timings. SettleFrames covers TSR history and auto-exposure; WriteFrames
	 * because screenshots write at end of draw. FallFrames is 180, not 300, because case F releases
	 * ~3,000 bricks; 3 s is still far more than a visible fall needs.
	 */
	constexpr int32 WarmUpFrames = 120;
	constexpr int32 SettleFrames = 60;
	constexpr int32 SlateFrames = 3;
	constexpr int32 WriteFrames = 5;
	constexpr int32 FallFrames = 180;

	/** Floors derived in Tests/PieceMenuScreenshotTest.cpp. */
	constexpr int64 MinimumScreenshotBytes = 32 * 1024;
	constexpr int32 MinimumScreenshotWidth = 640;
	constexpr int32 MinimumScreenshotHeight = 480;

	/** One millimetre. */
	constexpr double BeforeStillnessToleranceCm = 0.1;

	/**
	 * Camera standoff is computed per structure (60 cm to 11 m). With a 90 degree horizontal FOV, at
	 * standoff s the view is 2s wide and 2s * 1080/1920 tall.
	 */
	constexpr double FrameMargin = 1.25;
	constexpr double ViewportAspectHeightOverWidth = 1080.0 / 1920.0;

	/** Looks along -Y so +X is drawn to the right, matching claude_plans/CORBEL_CASES.html. */
	constexpr double CameraYawDegrees = -90.0;

	constexpr double MinimumStandoffCm = 120.0;

	/**
	 * Spawn offset along Y, applied to actors only (gravity is along Z, so readings are unchanged).
	 * Negative so the scenario wall is behind the camera; the largest standoff is ~860 cm.
	 */
	constexpr double StageOriginYCm = -1500.0;

	/**
	 * Per-case state shared across latent commands, which run frames apart. Reset at the start of
	 * each case.
	 */
	struct FCorbelShotRecord
	{
		bool bBuilt = false;

		FStructure Structure;
		TArray<FPieceBox> Boxes;

		/** One per piece handle; null if never spawned or gone. */
		TArray<TWeakObjectPtr<ABrickActor>> Actors;

		/** Laid position of each actor, to measure stillness. */
		TArray<FVector> LaidAtCm;

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
	 * Transform that makes the brick mesh fill the box. Copied from the subsystem's file-local
	 * BrickSpawnTransform. Reads scale and pivot off the mesh bounds: SM_Cube's origin is a corner,
	 * not its centre.
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

		// A missing mesh asset would make the sizing divide by zero bounds.
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

	/** Request a screenshot via the game viewport client; UEngine::Exec has no Shot handler. */
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

	/** Current brick location, or zero if gone. */
	inline FVector ActorLocationOf(const FCorbelShotRecord& Record, int32 Piece)
	{
		const ABrickActor* const Actor =
			Record.Actors.IsValidIndex(Piece) ? Record.Actors[Piece].Get() : nullptr;

		return Actor != nullptr ? Actor->GetActorLocation() : FVector::ZeroVector;
	}

	/** Largest distance any surviving brick has moved since it was laid. */
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

/** Check for a world and viewport, and log the scenario wall's brick count. Destroys nothing. */
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

	// Assert the viewport now; otherwise its absence looks like a render failure later.
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

/** Build one case world-free (as the E-to-J tests do), spawn it, and frame the camera on it. */
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

	// World-free half.

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
		 * Case A is the same producer with bFilled off, so A and B differ in one thing. Its bottom
		 * rung has e = 22.5 cm against a 5.125 cm half-bearing, so the resultant is outside the
		 * bearing (COMPOSITE_DEPTH_DESIGN.md), yet ComputeUtilisation reads 0.15613.
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

	// Non-destructive solve: the as-laid reading the world-free tests report.
	Record.Structure.SolveLoads();

	if (Record.RootJoint != INDEX_NONE)
	{
		Record.RootUtilisation = Record.Structure.GetConnectionUtilisation(Record.RootJoint);
		Record.CreditedDepthCm = Record.Structure.GetConnectionCompositeDepthCm(Record.RootJoint);

		const FVector ForceUu = Record.Structure.GetConnectionForce(Record.RootJoint);
		const FVector MomentUuCm = Record.Structure.GetConnectionMoment(Record.RootJoint);

		Record.EffectiveArmCm = ForceUu.Size() > 0.0 ? MomentUuCm.Size() / ForceUu.Size() : 0.0;
	}

	// World half.

	Record.Actors.SetNum(Record.Structure.NumPieces());
	Record.LaidAtCm.SetNum(Record.Structure.NumPieces());

	FBox Bounds(ForceInit);

	for (int32 Piece = 0; Piece < Record.Structure.NumPieces(); ++Piece)
	{
		if (Record.Structure.IsPieceRemoved(Piece))
		{
			continue;
		}

		// The only place the stage offset is applied.
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

	// Camera.

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

/** Assert nothing has moved since laying, then shoot the before frame. */
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
 * Settle and release every unsupported piece. Reproduces FStructureBinding::ApplyResults' rule
 * (a corbel is not an FBrickLayout, so no binding): release only pieces the last solve answered for
 * as unsupported. HasSupportAnswer keeps "not yet asked" from dropping the foundation.
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

	// Case J deletes the outermost full brick of the grounded course, as the user did.
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

		// As-cut reading of the remaining seat (Core.Structure.AFreeEndDeletionInATallWall).
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

/** Shoot the after frame. */
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

	// Reported, not asserted: displacement is never break evidence (DESIGN.md §4).
	Test->AddInfo(FString::Printf(
		TEXT("CASE %s: %d frames after the cascade the worst-moved surviving brick had travelled ")
		TEXT("%.3f cm (it had travelled %.9f cm when the before-frame was written)"),
		Case.Label, FallFrames, Record.WorstAfterMovementCm, Record.WorstBeforeMovementCm));

	RequestScreenshot(*Test, ShotCommandFor(FString(Case.BaseName) + TEXT("_After")));

	return true;
}

/** Destroy this case's own actors only; a world sweep would take the scenario wall too. */
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

/** Every file landed and is a real PNG. All were deleted before the run. */
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
			 * Hand-read PNG header: 8-byte signature, 4-byte length, "IHDR", then big-endian width
			 * and height.
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

	// Delete old files first, so a file's existence proves this run rendered it.
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
