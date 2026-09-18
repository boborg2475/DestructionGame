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
 * A REUSABLE CUT HARNESS FOR THE WAREHOUSE — one row per experiment, run by name, self-documenting.
 *
 * The owner asked (2026-09-18) to keep running experiments on `Lvl_Warehouse` cheaply. So the thing
 * that varies between experiments — WHICH pieces to pull — is a `FCutSpec` row in the table below,
 * and everything else (join the level as a player, select the named pieces by real clicks, hold,
 * delete through the player's own menu, time the break decision, track the fall, dump the data and
 * write a plain-English SUMMARY.txt) is shared. Adding an experiment is adding a row; running it is
 * `Automation RunTests Experiment.WarehouseCut.<RowName>`. Each run writes into its own folder,
 * `Experiments/WarehouseCut/<RowName>/`.
 *
 * TWO COMPLEX TESTS ENUMERATE THE SAME TABLE. `Experiment.WarehouseCut.<row>` is the LIVE capture
 * (needs a real RHI, no `-nullrhi`): it photographs the selection and the fall. `Experiment.
 * WarehouseCutTiming.<row>` is the HEADLESS timing spread (five fresh loads of the same cut through
 * the world-free layout + solver), for a decision-time number with a spread rather than one sample.
 *
 * THE FIRST EXPERIMENT (`BackWallCourse37`) reproduces the committed 2026-09-18 run whose write-up is
 * `Experiments/WarehouseCourse37/REPORT.md`; it is kept as the reference the harness must still match.
 */
namespace WarehouseCutExperiment
{
	using namespace DestructionLayout;

	constexpr double CoursePitchCm = 7.5;
	constexpr double CourseHeightCm = 6.5;
	constexpr double GeometryToleranceCm = 0.01;

	/*
	 * THE TWO LONG WALLS, BY THEIR Y BANDS (WAREHOUSE_DESIGN.md). The FRONT wall's wythe is Y [0,
	 * 10.25] and its pilasters project to Y -11.25; the BACK wall's wythe is Y [326.25, 336.5] and
	 * its pilasters project to Y 347.75. A piece belongs to a face by which side of the building's
	 * mid-Y (168.25) it sits on, which separates the two long walls cleanly and never claims an end
	 * wall (those run along Y through the middle and are excluded by the course + face test together).
	 */
	constexpr double FrontWallWytheMaxYCm = 10.25;
	constexpr double BackWallWytheMinYCm = 326.25;
	constexpr double BuildingMidYCm = 168.25;

	/** The six pilaster slots of a long wall, as X ranges (grid units 3,13,..,53 of pitch 11.25). */
	inline bool BehindAPilaster(const FPieceBox& Box)
	{
		const double XLo = Box.CentreCm.X - Box.ExtentCm.X;
		const double XHi = Box.CentreCm.X + Box.ExtentCm.X;
		for (int32 Slot = 0; Slot < 6; ++Slot)
		{
			const double PilasterLo = (3 + 10 * Slot) * 11.25;
			const double PilasterHi = PilasterLo + 21.5;
			if (XHi > PilasterLo + GeometryToleranceCm && XLo < PilasterHi - GeometryToleranceCm)
			{
				return true;
			}
		}
		return false;
	}

	enum class EFace : uint8 { Front, Back };

	/** One experiment: a name, the course to pull, which long walls, and which face to frame close. */
	struct FCutSpec
	{
		const TCHAR* Name = nullptr;
		const TCHAR* What = nullptr;
		int32 Course = 0;
		bool bFront = false;
		bool bBack = false;
		EFace CloseFace = EFace::Back;
	};

	/**
	 * THE TABLE. Add a row to add an experiment. `BackWallCourse37` is the committed reference; the
	 * others extend the same course across the front, and across both long walls at once.
	 */
	inline const TArray<FCutSpec>& Specs()
	{
		static const TArray<FCutSpec> Table = {
			{ TEXT("BackWallCourse37"),
			  TEXT("course 37 (the upper sill course) across the BACK long wall"),
			  37, /*front*/ false, /*back*/ true, EFace::Back },
			{ TEXT("FrontWallCourse37"),
			  TEXT("course 37 (the upper sill course) across the FRONT long wall"),
			  37, /*front*/ true, /*back*/ false, EFace::Front },
			{ TEXT("BothLongWallsCourse37"),
			  TEXT("course 37 (the upper sill course) across BOTH long walls — the back cut extended across the front face"),
			  37, /*front*/ true, /*back*/ true, EFace::Front },
		};
		return Table;
	}

	inline int32 IndexOfSpec(const FString& Name)
	{
		const TArray<FCutSpec>& Table = Specs();
		for (int32 Index = 0; Index < Table.Num(); ++Index)
		{
			if (Name.Equals(Table[Index].Name, ESearchCase::CaseSensitive))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** The face a piece belongs to, or false if it is on neither long wall (an end wall / roof). */
	inline bool FaceOf(const FPieceBox& Box, EFace& OutFace)
	{
		const double YLo = Box.CentreCm.Y - Box.ExtentCm.Y;
		const double YHi = Box.CentreCm.Y + Box.ExtentCm.Y;
		if (YHi <= FrontWallWytheMaxYCm + GeometryToleranceCm && Box.CentreCm.Y < BuildingMidYCm)
		{
			OutFace = EFace::Front;
			return true;
		}
		if (YLo >= BackWallWytheMinYCm - GeometryToleranceCm && Box.CentreCm.Y > BuildingMidYCm)
		{
			OutFace = EFace::Back;
			return true;
		}
		return false;
	}

	/** Is this piece part of the spec's cut: the named course, on a selected face? */
	inline bool IsCutPiece(const FPieceBox& Box, const FCutSpec& Spec)
	{
		const double ZLo = Box.CentreCm.Z - Box.ExtentCm.Z;
		const double ZHi = Box.CentreCm.Z + Box.ExtentCm.Z;
		const double CourseLo = Spec.Course * CoursePitchCm;
		const double CourseHi = CourseLo + CourseHeightCm;

		if (!FMath::IsNearlyEqual(ZLo, CourseLo, GeometryToleranceCm)
			|| !FMath::IsNearlyEqual(ZHi, CourseHi, GeometryToleranceCm))
		{
			return false;
		}

		EFace Face;
		if (!FaceOf(Box, Face))
		{
			return false;
		}
		return (Face == EFace::Front && Spec.bFront) || (Face == EFace::Back && Spec.bBack);
	}

	/**
	 * The ray a player would click this piece along. Everything on a wall's outer face is reached
	 * from OUTSIDE the building along that face's outward normal (-Y for the front wall, +Y for the
	 * back); a wythe brick hidden directly behind a pilaster is reached from INSIDE, where the shell
	 * is hollow and nothing stands between the interior and the wall's inner face.
	 */
	inline void RayFor(const FPieceBox& Box, FVector& OutStart, FVector& OutEnd)
	{
		EFace Face = EFace::Back;
		FaceOf(Box, Face);

		const double YLo = Box.CentreCm.Y - Box.ExtentCm.Y;
		const double YHi = Box.CentreCm.Y + Box.ExtentCm.Y;
		const bool bPilaster = (Face == EFace::Back) ? YLo > FrontWallWytheMaxYCm + 320.0 : YHi < 0.0;
		const bool bFromInside = !bPilaster && BehindAPilaster(Box);

		double StartY;
		if (Face == EFace::Back)
		{
			StartY = bFromInside ? 280.0 : 420.0;
		}
		else
		{
			StartY = bFromInside ? 80.0 : -80.0;
		}

		OutEnd = Box.CentreCm;
		OutStart = FVector(Box.CentreCm.X, StartY, Box.CentreCm.Z);
	}

	inline const TCHAR* MaterialName(const FStructure& S, int32 Piece)
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

	inline const TCHAR* SupportName(EPieceSupport S)
	{
		switch (S)
		{
			case EPieceSupport::Grounded:  return TEXT("Grounded");
			case EPieceSupport::Supported: return TEXT("Supported");
			case EPieceSupport::Stranded:  return TEXT("Stranded");
			default:                       return TEXT("Falling");
		}
	}

	inline int32 JointCount(const FStructure& S, int32 Piece)
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

	/* Frames, at the pinned 1/60 s — the same budget Tests/ScenarioLevelScreenshotTest.cpp uses. */
	constexpr double FixedDeltaSeconds = 1.0 / 60.0;
	constexpr int32 FrozenFrames = 30;
	constexpr int32 SlateFrames = 3;
	constexpr int32 ExposureFrames = 60;
	constexpr int32 WriteFrames = 5;
	constexpr int32 ReframeFrames = 40;
	constexpr int32 BaselineHoldFrames = 200;
	constexpr int32 FallFrames = 480;
	constexpr int32 MapLoadFrameBudget = 3000;
	constexpr double AspectHeightOverWidth = 1080.0 / 1920.0;

	inline FString SpecFolder(const FCutSpec& Spec)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Experiments/WarehouseCut") / Spec.Name);
	}

	inline FString ShotPath(const FString& BaseName)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir() / BaseName + TEXT(".png"));
	}

	/** The per-run record, keyed to one spec. Static, because latent commands run frames apart. */
	struct FRecord
	{
		int32 SpecIndex = INDEX_NONE;
		bool bJoined = false;
		TWeakObjectPtr<UWorld> WorldBeforeTravel;
		int32 RowIndex = INDEX_NONE;
		int32 StructureId = INDEX_NONE;
		TArray<FVector> LaidAtCm;
		double BuiltAtSeconds = 0.0;

		TArray<int32> CutPieces;
		TArray<FPieceRef> CutRefs;

		DestructionScenarios::FViewpoint CloseView;
		DestructionScenarios::FViewpoint WideView;

		double CutAtSeconds = 0.0;
		double CommitMs = 0.0;
		double LastFrameWallSeconds = 0.0;
		FStructure::FSolveAndBreakReport CutReport;
		TArray<int32> ReleasedPieces;
		TArray<FVector> PreviousCm;
		int32 FramesTracked = 0;
		TArray<FString> ShotsTaken;
		TArray<FString> ShotsDue;

		/* Summary tallies, filled at the cut. */
		int32 RelClay = 0, RelStone = 0, RelTimber = 0;
		int32 RelUpper = 0, RelRoof = 0;
		int32 GroundedAfter = 0, SupportedAfter = 0, RemovedCount = 0;
		double MaxDropFinalCm = 0.0;

		FString FallSummaryCsv;
		FString FallPiecesCsv;
		FString PiecesAfterCutCsv;
		FString JointsCsv;

		void Reset(int32 InSpecIndex)
		{
			*this = FRecord();
			SpecIndex = InSpecIndex;
		}
	};

	inline FRecord& Record()
	{
		static FRecord R;
		return R;
	}

	inline const FCutSpec& CurrentSpec()
	{
		return Specs()[Record().SpecIndex];
	}

	inline FString SpecFolder()
	{
		return SpecFolder(CurrentSpec());
	}

	inline void Append(const FString& FileName, const FString& Line)
	{
		FFileHelper::SaveStringToFile(
			Line + LINE_TERMINATOR, *(SpecFolder() / FileName),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
	}

	inline void Note(FAutomationTestBase& Test, const FString& Line)
	{
		Test.AddInfo(Line);
		Append(TEXT("run.log"), Line);
	}

	/** Buffered, for the big dumps: appending per line reopens the file and stalled the release frame. */
	inline void Buffer(FString& Buf, const FString& Line) { Buf += Line; Buf += LINE_TERMINATOR; }

	inline void Flush(const FString& FileName, const FString& Buf)
	{
		FFileHelper::SaveStringToFile(
			Buf, *(SpecFolder() / FileName),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get());
	}

	inline void WriteReport(const FString& FileName, const FString& Heading, const FStructure::FSolveAndBreakReport& R)
	{
		Append(FileName, Heading);
		Append(FileName, FString::Printf(
			TEXT("total %.3f ms; %d breaking pass(es), %d passes run; live pieces before %d; intact joints %d -> %d"),
			R.TotalMs, R.BreakingPasses, R.Passes.Num(), R.LivePiecesBefore, R.IntactJointsBefore, R.IntactJointsAfter));
		Append(FileName,
			TEXT("pass,passMs,solveMs,supportListsMs,reseatMs,fixpointMs,fixpointIterations,archingMs,gateMs,gateDisposition,jointsSeveredByGate,")
			TEXT("sweepMs,jointsGivenToSweep,proverMs,proverPoses,proverLpPivots,proverLpMs,proverLastBlocks,proverFell,")
			TEXT("jointsSeveredByProver,piecesFelledByProver,livePiecesAfter,intactJointsAfter,notHeldAfter"));
		for (const FStructure::FBreakPassReport& P : R.Passes)
		{
			Append(FileName, FString::Printf(
				TEXT("%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%d,%d,%.3f,%d,%.3f,%d,%d,%.3f,%d,%d,%d,%d,%d,%d,%d"),
				P.Pass, P.PassMs, P.Solve.TotalMs, P.Solve.SupportListsMs, P.Solve.ReseatMs, P.Solve.FixpointMs,
				P.Solve.FixpointIterations, P.Solve.ArchingMs, P.GateMs, P.GateDisposition, P.JointsSeveredByGate, P.CapacitySweepMs,
				P.JointsGivenToSweep, P.RegionalProverMs, P.RegionalPoses, P.RegionalLpPivots, P.RegionalLpMs,
				P.RegionalLastBlocks, P.bRegionalFell ? 1 : 0, P.JointsSeveredByProver, P.PiecesFelledByProver,
				P.LivePieces, P.IntactJointsAfter, P.NotHeldAfter));

			for (int32 It = 0; It < P.Solve.FixpointIterations; ++It)
			{
				Append(FileName, FString::Printf(
					TEXT("  pass %d fixpoint iteration %d: %d reached from the ground, %d overturned, %d stranded, %d released from a refused arch"),
					P.Pass, It + 1,
					P.Solve.SupportedPerIteration.IsValidIndex(It) ? P.Solve.SupportedPerIteration[It] : -1,
					P.Solve.OverturnedPerIteration.IsValidIndex(It) ? P.Solve.OverturnedPerIteration[It] : -1,
					P.Solve.StrandedPerIteration.IsValidIndex(It) ? P.Solve.StrandedPerIteration[It] : -1,
					P.Solve.ReleasedPerIteration.IsValidIndex(It) ? P.Solve.ReleasedPerIteration[It] : -1));
			}
		}
	}

	inline void Shot(FAutomationTestBase& Test, const FString& BaseName, bool bWithUI = true)
	{
		UGameViewportClient* const Viewport = GEngine != nullptr ? GEngine->GameViewport : nullptr;
		if (Viewport == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("no game viewport to photograph %s"), *BaseName));
			return;
		}
		Viewport->Exec(nullptr,
			*FString::Printf(TEXT("Shot %sfilename=%s -nosuffix"), bWithUI ? TEXT("showui ") : TEXT(""), *BaseName), *GLog);
	}

	inline float SetDilation(UWorld* World, float Dilation)
	{
		AWorldSettings* const Settings = World != nullptr ? World->GetWorldSettings() : nullptr;
		return Settings != nullptr ? Settings->SetTimeDilation(Dilation) : -1.0f;
	}

	inline void FrameTo(UWorld* World, const DestructionScenarios::FViewpoint& View)
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

	/**
	 * A three-quarter viewpoint that FACES a chosen long wall. The level's own ThreeQuarter frames
	 * from the +X/+Y/+Z octant, which looks at the back wall; a front-face cut needs the mirror, so
	 * the Y component of the orbit direction is signed by the face. The standoff is taken from the
	 * production `ViewpointFor` on the same box, so the framing distance is the tested one.
	 */
	inline DestructionScenarios::FViewpoint FaceView(const FBox& BoxCm, EFace Face)
	{
		const DestructionScenarios::FViewpoint Base = DestructionScenarios::ViewpointFor(
			BoxCm, AspectHeightOverWidth, DestructionScenarios::EScenarioFraming::ThreeQuarter);

		const FVector CentreCm = BoxCm.GetCenter();
		const double StandoffCm = FVector::Dist(Base.LocationCm, CentreCm);

		const double AzimuthRad = FMath::DegreesToRadians(40.0);
		const double ElevationRad = FMath::DegreesToRadians(30.0);
		const double FaceSign = Face == EFace::Back ? 1.0 : -1.0;

		const FVector Dir(
			FMath::Cos(ElevationRad) * FMath::Sin(AzimuthRad),
			FaceSign * FMath::Cos(ElevationRad) * FMath::Cos(AzimuthRad),
			FMath::Sin(ElevationRad));

		DestructionScenarios::FViewpoint View;
		View.LocationCm = CentreCm + StandoffCm * Dir;
		View.Rotation = (CentreCm - View.LocationCm).Rotation();
		return View;
	}

	inline ADestructionGamePlayerController* Controller(UWorld* World)
	{
		return World != nullptr ? Cast<ADestructionGamePlayerController>(World->GetFirstPlayerController()) : nullptr;
	}

	inline FStructureBinding* Binding(UWorld* World, int32 StructureId)
	{
		UDestructionStructureSubsystem* const Subsystem =
			World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;
		return Subsystem != nullptr ? Subsystem->Find(StructureId) : nullptr;
	}

	inline const FPieceAction* DeleteAction()
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
}

/* ================================ the latent commands ================================ */

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FWCutOpenSession, FAutomationTestBase*, Test);
bool FWCutOpenSession::Update()
{
	using namespace WarehouseCutExperiment;
	Test->TestTrue(TEXT("a game viewport is needed: -nullrhi must be absent"),
		GEngine != nullptr && GEngine->GameViewport != nullptr);
	FApp::SetFixedDeltaTime(FixedDeltaSeconds);
	FApp::SetUseFixedTimeStep(true);
	Note(*Test, FString::Printf(TEXT("clock pinned at %.6f s per frame"), FApp::GetFixedDeltaTime()));
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FWCutCloseSession, FAutomationTestBase*, Test);
bool FWCutCloseSession::Update() { FApp::SetUseFixedTimeStep(false); return true; }

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FWCutOpenMap, FAutomationTestBase*, Test);
bool FWCutOpenMap::Update()
{
	using namespace WarehouseCutExperiment;
	FRecord& R = Record();

	R.RowIndex = DestructionScenarios::IndexOfName(FName(TEXT("warehouse")));
	if (R.RowIndex == INDEX_NONE)
	{
		Test->AddError(TEXT("the catalogue has no 'warehouse' row"));
		return true;
	}
	const FString Package = FString(TEXT("/Game/Maps/Scenarios/")) + DestructionScenarios::Catalogue()[R.RowIndex].MapName;
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
	R.WorldBeforeTravel = World;
	Note(*Test, FString::Printf(TEXT("=== %s: %s ==="), CurrentSpec().Name, CurrentSpec().What));
	Note(*Test, FString::Printf(TEXT("opening %s"), *Package));
	GEngine->Exec(World, *FString::Printf(TEXT("Open %s"), *Package));
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FWCutJoin, FAutomationTestBase*, Test, int32, FramesWaited);
bool FWCutJoin::Update()
{
	using namespace WarehouseCutExperiment;
	FRecord& R = Record();
	if (R.RowIndex == INDEX_NONE)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	ADestructionGameGameMode* const GameMode = World != nullptr ? World->GetAuthGameMode<ADestructionGameGameMode>() : nullptr;

	const bool bIsThisRow = World != nullptr && World != R.WorldBeforeTravel.Get() && GameMode != nullptr
		&& GameMode->GetSelectedScenarioRow() == R.RowIndex && GameMode->GetBuiltStructureId() != INDEX_NONE;

	if (!bIsThisRow)
	{
		if (++FramesWaited < MapLoadFrameBudget)
		{
			return false;
		}
		Test->AddError(FString::Printf(TEXT("Lvl_Warehouse never came up after %d frames"), FramesWaited));
		return true;
	}

	SetDilation(World, 0.0f);
	R.BuiltAtSeconds = World->GetTimeSeconds();
	R.StructureId = GameMode->GetBuiltStructureId();

	FStructureBinding* const B = Binding(World, R.StructureId);
	if (B == nullptr)
	{
		Test->AddError(TEXT("the game mode's structure id names no binding"));
		return true;
	}
	const FStructure& S = B->GetStructure();
	const FCutSpec& Spec = CurrentSpec();

	R.LaidAtCm.SetNumZeroed(B->NumPieces());
	FBox BoundsCm(ForceInit);
	FBox CutBoxCm(ForceInit);

	for (int32 Piece = 0; Piece < B->NumPieces(); ++Piece)
	{
		if (const AActor* const Brick = Cast<AActor>(B->GetActor(Piece)))
		{
			R.LaidAtCm[Piece] = Brick->GetActorLocation();
		}
		const FPieceBox& Box = B->GetBinding(Piece).Box;
		BoundsCm += FBox(Box.CentreCm - Box.ExtentCm, Box.CentreCm + Box.ExtentCm);

		if (IsCutPiece(Box, Spec))
		{
			R.CutPieces.Add(Piece);
			CutBoxCm += FBox(Box.CentreCm - Box.ExtentCm, Box.CentreCm + Box.ExtentCm);
			FPieceRef Ref;
			Ref.StructureId = R.StructureId;
			Ref.PieceIndex = Piece;
			R.CutRefs.Add(Ref);
		}
	}

	/* A close view facing the spec's chosen wall, over the full building height at that wall. */
	CutBoxCm.Min.Z = 0.0;
	CutBoxCm.Max.Z = BoundsCm.Max.Z;
	R.CloseView = FaceView(CutBoxCm, Spec.CloseFace);
	R.WideView = DestructionScenarios::ViewpointFor(
		BoundsCm, AspectHeightOverWidth, DestructionScenarios::EScenarioFraming::ThreeQuarter);

	Note(*Test, FString::Printf(
		TEXT("JOINED at world time %.4f s: %d pieces, %d joints; the cut names %d pieces of course %d"),
		R.BuiltAtSeconds, S.NumPieces(), S.NumConnections(), R.CutPieces.Num(), Spec.Course));

	Test->TestTrue(TEXT("the cut must name pieces"), R.CutPieces.Num() > 0);

	Append(TEXT("selection.csv"), TEXT("piece,material,face,massKg,minX,minY,minZ,maxX,maxY,maxZ,joints,supportAsLaid,kind"));
	for (const int32 Piece : R.CutPieces)
	{
		const FPieceBox& Box = B->GetBinding(Piece).Box;
		const FVector Lo = Box.CentreCm - Box.ExtentCm;
		const FVector Hi = Box.CentreCm + Box.ExtentCm;
		EFace Face = EFace::Back;
		FaceOf(Box, Face);
		const bool bPilaster = (Face == EFace::Back) ? Lo.Y > 336.5 : Hi.Y < 0.0;
		Append(TEXT("selection.csv"), FString::Printf(
			TEXT("%d,%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%s,%s"),
			Piece, MaterialName(S, Piece), Face == EFace::Back ? TEXT("back") : TEXT("front"),
			S.GetPiece(Piece).MassKg, Lo.X, Lo.Y, Lo.Z, Hi.X, Hi.Y, Hi.Z,
			JointCount(S, Piece), SupportName(S.GetPieceSupport(Piece)),
			bPilaster ? TEXT("pilaster") : TEXT("wythe")));
	}

	R.bJoined = true;
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FWCutThaw, FAutomationTestBase*, Test);
bool FWCutThaw::Update()
{
	using namespace WarehouseCutExperiment;
	if (Record().bJoined)
	{
		SetDilation(AutomationCommon::GetAnyGameWorld(), 1.0f);
	}
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FWCutReframe, FAutomationTestBase*, Test, int32, Which);
bool FWCutReframe::Update()
{
	using namespace WarehouseCutExperiment;
	FRecord& R = Record();
	if (R.bJoined)
	{
		FrameTo(AutomationCommon::GetAnyGameWorld(), Which == 0 ? R.WideView : R.CloseView);
	}
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FWCutSelect, FAutomationTestBase*, Test);
bool FWCutSelect::Update()
{
	using namespace WarehouseCutExperiment;
	FRecord& R = Record();
	if (!R.bJoined)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	ADestructionGamePlayerController* const Ctl = Controller(World);
	FStructureBinding* const B = Binding(World, R.StructureId);
	if (Ctl == nullptr || B == nullptr)
	{
		Test->AddError(TEXT("no player controller or binding to select through"));
		return true;
	}

	int32 Landed = 0;
	const double T0 = FPlatformTime::Seconds();
	for (const int32 Piece : R.CutPieces)
	{
		FVector Start, End;
		RayFor(B->GetBinding(Piece).Box, Start, End);
		if (Ctl->PrimaryAlongRay(Start, End))
		{
			++Landed;
		}
		else
		{
			Note(*Test, FString::Printf(TEXT("click on piece %d did NOT land"), Piece));
		}
	}
	const double T1 = FPlatformTime::Seconds();

	const FPieceSelection& Selection = Ctl->GetPieceSelection();
	int32 Missing = 0;
	for (const FPieceRef& Ref : R.CutRefs)
	{
		if (!Selection.Contains(Ref))
		{
			++Missing;
		}
	}

	Note(*Test, FString::Printf(
		TEXT("SELECTED %.4f s into the hold: %d clicks, %d landed, selection holds %d (%d of the course missing); clicking took %.1f ms"),
		World->GetTimeSeconds() - R.BuiltAtSeconds, R.CutPieces.Num(), Landed, Selection.Num(), Missing, (T1 - T0) * 1000.0));

	Test->TestEqual(TEXT("every click must land"), Landed, R.CutPieces.Num());
	Test->TestEqual(TEXT("the selection holds the whole cut"), Selection.Num(), R.CutPieces.Num());
	Test->TestEqual(TEXT("every piece of the cut is selected"), Missing, 0);

	Shot(*Test, FString(CurrentSpec().Name) + TEXT("_Before"));
	R.ShotsTaken.Add(FString(CurrentSpec().Name) + TEXT("_Before"));
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FWCutShot, FAutomationTestBase*, Test, FString, BaseName);
bool FWCutShot::Update()
{
	using namespace WarehouseCutExperiment;
	if (!Record().bJoined)
	{
		return true;
	}
	Shot(*Test, BaseName, /*bWithUI*/ false);
	Record().ShotsTaken.Add(BaseName);
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FWCutBaseline, FAutomationTestBase*, Test);
bool FWCutBaseline::Update()
{
	using namespace WarehouseCutExperiment;
	FRecord& R = Record();
	if (!R.bJoined)
	{
		return true;
	}
	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	FStructureBinding* const B = Binding(World, R.StructureId);
	if (B == nullptr)
	{
		return true;
	}
	const FStructure::FSolveAndBreakReport& Rep = B->GetStructure().GetLastSolveAndBreakReport();
	Test->TestTrue(TEXT("the hold must have expired and the level run before the cut"),
		World->GetTimeSeconds() - R.BuiltAtSeconds > 4.0 && Rep.Passes.Num() > 0);
	WriteReport(TEXT("break_report.txt"),
		FString::Printf(TEXT("=== BASELINE: the level's own settle at the end of its hold, nothing cut (world time %.4f s) ==="),
			World->GetTimeSeconds()), Rep);
	Note(*Test, FString::Printf(TEXT("BASELINE settle: %.1f ms, %d breaking passes, %d not held"),
		Rep.TotalMs, Rep.BreakingPasses, Rep.Passes.Num() > 0 ? Rep.Passes.Last().NotHeldAfter : 0));
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FWCutDelete, FAutomationTestBase*, Test);
bool FWCutDelete::Update()
{
	using namespace WarehouseCutExperiment;
	FRecord& R = Record();
	if (!R.bJoined)
	{
		return true;
	}

	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	ADestructionGamePlayerController* const Ctl = Controller(World);
	const FPieceAction* const Delete = DeleteAction();
	if (Ctl == nullptr || Delete == nullptr)
	{
		Test->AddError(TEXT("no controller or Delete action"));
		return true;
	}

	int32 DeleteRow = INDEX_NONE;
	const TArrayView<const FPieceMenuRow> Rows = Ctl->GetShownPieceMenuRows();
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].Action == Delete)
		{
			DeleteRow = Index;
		}
	}
	if (DeleteRow == INDEX_NONE)
	{
		Test->AddError(FString::Printf(TEXT("the piece menu offers no Delete row (%d rows)"), Rows.Num()));
		return true;
	}
	const int32 RowRefs = Rows[DeleteRow].Refs.Num();

	const double T0 = FPlatformTime::Seconds();
	const bool bChose = Ctl->ChoosePieceMenuRow(DeleteRow);
	const double T1 = FPlatformTime::Seconds();

	R.CommitMs = (T1 - T0) * 1000.0;
	R.CutAtSeconds = World->GetTimeSeconds();
	R.LastFrameWallSeconds = T1;
	Test->TestTrue(TEXT("the Delete row must commit"), bChose);

	FStructureBinding* const B = Binding(World, R.StructureId);
	if (B == nullptr)
	{
		Test->AddError(TEXT("the binding vanished on delete"));
		return true;
	}
	const FStructure& S = B->GetStructure();
	R.CutReport = S.GetLastSolveAndBreakReport();

	R.PreviousCm.SetNumZeroed(B->NumPieces());
	FString PiecesCsv;
	Buffer(PiecesCsv, TEXT("piece,material,grounded,massKg,support,released,removed,cx,cy,cz"));

	int32 Stranded = 0, Falling = 0, Unanswered = 0;
	for (int32 Piece = 0; Piece < B->NumPieces(); ++Piece)
	{
		const bool bRemoved = B->IsPieceRemoved(Piece);
		const AActor* const Brick = Cast<AActor>(B->GetActor(Piece));
		const FVector At = Brick != nullptr ? Brick->GetActorLocation() : FVector::ZeroVector;
		R.PreviousCm[Piece] = At;

		const TCHAR* Sup = TEXT("removed");
		if (bRemoved)
		{
			++R.RemovedCount;
		}
		else if (!S.HasSupportAnswer(Piece))
		{
			Sup = TEXT("unanswered");
			++Unanswered;
		}
		else
		{
			const EPieceSupport PS = S.GetPieceSupport(Piece);
			Sup = SupportName(PS);
			switch (PS)
			{
				case EPieceSupport::Grounded:  ++R.GroundedAfter; break;
				case EPieceSupport::Supported: ++R.SupportedAfter; break;
				case EPieceSupport::Stranded:  ++Stranded; break;
				default:                       ++Falling; break;
			}
			if (B->IsReleased(Piece))
			{
				R.ReleasedPieces.Add(Piece);
				const TCHAR* M = MaterialName(S, Piece);
				if (FCString::Strcmp(M, TEXT("ClayBrick")) == 0) ++R.RelClay;
				else if (FCString::Strcmp(M, TEXT("Timber")) == 0) ++R.RelTimber;
				else ++R.RelStone;
				if (R.LaidAtCm[Piece].Z >= 479.0) ++R.RelRoof; else ++R.RelUpper;
			}
		}

		Buffer(PiecesCsv, FString::Printf(TEXT("%d,%s,%d,%.4f,%s,%d,%d,%.3f,%.3f,%.3f"),
			Piece, MaterialName(S, Piece), S.GetPiece(Piece).bIsGrounded ? 1 : 0, S.GetPiece(Piece).MassKg,
			Sup, (!bRemoved && B->IsReleased(Piece)) ? 1 : 0, bRemoved ? 1 : 0, At.X, At.Y, At.Z));
	}

	FString JointsCsv;
	Buffer(JointsCsv, TEXT("joint,pieceA,pieceB,nx,ny,nz,areaSqCm,breakPass,utilisationAfterCut"));
	for (int32 J = 0; J < S.NumConnections(); ++J)
	{
		const FConnection& C = S.GetConnection(J);
		const int32 Pass = S.GetBreakPass(J);
		const double Util = C.HasGiven() ? -1.0 : S.GetConnectionUtilisation(J);
		Buffer(JointsCsv, FString::Printf(TEXT("%d,%d,%d,%.4f,%.4f,%.4f,%.3f,%d,%.4f"),
			J, C.PieceA, C.PieceB, C.InterfaceNormal.X, C.InterfaceNormal.Y, C.InterfaceNormal.Z, C.InterfaceAreaSqCm, Pass, Util));
	}
	R.PiecesAfterCutCsv = MoveTemp(PiecesCsv);
	R.JointsCsv = MoveTemp(JointsCsv);

	WriteReport(TEXT("break_report.txt"),
		FString::Printf(TEXT("=== THE CUT: %s, deleted through the player's menu at world time %.4f s; whole commit %.3f ms ==="),
			CurrentSpec().What, R.CutAtSeconds, R.CommitMs), R.CutReport);

	Note(*Test, FString::Printf(
		TEXT("CUT: commit %.3f ms of which SolveAndBreak %.3f ms in %d breaking pass(es); after: %d grounded, %d supported, %d stranded, %d falling, %d removed; %d released"),
		R.CommitMs, R.CutReport.TotalMs, R.CutReport.BreakingPasses, R.GroundedAfter, R.SupportedAfter, Stranded, Falling, R.RemovedCount, R.ReleasedPieces.Num()));

	Buffer(R.FallSummaryCsv, TEXT("frame,secondsSinceCut,released,moving,maxDropCm,meanDropCm,maxDisplacementCm,belowGround,realMsSinceLastFrame"));
	Buffer(R.FallPiecesCsv, TEXT("frame,secondsSinceCut,piece,x,y,z,dropCm"));

	Shot(*Test, FString(CurrentSpec().Name) + TEXT("_After_0s"), /*bWithUI*/ false);
	R.ShotsTaken.Add(FString(CurrentSpec().Name) + TEXT("_After_0s"));
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FWCutTrack, FAutomationTestBase*, Test, int32, TotalFrames);
bool FWCutTrack::Update()
{
	using namespace WarehouseCutExperiment;
	FRecord& R = Record();
	if (!R.bJoined)
	{
		return true;
	}
	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	FStructureBinding* const B = Binding(World, R.StructureId);
	if (B == nullptr)
	{
		return true;
	}

	++R.FramesTracked;
	const int32 Frame = R.FramesTracked;
	const double Since = World->GetTimeSeconds() - R.CutAtSeconds;
	const double NowWall = FPlatformTime::Seconds();
	const double RealMs = (NowWall - R.LastFrameWallSeconds) * 1000.0;
	R.LastFrameWallSeconds = NowWall;

	int32 Moving = 0, BelowGround = 0;
	double MaxDrop = 0.0, SumDrop = 0.0, MaxDisp = 0.0;
	const bool bPerPiece = (Frame % 6) == 0;

	for (const int32 Piece : R.ReleasedPieces)
	{
		const AActor* const Brick = Cast<AActor>(B->GetActor(Piece));
		if (Brick == nullptr)
		{
			continue;
		}
		const FVector At = Brick->GetActorLocation();
		const double Drop = R.LaidAtCm[Piece].Z - At.Z;
		const double Disp = FVector::Dist(At, R.LaidAtCm[Piece]);
		const double Step = FVector::Dist(At, R.PreviousCm[Piece]);
		R.PreviousCm[Piece] = At;

		if (Step > 0.05) ++Moving;
		if (At.Z < -50.0) ++BelowGround;
		MaxDrop = FMath::Max(MaxDrop, Drop);
		SumDrop += Drop;
		MaxDisp = FMath::Max(MaxDisp, Disp);
		if (bPerPiece)
		{
			Buffer(R.FallPiecesCsv, FString::Printf(TEXT("%d,%.4f,%d,%.3f,%.3f,%.3f,%.3f"), Frame, Since, Piece, At.X, At.Y, At.Z, Drop));
		}
	}
	R.MaxDropFinalCm = MaxDrop;

	const int32 Released = R.ReleasedPieces.Num();
	Buffer(R.FallSummaryCsv, FString::Printf(TEXT("%d,%.4f,%d,%d,%.3f,%.3f,%.3f,%d,%.2f"),
		Frame, Since, Released, Moving, MaxDrop, Released > 0 ? SumDrop / Released : 0.0, MaxDisp, BelowGround, RealMs));

	struct FMoment { int32 Frame; const TCHAR* Suffix; };
	static const FMoment Moments[] = {
		{ 15, TEXT("_After_0p25s") }, { 30, TEXT("_After_0p5s") }, { 60, TEXT("_After_1s") },
		{ 120, TEXT("_After_2s") }, { 180, TEXT("_After_3s") }, { 300, TEXT("_After_5s") }, { 480, TEXT("_After_8s") },
	};
	for (const FMoment& M : Moments)
	{
		if (M.Frame == Frame)
		{
			Note(*Test, FString::Printf(TEXT("FALL %.3f s: %d released, %d moving, max drop %.1f cm, mean %.1f cm; frame took %.1f ms real"),
				Since, Released, Moving, MaxDrop, Released > 0 ? SumDrop / Released : 0.0, RealMs));
			R.ShotsDue.Add(FString(CurrentSpec().Name) + M.Suffix);
		}
	}

	/* One pending screenshot at a time: a second request while one is in flight loses the first. */
	if (R.ShotsDue.Num() > 0 && !FScreenshotRequest::IsScreenshotRequested())
	{
		const FString Name = R.ShotsDue[0];
		R.ShotsDue.RemoveAt(0);
		Shot(*Test, Name, /*bWithUI*/ false);
		R.ShotsTaken.Add(Name);
	}

	return Frame >= TotalFrames && R.ShotsDue.Num() == 0;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FWCutFinal, FAutomationTestBase*, Test);
bool FWCutFinal::Update()
{
	using namespace WarehouseCutExperiment;
	FRecord& R = Record();
	if (!R.bJoined)
	{
		return true;
	}
	UWorld* const World = AutomationCommon::GetAnyGameWorld();
	FStructureBinding* const B = Binding(World, R.StructureId);
	if (B == nullptr)
	{
		return true;
	}

	FString FinalCsv;
	Buffer(FinalCsv, TEXT("piece,released,removed,laidZ,finalZ,displacementCm,dropCm"));
	int32 MovedUnreleased = 0, Fallen = 0, Standing = 0;
	for (int32 Piece = 0; Piece < B->NumPieces(); ++Piece)
	{
		const bool bRemoved = B->IsPieceRemoved(Piece);
		const AActor* const Brick = Cast<AActor>(B->GetActor(Piece));
		if (Brick == nullptr)
		{
			Buffer(FinalCsv, FString::Printf(TEXT("%d,0,%d,,,,"), Piece, bRemoved ? 1 : 0));
			continue;
		}
		const FVector Laid = R.LaidAtCm[Piece];
		const FVector At = Brick->GetActorLocation();
		const double Disp = FVector::Dist(At, Laid);
		const bool bReleased = B->IsReleased(Piece);
		if (!bReleased && Disp > 0.1) ++MovedUnreleased;
		if (bReleased) { if (Disp > 5.0) ++Fallen; else ++Standing; }
		Buffer(FinalCsv, FString::Printf(TEXT("%d,%d,%d,%.3f,%.3f,%.3f,%.3f"),
			Piece, bReleased ? 1 : 0, bRemoved ? 1 : 0, Laid.Z, At.Z, Disp, Laid.Z - At.Z));
	}

	Flush(TEXT("pieces_final.csv"), FinalCsv);
	Flush(TEXT("pieces_after_cut.csv"), R.PiecesAfterCutCsv);
	Flush(TEXT("joints.csv"), R.JointsCsv);
	Flush(TEXT("fall_summary.csv"), R.FallSummaryCsv);
	Flush(TEXT("fall_pieces.csv"), R.FallPiecesCsv);

	Note(*Test, FString::Printf(TEXT("FINAL %.3f s after the cut: %d released moved >5 cm, %d did not, %d unreleased moved (must be 0)"),
		World->GetTimeSeconds() - R.CutAtSeconds, Fallen, Standing, MovedUnreleased));
	Test->TestEqual(TEXT("a brick the solver still holds must not have moved"), MovedUnreleased, 0);

	/* THE SELF-DOCUMENTING SUMMARY — the whole point of the harness: every run explains itself. */
	const FCutSpec& Spec = CurrentSpec();
	const FStructure::FSolveAndBreakReport& Rep = R.CutReport;
	const FStructure::FBreakPassReport* First = Rep.Passes.Num() > 0 ? &Rep.Passes[0] : nullptr;
	FString Sum;
	Buffer(Sum, FString::Printf(TEXT("# %s"), Spec.Name));
	Buffer(Sum, FString::Printf(TEXT("%s"), Spec.What));
	Buffer(Sum, TEXT(""));
	Buffer(Sum, FString::Printf(TEXT("CUT: %d pieces removed."), R.RemovedCount));
	Buffer(Sum, FString::Printf(TEXT("RELEASED: %d pieces (%d clay, %d stone, %d timber; %d upper wall, %d roof/gable)."),
		R.ReleasedPieces.Num(), R.RelClay, R.RelStone, R.RelTimber, R.RelUpper, R.RelRoof));
	Buffer(Sum, FString::Printf(TEXT("STOOD: %d grounded, %d supported."), R.GroundedAfter, R.SupportedAfter));
	Buffer(Sum, FString::Printf(TEXT("SETTLED: %d released pieces came to rest >5 cm from where they were laid; max drop %.0f cm."),
		Fallen, R.MaxDropFinalCm));
	Buffer(Sum, TEXT(""));
	Buffer(Sum, FString::Printf(TEXT("BREAK DECISION: %.1f ms total, %d breaking pass(es)."), Rep.TotalMs, Rep.BreakingPasses));
	if (First != nullptr)
	{
		Buffer(Sum, FString::Printf(TEXT("  first pass: load solve %.1f ms (%d fixpoint iterations), gate %.2f ms (declined=%d), sweep %.1f ms severed %d, prover %.1f ms (%d poses, %d pivots, %d blocks, fell=%d) severed %d."),
			First->Solve.TotalMs, First->Solve.FixpointIterations, First->GateMs, First->GateDisposition == 0 ? 1 : 0,
			First->CapacitySweepMs, First->JointsGivenToSweep, First->RegionalProverMs, First->RegionalPoses,
			First->RegionalLpPivots, First->RegionalLastBlocks, First->bRegionalFell ? 1 : 0, First->JointsSeveredByProver));
	}
	const int32 SeveredByBreak = Rep.IntactJointsBefore - Rep.IntactJointsAfter;
	Buffer(Sum, FString::Printf(TEXT("MECHANISM: %s (%d joints severed by the break authority; %d pieces lost their load path)."),
		SeveredByBreak == 0 ? TEXT("load-path loss — nothing over capacity, the masonry above simply lost the ground")
			: TEXT("joint failure — joints gave over their capacity"),
		SeveredByBreak, R.ReleasedPieces.Num() - SeveredByBreak >= 0 ? R.ReleasedPieces.Num() : R.ReleasedPieces.Num()));
	Flush(TEXT("SUMMARY.txt"), Sum);
	Note(*Test, TEXT("SUMMARY.txt written"));

	FrameTo(World, R.WideView);
	R.ShotsDue.Add(FString(Spec.Name) + TEXT("_After_Final_Wide"));
	return true;
}

/** After a reframe settles, take the two final frames (close then wide) and collect everything. */
DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FWCutFinalWideShot, FAutomationTestBase*, Test);
bool FWCutFinalWideShot::Update()
{
	using namespace WarehouseCutExperiment;
	FRecord& R = Record();
	if (!R.bJoined)
	{
		return true;
	}
	Shot(*Test, FString(CurrentSpec().Name) + TEXT("_After_Final_Wide"), /*bWithUI*/ false);
	R.ShotsTaken.Add(FString(CurrentSpec().Name) + TEXT("_After_Final_Wide"));
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FWCutCollect, FAutomationTestBase*, Test);
bool FWCutCollect::Update()
{
	using namespace WarehouseCutExperiment;
	for (const FString& BaseName : Record().ShotsTaken)
	{
		const FString From = ShotPath(BaseName);
		const FString To = SpecFolder() / BaseName + TEXT(".png");
		const int64 Size = IFileManager::Get().FileSize(*From);
		if (Size > 0)
		{
			IFileManager::Get().Copy(*To, *From, /*Replace*/ true);
		}
		Note(*Test, FString::Printf(TEXT("FRAME %s (%lld bytes)"), *BaseName, Size));
		Test->TestTrue(*FString::Printf(TEXT("%s must have been written"), *BaseName), Size > 32 * 1024);
	}
	return true;
}

/* ================================ the two complex tests ================================ */

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FWarehouseCutTest, "Experiment.WarehouseCut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::NonNullRHI | EAutomationTestFlags::ProductFilter)

void FWarehouseCutTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	for (const WarehouseCutExperiment::FCutSpec& Spec : WarehouseCutExperiment::Specs())
	{
		OutBeautifiedNames.Add(Spec.Name);
		OutTestCommands.Add(Spec.Name);
	}
}

bool FWarehouseCutTest::RunTest(const FString& Parameters)
{
	using namespace WarehouseCutExperiment;

	const int32 SpecIndex = IndexOfSpec(Parameters);
	if (SpecIndex == INDEX_NONE)
	{
		AddError(FString::Printf(TEXT("no cut spec named '%s'"), *Parameters));
		return false;
	}
	Record().Reset(SpecIndex);

	IFileManager::Get().MakeDirectory(*SpecFolder(), /*Tree*/ true);
	for (const TCHAR* const Name : { TEXT("run.log"), TEXT("selection.csv"), TEXT("break_report.txt"),
		TEXT("pieces_after_cut.csv"), TEXT("joints.csv"), TEXT("fall_summary.csv"), TEXT("fall_pieces.csv"),
		TEXT("pieces_final.csv"), TEXT("SUMMARY.txt") })
	{
		IFileManager::Get().Delete(*(SpecFolder() / Name), false, true, true);
	}
	TArray<FString> OldFrames;
	IFileManager::Get().FindFiles(OldFrames, *(FPaths::ScreenShotDir() / (FString(Parameters) + TEXT("_*.png"))), true, false);
	for (const FString& Old : OldFrames)
	{
		IFileManager::Get().Delete(*(FPaths::ScreenShotDir() / Old), false, true, true);
	}
	Append(TEXT("run.log"), FString::Printf(TEXT("=== %s, %s ==="), Specs()[SpecIndex].Name, *FDateTime::Now().ToString()));

	ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(TEXT("DisableAllScreenMessages")));
	ADD_LATENT_AUTOMATION_COMMAND(FWCutOpenSession(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWCutOpenMap(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWCutJoin(this, 0));

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForShadersToFinishCompilingInGame());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(SlateFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(FrozenFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWCutReframe(this, 1));
	ADD_LATENT_AUTOMATION_COMMAND(FWCutThaw(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ExposureFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FWCutSelect(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWCutShot(this, FString(Specs()[SpecIndex].Name) + TEXT("_Before_NoUI")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(BaselineHoldFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWCutBaseline(this));

	ADD_LATENT_AUTOMATION_COMMAND(FWCutDelete(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWCutTrack(this, FallFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FWCutFinal(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(ReframeFrames));
	ADD_LATENT_AUTOMATION_COMMAND(FWCutFinalWideShot(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForEngineFramesCommand(WriteFrames));

	ADD_LATENT_AUTOMATION_COMMAND(FWCutCloseSession(this));
	ADD_LATENT_AUTOMATION_COMMAND(FWCutCollect(this));
	return true;
}

/**
 * THE HEADLESS TIMING SPREAD — the same cut through the world-free layout and solver, five fresh
 * loads, no RHI. A decision-time number with a spread rather than the single sample the live capture
 * takes while a renderer draws beside it. Writes headless_timing.txt into the spec's folder.
 */
IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FWarehouseCutTimingTest, "Experiment.WarehouseCutTiming",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

void FWarehouseCutTimingTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	for (const WarehouseCutExperiment::FCutSpec& Spec : WarehouseCutExperiment::Specs())
	{
		OutBeautifiedNames.Add(Spec.Name);
		OutTestCommands.Add(Spec.Name);
	}
}

bool FWarehouseCutTimingTest::RunTest(const FString& Parameters)
{
	using namespace WarehouseCutExperiment;
	using namespace DestructionLayout;

	const int32 SpecIndex = IndexOfSpec(Parameters);
	if (SpecIndex == INDEX_NONE)
	{
		AddError(FString::Printf(TEXT("no cut spec named '%s'"), *Parameters));
		return false;
	}
	Record().Reset(SpecIndex);
	const FCutSpec& Spec = Specs()[SpecIndex];

	IFileManager::Get().MakeDirectory(*SpecFolder(), /*Tree*/ true);
	IFileManager::Get().Delete(*(SpecFolder() / TEXT("headless_timing.txt")), false, true, true);
	Append(TEXT("headless_timing.txt"), FString::Printf(
		TEXT("=== HEADLESS TIMING, %s: %s, one SolveAndBreak per fresh world-free load ==="), *FDateTime::Now().ToString(), Spec.What));

	constexpr int32 Repeats = 5;
	int32 ExpectedCut = INDEX_NONE;
	for (int32 Repeat = 0; Repeat < Repeats; ++Repeat)
	{
		FBrickLayout Cut;
		FString Why;
		const double LoadStart = FPlatformTime::Seconds();
		if (!TestTrue(*FString::Printf(TEXT("the warehouse must load: %s"), *Why),
				DestructionLayoutFile::LoadFile(DestructionLayoutFile::ContentPath(TEXT("Warehouse")), Cut, &Why)))
		{
			return false;
		}
		const double LoadMs = (FPlatformTime::Seconds() - LoadStart) * 1000.0;

		int32 Removed = 0;
		for (int32 Piece = 0; Piece < Cut.Boxes.Num(); ++Piece)
		{
			if (IsCutPiece(Cut.Boxes[Piece], Spec) && Cut.Structure.RemovePiece(Piece))
			{
				++Removed;
			}
		}
		if (ExpectedCut == INDEX_NONE)
		{
			ExpectedCut = Removed;
		}
		TestEqual(TEXT("the cut removes the same pieces every load"), Removed, ExpectedCut);

		const int32 Passes = Cut.Structure.SolveAndBreak();
		const FStructure::FSolveAndBreakReport R = Cut.Structure.GetLastSolveAndBreakReport();
		const FStructure::FBreakPassReport* P = R.Passes.Num() > 0 ? &R.Passes[0] : nullptr;

		int32 NotHeld = 0;
		for (int32 Piece = 0; Piece < Cut.Structure.NumPieces(); ++Piece)
		{
			if (Cut.Structure.IsPieceRemoved(Piece) || !Cut.Structure.HasSupportAnswer(Piece))
			{
				continue;
			}
			const EPieceSupport PS = Cut.Structure.GetPieceSupport(Piece);
			if (PS != EPieceSupport::Grounded && PS != EPieceSupport::Supported)
			{
				++NotHeld;
			}
		}

		Append(TEXT("headless_timing.txt"), FString::Printf(
			TEXT("repeat %d: load %.1f ms, %d removed | CUT DECISION: %.3f ms total, %d breaking pass(es); solve %.3f ms (%d fixpoint it), gate %.3f ms, sweep %.3f ms severed %d, prover %.3f ms (%d poses, %d pivots, %d blocks, fell %d) severed %d; %d not held"),
			Repeat, LoadMs, Removed, R.TotalMs, Passes,
			P ? P->Solve.TotalMs : 0.0, P ? P->Solve.FixpointIterations : 0, P ? P->GateMs : 0.0,
			P ? P->CapacitySweepMs : 0.0, P ? P->JointsGivenToSweep : 0, P ? P->RegionalProverMs : 0.0,
			P ? P->RegionalPoses : 0, P ? P->RegionalLpPivots : 0, P ? P->RegionalLastBlocks : -1,
			P && P->bRegionalFell ? 1 : 0, P ? P->JointsSeveredByProver : 0, NotHeld));

		if (Repeat == 0)
		{
			WriteReport(TEXT("headless_timing.txt"), TEXT("--- repeat 0 pass table ---"), R);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
