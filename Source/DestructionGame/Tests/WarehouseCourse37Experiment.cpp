// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/LayoutFile.h"
#include "Core/PieceActions.h"
#include "Core/PieceMenu.h"
#include "Core/PieceSelection.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"
#include "DestructionGameGameMode.h"
#include "DestructionGamePlayerController.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"
#include "World/BrickActor.h"
#include "World/DestructionScenarios.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE COURSE-37 EXPERIMENT ON THE WAREHOUSE — a recorded playtest, not a test of anything.
 *
 * The owner asked (2026-09-18): join `Lvl_Warehouse`, select every piece of course 37 across one
 * long wall exactly as a player would (a primary click on each), photograph the selection, delete
 * them through the player's own door, photograph and TRACK the collapse, and record how long the
 * break decision took and why. Everything it learns is written under `Experiments/WarehouseCourse37/`
 * so the situation can be re-examined later without re-running it.
 *
 * WHY COURSE 37 AND WHY THE BACK WALL. Course 37 is the upper-window SILL course (Z in [277.5, 284]):
 * ten stone sills and the brick between them, plus the six pilaster bricks of that course. Taking the
 * whole course out severs everything above it — the upper storey's jambs, lintels, the cornice and
 * whatever bears on them — from everything below. The BACK (+Y) long wall is the one the level's
 * three-quarter camera faces, so the frames show the cut rather than the far side of the building.
 *
 * NAMED OUTSIDE `DestructionGame.` ON PURPOSE. It is an experiment that takes minutes with a real RHI
 * and asserts only enough to prove its own frames are of the thing it says; the suite never runs it.
 * Run it from PowerShell exactly as `DestructionGame.Visual.ScenarioLevelScreenshots` is run, with
 * `-ExecCmds="Automation RunTests Experiment.WarehouseCourse37"` and WITHOUT `-nullrhi`.
 */
namespace WarehouseCourse37Experiment
{
	using namespace DestructionLayout;

	constexpr int32 ExperimentCourse = 37;
	constexpr double ExperimentCoursePitchCm = 7.5;
	constexpr double ExperimentCourseHeightCm = 6.5;

	/** The back long wall's wythe starts here; its pilasters stand a joint further out. */
	constexpr double ExperimentBackWallYMinCm = 326.25;
	constexpr double ExperimentBackWallFaceYCm = 336.5;

	constexpr double ExperimentGeometryToleranceCm = 0.01;

	/** Frames, at the pinned 1/60 s — the same budget Tests/ScenarioLevelScreenshotTest.cpp uses. */
	constexpr double ExperimentFixedDeltaSeconds = 1.0 / 60.0;
	constexpr int32 ExperimentFrozenFrames = 30;
	constexpr int32 ExperimentSlateFrames = 3;
	constexpr int32 ExperimentExposureFrames = 60;
	constexpr int32 ExperimentWriteFrames = 5;
	constexpr int32 ExperimentReframeFrames = 40;
	constexpr int32 ExperimentMapLoadFrameBudget = 3000;

	constexpr double ExperimentAspectHeightOverWidth = 1080.0 / 1920.0;

	inline FString ExperimentFolder()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Experiments/WarehouseCourse37"));
	}

	inline FString ExperimentShotPath(const FString& BaseName)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir() / BaseName + TEXT(".png"));
	}

	/** Append a line to a file in the experiment folder, creating it on first use. */
	inline void ExperimentAppend(const FString& FileName, const FString& Line)
	{
		FFileHelper::SaveStringToFile(
			Line + LINE_TERMINATOR, *(ExperimentFolder() / FileName),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(),
			FILEWRITE_Append);
	}

	inline void ExperimentNote(FAutomationTestBase& Test, const FString& Line)
	{
		Test.AddInfo(Line);
		ExperimentAppend(TEXT("run.log"), Line);
	}

	/**
	 * BUFFERED, FOR THE BIG DUMPS. ExperimentAppend opens, appends and closes the file per line, which
	 * is fine for a log and was a 3.2 s stall on the frame after the cut when it wrote 20,000 lines of
	 * joints and pieces — a stall the first two runs' engine logs attribute to the release frame and
	 * that was in fact the harness. A dump accumulates in memory and lands in one write.
	 */
	inline void ExperimentBuffer(FString& Buffer, const FString& Line)
	{
		Buffer += Line;
		Buffer += LINE_TERMINATOR;
	}

	inline void ExperimentFlush(const FString& FileName, const FString& Buffer)
	{
		FFileHelper::SaveStringToFile(
			Buffer, *(ExperimentFolder() / FileName),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get());
	}

	/** Is this box a piece of the chosen course of the back long wall (wythe or pilaster)? */
	inline bool ExperimentIsCutPiece(const FPieceBox& Box)
	{
		const double ZLo = Box.CentreCm.Z - Box.ExtentCm.Z;
		const double ZHi = Box.CentreCm.Z + Box.ExtentCm.Z;
		const double YLo = Box.CentreCm.Y - Box.ExtentCm.Y;

		const double CourseLo = ExperimentCourse * ExperimentCoursePitchCm;
		const double CourseHi = CourseLo + ExperimentCourseHeightCm;

		return FMath::IsNearlyEqual(ZLo, CourseLo, ExperimentGeometryToleranceCm)
			&& FMath::IsNearlyEqual(ZHi, CourseHi, ExperimentGeometryToleranceCm)
			&& YLo > ExperimentBackWallYMinCm - ExperimentGeometryToleranceCm;
	}

	/** The six pilaster slots of a long wall, as X ranges (grid units 3, 13, ... 53 of pitch 11.25). */
	inline bool ExperimentBehindAPilaster(const FPieceBox& Box)
	{
		const double XLo = Box.CentreCm.X - Box.ExtentCm.X;
		const double XHi = Box.CentreCm.X + Box.ExtentCm.X;

		for (int32 Slot = 0; Slot < 6; ++Slot)
		{
			const double PilasterLo = (3 + 10 * Slot) * 11.25;
			const double PilasterHi = PilasterLo + 21.5;

			if (XHi > PilasterLo + ExperimentGeometryToleranceCm && XLo < PilasterHi - ExperimentGeometryToleranceCm)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * The ray a player would click this piece along. Everything on the outer face is reached from
	 * OUTSIDE the building along -Y, which is where the camera is. A wythe brick standing directly
	 * behind a pilaster is hidden from there, so it is reached from INSIDE along +Y, where nothing
	 * stands between the floor of the building and the wall's inner face.
	 *
	 * NOT FROM INSIDE FOR THE CORNER BRICKS: the two end walls run along Y at X in [0, 10.25] and
	 * [641.25, 651.5], so an inside ray at a corner brick's X starts INSIDE an end-wall brick and
	 * clicks that instead — which is exactly what the first run of this did, and the selection then
	 * held 31 pieces of which two were the wrong ones.
	 */
	inline void ExperimentRayFor(const FPieceBox& Box, FVector& OutStart, FVector& OutEnd)
	{
		const double YLo = Box.CentreCm.Y - Box.ExtentCm.Y;
		const bool bPilaster = YLo > ExperimentBackWallFaceYCm;
		const bool bFromInside = !bPilaster && ExperimentBehindAPilaster(Box);

		OutEnd = Box.CentreCm;
		OutStart = FVector(Box.CentreCm.X, bFromInside ? 280.0 : 420.0, Box.CentreCm.Z);
	}

	inline const TCHAR* ExperimentMaterialName(const FStructure& S, int32 Piece)
	{
		const DestructionProfiles::FMaterialProfile* const M = S.GetPiece(Piece).Material;
		for (const DestructionProfiles::FNamedMaterialProfile& Row : DestructionProfiles::AllMaterialProfiles())
		{
			if (&Row.Profile == M)
			{
				return Row.Name;
			}
		}
		return TEXT("<none>");
	}

	inline const TCHAR* ExperimentSupportName(EPieceSupport S)
	{
		switch (S)
		{
			case EPieceSupport::Grounded:  return TEXT("Grounded");
			case EPieceSupport::Supported: return TEXT("Supported");
			case EPieceSupport::Stranded:  return TEXT("Stranded");
			default:                       return TEXT("Falling");
		}
	}

	inline int32 ExperimentJointCount(const FStructure& S, int32 Piece)
	{
		int32 N = 0;
		for (int32 J = 0; J < S.NumConnections(); ++J)
		{
			const FConnection& C = S.GetConnection(J);
			if (C.PieceA == Piece || C.PieceB == Piece)
			{
				++N;
			}
		}
		return N;
	}

	inline void ExperimentShot(FAutomationTestBase& Test, const FString& BaseName)
	{
		UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;

		if (Viewport == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("no game viewport to photograph %s into"), *BaseName));
			return;
		}

		Viewport->Exec(nullptr, *FString::Printf(TEXT("Shot showui filename=%s -nosuffix"), *BaseName), *GLog);
	}

	inline float ExperimentSetDilation(UWorld* World, float Dilation)
	{
		AWorldSettings* const Settings = World != nullptr ? World->GetWorldSettings() : nullptr;
		return Settings != nullptr ? Settings->SetTimeDilation(Dilation) : -1.0f;
	}

	/** Put the one player at a viewpoint: control rotation and pawn location, as the game mode does. */
	inline void ExperimentFrame(UWorld* World, const DestructionScenarios::FViewpoint& View)
	{
		APlayerController* const Controller = World != nullptr ? World->GetFirstPlayerController() : nullptr;
		if (Controller == nullptr)
		{
			return;
		}
		Controller->SetControlRotation(View.Rotation);
		if (APawn* const Pawn = Controller->GetPawn())
		{
			Pawn->SetActorLocation(View.LocationCm);
		}
	}

	struct FExperimentRecord
	{
		bool bJoined = false;
		TWeakObjectPtr<UWorld> WorldBeforeTravel;
		int32 RowIndex = INDEX_NONE;
		int32 StructureId = INDEX_NONE;
		TArray<FVector> LaidAtCm;
		double BuiltAtSeconds = 0.0;

		/** The pieces of the course, in piece-index order, and their refs. */
		TArray<int32> CutPieces;
		TArray<FPieceRef> CutRefs;

		DestructionScenarios::FViewpoint LevelView;
		DestructionScenarios::FViewpoint CloseView;

		/* The collapse half. */
		double CutAtSeconds = 0.0;
		double CommitMs = 0.0;

		/** Wall clock (FPlatformTime) when the commit returned, and at the last tracked frame. */
		double CutWallSeconds = 0.0;
		double LastFrameWallSeconds = 0.0;
		FStructure::FSolveAndBreakReport CutReport;
		TArray<int32> ReleasedPieces;
		TArray<FVector> PreviousCm;
		int32 FramesTracked = 0;
		TArray<FString> ShotsTaken;
		TArray<FString> ShotsDue;

		/** The big dumps, buffered until the end of the run. */
		FString FallSummaryCsv;
		FString FallPiecesCsv;
		FString PiecesAfterCutCsv;
		FString JointsCsv;

		void Reset() { *this = FExperimentRecord(); }
	};

	/** The row that takes a brick out of the world, by label — the same lookup the game mode makes. */
	inline const FPieceAction* ExperimentDeleteAction()
	{
		for (const FPieceAction& Action : AllPieceActions())
		{
			if (Action.Label != nullptr && FCString::Strcmp(Action.Label, TEXT("Delete")) == 0)
			{
				return &Action;
			}
		}
		return nullptr;
	}

	/** One SolveAndBreak report, laid out for a reader, appended to a file in the folder. */
	inline void ExperimentWriteReport(const FString& FileName, const FString& Heading, const FStructure::FSolveAndBreakReport& R)
	{
		ExperimentAppend(FileName, Heading);
		ExperimentAppend(FileName, FString::Printf(
			TEXT("total %.3f ms; %d breaking pass(es), %d passes run; live pieces before %d; intact joints %d -> %d"),
			R.TotalMs, R.BreakingPasses, R.Passes.Num(), R.LivePiecesBefore, R.IntactJointsBefore, R.IntactJointsAfter));
		ExperimentAppend(FileName,
			TEXT("pass,passMs,solveMs,supportListsMs,reseatMs,fixpointMs,fixpointIterations,archingMs,gateMs,gateDisposition,jointsSeveredByGate,")
			TEXT("sweepMs,jointsGivenToSweep,proverMs,proverPoses,proverLpPivots,proverLpMs,proverLastBlocks,proverFell,")
			TEXT("jointsSeveredByProver,piecesFelledByProver,livePiecesAfter,intactJointsAfter,notHeldAfter"));
		for (const FStructure::FBreakPassReport& P : R.Passes)
		{
			ExperimentAppend(FileName, FString::Printf(
				TEXT("%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%d,%d,%.3f,%d,%.3f,%d,%d,%.3f,%d,%d,%d,%d,%d,%d,%d"),
				P.Pass, P.PassMs, P.Solve.TotalMs, P.Solve.SupportListsMs, P.Solve.ReseatMs, P.Solve.FixpointMs,
				P.Solve.FixpointIterations, P.Solve.ArchingMs, P.GateMs, P.GateDisposition, P.JointsSeveredByGate, P.CapacitySweepMs,
				P.JointsGivenToSweep, P.RegionalProverMs, P.RegionalPoses, P.RegionalLpPivots, P.RegionalLpMs,
				P.RegionalLastBlocks, P.bRegionalFell ? 1 : 0, P.JointsSeveredByProver, P.PiecesFelledByProver,
				P.LivePieces, P.IntactJointsAfter, P.NotHeldAfter));

			for (int32 It = 0; It < P.Solve.FixpointIterations; ++It)
			{
				ExperimentAppend(FileName, FString::Printf(
					TEXT("  pass %d fixpoint iteration %d: %d reached from the ground, %d overturned, %d stranded, %d released from a refused arch"),
					P.Pass, It + 1,
					P.Solve.SupportedPerIteration.IsValidIndex(It) ? P.Solve.SupportedPerIteration[It] : -1,
					P.Solve.OverturnedPerIteration.IsValidIndex(It) ? P.Solve.OverturnedPerIteration[It] : -1,
					P.Solve.StrandedPerIteration.IsValidIndex(It) ? P.Solve.StrandedPerIteration[It] : -1,
					P.Solve.ReleasedPerIteration.IsValidIndex(It) ? P.Solve.ReleasedPerIteration[It] : -1));
			}
		}
	}

	inline FExperimentRecord& ExperimentRecord()
	{
		static FExperimentRecord Record;
		return Record;
	}

	inline ADestructionGamePlayerController* ExperimentController(UWorld* World)
	{
		return World != nullptr ? Cast<ADestructionGamePlayerController>(World->GetFirstPlayerController()) : nullptr;
	}

	inline FStructureBinding* ExperimentBinding(UWorld* World, int32 StructureId)
	{
		UDestructionStructureSubsystem* const Subsystem =
			World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;
		return Subsystem != nullptr ? Subsystem->Find(StructureId) : nullptr;
	}
}

/* ---- pin the clock, so a frame count is a duration ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FExperimentOpenSessionCommand, FAutomationTestBase*, Test);

bool FExperimentOpenSessionCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	Test->TestTrue(TEXT("a game viewport is needed: -nullrhi must be absent"),
		GEngine != nullptr && GEngine->GameViewport != nullptr);

	FApp::SetFixedDeltaTime(ExperimentFixedDeltaSeconds);
	FApp::SetUseFixedTimeStep(true);

	ExperimentNote(*Test, FString::Printf(TEXT("clock pinned at %.6f s per frame"), FApp::GetFixedDeltaTime()));
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FExperimentCloseSessionCommand, FAutomationTestBase*, Test);

bool FExperimentCloseSessionCommand::Update()
{
	FApp::SetUseFixedTimeStep(false);
	return true;
}

/* ---- travel to Lvl_Warehouse ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FExperimentOpenMapCommand, FAutomationTestBase*, Test);

bool FExperimentOpenMapCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	FExperimentRecord& Record = ExperimentRecord();
	Record.Reset();

	Record.RowIndex = DestructionScenarios::IndexOfName(FName(TEXT("warehouse")));

	if (Record.RowIndex == INDEX_NONE)
	{
		Test->AddError(TEXT("the catalogue has no 'warehouse' row"));
		return true;
	}

	const DestructionScenarios::FScenario& Row = DestructionScenarios::Catalogue()[Record.RowIndex];
	const FString Package = FString(TEXT("/Game/Maps/Scenarios/")) + Row.MapName;

	if (!FPackageName::DoesPackageExist(Package))
	{
		Test->AddError(FString::Printf(TEXT("no map at %s"), *Package));
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	if (World == nullptr)
	{
		Test->AddError(TEXT("there is no game world: a map must be named on the command line"));
		return true;
	}

	Record.WorldBeforeTravel = World;
	ExperimentNote(*Test, FString::Printf(TEXT("opening %s"), *Package));
	GEngine->Exec(World, *FString::Printf(TEXT("Open %s"), *Package));
	return true;
}

/* ---- wait for the level to build, freeze it, and record it as laid ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FExperimentJoinCommand, FAutomationTestBase*, Test, int32, FramesWaited);

bool FExperimentJoinCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	FExperimentRecord& Record = ExperimentRecord();
	if (Record.RowIndex == INDEX_NONE)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	ADestructionGameGameMode* const GameMode =
		World != nullptr ? World->GetAuthGameMode<ADestructionGameGameMode>() : nullptr;

	const bool bIsThisRow = World != nullptr
		&& World != Record.WorldBeforeTravel.Get()
		&& GameMode != nullptr
		&& GameMode->GetSelectedScenarioRow() == Record.RowIndex
		&& GameMode->GetBuiltStructureId() != INDEX_NONE;

	if (!bIsThisRow)
	{
		if (++FramesWaited < ExperimentMapLoadFrameBudget)
		{
			return false;
		}
		Test->AddError(FString::Printf(TEXT("Lvl_Warehouse never came up after %d frames"), FramesWaited));
		return true;
	}

	ExperimentSetDilation(World, 0.0f);
	Record.BuiltAtSeconds = World->GetTimeSeconds();
	Record.StructureId = GameMode->GetBuiltStructureId();

	FStructureBinding* const Binding = ExperimentBinding(World, Record.StructureId);
	if (Binding == nullptr)
	{
		Test->AddError(TEXT("the game mode's structure id names no binding"));
		return true;
	}

	const FStructure& S = Binding->GetStructure();

	Record.LaidAtCm.SetNumZeroed(Binding->NumPieces());
	FBox BoundsCm(ForceInit);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		if (const AActor* const Brick = Cast<AActor>(Binding->GetActor(Piece)))
		{
			Record.LaidAtCm[Piece] = Brick->GetActorLocation();
		}
		const FPieceBox& Box = Binding->GetBinding(Piece).Box;
		BoundsCm += FBox(Box.CentreCm - Box.ExtentCm, Box.CentreCm + Box.ExtentCm);

		if (ExperimentIsCutPiece(Box))
		{
			Record.CutPieces.Add(Piece);
			FPieceRef Ref;
			Ref.StructureId = Record.StructureId;
			Ref.PieceIndex = Piece;
			Record.CutRefs.Add(Ref);
		}
	}

	const DestructionScenarios::FScenario& Row = DestructionScenarios::Catalogue()[Record.RowIndex];
	Record.LevelView = DestructionScenarios::ViewpointFor(BoundsCm, ExperimentAspectHeightOverWidth, Row.Framing);

	/* A closer frame on the back wall alone, through the same production framing. */
	FBox WallCm(ForceInit);
	for (const int32 Piece : Record.CutPieces)
	{
		const FPieceBox& Box = Binding->GetBinding(Piece).Box;
		WallCm += FBox(Box.CentreCm - Box.ExtentCm, Box.CentreCm + Box.ExtentCm);
	}
	WallCm.Min.Z = 0.0;
	WallCm.Max.Z = BoundsCm.Max.Z;
	WallCm.Min.Y = ExperimentBackWallYMinCm;
	Record.CloseView = DestructionScenarios::ViewpointFor(
		WallCm, ExperimentAspectHeightOverWidth, DestructionScenarios::EScenarioFraming::ThreeQuarter);

	ExperimentNote(*Test, FString::Printf(
		TEXT("JOINED Lvl_Warehouse at world time %.4f s: %d pieces, %d joints, %d live; bounds X[%.3f,%.3f] Y[%.3f,%.3f] Z[%.3f,%.3f]"),
		Record.BuiltAtSeconds, S.NumPieces(), S.NumConnections(), S.NumLivePieces(),
		BoundsCm.Min.X, BoundsCm.Max.X, BoundsCm.Min.Y, BoundsCm.Max.Y, BoundsCm.Min.Z, BoundsCm.Max.Z));

	ExperimentNote(*Test, FString::Printf(
		TEXT("COURSE %d OF THE BACK WALL: %d pieces (Z in [%.1f, %.1f], Y >= %.2f). Level view (%.1f, %.1f, %.1f) pitch %.1f yaw %.1f; close view (%.1f, %.1f, %.1f) pitch %.1f yaw %.1f"),
		ExperimentCourse, Record.CutPieces.Num(),
		ExperimentCourse * ExperimentCoursePitchCm, ExperimentCourse * ExperimentCoursePitchCm + ExperimentCourseHeightCm,
		ExperimentBackWallYMinCm,
		Record.LevelView.LocationCm.X, Record.LevelView.LocationCm.Y, Record.LevelView.LocationCm.Z,
		Record.LevelView.Rotation.Pitch, Record.LevelView.Rotation.Yaw,
		Record.CloseView.LocationCm.X, Record.CloseView.LocationCm.Y, Record.CloseView.LocationCm.Z,
		Record.CloseView.Rotation.Pitch, Record.CloseView.Rotation.Yaw));

	Test->TestTrue(TEXT("course 37 of the back wall must name pieces"), Record.CutPieces.Num() > 0);

	/* The selection, as data: one line per piece, as laid. */
	ExperimentAppend(TEXT("selection.csv"),
		TEXT("piece,material,grounded,massKg,minX,minY,minZ,maxX,maxY,maxZ,joints,supportAsLaid,kind"));

	for (const int32 Piece : Record.CutPieces)
	{
		const FPieceBox& Box = Binding->GetBinding(Piece).Box;
		const FVector Lo = Box.CentreCm - Box.ExtentCm;
		const FVector Hi = Box.CentreCm + Box.ExtentCm;
		const bool bPilaster = Lo.Y > ExperimentBackWallFaceYCm;

		ExperimentAppend(TEXT("selection.csv"), FString::Printf(
			TEXT("%d,%s,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%s,%s"),
			Piece, ExperimentMaterialName(S, Piece), S.GetPiece(Piece).bIsGrounded ? 1 : 0,
			S.GetPiece(Piece).MassKg, Lo.X, Lo.Y, Lo.Z, Hi.X, Hi.Y, Hi.Z,
			ExperimentJointCount(S, Piece), ExperimentSupportName(S.GetPieceSupport(Piece)),
			bPilaster ? TEXT("pilaster") : TEXT("wythe")));
	}

	Record.bJoined = true;
	return true;
}

/* ---- let the exposure converge ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FExperimentThawCommand, FAutomationTestBase*, Test);

bool FExperimentThawCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	if (ExperimentRecord().bJoined)
	{
		ExperimentSetDilation(AutomationCommon::GetAnyGameWorld(), 1.0f);
	}
	return true;
}

/* ---- select the course exactly as a player would: one primary click per piece ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FExperimentSelectCommand, FAutomationTestBase*, Test);

bool FExperimentSelectCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	FExperimentRecord& Record = ExperimentRecord();
	if (!Record.bJoined)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	ADestructionGamePlayerController* const Controller = ExperimentController(World);
	FStructureBinding* const Binding = ExperimentBinding(World, Record.StructureId);

	if (Controller == nullptr || Binding == nullptr)
	{
		Test->AddError(TEXT("no player controller or binding to select through"));
		return true;
	}

	int32 Landed = 0;
	const double T0 = FPlatformTime::Seconds();

	for (const int32 Piece : Record.CutPieces)
	{
		FVector Start, End;
		ExperimentRayFor(Binding->GetBinding(Piece).Box, Start, End);

		const bool bHit = Controller->PrimaryAlongRay(Start, End);
		if (bHit)
		{
			++Landed;
		}
		else
		{
			ExperimentNote(*Test, FString::Printf(
				TEXT("click on piece %d along (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f) did NOT land"),
				Piece, Start.X, Start.Y, Start.Z, End.X, End.Y, End.Z));
		}
	}

	const double T1 = FPlatformTime::Seconds();

	const FPieceSelection& Selection = Controller->GetPieceSelection();

	int32 Highlighted = 0;
	for (const int32 Piece : Record.CutPieces)
	{
		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Piece));
		if (Brick != nullptr && Brick->GetHighlight() != EBrickHighlight::None)
		{
			++Highlighted;
		}
	}

	ExperimentNote(*Test, FString::Printf(
		TEXT("SELECTED at world time %.4f s (%.4f s into the hold): %d clicks issued, %d landed, selection holds %d, %d bricks wear a highlight; clicking took %.1f ms"),
		World->GetTimeSeconds(), World->GetTimeSeconds() - Record.BuiltAtSeconds,
		Record.CutPieces.Num(), Landed, Selection.Num(), Highlighted, (T1 - T0) * 1000.0));

	Test->TestEqual(TEXT("every click must land"), Landed, Record.CutPieces.Num());
	Test->TestEqual(TEXT("the selection must hold the whole course"), Selection.Num(), Record.CutPieces.Num());

	/* AND IT MUST BE THESE PIECES, not merely this many: a ray can land on a neighbour. */
	int32 Missing = 0;
	for (const FPieceRef& Ref : Record.CutRefs)
	{
		if (!Selection.Contains(Ref))
		{
			++Missing;
			ExperimentNote(*Test, FString::Printf(TEXT("piece %d is NOT in the selection"), Ref.PieceIndex));
		}
	}
	for (const FPieceRef& Ref : Selection.Refs())
	{
		if (!Record.CutRefs.Contains(Ref))
		{
			ExperimentNote(*Test, FString::Printf(TEXT("piece %d is in the selection but is NOT part of the course"), Ref.PieceIndex));
		}
	}
	Test->TestEqual(TEXT("every piece of the course must be in the selection"), Missing, 0);

	ExperimentShot(*Test, TEXT("Warehouse_C37_Before"));
	return true;
}

/* ---- the same frame without the UI, so the menu panel hides none of the wall ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FExperimentShotNoUICommand, FAutomationTestBase*, Test, FString, BaseName);

bool FExperimentShotNoUICommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	if (!ExperimentRecord().bJoined)
	{
		return true;
	}

	UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;
	UWorld* const World = AutomationCommon::GetAnyGameWorld();

	if (Viewport == nullptr)
	{
		Test->AddError(TEXT("no game viewport"));
		return true;
	}

	ExperimentNote(*Test, FString::Printf(TEXT("SHOT %s (no UI) at world time %.4f s"), *BaseName, World != nullptr ? World->GetTimeSeconds() : -1.0));
	Viewport->Exec(nullptr, *FString::Printf(TEXT("Shot filename=%s -nosuffix"), *BaseName), *GLog);
	return true;
}

/* ---- move to a viewpoint (0 = level's own, 1 = close on the back wall) ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FExperimentReframeCommand, FAutomationTestBase*, Test, int32, Which);

bool FExperimentReframeCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	FExperimentRecord& Record = ExperimentRecord();
	if (!Record.bJoined)
	{
		return true;
	}
	ExperimentFrame(AutomationCommon::GetAnyGameWorld(), Which == 0 ? Record.LevelView : Record.CloseView);
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FExperimentShotCommand, FAutomationTestBase*, Test, FString, BaseName);

bool FExperimentShotCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	if (!ExperimentRecord().bJoined)
	{
		return true;
	}
	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	ExperimentNote(*Test, FString::Printf(TEXT("SHOT %s at world time %.4f s"), *BaseName, World != nullptr ? World->GetTimeSeconds() : -1.0));
	ExperimentShot(*Test, BaseName);
	return true;
}

/* ---- copy every frame into the experiment folder, and check it is a real file ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FExperimentCollectCommand, FAutomationTestBase*, Test, TArray<FString>, BaseNames);

bool FExperimentCollectCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	for (const FString& BaseName : BaseNames)
	{
		const FString From = ExperimentShotPath(BaseName);
		const FString To = ExperimentFolder() / BaseName + TEXT(".png");
		const int64 Size = IFileManager::Get().FileSize(*From);

		Test->TestTrue(*FString::Printf(TEXT("%s must have been written (%lld bytes)"), *From, Size), Size > 32 * 1024);

		if (Size > 0)
		{
			IFileManager::Get().Copy(*To, *From, /*Replace*/ true);
			ExperimentNote(*Test, FString::Printf(TEXT("FRAME %s -> %s (%lld bytes)"), *BaseName, *To, Size));
		}
	}
	return true;
}

/* ---- the level's own settle at the end of its hold: the as-laid baseline solve, recorded ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FExperimentBaselineCommand, FAutomationTestBase*, Test);

bool FExperimentBaselineCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	FExperimentRecord& Record = ExperimentRecord();
	if (!Record.bJoined)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	FStructureBinding* const Binding = ExperimentBinding(World, Record.StructureId);
	if (Binding == nullptr)
	{
		return true;
	}

	const FStructure::FSolveAndBreakReport& R = Binding->GetStructure().GetLastSolveAndBreakReport();

	Test->TestTrue(TEXT("the hold must have expired and the level run before the cut"),
		World->GetTimeSeconds() - Record.BuiltAtSeconds > 4.0 && R.Passes.Num() > 0);

	ExperimentWriteReport(TEXT("break_report.txt"),
		FString::Printf(TEXT("=== BASELINE: the level's own settle when its hold expired, nothing cut (world time %.4f s) ==="),
			World->GetTimeSeconds()), R);

	ExperimentNote(*Test, FString::Printf(
		TEXT("BASELINE settle at world time %.4f s: %.1f ms, %d breaking passes, %d passes; solve %.1f ms with %d fixpoint iterations; %d not held"),
		World->GetTimeSeconds(), R.TotalMs, R.BreakingPasses, R.Passes.Num(),
		R.Passes.Num() > 0 ? R.Passes[0].Solve.TotalMs : 0.0,
		R.Passes.Num() > 0 ? R.Passes[0].Solve.FixpointIterations : 0,
		R.Passes.Num() > 0 ? R.Passes.Last().NotHeldAfter : 0));

	return true;
}

/* ---- DELETE the course through the player's own batch-commit door, timed, and dump everything ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FExperimentDeleteCommand, FAutomationTestBase*, Test);

bool FExperimentDeleteCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	FExperimentRecord& Record = ExperimentRecord();
	if (!Record.bJoined)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	UDestructionStructureSubsystem* const Subsystem =
		World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;
	ADestructionGamePlayerController* const Controller = ExperimentController(World);
	const FPieceAction* const Delete = ExperimentDeleteAction();

	if (Subsystem == nullptr || Delete == nullptr || Controller == nullptr)
	{
		Test->AddError(TEXT("no subsystem, controller or Delete action"));
		return true;
	}

	/*
	 * THE PLAYER'S OWN DELETE: the presented menu's Delete row, chosen exactly as the button chooses
	 * it. ChoosePieceMenuRow commits the row's refs through CommitPieceActionForAll — one removal
	 * per brick, ONE cascade behind the last of them, the orphaned meshes destroyed, one push onto
	 * the world — clears the selection and takes the panel down.
	 */
	int32 DeleteRow = INDEX_NONE;
	const TArrayView<const FPieceMenuRow> Rows = Controller->GetShownPieceMenuRows();
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].Action == Delete)
		{
			DeleteRow = Index;
		}
	}

	if (DeleteRow == INDEX_NONE)
	{
		Test->AddError(FString::Printf(TEXT("the piece menu offers no Delete row (%d rows shown)"), Rows.Num()));
		return true;
	}

	const int32 RowRefs = Rows[DeleteRow].Refs.Num();

	const double T0 = FPlatformTime::Seconds();
	const bool bChose = Controller->ChoosePieceMenuRow(DeleteRow);
	const double T1 = FPlatformTime::Seconds();

	Record.CommitMs = (T1 - T0) * 1000.0;
	Record.CutAtSeconds = World->GetTimeSeconds();
	Record.CutWallSeconds = T1;
	Record.LastFrameWallSeconds = T1;

	Test->TestTrue(TEXT("the Delete row must commit"), bChose);
	const int32 Ran = bChose ? RowRefs : 0;

	FStructureBinding* const Binding = ExperimentBinding(World, Record.StructureId);
	if (Binding == nullptr)
	{
		Test->AddError(TEXT("the binding vanished on delete"));
		return true;
	}

	const FStructure& S = Binding->GetStructure();
	Record.CutReport = S.GetLastSolveAndBreakReport();

	Test->TestEqual(TEXT("the delete must run against the whole course"), Ran, Record.CutRefs.Num());

	/* Who was released, and where every brick stands the instant after the push. */
	Record.PreviousCm.SetNumZeroed(Binding->NumPieces());
	int32 Stranded = 0, Falling = 0, Supported = 0, Grounded = 0, Unanswered = 0;

	FString PiecesCsv;
	ExperimentBuffer(PiecesCsv,
		TEXT("piece,material,grounded,massKg,support,released,removed,cx,cy,cz"));

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const bool bRemoved = Binding->IsPieceRemoved(Piece);
		const AActor* const Brick = Cast<AActor>(Binding->GetActor(Piece));
		const FVector At = Brick != nullptr ? Brick->GetActorLocation() : FVector::ZeroVector;
		Record.PreviousCm[Piece] = At;

		const TCHAR* SupportName = TEXT("removed");
		if (!bRemoved)
		{
			if (!S.HasSupportAnswer(Piece))
			{
				SupportName = TEXT("unanswered");
				++Unanswered;
			}
			else
			{
				const EPieceSupport Sup = S.GetPieceSupport(Piece);
				SupportName = ExperimentSupportName(Sup);
				switch (Sup)
				{
					case EPieceSupport::Grounded:  ++Grounded; break;
					case EPieceSupport::Supported: ++Supported; break;
					case EPieceSupport::Stranded:  ++Stranded; break;
					default:                       ++Falling; break;
				}
			}
			if (Binding->IsReleased(Piece))
			{
				Record.ReleasedPieces.Add(Piece);
			}
		}

		ExperimentBuffer(PiecesCsv, FString::Printf(
			TEXT("%d,%s,%d,%.4f,%s,%d,%d,%.3f,%.3f,%.3f"),
			Piece, ExperimentMaterialName(S, Piece), S.GetPiece(Piece).bIsGrounded ? 1 : 0, S.GetPiece(Piece).MassKg,
			SupportName, (!bRemoved && Binding->IsReleased(Piece)) ? 1 : 0, bRemoved ? 1 : 0, At.X, At.Y, At.Z));
	}

	/* Every joint: ends, normal, area, the pass that severed it (or none), and its utilisation now. */
	FString JointsCsv;
	ExperimentBuffer(JointsCsv,
		TEXT("joint,pieceA,pieceB,nx,ny,nz,areaSqCm,breakPass,utilisationAfterCut,aCz,bCz"));
	int32 SeveredByPass[64] = { 0 };
	int32 SeveredTotal = 0;

	for (int32 J = 0; J < S.NumConnections(); ++J)
	{
		const FConnection& C = S.GetConnection(J);
		const int32 Pass = S.GetBreakPass(J);
		if (Pass != INDEX_NONE)
		{
			++SeveredTotal;
			if (Pass >= 0 && Pass < 64)
			{
				++SeveredByPass[Pass];
			}
		}
		const double Util = C.HasGiven() ? -1.0 : S.GetConnectionUtilisation(J);
		ExperimentBuffer(JointsCsv, FString::Printf(
			TEXT("%d,%d,%d,%.4f,%.4f,%.4f,%.3f,%d,%.4f,%.3f,%.3f"),
			J, C.PieceA, C.PieceB, C.InterfaceNormal.X, C.InterfaceNormal.Y, C.InterfaceNormal.Z, C.InterfaceAreaSqCm,
			Pass, Util,
			Binding->GetBinding(C.PieceA).Box.CentreCm.Z, Binding->GetBinding(C.PieceB).Box.CentreCm.Z));
	}

	ExperimentWriteReport(TEXT("break_report.txt"),
		FString::Printf(TEXT("=== THE CUT: course %d of the back wall deleted through CommitPieceActionForAll at world time %.4f s; the whole commit (remove + cascade + destroy actors + push) took %.3f ms ==="),
			ExperimentCourse, Record.CutAtSeconds, Record.CommitMs), Record.CutReport);

	FString ByPass;
	for (int32 P = 1; P < 64; ++P)
	{
		if (SeveredByPass[P] > 0)
		{
			ByPass += FString::Printf(TEXT(" pass %d: %d;"), P, SeveredByPass[P]);
		}
	}

	ExperimentNote(*Test, FString::Printf(
		TEXT("CUT at world time %.4f s: commit %.3f ms of which SolveAndBreak %.3f ms in %d breaking pass(es) (%d run); ")
		TEXT("%d joints severed in total (%s); after: %d grounded, %d supported, %d stranded, %d falling, %d unanswered, %d removed; %d pieces released to physics"),
		Record.CutAtSeconds, Record.CommitMs, Record.CutReport.TotalMs, Record.CutReport.BreakingPasses,
		Record.CutReport.Passes.Num(), SeveredTotal, *ByPass, Grounded, Supported, Stranded, Falling, Unanswered,
		Binding->NumPieces() - S.NumLivePieces(), Record.ReleasedPieces.Num()));

	ExperimentBuffer(Record.FallSummaryCsv,
		TEXT("frame,secondsSinceCut,released,moving,maxDropCm,meanDropCm,maxDisplacementCm,belowGround,realMsSinceLastFrame"));
	ExperimentBuffer(Record.FallPiecesCsv, TEXT("frame,secondsSinceCut,piece,x,y,z,dropCm"));

	/*
	 * THE TWO BIG DUMPS LAND AFTER THE FALL, NOT NOW: writing them here is what stalled the release
	 * frame for 3.2 s in the first two runs. They are complete in memory already.
	 */
	Record.PiecesAfterCutCsv = MoveTemp(PiecesCsv);
	Record.JointsCsv = MoveTemp(JointsCsv);

	ExperimentShot(*Test, TEXT("Warehouse_C37_After_0s"));
	Record.ShotsTaken.Add(TEXT("Warehouse_C37_After_0s"));
	return true;
}

/* ---- track the fall frame by frame; photograph at set moments ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FExperimentTrackCommand, FAutomationTestBase*, Test, int32, TotalFrames);

bool FExperimentTrackCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	FExperimentRecord& Record = ExperimentRecord();
	if (!Record.bJoined)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	FStructureBinding* const Binding = ExperimentBinding(World, Record.StructureId);
	if (Binding == nullptr)
	{
		return true;
	}

	++Record.FramesTracked;
	const int32 Frame = Record.FramesTracked;
	const double Since = World->GetTimeSeconds() - Record.CutAtSeconds;

	/*
	 * REAL TIME PER FRAME, beside the pinned world time. The world clock says every frame is 1/60 s;
	 * the wall clock says what the machine actually paid for it — the first tracked frame carries the
	 * rest of the commit's frame (718 bodies handed to Chaos) and the physics step that followed.
	 */
	const double NowWall = FPlatformTime::Seconds();
	const double RealMs = (NowWall - Record.LastFrameWallSeconds) * 1000.0;
	Record.LastFrameWallSeconds = NowWall;

	if (Frame == 1)
	{
		ExperimentNote(*Test, FString::Printf(
			TEXT("REAL TIME: %.1f ms passed between the commit returning and the next latent tick — the frame that released %d bricks to physics, plus the first physics step"),
			RealMs, Record.ReleasedPieces.Num()));
	}

	int32 Moving = 0, BelowGround = 0;
	double MaxDrop = 0.0, SumDrop = 0.0, MaxDisp = 0.0;
	const bool bPerPiece = (Frame % 6) == 0;

	for (const int32 Piece : Record.ReleasedPieces)
	{
		const AActor* const Brick = Cast<AActor>(Binding->GetActor(Piece));
		if (Brick == nullptr)
		{
			continue;
		}
		const FVector At = Brick->GetActorLocation();
		const double Drop = Record.LaidAtCm[Piece].Z - At.Z;
		const double Disp = FVector::Dist(At, Record.LaidAtCm[Piece]);
		const double Step = FVector::Dist(At, Record.PreviousCm[Piece]);
		Record.PreviousCm[Piece] = At;

		if (Step > 0.05)
		{
			++Moving;
		}
		if (At.Z < -50.0)
		{
			++BelowGround;
		}
		MaxDrop = FMath::Max(MaxDrop, Drop);
		SumDrop += Drop;
		MaxDisp = FMath::Max(MaxDisp, Disp);

		if (bPerPiece)
		{
			ExperimentBuffer(Record.FallPiecesCsv, FString::Printf(
				TEXT("%d,%.4f,%d,%.3f,%.3f,%.3f,%.3f"), Frame, Since, Piece, At.X, At.Y, At.Z, Drop));
		}
	}

	const int32 Released = Record.ReleasedPieces.Num();
	ExperimentBuffer(Record.FallSummaryCsv, FString::Printf(
		TEXT("%d,%.4f,%d,%d,%.3f,%.3f,%.3f,%d,%.2f"),
		Frame, Since, Released, Moving, MaxDrop, Released > 0 ? SumDrop / Released : 0.0, MaxDisp, BelowGround, RealMs));

	struct FMoment { int32 Frame; const TCHAR* Name; };
	static const FMoment Moments[] = {
		/* No dot in a shot name: the screenshot writer treats ".25s" as an extension and drops it. */
		{ 15, TEXT("Warehouse_C37_After_0p25s") }, { 30, TEXT("Warehouse_C37_After_0p5s") },
		{ 60, TEXT("Warehouse_C37_After_1s") }, { 120, TEXT("Warehouse_C37_After_2s") },
		{ 180, TEXT("Warehouse_C37_After_3s") }, { 300, TEXT("Warehouse_C37_After_5s") },
		{ 480, TEXT("Warehouse_C37_After_8s") },
	};

	/*
	 * ONE SHOT IN FLIGHT AT A TIME. The engine holds a single pending screenshot request and a heavy
	 * frame (718 bodies waking in Chaos) can take several frames to drain it; a second request made
	 * while the first is still pending REPLACES it and the earlier file is never written — which is
	 * how the first run of this lost its 0.25 s and 0.5 s frames. So a due moment waits until nothing
	 * is pending, and the log records the frame it was actually taken on.
	 */
	for (const FMoment& M : Moments)
	{
		if (M.Frame == Frame)
		{
			ExperimentNote(*Test, FString::Printf(
				TEXT("FALL frame %d (%.3f s after the cut): %d released, %d moving this frame, max drop %.1f cm, mean drop %.1f cm, max displacement %.1f cm; this frame took %.1f ms of real time"),
				Frame, Since, Released, Moving, MaxDrop, Released > 0 ? SumDrop / Released : 0.0, MaxDisp, RealMs));
			Record.ShotsDue.Add(M.Name);
		}
	}

	if (Record.ShotsDue.Num() > 0 && !FScreenshotRequest::IsScreenshotRequested())
	{
		UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;
		if (Viewport != nullptr)
		{
			const FString Name = Record.ShotsDue[0];
			Record.ShotsDue.RemoveAt(0);
			Viewport->Exec(nullptr, *FString::Printf(TEXT("Shot filename=%s -nosuffix"), *Name), *GLog);
			Record.ShotsTaken.Add(Name);
			ExperimentNote(*Test, FString::Printf(TEXT("SHOT %s requested on frame %d (%.3f s after the cut)"), *Name, Frame, Since));
		}
	}

	return Frame >= TotalFrames && Record.ShotsDue.Num() == 0;
}

/**
 * THE SAME CUT HEADLESS, REPEATED: the layout file loaded world-free, the same 31 pieces removed, one
 * SolveAndBreak — five times on five fresh loads, so the timing has a spread rather than a single
 * sample taken while a renderer was drawing 5,612 bricks beside it. Also the intact as-laid solve,
 * five times, for the baseline. Runs under -nullrhi; asserts only that the verdict is the same every
 * time. Invoke with `Automation RunTests Experiment.WarehouseCourse37.HeadlessTiming`.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWarehouseCourse37HeadlessTimingTest,
	"Experiment.WarehouseCourse37.HeadlessTiming",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWarehouseCourse37HeadlessTimingTest::RunTest(const FString& Parameters)
{
	using namespace WarehouseCourse37Experiment;

	IFileManager::Get().MakeDirectory(*ExperimentFolder(), /*Tree*/ true);
	IFileManager::Get().Delete(*(ExperimentFolder() / TEXT("headless_timing.txt")), false, true, true);

	ExperimentAppend(TEXT("headless_timing.txt"), FString::Printf(
		TEXT("=== HEADLESS TIMING, %s: Warehouse.json loaded world-free, course %d of the back wall removed, one SolveAndBreak per fresh load ==="),
		*FDateTime::Now().ToString(), ExperimentCourse));

	constexpr int32 Repeats = 5;

	for (int32 Repeat = 0; Repeat < Repeats; ++Repeat)
	{
		FBrickLayout Intact;
		FString Why;
		const double LoadStart = FPlatformTime::Seconds();
		const bool bLoaded = DestructionLayoutFile::LoadFile(DestructionLayoutFile::ContentPath(TEXT("Warehouse")), Intact, &Why);
		const double LoadMs = (FPlatformTime::Seconds() - LoadStart) * 1000.0;

		if (!TestTrue(*FString::Printf(TEXT("the warehouse must load: %s"), *Why), bLoaded))
		{
			return false;
		}

		/* BASELINE: as laid. */
		const int32 PassesAsLaid = Intact.Structure.SolveAndBreak();
		const FStructure::FSolveAndBreakReport AsLaid = Intact.Structure.GetLastSolveAndBreakReport();

		/* THE CUT, on a fresh load so no stamp or cache carries over. */
		FBrickLayout Cut;
		DestructionLayoutFile::LoadFile(DestructionLayoutFile::ContentPath(TEXT("Warehouse")), Cut, &Why);

		int32 Removed = 0;
		for (int32 Piece = 0; Piece < Cut.Boxes.Num(); ++Piece)
		{
			if (ExperimentIsCutPiece(Cut.Boxes[Piece]) && Cut.Structure.RemovePiece(Piece))
			{
				++Removed;
			}
		}

		const int32 PassesCut = Cut.Structure.SolveAndBreak();
		const FStructure::FSolveAndBreakReport AfterCut = Cut.Structure.GetLastSolveAndBreakReport();

		int32 NotHeld = 0;
		for (int32 Piece = 0; Piece < Cut.Structure.NumPieces(); ++Piece)
		{
			if (Cut.Structure.IsPieceRemoved(Piece) || !Cut.Structure.HasSupportAnswer(Piece))
			{
				continue;
			}
			const EPieceSupport S = Cut.Structure.GetPieceSupport(Piece);
			if (S != EPieceSupport::Grounded && S != EPieceSupport::Supported)
			{
				++NotHeld;
			}
		}

		TestEqual(TEXT("the cut removes the course"), Removed, 31);
		TestEqual(TEXT("as laid it breaks nothing"), PassesAsLaid, 0);

		ExperimentAppend(TEXT("headless_timing.txt"), FString::Printf(
			TEXT("repeat %d: load+sweep %.1f ms | AS LAID: %.3f ms, %d fixpoint iterations, prover %.3f ms | CUT: %.3f ms total, %d breaking pass(es) of %d, solve %.3f ms (%d fixpoint iterations), gate %.3f ms, sweep %.3f ms, prover %.3f ms (%d poses, %d pivots, %d blocks, fell %d), %d not held"),
			Repeat, LoadMs, AsLaid.TotalMs,
			AsLaid.Passes.Num() > 0 ? AsLaid.Passes[0].Solve.FixpointIterations : 0,
			AsLaid.Passes.Num() > 0 ? AsLaid.Passes[0].RegionalProverMs : 0.0,
			AfterCut.TotalMs, PassesCut, AfterCut.Passes.Num(),
			AfterCut.Passes.Num() > 0 ? AfterCut.Passes[0].Solve.TotalMs : 0.0,
			AfterCut.Passes.Num() > 0 ? AfterCut.Passes[0].Solve.FixpointIterations : 0,
			AfterCut.Passes.Num() > 0 ? AfterCut.Passes[0].GateMs : 0.0,
			AfterCut.Passes.Num() > 0 ? AfterCut.Passes[0].CapacitySweepMs : 0.0,
			AfterCut.Passes.Num() > 0 ? AfterCut.Passes[0].RegionalProverMs : 0.0,
			AfterCut.Passes.Num() > 0 ? AfterCut.Passes[0].RegionalPoses : 0,
			AfterCut.Passes.Num() > 0 ? AfterCut.Passes[0].RegionalLpPivots : 0,
			AfterCut.Passes.Num() > 0 ? AfterCut.Passes[0].RegionalLastBlocks : INDEX_NONE,
			AfterCut.Passes.Num() > 0 && AfterCut.Passes[0].bRegionalFell ? 1 : 0,
			NotHeld));

		if (Repeat == 0)
		{
			ExperimentWriteReport(TEXT("headless_timing.txt"), TEXT("--- repeat 0, as laid, pass table ---"), AsLaid);
			ExperimentWriteReport(TEXT("headless_timing.txt"), TEXT("--- repeat 0, after the cut, pass table ---"), AfterCut);
		}
	}

	return true;
}

/* ---- where everything came to rest ---- */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FExperimentFinalCommand, FAutomationTestBase*, Test);

bool FExperimentFinalCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	FExperimentRecord& Record = ExperimentRecord();
	if (!Record.bJoined)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	FStructureBinding* const Binding = ExperimentBinding(World, Record.StructureId);
	if (Binding == nullptr)
	{
		return true;
	}

	FString FinalCsv;
	ExperimentBuffer(FinalCsv, TEXT("piece,released,removed,laidX,laidY,laidZ,finalX,finalY,finalZ,displacementCm,dropCm"));

	int32 MovedUnreleased = 0, Standing = 0, Fallen = 0;
	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const bool bRemoved = Binding->IsPieceRemoved(Piece);
		const AActor* const Brick = Cast<AActor>(Binding->GetActor(Piece));
		if (Brick == nullptr)
		{
			ExperimentBuffer(FinalCsv, FString::Printf(TEXT("%d,0,%d,,,,,,,,"), Piece, bRemoved ? 1 : 0));
			continue;
		}
		const FVector Laid = Record.LaidAtCm[Piece];
		const FVector At = Brick->GetActorLocation();
		const double Disp = FVector::Dist(At, Laid);
		const bool bReleased = Binding->IsReleased(Piece);

		if (!bReleased && Disp > 0.1)
		{
			++MovedUnreleased;
		}
		if (bReleased)
		{
			if (Disp > 5.0) ++Fallen; else ++Standing;
		}

		ExperimentBuffer(FinalCsv, FString::Printf(
			TEXT("%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f"),
			Piece, bReleased ? 1 : 0, bRemoved ? 1 : 0, Laid.X, Laid.Y, Laid.Z, At.X, At.Y, At.Z, Disp, Laid.Z - At.Z));
	}

	ExperimentFlush(TEXT("pieces_final.csv"), FinalCsv);
	ExperimentFlush(TEXT("pieces_after_cut.csv"), Record.PiecesAfterCutCsv);
	ExperimentFlush(TEXT("joints.csv"), Record.JointsCsv);
	ExperimentFlush(TEXT("fall_summary.csv"), Record.FallSummaryCsv);
	ExperimentFlush(TEXT("fall_pieces.csv"), Record.FallPiecesCsv);

	ExperimentNote(*Test, FString::Printf(
		TEXT("FINAL at world time %.4f s (%.3f s after the cut): of %d released pieces %d moved more than 5 cm and %d did not; %d unreleased bricks moved (must be 0)"),
		World->GetTimeSeconds(), World->GetTimeSeconds() - Record.CutAtSeconds,
		Record.ReleasedPieces.Num(), Fallen, Standing, MovedUnreleased));

	Test->TestEqual(TEXT("a brick the solver still holds must not have moved"), MovedUnreleased, 0);

	ExperimentShot(*Test, TEXT("Warehouse_C37_After_Final"));
	Record.ShotsTaken.Add(TEXT("Warehouse_C37_After_Final"));
	return true;
}

/** Collect whatever frames the run took, by the record rather than by a fixed list. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FExperimentCollectTakenCommand, FAutomationTestBase*, Test);

bool FExperimentCollectTakenCommand::Update()
{
	using namespace WarehouseCourse37Experiment;

	for (const FString& BaseName : ExperimentRecord().ShotsTaken)
	{
		const FString From = ExperimentShotPath(BaseName);
		const FString To = ExperimentFolder() / BaseName + TEXT(".png");
		const int64 Size = IFileManager::Get().FileSize(*From);
		if (Size > 0)
		{
			IFileManager::Get().Copy(*To, *From, /*Replace*/ true);
		}
		ExperimentNote(*Test, FString::Printf(TEXT("FRAME %s (%lld bytes)"), *BaseName, Size));
		Test->TestTrue(*FString::Printf(TEXT("%s must have been written"), *BaseName), Size > 32 * 1024);
	}
	return true;
}

/**
 * THE WHOLE EXPERIMENT: join, select, photograph, let the level's own hold run (the baseline settle),
 * delete the course through the player's door with a clock on it, photograph the instant after and
 * at seven moments through the fall, track every released brick, and dump the graph, the report and
 * the resting positions into the experiment folder.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWarehouseCourse37CollapseTest,
	"Experiment.WarehouseCourse37.Collapse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::NonNullRHI
		| EAutomationTestFlags::ProductFilter)

bool FWarehouseCourse37CollapseTest::RunTest(const FString& Parameters)
{
	using namespace WarehouseCourse37Experiment;

	IFileManager::Get().MakeDirectory(*ExperimentFolder(), /*Tree*/ true);

	for (const TCHAR* const Name : { TEXT("run.log"), TEXT("selection.csv"), TEXT("break_report.txt"),
		TEXT("pieces_after_cut.csv"), TEXT("joints.csv"), TEXT("fall_summary.csv"), TEXT("fall_pieces.csv"),
		TEXT("pieces_final.csv") })
	{
		IFileManager::Get().Delete(*(ExperimentFolder() / Name), false, true, true);
	}

	/* Every frame of an earlier run goes first, so a file's existence afterwards means this run drew it. */
	TArray<FString> OldFrames;
	IFileManager::Get().FindFiles(OldFrames, *(FPaths::ScreenShotDir() / TEXT("Warehouse_C37_*.png")), true, false);
	for (const FString& Old : OldFrames)
	{
		IFileManager::Get().Delete(*(FPaths::ScreenShotDir() / Old), false, true, true);
	}

	ExperimentAppend(TEXT("run.log"), FString::Printf(TEXT("=== COLLAPSE run, %s ==="), *FDateTime::Now().ToString()));

	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(TEXT("DisableAllScreenMessages")));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentOpenSessionCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentOpenMapCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentJoinCommand(this, 0));

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentSlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentFrozenFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentThawCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentExposureFrames));

	/* Selected and photographed during the hold, as the player finds it. */
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentSelectCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentWriteFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentShotNoUICommand(this, TEXT("Warehouse_C37_Before_NoUI")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentWriteFrames));

	/* Past the 4 s hold: the level runs its own settle on the intact building. That is the baseline. */
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(200));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentBaselineCommand(this));

	ADD_LATENT_AUTOMATION_COMMAND(FExperimentDeleteCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentTrackCommand(this, 480));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentWriteFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentFinalCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentWriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FExperimentCloseSessionCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentCollectCommand(this,
		TArray<FString>({ TEXT("Warehouse_C37_Before"), TEXT("Warehouse_C37_Before_NoUI") })));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentCollectTakenCommand(this));

	return true;
}

/**
 * THE BEFORE HALF: join, select the course by clicking, photograph it from the level's own
 * viewpoint and from close in on the back wall. Cuts nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWarehouseCourse37BeforeTest,
	"Experiment.WarehouseCourse37.Before",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::NonNullRHI
		| EAutomationTestFlags::ProductFilter)

bool FWarehouseCourse37BeforeTest::RunTest(const FString& Parameters)
{
	using namespace WarehouseCourse37Experiment;

	IFileManager::Get().MakeDirectory(*ExperimentFolder(), /*Tree*/ true);

	const TArray<FString> Frames = {
		TEXT("Warehouse_C37_Before"), TEXT("Warehouse_C37_Before_Close"), TEXT("Warehouse_C37_Before_Close_NoUI") };

	for (const FString& BaseName : Frames)
	{
		IFileManager::Get().Delete(*ExperimentShotPath(BaseName), false, true, true);
	}

	ExperimentAppend(TEXT("run.log"), FString::Printf(TEXT("=== BEFORE run, %s ==="), *FDateTime::Now().ToString()));

	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(TEXT("DisableAllScreenMessages")));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentOpenSessionCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentOpenMapCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentJoinCommand(this, 0));

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentSlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentFrozenFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentThawCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentExposureFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FExperimentSelectCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentWriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FExperimentReframeCommand(this, 1));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentReframeFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentShotCommand(this, TEXT("Warehouse_C37_Before_Close")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentWriteFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentShotNoUICommand(this, TEXT("Warehouse_C37_Before_Close_NoUI")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExperimentWriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FExperimentCloseSessionCommand(this));
	ADD_LATENT_AUTOMATION_COMMAND(FExperimentCollectCommand(this, Frames));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
