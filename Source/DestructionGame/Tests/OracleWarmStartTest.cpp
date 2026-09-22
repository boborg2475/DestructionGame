// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Warm starts after a deletion (PROMOTION_DESIGN.md §5.4, §12 D2''). A measurement, not a
 * feature: does seeding the simplex with the previous solve's basis cut the pivot count when
 * a brick is removed? The gate fixture needs 50 ms and answers cold in 66.6 ms.
 *
 * Seam (RigidBlockOracle.h): FOracleResult::FinalBasis plus its shape (NumStructCols,
 * ArtificialStart); FOracleProblem::StartingBasis (INDEX_NONE = no hint, empty = cold and
 * bit-identical to before); and FOracleResult::WarmStartColumnsAccepted, which separates a warm
 * start that saved nothing from one that was discarded. Primal simplex only.
 *
 * Two hazards the assertions catch. A warm basis is usually primal infeasible, but Refactorise
 * clamps negative basic values to zero and phase 1 is skipped when artificials sum to zero, so
 * the repair has to be forced. A warm basis may also be singular in the new matrix; it is
 * repaired with the cold default per row, not refused.
 *
 * Mapping: rows are 3 per non-grounded block, the lambda cap row, then strength rows per
 * contact; columns are lambda, [n+, n-, p, q] per contact, one slack per inequality row, one
 * artificial per row. Moving the doomed joints to the end lets every survivor keep its ordinal,
 * so the mapping is arithmetic on the reported shapes. The deleted block stays as a massless
 * orphan (three empty rows) so no rows vanish from the middle. The mapping is test-side:
 * production will map from its own assembly.
 *
 * Result (2026-08-16): the lever is refuted, and gets worse with size.
 *
 *     row                        COLD    WARM   accepted   cold/warm
 *     gate 8x10, one brick        343     342   2600       1.003x
 *     gate 8x10, top course       291   1,543   2032       0.189x
 *     gate 8x10, ground cut        90      77   2441       1.169x
 *     12x12, one brick            919   4,192   4560       0.219x
 *     wall-01, one brick        4,764   4,828      0       ABANDONED
 *
 * Predictions were 1.6x-18x. wall-01's seeded basis went singular at the first periodic
 * refactorisation (pivot 64), so the solver discarded it and solved cold; a dual simplex from
 * the same mapped basis would inherit that. The gravity-live row (PART C, 2026-08-18) matches
 * lambda* to the bit but is 0.406x. Ratios are read against the cold re-solve, not the previous
 * solve, which is already cheaper because the problem shrank. The discarded attempt's refusal
 * reason is not yet observable (N1, CURRENT_STATE).
 *
 * Pinned exactly: sizes, deletion counts, every pivot count (the solver is deterministic),
 * lambda, verdicts and accepted counts. Cold and warm lambda compare with operator== because in
 * the dead pose lambda is exactly LambdaCap or 0. Wall-clock time is reported, never asserted;
 * "warm < cold" is not asserted since it is what is being measured.
 *
 * Bite-provers: X12 (repair gated off) makes three PART B rows refuse rather than return a wrong
 * lambda. X14 (seed never installed) fails 16 pivot assertions while every WarmAccepted pin
 * stays green, which is why the pivot pins carry this file.
 *
 * Cost: FAST tier +5-6 s, FULL tier +56-61 s (wall-01). No ticking world. Named namespace with
 * a Warm prefix: unity builds merge anonymous namespaces (TRAPS).
 */
namespace OracleWarmStartSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	// Brick and grid dimensions.
	constexpr double WarmBrickLengthCm = 21.5;
	constexpr double WarmBrickWidthCm = 10.25;
	constexpr double WarmBrickHeightCm = 6.5;
	constexpr double WarmClayDensityGramsPerCubicCm = 1.9;
	constexpr double WarmJointCm = 1.0;

	/** Course pitch on the coordinating grid: 7.5 cm. */
	constexpr double WarmCoursePitchCm = WarmBrickHeightCm + WarmJointCm;

	/** Unmeasured sentinel. Not INDEX_NONE, which the seam reports for "no warm start supplied". */
	constexpr int32 WarmUnmeasured = -2;

	// PART C's pins: the one gravity-live row, where lambda* is a real number.
	constexpr int32 WarmLiveDoomedJoints = 6;
	constexpr int32 WarmLiveColdPivots = 186;
	constexpr int32 WarmLiveWarmPivots = 458;
	constexpr int32 WarmLiveAccepted = 340;

	/**
	 * Measured 2166.9018254298085. Cold and warm landed on the same bits, but the pin is a
	 * narrow window so it asserts the optimum, not the pivot path that reached it.
	 */
	constexpr double WarmLiveLambdaLo = 2166.90182;
	constexpr double WarmLiveLambdaHi = 2166.90183;

	/**
	 * Structural column count derived independently of the solver: lambda, then [n+, n-, p, q]
	 * per contact, two contacts per joint.
	 */
	int32 WarmStructuralColumnsFor(const FOracleProblem& Problem)
	{
		return 1 + 4 * (2 * Problem.Joints.Num());
	}

	/** Lay a scenario row's structure and apply its cut. */
	bool WarmBuildScenario(const TCHAR* ScenarioName, FStructure& Out, FString& OutWhy)
	{
		using namespace DestructionScenarios;

		const int32 Index = IndexOfName(FName(ScenarioName));

		if (Index == INDEX_NONE)
		{
			OutWhy = FString::Printf(TEXT("no scenario row named %s"), ScenarioName);
			return false;
		}

		FBrickLayout Layout;
		TArray<int32> CutPieces;

		if (!Build(Catalogue()[Index], Layout, CutPieces))
		{
			OutWhy = FString::Printf(TEXT("the producer refused %s"), ScenarioName);
			return false;
		}

		for (const int32 Piece : CutPieces)
		{
			if (!Layout.Structure.RemovePiece(Piece))
			{
				OutWhy = FString::Printf(
					TEXT("%s: cut piece %d could not be removed"), ScenarioName, Piece);
				return false;
			}
		}

		Out = MoveTemp(Layout.Structure);
		return true;
	}

	/** Intact running-bond wall; 8 x 10 is the gate fixture. */
	bool WarmBuildIntactWall(int32 Courses, int32 Cells, FStructure& Out, FString& OutWhy)
	{
		DestructionWallCases::FWallSpec Spec;
		Spec.BrickSizeCm = FVector(WarmBrickLengthCm, WarmBrickWidthCm, WarmBrickHeightCm);
		Spec.JointThicknessCm = WarmJointCm;
		Spec.DensityGramsPerCubicCm = WarmClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = Courses;
		Spec.Cells = Cells;
		Spec.Bond = DestructionWallCases::EWallBond::Running;
		Spec.Strength = GeneralPurposeMortar;

		DestructionWallCases::FWallLayout Wall;

		if (!DestructionWallCases::Build(Spec, Wall))
		{
			OutWhy = FString::Printf(
				TEXT("the wall producer refused %d courses x %d cells"), Courses, Cells);
			return false;
		}

		Out = MoveTemp(Wall.Layout.Structure);
		return true;
	}

	struct FWarmReading
	{
		bool bAnswered = false;
		double Lambda = 0.0;
		int32 Pivots = 0;
		int32 PhaseOnePivots = INDEX_NONE;
		int32 Accepted = INDEX_NONE;
		double Seconds = 0.0;
		FString WhyNot;
		FOracleBasis FinalBasis;
	};

	FWarmReading WarmSolve(const FOracleProblem& Problem)
	{
		FWarmReading Out;

		const double Started = FPlatformTime::Seconds();
		const FOracleResult Result = SolveRigidBlock(Problem);
		Out.Seconds = FPlatformTime::Seconds() - Started;

		Out.bAnswered = Result.bAnswered;
		Out.Lambda = Result.Lambda;
		Out.Pivots = Result.SimplexIterations;
		Out.PhaseOnePivots = Result.PhaseOnePivots;
		Out.Accepted = Result.WarmStartColumnsAccepted;
		Out.WhyNot = Result.WhyNot;
		Out.FinalBasis = Result.FinalBasis;

		return Out;
	}

	/** Answered with lambda >= 1, production's reading of the verdict. */
	bool WarmIsFeasible(const FWarmReading& Reading)
	{
		return Reading.bAnswered && Reading.Lambda >= 1.0;
	}

	enum class EWarmDelete : uint8
	{
		/** One brick out of the named course, nearest the wall's mid-span. */
		OneBrick,

		/** Every brick of the topmost course. */
		TopCourse,

		/** The grounded course. The verdict flips to falls, so the warm basis describes a dead load path. */
		GroundCourse,
	};

	/** Which blocks the deletion takes. Deterministic: lowest index wins every tie. */
	void WarmSelectDoomed(
		const FOracleProblem& Problem, EWarmDelete What, int32 Course, TArray<bool>& bDoomed)
	{
		bDoomed.Init(false, Problem.Blocks.Num());

		double LowestZ = TNumericLimits<double>::Max();
		double HighestZ = -TNumericLimits<double>::Max();
		double LowX = TNumericLimits<double>::Max();
		double HighX = -TNumericLimits<double>::Max();

		for (const FOracleBlock& Block : Problem.Blocks)
		{
			LowestZ = FMath::Min(LowestZ, Block.CentroidZCm);
			HighestZ = FMath::Max(HighestZ, Block.CentroidZCm);
			LowX = FMath::Min(LowX, Block.CentroidXCm);
			HighX = FMath::Max(HighX, Block.CentroidXCm);
		}

		if (What == EWarmDelete::GroundCourse)
		{
			for (int32 Index = 0; Index < Problem.Blocks.Num(); ++Index)
			{
				bDoomed[Index] = Problem.Blocks[Index].bGrounded;
			}

			return;
		}

		if (What == EWarmDelete::TopCourse)
		{
			for (int32 Index = 0; Index < Problem.Blocks.Num(); ++Index)
			{
				const double Height = Problem.Blocks[Index].CentroidZCm;
				bDoomed[Index] = FMath::Abs(Height - HighestZ) <= 0.5 * WarmCoursePitchCm;
			}

			return;
		}

		const double WantZ = LowestZ + double(Course) * WarmCoursePitchCm;
		const double WantX = 0.5 * (LowX + HighX);

		int32 Best = INDEX_NONE;
		double BestScore = TNumericLimits<double>::Max();

		for (int32 Index = 0; Index < Problem.Blocks.Num(); ++Index)
		{
			const FOracleBlock& Block = Problem.Blocks[Index];

			if (Block.bGrounded)
			{
				continue;
			}

			if (FMath::Abs(Block.CentroidZCm - WantZ) > 0.5 * WarmCoursePitchCm)
			{
				continue;
			}

			const double Score = FMath::Abs(Block.CentroidXCm - WantX);

			if (Score < BestScore)
			{
				BestScore = Score;
				Best = Index;
			}
		}

		if (Best != INDEX_NONE)
		{
			bDoomed[Best] = true;
		}
	}

	/**
	 * Move the doomed joints to the end of the array, keeping relative order, so every
	 * surviving row and column keeps its ordinal. Returns the doomed joint count.
	 */
	int32 WarmOrderDoomedJointsLast(FOracleProblem& Problem, const TArray<bool>& bDoomed)
	{
		TArray<FOracleJoint> Kept;
		TArray<FOracleJoint> Doomed;

		for (const FOracleJoint& Joint : Problem.Joints)
		{
			if (bDoomed[Joint.BlockA] || bDoomed[Joint.BlockB])
			{
				Doomed.Add(Joint);
			}
			else
			{
				Kept.Add(Joint);
			}
		}

		const int32 Count = Doomed.Num();
		Problem.Joints = MoveTemp(Kept);
		Problem.Joints.Append(Doomed);

		return Count;
	}

	/** The problem after the deletion: doomed joints dropped, doomed blocks massless. */
	FOracleProblem WarmApplyDeletion(
		const FOracleProblem& Before, const TArray<bool>& bDoomed, int32 DoomedJoints)
	{
		FOracleProblem After = Before;
		After.Joints.SetNum(After.Joints.Num() - DoomedJoints);

		for (int32 Index = 0; Index < After.Blocks.Num(); ++Index)
		{
			if (bDoomed[Index])
			{
				After.Blocks[Index].MassKg = 0.0;
			}
		}

		return After;
	}

	/**
	 * Carry a basis across the deletion by ordinal arithmetic on the two shapes. Structural
	 * columns keep their index if the contact survives; slacks keep their ordinal; artificials
	 * follow their row. Anything with no image, or a column already claimed by another row
	 * (which would make the basis singular), is left INDEX_NONE.
	 */
	FOracleBasis WarmMapBasis(const FOracleBasis& Before, const FOracleBasis& AfterShape)
	{
		FOracleBasis Out;
		Out.NumStructCols = AfterShape.NumStructCols;
		Out.ArtificialStart = AfterShape.ArtificialStart;
		Out.Columns.Init(INDEX_NONE, AfterShape.Columns.Num());

		const int32 AfterSlacks = AfterShape.ArtificialStart - AfterShape.NumStructCols;

		TSet<int32> Claimed;

		for (int32 Row = 0; Row < Out.Columns.Num(); ++Row)
		{
			if (Row >= Before.Columns.Num())
			{
				continue;
			}

			const int32 Was = Before.Columns[Row];
			int32 Now = INDEX_NONE;

			if (Was < 0)
			{
				Now = INDEX_NONE;
			}
			else if (Was < Before.NumStructCols)
			{
				Now = Was < Out.NumStructCols ? Was : INDEX_NONE;
			}
			else if (Was < Before.ArtificialStart)
			{
				const int32 Ordinal = Was - Before.NumStructCols;
				Now = Ordinal < AfterSlacks ? Out.NumStructCols + Ordinal : INDEX_NONE;
			}
			else
			{
				const int32 OwningRow = Was - Before.ArtificialStart;
				Now = OwningRow < Out.Columns.Num()
					? Out.ArtificialStart + OwningRow
					: INDEX_NONE;
			}

			if (Now != INDEX_NONE && !Claimed.Contains(Now))
			{
				Out.Columns[Row] = Now;
				Claimed.Add(Now);
			}
		}

		return Out;
	}

	/** How many rows the mapping offers a hint for. */
	int32 WarmHintsIn(const FOracleBasis& Basis)
	{
		int32 Count = 0;

		for (const int32 Column : Basis.Columns)
		{
			if (Column != INDEX_NONE)
			{
				++Count;
			}
		}

		return Count;
	}

	struct FWarmRow
	{
		const TCHAR* Name = nullptr;

		/** The pre-run prediction, printed beside the result. */
		const TCHAR* Prediction = nullptr;

		TFunction<bool(FStructure&, FString&)> Build;

		EWarmDelete Delete = EWarmDelete::OneBrick;
		int32 Course = 0;

		// Fixture guards.
		int32 Blocks = INDEX_NONE;
		int32 Joints = INDEX_NONE;
		int32 DoomedBlocks = INDEX_NONE;
		int32 DoomedJoints = INDEX_NONE;

		// The previous solve, whose basis is the warm start.
		bool bBeforeStands = true;
		int32 BeforePivots = WarmUnmeasured;

		// The cold re-solve: the baseline.
		bool bAfterStands = true;
		int32 ColdPivots = WarmUnmeasured;
		int32 ColdPhaseOnePivots = WarmUnmeasured;

		int32 WarmPivots = WarmUnmeasured;
		int32 WarmPhaseOnePivots = WarmUnmeasured;
		int32 WarmAccepted = WarmUnmeasured;

		/**
		 * The warm attempt refused and the wrapper answered with a fresh cold solve, forcing
		 * Accepted to 0. Distinct from "never pulled" (hint refused or repaired away). Such a row
		 * asserts Accepted == 0 exactly and pins WarmWastedPivots.
		 */
		bool bWarmAbandoned = false;

		/**
		 * Warm minus cold pivots on an abandoned row: the work wasted before the refusal. Also
		 * checks the retry cost exactly the cold baseline, which the warm total alone cannot.
		 */
		int32 WarmWastedPivots = WarmUnmeasured;
	};

	/** Run one row end to end. Shared by both tests so the two tiers measure the same thing. */
	void WarmRunRow(FAutomationTestBase& Test, const FWarmRow& Row)
	{
		FStructure Structure;
		FString Why;

		// Separate statement: writing Why inside the Printf that reads it is unsequenced (TRAPS).
		const bool bLaid = Row.Build(Structure, Why);

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: the producer must lay it (it said: %s)"),
					Row.Name, *Why),
				bLaid))
		{
			return;
		}

		FOracleProblem Live;

		const bool bBridged = BuildRigidBlockProblem(Structure, Live, Why);

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: the bridge must represent it (it said: %s)"),
					Row.Name, *Why),
				bBridged))
		{
			return;
		}

		// Production's pose: gravity dead, feasibility at lambda = 1 (PROMOTION_DESIGN §3.2).
		FOracleProblem Before = Live;
		Before.bGravityIsLive = false;

		TArray<bool> bDoomed;
		WarmSelectDoomed(Before, Row.Delete, Row.Course, bDoomed);

		int32 DoomedBlocks = 0;

		for (const bool bIsDoomed : bDoomed)
		{
			DoomedBlocks += bIsDoomed ? 1 : 0;
		}

		const int32 DoomedJoints = WarmOrderDoomedJointsLast(Before, bDoomed);
		const FOracleProblem After = WarmApplyDeletion(Before, bDoomed, DoomedJoints);

		const FWarmReading BeforeRead = WarmSolve(Before);

		FOracleProblem AfterCold = After;
		const FWarmReading ColdRead = WarmSolve(AfterCold);

		FOracleProblem AfterWarm = After;
		AfterWarm.StartingBasis = WarmMapBasis(BeforeRead.FinalBasis, ColdRead.FinalBasis);
		const int32 Hints = WarmHintsIn(AfterWarm.StartingBasis);
		const FWarmReading WarmRead = WarmSolve(AfterWarm);

		const double PivotRatio = WarmRead.Pivots > 0
			? double(ColdRead.Pivots) / double(WarmRead.Pivots)
			: -1.0;
		const double SecondsRatio = WarmRead.Seconds > 0.0
			? ColdRead.Seconds / WarmRead.Seconds
			: -1.0;

		const FString Line = FString::Printf(
			TEXT("WARMSTART %s: blocks=%d joints=%d | deleted blocks=%d joints=%d ")
			TEXT("| BEFORE feasible=%d lambda=%.17g pivots=%d phase1=%d secs=%.3f ")
			TEXT("| AFTER COLD feasible=%d lambda=%.17g pivots=%d phase1=%d secs=%.3f ")
			TEXT("| AFTER WARM feasible=%d lambda=%.17g pivots=%d phase1=%d secs=%.3f ")
			TEXT("hints=%d accepted=%d ")
			TEXT("| RATIO pivots %.3f seconds %.3f | %s%s%s"),
			Row.Name, Live.Blocks.Num(), Live.Joints.Num(), DoomedBlocks, DoomedJoints,
			WarmIsFeasible(BeforeRead) ? 1 : 0, BeforeRead.Lambda, BeforeRead.Pivots,
			BeforeRead.PhaseOnePivots, BeforeRead.Seconds,
			WarmIsFeasible(ColdRead) ? 1 : 0, ColdRead.Lambda, ColdRead.Pivots,
			ColdRead.PhaseOnePivots, ColdRead.Seconds,
			WarmIsFeasible(WarmRead) ? 1 : 0, WarmRead.Lambda, WarmRead.Pivots,
			WarmRead.PhaseOnePivots, WarmRead.Seconds,
			Hints, WarmRead.Accepted,
			PivotRatio, SecondsRatio, Row.Prediction,
			WarmRead.WhyNot.IsEmpty() ? TEXT("") : TEXT(" | warm whynot: "),
			WarmRead.WhyNot.IsEmpty() ? TEXT("") : *WarmRead.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		Test.AddInfo(Line);

		// Size pins first, so a row cannot quietly measure a different wall.
		if (Row.Blocks == INDEX_NONE || Row.Joints == INDEX_NONE)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED SIZE - pin blocks=%d joints=%d"),
				Row.Name, Live.Blocks.Num(), Live.Joints.Num()));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: block count"), Row.Name),
				Live.Blocks.Num(), Row.Blocks);
			Test.TestEqual(*FString::Printf(TEXT("%s: joint count"), Row.Name),
				Live.Joints.Num(), Row.Joints);
		}

		if (Row.DoomedBlocks == INDEX_NONE || Row.DoomedJoints == INDEX_NONE)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED DELETION - pin blocks=%d joints=%d"),
				Row.Name, DoomedBlocks, DoomedJoints));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: blocks the deletion takes"), Row.Name),
				DoomedBlocks, Row.DoomedBlocks);
			Test.TestEqual(*FString::Printf(TEXT("%s: joints the deletion takes"), Row.Name),
				DoomedJoints, Row.DoomedJoints);
		}

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: the BEFORE solve must answer (it said: %s)"),
					Row.Name, *BeforeRead.WhyNot),
				BeforeRead.bAnswered))
		{
			return;
		}

		// With no live load lambda is exactly LambdaCap or 0; anything between means a live load leaked in.
		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: a DEAD pose carries no live load, so BEFORE lambda must be ")
				TEXT("exactly LambdaCap (%.17g) or exactly 0, and was %.17g"),
				Row.Name, LambdaCap, BeforeRead.Lambda),
			BeforeRead.Lambda == LambdaCap || BeforeRead.Lambda == 0.0);

		Test.TestTrue(
			*FString::Printf(TEXT("%s: the BEFORE verdict (feasible=%d, wanted %d)"),
				Row.Name, WarmIsFeasible(BeforeRead) ? 1 : 0, Row.bBeforeStands ? 1 : 0),
			WarmIsFeasible(BeforeRead) == Row.bBeforeStands);

		if (Row.BeforePivots == WarmUnmeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED BEFORE pivots - pin %d"), Row.Name, BeforeRead.Pivots));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: BEFORE pivots"), Row.Name),
				BeforeRead.Pivots, Row.BeforePivots);
		}

		/*
		 * The solve must return a basis, and its structural column count is checked against
		 * the documented layout so a misread shape fails here rather than mis-mapping below.
		 */
		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the BEFORE solve must report the basis it ended on (it reported ")
				TEXT("%d columns)"),
				Row.Name, BeforeRead.FinalBasis.Columns.Num()),
			BeforeRead.FinalBasis.Columns.Num() > 0);

		Test.TestEqual(
			*FString::Printf(TEXT("%s: the BEFORE basis names the right column layout"),
				Row.Name),
			BeforeRead.FinalBasis.NumStructCols, WarmStructuralColumnsFor(Before));

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the BEFORE basis's slack block is non-negative (structural %d, ")
				TEXT("artificials start %d)"),
				Row.Name, BeforeRead.FinalBasis.NumStructCols,
				BeforeRead.FinalBasis.ArtificialStart),
			BeforeRead.FinalBasis.ArtificialStart >= BeforeRead.FinalBasis.NumStructCols);

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: the COLD re-solve must answer (it said: %s)"),
					Row.Name, *ColdRead.WhyNot),
				ColdRead.bAnswered))
		{
			return;
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: COLD lambda must be exactly LambdaCap (%.17g) or exactly 0, and ")
				TEXT("was %.17g"),
				Row.Name, LambdaCap, ColdRead.Lambda),
			ColdRead.Lambda == LambdaCap || ColdRead.Lambda == 0.0);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the verdict after the deletion (feasible=%d, wanted %d)"),
				Row.Name, WarmIsFeasible(ColdRead) ? 1 : 0, Row.bAfterStands ? 1 : 0),
			WarmIsFeasible(ColdRead) == Row.bAfterStands);

		if (Row.ColdPivots == WarmUnmeasured || Row.ColdPhaseOnePivots == WarmUnmeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED COLD cost - pin pivots=%d phase1=%d"),
				Row.Name, ColdRead.Pivots, ColdRead.PhaseOnePivots));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: COLD pivots"), Row.Name),
				ColdRead.Pivots, Row.ColdPivots);
			Test.TestEqual(*FString::Printf(TEXT("%s: COLD phase-1 pivots"), Row.Name),
				ColdRead.PhaseOnePivots, Row.ColdPhaseOnePivots);
		}

		Test.TestEqual(
			*FString::Printf(TEXT("%s: the COLD re-solve names the right column layout"),
				Row.Name),
			ColdRead.FinalBasis.NumStructCols, WarmStructuralColumnsFor(After));

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: the WARM re-solve must answer (it said: %s)"),
					Row.Name, *WarmRead.WhyNot),
				WarmRead.bAnswered))
		{
			return;
		}

		/*
		 * A warm start may change the cost, never the answer. Exact equality is valid because
		 * the dead pose admits only two lambda values.
		 */
		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: WARM lambda must equal COLD lambda BIT FOR BIT (%.17g vs %.17g)"),
				Row.Name, WarmRead.Lambda, ColdRead.Lambda),
			WarmRead.Lambda == ColdRead.Lambda);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: WARM verdict must equal COLD verdict (%d vs %d)"),
				Row.Name, WarmIsFeasible(WarmRead) ? 1 : 0, WarmIsFeasible(ColdRead) ? 1 : 0),
			WarmIsFeasible(WarmRead) == WarmIsFeasible(ColdRead));

		/*
		 * The hint must have been taken; otherwise a ratio of 1.0 would read as a refutation
		 * rather than a lever never pulled. An abandoned row instead asserts exactly zero and
		 * pins the wasted pivots.
		 */
		if (Row.bWarmAbandoned)
		{
			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: the warm attempt was ABANDONED, so the solve must report ")
					TEXT("EXACTLY ZERO columns accepted - the answer came from a fresh cold ")
					TEXT("solve and reporting the columns the discarded attempt briefly held ")
					TEXT("would credit the lever with work it did not do (it said %d of %d ")
					TEXT("offered)"),
					Row.Name, WarmRead.Accepted, Hints),
				WarmRead.Accepted, 0);

			const int32 Wasted = WarmRead.Pivots - ColdRead.Pivots;

			if (Row.WarmWastedPivots == WarmUnmeasured)
			{
				Test.AddError(FString::Printf(
					TEXT("%s: UNMEASURED WASTED WORK - pin warm-minus-cold pivots=%d"),
					Row.Name, Wasted));
			}
			else
			{
				Test.TestEqual(
					*FString::Printf(
						TEXT("%s: the pivots the abandoned warm attempt burned before it ")
						TEXT("refused, which is warm total minus the cold baseline. Pinned as ")
						TEXT("the DIFFERENCE and not as the total because the difference says ")
						TEXT("two things at once: how far the mapped basis got, and that the ")
						TEXT("retry underneath it was a genuine cold solve costing exactly the ")
						TEXT("cold baseline"),
						Row.Name),
					Wasted, Row.WarmWastedPivots);
			}
		}
		else
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: the solve must report how much of the warm start it took (it ")
					TEXT("said %d of %d offered)"),
					Row.Name, WarmRead.Accepted, Hints),
				WarmRead.Accepted > 0);
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: it cannot accept more hints than were offered (%d of %d)"),
				Row.Name, WarmRead.Accepted, Hints),
			WarmRead.Accepted <= Hints);

		if (Row.WarmAccepted == WarmUnmeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED WARM acceptance - pin accepted=%d of %d offered"),
				Row.Name, WarmRead.Accepted, Hints));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: WARM columns accepted"), Row.Name),
				WarmRead.Accepted, Row.WarmAccepted);
		}

		/*
		 * Total and phase-1 pivots pinned separately: repairing an infeasible seed is phase-1
		 * work, and reporting phase 2 alone would hide it.
		 */
		if (Row.WarmPivots == WarmUnmeasured || Row.WarmPhaseOnePivots == WarmUnmeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED WARM cost - pin pivots=%d phase1=%d (cold was %d/%d, ")
				TEXT("so the lever reads %.3f on pivots)"),
				Row.Name, WarmRead.Pivots, WarmRead.PhaseOnePivots,
				ColdRead.Pivots, ColdRead.PhaseOnePivots, PivotRatio));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: WARM pivots"), Row.Name),
				WarmRead.Pivots, Row.WarmPivots);
			Test.TestEqual(*FString::Printf(TEXT("%s: WARM phase-1 pivots"), Row.Name),
				WarmRead.PhaseOnePivots, Row.WarmPhaseOnePivots);
		}
	}
}

/*
 * Fast tier. PART A: a solve's own final basis fed back must cost zero pivots. PART C: the one
 * gravity-live row, where lambda* is a real number. PART B: the measurement rows.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleWarmStartTest,
	"OracleSweepFast.RigidBlock.WarmStartAfterADeletion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleWarmStartTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleWarmStartSupport;

	// PART A: a solve's own basis is an optimal start.
	{
		FStructure Small;
		FString Why;

		const bool bLaid = WarmBuildIntactWall(4, 4, Small, Why);

		if (TestTrue(
				*FString::Printf(TEXT("seam fixture: the producer must lay it (it said: %s)"),
					*Why),
				bLaid))
		{
			FOracleProblem Problem;

			const bool bBridged = BuildRigidBlockProblem(Small, Problem, Why);

			if (TestTrue(
					*FString::Printf(
						TEXT("seam fixture: the bridge must represent it (it said: %s)"), *Why),
					bBridged))
			{
				Problem.bGravityIsLive = false;

				const FWarmReading Cold = WarmSolve(Problem);

				const FString Line = FString::Printf(
					TEXT("WARMSTART seam 4x4 wall: blocks=%d joints=%d | COLD lambda=%.17g ")
					TEXT("pivots=%d phase1=%d basis columns=%d structcols=%d ")
					TEXT("artificialstart=%d"),
					Problem.Blocks.Num(), Problem.Joints.Num(), Cold.Lambda, Cold.Pivots,
					Cold.PhaseOnePivots, Cold.FinalBasis.Columns.Num(),
					Cold.FinalBasis.NumStructCols, Cold.FinalBasis.ArtificialStart);

				UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
				AddInfo(Line);

				TestTrue(
					*FString::Printf(TEXT("seam: the cold solve must answer (it said: %s)"),
						*Cold.WhyNot),
					Cold.bAnswered);

				// Fixture guards, measured 2026-08-16 (TRAPS: only a count pin catches a changed fixture).
				TestEqual(TEXT("seam fixture: block count"), Problem.Blocks.Num(), 18);
				TestEqual(TEXT("seam fixture: joint count"), Problem.Joints.Num(), 35);
				TestEqual(TEXT("seam fixture: cold pivots"), Cold.Pivots, 48);
				TestEqual(TEXT("seam fixture: cold phase-1 pivots"), Cold.PhaseOnePivots, 47);

				// No warm start supplied: INDEX_NONE means "not asked", 0 means "asked, took nothing".
				TestEqual(
					TEXT("seam: a cold solve reports no warm-start acceptance at all"),
					Cold.Accepted, int32(INDEX_NONE));

				TestTrue(
					*FString::Printf(
						TEXT("seam: the solve must report the basis it ended on (it ")
						TEXT("reported %d columns)"),
						Cold.FinalBasis.Columns.Num()),
					Cold.FinalBasis.Columns.Num() > 0);

				TestEqual(
					TEXT("seam: the reported basis names the documented column layout"),
					Cold.FinalBasis.NumStructCols, WarmStructuralColumnsFor(Problem));

				// Idempotence: re-solving from its own optimal basis costs zero pivots, same lambda.
				FOracleProblem Again = Problem;
				Again.StartingBasis = Cold.FinalBasis;

				const FWarmReading Warm = WarmSolve(Again);

				const FString WarmLine = FString::Printf(
					TEXT("WARMSTART seam 4x4 wall RE-SOLVED FROM ITS OWN BASIS: ")
					TEXT("lambda=%.17g pivots=%d phase1=%d accepted=%d of %d"),
					Warm.Lambda, Warm.Pivots, Warm.PhaseOnePivots, Warm.Accepted,
					Cold.FinalBasis.Columns.Num());

				UE_LOG(LogTemp, Display, TEXT("%s"), *WarmLine);
				AddInfo(WarmLine);

				TestTrue(
					*FString::Printf(TEXT("seam: the warm solve must answer (it said: %s)"),
						*Warm.WhyNot),
					Warm.bAnswered);

				TestTrue(
					*FString::Printf(
						TEXT("seam: re-solving from its own basis must give the same lambda ")
						TEXT("BIT FOR BIT (%.17g vs %.17g)"),
						Warm.Lambda, Cold.Lambda),
					Warm.Lambda == Cold.Lambda);

				TestEqual(
					TEXT("seam: its own basis is entirely usable, so every column is taken"),
					Warm.Accepted, Cold.FinalBasis.Columns.Num());

				TestEqual(
					TEXT("seam: an optimal start costs no pivots at all"),
					Warm.Pivots, 0);

				TestEqual(
					TEXT("seam: and none of them in phase 1"),
					Warm.PhaseOnePivots, 0);
			}
		}
	}

	/*
	 * PART C: the one gravity-live row. In the dead pose lambda is only LambdaCap or 0, so the
	 * other rows' lambda identities are verdict comparisons and would miss a small perturbation.
	 * Here lambda* is a real number, which also exposes an appended negated column left basic at
	 * a non-zero value (invisible to the admissibility gate). Tolerance is the sweep's 1e-6
	 * relative, since two pivot paths agree to rounding, not necessarily to the bit.
	 */
	{
		FStructure Live;
		FString Why;

		const bool bLaid = WarmBuildIntactWall(4, 4, Live, Why);

		if (TestTrue(
				*FString::Printf(TEXT("live seam: the producer must lay it (it said: %s)"), *Why),
				bLaid))
		{
			FOracleProblem Intact;

			const bool bBridged = BuildRigidBlockProblem(Live, Intact, Why);

			if (TestTrue(
					*FString::Printf(
						TEXT("live seam: the bridge must represent it (it said: %s)"), *Why),
					bBridged))
			{
				Intact.bGravityIsLive = true;

				TArray<bool> bDoomed;
				WarmSelectDoomed(Intact, EWarmDelete::OneBrick, 1, bDoomed);

				int32 DoomedBlocks = 0;

				for (const bool bIsDoomed : bDoomed)
				{
					DoomedBlocks += bIsDoomed ? 1 : 0;
				}

				const int32 DoomedJoints = WarmOrderDoomedJointsLast(Intact, bDoomed);
				const FOracleProblem After = WarmApplyDeletion(Intact, bDoomed, DoomedJoints);

				const FWarmReading BeforeRead = WarmSolve(Intact);

				FOracleProblem AfterCold = After;
				const FWarmReading ColdRead = WarmSolve(AfterCold);

				FOracleProblem AfterWarm = After;
				AfterWarm.StartingBasis =
					WarmMapBasis(BeforeRead.FinalBasis, ColdRead.FinalBasis);
				const int32 Hints = WarmHintsIn(AfterWarm.StartingBasis);
				const FWarmReading WarmRead = WarmSolve(AfterWarm);

				const double Gap = FMath::Abs(WarmRead.Lambda - ColdRead.Lambda);
				const double Relative = ColdRead.Lambda != 0.0
					? Gap / FMath::Abs(ColdRead.Lambda)
					: Gap;

				const FString Line = FString::Printf(
					TEXT("WARMSTART LIVE seam 4x4 wall: blocks=%d joints=%d | deleted blocks=%d ")
					TEXT("joints=%d | BEFORE lambda*=%.17g pivots=%d | AFTER COLD lambda*=%.17g ")
					TEXT("pivots=%d phase1=%d | AFTER WARM lambda*=%.17g pivots=%d phase1=%d ")
					TEXT("hints=%d accepted=%d | lambda gap %.3g absolute, %.3g relative"),
					Intact.Blocks.Num(), Intact.Joints.Num(), DoomedBlocks, DoomedJoints,
					BeforeRead.Lambda, BeforeRead.Pivots,
					ColdRead.Lambda, ColdRead.Pivots, ColdRead.PhaseOnePivots,
					WarmRead.Lambda, WarmRead.Pivots, WarmRead.PhaseOnePivots,
					Hints, WarmRead.Accepted, Gap, Relative);

				UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
				AddInfo(Line);

				TestEqual(TEXT("live seam: block count"), Intact.Blocks.Num(), 18);
				TestEqual(TEXT("live seam: joint count"), Intact.Joints.Num(), 35);
				TestEqual(TEXT("live seam: blocks the deletion takes"), DoomedBlocks, 1);
				TestEqual(TEXT("live seam: joints the deletion takes"),
					DoomedJoints, WarmLiveDoomedJoints);

				TestTrue(
					*FString::Printf(
						TEXT("live seam: the BEFORE solve must answer (it said: %s)"),
						*BeforeRead.WhyNot),
					BeforeRead.bAnswered);

				TestTrue(
					*FString::Printf(
						TEXT("live seam: the COLD re-solve must answer (it said: %s)"),
						*ColdRead.WhyNot),
					ColdRead.bAnswered);

				TestTrue(
					*FString::Printf(
						TEXT("live seam: the WARM re-solve must answer (it said: %s)"),
						*WarmRead.WhyNot),
					WarmRead.bAnswered);

				// Precondition: lambda* strictly inside (0, cap), or this row is just another dead one.
				TestTrue(
					*FString::Printf(
						TEXT("live seam: lambda* must be a REAL NUMBER strictly inside ")
						TEXT("(0, LambdaCap=%.17g) - that is the whole reason this row exists, ")
						TEXT("and it read %.17g"),
						LambdaCap, ColdRead.Lambda),
					ColdRead.Lambda > 0.0 && ColdRead.Lambda < LambdaCap);

				// A warm start may take a different path, not reach a different optimum.
				TestTrue(
					*FString::Printf(
						TEXT("live seam, THE POINT: a warm start may change the PATH and not ")
						TEXT("the ANSWER - warm lambda* %.17g against cold %.17g is %.3g ")
						TEXT("relative, and must be within 1e-6"),
						WarmRead.Lambda, ColdRead.Lambda, Relative),
					Relative <= 1.0e-6);

				TestTrue(
					*FString::Printf(
						TEXT("live seam: and the hint must actually have been taken (it said ")
						TEXT("%d of %d offered)"),
						WarmRead.Accepted, Hints),
					WarmRead.Accepted > 0);

				// Cost pins: without them a discarded warm start would also pass the agreement above.
				if (WarmLiveColdPivots == WarmUnmeasured)
				{
					AddError(FString::Printf(
						TEXT("live seam: UNMEASURED - pin doomed joints=%d cold pivots=%d ")
						TEXT("warm pivots=%d accepted=%d, and a lambda* window around %.17g"),
						DoomedJoints, ColdRead.Pivots, WarmRead.Pivots, WarmRead.Accepted,
						ColdRead.Lambda));
				}
				else
				{
					TestEqual(TEXT("live seam: COLD pivots"),
						ColdRead.Pivots, WarmLiveColdPivots);
					TestEqual(TEXT("live seam: WARM pivots"),
						WarmRead.Pivots, WarmLiveWarmPivots);
					TestEqual(TEXT("live seam: WARM columns accepted"),
						WarmRead.Accepted, WarmLiveAccepted);

					TestTrue(
						*FString::Printf(
							TEXT("live seam: lambda* must lie in [%.9g, %.9g] and was %.17g"),
							WarmLiveLambdaLo, WarmLiveLambdaHi, ColdRead.Lambda),
						ColdRead.Lambda >= WarmLiveLambdaLo
							&& ColdRead.Lambda <= WarmLiveLambdaHi);
				}
			}
		}
	}

	/*
	 * PART B: the measurement. Each row keeps its prediction beside its pins (measured
	 * 2026-08-16) so agreement can be told from transcription.
	 */
	TArray<FWarmRow> Rows;

	Rows.Add({ TEXT("gate wall 8x10, ONE BRICK out of course 3"),
		TEXT("PREDICTED 1.6x-5x on pivots: (A) local repair says 5x, (B) thrust-is-not-local ")
		TEXT("says 1.6x. This is the row the gate arithmetic rests on - the cold re-solve is ")
		TEXT("343 pivots and ~51 ms against a 50 ms budget"),
		[](FStructure& Out, FString& Why) { return WarmBuildIntactWall(8, 10, Out, Why); },
		EWarmDelete::OneBrick, 3,
		/*Blocks*/ 84, /*Joints*/ 207, /*DoomedBlocks*/ 1, /*DoomedJoints*/ 6,
		/*bBeforeStands*/ true, /*BeforePivots*/ 436,
		/*bAfterStands*/ true, /*ColdPivots*/ 343, /*ColdPhaseOnePivots*/ 342,
		/*WarmPivots*/ 342, /*WarmPhaseOnePivots*/ 342, /*WarmAccepted*/ 2600 });

	Rows.Add({ TEXT("gate wall 8x10, the WHOLE TOP COURSE out"),
		TEXT("PREDICTED 1.2x-2.5x: eleven bricks is a large change, and this is also the ")
		TEXT("roadmap's neighbouring-ladder-rung case (an 8-course wall re-solved as a ")
		TEXT("7-course one) posed as a deletion so the basis can be mapped. Its BEFORE solve ")
		TEXT("is the reorder's own control: 491 pivots, bit-identical to the intact wall's ")
		TEXT("pinned dead-pose count, because this row's reorder is a no-op"),
		[](FStructure& Out, FString& Why) { return WarmBuildIntactWall(8, 10, Out, Why); },
		EWarmDelete::TopCourse, 0,
		/*Blocks*/ 84, /*Joints*/ 207, /*DoomedBlocks*/ 11, /*DoomedJoints*/ 30,
		/*bBeforeStands*/ true, /*BeforePivots*/ 491,
		/*bAfterStands*/ true, /*ColdPivots*/ 291, /*ColdPhaseOnePivots*/ 290,
		/*WarmPivots*/ 1543, /*WarmPhaseOnePivots*/ 1543, /*WarmAccepted*/ 2032 });

	Rows.Add({ TEXT("gate wall 8x10, the GROUND CUT - the verdict flips to falls"),
		TEXT("PREDICTED 0.7x-1.5x, and this is THE TRAP ROW: the warm basis describes a load ")
		TEXT("path that no longer exists, so a warm start may cost MORE than a cold one. It ")
		TEXT("is also the only row whose AFTER verdict is 'falls', which is what stops the ")
		TEXT("bit-identity assertion resting on a set of fixtures that all stand"),
		[](FStructure& Out, FString& Why) { return WarmBuildIntactWall(8, 10, Out, Why); },
		EWarmDelete::GroundCourse, 0,
		/*Blocks*/ 84, /*Joints*/ 207, /*DoomedBlocks*/ 10, /*DoomedJoints*/ 20,
		/*bBeforeStands*/ true, /*BeforePivots*/ 442,
		/*bAfterStands*/ false, /*ColdPivots*/ 90, /*ColdPhaseOnePivots*/ 90,
		/*WarmPivots*/ 77, /*WarmPhaseOnePivots*/ 77, /*WarmAccepted*/ 2441 });

	Rows.Add({ TEXT("12x12 wall, ONE BRICK out of course 6"),
		TEXT("PREDICTED 2.5x-6x. The wall the regional sandwich measured and the same course ")
		TEXT("it cut, so the two levers are priced on one fixture - intact here at 150 ")
		TEXT("blocks where the sandwich starts from the cut wall's 149"),
		[](FStructure& Out, FString& Why) { return WarmBuildIntactWall(12, 12, Out, Why); },
		EWarmDelete::OneBrick, 6,
		/*Blocks*/ 150, /*Joints*/ 391, /*DoomedBlocks*/ 1, /*DoomedJoints*/ 6,
		/*bBeforeStands*/ true, /*BeforePivots*/ 940,
		/*bAfterStands*/ true, /*ColdPivots*/ 919, /*ColdPhaseOnePivots*/ 918,
		/*WarmPivots*/ 4192, /*WarmPhaseOnePivots*/ 4192, /*WarmAccepted*/ 4560 });

	for (const FWarmRow& Row : Rows)
	{
		WarmRunRow(*this, Row);
	}

	return true;
}

/*
 * wall-01 (375 blocks), which answers feasibility cold in ~26 s. Full tier, since three solves
 * of this size would make the fast tier unusable for iteration.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleWarmStartAtWallScaleTest,
	"OracleSweepFull.RigidBlock.WarmStartAtWallScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleWarmStartAtWallScaleTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleWarmStartSupport;

	FWarmRow Row;
	Row.Name = TEXT("wall-01 thirty courses, ONE BRICK out of course 3");
	Row.Prediction =
		TEXT("PREDICTED 5x-18x on pivots against a cold 5,407 - the fixture that separates ")
		TEXT("(A) local repair from (B) global repair most sharply, because the change is ")
		TEXT("the same size as the gate wall's while the structure is 4.5x bigger");
	Row.Build = [](FStructure& Out, FString& Why)
	{
		return WarmBuildScenario(TEXT("wall-01"), Out, Why);
	};
	Row.Delete = EWarmDelete::OneBrick;
	Row.Course = 3;

	// Measured 2026-08-16: three solves, 24.5 + 15.5 + 15.6 s.
	Row.Blocks = 375;
	Row.Joints = 1030;
	Row.DoomedBlocks = 1;
	Row.DoomedJoints = 6;
	Row.bBeforeStands = true;
	Row.BeforePivots = 5643;
	Row.bAfterStands = true;
	Row.ColdPivots = 4764;
	Row.ColdPhaseOnePivots = 4763;

	/*
	 * This row abandons: 12,459 of 13,362 mapped columns were offered, the seeded basis went
	 * singular at the first periodic refactorisation (pivot 64), and the solver answered from a
	 * fresh cold solve (4,764, matching the baseline). The 64 is pinned because it is the
	 * evidence that the mapped basis is fragile at this scale.
	 */
	Row.WarmPivots = 4828;
	Row.WarmPhaseOnePivots = 4827;
	Row.WarmAccepted = 0;
	Row.bWarmAbandoned = true;
	Row.WarmWastedPivots = 64;

	WarmRunRow(*this, Row);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
