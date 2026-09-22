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
 * The fixture-catalogue sweep (DESIGN.md §7). Every affordable acceptance fixture is laid
 * through production's producers, projected through BuildRigidBlockProblem, solved by the
 * rigid-block LP, and diffed against the production cascade on the identical structure. Each
 * row pins lambda* (the load factor) and the agree/disagree classification, so a change on
 * either side fails loudly.
 *
 * A classification is not a verdict on which method is right: the oracle is finite-tension
 * rigid-plastic limit analysis at the coded strengths, production an uncracked-elastic
 * routing solver plus an overturning guard. lambda* runs high (plastic limit, up to 3x the
 * first-crack moment, plus full redistribution), and strengths are the profile's mean-basis
 * values post the 2026-08-14 re-anchor (TRAPS.md); the old /6 discount now lives in the data
 * and must not double up.
 *
 * Production's verdict is SolveAndBreak to a standstill; stranded pieces count as fallen but
 * are reported separately. The oracle's verdict is global: lambda* is one number for the
 * whole structure, so a local loss reads Falls like a collapse.
 *
 * Slow rows live in the opt-in OracleSweepFull tier; neither tier's name contains
 * "DestructionGame". Excluded as too large: 30-course walls (cases 1-5, ~375 pieces),
 * corbels E35/E36 and F. No ticking world needed. Named namespace because unity builds
 * merge translation units.
 */
namespace RigidBlockSweepTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	/* Brick and units, derived here and imported from nowhere: a wrong production constant
	 * must disagree with this file. */

	constexpr double SweepBrickLengthCm = 21.5;
	constexpr double SweepBrickWidthCm = 10.25;
	constexpr double SweepBrickHeightCm = 6.5;
	constexpr double SweepClayDensityGramsPerCubicCm = 1.9;
	constexpr double SweepJointCm = 1.0;
	constexpr double SweepCoursePitchCm = SweepBrickHeightCm + SweepJointCm;

	/** Density-first multiplication order (the PieceMassKg contract); 2.72163125 kg. */
	constexpr double SweepBrickMassKg = SweepClayDensityGramsPerCubicCm
		* SweepBrickLengthCm * SweepBrickWidthCm * SweepBrickHeightCm / 1000.0;

	/**
	 * Tolerance for "the LP prices two fixtures identically", three orders tighter than any
	 * row window. Measured 2026-08-12: both pairs agree to ~1 ulp (2.21e-16, 1.31e-16); 1e-15
	 * allows a different pivot path to the same optimum, not a different optimum.
	 */
	constexpr double SameNumberRelativeTolerance = 1.0e-15;

	/*
	 * Production's wall-16 / wall-15 worst-joint ratio, measured 4.5082330043756453 on the
	 * mean basis (was 31.5576 at characteristic data). Not strength-invariant: the bare side
	 * is tension-governed and moved /7, the loaded side is compression-governed and did not.
	 */
	constexpr double SuperimposedReadingRatioLo = 4.5081;
	constexpr double SuperimposedReadingRatioHi = 4.5084;

	/**
	 * Production's 20-course / 10-course free-end ratio, measured 2.11985065259119.
	 * Height-linear, a shade over 2 because the removed brick's own course does not double.
	 */
	constexpr double FreeEndReadingRatioLo = 2.119;
	constexpr double FreeEndReadingRatioHi = 2.121;

	/** How the two methods relate on one fixture. Unmeasured fails when asserted. */
	enum class ERelation : uint8
	{
		Unmeasured,

		/** lambda* >= 1 and the cascade dropped nothing. */
		AgreeStands,

		/** lambda* < 1 and the cascade dropped at least one piece. */
		AgreeFalls,

		/** The oracle finds an admissible equilibrium production's rules refuse. */
		OracleStandsProductionFalls,

		/** Production holds up a structure the limit theorem says has no equilibrium. */
		OracleFallsProductionStands,

		/**
		 * Post-solve verification fails at this scale. A canary: if the solver ever answers
		 * here, the row fails loudly and must be promoted to a measured relation, not
		 * absorbed. Unused today (the dense-era canary was promoted 2026-08-12); kept for
		 * future refusal pins.
		 */
		OracleRefusesAtThisScale,

		/**
		 * Phase-2 simplex fails, never reaching an answer to reject; a separate enumerator
		 * from the verification refusal above because the two mean opposite things about how
		 * far the solve got. Unused and currently unreachable (since Slice 0a, 2026-08-15, the
		 * lambda-cap tower terminates Optimal); kept for the next refusal.
		 */
		OracleRefusesPhaseTwo,
	};

	const TCHAR* RelationName(ERelation Relation)
	{
		switch (Relation)
		{
		case ERelation::AgreeStands:                 return TEXT("AGREE (stands)");
		case ERelation::AgreeFalls:                  return TEXT("AGREE (falls)");
		case ERelation::OracleStandsProductionFalls: return TEXT("ORACLE STANDS / PRODUCTION FALLS");
		case ERelation::OracleFallsProductionStands: return TEXT("ORACLE FALLS / PRODUCTION STANDS");
		case ERelation::OracleRefusesAtThisScale:    return TEXT("ORACLE REFUSES (at this scale)");
		case ERelation::OracleRefusesPhaseTwo:       return TEXT("ORACLE REFUSES (phase-2 simplex failed)");
		default:                                     return TEXT("UNMEASURED");
		}
	}

	struct FSweepRow
	{
		const TCHAR* Name = nullptr;

		/** The named mechanism behind the expected relation, printed on any failure. */
		const TCHAR* Mechanism = nullptr;

		/** Leaves the structure post-cut, pre-cascade. False is a fixture failure. */
		TFunction<bool(FStructure&, FString&)> Build;

		ERelation Expected = ERelation::Unmeasured;

		/**
		 * The lambda* pin, inclusive. The solver is deterministic, so these are tight windows
		 * around the measured value; a negative low bound means unmeasured and must fail.
		 */
		double LambdaLo = -1.0;
		double LambdaHi = -1.0;

		/** Production's drop count, pinned exactly (the DropsToday pattern). */
		int32 ProductionFallen = INDEX_NONE;

		/**
		 * Production's stranded count, pinned separately: a stranded piece is one the router
		 * could not route, not one the wall could not hold (DESIGN.md §5.1), so part of a
		 * "collapse" can be a routing limitation. INDEX_NONE leaves it unasserted.
		 */
		int32 ProductionStranded = INDEX_NONE;

		/**
		 * Pricing experiment: rewrite the oracle's problem between bridge and solve, returning
		 * how many joints it changed. Editing FOracleProblem::Joints[i].Strength moves one
		 * method while production reads a bit-identical control; editing FStructure would move
		 * both. The return count matters because "lambda* did not move" is a valid answer, so
		 * an override that silently touched zero joints must be distinguishable from a real
		 * negative result.
		 */
		TFunction<int32(FOracleProblem&)> AdjustProblem;

		/** How many joints the override must have changed. INDEX_NONE: unasserted. */
		int32 OverriddenJoints = INDEX_NONE;
	};

	struct FSweepReading
	{
		bool bBuilt = false;
		FString BuildWhy;

		bool bBridged = false;
		FString BridgeWhy;
		int32 Blocks = 0;
		int32 Joints = 0;

		FOracleResult Oracle;
		double OracleSeconds = 0.0;

		/** Joints the row's AdjustProblem rewrote; zero on every row that has none. */
		int32 JointsOverridden = 0;

		/** Production, on the identical structure: reading first, then the cascade. */
		double WorstUtilisation = 0.0;
		int32 Passes = 0;
		int32 Fallen = 0;
		int32 Stranded = 0;
	};

	/** Live pieces with no path to the earth after the cascade. Stranded counts. */
	void CountFallen(const FStructure& Structure, int32& OutFallen, int32& OutStranded)
	{
		OutFallen = 0;
		OutStranded = 0;

		for (int32 Piece = 0; Piece < Structure.NumPieces(); ++Piece)
		{
			if (Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				++OutFallen;
			}

			if (Support == EPieceSupport::Stranded)
			{
				++OutStranded;
			}
		}
	}

	/**
	 * Run one fixture both ways. The oracle goes first because BuildRigidBlockProblem reads a
	 * const structure and SolveAndBreak mutates it; both must judge the identical post-cut graph.
	 */
	void MeasureRow(const FSweepRow& Row, FSweepReading& Out)
	{
		FStructure Structure;

		Out.bBuilt = Row.Build(Structure, Out.BuildWhy);

		if (!Out.bBuilt)
		{
			return;
		}

		FOracleProblem Problem;
		Out.bBridged = BuildRigidBlockProblem(Structure, Problem, Out.BridgeWhy);

		if (Out.bBridged)
		{
			Out.Blocks = Problem.Blocks.Num();
			Out.Joints = Problem.Joints.Num();

			/*
			 * The pricing edit, after the block and joint counts are taken so those still
			 * describe the fixture: an override changes strengths, never geometry.
			 */
			if (Row.AdjustProblem)
			{
				Out.JointsOverridden = Row.AdjustProblem(Problem);
			}

			const double Started = FPlatformTime::Seconds();
			Out.Oracle = SolveRigidBlock(Problem);
			Out.OracleSeconds = FPlatformTime::Seconds() - Started;
		}

		/* The production reading (non-destructive), then the cascade to a standstill. */
		Structure.SolveLoads();

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const double Utilisation = Structure.GetConnectionUtilisation(Index);

			if (FMath::IsFinite(Utilisation) && Utilisation > Out.WorstUtilisation)
			{
				Out.WorstUtilisation = Utilisation;
			}
		}

		Out.Passes = Structure.SolveAndBreak();
		CountFallen(Structure, Out.Fallen, Out.Stranded);
	}

	/**
	 * One line per fixture, logged immediately as well as to the report: the slow sweep runs
	 * for minutes per row, and a measurement that only appears at the end is lost if the run is killed.
	 */
	void ReportRow(FAutomationTestBase& Test, const FSweepRow& Row, const FSweepReading& R)
	{
		/* 17 digits, not 9: the cross-row same-number pins assert agreement far tighter than
		 * any single row's window, so the log line must carry enough digits to re-derive by hand. */
		const FString Line = FString::Printf(
			TEXT("SWEEP %s: lambda=%.17g answered=%d pivots=%d secs=%.3f blocks=%d joints=%d ")
			TEXT("overridden=%d ")
			TEXT("| production worstU=%.17g passes=%d fallen=%d stranded=%d | expected %s%s%s"),
			Row.Name, R.Oracle.Lambda, R.Oracle.bAnswered ? 1 : 0, R.Oracle.SimplexIterations,
			R.OracleSeconds, R.Blocks, R.Joints, R.JointsOverridden,
			R.WorstUtilisation, R.Passes, R.Fallen, R.Stranded, RelationName(Row.Expected),
			R.Oracle.WhyNot.IsEmpty() ? TEXT("") : TEXT(" | whynot: "),
			R.Oracle.WhyNot.IsEmpty() ? TEXT("") : *R.Oracle.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		Test.AddInfo(Line);
	}

	/**
	 * Production's half of the diff, pinned exactly (the DropsToday pattern), in both the
	 * measured and the refusal branch so a refusal row watches production just as closely.
	 */
	void CheckProductionCounts(
		FAutomationTestBase& Test, const FSweepRow& Row, const FSweepReading& R)
	{
		if (Row.ProductionFallen != INDEX_NONE)
		{
			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: production's drop count is pinned at %d (the DropsToday ")
					TEXT("pattern) and was %d"),
					Row.Name, Row.ProductionFallen, R.Fallen),
				R.Fallen, Row.ProductionFallen);
		}

		if (Row.ProductionStranded != INDEX_NONE)
		{
			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: production STRANDED %d piece(s), pinned at %d — a stranded ")
					TEXT("piece is a routing limitation, not a wall that could not hold, so ")
					TEXT("this moving is a different finding from the drop count moving"),
					Row.Name, R.Stranded, Row.ProductionStranded),
				R.Stranded, Row.ProductionStranded);
		}
	}

	/** Every assertion a measured row makes; an unmeasured row is a loud red. */
	void CheckRow(FAutomationTestBase& Test, const FSweepRow& Row, const FSweepReading& R)
	{
		if (!R.bBuilt)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: FIXTURE could not be laid: %s"), Row.Name, *R.BuildWhy));

			return;
		}

		/* The bridge must represent every swept fixture; also the watch that no fixture
		 * smuggles in a Y-normal joint the 2D oracle would refuse. */
		if (!Test.TestTrue(
				*FString::Printf(
					TEXT("%s: the bridge must represent this fixture (it said: %s)"),
					Row.Name, *R.BridgeWhy),
				R.bBridged))
		{
			return;
		}

		/*
		 * A pricing experiment must have priced something. Checked ahead of the relation
		 * because a probe whose selector matched nothing reproduces the control's lambda* and
		 * reports "the strength does not bind" without having varied it; only this tells the two apart.
		 */
		if (Row.OverriddenJoints != INDEX_NONE)
		{
			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: the strength override must have rewritten %d joint(s) and ")
					TEXT("rewrote %d — an override that matched nothing measures nothing, and ")
					TEXT("looks exactly like a negative result"),
					Row.Name, Row.OverriddenJoints, R.JointsOverridden),
				R.JointsOverridden, Row.OverriddenJoints);
		}

		/*
		 * A pinned refusal asserts the refusal itself: not answered, for the recorded reason,
		 * with production's side still pinned. An answer here means the solver's envelope
		 * moved: promote the row, don't delete the failure. Unreachable today; kept for future pins.
		 */
		if (Row.Expected == ERelation::OracleRefusesAtThisScale)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: pinned as ORACLE REFUSES (mechanism: %s). It ANSWERED, ")
					TEXT("lambda* %.9g — the solver's envelope has moved; measure this row ")
					TEXT("and promote the pin to a relation, never delete this failure."),
					Row.Name, Row.Mechanism, R.Oracle.Lambda),
				!R.Oracle.bAnswered);

			if (!R.Oracle.bAnswered)
			{
				Test.TestTrue(
					*FString::Printf(
						TEXT("%s: the refusal must be the post-solve verification, and ")
						TEXT("was: %s"),
						Row.Name, *R.Oracle.WhyNot),
					R.Oracle.WhyNot.Contains(TEXT("failed verification")));
			}

			CheckProductionCounts(Test, Row, R);

			return;
		}

		/*
		 * Same canary shape for the other refusal, where the solver never reaches an answer to
		 * reject. A separate branch, not a widened substring test, because the two refusals mean
		 * opposite things about how far the solve got.
		 */
		if (Row.Expected == ERelation::OracleRefusesPhaseTwo)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: pinned as ORACLE REFUSES IN PHASE 2 (mechanism: %s). It ")
					TEXT("ANSWERED, lambda* %.9g — the solver's numerics have moved; measure ")
					TEXT("this row and promote the pin to a relation, never delete this ")
					TEXT("failure."),
					Row.Name, Row.Mechanism, R.Oracle.Lambda),
				!R.Oracle.bAnswered);

			if (!R.Oracle.bAnswered)
			{
				Test.TestTrue(
					*FString::Printf(
						TEXT("%s: the refusal must be the phase-2 simplex, and was: %s"),
						Row.Name, *R.Oracle.WhyNot),
					R.Oracle.WhyNot.Contains(TEXT("phase-2 simplex failed")));
			}

			CheckProductionCounts(Test, Row, R);

			return;
		}

		if (!Test.TestTrue(
				*FString::Printf(
					TEXT("%s: the oracle must answer (it said: %s)"),
					Row.Name, *R.Oracle.WhyNot),
				R.Oracle.bAnswered))
		{
			return;
		}

		if (Row.Expected == ERelation::Unmeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED — this row has no pinned relation yet. Measured: ")
				TEXT("lambda* %.17g, production dropped %d (stranded %d, worst reading %.17g). ")
				TEXT("Pin the relation, the lambda window and the drop count."),
				Row.Name, R.Oracle.Lambda, R.Fallen, R.Stranded, R.WorstUtilisation));

			return;
		}

		const EOracleOutcome Outcome = OutcomeOf(R.Oracle);

		const bool bOracleStands = Outcome == EOracleOutcome::Stands;
		const bool bProductionStands = R.Fallen == 0;

		bool bRelationHolds = false;

		switch (Row.Expected)
		{
		case ERelation::AgreeStands:
			bRelationHolds = bOracleStands && bProductionStands;
			break;
		case ERelation::AgreeFalls:
			bRelationHolds = !bOracleStands && !bProductionStands;
			break;
		case ERelation::OracleStandsProductionFalls:
			bRelationHolds = bOracleStands && !bProductionStands;
			break;
		case ERelation::OracleFallsProductionStands:
			bRelationHolds = !bOracleStands && bProductionStands;
			break;
		default:
			break;
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the pinned relation is %s (mechanism: %s); the oracle read ")
				TEXT("lambda* %.9g (%s) and production dropped %d piece(s). If one side ")
				TEXT("changed, re-derive the classification rather than flipping it."),
				Row.Name, RelationName(Row.Expected), Row.Mechanism,
				R.Oracle.Lambda, bOracleStands ? TEXT("stands") : TEXT("falls"), R.Fallen),
			bRelationHolds);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* must lie in [%.9g, %.9g] and was %.17g — the margin is ")
				TEXT("the measurement; a silent move is a change in one of the two models"),
				Row.Name, Row.LambdaLo, Row.LambdaHi, R.Oracle.Lambda),
			R.Oracle.Lambda >= Row.LambdaLo && R.Oracle.Lambda <= Row.LambdaHi);

		CheckProductionCounts(Test, Row, R);
	}

	/**
	 * Run every row, keeping the readings, because some findings live between two rows: two
	 * fixtures the LP prices identically while production separates them, or the reverse. A
	 * cross-row pin asserts two numbers are the same number, which a same-sized drift on both
	 * sides cannot satisfy.
	 */
	void RunRows(
		FAutomationTestBase& Test,
		const TArray<FSweepRow>& Rows,
		TArray<FSweepReading>& OutReadings)
	{
		OutReadings.Reset();
		OutReadings.SetNum(Rows.Num());

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			MeasureRow(Rows[Index], OutReadings[Index]);
			ReportRow(Test, Rows[Index], OutReadings[Index]);
			CheckRow(Test, Rows[Index], OutReadings[Index]);
		}
	}

	void RunRows(FAutomationTestBase& Test, const TArray<FSweepRow>& Rows)
	{
		TArray<FSweepReading> Unread;
		RunRows(Test, Rows, Unread);
	}

	/** A reading by row name; a cross-row pin names its rows rather than indexing them. */
	const FSweepReading* ReadingNamed(
		FAutomationTestBase& Test,
		const TArray<FSweepRow>& Rows,
		const TArray<FSweepReading>& Readings,
		const TCHAR* Name)
	{
		for (int32 Index = 0; Index < Rows.Num() && Index < Readings.Num(); ++Index)
		{
			if (FCString::Strcmp(Rows[Index].Name, Name) == 0)
			{
				return &Readings[Index];
			}
		}

		Test.AddError(FString::Printf(
			TEXT("cross-row pin: no row named '%s' ran — a renamed row must take its ")
			TEXT("cross-row pins with it, never lose them silently"),
			Name));

		return nullptr;
	}

	/**
	 * The same-number pin: two fixtures whose lambda* the LP reads identically, at a tolerance
	 * orders tighter than either row's window. The identity is the finding, not the magnitude.
	 */
	void CheckSameLambda(
		FAutomationTestBase& Test,
		const TCHAR* What,
		const FSweepReading& First,
		const FSweepReading& Second,
		double RelativeTolerance)
	{
		const double Difference = FMath::Abs(First.Oracle.Lambda - Second.Oracle.Lambda);
		const double Scale = FMath::Max(FMath::Abs(First.Oracle.Lambda), 1.0);
		const double Relative = Difference / Scale;

		Test.AddInfo(FString::Printf(
			TEXT("SAME-NUMBER %s: %.17g vs %.17g, relative difference %.3g (tolerance %.3g)"),
			What, First.Oracle.Lambda, Second.Oracle.Lambda, Relative, RelativeTolerance));

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the two lambda* must be THE SAME NUMBER to %.3g relative and read ")
				TEXT("%.17g vs %.17g (relative difference %.3g). This is a tighter claim than ")
				TEXT("either row's window on purpose — if one side moved, the identity is the ")
				TEXT("finding that broke, so re-derive it rather than widening the tolerance."),
				What, RelativeTolerance, First.Oracle.Lambda, Second.Oracle.Lambda, Relative),
			Relative <= RelativeTolerance);
	}

	/**
	 * The other half of a pair pin: production's readings on the same two fixtures must stay
	 * apart by the measured ratio. Stops a same-number pin passing vacuously, since two rows
	 * accidentally built from one fixture would read identical lambda* and utilisations.
	 */
	void CheckReadingRatio(
		FAutomationTestBase& Test,
		const TCHAR* What,
		double Numerator,
		double Denominator,
		double RatioLo,
		double RatioHi)
	{
		const double Ratio = Denominator != 0.0
			? Numerator / Denominator
			: TNumericLimits<double>::Max();

		Test.AddInfo(FString::Printf(
			TEXT("READING RATIO %s: %.17g / %.17g = %.17g"),
			What, Numerator, Denominator, Ratio));

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: production's two readings must stay apart by a ratio in [%.9g, ")
				TEXT("%.9g] and read %.17g / %.17g = %.17g — this pins the discrimination ")
				TEXT("production DOES make, and refuses a pair that quietly became one fixture"),
				What, RatioLo, RatioHi, Numerator, Denominator, Ratio),
			Ratio >= RatioLo && Ratio <= RatioHi);
	}

	/* Fixture builders. Producers are production's; hand-laid geometry is transcribed from the
	 * acceptance fixture it mirrors, with preconditions pinning the shape. */

	/** Lay a scenario row's structure and apply its cut; production data end to end. */
	bool BuildScenarioStructure(const TCHAR* ScenarioName, FStructure& Out, FString& OutWhy)
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

	/**
	 * The leaning-stack acceptance fixture, transcribed from LeaningStackAcceptanceTest:
	 * course i at (10*i, 0, 3.25 + 7.5*i), base grounded, mortared through MakeInterface
	 * with a 1 cm bed. The slice-1 bridge test proved this layout and the hand-built oracle
	 * problem are the same problem bit for bit.
	 */
	bool BuildLeaningStack(int32 Courses, FStructure& Out, FString& OutWhy)
	{
		TArray<FPieceBox> Boxes;

		for (int32 Course = 0; Course < Courses; ++Course)
		{
			FPieceBox Box;
			Box.ExtentCm = FVector(SweepBrickLengthCm, SweepBrickWidthCm, SweepBrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				double(Course) * 10.0, 0.0,
				SweepBrickHeightCm / 2.0 + double(Course) * SweepCoursePitchCm);

			Out.AddPiece(SweepBrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			Boxes.Add(Box);
		}

		for (int32 First = 0; First < Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(First, Boxes[First], Second, Boxes[Second],
						SweepJointCm, GeneralPurposeMortar, Joint))
				{
					Out.AddConnection(Joint);
				}
			}
		}

		if (Out.NumConnections() != Courses - 1)
		{
			OutWhy = FString::Printf(
				TEXT("stack fixture emitted %d joints for %d courses"),
				Out.NumConnections(), Courses);
			return false;
		}

		return true;
	}

	/* --- The beam pair, transcribed from BeamAcceptanceTest. ------------------------- */

	constexpr double BeamSectionCm = 10.0;
	constexpr double BeamSegmentLengthCm = 220.0;
	constexpr double BeamBearingLengthCm = 40.0;
	constexpr double BeamPierLengthCm = 60.0;
	constexpr double BeamPierHeightCm = 40.0;
	constexpr double BeamBlockLengthCm = 200.0;

	constexpr double BeamC24DensityGramsPerCubicCm = 0.42;

	/*
	 * Mean basis, re-transcribed 2026-08-14 in step with BeamAcceptanceTest's 2026-08-13
	 * move (JCSS x1.50 on EN 338's f_m,k and f_v,k; JCSS Table A static yield for S275).
	 * Characteristic strengths would measure a different fixture from the catalogue row.
	 */
	constexpr double BeamC24BendingMPa = 36.0;
	constexpr double BeamC24ShearMPa = 6.0;
	constexpr double BeamS275YieldMPa = 290.0;
	constexpr double BeamSteelDensityGramsPerCubicCm = 7.85;
	constexpr double BeamConcreteDensityGramsPerCubicCm = 2.4;

	/** The glue line's profile: member strengths, mu exactly 0 (a section, not an interface). */
	FConnectionStrength BeamMemberStrength(double BendingMPa, double ShearMPa)
	{
		FConnectionStrength Strength;
		Strength.CompressiveStrengthMPa = BendingMPa;
		Strength.ShearCohesionMPa = ShearMPa;
		Strength.TensileStrengthMPa = BendingMPa;
		Strength.FrictionCoefficient = 0.0;

		return Strength;
	}

	bool BuildBeam(
		double MemberDensity, double MemberBendingMPa, double MemberShearMPa,
		double BlockHeightCm, FStructure& Out, FString& OutWhy)
	{
		const double BeamCentreZCm = BeamPierHeightCm + BeamSectionCm / 2.0;
		const double BeamTopZCm = BeamPierHeightCm + BeamSectionCm;
		const double PierInnerCm = BeamSegmentLengthCm - BeamBearingLengthCm;
		const double PierCentreCm = PierInnerCm + BeamPierLengthCm / 2.0;

		TArray<FPieceBox> Boxes;

		const auto AddBox = [&Out, &Boxes](const FPieceBox& Box, double Density, bool bGrounded)
		{
			const double MassKg = Density
				* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0)
				/ 1000.0;

			const int32 Handle = Out.AddPiece(MassKg, bGrounded, Box.CentreCm);
			Boxes.Add(Box);

			return Handle;
		};

		FPieceBox Pier;
		Pier.ExtentCm = FVector(BeamPierLengthCm, BeamSectionCm, BeamPierHeightCm) * 0.5;

		Pier.CentreCm = FVector(-PierCentreCm, 0.0, BeamPierHeightCm / 2.0);
		AddBox(Pier, BeamConcreteDensityGramsPerCubicCm, true);

		Pier.CentreCm = FVector(PierCentreCm, 0.0, BeamPierHeightCm / 2.0);
		AddBox(Pier, BeamConcreteDensityGramsPerCubicCm, true);

		FPieceBox Segment;
		Segment.ExtentCm = FVector(BeamSegmentLengthCm, BeamSectionCm, BeamSectionCm) * 0.5;

		Segment.CentreCm = FVector(-BeamSegmentLengthCm / 2.0, 0.0, BeamCentreZCm);
		const int32 LeftSegment = AddBox(Segment, MemberDensity, false);

		Segment.CentreCm = FVector(BeamSegmentLengthCm / 2.0, 0.0, BeamCentreZCm);
		const int32 RightSegment = AddBox(Segment, MemberDensity, false);

		FPieceBox Block;
		Block.ExtentCm = FVector(BeamBlockLengthCm, BeamSectionCm, BlockHeightCm) * 0.5;
		Block.CentreCm = FVector(0.0, 0.0, BeamTopZCm + BlockHeightCm / 2.0);
		AddBox(Block, BeamSteelDensityGramsPerCubicCm, false);

		const FConnectionStrength Member =
			BeamMemberStrength(MemberBendingMPa, MemberShearMPa);

		for (int32 First = 0; First < Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Boxes.Num(); ++Second)
			{
				const bool bIsTheMember =
					(First == LeftSegment && Second == RightSegment);

				FConnection Joint;

				if (MakeInterface(First, Boxes[First], Second, Boxes[Second],
						/*JointThicknessCm*/ 0.0, bIsTheMember ? Member : DryStone, Joint))
				{
					Out.AddConnection(Joint);
				}
			}
		}

		if (Out.NumConnections() != 5)
		{
			OutWhy = FString::Printf(
				TEXT("beam fixture emitted %d joints, wants the acceptance file's 5"),
				Out.NumConnections());
			return false;
		}

		return true;
	}

	/**
	 * The one-cell dry jamming pair, transcribed from the slice-1 bridge test: grounded
	 * seat G, half-seated P (centroid 5.625 cm outboard of its 10.25 cm seat), abutting
	 * neighbour N on its own grounded G2, everything dry stone.
	 */
	bool BuildOneCellDryPair(FStructure& Out, FString& OutWhy)
	{
		const FVector Extent =
			FVector(SweepBrickLengthCm, SweepBrickWidthCm, SweepBrickHeightCm) * 0.5;

		const FVector Centres[] = {
			FVector(-0.5, 0.0, SweepBrickHeightCm / 2.0),
			FVector(10.75, 0.0, SweepBrickHeightCm / 2.0 + SweepCoursePitchCm),
			FVector(33.25, 0.0, SweepBrickHeightCm / 2.0 + SweepCoursePitchCm),
			FVector(33.25, 0.0, SweepBrickHeightCm / 2.0),
		};
		const bool Grounded[] = { true, false, false, true };

		TArray<FPieceBox> Boxes;

		for (int32 Piece = 0; Piece < 4; ++Piece)
		{
			FPieceBox Box;
			Box.ExtentCm = Extent;
			Box.CentreCm = Centres[Piece];

			Out.AddPiece(SweepBrickMassKg, Grounded[Piece], Box.CentreCm);
			Boxes.Add(Box);
		}

		for (int32 First = 0; First < Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(First, Boxes[First], Second, Boxes[Second],
						1.0, DryStone, Joint))
				{
					Out.AddConnection(Joint);
				}
			}
		}

		if (Out.NumConnections() != 3)
		{
			OutWhy = FString::Printf(
				TEXT("one-cell fixture emitted %d joints, wants 3"), Out.NumConnections());
			return false;
		}

		return true;
	}

	/**
	 * The free-end family: a 7-cell running-bond wall of N courses through the acceptance-wall
	 * producer, outermost full brick of the grounded course removed. StructureFreeEndHeightTest's fixture.
	 */
	bool BuildFreeEnd(int32 Courses, FStructure& Out, FString& OutWhy)
	{
		using namespace DestructionWallCases;

		FWallSpec Spec;
		Spec.BrickSizeCm = FVector(SweepBrickLengthCm, SweepBrickWidthCm, SweepBrickHeightCm);
		Spec.JointThicknessCm = SweepJointCm;
		Spec.DensityGramsPerCubicCm = SweepClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = Courses;
		Spec.Cells = 7;
		Spec.Bond = EWallBond::Running;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		FWallLayout Laid;

		if (!Build(Spec, Laid))
		{
			OutWhy = TEXT("the wall producer refused the free-end spec");
			return false;
		}

		/* The cell-0 brick of course 0: centre x = 0 exactly, the wall's free left end. */
		const FWallRegion Cut[] = { { 0, 0, -0.25, 0.25 } };

		TArray<int32> CutPieces;
		PiecesInRegions(Laid, Cut, CutPieces);

		if (CutPieces.Num() != 1)
		{
			OutWhy = FString::Printf(
				TEXT("the free-end cut named %d bricks, wants exactly 1"), CutPieces.Num());
			return false;
		}

		if (!Laid.Layout.Structure.RemovePiece(CutPieces[0]))
		{
			OutWhy = TEXT("the free-end brick could not be removed");
			return false;
		}

		Out = MoveTemp(Laid.Layout.Structure);
		return true;
	}

	/* --- The case-21 opening family, parameterised for the two mechanism ladders. ----- */

	/* No OpeningCells constant: the matched-span experiment made opening width a rung
	 * parameter, so every rung passes its own count and case 21's 18 lives at the call sites. */

	/** Courses the opening is cut through. Held at case 21's three. */
	constexpr int32 LadderOpeningCourses = 3;

	/** Courses of masonry over the opening. Held at the user's two-course minimum. */
	constexpr int32 LadderCoverCourses = 2;

	/**
	 * Z of the cover's underside for a rung with this much masonry below the opening. Below
	 * this line every bed joint is a jamb bed joint (the opening is a hole, so between the
	 * reveals there is nothing to bed); above it the cover is continuous. So "bed joint below
	 * the cover" means "jamb bed joint", derived from geometry rather than a per-jamb-width X
	 * window. Clearance is 0.5 cm below and 7 above, so it never clips a joint it must separate.
	 */
	constexpr double LadderCoverUndersideZCm(int32 CoursesBelow)
	{
		return double(CoursesBelow + LadderOpeningCourses) * SweepCoursePitchCm;
	}

	/** A joint whose normal is Z is a bed joint; one whose normal is X is a head joint. */
	bool IsBedJoint(const FOracleJoint& Joint)
	{
		return FMath::Abs(Joint.NormalZ) > 0.5;
	}

	bool IsHeadJoint(const FOracleJoint& Joint)
	{
		return FMath::Abs(Joint.NormalX) > 0.5;
	}

	/**
	 * Experiment one: zero the cohesion in the jamb's bed joints, leaving friction, tension and
	 * every other joint as the bridge built them. The case-21 hypothesis is that the LP bleeds
	 * its thrust into the jamb bed joints as Coulomb shear down to ground; cohesion dominates
	 * that capacity (0.2 MPa vs mu x precompression ~0.6 x 0.018 MPa, ~18x), so if the chain
	 * binds, removing cohesion removes the mechanism.
	 *
	 * @return how many joints were rewritten, pinned by the row.
	 */
	int32 ZeroJambBedCohesion(int32 CoursesBelow, FOracleProblem& Problem)
	{
		int32 Touched = 0;

		for (FOracleJoint& Joint : Problem.Joints)
		{
			if (IsBedJoint(Joint) && Joint.CentreZCm < LadderCoverUndersideZCm(CoursesBelow))
			{
				Joint.Strength.ShearCohesionMPa = 0.0;
				++Touched;
			}
		}

		return Touched;
	}

	/**
	 * Second probe: scale the tensile strength of the cover's head joints only. A cover
	 * spanning as a deep bending panel carries bending as horizontal tension across the head
	 * joints, so its flexural capacity is linear in this number; head-joint compression (what
	 * an arch uses) is left untouched, separating the two mechanisms. Prediction: it will not
	 * track (5.511 is 66x what a tension-bond panel holds here), a negative result the
	 * joint-count pin keeps distinct from an override matching nothing.
	 */
	int32 ScaleCoverHeadTension(int32 CoursesBelow, double Factor, FOracleProblem& Problem)
	{
		int32 Touched = 0;

		for (FOracleJoint& Joint : Problem.Joints)
		{
			if (IsHeadJoint(Joint) && Joint.CentreZCm > LadderCoverUndersideZCm(CoursesBelow))
			{
				Joint.Strength.TensileStrengthMPa *= Factor;
				++Touched;
			}
		}

		return Touched;
	}

	/**
	 * Slice 0c's crushing-relax value. Relaxing the crushing cap means raising the compressive
	 * limit, not zeroing it (zero would forbid the joint carrying compression and collapse the
	 * wall for a non-crushing reason). 1e6 MPa is 1e5x the profile's 10 MPa, enough to make the
	 * crushing row non-binding without a NaN or infinity the oracle would refuse.
	 */
	constexpr double ResidualUncappedCrushingMPa = 1.0e6;

	/**
	 * Height line splitting the jamb chain into its top two bearing courses and its run to
	 * ground. For case 21 the cover underside is at 4x pitch (CoursesBelow=1,
	 * LadderOpeningCourses=3); two pitches up puts the bearing courses on one side and the run
	 * to ground on the other, with a full course of clearance either side.
	 */
	constexpr double LadderChainBearingSplitZCm(int32 CoursesBelow)
	{
		return double(CoursesBelow + LadderOpeningCourses - 2) * SweepCoursePitchCm;
	}

	/**
	 * Slice 0c's residual-attribution knob: from case 21's residual state (jamb bed cohesion
	 * gone, lambda* 4.768), remove one more mechanism from a chosen band of the jamb chain and
	 * re-solve to see whether it held the residual up. Same per-joint override shape as
	 * ZeroJambBedCohesion (production reads the untouched wall as control), extended two ways:
	 * which mechanism (cohesion, friction, the crushing cap relaxed per ResidualUncappedCrushingMPa,
	 * the flexural tensile bond zeroed) in any combination, and where (a Z band [BandLo, BandHi)),
	 * so the chain-vs-bearing split is one row of this shape.
	 *
	 * @return how many jamb bed joints were rewritten, pinned by the row (a band matching
	 *         nothing measures nothing).
	 */
	int32 AdjustJambBedResidual(
		int32 CoursesBelow,
		bool bZeroCohesion,
		bool bZeroFriction,
		bool bRelaxCrushing,
		bool bZeroTension,
		double BandLoZCm,
		double BandHiZCm,
		FOracleProblem& Problem)
	{
		int32 Touched = 0;

		for (FOracleJoint& Joint : Problem.Joints)
		{
			if (IsBedJoint(Joint)
				&& Joint.CentreZCm < LadderCoverUndersideZCm(CoursesBelow)
				&& Joint.CentreZCm >= BandLoZCm
				&& Joint.CentreZCm < BandHiZCm)
			{
				if (bZeroCohesion)
				{
					Joint.Strength.ShearCohesionMPa = 0.0;
				}

				if (bZeroFriction)
				{
					Joint.Strength.FrictionCoefficient = 0.0;
				}

				if (bRelaxCrushing)
				{
					Joint.Strength.CompressiveStrengthMPa = ResidualUncappedCrushingMPa;
				}

				if (bZeroTension)
				{
					Joint.Strength.TensileStrengthMPa = 0.0;
				}

				++Touched;
			}
		}

		return Touched;
	}

	/**
	 * One rung of either ladder: case 21's wall with exactly one dimension moved. `CoursesBelow`
	 * is the masonry under the opening (rise-ladder variable, case 21 = 1); `JambCells` the
	 * masonry either side (abutment-ladder variable, case 21 = 2); `OpeningCells` the cut's width
	 * in bricks (matched-span variable, case 21 = 18). `CoverTailCells` is the disambiguating
	 * rung's variable, INDEX_NONE elsewhere (see the trim block below).
	 *
	 * The reveal the cover bears on is not OpeningCells x cell pitch: an even course loses
	 * OpeningCells whole bricks, an odd one loses one fewer and bears a cell narrower. The top
	 * opening course is what the cover sits on, so an 18-cell odd-top cut and a 17-cell even-top
	 * cut present the same 383.50 cm on opposite bond parities.
	 *
	 * Production's own bricklayer driven by this file's constants; the s=1/j=2 rung is case 21,
	 * and the ladder test pins it equal to the Scenario("wall-21") row, so the arithmetic here
	 * is checked against the shipped catalogue rather than trusted.
	 *
	 * The cut, in (course, cell) vocabulary (piece in when its course is within the inclusive
	 * range and its cell strictly between the bounds): courses s..s+2, cells j..j+OpeningCells-1
	 * of the even courses. At 18 cells, s=1, j=2 that is case 21's { 1, 3, 1.75, 19.25 }.
	 *
	 * Parity caveat: running bond makes an even course lose OpeningCells bricks and an odd one
	 * one fewer, so an opening starting odd takes 17+18+17=52 bricks and one starting even
	 * 18+17+18=53. Real and small; a ladder driven by this would alternate, one driven by the
	 * intended variable moves by a factor.
	 */
	bool BuildOpeningLadderWall(
		int32 OpeningCells, int32 CoursesBelow, int32 JambCells, int32 CoverTailCells,
		FStructure& Out, FString& OutWhy)
	{
		if (CoursesBelow < 1 || JambCells < 1 || OpeningCells < 2)
		{
			OutWhy = FString::Printf(
				TEXT("a rung needs at least one course below the opening, one cell of jamb ")
				TEXT("and two cells of opening; asked for %d, %d and %d"),
				CoursesBelow, JambCells, OpeningCells);

			return false;
		}

		DestructionWallCases::FWallSpec Spec;
		Spec.BrickSizeCm = FVector(SweepBrickLengthCm, SweepBrickWidthCm, SweepBrickHeightCm);
		Spec.JointThicknessCm = SweepJointCm;
		Spec.DensityGramsPerCubicCm = SweepClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = CoursesBelow + LadderOpeningCourses + LadderCoverCourses;
		Spec.Cells = OpeningCells + 2 * JambCells;
		Spec.Bond = DestructionWallCases::EWallBond::Running;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		DestructionWallCases::FWallLayout Laid;

		if (!DestructionWallCases::Build(Spec, Laid))
		{
			OutWhy = FString::Printf(
				TEXT("the wall producer refused a %d-course, %d-cell ladder rung"),
				Spec.CoursesHigh, Spec.Cells);

			return false;
		}

		TArray<DestructionWallCases::FWallRegion> Cut;

		Cut.Add({ CoursesBelow,
			CoursesBelow + LadderOpeningCourses - 1,
			double(JambCells) - 0.25,
			double(JambCells + OpeningCells - 1) + 0.25 });

		/*
		 * The cover tail, used only by the disambiguating rung. Widening the jamb widens both
		 * the bearing under the cover and the cover length over it, so the abutment ladder alone
		 * cannot say which bought the margin. Trimming the cover to a two-cell tail over a
		 * three-cell jamb separates them: the reaction keeps the wider bearing, the panel loses
		 * the tail. The trim is symmetric but not exactly one cell (odd cover courses close with
		 * half bats), supporting "overhangs by about two cells instead of three", not an exact tail.
		 */
		if (CoverTailCells > 0 && CoverTailCells < JambCells)
		{
			const int32 CoverLo = CoursesBelow + LadderOpeningCourses;
			const int32 CoverHi = CoverLo + LadderCoverCourses - 1;
			const int32 TrimCells = JambCells - CoverTailCells;

			Cut.Add({ CoverLo, CoverHi, -1.0, double(TrimCells) - 0.25 });
			Cut.Add({ CoverLo, CoverHi,
				double(Spec.Cells - TrimCells) - 0.75, double(Spec.Cells) + 1.0 });
		}

		TArray<int32> CutPieces;
		DestructionWallCases::PiecesInRegions(Laid, Cut, CutPieces);

		/* A rung's cut is 52 or 53 bricks (parity note above); anything else means the region
		 * arithmetic and the bricklayer have stopped agreeing. */
		const int32 WholeCourses = LadderOpeningCourses / 2 + 1;
		const int32 HalfCourses = LadderOpeningCourses - WholeCourses;

		const int32 EvenFirst = OpeningCells * WholeCourses
			+ (OpeningCells - 1) * HalfCourses;
		const int32 OddFirst = OpeningCells * HalfCourses
			+ (OpeningCells - 1) * WholeCourses;

		int32 Wanted = (CoursesBelow % 2 == 0) ? EvenFirst : OddFirst;

		/* Trim: the two cover courses are one even and one odd, so each end loses TrimCells off
		 * the even course and TrimCells + 1 off the odd (the half bat). */
		if (CoverTailCells > 0 && CoverTailCells < JambCells)
		{
			Wanted += 2 * (2 * (JambCells - CoverTailCells) + 1);
		}

		if (CutPieces.Num() != Wanted)
		{
			OutWhy = FString::Printf(
				TEXT("the rung's cut named %d bricks; a %s-course opening of %d cells over ")
				TEXT("%d courses (cover tail %d) wants %d"),
				CutPieces.Num(), (CoursesBelow % 2 == 0) ? TEXT("even") : TEXT("odd"),
				OpeningCells, LadderOpeningCourses, CoverTailCells, Wanted);

			return false;
		}

		for (const int32 Piece : CutPieces)
		{
			if (!Laid.Layout.Structure.RemovePiece(Piece))
			{
				OutWhy = FString::Printf(
					TEXT("the rung's cut piece %d could not be removed"), Piece);

				return false;
			}
		}

		Out = MoveTemp(Laid.Layout.Structure);
		return true;
	}

	/**
	 * The eight-course-cover family: acceptance case 22's shape, parameterised on width. A
	 * separate builder from BuildOpeningLadderWall because LadderCoverCourses is a file constant
	 * read by eleven pinned rungs; this family varies exactly the courses of cover, and its cut
	 * is one course band whatever the width.
	 *
	 * Case 22's shape, derived here: one grounded course, a three-course opening cut through
	 * courses 1..3, jambs of JambCells either side, CoverCourses of masonry over it, so
	 * CoursesHigh = 1 + 3 + CoverCourses and Cells = OpeningCells + 2*JambCells. At eight cover
	 * courses and a 35-cell opening between two-cell jambs that is case 22's { 1, 3, 1.75, 36.25 }.
	 *
	 * Cut-count pin (parity check): the cut starts at course 1 (odd), so it loses
	 * (OpeningCells-1) + OpeningCells + (OpeningCells-1) bricks; anything else means the builder
	 * and bricklayer disagree.
	 */
	bool BuildCoveredOpeningWall(
		int32 CoverCourses, int32 OpeningCells, int32 JambCells,
		FStructure& Out, FString& OutWhy)
	{
		if (CoverCourses < 1 || JambCells < 1 || OpeningCells < 2)
		{
			OutWhy = FString::Printf(
				TEXT("this family needs at least one course of cover, one cell of jamb and ")
				TEXT("two cells of opening; asked for %d, %d and %d"),
				CoverCourses, JambCells, OpeningCells);

			return false;
		}

		DestructionWallCases::FWallSpec Spec;
		Spec.BrickSizeCm = FVector(SweepBrickLengthCm, SweepBrickWidthCm, SweepBrickHeightCm);
		Spec.JointThicknessCm = SweepJointCm;
		Spec.DensityGramsPerCubicCm = SweepClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = 1 + LadderOpeningCourses + CoverCourses;
		Spec.Cells = OpeningCells + 2 * JambCells;
		Spec.Bond = DestructionWallCases::EWallBond::Running;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		DestructionWallCases::FWallLayout Laid;

		if (!DestructionWallCases::Build(Spec, Laid))
		{
			OutWhy = FString::Printf(
				TEXT("the wall producer refused a %d-course, %d-cell wall"),
				Spec.CoursesHigh, Spec.Cells);

			return false;
		}

		TArray<DestructionWallCases::FWallRegion> Cut;

		Cut.Add({ 1, LadderOpeningCourses,
			double(JambCells) - 0.25,
			double(JambCells + OpeningCells - 1) + 0.25 });

		TArray<int32> CutPieces;
		DestructionWallCases::PiecesInRegions(Laid, Cut, CutPieces);

		const int32 Wanted = 3 * OpeningCells - 2;

		if (CutPieces.Num() != Wanted)
		{
			OutWhy = FString::Printf(
				TEXT("the cut named %d bricks; a three-course opening of %d cells starting ")
				TEXT("on an odd course wants %d"),
				CutPieces.Num(), OpeningCells, Wanted);

			return false;
		}

		for (const int32 Piece : CutPieces)
		{
			if (!Laid.Layout.Structure.RemovePiece(Piece))
			{
				OutWhy = FString::Printf(TEXT("cut piece %d could not be removed"), Piece);
				return false;
			}
		}

		Out = MoveTemp(Laid.Layout.Structure);
		return true;
	}

	/**
	 * A ladder relation, pinned as a ratio of two lambda*, with both competing predictions
	 * printed beside the measurement on every run and quoted in the failure.
	 *
	 * A ratio rather than two windows because two windows say where each rung sits, while
	 * the ladder's whole content is how the rungs move relative to one another — a claim
	 * neither window makes. A solver change that shifted every rung by the same factor
	 * would keep the relation and break both windows, which is the split wanted: the
	 * relation is the physics finding and the windows are the arithmetic.
	 */
	void CheckLambdaLadderRatio(
		FAutomationTestBase& Test,
		const TCHAR* What,
		const TCHAR* Predictions,
		const FSweepReading& Top,
		const FSweepReading& Bottom,
		double RatioLo,
		double RatioHi)
	{
		const double Ratio = Bottom.Oracle.Lambda != 0.0
			? Top.Oracle.Lambda / Bottom.Oracle.Lambda
			: TNumericLimits<double>::Max();

		Test.AddInfo(FString::Printf(
			TEXT("LADDER %s: %.17g / %.17g = %.9g (%s)"),
			What, Top.Oracle.Lambda, Bottom.Oracle.Lambda, Ratio, Predictions));

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the measured ratio must lie in [%.9g, %.9g] and was %.17g / ")
				TEXT("%.17g = %.9g. %s. This pin is the LADDER's finding rather than either ")
				TEXT("rung's value — if it moved, a mechanism changed, and the two ")
				TEXT("predictions beside it are what to re-read before touching the window"),
				What, RatioLo, RatioHi, Top.Oracle.Lambda, Bottom.Oracle.Lambda, Ratio,
				Predictions),
			Ratio >= RatioLo && Ratio <= RatioHi);
	}

	/**
	 * A rung must be the fixture its name claims. Block count is the cheapest total
	 * statement of that: every rung of both ladders has a different one, so a builder that
	 * quietly ignored its parameter would collide here rather than reporting a plausible
	 * lambda* for a wall nobody asked for. Measured to be the only net that catches it:
	 * under the recorded rung-flip mutation (s=3 secretly built as s=1) the lambda window
	 * and the depth-ratio pin both passed — the two lambdas sit 1e-6 apart inside a 2e-5
	 * window, and the ratio reads exactly the 1.0 wanted — so window, ratio and size pins
	 * work as a set, never alone (TRAPS records the lesson).
	 */
	void CheckRungSize(
		FAutomationTestBase& Test,
		const TCHAR* What,
		const FSweepReading& Reading,
		int32 WantedBlocks)
	{
		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: the rung must bridge to %d live blocks and bridged %d — a rung ")
				TEXT("that is not the wall its name claims measures nothing"),
				What, WantedBlocks, Reading.Blocks),
			Reading.Blocks, WantedBlocks);
	}

	/**
	 * Slice 0d's flag-on AdjustProblem hook: turn the first-crack rows on and return the count
	 * of joints that will carry them, the bonded set (f_t > 0). Pinning that through
	 * OverriddenJoints makes "a row for every bonded joint, none for a dry one" a contract. The
	 * count uses the same f_t > 0 predicate the oracle keys its rows on, so agreement is the
	 * check, not a copy.
	 */
	int32 TurnOnFirstCrackRows(FOracleProblem& Problem)
	{
		Problem.bFirstCrackRows = true;

		int32 Bonded = 0;

		for (const FOracleJoint& Joint : Problem.Joints)
		{
			if (Joint.Strength.TensileStrengthMPa > 0.0)
			{
				++Bonded;
			}
		}

		return Bonded;
	}
}

/**
 * The leaning stack through the real fixture and bridge (sweep item (b)), and the
 * composite-depth measurement of item (e) on the fixture where composite depth is the whole
 * defect. lambda* runs 4.4582 / 1.2411 / 0.06293 / 0.03443 at 5/8/30/40 courses (characteristic
 * bond), matching slice 1's hand-built ladder; production agrees on every verdict (0/0/29/39
 * dropped) with a height-invariant worst reading of 0.138781067. That invariance is the
 * composite-depth point: the deep beam's m^2 cancels the demand's m^2, so at 30 courses the
 * joint reading overstates the true margin ~115x. Verdicts only agree because BreakOverturnedBodies
 * exists, which DESIGN.md §7 step 4 replaces.
 *
 * Re-measured at the mean re-anchor (2026-08-14): every rung is tension-governed and moved x7
 * with the bond (31.2074 / 8.6878 / 0.44049 / 0.24101); drop counts held.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSweepLeaningStackTest,
	"DestructionGame.Oracle.RigidBlock.Sweep.LeaningStack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSweepLeaningStackTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	Rows.Add({ TEXT("leaning stack, 5 courses"),
		TEXT("bond holds the lean; guard leaves it alone"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(5, Out, Why); },
		ERelation::AgreeStands, 31.2072, 31.2076, 0 });

	Rows.Add({ TEXT("leaning stack, 8 courses"),
		TEXT("bond holds the lean; guard leaves it alone"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(8, Out, Why); },
		ERelation::AgreeStands, 8.68772, 8.68782, 0 });

	Rows.Add({ TEXT("leaning stack, 30 courses"),
		TEXT("no equilibrium at ~0.44 of gravity; production agrees via the interim guard"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(30, Out, Why); },
		ERelation::AgreeFalls, 0.440491, 0.440495, 29, 0 });

	Rows.Add({ TEXT("leaning stack, 40 courses"),
		TEXT("no equilibrium; production agrees via the interim guard"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(40, Out, Why); },
		ERelation::AgreeFalls, 0.241006, 0.241010, 39, 0 });

	RunRows(*this, Rows);

	return true;
}

/**
 * The beam pair (sweep item (c)): the first measurement of the beam rows by a method with
 * global equilibrium that can express member failure (the glue line's tension bound is the
 * member's bending strength at the plastic stress block). Production drops all three beams,
 * light load included (worst joint reads Max as built, verified against Acceptance.Beam.Catalogue),
 * because the dry no-tension bearings are refused arching relief and read Max outside the kern.
 * The oracle stands all three, discriminating the member material 6.9x where production answers
 * identically. The heavy-timber stand does not contradict the catalogue's first-crack figure:
 * the oracle reads the plastic stress block (3x first-crack) plus redistribution, and for brittle
 * timber first-crack stays the honest criterion, so the catalogue verdict survives thin.
 *
 * Re-measured at the mean re-anchor (2026-08-14): C24 36/6, S275 290; lambda* moved by the member
 * factor to 2.6461 (heavy timber), 28.801 (light), 18.299 (steel).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSweepBeamPairTest,
	"DestructionGame.Oracle.RigidBlock.Sweep.BeamPair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSweepBeamPairTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	/*
	 * Row 1 re-pinned at the first-crack promotion (2026-08-28). The oracle is default-off, so
	 * row 1's window stays 2.64610374 (plastic + redistribution). Production's half moved:
	 * below the 200-block cap it solves with first-crack rows live, and the heavy C24 beam's
	 * midspan glue line binds at first-crack lambda* = 0.88258 < 1, so production fells it
	 * (drops 3). Rows 2/3 keep agree-stands: light timber (28.80) and steel (18.30) clear 1.0
	 * even at first crack's /3.
	 */
	Rows.Add({ TEXT("C24 timber beam, heavy load"),
		TEXT("oracle (default-off, plastic) stands at 2.65; production now fells the beam ")
		TEXT("below the cap — first crack binds the bonded midspan glue line at 0.883 and the ")
		TEXT("two half-beams plus block come down"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamC24DensityGramsPerCubicCm, BeamC24BendingMPa,
				BeamC24ShearMPa, 120.0, Out, Why);
		},
		ERelation::OracleStandsProductionFalls, 2.64609, 2.64612, 3, 0 });

	Rows.Add({ TEXT("C24 timber beam, light load"),
		TEXT("a sensibly loaded joist; production now stands it too below the cap"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamC24DensityGramsPerCubicCm, BeamC24BendingMPa,
				BeamC24ShearMPa, 10.0, Out, Why);
		},
		ERelation::AgreeStands, 28.8010, 28.8013, 0, 0 });

	Rows.Add({ TEXT("S275 steel beam, heavy load"),
		TEXT("steel at 18.30 vs timber's 2.65: the oracle discriminates the member ")
		TEXT("material 6.9x; production stands all three below the cap"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamSteelDensityGramsPerCubicCm, BeamS275YieldMPa,
				BeamS275YieldMPa / FMath::Sqrt(3.0), 120.0, Out, Why);
		},
		ERelation::AgreeStands, 18.2992, 18.2994, 0, 0 });

	RunRows(*this, Rows);

	return true;
}

/**
 * The corbel family (sweep item (a)). A and B here; C and D in the slow test on cost; E35/E36
 * and F excluded (file header). The oracle agrees with every §8 corbel ruling even at the
 * characteristic bond (A 2.905, B 221.4, C 2.567, D 58.06, all >= 1), so the limit theorem does
 * not condemn the corbels the user ruled must stand. Two findings the verdicts cannot carry:
 * filling buys 76x (bare arm hangs on its bond ladder, filled corbel jams as a block), and the
 * counterweight buys 22.6x in the LP where production (downward-only routing) reads C and D within
 * 0.4% and gains nothing.
 *
 * Re-measured at the mean re-anchor (2026-08-14): the two rungs moved by different factors (proof
 * no window here may be re-pinned by scaling) — A tension-bound x7 (20.333), B's Mohr-Coulomb
 * jamming x1.506 (333.40) — so filling now buys 16.4x.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSweepCorbelFamilyTest,
	"DestructionGame.Oracle.RigidBlock.Sweep.CorbelFamily",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSweepCorbelFamilyTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	Rows.Add({ TEXT("corbel A, bare arm of four"),
		TEXT("a mortared 11.25 cm/course lean of four bricks; the mean bond covers it ")
		TEXT("20.3x (2.9x at the retired characteristic strength)"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("corbel-a-bare-4"), Out, Why);
		},
		ERelation::AgreeStands, 20.3328, 20.3331, 0 });

	/*
	 * Slice 0d mover: the bare arm with first-crack rows on. All four bonded joints reach first
	 * crack and the ladder cuts to 0.2786 of control (5.6639); it still stands. The flag touches
	 * only the oracle, so production's relation and zero drop count carry over. OverriddenJoints = 4
	 * pins a row for every bonded joint.
	 *
	 * Why 0.2786 is below the 1/3 floor and is not per-joint net tension (review 2026-08-21): a
	 * single joint at eccentricity k has first-crack/plastic ratio (1+k)/(1+3k) in (1/3, 1],
	 * reaching 1/3 only in pure bending (k -> infinity, the beams). The sub-1/3 result is a
	 * multi-joint effect: first crack binds all four joints at once, so the LP can no longer trade
	 * eccentricity between them to stay in each kern. A mechanism finding, not a bug; the beams
	 * landing at /3 prove the per-joint floor holds.
	 */
	Rows.Add({ TEXT("corbel A, bare arm of four — first crack"),
		TEXT("bonded bending across the four-joint arm, cut to 0.279 of control — below the ")
		TEXT("per-joint 1/3 floor by multi-joint redistribution, not per-joint net tension; ")
		TEXT("still stands 5.66x"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("corbel-a-bare-4"), Out, Why);
		},
		ERelation::AgreeStands, 5.66385, 5.66390, 0, 0,
		TurnOnFirstCrackRows, 4 });

	Rows.Add({ TEXT("corbel B, filled four steps"),
		TEXT("the same reach filled solid jams as a block: 16.4x the bare arm's margin"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("corbel-b-filled-4"), Out, Why);
		},
		ERelation::AgreeStands, 333.402, 333.405, 0 });

	/*
	 * Slice 0d compression control, pinned as a characterisation because the invariance is
	 * approximate. Corbel B jams as a block (Mohr-Coulomb), so PROMOTION_DESIGN Sec 4.5 predicted
	 * first crack ~unchanged; measured it moves 8% (0.9205 of control, 306.885) because some
	 * bonded joints reach net tension in bending. Recorded, not smoothed. Only the dry set (0
	 * bonded joints, no rows) returns truly bit-identical.
	 */
	Rows.Add({ TEXT("corbel B, filled four steps — first crack"),
		TEXT("compression/jamming governs, so first crack moves it only 8% (0.9205 of ")
		TEXT("control) — APPROXIMATE invariance, pinned as a characterisation, NOT asserted ")
		TEXT("as unchanged"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("corbel-b-filled-4"), Out, Why);
		},
		ERelation::AgreeStands, 306.883, 306.886, 0, 0,
		TurnOnFirstCrackRows, 26 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* AOff =
		ReadingNamed(*this, Rows, Readings, TEXT("corbel A, bare arm of four"));
	const FSweepReading* AOn = ReadingNamed(
		*this, Rows, Readings, TEXT("corbel A, bare arm of four — first crack"));
	const FSweepReading* BOff =
		ReadingNamed(*this, Rows, Readings, TEXT("corbel B, filled four steps"));
	const FSweepReading* BOn = ReadingNamed(
		*this, Rows, Readings, TEXT("corbel B, filled four steps — first crack"));

	if (AOff != nullptr && AOn != nullptr && BOff != nullptr && BOn != nullptr)
	{
		/*
		 * The move is the finding, not either rung's window. A single pure-bending joint reads
		 * /3 = 0.3333 and none reads below it; corbel A's four-joint arm reads 0.2786 below the
		 * floor because first crack binds all four at once (see the mover comment above). If this
		 * ratio moved, the mechanism changed.
		 */
		CheckLambdaLadderRatio(*this,
			TEXT("SLICE 0d: corbel A's four-joint arm under first crack"),
			TEXT("a single pure-bending joint reads /3 = 0.3333 and none reads below it; the ")
			TEXT("four-joint arm cuts to 0.2786 by multi-joint redistribution — the MOVE, not ")
			TEXT("either rung, is the finding"),
			*AOn, *AOff, 0.27854, 0.27858);

		/*
		 * The control's small move, pinned so "approximate invariance" is a number: a
		 * compression-governed block still writes rows that bind by 8% (0.9205). A future change
		 * that moved corbel B more (or bit-identical) fails here loudly.
		 */
		CheckLambdaLadderRatio(*this,
			TEXT("SLICE 0d: corbel B's jamming block under first crack (approximate invariance)"),
			TEXT("compression governs so the prediction was ~none; measured 0.9205 — a bonded ")
			TEXT("block still writes rows that slightly bind, so the invariance is approximate"),
			*BOn, *BOff, 0.92045, 0.92047);
	}

	return true;
}

/**
 * The one-cell dry half seat with abutment. The oracle stands it by edge-contact jamming (the
 * head joint's top contact supplies the couple at a 7 cm arm); production once refused the
 * arching relief at 1.4921 of sliding capacity and the dry joint read Max. Limit theorem vs
 * uncracked-section convention, neither wrong, both pinned.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSweepOneCellTest,
	"DestructionGame.Oracle.RigidBlock.Sweep.OneCellDisagreement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSweepOneCellTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	/*
	 * Moved from oracle-stands / production-falls to agree-stands at slice 3b/4 (2026-08-27);
	 * the oracle lambda* is unchanged at 9592.68. Production once dropped both non-grounded
	 * bricks (the missing no-tension rocking model). Below the 200-block cap the equilibrium LP
	 * is now the break authority and finds the edge-contact jamming the limit theorem
	 * guaranteed, so production stands both bricks (drops 0, was 2).
	 */
	Rows.Add({ TEXT("one-cell dry half seat, with abutment"),
		TEXT("edge-contact jamming (limit theorem); production now stands both bricks below the ")
		TEXT("cap where the LP replaced the kern-and-centroid refusal"),
		[](FStructure& Out, FString& Why) { return BuildOneCellDryPair(Out, Why); },
		ERelation::AgreeStands, 9592.67, 9592.69, 0, 0 });

	RunRows(*this, Rows);

	return true;
}

/**
 * Slice 0d: the first-crack rows bite bonded bending and spare dry joints. With
 * FOracleProblem::bFirstCrackRows set, the LP carries -(n1+n2) + 3|n1-n2| <= f_t*A for every
 * joint with a real tensile bond and only those, cutting a bonded section's plastic bending
 * capacity to a third (a bonded-bending lambda* falls toward control/3) while every dry-stone
 * joint returns bit-identical. Full per-fixture table in PROMOTION_DESIGN Sec 4.3/4.5.
 *
 * The assertions:
 *   - Mechanism not outcome: the beam rows assert lambda* moved, never a verdict. Row 1's 0.882
 *     crosses 1.0 and would move a catalogue relation, a user ruling reported not encoded.
 *   - Window near control/3: BuildBeam lays one bonded joint and a simply-supported span carries
 *     N~=0 at midspan, so first crack is at full /3. Window 0.28-0.42 of control; 1.0 (no-op) is
 *     nowhere near, so the red reason is unambiguous.
 *   - Bit-identity for the dry pair is the keyed-on-data invariant: green on arrival under a
 *     no-op rule, still green once keyed on f_t > 0. Its bite-prover (key on material/always,
 *     dry lambda* moves) is recorded, not run here.
 *
 * Tier: default suite (beam is microseconds, dry one-cell solves fast). The full-sweep
 * re-measurement is OracleSweepFull work. No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockFirstCrackBitesTest,
	"DestructionGame.Oracle.RigidBlock.FirstCrack.BitesBondedBendingSparesDryJoints",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockFirstCrackBitesTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	/** Joints the first-crack rule keys on: a real tensile bond, exactly bCanTension's set. */
	const auto CountBonded = [](const FOracleProblem& Problem)
	{
		int32 Bonded = 0;

		for (const FOracleJoint& Joint : Problem.Joints)
		{
			if (Joint.Strength.TensileStrengthMPa > 0.0)
			{
				++Bonded;
			}
		}

		return Bonded;
	};

	/*
	 * Solve one built structure twice on the identical bridged problem: control (flag off) then
	 * first-crack (flag on). Reports why if fixture, bridge or either solve fails, so a red never
	 * hides behind a broken fixture.
	 */
	struct FPair
	{
		bool bOk = false;
		int32 Bonded = 0;
		FOracleResult Off;
		FOracleResult On;
	};

	const auto SolveBothWays =
		[&CountBonded](FAutomationTestBase& Test, const TCHAR* Name,
			const TFunction<bool(FStructure&, FString&)>& Build) -> FPair
	{
		FPair Out;

		FStructure Structure;
		FString Why;

		if (!Build(Structure, Why))
		{
			Test.AddError(FString::Printf(TEXT("%s: fixture could not be laid: %s"), Name, *Why));
			return Out;
		}

		FOracleProblem Problem;

		if (!BuildRigidBlockProblem(Structure, Problem, Why))
		{
			Test.AddError(FString::Printf(TEXT("%s: bridge refused the fixture: %s"), Name, *Why));
			return Out;
		}

		Out.Bonded = CountBonded(Problem);

		Problem.bFirstCrackRows = false;
		Out.Off = SolveRigidBlock(Problem);

		Problem.bFirstCrackRows = true;
		Out.On = SolveRigidBlock(Problem);

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: control solve (flag off) must answer"), Name),
				Out.Off.bAnswered)
			|| !Test.TestTrue(
				*FString::Printf(TEXT("%s: first-crack solve (flag on) must answer"), Name),
				Out.On.bAnswered))
		{
			return Out;
		}

		Out.bOk = true;

		UE_LOG(LogTemp, Display,
			TEXT("FIRSTCRACK %s: bonded=%d lambdaOff=%.17g lambdaOn=%.17g ratio=%.6g ")
			TEXT("pivotsOff=%d pivotsOn=%d"),
			Name, Out.Bonded, Out.Off.Lambda, Out.On.Lambda,
			Out.Off.Lambda > 0.0 ? Out.On.Lambda / Out.Off.Lambda : -1.0,
			Out.Off.SimplexIterations, Out.On.SimplexIterations);

		return Out;
	};

	/* ---- The three beam rows: one bonded glue line each, bending governs cleanly. -------- */
	struct FBeamRow
	{
		const TCHAR* Name;
		double Density;
		double BendingMPa;
		double ShearMPa;
		double BlockHeightCm;
		double ControlLambda;
	};

	const FBeamRow BeamRows[] = {
		{ TEXT("C24 timber beam, heavy load"), BeamC24DensityGramsPerCubicCm,
			BeamC24BendingMPa, BeamC24ShearMPa, 120.0, 2.6461037357339725 },
		{ TEXT("C24 timber beam, light load"), BeamC24DensityGramsPerCubicCm,
			BeamC24BendingMPa, BeamC24ShearMPa, 10.0, 28.801133612618997 },
		{ TEXT("S275 steel beam, heavy load"), BeamSteelDensityGramsPerCubicCm,
			BeamS275YieldMPa, BeamS275YieldMPa / FMath::Sqrt(3.0), 120.0,
			18.299323934632291 },
	};

	for (const FBeamRow& Beam : BeamRows)
	{
		const FPair P = SolveBothWays(*this, Beam.Name,
			[&Beam](FStructure& Out, FString& Why)
			{
				return BuildBeam(Beam.Density, Beam.BendingMPa, Beam.ShearMPa,
					Beam.BlockHeightCm, Out, Why);
			});

		if (!P.bOk)
		{
			continue;
		}

		/* Exactly one bonded joint (the glue line). If not 1 the fixture changed and every
		 * prediction below is about a different wall. */
		TestEqual(
			*FString::Printf(
				TEXT("%s: exactly one bonded joint (the glue line) carries a first-crack row"),
				Beam.Name),
			P.Bonded, 1);

		/* The control must still read its pinned window, or the flag-off path is not the wall the
		 * prediction was made against and the ratio below is meaningless. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: control lambda* %.9g must sit at the pinned %.9g (flag off is the ")
				TEXT("untouched wall)"),
				Beam.Name, P.Off.Lambda, Beam.ControlLambda),
			FMath::Abs(P.Off.Lambda - Beam.ControlLambda) <= 1.0e-4 * Beam.ControlLambda);

		/* The bite (the red's must-pass): adding first crack can only shrink lambda* (monotone),
		 * and where bonded bending governs it shrinks past half. Rows absent => lambda*(on) ==
		 * lambda*(off) and this fails, the whole red. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: FIRST-CRACK ROWS MUST BITE — lambda*(on)=%.9g must fall below ")
				TEXT("0.5x lambda*(off)=%.9g (predicted control/3=%.9g). It did not move, so ")
				TEXT("the rows are absent"),
				Beam.Name, P.On.Lambda, P.Off.Lambda, Beam.ControlLambda / 3.0),
			P.On.Lambda < 0.5 * P.Off.Lambda);

		/* N~=0 at midspan puts first crack at full /3 severity, so lambda*(on) lands near
		 * control/3. The window allows arching/shear wobble; 1.0 (no-op) is nowhere near. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda*(on)=%.9g must land near control/3=%.9g (window 0.28-0.42 of ")
				TEXT("control %.9g) — the first-crack factor is exactly 3 in pure bending"),
				Beam.Name, P.On.Lambda, Beam.ControlLambda / 3.0, Beam.ControlLambda),
			P.On.Lambda >= 0.28 * P.Off.Lambda && P.On.Lambda <= 0.42 * P.Off.Lambda);
	}

	/* User-decision item, reported not asserted: beam row 1's 0.882 (2.6461/3) crosses 1.0,
	 * moving its catalogue relation toward AGREE(falls) — right verdict (member fails in bending)
	 * by a slightly wrong route (glue line cracks). The test asserts the drop, not the flip. */
	AddInfo(TEXT(
		"USER DECISION (do not bank): first-crack takes beam row 1 (C24 heavy) to ~0.882 "
		"< 1.0, an oracle verdict flip to Falls — correct verdict, glue-line-cracking route "
		"rather than member bending. Measure and REPORT for the user's ruling; do not encode "
		"it as settled (PROMOTION_DESIGN Sec 4.3, Sec 8)."));

	/* ---- The dry one-cell pair: no bond, must return bit-identical. ---------------------- */
	{
		const FPair P = SolveBothWays(*this, TEXT("one-cell dry half seat"),
			[](FStructure& Out, FString& Why) { return BuildOneCellDryPair(Out, Why); });

		if (P.bOk)
		{
			/* Keyed on data: a dry fixture offers the first-crack rule nothing to key on. */
			TestEqual(
				TEXT("one-cell dry half seat: zero bonded joints (nothing to carry a "
					 "first-crack row)"),
				P.Bonded, 0);

			/* No bonded joint => no rows => lambda* and pivot count bit-identical with the flag
			 * on. Green on arrival; its bite-prover (key on material/always, dry lambda* moves) is
			 * recorded, not run here. */
			TestEqual(
				TEXT("one-cell dry half seat: lambda* is BIT-IDENTICAL with the flag on — dry "
					 "stone has no bond to crack, so the rule keyed on f_t > 0 writes no row"),
				P.On.Lambda, P.Off.Lambda);

			TestEqual(
				TEXT("one-cell dry half seat: pivot count is bit-identical with the flag on — "
					 "an identical problem is solved either way"),
				P.On.SimplexIterations, P.Off.SimplexIterations);
		}
	}

	return true;
}

/* ====================================================================================
 * The opt-in oracle sweep, in two tiers. Neither name contains "DestructionGame", so the
 * full-suite command never runs either tier. Both substring-match one filter:
 *
 *     -ExecCmds="Automation RunTests OracleSweepFast"  10 tests,   ~2 min
 *     -ExecCmds="Automation RunTests OracleSweepFull"   6 tests,  ~24 min
 *     -ExecCmds="Automation RunTests OracleSweep"      16 tests,  ~26 min (both)
 *
 * Counts re-checked 2026-09-03. The filter is a plain substring match, so the shared OracleSweep
 * stem is the union and neither tier is reachable by the default DestructionGame filter (TRAPS).
 *
 * Which test goes where is cost alone; every row carries pinned expectations. Per-test seconds,
 * measured 2026-08-16 (this machine varies ~5%, so treat as a range):
 *
 *     FULL   WallsAndLadders                              478 s
 *     FULL   PhaseTwoMustNotRefuseTheCoveredOpeningFamily  455 s
 *     FULL   FeasibilityReformulationCost (other file)     313 s
 *     fast   OpeningMechanismLadders                      22.4 s
 *     fast   PhaseTwoMustNotRefuseABoundedProblem         19.6 s
 *     fast   FreeEndHeightLadder                          16.1 s
 *     fast   Case21ResidualAttribution                    ~7.0 s
 *     fast   OpeningStrengthProbes                         4.1 s
 *     fast   RefusalNamesItsReason                         1.0 s
 *
 * The fast tier is iteration, not verification. Three tests are 94% of the cost and the three
 * that watch the solver at scale (wall catalogue, covered-opening refusals, feasibility cost
 * table). OracleSweepFull is mandatory before any commit touching the LP oracle, and before any
 * commit if the solver changed; a green fast tier says nothing about that.
 *
 * The hazard: an opt-in tier rots (the NonNullRHI test rotted five slices and produced twelve
 * errors when finally run, TRAPS). The mitigation is the rule above plus the cost table, so a
 * tier can be priced. Whole-group cost is ~490-530 s against a ~30 s default budget, and every
 * run so far returned bit-identical lambda* and worst readings (the determinism contract). Keep
 * the figure current: it is the argument for the group existing.
 * ==================================================================================== */

/**
 * The wall catalogue (sweep item (d)). Since the 2026-08-12 sparse rewrite every wall except the
 * 30-course five answers, and fifteen of the twenty acceptance cases carry a load factor, pinned
 * below. Rows are laid through the scenario catalogue (same producer and cuts as the acceptance
 * suite), removed, bridged and diffed, ordered cheap to expensive so a killed run leaves its
 * measurements. The free-end ladder (item (e)) moved to its own test below.
 *
 * What the measurements said once classified:
 *   - The oracle's ordering agreed with the catalogue's verdicts even where its threshold did
 *     not: the four fixtures the catalogue found hardest are the four the LP prices lowest.
 *   - The four rows needing a user ruling were all ruled 2026-08-12 (9/10/19 collapse -> stands,
 *     20 confirmed); each ruling is in its own row's mechanism text.
 *   - Cases 10 and 19 stayed red with the sign reversed: the catalogue says they stand and
 *     production drops 12 and 34, the suite's first rows where a stands verdict is what fails.
 *   - Two pairs are pinned as cross-row identities, not two windows: 15/16 and the free-end
 *     height ladder (see CheckSameLambda for why an identity is a stronger pin).
 *   - Two "collapses" break no joint: wall-10 (12 down) and wall-19 (34 down) cascade in zero
 *     passes at readings under 0.32, so those pieces are unrouted not overloaded. wall-20 is the
 *     opposite (a joint genuinely 42x over), which is why its verdict was confirmed not moved.
 *   - The stack-bond pair separates 36.75x in production vs 1.41x in the LP, not pinned as a
 *     ratio because the family carries a deliberate red (StackBondColumnShearIsHeightIndependent).
 *
 * The 30-course walls (cases 1-5) remain beyond the practical envelope on pricing cost.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowWallSweepTest,
	"OracleSweepFull.RigidBlock.WallsAndLadders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowWallSweepTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	const auto Scenario = [](const TCHAR* Name)
	{
		return [Name](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(Name, Out, Why);
		};
	};

	TArray<FSweepRow> Rows;

	/* --- cheap first, everything measured 2026-08-11 --------------------------------- */

	/*
	 * The row that settled a ruling rather than recording one. Until 2026-08-11 wall-08 was
	 * flagged: the catalogue ruled it a local loss, production dropped nothing, this measurement
	 * stood it at 325x, and the catalogue was the outlier. The user re-ruled it stands (DESIGN §8),
	 * so the three methods now agree. The relation enum never moved (AgreeStands is
	 * oracle-vs-production by construction); only the mechanism text changed, and it must still
	 * name the mechanism.
	 */
	Rows.Add({ TEXT("wall-08 four-cell opening, one course over"),
		TEXT("the flat-arch row: the four-cell coverless course jams against its ")
		TEXT("abutments through head-joint compression, so cover is not what carries it ")
		TEXT("and at 325x own weight the verdict survives every charitable-reading ")
		TEXT("discount (/3 plastic, /6 characteristic-vs-mean, i.e. 18.0 discounted both ")
		TEXT("ways at once — and the characteristic-vs-mean slant runs the other way, so ")
		TEXT("that half is budget rather than correction). THIS MEASUREMENT IS WHAT MOVED ")
		TEXT("THE CATALOGUE: case 8 was re-ruled from LOCAL LOSS to STANDS on 2026-08-11, ")
		TEXT("costing the set its last 'no room to arch' discriminator"),
		Scenario(TEXT("wall-08")),
		ERelation::AgreeStands, 481.519, 481.539, 0 });

	Rows.Add({ TEXT("corbel C, ten steps"),
		TEXT("the §8 ruling's fixture: the limit theorem agrees it stands — 17.97x at the ")
		TEXT("mean bond (2.57x at the retired characteristic, an exact x7: this rung is ")
		TEXT("tension-bound)"),
		Scenario(TEXT("corbel-c-10")),
		ERelation::AgreeStands, 17.97037, 17.97109, 0 });

	Rows.Add({ TEXT("wall-14 corbel, half brick per course"),
		TEXT("the projection pair's far half; see wall-13 for the pair's cross-method ")
		TEXT("history"),
		Scenario(TEXT("wall-14")),
		ERelation::AgreeStands, 440.200, 440.218, 0 });

	Rows.Add({ TEXT("wall-13 corbel, quarter brick per course"),
		TEXT("the corbel projection pair. AT THE CHARACTERISTIC DATA the pair was the ")
		TEXT("suite's strongest cross-validation — production 2.7786x vs LP 2.7777x, ")
		TEXT("0.03% apart. AT THE MEAN DATA that agreement is GONE: production's ratio is ")
		TEXT("unchanged (both readings moved /7 together) while the LP's fell to ")
		TEXT("817.047/440.209 = 1.856 — the two rungs' binding constraints no longer move ")
		TEXT("together, so the LP-side projection term is an open re-derivation (logged in ")
		TEXT("CURRENT_STATE), and the windows are what this row still pins"),
		Scenario(TEXT("wall-13")),
		ERelation::AgreeStands, 817.030, 817.063, 0 });

	Rows.Add({ TEXT("corbel D, ten steps with counterweight"),
		TEXT("global equilibrium prices the counterweight at 8.97x on the mean basis ")
		TEXT("(C's 17.97 -> 161.14; it was 22.6x at the characteristic data) where ")
		TEXT("production reads C and D 0.4% apart and the deliberate red ")
		TEXT("CorbelStepsBeforeTensionWins records them crossing 1.0 at 124 steps ")
		TEXT("identically"),
		Scenario(TEXT("corbel-d-10-counterweight")),
		ERelation::AgreeStands, 161.1375, 161.1439, 0 });

	/* --- the twelve the sparse rewrite unblocked, pinned 2026-08-12, cheap first ------ */

	/*
	 * The bond pair, and case 17's first number of any kind. As an outcome row case 17 has zero
	 * discriminating power (both halves stand), but the LP separates the pair: 918.05 intact vs
	 * 649.46 with one brick out, 1.41x. Production's readings are printed but not pinned as a
	 * ratio, because the stack-bond family is deliberately red
	 * (StackBondColumnShearIsHeightIndependent) and a ratio over a known-wrong reading would dress
	 * the defect as a discrimination.
	 */
	Rows.Add({ TEXT("wall-18 stack bond, one brick out"),
		TEXT("stack bond has no bond to spread a loss, so the column over the hole hangs ")
		TEXT("on head-joint tension into its neighbours: 897.73x at the mean bond (649.46 ")
		TEXT("at the characteristic — this rung moved x1.382 while the intact wall-17's ")
		TEXT("918.05 did not move at all, so the pair's margin shrank 1.41x -> 1.023x: ")
		TEXT("the strength-governed rung closed most of the gap on the crush-governed one)"),
		Scenario(TEXT("wall-18")),
		ERelation::AgreeStands, 897.712, 897.749, 0, 0 });

	Rows.Add({ TEXT("wall-17 stack bond, intact"),
		TEXT("the highest lambda* in the swept set (918.05x) and the first absolute ")
		TEXT("number case 17 has ever carried; an intact stack-bond wall standing is not ")
		TEXT("news, the MARGIN against wall-18 is"),
		Scenario(TEXT("wall-17")),
		ERelation::AgreeStands, 918.04511, 918.04695, 0, 0 });

	/*
	 * The four flagged rows, all ruled on 2026-08-12. Rows 10, 19, 9 and 20 were flagged for a
	 * ruling until the user ruled the same day. No measurement or relation enum moves (all are
	 * oracle-vs-production by construction); only the mechanism texts changed, as wall-08 did:
	 *
	 *     wall-09   collapse -> stands   model agrees, the acceptance row went green
	 *     wall-10   collapse -> stands   model does not agree; both stay red in a new
	 *     wall-19   collapse -> stands   direction (expected to stand, 12 and 34 dropped)
	 *     wall-20   local loss, kept     confirmed not moved; lambda* has no local vocabulary
	 *
	 * Hand figures vs lambda*, which the rulings weighed: the hand arithmetic is a tension-plane
	 * elastic check (bending stress on a vertical crack plane vs flexural strength), the same kind
	 * of answer production computes, so the two agree. Lambda* is a different question: the limit
	 * theorem finds the best admissible compression path, which needs almost no tension, so it runs
	 * 14-32x above the hand figures. A ruling weighs both. On 9, 10 and 19 the hand check also
	 * acquitted the fixture on the strength basis, so lambda* was confirming not outvoting.
	 */

	/*
	 * The partial-pricing re-pin (2026-08-12). An uncommitted partial-pricing slice changed the
	 * simplex's pivot path; lambda* stays bit-identical on every validation fixture, but five
	 * sweep fixtures at 120-150 blocks land a different distance from the unique optimum:
	 *
	 *     wall-10   35.8172298   -> 35.817113279469787    3.25e-6 relative
	 *     wall-19   12.3824832   -> 12.382629959455667    1.19e-5 relative  (the worst)
	 *     wall-06  622.031942    -> 622.03383039924574    3.04e-6 relative
	 *     wall-12   89.1151243   ->  89.115041750073942   9.26e-7 relative
	 *     wall-09   36.5639285   ->  36.563909109648513   5.30e-7 relative
	 *
	 * Both readings are certified (each passes post-solve verification at 1e-6 relative), so the
	 * old +/-1e-6 windows were pinning the pivot path, not the optimum: at this scale the spread
	 * across pivot paths is ~1e-5 relative, worst 1.19e-5 (wall-19). All five are re-centred on the
	 * midpoint with a +/-2e-5 half-width. A window tighter than ~1e-5 here pins the algorithm.
	 *
	 * wall-12 and wall-09 are re-pinned though they passed with little clearance (6.5% and 24% of
	 * the old box): the next same-direction change would fail as a phantom masonry regression, so
	 * the rule applies to every row it touches. Where exactness matters (15/16 and the free-end
	 * pair) the identity check (CheckSameLambda) is the right tool, so those needed no change.
	 */

	Rows.Add({ TEXT("wall-10 opening at a free end, no abutment"),
		TEXT("THIS MEASUREMENT IS WHAT MOVED THE CATALOGUE: case 10 was re-ruled from ")
		TEXT("COLLAPSE to STANDS on 2026-08-12, and unlike case 8 the model does NOT ")
		TEXT("follow — the acceptance row stays red in the inverted direction, expected to ")
		TEXT("stand and dropping 12. Production drops those 12, of which 3 ")
		TEXT("are STRANDED (unroutable, not unheld — the absent cycle rule, DESIGN §5.1); ")
		TEXT("the LP stands it at 35.82x. The panel over the opening is a 3.75-cell ")
		TEXT("(84 cm) cantilever 8 courses (60 cm) deep off a single jamb: 8 x 3.75 = 30 ")
		TEXT("brick weights = 800 N at a 42.2 cm lever is M = 337 N*m, over ")
		TEXT("t*D^2/6 = 6150 cm^3 that is 0.055 MPa — 0.55x characteristic f_xk1 (0.10) ")
		TEXT("and 0.14x of f_xk2 (0.40) on a STRAIGHT vertical plane, and the real crack path ")
		TEXT("is a toothed staircase carrying bed-joint cohesion the plane cannot see. ")
		TEXT("Same mechanism family as the 2026-08-06 free-end ruling, scaled up to 3.75 ")
		TEXT("cells, which is what the ruling credited. PRODUCTION NOW AGREES (Slice 3b/4, ")
		TEXT("2026-08-27): below the 200-block cap the equilibrium LP is the break authority ")
		TEXT("and carries the panel the downward-only router could only strand, so production ")
		TEXT("drops 0 (was 12) and strands 0 (was 3) — the absent-mechanism verdict is gone and ")
		TEXT("the row agrees with the oracle. The lambda* window is UNCHANGED; only production's ")
		TEXT("half of the diff moved"),
		Scenario(TEXT("wall-10")),
		/* Re-pinned 2026-08-12 for the partial-pricing spread (note above): midpoint of 35.8172298
		 * and 35.817113279469787, +/-2e-5. Untouched by Slice 4; only ProductionFallen/Stranded
		 * went 12/3 -> 0. */
		ERelation::AgreeStands, 111.4952, 111.4997, 0, 0 });

	Rows.Add({ TEXT("wall-19 bottom course out under half the wall"),
		TEXT("THIS MEASUREMENT IS WHAT MOVED THE CATALOGUE, and it is the CLOSEST CALL of ")
		TEXT("the four ruled on 2026-08-12: case 19 re-ruled COLLAPSE to STANDS knowingly, ")
		TEXT("with the model still dropping 34 so the acceptance row stays red in the ")
		TEXT("inverted direction. The LOWEST lambda* of the fifteen walls (12.38x) ")
		TEXT("— still above 1. Production drops 34 with 6 STRANDED. ")
		TEXT("6.0 cells (135 cm) of base gone at the wall's END — course 0 is an EVEN course ")
		TEXT("of whole bricks, so the cut takes cells 0..5, six of them, and the run prints ")
		TEXT("cut 6 — so the 9 courses over ")
		TEXT("it CANTILEVER rather than span: 9 x 6 = 54 brick weights = 1440 N at a ")
		TEXT("67.5 cm lever is M = 972 N*m, over t*D^2/6 = 7784 cm^3 that is 0.125 MPa ")
		TEXT("— 1.25x characteristic f_xk1 (0.10) but ")
		TEXT("0.31x of f_xk2's 0.40 and ~0.21x of the mean basis, and f_xk2 is what a ")
		TEXT("toothed vertical crack path ")
		TEXT("costs in a real wall. The two strengths STRADDLE the verdict, so this was a ")
		TEXT("judgement and is recorded as one: it is an END CANTILEVER with no second ")
		TEXT("support to redistribute to, against the practice anchor of ~1 m underpinning ")
		TEXT("bays that a bonded wall is expected to bridge — and 135 cm is longer than ")
		TEXT("that, which the ruling knows. PRODUCTION NOW AGREES (Slice 3b/4, 2026-08-27): ")
		TEXT("below the cap the LP carries the wedge the base-less router could only strand, so ")
		TEXT("production drops 0 (was 34) and strands 0 (was 6). The lambda* window is UNCHANGED"),
		Scenario(TEXT("wall-19")),
		/*
		 * Re-pinned 2026-08-12 for the partial-pricing pivot-path spread (see the
		 * partial-pricing re-pin note above wall-10): midpoint of 12.3824832 (old path)
		 * and 12.382629959455667 (new path), +/-2e-5 relative — this fixture carries the
		 * worst measured pivot-path spread (1.19e-5), the measurement the +/-2e-5
		 * half-width is built from. The lambda* window is untouched by Slice 4 — only
		 * ProductionFallen/Stranded went 34/6 -> 0.
		 */
		ERelation::AgreeStands, 47.96274, 47.96466, 0, 0 });

	/*
	 * The pier pair, whose discrimination DESIGN §8 explicitly handed to this oracle. The
	 * 2026-08-09 case-12 ruling recorded the cost of rewriting case 12: "the pier-width
	 * pair (11 vs 12) now has no measurable discrimination — the solver reads them 0.03%
	 * apart because it carries thrust as springing shear with no pier-width term; the LP
	 * oracle owns measuring that." It now does: 128.12 on three cells of bearing against
	 * 89.12 on one, 1.44x, and in the physically right direction.
	 */
	Rows.Add({ TEXT("wall-12 the same span on a one-brick pier"),
		TEXT("the pier pair's narrow half: 89.12x, 1.44x less margin than wall-11's ")
		TEXT("three cells of bearing, where production reads the two 0.03% apart ")
		TEXT("(0.362067 vs 0.362193) because springing shear carries no pier-width term"),
		Scenario(TEXT("wall-12")),
		/*
		 * Re-pinned 2026-08-12 for the partial-pricing pivot-path spread (see the
		 * partial-pricing re-pin note above wall-10): midpoint of 89.1151243 (old path)
		 * and 89.115041750073942 (new path), +/-2e-5 relative. This row did not fail — the
		 * new reading sat 6.5% of the way into the old box — which is why it is re-pinned
		 * rather than left to fail next time as a phantom masonry regression.
		 */
		ERelation::AgreeStands, 206.8525, 206.8608, 0, 0 });

	/*
	 * The superimposed-load pair reads as one number, and that is a statement about the
	 * problem shape, not a coincidence. Gravity is the LP's single live load
	 * (FOracleProblem::bGravityIsLive), so piling six courses on the header's tail scales
	 * the demand it makes and the pre-compression that steadies it by the same lambda, and
	 * a multiplier-on-own-weight measure cannot see the difference. Production does see it
	 * — 31.6x at the header's own joint — so the pair is pinned from both sides below: the
	 * lambda* identity and production's reading ratio, the second of which also stops the
	 * identity passing vacuously if the two rows ever built one fixture. Wanted (logged
	 * for CURRENT_STATE): the honest LP-side version of this pair marks the six courses
	 * live and the rest dead, which the problem shape already supports and which nothing
	 * but the sliding rows exercises.
	 */
	Rows.Add({ TEXT("wall-16 header at the top, nothing on it"),
		TEXT("the superimposed-load pair's bare half; the LP prices it identically to ")
		TEXT("wall-15 because self-weight is its only live load, pinned as a cross-row ")
		TEXT("identity below"),
		Scenario(TEXT("wall-16")),
		ERelation::AgreeStands, 868.62287, 868.62461, 0, 0 });

	Rows.Add({ TEXT("wall-15 header with six courses on top"),
		TEXT("the loaded half, and the SAME NUMBER: 868.623736 either way. Production ")
		TEXT("separates the pair 31.6x at the header's own bed joint (0.00184437 vs ")
		TEXT("0.058203838 — the surcharge drives tension to zero and leaves compression ")
		TEXT("governing), so the discrimination the catalogue wanted is production's ")
		TEXT("here and the LP's only under a dead/live split it does not yet use"),
		Scenario(TEXT("wall-15")),
		ERelation::AgreeStands, 868.62287, 868.62461, 0, 0 });

	Rows.Add({ TEXT("wall-20 staircase void"),
		TEXT("RE-RULED TO AGREE-STANDS AT SLICE 4 (2026-08-27), the standing doubt settled in ")
		TEXT("the LP's direction. Through 2026-08-12 the catalogue ruled a LOCAL LOSS of two ")
		TEXT("NAMED teeth (course 3 cell 4.5, course 5 cell 2.5) that production over-answered by ")
		TEXT("dropping 9; below the 200-block cap the equilibrium LP is now the break authority ")
		TEXT("and stands the whole wall INCLUDING both teeth, so production drops 0 (was 9) and ")
		TEXT("agrees with the oracle. Why hanging a tooth is cheap, by hand: 0.1 MPa over ")
		TEXT("two head joints (2 x 66.625 cm^2) plus the two bed patches above it (2 x ")
		TEXT("105.0625 cm^2) is 3434 N against a brick's 2667 uu = 26.67 N, about 129x, ")
		TEXT("so the teeth are not what governs and the LP is not merely tolerating them. The ")
		TEXT("catalogue row is re-ruled STANDS to match (a global feasible force path exists); a ")
		TEXT("genuinely-local two-tooth mechanism, if one exists, needs the per-region ")
		TEXT("interrogation the global solve cannot express (PROMOTION_DESIGN §3.5). The lambda* ")
		TEXT("window is UNCHANGED"),
		Scenario(TEXT("wall-20")),
		ERelation::AgreeStands, 218.4185, 218.4272, 0, 0 });

	Rows.Add({ TEXT("wall-11 wall on two piers, six-brick clear span"),
		TEXT("the pier pair's wide half at 128.12x; the §8 case-11 ruling worked this ")
		TEXT("fixture by hand at 0.03-0.04 MPa of deep-beam bending against mean bond, ")
		TEXT("and the LP's three-figure margin at CHARACTERISTIC bond is that ruling's ")
		TEXT("first independent confirmation"),
		Scenario(TEXT("wall-11")),
		ERelation::AgreeStands, 269.3169, 269.3277, 0, 0 });

	Rows.Add({ TEXT("wall-09 ten-brick opening, eight courses over"),
		TEXT("THIS MEASUREMENT IS WHAT MOVED THE CATALOGUE: case 9 was re-ruled from ")
		TEXT("COLLAPSE to STANDS on 2026-08-12 and, alone of the three moved that day, ")
		TEXT("the MODEL AGREES — the acceptance row went green and left the known-red ")
		TEXT("list. Still the least stable relation in this file: ")
		TEXT("production STANDS at worst reading 0.985 — ONE RETUNE ")
		TEXT("FROM FLAPPING, and the day it crosses 1.0 this row's AgreeStands becomes ")
		TEXT("AgreeFalls with nothing about the physics having changed, which is now ")
		TEXT("watched from the production side too by Acceptance.Wall.SpanIsReadInThe")
		TEXT("JointNotInTheOutcome — while the LP ")
		TEXT("stands it at 36.56x. By hand: a 9.5-cell (214 cm) opening under 8 courses ")
		TEXT("(60 cm) of cover is a deep beam at span/depth 3.6 carrying 8 x 9.5 = 76 ")
		TEXT("brick weights = 2027 N, W*L/8 = 542 N*m over t*D^2/6 = 6150 cm^3 = 0.088 ")
		TEXT("MPa — 0.88x characteristic f_xk1 (0.10), 0.22x of f_xk2 (0.40) and ~0.15x ")
		TEXT("of the mean basis. That ")
		TEXT("hand figure and production's 0.985 are the same answer to within the ")
		TEXT("routing — two methods agreeing against one, which is what decided it. The ")
		TEXT("catalogue's COLLAPSE came from the published arching ")
		TEXT("gate (no room for a 45 deg triangle over 2.1 m), and this is the THIRD ")
		TEXT("verdict that gate has lost after cases 11 and 8: the set now has no case ")
		TEXT("refusing a span for want of rise"),
		Scenario(TEXT("wall-09")),
		/* Re-pinned 2026-08-12 for the partial-pricing spread (note above wall-10): midpoint of
		 * 36.5639285 and 36.563909109648513, +/-2e-5. Re-pinned though it passed, so the next
		 * pivot-path change cannot arrive dressed as a regression on the least stable relation. */
		ERelation::AgreeStands, 105.6457, 105.6499, 0, 0 });

	/*
	 * The cover pair's honest measurement, owed since 2026-08-11. DESIGN §8 declined to pin
	 * 7-vs-8 on readings because production reads case 7 worse than case 8 (0.269 vs 0.219, 1.23x)
	 * and a downward router reads cover as load. The LP, with no router, reads the same direction:
	 * 296.22 with eight courses of cover vs 324.73 with one, 1.10x worse. Lambda* is a multiplier
	 * on own weight, so added cover raises demand and capacity together and the net direction
	 * depends on which grows faster; the walls also differ in size (140 vs 52 blocks), so the
	 * comparison is ordinal. The useful part: a routerless method reproduces the direction, so the
	 * direction is uninformative about the router defect. Whether §8 should move is an open user
	 * call. The MatchedPairs row it mentions retired 2026-08-12 when cases 9 and 10 were re-ruled.
	 */
	Rows.Add({ TEXT("wall-07 four-cell opening, eight courses over"),
		TEXT("296.22x against wall-08's 324.73x: the LP prices eight courses of cover ")
		TEXT("1.10x WORSE than one, the same direction production reads at the joint and ")
		TEXT("for a reason that is about the measure (lambda* is a multiplier on own ")
		TEXT("weight) rather than about the router"),
		Scenario(TEXT("wall-07")),
		ERelation::AgreeStands, 456.874, 456.893, 0, 0 });

	Rows.Add({ TEXT("wall-06 two-cell opening, deep cover"),
		TEXT("the span ladder's short end: half wall-07's span under the same cover ")
		TEXT("prices 2.10x its margin (622.03 vs 296.22), which is the discrimination the ")
		TEXT("outcome column never had — 06, 07 and 08 all stand and always did"),
		Scenario(TEXT("wall-06")),
		/* Re-pinned 2026-08-12 for the partial-pricing spread (note above wall-10): midpoint of
		 * 622.031942 and 622.03383039924574, +/-2e-5. */
		ERelation::AgreeStands, 634.570, 634.596, 0, 0 });

	/*
	 * The first row where the oracle is the outlier, and the first whose acceptance verdict is
	 * Collapse. Hand statics and production both condemn the wall; the LP stood it at 5.51x
	 * (characteristic era). Pinned as a measurement of the disagreement, not a verdict: what the
	 * LP prices here has not been established. 5.511 is 66x the mechanism the verdict condemns (a
	 * 15 cm panel on flexural bond fails at 0.10/1.2014 = 0.0832), so something other than the bond
	 * carries it.
	 *
	 * Two measured ladders, 2026-08-13 (mean span L = 394.75 + (cells-22) x 22.5 cm):
	 *   cover at 22 cells   2 crs 5.511   4 crs 7.683   6 crs 9.183   8 crs refused
	 *   span at 2 courses   18 c 10.408   22 c 5.511   27 c 3.102   32 c 1.985  40 c 1.143  45 c 0.863
	 *
	 * The priced hypothesis, still only a hypothesis: a full-height arch whose thrust dives through
	 * the jambs prices at 22 cells to 5.48 vs the LP's 5.511 (one rung), but along the cover ladder
	 * it gives 5.48/4.11/3.65, falling where the measurement rises, so it is wrong in direction. It
	 * may be a third charity (the oracle's immovable ground gives free foundation restraint), or a
	 * real load path the router cannot see. An open user call in CURRENT_STATE, with two
	 * discriminating ladders (vary courses below the opening; widen the jambs).
	 *
	 * Laid through Scenario(...) since the wall-21 level landed 2026-08-13 (briefly a hand-rolled
	 * BuildOpeningWall, now deleted; the window is unchanged across the swap).
	 */
	/*
	 * Re-measured at the mean re-anchor (2026-08-14): production now stands the wall (worst reading
	 * 0.93542327561664174, not the scaled 0.549, so the governing axis moved and cannot be
	 * re-derived by scaling). Which axis is unverified: the "squeezed-edge compression" first
	 * written here proved wrong for cases 9 and 22 (Mohr-Coulomb shear) on this jamb-bed shape, so
	 * decompose over GetConnectionForce before believing any attribution (CURRENT_STATE; TRAPS).
	 * Zero passes, zero dropped, so AgreeStands: both models now disagree with the ruled Collapse.
	 * The LP's stand moved x3.128 to 17.2389, not the x4.5 pure jamb-cohesion predicts, so the
	 * 2026-08-13 attribution is incomplete at the mean data (the zeroed-cohesion probe keeps 27.7%,
	 * see OpeningStrengthProbes); the mechanism split is open in CURRENT_STATE. The prose above is
	 * the characteristic era's.
	 */
	Rows.Add({ TEXT("wall-21 eighteen-cell opening, two courses over"),
		TEXT("the COVER counter-case: two courses (15 cm) over a 4.06 m opening is ")
		TEXT("1.2014 MPa of deep-beam bending, 1.71x the coded mean f_x1 — the catalogue's ")
		TEXT("ruled Collapse. Since the mean re-anchor BOTH models here stand it: the LP at ")
		TEXT("17.24, production at a worst reading of 0.9354 with nothing dropped — so this ")
		TEXT("row now measures the two of them agreeing against the ruled verdict, which is ")
		TEXT("the catalogue's inverted deliberate red, not this file's"),
		Scenario(TEXT("wall-21")),
		ERelation::AgreeStands, 17.23854, 17.23923, 0, 0 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	/* --- the cross-row pins, which no single row's window can make ------------------- */

	const FSweepReading* Loaded =
		ReadingNamed(*this, Rows, Readings, TEXT("wall-15 header with six courses on top"));
	const FSweepReading* Bare =
		ReadingNamed(*this, Rows, Readings, TEXT("wall-16 header at the top, nothing on it"));

	if (Loaded != nullptr && Bare != nullptr)
	{
		CheckSameLambda(
			*this,
			TEXT("the superimposed-load pair (wall-15 vs wall-16) under one live load"),
			*Loaded, *Bare, SameNumberRelativeTolerance);

		/* The production side must stay apart: the ratio is pinned wide enough to be a ratio, narrow
		 * enough that the pair collapsing to one fixture (which would satisfy the identity) cannot pass. */
		CheckReadingRatio(
			*this,
			TEXT("production separates the superimposed-load pair where the LP does not"),
			Bare->WorstUtilisation, Loaded->WorstUtilisation,
			SuperimposedReadingRatioLo, SuperimposedReadingRatioHi);
	}

	return true;
}

/**
 * The free-end height ladder: the first real measurement of the lambda = 3.464 ruling (DESIGN.md
 * §7 gap 5), its own test because the finding lives between the two heights. The fixture is
 * StructureFreeEndHeightTest's (a 7-cell running-bond wall with the grounded course's outermost
 * full brick removed) at 10 and 20 courses. The dense solver refused the 10-course wall (this
 * file's pinned canary) until the 2026-08-12 sparse rewrite answered it (256.820018).
 *
 * The characteristic-era finding: the LP read the same number at both heights (some shared local
 * feature binds, not the half seat, which would halve lambda*), pinned as an identity; production
 * was height-linear and crossed 1.0 between the two, so the 20-course wall dropped a piece. That
 * worst joint is not the composite-relieved seat (0.32828 at twenty courses); a candidate is the
 * patch over the seatless half bat (the decomposition test in CURRENT_STATE names it).
 *
 * Re-measured at the mean re-anchor (2026-08-14): both halves are now history.
 *   - The LP's height-identity died: 629.19922 at ten courses vs 297.99734 at twenty, ratio 2.1114,
 *     within 0.4% of production's 2.1198. The binding constraint is now strength-governed and scales
 *     with load; re-deriving it belongs with the step-4 promotion.
 *   - Production's crossing died with the free-end loss: same /7-anchor multiples (0.13193, 0.27967),
 *     ratio untouched, but both now under 1.0 with the crossing past ~412 courses. The 20-course wall
 *     no longer drops, so both rows read AgreeStands and the ladder keeps trend + agreement only.
 *
 * No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowFreeEndLadderTest,
	"OracleSweepFast.RigidBlock.FreeEndHeightLadder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowFreeEndLadderTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	Rows.Add({ TEXT("free end, 7 cells x 10 courses"),
		TEXT("the promoted canary: the LP prices the ten-course free end at 629.2x own ")
		TEXT("weight at the mean strengths while production's worst joint reads 0.132 ")
		TEXT("and holds"),
		[](FStructure& Out, FString& Why) { return BuildFreeEnd(10, Out, Why); },
		ERelation::AgreeStands, 629.187, 629.212, 0, 0 });

	Rows.Add({ TEXT("free end, 7 cells x 20 courses"),
		TEXT("the same wall twice as tall: since the mean re-anchor BOTH sides scale ")
		TEXT("with height (the LP 2.1114x, production 2.1199x) and both stand — the ")
		TEXT("characteristic-era height-flat LP and the production crossing are history ")
		TEXT("(header)"),
		[](FStructure& Out, FString& Why) { return BuildFreeEnd(20, Out, Why); },
		ERelation::AgreeStands, 297.991, 298.004, 0, 0 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* Short =
		ReadingNamed(*this, Rows, Readings, TEXT("free end, 7 cells x 10 courses"));
	const FSweepReading* Tall =
		ReadingNamed(*this, Rows, Readings, TEXT("free end, 7 cells x 20 courses"));

	if (Short == nullptr || Tall == nullptr)
	{
		return true;
	}

	/* The LP's height scaling as a ratio, replacing the retired height-identity (header):
	 * 629.19922 / 297.99734 = 2.1114, within 0.4% of production's 2.1198 and pinned separately so
	 * the two models' agreement about height stays a measured fact. */
	CheckReadingRatio(
		*this,
		TEXT("the LP's free-end lambda* now scales with height (7x10 over 7x20)"),
		Short->Oracle.Lambda, Tall->Oracle.Lambda,
		2.11122, 2.11163);

	/* Production's side as a ratio. The characteristic-era crossing between these heights is gone
	 * (moved past ~412 courses, CURRENT_STATE); what remains is that the reading is height-linear
	 * and both heights stand clear, which AgreeStands already demands. */
	CheckReadingRatio(
		*this,
		TEXT("production's free-end reading is height-linear"),
		Tall->WorstUtilisation, Short->WorstUtilisation,
		FreeEndReadingRatioLo, FreeEndReadingRatioHi);

	TestTrue(
		*FString::Printf(
			TEXT("production's free-end readings both sit under capacity on the mean basis ")
			TEXT("— 10 courses read %.17g and 20 courses read %.17g; the characteristic-era ")
			TEXT("crossing between these heights is retired, and either reading crossing 1.0 ")
			TEXT("again means the ladder's slope must be re-derived"),
			Short->WorstUtilisation, Tall->WorstUtilisation),
		Short->WorstUtilisation < 1.0 && Tall->WorstUtilisation < 1.0);

	return true;
}

/**
 * The case-21 mechanism ladders: the two discriminating experiments DESIGN §8's 2026-08-13 entry
 * specified, and the gate on evolution step 4. The LP stands case 21 (two courses of cover over a
 * 4.06 m opening) at lambda* = 5.511 where hand statics read 1.2014 MPa of bending and production
 * drops the cover; 5.511 is 66x what a tension-bond panel holds, so the LP prices something
 * unidentified. The first attribution (an arch springing from the grounded course) was refuted:
 * it predicts lambda* falling as cover grows where the measurement rises. Two ladders vary its two
 * terms one at a time.
 *
 *   Rise ladder: courses below the opening s=1..4, span and cover held. An arch gains rise per
 *   course (predicts 1.75x across the ladder); a cover-carried mechanism predicts flat.
 *   Abutment ladder: jambs 2 -> 4 cells, all else held. A jamb-reacting mechanism predicts ~2x;
 *   a cover-carried one flat.
 *
 * Characteristic-era measurements (2026-08-13; the 2026-08-14 re-anchor moved every lambda* and
 * killed the abutment sensitivity, see the note above the rows):
 *
 *     rung                       blocks  joints  lambda*              production
 *     rise s=1 / abutment j=2      83     133    5.5110095421575718   45 fall, 3 passes
 *     rise s=2                    104     193    4.8119163296983674   45 fall, 3 passes
 *     rise s=3                    128     264    5.5110085114812968   45 fall, 3 passes
 *     rise s=4                    149     324    4.8119026252913413   45 fall, 3 passes
 *     abutment j=3                 95     163    7.3790076229482224   49 fall, 4 passes
 *     abutment j=4                107     193    9.3587305816217992   53 fall, 5 passes
 *     abutment j=3, tail trimmed   89     147    6.2895385246488216   43 fall, 3 passes
 *     matched span 17 cells s=1    80     129    6.3681142090745144   43 fall, 3 passes
 *     matched span 17 cells s=2   100     186    5.4541091235793511   43 fall, 3 passes
 *
 * Findings. The rise ladder refutes the arch: held at one parity, depth does nothing (s=3/s=1 =
 * 0.99999981 against 1.50 predicted; s=4/s=2 = 0.99999715 against 1.40), so the LP does not know
 * the wall under the opening is there. The abutment ladder shows sensitivity (j=3/j=2 = 1.339,
 * j=4/j=2 = 1.698, between flat and bearing-proportional). The tail-trimmed j=3 rung shows both
 * bearing and cover-tail are live (1.339 -> 1.141 with the tail cut). The ~13% "parity" step is
 * the bearing span, not a third criterion: the matched-span pair (a 17-cell even-first cut on
 * case 21's own 383.50 cm reveal) collapses it to 0.98968 while flipping the cover parity, so ~11/12
 * is span. The mechanism is still unidentified; the ladders bought a smaller search space.
 *
 * What is pinned is the measurement, never the hypothesis: each rung a lambda* window and drop
 * count, each ladder a ratio, predictions printed beside each. The control (first row) is asserted
 * the same number as Scenario("wall-21") at the same-number tolerance; if the parameterisation
 * drifted one brick the control fails and every rung is void. Windows are +/-2e-5 (pivot-path-honest,
 * see WallsAndLadders); ratio windows ~1e-4. The j=4 rung, once a phase-2 refusal canary, is centred
 * on two certified readings 5.3e-7 apart after the 2026-08-13 relative pivot floor flipped it.
 *
 * Cost: ten solves, ~28 s. No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowOpeningLaddersTest,
	"OracleSweepFast.RigidBlock.OpeningMechanismLadders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowOpeningLaddersTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	const auto Rung =
		[](int32 OpeningCells, int32 CoursesBelow, int32 JambCells, int32 CoverTailCells)
	{
		return [OpeningCells, CoursesBelow, JambCells, CoverTailCells]
			(FStructure& Out, FString& Why)
		{
			return BuildOpeningLadderWall(
				OpeningCells, CoursesBelow, JambCells, CoverTailCells, Out, Why);
		};
	};

	TArray<FSweepRow> Rows;

	/*
	 * Re-measured at the mean re-anchor (2026-08-14); two things moved at once. (1) Production
	 * stands the whole family (the approved case-21 fallout): every drop pin is 0 and every
	 * relation AgreeStands. The even-parity rungs (s=2, s=4) newly break one joint in one pass and
	 * still drop nothing (worst reading 1.0433 vs the odd rungs' 0.9354, on an undecomposed axis;
	 * see wall-21 above). (2) The characteristic-era findings are history: parity step 0.8875 (was
	 * 0.8731), matched-span identity 1.00088, pure span step 1.1385, abutment sensitivity collapsed
	 * (j=3/j=2 = 1.0378, j=4/j=2 = 1.0514), depth-flatness held. So on the mean basis the mechanism
	 * is span-dominated and nearly abutment-blind, an open split in CURRENT_STATE. The pins below
	 * hold the mean-basis measurements; the header records the characteristic-era experiment.
	 */
	Rows.Add({ TEXT("wall-21 through the scenario catalogue"),
		TEXT("the equivalence control: the shipped case-21 fixture, laid the way every ")
		TEXT("verdict row in this file is laid, so the hand-parameterised rung below can be ")
		TEXT("held against it as the same number"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("wall-21"), Out, Why);
		},
		ERelation::AgreeStands, 17.23854, 17.23923, 0, 0 });

	Rows.Add({ TEXT("rise s=1 / abutment j=2 (case 21 itself)"),
		TEXT("both ladders' bottom rung and the equivalence control's partner — the same ")
		TEXT("83 blocks and 133 joints as the catalogue row above, which is what the ")
		TEXT("same-number pin below turns into an assertion"),
		Rung(18, 1, 2, INDEX_NONE),
		ERelation::AgreeStands, 17.23854, 17.23923, 0, 0 });

	Rows.Add({ TEXT("rise s=2"),
		TEXT("one more course under the opening: an arch from the ground gains 7.5 cm of ")
		TEXT("rise, a cover-carried mechanism gains nothing. It gains nothing — what moves ")
		TEXT("here is the REVEAL PARITY (an even-first opening), and the depth pins below ")
		TEXT("compare s=3 against s=1 and s=4 against s=2 to keep the two apart"),
		Rung(18, 2, 2, INDEX_NONE),
		ERelation::AgreeStands, 15.29968, 15.30030, 0, 0 });

	Rows.Add({ TEXT("rise s=3"),
		TEXT("two more courses under the opening, back on case 21's odd-first parity: the ")
		TEXT("same lambda* to 6.6e-6 relative at the mean data (1.9e-7 at the ")
		TEXT("characteristic)"),
		Rung(18, 3, 2, INDEX_NONE),
		ERelation::AgreeStands, 17.23865, 17.23934, 0, 0 });

	Rows.Add({ TEXT("rise s=4"),
		TEXT("three more courses under the opening: the springing is 22.5 cm deeper than ")
		TEXT("case 21's, which is 1.75x the rise the priced arch was fitted at, and lambda* ")
		TEXT("reads its parity partner s=2 to the last bit at the mean data"),
		Rung(18, 4, 2, INDEX_NONE),
		ERelation::AgreeStands, 15.29968, 15.30030, 0, 0 });

	Rows.Add({ TEXT("abutment j=3"),
		TEXT("half again the jamb, same cut span, same wall height, same reveal parity — ")
		TEXT("1.34x at the characteristic data, and only 1.04x at the mean: the abutment ")
		TEXT("sensitivity all but vanished at the re-anchor (the family's open mechanism ")
		TEXT("question — see the 2026-08-14 note above)"),
		Rung(18, 1, 3, INDEX_NONE),
		ERelation::AgreeStands, 17.89086, 17.89158, 0, 0 });

	Rows.Add({ TEXT("abutment j=4"),
		TEXT("twice the jamb: twice the bearing and twice the weight over it, with the ")
		TEXT("opening, the cover and the courses below all unmoved. This rung REFUSED in ")
		TEXT("phase 2 until 2026-08-13 and was pinned as a canary; the ratio test gained ")
		TEXT("its relative pivot floor and the canary flipped. The 2x test's mean-basis ")
		TEXT("answer is 1.05x — the bearing-proportional reading died with the re-anchor"),
		Rung(18, 1, 4, INDEX_NONE),
		ERelation::AgreeStands, 18.12397, 18.12469, 0, 0 });

	Rows.Add({ TEXT("abutment j=3, cover tail trimmed to two cells"),
		TEXT("THE DISAMBIGUATOR the abutment ladder needs: widening a jamb widens the ")
		TEXT("bearing AND lengthens the cover built in over it, and j=3 alone cannot say ")
		TEXT("which paid. At the mean data it lands between its neighbours again — 17.654 ")
		TEXT("against 17.239 and 17.891 — but the whole spread is now 3.8%, so what it ")
		TEXT("disambiguates is proportionally small (see the 2026-08-14 note above)"),
		Rung(18, 1, 3, 2),
		ERelation::AgreeStands, 17.65379, 17.65449, 0, 0 });

	/*
	 * The matched-span pair: the experiment separating the two things the rise ladder's 13%
	 * "parity" step confounds. s=2 reads 0.873 of s=1, but the two rungs differ in two ways: the
	 * cover bears on a reveal a full cell wider (406.00 vs 383.50 cm, span-squared predicts 0.892)
	 * and the cover's two courses are on the opposite bond parity. A 17-cell even-first cut presents
	 * the cover with case 21's own 383.50 cm, so held against the 18-cell s=1 rung the span is
	 * matched and the parity opposite: ratio 1.00 means parity buys nothing, 0.873 means the step
	 * survives. The fourth rung (17 cells s=1, 361.00 cm on the odd parity) makes it a 2x2, pricing
	 * pure span (span-squared predicts 1.128).
	 *
	 * Both rungs are 21 cells wide, so block counts (80, 100) differ from every 22-cell rung; the
	 * size pins refuse a rung that quietly built its partner's wall (TRAPS: window and ratio pins
	 * both passed under the recorded rung-flip; only the size pin caught it).
	 */
	Rows.Add({ TEXT("matched span: 17 cells, s=1"),
		TEXT("the pure SPAN step at fixed odd-first parity — one cell of opening off case ")
		TEXT("21, so the cover bears on 361.00 cm instead of 383.50 with the toothing and ")
		TEXT("the cover's bond parity unchanged. Span-squared predicts 1.128x case 21; the ")
		TEXT("mean data measures 1.1385 (the characteristic data measured 1.156)"),
		Rung(17, 1, 2, INDEX_NONE),
		ERelation::AgreeStands, 19.62579, 19.62658, 0, 0 });

	Rows.Add({ TEXT("matched span: 17 cells, s=2"),
		TEXT("THE MATCHED-SPAN RUNG: a 17-cell cut starting on an even course bears its ")
		TEXT("cover on the SAME 383.50 cm clear reveal as case 21's 18-cell odd-first one, ")
		TEXT("with the cover's two courses on the opposite bond parity. The span hypothesis ")
		TEXT("predicts ~1.00 against case 21 and the cover-bond-parity hypothesis the ")
		TEXT("parity step; the mean data measures 1.00088 (0.990 at the characteristic), ")
		TEXT("so the step IS the bearing span on either basis"),
		Rung(17, 2, 2, INDEX_NONE),
		ERelation::AgreeStands, 17.25377, 17.25446, 0, 0 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* Catalogue =
		ReadingNamed(*this, Rows, Readings, TEXT("wall-21 through the scenario catalogue"));
	const FSweepReading* S1 =
		ReadingNamed(*this, Rows, Readings, TEXT("rise s=1 / abutment j=2 (case 21 itself)"));
	const FSweepReading* S2 = ReadingNamed(*this, Rows, Readings, TEXT("rise s=2"));
	const FSweepReading* S3 = ReadingNamed(*this, Rows, Readings, TEXT("rise s=3"));
	const FSweepReading* S4 = ReadingNamed(*this, Rows, Readings, TEXT("rise s=4"));
	const FSweepReading* J3 = ReadingNamed(*this, Rows, Readings, TEXT("abutment j=3"));
	const FSweepReading* J4 = ReadingNamed(*this, Rows, Readings, TEXT("abutment j=4"));
	const FSweepReading* J3Trimmed = ReadingNamed(
		*this, Rows, Readings, TEXT("abutment j=3, cover tail trimmed to two cells"));
	const FSweepReading* W17S1 =
		ReadingNamed(*this, Rows, Readings, TEXT("matched span: 17 cells, s=1"));
	const FSweepReading* W17S2 =
		ReadingNamed(*this, Rows, Readings, TEXT("matched span: 17 cells, s=2"));

	if (Catalogue == nullptr || S1 == nullptr || S2 == nullptr || S3 == nullptr
		|| S4 == nullptr || J3 == nullptr || J4 == nullptr || J3Trimmed == nullptr
		|| W17S1 == nullptr || W17S2 == nullptr)
	{
		return true;
	}

	CheckSameLambda(
		*this,
		TEXT("the ladder's bottom rung IS case 21 (hand-parameterised vs the catalogue)"),
		*S1, *Catalogue, SameNumberRelativeTolerance);

	CheckRungSize(*this, TEXT("rise s=1 / abutment j=2"), *S1, 83);
	CheckRungSize(*this, TEXT("rise s=2"), *S2, 104);
	CheckRungSize(*this, TEXT("rise s=3"), *S3, 128);
	CheckRungSize(*this, TEXT("rise s=4"), *S4, 149);
	CheckRungSize(*this, TEXT("abutment j=3"), *J3, 95);
	CheckRungSize(*this, TEXT("abutment j=4"), *J4, 107);
	CheckRungSize(*this, TEXT("abutment j=3, cover tail trimmed"), *J3Trimmed, 89);
	CheckRungSize(*this, TEXT("matched span: 17 cells, s=1"), *W17S1, 80);
	CheckRungSize(*this, TEXT("matched span: 17 cells, s=2"), *W17S2, 100);

	/*
	 * The rise ladder's finding, in two pins that separate its two effects. The four rungs are not
	 * one series (parity note in BuildOpeningLadderWall: s=1/s=3 start odd, s=2/s=4 even, toothing
	 * the reveal differently), so it reads as two pins:
	 *
	 *   depth, at fixed parity   s=3 / s=1 = 0.99999981   s=4 / s=2 = 0.99999715
	 *   parity, at fixed depth   s=2 / s=1 = 0.87314607   s=4 / s=3 = 0.87314375
	 *
	 * The depth pins refute the arch: it predicts 1.50 and 1.40 (r = 30.0 -> 52.5 cm) where they
	 * measure 1.000000 to seven figures, so the mechanism does not know the wall below the opening
	 * is there. This refutes rise-priced mechanisms, not every jamb path (a jamb bed-joint reaction
	 * is area-governed and predicts flat too, separated by the abutment ladder). The parity pins are
	 * the only movement in the rise ladder, so a flat-ladder claim must survive them.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE RISE LADDER's depth, odd-parity: three courses below against one"),
		TEXT("arch-from-the-grounded-course predicts 1.50 (r 45.0 against 30.0 cm), ")
		TEXT("cover-carried and jamb-reaction both predict 1.00"),
		*S3, *S1, 0.99990, 1.00010);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE RISE LADDER's depth, even-parity: four courses below against two"),
		TEXT("arch-from-the-grounded-course predicts 1.40 (r 52.5 against 37.5 cm), ")
		TEXT("cover-carried and jamb-reaction both predict 1.00"),
		*S4, *S2, 0.99990, 1.00010);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE RISE LADDER's parity step, even-first opening against odd-first (s=2/s=1)"),
		TEXT("the ONLY movement in the rise ladder — the two parities bear the cover on ")
		TEXT("different clear reveals (383.50 vs 406.00 cm, a full cell of span), and ")
		TEXT("span-squared accounts for most of it ((383.5/406)^2 = 0.892 vs this 0.873). ")
		TEXT("SETTLED 2026-08-13 by the matched-span pair below: with the spans matched the ")
		TEXT("step falls to 0.990, so this is the reveal SPAN and not the cover's parity"),
		*S2, *S1, 0.88748, 0.88758);

	/*
	 * ================================================================================
	 * AND WHAT THE PARITY STEP TURNED OUT TO BE: THE SPAN, MEASURED 2026-08-13.
	 * ================================================================================
	 *
	 * The two 17-cell rungs answer the question the parity pins above could only pose.
	 * Predictions were recorded before the run; both are printed beside their measurement:
	 *
	 *     matched span (17,s=2)/(18,s=1)   span 1.00   parity 0.873   measured 0.98968
	 *     pure span    (17,s=1)/(18,s=1)   span^2 1.128               measured 1.15553
	 *
	 * The step collapses when the spans match. Two rungs whose covers are laid on
	 * opposite bond parities — even-then-odd against odd-then-even, so their head joints
	 * line up differently over the hole — and whose bearing reveals are the same 383.50 cm
	 * read within 1.03% of each other, against the 12.7% the same parity flip is worth when
	 * the span moves with it. So the reveal span carries roughly eleven twelfths of the
	 * step ((1 - 0.98968) / (1 - 0.87315) = 0.081 of it survives the span match), and that
	 * residue is an upper bound on the cover's bond parity rather than its measurement —
	 * the matched pair also differs by one course of wall height and one cell of width,
	 * though the depth ladder's 2e-7 flatness prices the course term at nothing. The TRAPS
	 * entry that called the step "mostly a reveal-span effect" was right and can now be
	 * stated as a measurement.
	 *
	 * Production says it independently and more sharply, which is why the reading pins
	 * below are part of this finding rather than bookkeeping: its worst joint on the
	 * matched-span rung is case 21's to the last bit (3.842883954008586 both), while the
	 * 361.00 cm rung reads 0.8949 of it. A downward-routing solver with no equilibrium and
	 * a limit-analysis LP agree that this fixture family is priced by the clear reveal the
	 * cover bears on, and by nothing else the parity carries.
	 *
	 * The span exponent is 2.39, not 2, and the two steps agree on it to two figures:
	 * 1.15553 over 383.5/361 is L^-2.391; the 18-cell parity step's 0.873146 over
	 * 406/383.5 is L^-2.378. Two independent cell-steps agreeing to two figures, both
	 * steeper than the span-squared a plain bending panel gives — a real property of
	 * whatever the LP is pricing, recorded as a measurement and not attributed to a
	 * mechanism here.
	 *
	 * The third ratio a 2x2 suggests — (17,s=2)/(17,s=1) = 0.85647 — is not pinned, and the
	 * omission is deliberate: it is exactly the quotient of the two pins below, so it can
	 * only fail when one of them already has, and a pin that cannot fail alone is a pin
	 * that dilutes the failure it appears in.
	 *
	 * Proven to bite by the file's recorded rung-flip, 2026-08-13: the matched-span rung
	 * built secretly as case 21 fires four assertions — the lambda window, the drop count,
	 * the block count and the matched-span ratio, which reads exactly the 1.00 the span
	 * hypothesis predicts. One thing it does not fire, and it is worth knowing: production's
	 * matched-span reading identity below passes under that flip, because production reads
	 * the two fixtures identically on purpose. The LP-side pins are what carry this finding;
	 * the production one corroborates it and cannot police it.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE MATCHED-SPAN PAIR: 17 cells at s=2 against case 21, both bearing 383.50 cm"),
		TEXT("the bearing-span hypothesis predicts 1.00 (parity buys nothing once the spans ")
		TEXT("match), the cover-bond-parity hypothesis predicts the 0.873 step survives"),
		*W17S2, *S1, 1.00083, 1.00094);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE PURE SPAN STEP at fixed odd-first parity: 361.00 cm against 383.50 cm"),
		TEXT("span-squared predicts 1.128 and a ladder that had simply stopped responding ")
		TEXT("predicts 1.00; without this rung a matched-span 1.00 could not be told from ")
		TEXT("an insensitive one"),
		*W17S1, *S1, 1.13843, 1.13854);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE RISE LADDER's parity step, repeated two courses deeper (s=4/s=3)"),
		TEXT("the same reveal-width step, measured again at a different depth: if the ")
		TEXT("parity step were really a depth effect in disguise, these two would differ"),
		*S4, *S3, 0.88747, 0.88757);

	/*
	 * The abutment ladder's finding, and the half the oracle refused to answer. j=3 reads 1.339
	 * against j=2, not flat (34% on a 50% wider jamb), so a cover-confined mechanism is refuted;
	 * it is short of the 1.50 a bearing-proportional reaction predicts, what a cohesion-plus-friction
	 * mix does.
	 *
	 * j=4 (the 2.00 test) refused in phase 2 at 107 blocks and was a canary until 2026-08-13. The
	 * refusal was the solver, not the fixture: the ratio test accepted pivots ~1e-16 of their column
	 * scale and read an unusable column as an unbounded ray. The fix is the relative pivot floor in
	 * RigidBlockOracle.cpp (the no-entry seam is now unreached, deliberately not repaired; the defect
	 * test's header has the story), and the canary flipped. Completed, j=4/j=2 = 1.698, between flat
	 * and bearing-proportional and decelerating (two cells bought 1.339, the next 1.268), what a
	 * cohesion-plus-friction mix does. The four answering rungs came back bit-identical across the
	 * repair, so only the readings that could not be taken moved.
	 *
	 * The trimmed rung makes j=3 mean something: widening the jamb widens the bearing and lengthens
	 * the cover tail at once, two mechanisms with one prediction the ladder cannot separate.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE ABUTMENT LADDER's step, three cells of jamb against two"),
		TEXT("a jamb-width-proportional reaction predicts ~1.50, a mechanism carried ")
		TEXT("between the reveals predicts 1.00"),
		*J3, *S1, 1.03779, 1.03789);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE ABUTMENT LADDER's 2x TEST: four cells of jamb against two"),
		TEXT("a bearing-proportional reaction predicts 2.00 and a mechanism carried ")
		TEXT("between the reveals predicts 1.00 — measured 1.698, between them and nearer ")
		TEXT("the first, which is the j=3 step's verdict restated over a doubled jamb"),
		*J4, *S1, 1.05131, 1.05141);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE DISAMBIGUATOR: the three-cell jamb with its cover tail trimmed back"),
		TEXT("if the jamb's bearing bought the 1.34, this stays near 1.34; if the longer ")
		TEXT("cover tail bought it, this falls back toward 1.00 — measured 1.141, which is ")
		TEXT("NEITHER, so both are live and this is the pin that says so"),
		*J3Trimmed, *S1, 1.02404, 1.02414);

	/*
	 * The same measurement the other way round (both pins fail differently): the trimmed rung
	 * against the untrimmed j=3 states how much of the wider jamb's gain the trim gives back
	 * (0.339 over j=2 falls to 0.141, so about half went with the cover tail). Caveat: trimming
	 * the tail also removes precompression from the jamb bed joints, so it understates the bearing's
	 * share by whatever friction contributes; cohesion dominates the mix (0.2 vs 0.6 x ~0.018), so
	 * the understatement is small but not zero, hence "comparable halves" not a number.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE DISAMBIGUATOR against the untrimmed jamb (trimmed j=3 / j=3)"),
		TEXT("all-bearing predicts 1.00 (the trim would change nothing), all-tail predicts ")
		TEXT("0.747 (the whole gain given back)"),
		*J3Trimmed, *J3, 0.98670, 0.98680);

	/*
	 * Production's reading on the same two splits, because a ladder only the LP sees is a ladder
	 * about the LP. Production is indifferent to depth to the last bit (its worst joint is in the
	 * cover) and sees the reveal parity as the LP does, so the parity step is a fixture property,
	 * not the simplex's.
	 */
	CheckReadingRatio(
		*this,
		TEXT("production's worst reading does not move with depth either (s=3 / s=1)"),
		S3->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("production sees the reveal parity the LP sees (s=2 / s=1)"),
		S2->WorstUtilisation, S1->WorstUtilisation, 1.11531, 1.11543);

	/*
	 * Production is blind to the jamb, the one net the six lambda pins cannot cast. Every pin above
	 * watches the LP; production's drop counts move only because a wider wall has a wider cover
	 * (45/49/53/43). What it read is the striking half: where the LP moves 34% per cell of jamb and
	 * refuses the next, production's worst joint does not move in any digit on any rung, because that
	 * reading is in the cover and the masonry the load reacts into is not a term in it. The two
	 * methods disagree about whether an abutment exists at all, which DESIGN §7 step 4 must resolve;
	 * until then this fires if production's side changes while every lambda pin stays green.
	 *
	 * Pinned at 1e-9 as ratios (four readings are one number, stronger than four windows). The
	 * trimmed rung is included deliberately: it changes what falls without changing what is read.
	 * Proven to bite 2026-08-13 by scaling production's utilisation by (1 + 1e-6 x NumPieces()): it
	 * fired exactly five assertions while every window, ratio and count stayed green.
	 */
	CheckReadingRatio(
		*this,
		TEXT("production is blind to the jamb: three cells against two (j=3 / j=2)"),
		J3->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("production is blind to the jamb: four cells against two (j=4 / j=2)"),
		J4->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("production is blind to the cover tail as well (trimmed j=3 / j=2)"),
		J3Trimmed->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	/*
	 * Production's half of the matched-span finding, the sharpest number here: two rungs one brick
	 * different in width, one course in height and on opposite cover parities read the same worst
	 * joint to the last bit because both bear on 383.50 cm, while the 361.00 cm rung reads 0.8949 of
	 * it. Pinned as an identity plus a stay-apart ratio (TRAPS: an identity alone passes when two
	 * rows have quietly become one fixture).
	 */
	CheckReadingRatio(
		*this,
		TEXT("production prices the matched-span pair identically (17@s=2 / case 21)"),
		W17S2->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("and separates the span step it is matched against (17@s=1 / case 21)"),
		W17S1->WorstUtilisation, S1->WorstUtilisation, 0.89065, 0.89076);

	return true;
}

/**
 * The case-21 pricing experiments: the first probes to vary a strength rather than the geometry,
 * the closing experiment CURRENT_STATE specified after the mechanism ladders refuted both candidate
 * mechanisms. The ladders narrowed the search to a mechanism flat below the opening and sensitive
 * to jamb bearing and cover tail, but two candidates still fit (a thrust confined to the cover bled
 * into the jamb bed joints as Coulomb shear, and a sill-springing arch), and no geometry rung
 * distinguishes them. A strength probe can reach which capacity is binding: hold the fixture still
 * and take one strength away at a time. FOracleProblem carries per-joint strengths, so the override
 * is eleven lines here and nothing in the oracle changed (an instrument left untouched during its
 * own measurement). Production is the control, asserted at 1e-9 identical to the control's reading.
 *
 * Predictions, written before the run:
 *   Probe 1, jamb bed cohesion to zero. If the shear chain binds, lambda* collapses (cohesion
 *   0.2 MPa vs mu x precompression ~0.6 x 0.018, ~18x). It does not separate the two shapes: a sill
 *   arch's reaction is also horizontal thrust into these same joints (a review correction).
 *   Probes 2/3, cover head-joint tension halved and doubled. A bending panel reads 4.00x apart;
 *   expected flat (5.511 is 66x the panel's capacity), so the joint-count pin is load-bearing.
 *
 * Measured 2026-08-13 (characteristic era; the mean re-anchor changed probe 1, see the row note):
 *     row                            joints  lambda*              vs control
 *     control (as built)                 0   5.5110095421575718     1.00000
 *     jamb bed cohesion zeroed          36   0.78511681229249863    0.14246
 *     cover head tension x0.5           43   5.2459912546514023     0.95191
 *     cover head tension x2             43   6.0401281381456675     1.09601
 *
 * Probe 1 took lambda* below 1 (5.511 -> 0.785, x7.02): with jamb bed cohesion gone the wall falls,
 * so the whole case-21 disagreement is bought by one capacity (this row is AgreeFalls where every
 * other case-21 row is oracle-stands/production-falls). Read carefully: this is about a wall the
 * other methods never judged, not a three-method agreement on case 21 (still the open call). Bed
 * cohesion is horizontal shear capacity, so the mechanism delivers thrust into the jamb bed joints
 * priced by their cohesion, consistent with all three ladders; it still does not pick between the
 * two thrust-line shapes. Probes 2/3 came out flat (4x change moves lambda* 15%), so the cover's
 * tensile bond is minor; the 43-joint pin makes the flat answer trustworthy.
 *
 * Both probes proven to bite, failing differently (count pin and ratio pins are not one net): a
 * neutered selector fails the count/relation/window/ratio with lambda* bit-identical to control; a
 * neutered factor fails both windows and ratios while the count passes (it touched 43 joints, wrote
 * the same number). One limit of probe 1: it zeroes all 36 joints at once, so it cannot say whether
 * the bearing or the run to ground is tighter (a split left for CURRENT_STATE). Slice 0d added a
 * fifth row (first-crack on: lambda* falls 12.3% to 15.117 and stands, the §4 confirmation that bond
 * bending is minor), with the same guards plus its own move ratio.
 *
 * Cost: five solves at 83 blocks, ~6 s. No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowOpeningProbesTest,
	"OracleSweepFast.RigidBlock.OpeningStrengthProbes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowOpeningProbesTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	/** Every row is case 21's own wall — s = 1, j = 2, 18 cells, untrimmed cover. */
	const auto CaseTwentyOne = [](FStructure& Out, FString& Why)
	{
		return BuildOpeningLadderWall(18, 1, 2, INDEX_NONE, Out, Why);
	};

	TArray<FSweepRow> Rows;

	Rows.Add({ TEXT("probe control: case 21 with every strength as built"),
		TEXT("the baseline every probe below is read against, and the row that says the ")
		TEXT("override machinery is inert when nothing asks it to do anything — same wall, ")
		TEXT("same 83 blocks, the lambda* the mechanism ladders pin, and ZERO joints ")
		TEXT("overridden, pinned rather than implied"),
		CaseTwentyOne,
		ERelation::AgreeStands, 17.23854, 17.23923, 0, 0, nullptr, 0 });

	/*
	 * Re-measured at the mean re-anchor (2026-08-14). The characteristic-era finding (jamb bed
	 * cohesion is the whole capacity, its removal collapses lambda* past 1.0 to 0.785, the family's
	 * one AgreeFalls) does not survive: zeroing the same 36 joints now leaves lambda* at 4.768
	 * (0.277 of control) and the wall stands, because the strengths the probe leaves (bond tension
	 * 0.70) carry it. So cohesion is the dominant single term (72%) but not the whole capacity, the
	 * AgreeFalls arm is dead, and the mean-basis split is open in CURRENT_STATE. The 2026-08-13
	 * ruling is unchanged.
	 */
	Rows.Add({ TEXT("probe: jamb bed joints, cohesion zeroed"),
		TEXT("THE CLOSING EXPERIMENT, re-derived at the mean basis. Every bed joint below ")
		TEXT("the cover loses its cohesion and keeps its friction and its tension. At the ")
		TEXT("characteristic data that collapsed lambda* past 1.0 (0.785 — the wall fell); ")
		TEXT("at the mean data it reads 4.768, a factor of 0.277 of the control: the ")
		TEXT("dominant single term, no longer the whole capacity, and the modified wall ")
		TEXT("stands"),
		CaseTwentyOne,
		ERelation::AgreeStands, 4.76789, 4.76808, 0, 0,
		[](FOracleProblem& Problem) { return ZeroJambBedCohesion(1, Problem); }, 36 });

	Rows.Add({ TEXT("probe: cover head-joint tension x0.5"),
		TEXT("half the tensile bond across the cover's own head joints, nothing else ")
		TEXT("touched — the knob a deep bending panel's capacity is linear in. Halving it ")
		TEXT("costs 2.7% at the mean data (4.8% at the characteristic), where a panel ")
		TEXT("would lose half"),
		CaseTwentyOne,
		ERelation::AgreeStands, 16.76947, 16.77014, 0, 0,
		[](FOracleProblem& Problem)
		{
			return ScaleCoverHeadTension(1, 0.5, Problem);
		}, 43 });

	Rows.Add({ TEXT("probe: cover head-joint tension x2"),
		TEXT("and twice it, so the pair spans a factor of four: a cover carrying its span ")
		TEXT("in bond tension reads 4.00x across the two, and anything else reads 1.00. It ")
		TEXT("reads 1.068 at the mean data (1.151 at the characteristic) — the bond ")
		TEXT("contributes, and it is not what holds the wall up"),
		CaseTwentyOne,
		ERelation::AgreeStands, 17.90343, 17.90415, 0, 0,
		[](FOracleProblem& Problem)
		{
			return ScaleCoverHeadTension(1, 2.0, Problem);
		}, 43 });

	/*
	 * Slice 0d: the first-crack flag-on window for case 21, the §4 confirmation. With uncracked
	 * first-crack rows on every bonded joint (133, the whole wall), lambda* falls only 12.3% to
	 * 15.116881825572943 and the wall stands. The brittleness rule does not close the case-21
	 * disagreement: the residual is the friction+compression mechanism Slice 0c attributed, which
	 * first crack does not touch. Recorded not smoothed: the prediction guessed up to 3x; measured
	 * 12.3%, so bond tension is even more minor than estimated. OverriddenJoints = 133 pins a row
	 * for every bonded joint. The window is ~+/-2e-5, matching the sibling probes so a future
	 * pivot-path change flaps them together.
	 */
	Rows.Add({ TEXT("probe: first-crack rows on"),
		TEXT("Slice 0d's flag-on window: the whole wall re-solved with uncracked first-crack ")
		TEXT("rows on every bonded joint. lambda* falls only 12.3% (0.8769) to 15.117 and the ")
		TEXT("wall STANDS — brittle bond bending is a minor term here, the §4 confirmation; it ")
		TEXT("does not flip the verdict"),
		CaseTwentyOne,
		ERelation::AgreeStands, 15.11658, 15.11718, 0, 0,
		TurnOnFirstCrackRows, 133 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* Control = ReadingNamed(
		*this, Rows, Readings, TEXT("probe control: case 21 with every strength as built"));
	const FSweepReading* NoCohesion = ReadingNamed(
		*this, Rows, Readings, TEXT("probe: jamb bed joints, cohesion zeroed"));
	const FSweepReading* HalfTension = ReadingNamed(
		*this, Rows, Readings, TEXT("probe: cover head-joint tension x0.5"));
	const FSweepReading* DoubleTension = ReadingNamed(
		*this, Rows, Readings, TEXT("probe: cover head-joint tension x2"));
	const FSweepReading* FirstCrack = ReadingNamed(
		*this, Rows, Readings, TEXT("probe: first-crack rows on"));

	if (Control == nullptr || NoCohesion == nullptr || HalfTension == nullptr
		|| DoubleTension == nullptr || FirstCrack == nullptr)
	{
		return true;
	}

	/*
	 * The first-crack row is the same 83-block wall and touched the LP only, so it takes the same
	 * two guards as the strength probes: same rung size, and a bit-identical production reading
	 * against the control.
	 */
	CheckRungSize(*this, TEXT("probe: first-crack on"), *FirstCrack, 83);

	CheckReadingRatio(
		*this,
		TEXT("the first-crack flag changed the LP only (production reading vs control)"),
		FirstCrack->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);

	/*
	 * The §4 confirmation, as a ratio: case 21 with first crack on falls to 0.8769 of the
	 * control and stays above 1.0. A panel spanning in brittle bond bending would third
	 * (0.3333); case 21 barely moves, the measured statement that bond bending is a minor
	 * term and the residual is friction+compression (Slice 0c), not brittle bond. The
	 * verdict does not flip — the finding PROMOTION_DESIGN Sec 4 set this fixture up to
	 * make.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("SLICE 0d: case 21 under first crack STANDS and does not flip"),
		TEXT("a bond-bending panel predicts 0.3333 (/3); case 21 measures 0.8769 and stays ")
		TEXT("above 1.0 — bond bending is minor, the residual is friction+compression"),
		*FirstCrack, *Control, 0.87689, 0.87692);

	/*
	 * All four rows are one wall, so all four bridge to the same 83 blocks. A probe row
	 * that quietly built a different fixture would otherwise report a moved lambda* and be
	 * read as a strength finding.
	 */
	CheckRungSize(*this, TEXT("probe control"), *Control, 83);
	CheckRungSize(*this, TEXT("probe: cohesion zeroed"), *NoCohesion, 83);
	CheckRungSize(*this, TEXT("probe: tension x0.5"), *HalfTension, 83);
	CheckRungSize(*this, TEXT("probe: tension x2"), *DoubleTension, 83);

	/*
	 * And all four production readings are one number, because the override never touches
	 * the structure. This is the control that makes a moved lambda* attributable: if a
	 * probe row's cascade reading moves, the fixture moved and the LP's number means
	 * nothing.
	 */
	CheckReadingRatio(
		*this,
		TEXT("the cohesion probe changed the LP only (production reading vs control)"),
		NoCohesion->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("the x0.5 tension probe changed the LP only (production reading vs control)"),
		HalfTension->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("the x2 tension probe changed the LP only (production reading vs control)"),
		DoubleTension->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);

	/*
	 * ================================================================================
	 * THE TWO FINDINGS, AS RATIOS — because each row's window pins where a number is and
	 * only a ratio pins what the experiment MEASURED.
	 * ================================================================================
	 *
	 * A solver change that moved every lambda* in this file by one factor would break four
	 * windows and leave both statements below intact, which is the split wanted: the
	 * windows are the arithmetic and the ratios are the physics.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE CLOSING EXPERIMENT: case 21 with its jamb bed cohesion taken away"),
		TEXT("the jamb-shear-chain hypothesis predicts a collapse toward the residue, ")
		TEXT("anything else predicts 1.00 — the mean data measures 0.2766 (0.1425 at the ")
		TEXT("characteristic, where it also crossed 1.0 absolute): the dominant single ")
		TEXT("term, no longer the whole capacity"),
		*NoCohesion, *Control, 0.27657, 0.27660);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE COVER'S OWN BOND, over a four-fold lever (tension x2 / tension x0.5)"),
		TEXT("a cover spanning in bond tension predicts 4.00, a cover whose tension is ")
		TEXT("incidental predicts 1.00 — the mean data measures 1.068 (1.151 at the ")
		TEXT("characteristic), so the bond contributes little of what carrying the span ")
		TEXT("would require"),
		*DoubleTension, *HalfTension, 1.06757, 1.06767);

	/*
	 * The low half of the lever alone. The four-fold ratio above would also pass for a solver that
	 * had stopped responding to strengths; this says the x0.5 rung sits 2.7% under control, so the
	 * knob is connected. Its partner (x2 / control) is the quotient of these two, not pinned again.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("the low half of the tension lever (tension x0.5 / control)"),
		TEXT("a bond-governed cover predicts 0.50, an unaffected one 1.00 — the mean data ")
		TEXT("measures 0.9728 (0.952 at the characteristic)"),
		*HalfTension, *Control, 0.97274, 0.97284);

	return true;
}

/**
 * Slice 0c: attribute case 21's residual lambda* = 4.768 to its physical cause. The residual (the
 * lambda* that survives when jamb bed cohesion is zeroed, pinned by OpeningStrengthProbes) is
 * re-solved with the jamb's friction zeroed, its crushing cap relaxed, both, and with the cohesion
 * removal split into the top bearing courses vs the run to ground, so the readings say which
 * mechanism holds it up.
 *
 * PROMOTION_DESIGN §4.4 makes attributing the 4.768 a precondition on promoting the LP to cascade
 * authority. The mean re-anchor refuted the 2026-08-13 cohesion-chain account (zeroing cohesion
 * still stands at 4.768), and §4.2 asserts the residual is friction and compression, both
 * non-brittle. This measures that in the shape §4.4 names: three probes plus the split.
 *
 * Predictions kept beside what was measured (mean GeneralPurposeMortar: mu=0.75, cap=2.0,
 * crushing=10, tension=0.7 MPa; self-weight bed compression ~0.018 MPa):
 *
 *   Probe 1 (friction also zeroed). Predicted falls; measured rises to 9.9349181343793767 and
 *     stands. mu=0 with cohesion already gone forbids all bed-joint shear and lifts the n+ >= n-
 *     compression coupling, so the 0.70 MPa flexural tensile bond (left intact) becomes usable and
 *     the wall re-forms on a distinct bond-tension mechanism 2.08x higher. Provable: a net-compression
 *     mu=0 optimum would be feasible for the control and bounded by 4.768, so 9.935 needs net tension
 *     somewhere; probe 5 removes that bond and collapses it.
 *   Probe 2 (crushing relaxed). Confirmed unchanged: 4.7679865256023373 vs control's
 *     4.7679865256023284 (1.86e-15). Crushing sits ~500x clear; asserted as an identity against
 *     control (a binding cap relaxed could only raise lambda*). Inert.
 *   Probe 3 (both). Equal to probe 1 (9.9349181343793749, 1.79e-16): once friction is gone the
 *     crushing cap changes nothing. Asserted as an identity against probe 1.
 *   Chain-vs-bearing split (cohesion zeroed one band at a time). Predicted the run to ground is the
 *     tighter link; measured the opposite: bearing courses only reads 4.76816 (the whole residual),
 *     run to ground only 5.96237, so removing bearing cohesion collapses it more. For cohesion
 *     specifically the bearing courses are the tighter link.
 *   Probe 5 (friction and jamb-bed tension both zeroed). Proves the 9.935 is bond tension: with
 *     shear and tension both forbidden, lambda* collapses to 3.7471595015086909, below even the
 *     control (a compression-only bed joint is a strict subset of the mu=0.75 control).
 *
 * Every window is a measured pin at the file's bit-window discipline (~2e-5). The two identities
 * prove crushing inert; the friction-raises-lambda headline is asserted as a mechanism inequality,
 * not two floats. No new oracle seam (every probe rewrites FOracleProblem after the bridge), so the
 * chain-vs-bearing split is measured by spatial strength probes rather than a per-joint force seam.
 *
 * Cost: seven solves at 83 blocks, ~8 s. No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowCase21ResidualTest,
	"OracleSweepFast.RigidBlock.Case21ResidualAttribution",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowCase21ResidualTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	/** Every row is case 21's own wall — s = 1, j = 2, 18 cells, untrimmed cover. */
	const auto CaseTwentyOne = [](FStructure& Out, FString& Why)
	{
		return BuildOpeningLadderWall(18, 1, 2, INDEX_NONE, Out, Why);
	};

	/** The full below-cover band: every jamb bed joint, the 36 the cohesion probe touches. */
	const double WholeChainLoZCm = -1.0e9;
	const double WholeChainHiZCm = 1.0e9;
	const double BearingSplitZCm = LadderChainBearingSplitZCm(1);

	TArray<FSweepRow> Rows;

	/*
	 * The residual control: not the as-built wall (17.24) but case 21 with its jamb bed cohesion
	 * gone, the 4.768 state this test attributes. Every probe is read against it. Its value is the
	 * one OpeningStrengthProbes pins, so this is a known anchor, not a prediction.
	 */
	Rows.Add({ TEXT("residual control: case 21 with jamb bed cohesion zeroed"),
		TEXT("the 4.768 residual itself — jamb bed cohesion gone, everything else as built. ")
		TEXT("The state the three probes strip further and the two split rows re-pose; its ")
		TEXT("lambda* is the value OpeningStrengthProbes already pins"),
		CaseTwentyOne,
		ERelation::AgreeStands, 4.76789, 4.76808, 0, 0,
		[WholeChainLoZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, false, false, false, WholeChainLoZCm, WholeChainHiZCm, Problem);
		}, 36 });

	/*
	 * Probe 1, friction also zeroed. The headline. Predicted falls; measured it rises to
	 * 9.9349181343793767 and stands, not because friction capped a compression arch: mu=0 with
	 * cohesion gone forbids all bed-joint shear and lifts the n+ >= n- compression coupling, so the
	 * 0.70 MPa tensile bond (left intact) becomes usable and the wall re-forms on a distinct
	 * bond-tension mechanism (2.08x). The mechanism inequality is asserted below; probe 5 names the
	 * higher mechanism as bond tension.
	 */
	Rows.Add({ TEXT("probe 1: friction also zeroed on the whole jamb chain"),
		TEXT("cohesion AND the Coulomb mu gone from every jamb bed joint. lambda* RISES from ")
		TEXT("4.768 to 9.935: disallowing bed-joint shear lifts the compression coupling and ")
		TEXT("unlocks the 0.70 MPa tensile bond (left intact here), so the wall re-forms on a ")
		TEXT("DISTINCT, higher bond-tension mechanism — friction did not cap a compression ")
		TEXT("arch. CONFIRMS PROMOTION_DESIGN §4.2's non-brittle residual; probe 5 proves the ")
		TEXT("9.935 is bond tension"),
		CaseTwentyOne,
		ERelation::AgreeStands, 9.93490, 9.93494, 0, 0,
		[WholeChainLoZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, true, false, false, WholeChainLoZCm, WholeChainHiZCm, Problem);
		}, 36 });

	/*
	 * Probe 2, crushing relaxed. Measured unchanged: 4.7679865256023373, bit-identical to the
	 * control (1.86e-15). Crushing (10 MPa) is ~500x clear of demand. The same-number identity below
	 * is the real claim (it fails if crushing binds, since relaxing a binding cap raises lambda*).
	 */
	Rows.Add({ TEXT("probe 2: crushing cap relaxed on the whole jamb chain"),
		TEXT("cohesion gone and the crushing cap lifted to effectively infinite, friction ")
		TEXT("left in place. If compression bearing is what the residual runs on and the ")
		TEXT("joints are crushing-limited, lambda* rises; PREDICTED UNCHANGED because ")
		TEXT("crushing sits ~500x clear of demand"),
		CaseTwentyOne,
		ERelation::AgreeStands, 4.76789, 4.76808, 0, 0,
		[WholeChainLoZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, false, true, false, WholeChainLoZCm, WholeChainHiZCm, Problem);
		}, 36 });

	/*
	 * Probe 3, both. Measured equal to probe 1: 9.9349181343793749 vs 9.9349181343793767 (1.79e-16).
	 * With friction gone the bed joints carry no shear whatever the cap, so relaxing crushing changes
	 * nothing. The real claim is the same-number identity against probe 1 below.
	 */
	Rows.Add({ TEXT("probe 3: friction zeroed AND crushing relaxed on the whole jamb chain"),
		TEXT("both non-cohesive mechanisms removed at once. Reads probe 1's 9.935 value — a ")
		TEXT("friction-freed arch does not care how high the crushing cap is; the identity ")
		TEXT("against probe 1 is the finding"),
		CaseTwentyOne,
		ERelation::AgreeStands, 9.93490, 9.93494, 0, 0,
		[WholeChainLoZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, true, true, false, WholeChainLoZCm, WholeChainHiZCm, Problem);
		}, 36 });

	/*
	 * The chain-vs-bearing split: cohesion zeroed only in the top two bearing courses, then only in
	 * the run to ground. Each band touches 18 of the chain's 36 joints (a pinned partition). The
	 * bearing band drops lambda* to 4.7681626950893188 (the whole residual) while run-to-ground only
	 * reaches 5.9623697933567943, so the bearing courses are the tighter link for cohesion, refuting
	 * "thrust runs to ground" for the cohesion term (friction localises differently; the two
	 * attributions stay distinct).
	 */
	Rows.Add({ TEXT("split: cohesion zeroed in the bearing courses only"),
		TEXT("cohesion removed only from the top of the jamb, where the cover bears. Reads ")
		TEXT("4.76816 — essentially the whole 4.768 residual — so the bearing courses are ")
		TEXT("the tighter link for cohesion, NOT the run to ground. Refutes 'thrust runs to ")
		TEXT("ground' for the cohesion contribution specifically"),
		CaseTwentyOne,
		ERelation::AgreeStands, 4.76814, 4.76818, 0, 0,
		[BearingSplitZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, false, false, false, BearingSplitZCm, WholeChainHiZCm, Problem);
		}, 18 });

	Rows.Add({ TEXT("split: cohesion zeroed in the run to ground only"),
		TEXT("cohesion removed only from the lower jamb, its run to the foundation. Reads ")
		TEXT("5.96237 — a WEAKER effect than the bearing band, refuting the prediction that ")
		TEXT("the run to ground is the tighter link for cohesion. Removing cohesion here ")
		TEXT("leaves more of the residual standing than removing it in the bearing courses"),
		CaseTwentyOne,
		ERelation::AgreeStands, 5.96235, 5.96239, 0, 0,
		[WholeChainLoZCm, BearingSplitZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, false, false, false, WholeChainLoZCm, BearingSplitZCm, Problem);
		}, 18 });

	/*
	 * Probe 5, friction and the jamb-bed tensile bond both zeroed. The empirical proof that 9.935
	 * is bond tension. Probe 1 left the 0.70 MPa bond intact and the wall re-formed on it; this
	 * zeroes both, so the bed joints carry compression only and the bond-tension mechanism cannot
	 * form. Predicted lambda* <= 4.768 (a compression-only bed joint is a strict subset of the
	 * mu=0.75 control); measured 3.7471595015086909, below even the control (which kept its
	 * friction-shear). Asserted as a bit-window and as a strict inequality against probe 1 (finding
	 * 5). No closed form for 9.935 without a per-joint force seam, so the ceiling stays a window.
	 */
	Rows.Add({ TEXT("probe 5: friction AND jamb-bed tension both zeroed on the whole jamb chain"),
		TEXT("cohesion, the Coulomb mu AND the 0.70 MPa tensile bond all gone from every jamb ")
		TEXT("bed joint — compression only. The bond-tension mechanism probe 1 unlocked cannot ")
		TEXT("form, so lambda* COLLAPSES from 9.935 to 3.74716 — below even the 4.768 frictional ")
		TEXT("residual (which kept its friction-shear). Empirical proof the 9.935 was bond tension"),
		CaseTwentyOne,
		ERelation::AgreeStands, 3.74714, 3.74718, 0, 0,
		[WholeChainLoZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, true, false, true, WholeChainLoZCm, WholeChainHiZCm, Problem);
		}, 36 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* Control = ReadingNamed(
		*this, Rows, Readings,
		TEXT("residual control: case 21 with jamb bed cohesion zeroed"));
	const FSweepReading* FrictionOff = ReadingNamed(
		*this, Rows, Readings,
		TEXT("probe 1: friction also zeroed on the whole jamb chain"));
	const FSweepReading* CrushingOff = ReadingNamed(
		*this, Rows, Readings,
		TEXT("probe 2: crushing cap relaxed on the whole jamb chain"));
	const FSweepReading* Both = ReadingNamed(
		*this, Rows, Readings,
		TEXT("probe 3: friction zeroed AND crushing relaxed on the whole jamb chain"));
	const FSweepReading* Bearing = ReadingNamed(
		*this, Rows, Readings,
		TEXT("split: cohesion zeroed in the bearing courses only"));
	const FSweepReading* RunToGround = ReadingNamed(
		*this, Rows, Readings,
		TEXT("split: cohesion zeroed in the run to ground only"));
	const FSweepReading* FrictionAndTensionOff = ReadingNamed(
		*this, Rows, Readings,
		TEXT("probe 5: friction AND jamb-bed tension both zeroed on the whole jamb chain"));

	if (Control == nullptr || FrictionOff == nullptr || CrushingOff == nullptr
		|| Both == nullptr || Bearing == nullptr || RunToGround == nullptr
		|| FrictionAndTensionOff == nullptr)
	{
		return true;
	}

	/* All seven rows are one wall — all must bridge to the same 83 blocks. */
	CheckRungSize(*this, TEXT("residual control"), *Control, 83);
	CheckRungSize(*this, TEXT("probe 1 friction off"), *FrictionOff, 83);
	CheckRungSize(*this, TEXT("probe 2 crushing relaxed"), *CrushingOff, 83);
	CheckRungSize(*this, TEXT("probe 3 both"), *Both, 83);
	CheckRungSize(*this, TEXT("split bearing"), *Bearing, 83);
	CheckRungSize(*this, TEXT("split run to ground"), *RunToGround, 83);
	CheckRungSize(*this, TEXT("probe 5 friction and tension off"), *FrictionAndTensionOff, 83);

	/*
	 * The override touches the LP only, so every production reading must equal the control's; a
	 * moved reading would mean the fixture moved and the LP's number is uninterpretable.
	 */
	CheckReadingRatio(
		*this, TEXT("probe 1 changed the LP only (production vs control)"),
		FrictionOff->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);
	CheckReadingRatio(
		*this, TEXT("probe 2 changed the LP only (production vs control)"),
		CrushingOff->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);
	CheckReadingRatio(
		*this, TEXT("probe 5 changed the LP only (production vs control)"),
		FrictionAndTensionOff->WorstUtilisation, Control->WorstUtilisation,
		0.999999999, 1.000000001);
	CheckReadingRatio(
		*this, TEXT("the split rows changed the LP only (bearing vs run-to-ground reading)"),
		Bearing->WorstUtilisation, RunToGround->WorstUtilisation, 0.999999999, 1.000000001);

	/*
	 * The split selector is honest: the two bands partition the chain (counts sum to the control's
	 * 36, neither empty). Each band's own count (18) is pinned in its row; this is the complementary
	 * partition check, catching a band that matched nothing or a pair that overlapped.
	 */
	TestEqual(
		TEXT("the two split bands must partition the whole jamb chain: bearing + run == 36"),
		Bearing->JointsOverridden + RunToGround->JointsOverridden, 36);
	TestTrue(
		TEXT("the bearing band must have matched at least one jamb bed joint"),
		Bearing->JointsOverridden > 0);
	TestTrue(
		TEXT("the run-to-ground band must have matched at least one jamb bed joint"),
		RunToGround->JointsOverridden > 0);

	/*
	 * Finding 1: crushing does not bind the residual. The identity is tighter than probe 2's window:
	 * relaxing a non-binding cap leaves lambda* bit-stable, a binding one raises it.
	 */
	CheckSameLambda(
		*this,
		TEXT("crushing relaxed vs the residual control"),
		*CrushingOff, *Control, 1.0e-6);

	/*
	 * Finding 2: crushing cannot revive a friction-killed residual. Probe 3 must read probe 1
	 * (once friction is gone the bed joints carry no shear whatever the cap). A break would say the
	 * two mechanisms interact, which nothing predicts.
	 */
	CheckSameLambda(
		*this,
		TEXT("both (friction off + crushing relaxed) vs friction off alone"),
		*Both, *FrictionOff, 1.0e-6);

	/*
	 * Finding 3: zeroing friction unlocks a distinct, higher bond-tension mechanism. The headline,
	 * asserted as a mechanism inequality not two floats: zeroing friction on the cohesionless chain
	 * raises lambda* (4.768 -> 9.935), not because friction capped an arch but because mu=0 forbids
	 * bed-joint shear and lifts the compression coupling, so the 0.70 MPa tensile bond (left intact)
	 * becomes usable. Provable: a net-compression mu=0 optimum would be bounded by the control, so
	 * 9.935 > 4.768 means net tension is mobilised. Confirms PROMOTION_DESIGN §4.2; probe 5 measures
	 * that the higher mechanism is the brittle bond.
	 */
	AddInfo(FString::Printf(
		TEXT("FRICTION-OFF-UNLOCKS-BOND: residual control lambda* %.17g, friction-off %.17g, ")
		TEXT("ratio %.17g (mu=0 forbids shear and unlocks the tensile bond — a distinct, ")
		TEXT("higher mechanism, not friction capping an arch)"),
		Control->Oracle.Lambda, FrictionOff->Oracle.Lambda,
		FrictionOff->Oracle.Lambda / Control->Oracle.Lambda));
	TestTrue(
		*FString::Printf(
			TEXT("zeroing friction must UNLOCK a higher mechanism, not cap the residual: ")
			TEXT("friction-off lambda* (%.17g) must be STRICTLY GREATER than the residual ")
			TEXT("control (%.17g). With cohesion gone, mu=0 forbids bed-joint shear and lifts ")
			TEXT("the compression coupling, so the 0.70 MPa tensile bond becomes usable and the ")
			TEXT("wall re-forms on a DISTINCT bond-tension mechanism that stands higher — a ")
			TEXT("mu=0 optimum in net compression everywhere would be bounded by the control, ")
			TEXT("so a rise proves net tension is mobilised (probe 5 removes that bond)"),
			FrictionOff->Oracle.Lambda, Control->Oracle.Lambda),
		FrictionOff->Oracle.Lambda > Control->Oracle.Lambda);

	/*
	 * Finding 5: the 9.935 is bond tension, remove the bond and it collapses. The empirical
	 * companion to finding 3. With shear (mu=0) and tension (bond=0) both forbidden the joints carry
	 * compression only (a strict subset of the mu=0.75 control), so lambda* must fall from 9.935 to
	 * at-or-below 4.768. Asserted strictly below probe 1 and within a small margin at-or-below the
	 * control. Info line first so the value is logged regardless of verdict.
	 */
	AddInfo(FString::Printf(
		TEXT("BOND-TENSION-COLLAPSE: friction-off (bond intact) lambda* %.17g, friction+tension ")
		TEXT("off %.17g, residual control %.17g — removing the 0.70 MPa bond drops the mechanism"),
		FrictionOff->Oracle.Lambda, FrictionAndTensionOff->Oracle.Lambda, Control->Oracle.Lambda));
	TestTrue(
		*FString::Printf(
			TEXT("removing the jamb-bed tensile bond must COLLAPSE the 9.935 mechanism: ")
			TEXT("friction+tension-off lambda* (%.17g) must be STRICTLY LESS than friction-off ")
			TEXT("alone (%.17g). If it did not fall, the 9.935 was NOT carried by bond tension ")
			TEXT("and finding 3's attribution is overturned"),
			FrictionAndTensionOff->Oracle.Lambda, FrictionOff->Oracle.Lambda),
		FrictionAndTensionOff->Oracle.Lambda < FrictionOff->Oracle.Lambda);
	TestTrue(
		*FString::Printf(
			TEXT("with shear and tension both forbidden the residual must return to at-or-below ")
			TEXT("the frictional control: friction+tension-off lambda* (%.17g) must be <= the ")
			TEXT("control (%.17g) plus a small margin — a compression-only bed joint is a strict ")
			TEXT("capacity subset of the mu=0.75 control, whose optimum bounds it"),
			FrictionAndTensionOff->Oracle.Lambda, Control->Oracle.Lambda),
		FrictionAndTensionOff->Oracle.Lambda <= Control->Oracle.Lambda + 1.0e-6);

	/*
	 * Finding 4: for cohesion, the bearing courses are the tighter link, not the run to ground.
	 * Predicted the reverse; measured the opposite. Removing bearing cohesion collapses the residual
	 * (4.76816) while removing it in the run to ground leaves more standing (5.96237), so bearing <
	 * run-to-ground. Refutes "thrust runs to ground" for the cohesion term. Info line first so both
	 * band values are logged regardless of verdict.
	 */
	AddInfo(FString::Printf(
		TEXT("CHAIN-VS-BEARING: cohesion-off bearing lambda* %.17g, run-to-ground %.17g ")
		TEXT("(control 17.24 as built, residual 4.768 whole-chain)"),
		Bearing->Oracle.Lambda, RunToGround->Oracle.Lambda));
	TestTrue(
		*FString::Printf(
			TEXT("the bearing courses must be the tighter link for cohesion: bearing ")
			TEXT("lambda* (%.17g) < run-to-ground lambda* (%.17g). Removing cohesion at the ")
			TEXT("top collapses the residual more than removing it in the run to ground — ")
			TEXT("thrust does NOT run to ground for the cohesion term"),
			Bearing->Oracle.Lambda, RunToGround->Oracle.Lambda),
		Bearing->Oracle.Lambda < RunToGround->Oracle.Lambda);

	return true;
}

/**
 * The phase-2 refusal is a solver defect: a bounded, feasible problem must not come back "phase-2
 * simplex failed". Two opening-ladder rungs of 99 and 107 blocks (each smaller than answering rungs
 * measured beside them, each bounded by the lambda-cap row) must answer, and land between their
 * bracketing neighbours. Not fail-closed correctness: both were solved to a verified optimum (the
 * 1e-6 gate passed) once the anti-cycling fallback was stood down, so an admissible system exists
 * and the solver can find it.
 *
 * Diagnosis (2026-08-13). One refusal string hides two failures:
 *   (a) Spurious unboundedness (107-block j=4 rung). The LP splits each shear into p-q; the two
 *       columns are exact bitwise negations, so when one is basic the other's reduced cost is zero
 *       and 1.12e-9 of drift enters against CostTol 1e-9. Its FTRAN is -e_91, no positive ratio-test
 *       entry, so Unbounded on a capped problem.
 *   (b) A basis pivoted into singularity (99-block rung, and a 218-block wall). A near-dependent
 *       column is accepted on a ratio-test pivot barely over PivotTol 1e-9; the next refactorisation
 *       finds an LU pivot below SingularPivotTol 1e-11 and Factorise refuses.
 *
 * The first attribution (the Bland fallback's first-past-the-post entering rule letting 1e-9 win)
 * was refuted: making its tolerance relative changed nothing, the drift is inherited from the dual
 * solve (||y||inf = 6.7e6, machine eps 6.7e-10), and three of four noise pivots were on the ranked
 * path. The real cause: the ratio test had no sense of scale, accepting a 4.7e-9 coefficient from a
 * 3e7 column (relative pivot 1.6e-16, rounding). Both modes are that defect downstream, closed by
 * one change: RelativePivotTol (RigidBlockOracle.cpp), a floor of 1e-11 of the entering column's
 * largest magnitude. Reverting it reproduces the original red (836/671 pivots, both refusals), the
 * recorded bite-prover.
 *
 * Not needed: a repair for the no-entry seam itself was written and removed (with the floor in place,
 * reverting the seam leaves both fixtures answering bit-identically, so no test drove it). The seam
 * still refuses a bounded problem if reached; CURRENT_STATE books it as the residual mode, and Slice
 * 0a showed nothing reaches it. The 218-block rung is not asserted (143-275 s); the fallback earns
 * its keep there (without it the run passes the 100k-pivot cap). The fix must not loosen the
 * verification gate, remove the iteration cap, or scale the optimality tolerance by ||y|| (a simplex
 * stopping early reports a too-low lambda* the admissibility check certifies happily), which is why
 * the brackets below are load-bearing.
 *
 * Why the expected values are brackets, not pinned numbers: lambda* at 100+ blocks reproduces only
 * to ~1e-5 across pivot paths, so a pin on an unsolved fixture would pin the algorithm. Each refuser
 * is bracketed by its two ladder neighbours measured in the same run: the span ladder falls with
 * span (9-cell between 8 and 10, corroborated by L^-2.39), the abutment ladder rises with jamb (4-cell
 * between 3 and 5). The neighbours carry loose +/-1% sanity windows (a floor under the bracket, not a
 * measurement). The 99-block rung also carries a real +/-2e-5 window now that two pivot paths solved
 * it two ulps apart. Every rung carries a block-count pin (TRAPS: only a size pin caught the recorded
 * rung-flip), which also shows scale is not the cause (99/107 refuse while 104/119 answer, and wall-01's
 * 375 pieces answer).
 *
 * Cost: six solves, ~28 s (the j=3/j=4 rungs duplicate OpeningMechanismLadders deliberately, since a
 * bracket must be closed by neighbours from the same binary). No ticking world; no production cascade.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockPhaseTwoBoundedTest,
	"OracleSweepFast.RigidBlock.PhaseTwoMustNotRefuseABoundedProblem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockPhaseTwoBoundedTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	struct FRung
	{
		const TCHAR* Name = nullptr;
		int32 OpeningCells = 0;
		int32 CoursesBelow = 0;
		int32 JambCells = 0;

		/** The fixture's size, pinned: only this catches a rung building another's wall. */
		int32 WantBlocks = 0;
		int32 WantJoints = 0;

		/** A loose sanity window on an answering neighbour; both zero on a rung under test. */
		double SanityLo = 0.0;
		double SanityHi = 0.0;

		/**
		 * The midpoint-of-two-certified-readings window, +/-2e-5 relative, on a rung solved by two
		 * pivot paths. Both zero where no such pair exists (the j=4 rung's pair is in
		 * OpeningMechanismLadders, not duplicated here).
		 */
		double CertifiedLo = 0.0;
		double CertifiedHi = 0.0;
	};

	/*
	 * Mean re-anchor re-pins (2026-08-14): every window re-measured. Sanity windows keep their ~±1%
	 * looseness; the 99er's certified window is one reading at ±2e-5 (89.898933555823774; a re-pair
	 * is owed with the next pivot-path change). The must-answer halves and the bracket (107er below
	 * the 5-cell neighbour: 18.1243 < 18.1419) held.
	 */
	const FRung Rungs[] =
	{
		{ TEXT("span ladder, 8-cell opening (the wide-lambda neighbour)"),
			8, 3, 3, 94, 206, 115.9, 118.2, 0.0, 0.0 },
		{ TEXT("span ladder, 9-cell opening (THE 99-BLOCK REFUSER)"),
			9, 3, 3, 99, 216, 0.0, 0.0, 89.8971, 89.9007 },
		{ TEXT("span ladder, 10-cell opening (the narrow-lambda neighbour)"),
			10, 3, 3, 104, 226, 68.0, 69.4, 0.0, 0.0 },
		{ TEXT("abutment ladder, 3-cell jamb (the narrow-lambda neighbour)"),
			18, 1, 3, 95, 163, 17.71, 18.07, 0.0, 0.0 },
		{ TEXT("abutment ladder, 4-cell jamb (THE 107-BLOCK REFUSER)"),
			18, 1, 4, 107, 193, 0.0, 0.0, 0.0, 0.0 },
		{ TEXT("abutment ladder, 5-cell jamb (the wide-lambda neighbour)"),
			18, 1, 5, 119, 223, 17.96, 18.33, 0.0, 0.0 },
	};

	constexpr int32 NumRungs = UE_ARRAY_COUNT(Rungs);

	FOracleResult Results[NumRungs];
	bool bMeasured[NumRungs] = {};

	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		FStructure Structure;
		FString BuildWhy;

		if (!BuildOpeningLadderWall(
				Rung.OpeningCells, Rung.CoursesBelow, Rung.JambCells, INDEX_NONE,
				Structure, BuildWhy))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE could not be laid: %s"), Rung.Name, *BuildWhy));

			continue;
		}

		FOracleProblem Problem;
		FString BridgeWhy;

		/* Read the bridge's reason into a local first: argument evaluation is unsequenced, so
		 * folding it into the Printf could build the message from the pre-call empty string (TRAPS). */
		const bool bBridged = BuildRigidBlockProblem(Structure, Problem, BridgeWhy);

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: the bridge must represent this fixture (it said: %s)"),
					Rung.Name, *BridgeWhy),
				bBridged))
		{
			continue;
		}

		const double Started = FPlatformTime::Seconds();
		Results[Index] = SolveRigidBlock(Problem);
		const double Seconds = FPlatformTime::Seconds() - Started;
		bMeasured[Index] = true;

		const FString Line = FString::Printf(
			TEXT("PHASE2 %s: blocks=%d joints=%d answered=%d lambda=%.17g pivots=%d ")
			TEXT("bland=%d secs=%.2f%s%s"),
			Rung.Name, Problem.Blocks.Num(), Problem.Joints.Num(),
			Results[Index].bAnswered ? 1 : 0, Results[Index].Lambda,
			Results[Index].SimplexIterations, Results[Index].BlandDegenerateEntries, Seconds,
			Results[Index].WhyNot.IsEmpty() ? TEXT("") : TEXT(" | whynot: "),
			Results[Index].WhyNot.IsEmpty() ? TEXT("") : *Results[Index].WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the rung must be %d blocks and was %d — a size pin is the only ")
				TEXT("thing that catches a rung quietly building its neighbour's wall"),
				Rung.Name, Rung.WantBlocks, Problem.Blocks.Num()),
			Problem.Blocks.Num(), Rung.WantBlocks);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the rung must carry %d joints and carried %d"),
				Rung.Name, Rung.WantJoints, Problem.Joints.Num()),
			Problem.Joints.Num(), Rung.WantJoints);
	}

	/*
	 * The neighbours first: they are answering rows today, and their sanity windows are
	 * what stops the brackets below being satisfiable by four pieces of garbage.
	 */
	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		if (!bMeasured[Index] || Rung.SanityHi <= 0.0)
		{
			continue;
		}

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: this neighbour answers today and must keep answering (it said: ")
					TEXT("%s)"),
					Rung.Name, *Results[Index].WhyNot),
				Results[Index].bAnswered))
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* %.17g must stay inside the loose sanity window ")
				TEXT("[%.6g, %.6g] — a deliberately slack floor under the brackets, a ")
				TEXT("thousand times wider than the ~1e-5 a changed pivot path moves it"),
				Rung.Name, Results[Index].Lambda, Rung.SanityLo, Rung.SanityHi),
			Results[Index].Lambda >= Rung.SanityLo && Results[Index].Lambda <= Rung.SanityHi);
	}

	/*
	 * The one rung with two certified readings gets a real window, and it sits beside the
	 * bracket rather than instead of it: the bracket states a physical relation the number
	 * has to respect, this states the number. See the header for why 37.932472960794136
	 * (fallback stood down) and 37.932472960794129 (repaired solver) — two ulps apart —
	 * license a +/-2e-5 window where an unsolved fixture licensed none.
	 */
	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		if (!bMeasured[Index] || !(Rung.CertifiedHi > 0.0) || !Results[Index].bAnswered)
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* %.17g must lie in [%.9g, %.9g] — the midpoint of TWO ")
				TEXT("certified readings taken by different pivot paths, +/-2e-5 relative, ")
				TEXT("which is ~1e4 times the two ulps those paths actually differed by. A ")
				TEXT("move inside this window is the algorithm; a move outside it is the ")
				TEXT("physics, and the bracket beside it says which direction is even legal"),
				Rung.Name, Results[Index].Lambda, Rung.CertifiedLo, Rung.CertifiedHi),
			Results[Index].Lambda >= Rung.CertifiedLo
				&& Results[Index].Lambda <= Rung.CertifiedHi);
	}

	/*
	 * The defect itself. Each refuser must answer, and between its two ladder neighbours (named in
	 * the message so a failure says which reading it fell outside).
	 */
	const auto CheckBracketed =
		[&](int32 Under, int32 Low, int32 High, const TCHAR* Why)
	{
		if (!bMeasured[Under] || !bMeasured[Low] || !bMeasured[High])
		{
			return;
		}

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: the oracle MUST ANSWER this fixture — its LP is bounded by the ")
					TEXT("lambda-cap row and an admissible optimum exists — reachable and ")
					TEXT("verifiable, the ratio test's missing sense of scale being what ")
					TEXT("stood between the solver and it. It refused after %d pivots (%d ")
					TEXT("of them in the Bland fallback), saying: %s"),
					Rungs[Under].Name, Results[Under].SimplexIterations,
					Results[Under].BlandDegenerateEntries, *Results[Under].WhyNot),
				Results[Under].bAnswered))
		{
			return;
		}

		if (!Results[Low].bAnswered || !Results[High].bAnswered)
		{
			return;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* %.17g must lie strictly between its ladder neighbours ")
				TEXT("(%.17g and %.17g) — %s. Failing THIS while the answer arrives is a ")
				TEXT("different finding from refusing: it says the solver answered, wrongly."),
				Rungs[Under].Name, Results[Under].Lambda, Results[Low].Lambda,
				Results[High].Lambda, Why),
			Results[Under].Lambda > Results[Low].Lambda
				&& Results[Under].Lambda < Results[High].Lambda);
	};

	CheckBracketed(1, 2, 0,
		TEXT("lambda* falls with the clear reveal (158.5 / 181.0 / 203.5 cm across these "
			"three rungs), so a 9-cell opening cannot be stronger than an 8-cell one nor "
			"weaker than a 10-cell one"));

	CheckBracketed(4, 3, 5,
		TEXT("more jamb is more bearing and more weight over it, and this ladder reads "
			"7.379 at three cells and 11.115 at five"));

	return true;
}

/**
 * A refusal must name which termination produced it (slice 0a's first red). When SolveRigidBlock
 * refuses, the result must carry a machine-readable reason distinguishing its terminations (hit
 * iteration cap, spurious unbounded ray, numerical failure being three different events with three
 * fixes) rather than collapsing them into "phase-2 simplex failed". That collapse cost two
 * instrumented builds in a week and blocks the promotion design's risk experiment (§11 R4 counts
 * refusals by reason; §5.6 counts fallbacks by reason), so the reason is an enumerator and the
 * sentence is derived from it.
 *
 * The assertion. Distinctness is asserted over the whole taxonomy and needs no fixture (only one of
 * six refusing arms is reachable from a fixture this project owns, so a fixture-only test could say
 * nothing about the arms the diagnosis had to tell apart). The pairwise-distinct sweep costs nothing
 * and a constant string cannot satisfy it. The taxonomy is tied to reality by fixtures beside it:
 * the answering rung must report None with an empty sentence, the poisoned problem InvalidProblem
 * and print its phrase (the enumerator and sentence are one statement).
 *
 * The phase-2 canary fired the day it was written and is gone (2026-08-15). It pinned the 8-cell
 * 128-block wall refusing with PhaseTwoNumericalFailure, under the instruction to re-point or delete
 * it once the solver answered. Its sibling PhaseTwoMustNotRefuseTheCoveredOpeningFamily was made
 * green the same day (RelativePivotTol 1e-11 -> 1e-9) and no fixture reaches a phase-2 arm now, so
 * the row was deleted rather than kept as a 75 s no-op. Cost recorded: the three PhaseTwo enumerators
 * plus PhaseOneFailure and VerificationFailure are proven distinct but reported by no fixture, so a
 * wrong-reason site would go uncaught; CURRENT_STATE carries the owed replacement.
 *
 * What was measured 2026-08-15: CURRENT_STATE proposed the unbreakable lambda-cap tower as a
 * refuser, but the cap row bounds it so it terminates Optimal (ValidationCatalogue pins it); it
 * reaches an unbounded ray only under mutation M5. The covered-opening family instead returned
 * NumericalFailure from the periodic refactorisation (Factorise found an LU pivot under
 * SingularPivotTol):
 *
 *     8-cell opening (128 blocks) : position 4018/4021, col 6119, pivot 4.3758616521949456e-12
 *     16-cell opening (200 blocks): position 6322/6325, col 9639, pivot 4.928959752317684e-12
 *
 * Not the spurious-unbounded arm and not NB-7; both suspects refuted, the repair in
 * PhaseTwoMustNotRefuseTheCoveredOpeningFamily's header. This test's 8-cell pin reported
 * PhaseTwoNumericalFailure, the shipped taxonomy reproducing the instrumented finding.
 *
 * Cost: two solves under a second, plus the taxonomy sweep. No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockRefusalReasonTest,
	"OracleSweepFast.RigidBlock.RefusalNamesItsReason",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockRefusalReasonTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	/* --- 1. The taxonomy: every refusing reason reads differently from every other. --- */

	struct FReason
	{
		EOracleRefusal Value;
		const TCHAR* Name;
	};

	const FReason Reasons[] =
	{
		{ EOracleRefusal::InvalidProblem,           TEXT("InvalidProblem") },
		{ EOracleRefusal::PhaseOneFailure,          TEXT("PhaseOneFailure") },
		{ EOracleRefusal::PhaseTwoIterationCap,     TEXT("PhaseTwoIterationCap") },
		{ EOracleRefusal::PhaseTwoUnbounded,        TEXT("PhaseTwoUnbounded") },
		{ EOracleRefusal::PhaseTwoNumericalFailure, TEXT("PhaseTwoNumericalFailure") },
		{ EOracleRefusal::VerificationFailure,      TEXT("VerificationFailure") },
	};

	TestTrue(
		TEXT("an ANSWERED result has no reason, so RefusalText(None) is empty — the reason "
			"and bAnswered must be checkable against each other, which they cannot be if "
			"None carries a sentence of its own"),
		RefusalText(EOracleRefusal::None).IsEmpty());

	for (const FReason& Reason : Reasons)
	{
		TestTrue(
			*FString::Printf(
				TEXT("%s must have a reason phrase — an unnamed refusal is the state this "
					"test exists to end"),
				Reason.Name),
			!RefusalText(Reason.Value).IsEmpty());
	}

	constexpr int32 NumReasons = UE_ARRAY_COUNT(Reasons);

	for (int32 First = 0; First < NumReasons; ++First)
	{
		for (int32 Second = First + 1; Second < NumReasons; ++Second)
		{
			const FString FirstText = RefusalText(Reasons[First].Value);
			const FString SecondText = RefusalText(Reasons[Second].Value);

			TestTrue(
				*FString::Printf(
					TEXT("%s and %s must not read the same, and both read \"%s\" — two "
						"terminations with one sentence between them is exactly what forced "
						"an instrumented build twice this week"),
					Reasons[First].Name, Reasons[Second].Name, *FirstText),
				FirstText != SecondText);
		}
	}

	/* --- 2. An answered fixture reports no reason at all. ---------------------------- */

	FStructure Standing;
	FString StandingWhy;

	/* Read the builder's reason into a local before building the message: the Printf argument list
	 * is unsequenced and would use the pre-call empty string (TRAPS). */
	const bool bStandingLaid = BuildCoveredOpeningWall(2, 18, 2, Standing, StandingWhy);

	if (TestTrue(
			*FString::Printf(
				TEXT("the answering control must be laid (it said: %s)"), *StandingWhy),
			bStandingLaid))
	{
		FOracleProblem Problem;
		FString BridgeWhy;

		const bool bBridged = BuildRigidBlockProblem(Standing, Problem, BridgeWhy);

		if (TestTrue(
				*FString::Printf(
					TEXT("the answering control must bridge (it said: %s)"), *BridgeWhy),
				bBridged))
		{
			const FOracleResult Answered = SolveRigidBlock(Problem);

			TestTrue(
				*FString::Printf(
					TEXT("CONTROL (case 21's own rung, 83 blocks): it answers today at "
						"lambda* ~17.24 and must keep answering, or every row below is "
						"asserting against a solver that refuses everything (it said: %s)"),
					*Answered.WhyNot),
				Answered.bAnswered);

			TestTrue(
				TEXT("an answered result carries EOracleRefusal::None — the reason field and "
					"bAnswered must agree, and this is the arm that catches a reason left "
					"set from a previous solve"),
				Answered.Refusal == EOracleRefusal::None);

			TestTrue(
				*FString::Printf(
					TEXT("an answered result carries no sentence either, and carried \"%s\""),
					*Answered.WhyNot),
				Answered.WhyNot.IsEmpty());
		}
	}

	/* --- 3. A refusal BEFORE the simplex names validation, not a simplex arm. --------- */

	{
		FStructure Poisonable;
		FString PoisonWhy;

		const bool bPoisonableLaid =
			BuildCoveredOpeningWall(2, 18, 2, Poisonable, PoisonWhy);

		if (TestTrue(
				*FString::Printf(
					TEXT("the poisoned fixture must be laid (it said: %s)"), *PoisonWhy),
				bPoisonableLaid))
		{
			FOracleProblem Problem;
			FString BridgeWhy;

			if (BuildRigidBlockProblem(Poisonable, Problem, BridgeWhy)
				&& Problem.Joints.Num() > 0)
			{
				/* A non-unit normal: refused by ValidateProblem, so nothing is solved. */
				Problem.Joints[0].NormalX = 0.5;
				Problem.Joints[0].NormalZ = 0.5;

				const FOracleResult Refused = SolveRigidBlock(Problem);

				TestTrue(
					TEXT("a poisoned problem is still refused"),
					!Refused.bAnswered);

				TestTrue(
					*FString::Printf(
						TEXT("a validation refusal reports InvalidProblem and reported %d — "
							"the reason must separate 'the problem was never solvable' from "
							"'the solver could not finish', which is the coarsest split in "
							"the taxonomy and the one a caller branches on first"),
						int32(Refused.Refusal)),
					Refused.Refusal == EOracleRefusal::InvalidProblem);

				TestTrue(
					*FString::Printf(
						TEXT("and the sentence it prints carries that reason's phrase: "
							"\"%s\" must contain \"%s\""),
						*Refused.WhyNot, *RefusalText(EOracleRefusal::InvalidProblem)),
					Refused.WhyNot.Contains(RefusalText(EOracleRefusal::InvalidProblem)));
			}
		}
	}

	return true;
}

/**
 * The eight-course-cover family still refuses a bounded, feasible problem, and it is not the wall
 * the record names. An 8-cell opening under eight courses of cover (128 blocks, smaller than the
 * 137- and 218-block members that answer beside it) must answer rather than "phase-2 simplex
 * failed", and land between its ladder neighbours. Same defect as
 * PhaseTwoMustNotRefuseABoundedProblem (read that header first), one notch downstream: the
 * 2026-08-13 RelativePivotTol fix (floor 1e-11) closed the 99/107-block rungs but is not enough here.
 *
 * The record was stale (2026-08-15). CURRENT_STATE, PROMOTION_DESIGN §6/§10 and WallAcceptanceTest
 * named the 218/290/371-block members as the refusers, measured at characteristic strengths one day
 * before the mean re-anchor. Re-measured (opening cells between two-cell jambs, eight courses cover):
 *
 *     opening   blocks  joints   lambda*                 pivots   secs
 *        2        74     174     537.60206692224722       6,578    4.9
 *        4        92     218     384.91922849080379      10,455   15.7
 *        5       101     240     298.0111013112051       13,997   26.1
 *        6       110     262     236.97383758407059      16,353   39.5
 *        7       119     284     190.53602223331399      17,983   52.3
 *        8       128     306     REFUSED                 17,316   40.1
 *        9       137     328     127.7264657209236       22,753   89.7
 *       10       146     350     105.64778136561756      24,635  106.9
 *       12       164     394      75.120451403407529     29,261  157.5
 *       14       182     438      55.975345056998457     30,848  166.0
 *       16       200     482     REFUSED                  6,244   18.3
 *       18       218     526      33.651690962702006     37,705  254.9
 *       26       290     702      15.622461777098057     60,721  555.1
 *       35       371     900       8.4149459982219277    88,810 1196.9
 *
 * So all three record-named walls answer today (including case 22 at 371 blocks). The blockers are
 * two isolated holes at 128 and 200 blocks inside a smooth monotone curve their neighbours certify
 * both sides, a stronger defect than the stale record: 128 blocks is a third of the size the refusal
 * was thought to start at. Note also that case 22 answers at 88,810 of 100,000 pivots, so a
 * pivot-path change could push it over the cap (CURRENT_STATE). The problem is bounded (the cap row)
 * and feasible (lambda=0 satisfies the equalities, and the neighbours answer at 190.5 and 127.7).
 *
 * The arm, measured 2026-08-15 and reverted: phase 2 returns NumericalFailure from the periodic
 * refactorisation (Factorise finds an LU pivot <= SingularPivotTol 1e-11 in the last few columns):
 *
 *     8-cell opening (128 blocks) : position 4018 of 4021, col 6119, pivot 4.3758616521949456e-12
 *    16-cell opening (200 blocks) : position 6322 of 6325, col 9639, pivot 4.928959752317684e-12
 *
 * Both recorded suspects refuted: the spurious-unbounded arm fired on no solve, and NB-7's pivot-out
 * pass printed nothing under 1e-6 absolute or 1e-7 relative. Two one-constant experiments both reach
 * the answer, proving it exists: RelativePivotTol 1e-11 -> 1e-9 gives 155.63200561101226 (128) and
 * 43.132916253688222 (200); RefactoriseEvery 64 -> 16 gives 155.63200342392039 and 43.132560867194137.
 * The pairs agree to 1.4e-8 and 8.2e-6 on different pivot paths and both clear the 1e-6 gate, which
 * licenses the windows below. Both levers attack LU-singularity from opposite ends, so the basis is
 * driven singular by the solver's arithmetic, not the wall; the right fix (possibly the factorisation's
 * fixed column order, pointing at the roadmap's Markowitz ordering) is left open. The fix must not
 * loosen the verification gate, remove the iteration cap, or scale the optimality tolerance by ||y||.
 *
 * A window and a bracket. The window is +/-1e-4 (ten times the usual 2e-5) because the certified
 * readings came from diagnostic variants, not the fixed solver, whose path is a third this pair has
 * not sampled; tighten to 2e-5 once the fixed solver produces its own reading. The bracket states the
 * physics the window cannot (lambda* falls monotonically with the opening, closed by neighbours in
 * this run). The parity confound is measured small (the cut's courses are fixed and the odd rungs sit
 * smoothly; the bracket spans a factor of 1.49). Every rung carries a block-and-joint pin (TRAPS). The
 * 200-block refuser carries a window but no bracket (its neighbours cost 166/255 s; the bracket's
 * value is bought once by the 8-cell trio), and is here so the defect is not one wall's accident.
 *
 * Cost: four solves, ~200 s (expect ~420 s once they answer). No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockCoveredOpeningRefusalTest,
	"OracleSweepFull.RigidBlock.PhaseTwoMustNotRefuseTheCoveredOpeningFamily",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockCoveredOpeningRefusalTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	struct FRung
	{
		const TCHAR* Name = nullptr;
		int32 OpeningCells = 0;

		int32 WantBlocks = 0;
		int32 WantJoints = 0;

		/** A loose sanity window on an answering neighbour; both zero on the rung under test. */
		double SanityLo = 0.0;
		double SanityHi = 0.0;

		/**
		 * The midpoint of two certified readings from different pivot paths, +/-1e-4 relative. Both
		 * zero where no such pair exists. The header says why the window is 10x the usual 2e-5.
		 */
		double CertifiedLo = 0.0;
		double CertifiedHi = 0.0;
	};

	const FRung Rungs[] =
	{
		{ TEXT("covered opening, 7 cells (the strong-lambda neighbour)"),
			7, 119, 284, 188.6, 192.5, 0.0, 0.0 },
		{ TEXT("covered opening, 8 cells (THE 128-BLOCK REFUSER)"),
			8, 128, 306, 0.0, 0.0, 155.6164, 155.6476 },
		{ TEXT("covered opening, 9 cells (the weak-lambda neighbour)"),
			9, 137, 328, 126.4, 129.0, 0.0, 0.0 },
		{ TEXT("covered opening, 16 cells (THE 200-BLOCK REFUSER)"),
			16, 200, 482, 0.0, 0.0, 43.1284, 43.1371 },
	};

	constexpr int32 NumRungs = UE_ARRAY_COUNT(Rungs);

	FOracleResult Results[NumRungs];
	bool bMeasured[NumRungs] = {};

	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		FStructure Structure;
		FString BuildWhy;

		if (!BuildCoveredOpeningWall(8, Rung.OpeningCells, 2, Structure, BuildWhy))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE could not be laid: %s"), Rung.Name, *BuildWhy));

			continue;
		}

		FOracleProblem Problem;
		FString BridgeWhy;

		/* The bridge's reason is read into a local first — argument order is unsequenced. */
		const bool bBridged = BuildRigidBlockProblem(Structure, Problem, BridgeWhy);

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: the bridge must represent this fixture (it said: %s)"),
					Rung.Name, *BridgeWhy),
				bBridged))
		{
			continue;
		}

		const double Started = FPlatformTime::Seconds();
		Results[Index] = SolveRigidBlock(Problem);
		const double Seconds = FPlatformTime::Seconds() - Started;
		bMeasured[Index] = true;

		const FString Line = FString::Printf(
			TEXT("COVERED %s: blocks=%d joints=%d answered=%d lambda=%.17g pivots=%d ")
			TEXT("bland=%d secs=%.2f%s%s"),
			Rung.Name, Problem.Blocks.Num(), Problem.Joints.Num(),
			Results[Index].bAnswered ? 1 : 0, Results[Index].Lambda,
			Results[Index].SimplexIterations, Results[Index].BlandDegenerateEntries, Seconds,
			Results[Index].WhyNot.IsEmpty() ? TEXT("") : TEXT(" | whynot: "),
			Results[Index].WhyNot.IsEmpty() ? TEXT("") : *Results[Index].WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the rung must be %d blocks and was %d — a size pin is the only ")
				TEXT("thing that catches a rung quietly building its neighbour's wall"),
				Rung.Name, Rung.WantBlocks, Problem.Blocks.Num()),
			Problem.Blocks.Num(), Rung.WantBlocks);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the rung must carry %d joints and carried %d"),
				Rung.Name, Rung.WantJoints, Problem.Joints.Num()),
			Problem.Joints.Num(), Rung.WantJoints);
	}

	/* The neighbours first: they answer today, and their loose sanity windows stop the bracket
	 * below being satisfiable by three pieces of garbage. */
	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		if (!bMeasured[Index] || Rung.SanityHi <= 0.0)
		{
			continue;
		}

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: this neighbour answers today and must keep answering (it said: ")
					TEXT("%s)"),
					Rung.Name, *Results[Index].WhyNot),
				Results[Index].bAnswered))
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* %.17g must stay inside the loose sanity window ")
				TEXT("[%.6g, %.6g] — a deliberately slack floor under the bracket, a thousand ")
				TEXT("times wider than the ~1e-5 a changed pivot path moves it"),
				Rung.Name, Results[Index].Lambda, Rung.SanityLo, Rung.SanityHi),
			Results[Index].Lambda >= Rung.SanityLo && Results[Index].Lambda <= Rung.SanityHi);
	}

	/* --- The defect itself ------------------------------------------------------------ */

	/*
	 * Every refuser must answer, and answer the number two independent pivot paths agreed on. The
	 * window stops a "fix" that merely stops refusing: a solver reaching a different optimum fails it.
	 */
	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		if (!bMeasured[Index] || !(Rung.CertifiedHi > 0.0))
		{
			continue;
		}

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: the oracle MUST ANSWER this fixture — its LP is bounded by the ")
					TEXT("lambda-cap row, and it has been SOLVED TWICE during the diagnosis, ")
					TEXT("by two solver variants taking different pivot paths, both clearing ")
					TEXT("the post-solve verification gate. It refused after %d pivots (%d of ")
					TEXT("them in the Bland fallback), saying: %s"),
					Rung.Name, Results[Index].SimplexIterations,
					Results[Index].BlandDegenerateEntries, *Results[Index].WhyNot),
				Results[Index].bAnswered))
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* %.17g must lie in [%.9g, %.9g] — the midpoint of two ")
				TEXT("certified readings, +/-1e-4 relative. A move inside this window is the ")
				TEXT("algorithm; a move outside it is a different answer, and this fixture's ")
				TEXT("answer is not in dispute"),
				Rung.Name, Results[Index].Lambda, Rung.CertifiedLo, Rung.CertifiedHi),
			Results[Index].Lambda >= Rung.CertifiedLo
				&& Results[Index].Lambda <= Rung.CertifiedHi);
	}

	/*
	 * The bracket beside the window: the window says what the number is, the bracket what the
	 * physics allows, closed by two neighbours measured in this run rather than carried in.
	 */
	if (bMeasured[0] && bMeasured[1] && bMeasured[2])
	{
		if (Results[1].bAnswered && Results[0].bAnswered && Results[2].bAnswered)
		{
			TestTrue(
				*FString::Printf(
					TEXT("covered opening, 8 cells: lambda* %.17g must lie strictly between ")
					TEXT("its neighbours (%.17g at 7 cells and %.17g at 9 cells) — lambda* ")
					TEXT("falls monotonically with the opening across all twelve rungs of this ")
					TEXT("family, so an 8-cell opening cannot be stronger than a 7-cell one ")
					TEXT("nor weaker than a 9-cell one, and the family's L^-2.4 fit predicts ")
					TEXT("~156. Failing THIS while the answer arrives is a different finding ")
					TEXT("from refusing: it says the solver answered, wrongly"),
					Results[1].Lambda, Results[0].Lambda, Results[2].Lambda),
				Results[1].Lambda < Results[0].Lambda && Results[1].Lambda > Results[2].Lambda);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
