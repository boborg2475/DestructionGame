// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "Tests/RigidBlockOracle.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE FIXTURE-CATALOGUE SWEEP — DESIGN.md §7 evolution step 3, slice 2. Every affordable
 * acceptance fixture is laid through PRODUCTION'S OWN PRODUCERS, projected through
 * BuildRigidBlockProblem, solved by the rigid-block LP, and DIFFED against what the
 * production cascade does with the identical structure. The deliverable is a measurement
 * per fixture — lambda*, the load factor the acceptance rulings never had — and a PINNED
 * CLASSIFICATION of every agreement and disagreement, so that a change on EITHER side
 * fails loudly. This is the DropsToday pattern lifted from "what the model wrongly does"
 * to "how the two methods relate".
 *
 * WHAT A CLASSIFICATION IS AND IS NOT. The oracle is finite-tension rigid-plastic limit
 * analysis at the CODED characteristic strengths; production is an uncracked-elastic
 * routing solver plus an interim overturning guard. Where they disagree, NEITHER is
 * automatically right: each disagreement row names its mechanism, and the ones that
 * touch a user ruling (DESIGN.md §8) classify against the ruling's recorded cost rather
 * than relitigating it. Two systematic slants to keep in mind when reading lambda*:
 *
 *   - CAPACITY IS READ AT THE PLASTIC LIMIT — up to 3x the uncracked first-crack moment
 *     (the oracle header's own caveat), plus FULL LOAD REDISTRIBUTION: a rigid block
 *     resting on two contacts may put its weight wherever equilibrium allows, where the
 *     real elastic path loads the middle. Both push lambda* UP relative to a brittle
 *     first-crack criterion, so "the oracle stands it" is the MOST charitable reading.
 *   - STRENGTHS ARE THE CODED CHARACTERISTIC VALUES (f_xk1 = 0.10), while the §8
 *     rulings are argued at MEAN strength (0.6-1.0). For a tension-governed fixture
 *     lambda* scales linearly with f_t, so multiply by ~6 to read a lambda* against a
 *     mean-strength ruling. Rows that turn on this say so.
 *
 * PRODUCTION'S VERDICT, FOR THE DIFF, is the cascade outcome on the same structure:
 * SolveAndBreak to a standstill, then count live pieces with no path to the earth
 * (Stranded counts as fallen, exactly as the acceptance suite folds them — a piece
 * nothing carries comes down either way; the stranded count is reported separately so a
 * routing limitation stays visible). "Production falls" in a relation below means "at
 * least one piece came down", with the exact count PINNED per row.
 *
 * THE ORACLE'S VERDICT IS GLOBAL, and that is a real asymmetry of vocabulary: lambda* is
 * one number for the whole structure, so a LOCAL LOSS — two bricks drop, the wall is
 * fine — reads Falls (lambda* ~ 0) exactly like a collapse. The relation rows account
 * for it in their mechanism text; where the local/global split is the whole question the
 * row's comment works the survivor arithmetic rather than pretending lambda* can.
 *
 * COST AND THE DENSE ENVELOPE, BOTH MEASURED — HISTORY NOW; the solver described in this
 * paragraph and the next was retired 2026-08-12 (see THE SPARSE REWRITE below). The dense
 * tableau WAS O(rows x columns) doubles and one pivot touched all of
 * it. Measured 2026-08-09: the 40-course stack solves in 0.084 s, the beam pair and the
 * one-cell pair in microseconds, corbel B in 0.21 s — those live in the fast suite.
 * Corbel C took 6.8 s and corbel D 79.6 s (90 blocks / 200 joints ~ 20 ms per pivot x
 * 4,092 pivots), so C and D live in the OPT-IN slow test at the bottom of this file;
 * the slow test's name deliberately does NOT contain "DestructionGame" so the documented
 * full-suite command never pays for it — see its header for the convention.
 *
 * THE ENVELOPE FINDING (2026-08-11, the full unmeasured sweep run, one row at a time):
 * past roughly a hundred blocks the dense simplex STOPS ANSWERING — the post-solve
 * verification finds the basic solution violating an assembly row by more than its
 * 1e-6 relative tolerance and the oracle refuses, fail closed, exactly as designed
 * (accumulated pivot error in a dense tableau, not a formulation defect: every refusal
 * came after ~4,000-13,000 pivots). Every fixture at >= 119 blocks refused. Every
 * fixture at <= 90 blocks answered — reproducing its slice-1 value where one existed —
 * EXCEPT the 74-block free-end 7x10 (183 joints, 3,993 pivots), which refuses and is
 * this file's live canary: block count alone does not separate answer from refusal
 * (corbel D answers at 90 blocks / 200 joints / 4,092 pivots), so treat the envelope
 * as measured per-fixture, never as a block-count rule. A 74-block refusal is the
 * recorded state, not a solver regression.
 * The refusals were RECORDED MEASUREMENTS (the slow test's header carried the table),
 * one refusal stayed live as a pinned canary until the sparse rewrite flipped and
 * promoted it, and the walls beyond the envelope were deliberately-listed deferrals —
 * never silent skips. No refusal row is live today; the refusal branch below is kept
 * for FUTURE refusal pins (a 30-course attempt would use it), not exercised by any row.
 * EXCLUDED OUTRIGHT on arithmetic alone: the 30-course walls (cases 1-5, ~375 pieces /
 * ~1,000 joints: a ~13k x 33k tableau is ~3.5 GB and hours of pivots), corbels E35/E36
 * (~400 pieces, same league) and corbel F (3,015 pieces, hundreds of GB).
 *
 * THE SPARSE REWRITE RETIRED THAT ENVELOPE (2026-08-12). The solver is now a sparse
 * revised simplex with periodic clean refactorisation (RigidBlockOracle.h records the
 * method); every fixture the dense tableau refused ANSWERS with verified residuals,
 * the free-end 7x10 canary failed exactly as written and was promoted to a measured
 * pin, and the paragraphs above stand as the history that motivated the rewrite. The
 * slow test's header carries the new measured table, and every fixture in it became a
 * PINNED ROW in the same day's classification slice — nineteen rows across the two slow
 * tests, fifteen of them acceptance wall cases. Three scales remain beyond the
 * PRACTICAL envelope: the 30-course walls (cases 1-5, ~375 pieces / ~13k rows) are now
 * REPRESENTABLE (tens of MB, no dense tableau), but wall-01 was still pivoting after
 * ~45 minutes when its measuring run was cut off — full Dantzig pricing over ~34k
 * columns per iteration is the measured cost driver, and partial pricing is the known
 * lever if those five rows are ever wanted; corbels E35/E36 (~400 pieces) sit in the
 * same league; corbel F (3,015 pieces) remains far out.
 *
 * NEEDS A TICKING WORLD: NO. Producers, graph, LP — plain arithmetic on plain structs.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation
 * unit.
 */
namespace RigidBlockSweepTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * THE BRICK AND THE UNITS — derived here, imported from nowhere, same discipline as
	 * every acceptance file: a wrong production constant must DISAGREE with this file.
	 * ================================================================================ */

	constexpr double SweepBrickLengthCm = 21.5;
	constexpr double SweepBrickWidthCm = 10.25;
	constexpr double SweepBrickHeightCm = 6.5;
	constexpr double SweepClayDensityGramsPerCubicCm = 1.9;
	constexpr double SweepJointCm = 1.0;
	constexpr double SweepCoursePitchCm = SweepBrickHeightCm + SweepJointCm;

	/** Density-first multiplication order — the PieceMassKg contract; 2.72163125 kg. */
	constexpr double SweepBrickMassKg = SweepClayDensityGramsPerCubicCm
		* SweepBrickLengthCm * SweepBrickWidthCm * SweepBrickHeightCm / 1000.0;

	/* ================================================================================
	 * THE CROSS-ROW TOLERANCES — chosen from the measurement, not from taste.
	 * ================================================================================ */

	/**
	 * How close two fixtures have to read for "the LP prices these identically" to be a
	 * PIN rather than a remark — a window is a box around one number and cannot say two
	 * numbers are the same one, so this is deliberately three orders tighter than any row
	 * window in the file. MEASURED 2026-08-12: the free-end pair agrees to 2.21e-16
	 * relative (256.82001841913814 vs ...19 — ONE ULP) and the superimposed pair to
	 * 1.31e-16 (868.62373637245832 vs ...21, also one ulp). 1e-15 leaves four to eight
	 * ulps of headroom, which is room for a different pivot path to the same optimum and
	 * no room at all for a different optimum.
	 */
	constexpr double SameNumberRelativeTolerance = 1.0e-15;

	/**
	 * Production's wall-16 / wall-15 worst-joint ratio, measured 31.5576310306295 — and
	 * both ends of it are the DESIGN §6 anchors (0.058203838191552663 and
	 * 0.0018443665221594297), so this ratio is the surcharge discrimination the acceptance
	 * suite pins, re-measured through the scenario catalogue instead of its own fixture.
	 */
	constexpr double SuperimposedReadingRatioLo = 31.55;
	constexpr double SuperimposedReadingRatioHi = 31.57;

	/**
	 * Production's 20-course / 10-course free-end ratio, measured 2.11985065259119
	 * (1.9576612517914398 over 0.92349017578125003). Height-LINEAR, and a shade over 2
	 * because the removed brick's own course does not double with the wall.
	 */
	constexpr double FreeEndReadingRatioLo = 2.119;
	constexpr double FreeEndReadingRatioHi = 2.121;

	/* ================================================================================
	 * WHAT ONE SWEPT FIXTURE PRODUCES, AND HOW A ROW IS JUDGED.
	 * ================================================================================ */

	/**
	 * How the two methods relate on one fixture. Unmeasured is the ZERO enumerator and
	 * is a FAILURE when asserted — a row nobody has measured yet is this slice's red,
	 * never a silent skip.
	 */
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
		 * The oracle REFUSES the fixture — post-solve verification fails at this
		 * scale. A pinned refusal is a canary: the day the solver answers here (a
		 * pricing improvement, a tolerance change), the row fails loudly and must be
		 * promoted to a measured relation, never silently absorbed. NO ROW USES THIS
		 * TODAY — the dense-era canary flipped and was promoted 2026-08-12; the
		 * machinery stays for future refusal pins (a 30-course attempt would use it).
		 */
		OracleRefusesAtThisScale,
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
		default:                                     return TEXT("UNMEASURED");
		}
	}

	struct FSweepRow
	{
		const TCHAR* Name = nullptr;

		/** The named mechanism behind the expected relation, printed on any failure. */
		const TCHAR* Mechanism = nullptr;

		/** Leaves the structure POST-CUT, PRE-CASCADE. False is a fixture failure. */
		TFunction<bool(FStructure&, FString&)> Build;

		ERelation Expected = ERelation::Unmeasured;

		/**
		 * The lambda* pin, inclusive. The solver is deterministic (bit-identical per
		 * problem), so these are tight relative windows around the measured value; a
		 * negative low bound means the row is unmeasured and must fail.
		 */
		double LambdaLo = -1.0;
		double LambdaHi = -1.0;

		/** Production's drop count, pinned exactly — the DropsToday pattern. */
		int32 ProductionFallen = INDEX_NONE;

		/**
		 * Production's STRANDED count, pinned separately from the drop count and for a
		 * different reason: a stranded piece is one the router could not route, not one
		 * the wall could not hold (DESIGN.md §5.1 — the cycle-division rule is absent), so
		 * part of a production "collapse" can be a routing limitation wearing a collapse's
		 * clothes. The acceptance suite draws the same distinction with StrandsToday; this
		 * is the same pin on the scenario-catalogue path. INDEX_NONE leaves it unasserted.
		 */
		int32 ProductionStranded = INDEX_NONE;
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
	 * Run one fixture both ways. The oracle goes FIRST because BuildRigidBlockProblem
	 * reads a const structure and SolveAndBreak mutates it — the two methods must judge
	 * the IDENTICAL post-cut graph or the diff is between two different structures.
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
	 * One line per fixture, to the LOG IMMEDIATELY as well as to the test report: the
	 * slow sweep can run for minutes per row, and a measurement that only appears when
	 * the whole test finishes is a measurement a killed run loses.
	 */
	void ReportRow(FAutomationTestBase& Test, const FSweepRow& Row, const FSweepReading& R)
	{
		/*
		 * SEVENTEEN DIGITS, not nine, on both measured quantities: the cross-row SAME-NUMBER
		 * pins below assert agreement far tighter than any single row's window, so the log
		 * line has to carry enough digits to re-derive one of those tolerances by hand.
		 */
		const FString Line = FString::Printf(
			TEXT("SWEEP %s: lambda=%.17g answered=%d pivots=%d secs=%.3f blocks=%d joints=%d ")
			TEXT("| production worstU=%.17g passes=%d fallen=%d stranded=%d | expected %s%s%s"),
			Row.Name, R.Oracle.Lambda, R.Oracle.bAnswered ? 1 : 0, R.Oracle.SimplexIterations,
			R.OracleSeconds, R.Blocks, R.Joints,
			R.WorstUtilisation, R.Passes, R.Fallen, R.Stranded, RelationName(Row.Expected),
			R.Oracle.WhyNot.IsEmpty() ? TEXT("") : TEXT(" | whynot: "),
			R.Oracle.WhyNot.IsEmpty() ? TEXT("") : *R.Oracle.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		Test.AddInfo(Line);
	}

	/**
	 * Production's half of the diff, pinned exactly — the DropsToday pattern, in both the
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

		/*
		 * The bridge must represent every swept fixture — this is also the standing
		 * watch that no fixture smuggles in a Y-normal joint the 2D oracle would refuse.
		 */
		if (!Test.TestTrue(
				*FString::Printf(
					TEXT("%s: the bridge must represent this fixture (it said: %s)"),
					Row.Name, *R.BridgeWhy),
				R.bBridged))
		{
			return;
		}

		/*
		 * A pinned refusal asserts the refusal ITSELF: not answered, for the recorded
		 * reason, with production's side of the diff still pinned. An ANSWER here means
		 * the solver's envelope moved — promote the row to a measured relation with a
		 * lambda window; do not delete the failure. Unreachable today (no refusal row
		 * exists since the 2026-08-12 promotion); kept for future refusal pins.
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
	 * Run every row, KEEPING the readings, because some findings live BETWEEN two rows and
	 * cannot be expressed by either row's own pins: two fixtures the LP prices identically
	 * while production separates them, or the reverse. A per-row window is a box around one
	 * number; a cross-row pin asserts two numbers are THE SAME NUMBER, which is a strictly
	 * stronger statement and the only one a same-sized drift on both sides cannot satisfy.
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

	/** A reading by ROW NAME — a cross-row pin names its rows rather than indexing them. */
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
	 * THE SAME-NUMBER PIN. Two fixtures whose lambda* the LP reads IDENTICALLY, asserted at
	 * a tolerance orders tighter than either row's own window — which is the whole point:
	 * a 1e-6 window around each value passes happily while the two drift apart at 1e-9, and
	 * the identity is the finding, not the magnitude.
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
	 * The other half of a pair pin: production's readings on the same two fixtures must
	 * STAY APART by the measured ratio. It pins the discrimination AND it is what stops a
	 * same-number pin passing vacuously — two rows accidentally built from one fixture read
	 * identical lambda* and identical utilisations, which this refuses.
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

	/* ================================================================================
	 * FIXTURE BUILDERS. Producers are production's; hand-laid geometry is transcribed
	 * from the acceptance fixture it mirrors, with preconditions pinning the shape.
	 * ================================================================================ */

	/** Lay a scenario row's structure and apply its cut — production data end to end. */
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
	 * with a 1 cm bed. The slice-1 bridge test proved this layout and the hand-built
	 * oracle problem are the same problem bit for bit.
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
	constexpr double BeamC24BendingMPa = 24.0;
	constexpr double BeamC24ShearMPa = 4.0;
	constexpr double BeamS275YieldMPa = 275.0;
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
	 * The free-end family: a 7-cell running-bond wall of N courses through production's
	 * acceptance-wall producer, with the outermost full brick of the grounded course
	 * removed — the fixture StructureFreeEndHeightTest reads its ladder on.
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
}

/**
 * THE LEANING STACK THROUGH THE REAL FIXTURE AND BRIDGE — sweep item (b), plus the
 * composite-depth measurement of sweep item (e) on the fixture where composite depth is
 * the whole defect.
 *
 * WHAT WAS MEASURED, 2026-08-09 (the pins below): lambda* = 4.4582 / 1.2411 / 0.06293 /
 * 0.03443 at 5/8/30/40 courses — matching slice 1's hand-built ladder through the REAL
 * fixture and bridge this time — while production's cascade agrees on every verdict
 * (0 / 0 / 29 / 39 dropped, the tall rows via the interim guard) and its worst JOINT
 * reading is 0.138781067 AT EVERY HEIGHT. That last number is the composite-depth
 * measurement in one line: the credited deep beam's m^2 cancels the demand's m^2, so a
 * fixture whose true margin the LP measures moving 130x (4.458 -> 0.0344) reads
 * IDENTICAL to the joint checks, and at 30 courses the joint reading overstates the
 * true margin by a factor of ~115 (1/0.1388 = 7.2 of claimed margin against a real
 * lambda* of 0.063). The verdicts only agree because BreakOverturnedBodies exists —
 * exactly what DESIGN.md §7 step 4 will replace with this LP.
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
		ERelation::AgreeStands, 4.4581, 4.4583, 0 });

	Rows.Add({ TEXT("leaning stack, 8 courses"),
		TEXT("bond holds the lean; guard leaves it alone"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(8, Out, Why); },
		ERelation::AgreeStands, 1.24105, 1.24117, 0 });

	Rows.Add({ TEXT("leaning stack, 30 courses"),
		TEXT("no equilibrium at a sixteenth of gravity; production agrees via the interim guard"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(30, Out, Why); },
		ERelation::AgreeFalls, 0.062925, 0.062930, 29, 0 });

	Rows.Add({ TEXT("leaning stack, 40 courses"),
		TEXT("no equilibrium; production agrees via the interim guard"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(40, Out, Why); },
		ERelation::AgreeFalls, 0.034428, 0.034431, 39, 0 });

	RunRows(*this, Rows);

	return true;
}

/**
 * THE BEAM PAIR — sweep item (c): the first measurement of the beam rows' outcomes by a
 * method that HAS global equilibrium and CAN express member failure (the glue line's
 * tension bound is the member's own bending strength at the plastic stress block).
 *
 * WHAT WAS MEASURED, 2026-08-09, and both halves are findings:
 *
 *   - PRODUCTION DROPS ALL THREE BEAMS ENTIRELY, light load included. Its worst joint
 *     reads Max() AS BUILT, one pass breaks it, and every non-grounded piece falls —
 *     verified against Acceptance.Beam.Catalogue's own run the same day (identical
 *     fallen sets and Max() readings), so this is production's real current mode, not a
 *     fixture drift. The BeamAcceptanceTest header's "nothing anywhere is close to
 *     failing, at any load" and DESIGN §6's "light-wood and heavy-steel rows stand"
 *     PREDATE it: the dry bearings' eccentric moments are now refused arching relief
 *     (the one-cell gate's dry-stone refusal, H/V-shaped ratio far over mu = 0.7), the
 *     no-tension bearings read Max outside the kern, and the whole fixture unzips.
 *
 *   - THE ORACLE, WITH GLOBAL EQUILIBRIUM, STANDS ALL THREE: lambda* = 1.764 (heavy
 *     timber), 19.20 (light timber), 17.35 (heavy steel). The bearings equilibrate
 *     trivially at contact points; the material pair discriminates 9.8x (1.764 vs
 *     17.35) where production answers all three identically. The heavy-timber 1.764 is
 *     NOT a contradiction of the catalogue's PARTS AT MIDSPAN: the catalogue's 3.48x is
 *     uncracked FIRST-CRACK arithmetic, the oracle reads the plastic stress block (3x
 *     first-crack, its documented ceiling) PLUS rigid-block load redistribution (the
 *     rigid block may bear at its contact edges; 3.48/3 = 1.16 of plastic capacity,
 *     and redistribution buys the rest). For BRITTLE timber first-crack is the honest
 *     criterion, so the catalogue's verdict survives the measurement — but the margin
 *     is thin, and lambda* says exactly how thin.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSweepBeamPairTest,
	"DestructionGame.Oracle.RigidBlock.Sweep.BeamPair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSweepBeamPairTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	Rows.Add({ TEXT("C24 timber beam, heavy load"),
		TEXT("oracle: plastic stress block + rigid-block redistribution reach 1.76; ")
		TEXT("production: dry bearings read Max() outside the kern and everything falls"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamC24DensityGramsPerCubicCm, BeamC24BendingMPa,
				BeamC24ShearMPa, 120.0, Out, Why);
		},
		ERelation::OracleStandsProductionFalls, 1.76400, 1.76414, 3, 0 });

	Rows.Add({ TEXT("C24 timber beam, light load"),
		TEXT("a sensibly loaded joist; production still drops it whole (bearing Max)"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamC24DensityGramsPerCubicCm, BeamC24BendingMPa,
				BeamC24ShearMPa, 10.0, Out, Why);
		},
		ERelation::OracleStandsProductionFalls, 19.2006, 19.2009, 3, 0 });

	Rows.Add({ TEXT("S275 steel beam, heavy load"),
		TEXT("steel at 17.35 vs timber's 1.76: the oracle discriminates the member ")
		TEXT("material 9.8x; production answers all three rows identically"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamSteelDensityGramsPerCubicCm, BeamS275YieldMPa,
				BeamS275YieldMPa / FMath::Sqrt(3.0), 120.0, Out, Why);
		},
		ERelation::OracleStandsProductionFalls, 17.3527, 17.3529, 3, 0 });

	RunRows(*this, Rows);

	return true;
}

/**
 * THE CORBEL FAMILY — sweep item (a). A and B here; C (7.3 s) and D (81.7 s) in the
 * slow test on measured cost; E35/E36 and F excluded on the dense tableau's arithmetic
 * (file header).
 *
 * WHAT WAS MEASURED, 2026-08-09, against the §8 corbel ruling's recorded cost
 * ("production cannot express overturning of a bonded corbel; the oracle can"): the
 * oracle AGREES WITH EVERY RULING even at the CODED characteristic bond — A 2.905,
 * B 221.4, C 2.567, D 58.06, all >= 1 — so the limit theorem does NOT condemn the
 * corbels the user ruled must stand, and the expected principled disagreement never
 * materialised. The measured margins carry two findings the verdicts cannot:
 *
 *   - FILLING BUYS 76x (A 2.905 -> B 221.4, same reach): a filled corbel jams as a
 *     block where a bare arm hangs on its bond ladder.
 *   - THE COUNTERWEIGHT BUYS 22.6x (C 2.567 -> D 58.06) in the LP, where production
 *     reads C and D within 0.4% of each other (0.34481 vs 0.34348) and the deliberate
 *     red CorbelStepsBeforeTensionWins records they cross 1.0 at 36 steps IDENTICALLY.
 *     Downward-only routing makes the counterweight buy nothing; global equilibrium
 *     measures exactly what it buys. This is the C-vs-D finding turned into a number.
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
		TEXT("a mortared 11.25 cm/course lean of four bricks; bond covers it 2.9x at ")
		TEXT("characteristic strength"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("corbel-a-bare-4"), Out, Why);
		},
		ERelation::AgreeStands, 2.90468, 2.90472, 0 });

	Rows.Add({ TEXT("corbel B, filled four steps"),
		TEXT("the same reach filled solid jams as a block: 76x the bare arm's margin"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("corbel-b-filled-4"), Out, Why);
		},
		ERelation::AgreeStands, 221.427, 221.430, 0 });

	RunRows(*this, Rows);

	return true;
}

/**
 * THE ONE-CELL DRY HALF SEAT WITH ABUTMENT — the disagreement CURRENT_STATE records for
 * this sweep to CLASSIFY, NOT FIX: the oracle stands it by edge-contact jamming (the
 * head joint's top contact supplies the couple at a 7 cm arm), production refuses the
 * arching relief at 1.4921 of sliding capacity (kern + centroid-arm convention) and the
 * dry joint then reads Max(). Limit theorem vs uncracked-section convention; neither
 * wrong; both pinned so a change in EITHER fails loudly.
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
	 * MEASURED 2026-08-09: lambda* = 9592.68 (slice 1's floor of 100 was generous by
	 * two orders), production drops BOTH non-grounded bricks in two passes: pass 1
	 * breaks P's seat (relief refused at 1.4921, dry tension reads Max), P falls back
	 * on its head joint to N, and pass 2 then condemns N's own seat — the combined
	 * resultant sits outside N's kern and a dry joint outside the kern reads Max (the
	 * missing no-tension rocking model, DESIGN's known standing-reads-as-falling gap).
	 * So the disagreement is TWO bricks wide, not one.
	 */
	Rows.Add({ TEXT("one-cell dry half seat, with abutment"),
		TEXT("edge-contact jamming (limit theorem) vs kern-and-centroid refusal at ")
		TEXT("1.4921, which then takes the abutting neighbour down with it"),
		[](FStructure& Out, FString& Why) { return BuildOneCellDryPair(Out, Why); },
		ERelation::OracleStandsProductionFalls, 9592.67, 9592.69, 2, 0 });

	RunRows(*this, Rows);

	return true;
}

/* ====================================================================================
 * THE OPT-IN SLOW SWEEP.
 *
 * ITS NAME DELIBERATELY DOES NOT CONTAIN "DestructionGame", so the documented full-suite
 * command — Automation RunTests DestructionGame — NEVER runs it and the ~30 s suite
 * budget is untouched. Run it deliberately, either whole or one test at a time (the
 * free-end ladder alone is ~23 s, which is what makes it the cheap place to prove a
 * cross-row pin bites):
 *
 *     -ExecCmds="Automation RunTests OracleSlowSweep"
 *
 * This is the documented convention for a slow path: a distinct top-level group, opt-in
 * by filter, never a silent skip. Every row still carries pinned expectations exactly
 * like the fast rows; slowness buys exclusion from the default run and nothing else.
 *
 * WHAT IT COSTS, AND WHY THAT FIGURE IS WRITTEN DOWN. The sparse rewrite (2026-08-12)
 * made the six dense-era rows so cheap that this group ran in 10.5 s — at which point
 * sitting outside the default filter was no longer earned, and the review said so. The
 * 2026-08-12 classification slice restored the rationale by PINNING the thirteen fixtures
 * the rewrite had only measured, and the partial-pricing slice then re-timed every row.
 * MEASURED ON THIS TREE: ~490 s — WallsAndLadders 475.4 s and 478.9 s over two runs,
 * FreeEndHeightLadder 11.8 s both times — against a ~30 s budget for the ENTIRE default
 * suite. (The pre-pricing figures were ~437 s total, 436.7 / 434.9 / 438.1 over three
 * runs, ~415 s of it WallsAndLadders and ~23 s the ladder; the ladder halved while the
 * walls grew, which is the per-fixture trade the WallsAndLadders header works out.)
 * Every run so far has returned
 * bit-identical lambda* and worst readings on all nineteen rows, which is the determinism
 * contract holding across the widest problem set it has ever been asked about. Keep this
 * number current — it is the whole argument for the group existing, and a slow group
 * nobody can justify is a slow group somebody deletes.
 * ==================================================================================== */

/**
 * THE WALL CATALOGUE — sweep item (d), scoped in the dense era to that solver's measured
 * envelope; since the 2026-08-12 sparse rewrite every scoped-out wall except the
 * 30-course five ANSWERS, and as of the same day's classification slice every one of them
 * is PINNED here. Fifteen of the twenty acceptance wall cases now carry a load factor.
 * Rows are laid through the SCENARIO CATALOGUE (production data end to end: the same
 * producer, the same cuts the acceptance suite pins), removed, bridged and diffed.
 * Ordered cheap to expensive so a killed run still leaves its measurements in the log
 * (each row logs immediately — which is exactly how the 2026-08-11 measuring run survived
 * being killed at row 15 of 18). Sweep item (e), the free-end ladder, moved to its own
 * test below when the classification slice gave it a second height to be compared with.
 *
 * WHAT THE 2026-08-11 RUN MEASURED, ROW BY ROW. Answered, and now pinned below:
 *
 *     corbel C     lambda* 2.56724674   1,502 pivots   6.8 s    51 blk / 110 jnt
 *     wall-08      lambda* 324.732096   1,289 pivots   4.7 s    52 blk / 101 jnt
 *     wall-13      lambda* 645.946096   2,948 pivots  53.6 s    87 blk / 218 jnt
 *     wall-14      lambda* 232.549126   2,541 pivots  45.0 s    89 blk / 215 jnt
 *     corbel D     lambda* 58.0599138   4,092 pivots  79.6 s    90 blk / 200 jnt
 *
 * REFUSED — post-solve verification failed, fail closed, at every fixture >= 119
 * blocks (production's half of each diff still measured, and it matched every
 * acceptance pin: wall-19 dropped 34 and stranded 6, wall-10 dropped 12 and
 * stranded 3, everything else dropped nothing):
 *
 *     free end 7x10   74 blk / 183 jnt    3,993 pivots    69 s   (worstU 0.9235)
 *     wall-17        120 blk / 207 jnt    4,293 pivots   104 s
 *     wall-18        119 blk / 203 jnt    4,894 pivots   114 s
 *     wall-15        125 blk / 320 jnt    5,372 pivots   237 s
 *     wall-16        125 blk / 320 jnt    7,230 pivots   408 s
 *     wall-19        119 blk / 308 jnt    5,173 pivots   233 s
 *     wall-06        146 blk / 372 jnt   13,397 pivots 1,188 s
 *     wall-07        140 blk / 350 jnt    8,551 pivots   570 s
 *     wall-10        138 blk / 349 jnt    5,873 pivots   316 s
 *
 * The free-end row stayed LIVE in this test as the pinned canary — one refusal kept
 * executable so the envelope was asserted by a test rather than remembered by a
 * comment, and chosen because it is the lambda = 3.464 measurement fixture: the day
 * the solver answers it, the canary fails and the gap-5 measurement it owes becomes
 * takeable. THAT DAY WAS 2026-08-12: the sparse rewrite answered it, the canary
 * failed exactly as written (lambda* 256.820018 against the pinned refusal), and its
 * promotion now lives in FreeEndHeightLadder below, beside the second height that
 * turned one reading into the ladder measurement.
 *
 * WHAT THE 2026-08-12 SPARSE MEASURING RUN ANSWERED — every residual verified, and as
 * of the 2026-08-12 CLASSIFICATION SLICE every one of them is a PINNED ROW below rather
 * than a comment. The measuring run's cost table is kept because it is the reason the
 * rows sit in the opt-in group and because it is the only record of the pivot counts.
 * MEASURED PRE-PARTIAL-PRICING (2026-08-12, Dantzig path). EVERY LAMBDA* COLUMN BELOW IS
 * AN OLD-PATH READING, and five of them have since moved: the uncommitted partial-pricing
 * slice takes a different pivot path, which re-pinned the wall-10, wall-19, wall-06,
 * wall-12 and wall-09 windows. The PARTIAL-PRICING RE-PIN note by wall-10 carries the
 * new-path value for each and the spread between the two; this table is kept as the
 * Dantzig-path record — the only surviving one of its pivot counts — not as the current
 * reading:
 *
 *     wall-18          649.464192      791 pv    1.3 s   119 blk / 203 jnt
 *     wall-17          918.046031    1,608 pv    4.2 s   120 blk / 207 jnt
 *     wall-10           35.8172298   2,640 pv   11.5 s   138 blk / 349 jnt  (production: 12 fall, 3 stranded)
 *     wall-19           12.3824832   2,970 pv   16.1 s   119 blk / 308 jnt  (production: 34 fall, 6 stranded)
 *     wall-12           89.1151243   2,877 pv   16.2 s   128 blk / 326 jnt
 *     wall-16          868.623736    3,319 pv   23.1 s   125 blk / 320 jnt
 *     wall-15          868.623736    3,745 pv   25.7 s   125 blk / 320 jnt
 *     wall-20           82.629597    3,387 pv   27.9 s   156 blk / 384 jnt  (production: 9 fall in 1 pass)
 *     wall-11          128.118261    5,102 pv   45.8 s   128 blk / 326 jnt
 *     wall-09           36.5639285   4,885 pv   50.4 s   146 blk / 350 jnt  (production stands at worstU 0.985)
 *     wall-07          296.220581    5,293 pv   62.0 s   140 blk / 350 jnt
 *     wall-06          622.031942    8,819 pv  126.6 s   146 blk / 372 jnt
 *     free end 7x20    256.820018   2,701 pv   22.5 s   149 blk / 388 jnt  (production: 1 falls, worstU 1.958)
 *
 * NOTE, POST-PARTIAL-PRICING. Two rows illustrate the trade in opposite directions:
 * corbel D 8,439 pivots / ~9.0 s (8.95 and 8.995 over two runs) and wall-09 25,848
 * pivots / 120.9-124.6 s against the table's 4,885 pv / 50.4 s. Corbel D IS a row of this
 * slow test and always has been — the 2026-08-12 table above simply omits it, because it
 * was timed on 2026-08-11 at 79.6 s (4,092 pv) and never re-listed; the comparison is
 * against that older figure rather than against a fast-suite measurement, which is what
 * an earlier draft of this note wrongly claimed. BOTH rows take MORE pivots; corbel D
 * still finishes ~8.8x faster and wall-09 ~2.4x slower. Partial pricing buys cheap
 * iterations and spends some of the saving on a longer path, and which way the net falls
 * is a property of the fixture.
 *
 * WHAT IT COSTS OVERALL, HONESTLY: +9.6% across the THIRTEEN ROWS THIS TABLE TIMES, 433 s
 * of solver on the Dantzig path against 475 s on the partial-pricing one, and the per-row
 * outcomes run from 5.3x FASTER (wall-15, 25.7 -> 4.9 s) to 2.7x SLOWER (wall-19, 16.1 ->
 * 44.2 s), with wall-09 (50.4 -> 120.9 s) and wall-12 (16.2 -> 33.7 s) the other two that
 * pay. Calling that "a wash" reads as neutral and hides both tails. THE ARGUMENT FOR THE
 * SLICE IS NOT THIS TEST'S TOTAL — it is wall-01, which full Dantzig never answered at
 * all (cut off still pivoting after ~45 minutes) and which the partial pricer answers at
 * lambda* = 272.20 in ~4.2 minutes. A ~10% tax on the rows that already ran, in exchange
 * for a scale that did not run, is the trade being made; Devex/dynamic weights are the
 * roadmapped way to stop paying it (CURRENT_STATE, the solver performance roadmap).
 *
 * WHAT THE MEASUREMENTS SAID ONCE CLASSIFIED, which is the part a table cannot carry:
 *
 *   - THE ORACLE'S ORDERING AGREED WITH THE CATALOGUE'S VERDICTS EVEN WHERE ITS
 *     THRESHOLD DID NOT, AND THE 2026-08-12 RULINGS THEN FOLLOWED THE ORDERING. As
 *     measured, the four rows the catalogue did not rule STANDS read the four LOWEST
 *     lambda* of the fifteen wall fixtures — 19 at 12.38, 10 at 35.82, 9 at 36.56, 20 at
 *     82.63 — and the next lowest is wall-12 at 89.12: a clean separation at ~85 with
 *     nothing in between, so the LP ranked the human rulings correctly and simply put the
 *     collapse line somewhere else. THREE OF THOSE FOUR HAVE SINCE BEEN RE-RULED TO
 *     STAND, so the ordering is no longer a ranking of verdicts; what survives is that
 *     the four fixtures the catalogue found hardest are still the four the LP prices
 *     lowest, which is the cross-method agreement the rulings rested on. No separate
 *     assertion pins the ordering because every row's window is tight enough that a
 *     reorder cannot happen without a window failing first.
 *   - THE FOUR ROWS THAT NEEDED A USER RULING WERE ALL RULED ON 2026-08-12, and each
 *     records its own outcome in its mechanism text: 9, 10 and 19 re-ruled from COLLAPSE
 *     to STANDS, 20 confirmed as it stood. The wall-08 precedent held throughout — the
 *     pin records the outlier state, the ruling resolves it, and the two are separate
 *     acts, so not one lambda* window or relation enum moved with the rulings.
 *   - AND THE RULINGS SPLIT THE ACCEPTANCE SET IN A WAY IT HAD NOT BEEN SPLIT BEFORE.
 *     Case 9's re-ruling handed its row to the model and it went green; cases 10 and 19
 *     stayed red with the SIGN REVERSED — the catalogue now says they stand and
 *     production drops 12 and 34. Those are the suite's first rows where a STANDS verdict
 *     is the thing the model fails.
 *   - TWO PAIRS ARE PINNED AS CROSS-ROW IDENTITIES rather than as two windows: 15/16
 *     (the LP reads the superimposed-load pair as the SAME NUMBER) and the free-end
 *     height ladder in its own test below. See CheckSameLambda for why an identity is a
 *     strictly stronger pin than two boxes.
 *   - TWO OF THE FOUR PRODUCTION "COLLAPSES" BREAK NO JOINT AT ALL. wall-10 (12 down)
 *     and wall-19 (34 down) both cascade in ZERO passes at worst readings of 0.300 and
 *     0.318: every one of those pieces is UNROUTED, not overloaded. That is measurable
 *     here and nowhere else in the suite — the acceptance rows count what came down, not
 *     what broke — and it says the disagreement with the LP on those two rows is about a
 *     missing mechanism (nothing routes load sideways or upward) rather than about
 *     strength. THAT ZERO IS WHAT DECIDED THE 2026-08-12 RULINGS ON BOTH: a router with
 *     nowhere to send the load is not a third opinion about masonry, so those two
 *     "collapses" were never evidence against the LP. wall-20 is the opposite and its
 *     text says so: one pass, worst reading 42.71, a joint genuinely 42x over capacity —
 *     which is exactly why its verdict was CONFIRMED rather than moved.
 *   - THE STACK-BOND PAIR SEPARATES 36.75x IN PRODUCTION (0.040033 with a brick out
 *     against 0.0010893 intact) and 1.41x in the LP. Deliberately NOT pinned as a ratio:
 *     the stack-bond family is where production carries a standing deliberate red
 *     (StackBondColumnShearIsHeightIndependent reads this routing height-linearly), and
 *     pinning a ratio over a known-wrong reading dresses the defect as a discrimination
 *     — the trap DESIGN §8 recorded for the 7-vs-8 cover pair.
 *
 * The 30-course walls (cases 1-5) remain beyond the PRACTICAL envelope — now on
 * pricing cost, not memory; the file header at the top carries the measured detail.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowWallSweepTest,
	"OracleSlowSweep.RigidBlock.WallsAndLadders",
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
	 * THE ROW THAT SETTLED A RULING RATHER THAN RECORDING ONE. Until 2026-08-11 this row
	 * carried a flag: the acceptance catalogue ruled wall-08 a LOCAL LOSS, production
	 * dropped nothing, this measurement stood it at 325x, and the catalogue was the
	 * OUTLIER OF THREE METHODS with the disagreement escalated for a user decision. The
	 * user re-ruled case 8 STANDS on that basis (DESIGN §8, the standing instruction from
	 * cases 11 and 12), so the flag retires and the three methods now AGREE.
	 *
	 * The relation enum does not move and never did: `AgreeStands` is oracle-vs-PRODUCTION
	 * by construction (lambda* >= 1 and the cascade dropped nothing), which was already
	 * true while the catalogue disagreed. What changed is only what the mechanism text has
	 * to say — and it must still name the mechanism, because a pin reading "everyone
	 * agrees" is a pin nobody can check.
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
		ERelation::AgreeStands, 324.731, 324.733, 0 });

	Rows.Add({ TEXT("corbel C, ten steps"),
		TEXT("the §8 ruling's fixture: the limit theorem agrees it stands, 2.57x at ")
		TEXT("characteristic bond"),
		Scenario(TEXT("corbel-c-10")),
		ERelation::AgreeStands, 2.56723, 2.56726, 0 });

	Rows.Add({ TEXT("wall-14 corbel, half brick per course"),
		TEXT("the projection pair's far half; see wall-13 for the pair's cross-method ")
		TEXT("agreement"),
		Scenario(TEXT("wall-14")),
		ERelation::AgreeStands, 232.548, 232.550, 0 });

	Rows.Add({ TEXT("wall-13 corbel, quarter brick per course"),
		TEXT("the corbel projection pair, measured BOTH ways: production's joint ")
		TEXT("readings separate 13 from 14 by 0.195160875/0.0702368 = 2.7786x, the LP's ")
		TEXT("lambda* by 645.946/232.549 = 2.7777x — two methods derived independently ")
		TEXT("agree on the projection term to 0.03%, the strongest cross-validation of ")
		TEXT("the composite-depth reading the suite has"),
		Scenario(TEXT("wall-13")),
		ERelation::AgreeStands, 645.945, 645.947, 0 });

	Rows.Add({ TEXT("corbel D, ten steps with counterweight"),
		TEXT("global equilibrium prices the counterweight at 22.6x (C's 2.57 -> 58.06) ")
		TEXT("where production reads C and D 0.4% apart (0.34481 vs 0.34348) and the ")
		TEXT("deliberate red CorbelStepsBeforeTensionWins records them crossing 1.0 at ")
		TEXT("36 steps identically"),
		Scenario(TEXT("corbel-d-10-counterweight")),
		ERelation::AgreeStands, 58.0597, 58.0601, 0 });

	/* --- the twelve the sparse rewrite unblocked, pinned 2026-08-12, cheap first ------ */

	/*
	 * THE BOND PAIR, AND CASE 17'S FIRST NUMBER OF ANY KIND. CURRENT_STATE's standing
	 * doubt says case 17 "has zero discriminating power intact" — as an OUTCOME row that
	 * is true and stays true (both halves stand), but the LP gives the pair a margin to
	 * separate: 918.05 intact against 649.46 with one brick out, 1.41x. Production's own
	 * readings on the two are printed by the sweep line and are NOT pinned as a ratio
	 * here, because the stack-bond family is where production is deliberately red
	 * (StackBondColumnShearIsHeightIndependent) and a ratio pin over a known-wrong
	 * reading would dress the defect up as a discrimination — the same trap DESIGN §8
	 * recorded for the 7-vs-8 cover pair.
	 */
	Rows.Add({ TEXT("wall-18 stack bond, one brick out"),
		TEXT("stack bond has no bond to spread a loss, so the column over the hole hangs ")
		TEXT("on head-joint tension into its neighbours: 649.46x at characteristic bond, ")
		TEXT("1.41x less margin than the intact wall-17, which is the pair's ONLY ")
		TEXT("discrimination now that both halves stand"),
		Scenario(TEXT("wall-18")),
		ERelation::AgreeStands, 649.46354, 649.46484, 0, 0 });

	Rows.Add({ TEXT("wall-17 stack bond, intact"),
		TEXT("the highest lambda* in the swept set (918.05x) and the first absolute ")
		TEXT("number case 17 has ever carried; an intact stack-bond wall standing is not ")
		TEXT("news, the MARGIN against wall-18 is"),
		Scenario(TEXT("wall-17")),
		ERelation::AgreeStands, 918.04511, 918.04695, 0, 0 });

	/*
	 * ================== THE FOUR FLAGGED ROWS, ALL RULED ON 2026-08-12 ==================
	 * Rows 10, 19, 9 and 20 carried a FLAGGED FOR A RULING banner from the 2026-08-12
	 * classification slice until the user ruled on them the same day. The banners retire
	 * here; the measurements do not move, and neither does a single relation enum — every
	 * one of these rows is oracle-vs-PRODUCTION by construction and none of them ever
	 * spoke for the catalogue. What the rulings changed is what the mechanism texts have
	 * to say, exactly as the wall-08 precedent above did:
	 *
	 *     wall-09   COLLAPSE -> STANDS   the measurement moved the catalogue, and the
	 *                                    model agrees, so the acceptance row went GREEN
	 *     wall-10   COLLAPSE -> STANDS   the measurement moved the catalogue, and the
	 *     wall-19   COLLAPSE -> STANDS   model does NOT agree — both stay red in a NEW
	 *                                    direction (expected to stand, 12 and 34 dropped)
	 *     wall-20   LOCAL LOSS, KEPT     the one ruling that CONFIRMED a row rather than
	 *                                    moving it; lambda* has no local vocabulary and
	 *                                    the measurement could not reach the question
	 *
	 * RECONCILING THE HAND FIGURES WITH LAMBDA*, so nobody reads them as the same check —
	 * and this is what the rulings weighed: the hand arithmetic in these rows is a
	 * TENSION-PLANE ELASTIC check (bending stress on a vertical crack plane vs flexural
	 * strength) — the same kind of answer production computes, which is why the hand
	 * figures and production's readings agree within the routing. Lambda* is a different
	 * question: the limit theorem finds the best ADMISSIBLE COMPRESSION PATH, which for
	 * these fixtures needs almost no tension at all — that is why lambda* runs 14-32x
	 * above the hand figures' implied factors, a gap the declared slants (x3 plastic, x6
	 * strength basis) do not cover and are not meant to. The hand checks corroborate
	 * PRODUCTION's number; lambda* answers whether any equilibrium exists. A ruling weighs
	 * both, it does not average them — and on 9, 10 and 19 what tipped all three was that
	 * the hand check ALSO acquitted the fixture on the strength basis the rulings are
	 * argued on, so lambda* was confirming rather than outvoting.
	 * ==================================================================================
	 */

	/*
	 * THE PARTIAL-PRICING RE-PIN (2026-08-12). An uncommitted partial-pricing slice
	 * changed the simplex's pivot PATH — lambda* stays bit-identical on every
	 * validation fixture, but on FIVE sweep fixtures at this scale (120-150 blocks)
	 * the new path lands a different distance from the unique optimum:
	 *
	 *     wall-10   35.8172298   -> 35.817113279469787    3.25e-6 relative
	 *     wall-19   12.3824832   -> 12.382629959455667    1.19e-5 relative  (the worst)
	 *     wall-06  622.031942    -> 622.03383039924574    3.04e-6 relative
	 *     wall-12   89.1151243   ->  89.115041750073942   9.26e-7 relative
	 *     wall-09   36.5639285   ->  36.563909109648513   5.30e-7 relative
	 *
	 * BOTH readings on every fixture are CERTIFIED — each passes its own post-solve
	 * verification at 1e-6 relative — so the old +/-1e-6 windows were unknowingly
	 * pinning the PIVOT PATH, not the optimum: at this problem scale (thousands of
	 * pivots across hundreds of rows) the solver's placement AROUND the true optimum
	 * spreads by roughly 1e-5 relative across pivot paths, and the worst measured
	 * spread anywhere is 1.19e-5 (wall-19). All five windows are re-centred on the
	 * MIDPOINT of the two certified readings with a +/-2e-5 relative half-width — a
	 * shade over the worst measured spread, honest about the METHOD rather than about
	 * one path through it. A window tighter than ~1e-5 relative at this scale pins the
	 * ALGORITHM, not the physics.
	 *
	 * WHY THE LAST TWO ARE RE-PINNED THOUGH THEY DID NOT FAIL, which is the part that
	 * looks like make-work and is not: wall-12 and wall-09 moved 9.3e-7 and 5.3e-7 and
	 * both landed INSIDE their old +/-1e-6 boxes — 6.5% and 24% of the box width from
	 * the floor. A pass with that little clearance is a pass by luck. The next change of
	 * the same magnitude, in the same direction, puts them outside, and the failure that
	 * arrives then reads as a MASONRY REGRESSION on two wall fixtures rather than as
	 * arithmetic noise from a different pivot path — a phantom the reader would chase
	 * through the physics. Applying the file's own rule to every row it touches, rather
	 * than only to the rows that happened to break, is what stops that.
	 *
	 * Where exactness genuinely matters — the 15/16 superimposed-load pair and the
	 * free-end height pair — the right tool is the SAME-NUMBER identity check
	 * (CheckSameLambda) rather than a tighter window: both sides of an identity move
	 * TOGETHER under a pivot-path change, which is exactly why those pins needed no
	 * change here. No window outside these five moved for this re-pin; every other row
	 * still reads inside its existing box.
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
		TEXT("cells, which is what the ruling credited. AND ")
		TEXT("PRODUCTION BREAKS NOTHING TO GET HERE: 0 cascade passes at a worst reading ")
		TEXT("of 0.300, so all 12 are pieces the downward-only router could not route, ")
		TEXT("not joints that gave — its verdict is an ABSENT MECHANISM, not a strength ")
		TEXT("finding, and that is what decided the ruling: a router with nowhere to send ")
		TEXT("the load is not a third opinion about masonry"),
		Scenario(TEXT("wall-10")),
		/*
		 * RE-PINNED 2026-08-12 for the partial-pricing pivot-path spread (see the
		 * PARTIAL-PRICING RE-PIN note above): midpoint of 35.8172298 (old path) and
		 * 35.817113279469787 (new path), +/-2e-5 relative.
		 */
		ERelation::OracleStandsProductionFalls, 35.81645, 35.81789, 12, 3 });

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
		TEXT("that, which the ruling knows. Production ")
		TEXT("breaks NOTHING to drop 34 either (0 cascade passes, worst reading 0.318) — ")
		TEXT("with the base gone a downward-only router has nowhere to send the load, so ")
		TEXT("this row and wall-10 both report an absent mechanism as a collapse"),
		Scenario(TEXT("wall-19")),
		/*
		 * RE-PINNED 2026-08-12 for the partial-pricing pivot-path spread (see the
		 * PARTIAL-PRICING RE-PIN note above wall-10): midpoint of 12.3824832 (old
		 * path) and 12.382629959455667 (new path), +/-2e-5 relative — this fixture
		 * carries the WORST measured pivot-path spread (1.19e-5), which is the
		 * measurement the +/-2e-5 half-width is built from.
		 */
		ERelation::OracleStandsProductionFalls, 12.382308, 12.382805, 34, 6 });

	/*
	 * THE PIER PAIR, WHOSE DISCRIMINATION DESIGN §8 EXPLICITLY HANDED TO THIS ORACLE.
	 * The 2026-08-09 case-12 ruling recorded the cost of rewriting case 12: "the pier-width
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
		 * RE-PINNED 2026-08-12 for the partial-pricing pivot-path spread (see the
		 * PARTIAL-PRICING RE-PIN note above wall-10): midpoint of 89.1151243 (old
		 * path) and 89.115041750073942 (new path), +/-2e-5 relative. This row did NOT
		 * fail — the new reading sat 6.5% of the way into the old box — and that is
		 * exactly why it is re-pinned rather than left to fail next time as a phantom
		 * masonry regression.
		 */
		ERelation::AgreeStands, 89.113300, 89.116866, 0, 0 });

	/*
	 * THE SUPERIMPOSED-LOAD PAIR READS AS ONE NUMBER, AND THAT IS A STATEMENT ABOUT THE
	 * PROBLEM SHAPE, NOT A COINCIDENCE. Gravity is the LP's single live load
	 * (FOracleProblem::bGravityIsLive), so piling six courses on the header's tail scales
	 * the demand it makes and the pre-compression that steadies it by the same lambda, and
	 * a multiplier-on-own-weight measure cannot see the difference. Production does see it
	 * — 31.6x at the header's own joint — so the pair is pinned from BOTH sides below: the
	 * lambda* identity and production's reading ratio, the second of which is also what
	 * stops the identity passing vacuously if the two rows ever built one fixture.
	 * WANTED (logged for CURRENT_STATE): the honest LP-side version of this pair marks the
	 * six courses LIVE and the rest DEAD, which the problem shape already supports and
	 * which nothing but the sliding rows exercises.
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
		TEXT("THE RULING THAT CONFIRMED A ROW RATHER THAN MOVING IT: examined on 2026-08-12 ")
		TEXT("beside 9, 10 and 19 — which all moved — and ruled UNCHANGED, verdict, named ")
		TEXT("teeth, pins and doubt alike. This is the row where the vocabularies do not meet: the ")
		TEXT("catalogue rules a LOCAL LOSS of two NAMED teeth (course 3 cell 4.5, course ")
		TEXT("5 cell 2.5), production drops 9, and a GLOBAL lambda* has no local ")
		TEXT("vocabulary at all — 82.63x says every block INCLUDING both teeth has an ")
		TEXT("admissible equilibrium. Why hanging a tooth is cheap, by hand: 0.1 MPa over ")
		TEXT("two head joints (2 x 66.625 cm^2) plus the two bed patches above it (2 x ")
		TEXT("105.0625 cm^2) is 3434 N against a brick's 2667 uu = 26.67 N, about 129x, ")
		TEXT("so the teeth are not what governs 82.63 and the LP is not merely tolerating ")
		TEXT("them. CURRENT_STATE's standing doubt (true count more than 2, fewer than 9) ")
		TEXT("is untouched by this measurement, stays open, and waits on equilibrium ")
		TEXT("promotion rather than on another ruling. Unlike rows 10 and 19 this ")
		TEXT("one IS a strength verdict in production: worst reading 42.71, one cascade ")
		TEXT("pass, i.e. a joint 42x over capacity — the raking cut leaves teeth hanging ")
		TEXT("where the two rows above merely leave pieces unrouted"),
		Scenario(TEXT("wall-20")),
		ERelation::OracleStandsProductionFalls, 82.629514, 82.629680, 9, 0 });

	Rows.Add({ TEXT("wall-11 wall on two piers, six-brick clear span"),
		TEXT("the pier pair's wide half at 128.12x; the §8 case-11 ruling worked this ")
		TEXT("fixture by hand at 0.03-0.04 MPa of deep-beam bending against mean bond, ")
		TEXT("and the LP's three-figure margin at CHARACTERISTIC bond is that ruling's ")
		TEXT("first independent confirmation"),
		Scenario(TEXT("wall-11")),
		ERelation::AgreeStands, 128.11813, 128.11839, 0, 0 });

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
		/*
		 * RE-PINNED 2026-08-12 for the partial-pricing pivot-path spread (see the
		 * PARTIAL-PRICING RE-PIN note above wall-10): midpoint of 36.5639285 (old
		 * path) and 36.563909109648513 (new path), +/-2e-5 relative. Like wall-12 this
		 * row did NOT fail — the new reading sat 24% of the way into the old box — and
		 * is re-pinned so the next same-magnitude pivot-path change cannot arrive
		 * dressed as a masonry regression on the file's least stable relation.
		 */
		ERelation::AgreeStands, 36.563187, 36.564651, 0, 0 });

	/*
	 * THE COVER PAIR'S HONEST MEASUREMENT, OWED SINCE 2026-08-11 AND UNCOMFORTABLE.
	 * DESIGN §8's case-8 entry declined to pin 7-vs-8 on readings because production reads
	 * case 7 WORSE than case 8 at the same joint (0.269 vs 0.219, 1.23x) and "a downward
	 * router reads cover as load, never capacity" — pinning it would encode the defect as
	 * a discrimination. The LP, which has global equilibrium and no router at all, reads
	 * the SAME DIRECTION: 296.22 with eight courses of cover against 324.73 with one,
	 * 1.10x worse. Lambda* is a multiplier on the fixture's OWN weight, so added cover
	 * raises demand along with capacity and the net direction depends on which grows
	 * faster — here it came out down, and the two walls also differ in size (140 vs 52
	 * blocks), so the cross-fixture comparison is ordinal at best. What survives all of
	 * that is the useful part: a method with NO router reproduces the direction, so the
	 * direction alone is uninformative about the router defect. That does not make
	 * production's 1.23x right. Whether §8's note should move on this is a USER call, and
	 * it is still open. The MatchedPairs row it mentions is doubly moot as of 2026-08-12:
	 * that whole test retired when cases 9 and 10 were re-ruled and its last two pairs
	 * lost their separation.
	 */
	Rows.Add({ TEXT("wall-07 four-cell opening, eight courses over"),
		TEXT("296.22x against wall-08's 324.73x: the LP prices eight courses of cover ")
		TEXT("1.10x WORSE than one, the same direction production reads at the joint and ")
		TEXT("for a reason that is about the measure (lambda* is a multiplier on own ")
		TEXT("weight) rather than about the router"),
		Scenario(TEXT("wall-07")),
		ERelation::AgreeStands, 296.2203, 296.2209, 0, 0 });

	Rows.Add({ TEXT("wall-06 two-cell opening, deep cover"),
		TEXT("the span ladder's short end: half wall-07's span under the same cover ")
		TEXT("prices 2.10x its margin (622.03 vs 296.22), which is the discrimination the ")
		TEXT("outcome column never had — 06, 07 and 08 all stand and always did"),
		Scenario(TEXT("wall-06")),
		/*
		 * RE-PINNED 2026-08-12 for the partial-pricing pivot-path spread (see the
		 * PARTIAL-PRICING RE-PIN note above wall-10, this file's WallsAndLadders
		 * test): midpoint of 622.031942 (old path) and 622.03383039924574 (new
		 * path), +/-2e-5 relative.
		 */
		ERelation::AgreeStands, 622.0204, 622.0454, 0, 0 });

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

		/*
		 * And the production side must STAY APART. The measured ratio is pinned wide
		 * enough to be a ratio rather than a third copy of two windows, and narrow enough
		 * that the pair collapsing to one fixture — which would satisfy the identity above
		 * perfectly — cannot pass.
		 */
		CheckReadingRatio(
			*this,
			TEXT("production separates the superimposed-load pair where the LP does not"),
			Bare->WorstUtilisation, Loaded->WorstUtilisation,
			SuperimposedReadingRatioLo, SuperimposedReadingRatioHi);
	}

	return true;
}

/**
 * THE FREE-END HEIGHT LADDER — the first real measurement of the lambda = 3.464 ruling
 * (DESIGN.md §7 gap 5), and the reason it is its own test rather than two more rows: the
 * finding lives BETWEEN the two heights and neither row's window can state it.
 *
 * THE FIXTURE is the one StructureFreeEndHeightTest reads its ladder on — a 7-cell
 * running-bond wall with the outermost full brick of the grounded course removed — at 10
 * courses and at 20. The dense solver refused the 10-course wall and that refusal was
 * this file's pinned canary until the 2026-08-12 sparse rewrite answered it (lambda*
 * 256.820018 against the pinned refusal, exactly as the canary was written to fail); the
 * 20-course wall was measured in the same run and is pinned here for the first time.
 *
 * WHAT THE PAIR MEASURES, AND THE TWO SIDES DISAGREE ABOUT HEIGHT:
 *
 *   - THE LP READS THE SAME NUMBER AT BOTH HEIGHTS. Doubling the wall does not move
 *     lambda* at all, which says the mechanism that governs the LP's answer is NOT the
 *     free-end half seat: a half seat's demand grows with the courses stacked over it
 *     while its capacity does not, so a governing half seat would halve lambda*. Some
 *     local feature the two walls SHARE is what binds. That is a statement about the
 *     limit theorem's answer, not about lambda the constant — and it is pinned as an
 *     identity so that a solver change on either height fails loudly instead of moving
 *     both windows a little.
 *   - PRODUCTION IS HEIGHT-LINEAR AND CROSSES 1.0 BETWEEN THE TWO. Its worst reading is
 *     an UNRELIEVED-half-seat-shaped number — an exact multiple of the 0.058203838...
 *     per-brick-weight anchor (15.867 bw at ten courses, 33.635 at twenty) — reading
 *     ~0.92 then ~1.96: under capacity, then over it, and the 20-course wall
 *     accordingly drops a piece. WHICH joint carries it has NOT been established here:
 *     it is provably NOT the composite-relieved seat that
 *     Core.Structure.AFreeEndDeletionInATallWall reads on this fixture (that joint
 *     reads 0.32827956 at TWENTY courses, ~0.0116 per bw, arm-capped and height-flat —
 *     five times lower), and 28.33 bw unrelieved would read 1.649, not 1.958, so the
 *     worst joint also carries more than that brick's seat. A candidate is the patch
 *     over the seatless half bat; the specified decomposition test (CURRENT_STATE, LP
 *     oracle section) names it arithmetically rather than by prose. DESIGN §5.5
 *     records the linearity as live behaviour; this pins BOTH the ratio and the
 *     crossing, so the two halves of the finding cannot rot separately.
 *
 * The free-end LADDER's published 0.3283 (at TWENTY courses — the ladder's heights are
 * 20/30/40/50/60) is that composite-relieved seat, a different reading from this
 * fixture's worst; quoting it as production's worst for this fixture was the
 * 2026-08-09 draft's error and is not repeated here.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowFreeEndLadderTest,
	"OracleSlowSweep.RigidBlock.FreeEndHeightLadder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowFreeEndLadderTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	Rows.Add({ TEXT("free end, 7 cells x 10 courses"),
		TEXT("the promoted canary: the LP prices the ten-course free end at 256.82x own ")
		TEXT("weight while production's worst joint reads 0.92 of capacity and holds"),
		[](FStructure& Out, FString& Why) { return BuildFreeEnd(10, Out, Why); },
		ERelation::AgreeStands, 256.819, 256.821, 0, 0 });

	Rows.Add({ TEXT("free end, 7 cells x 20 courses"),
		TEXT("the same wall twice as tall: the LP does not move (256.82x, pinned as an ")
		TEXT("identity below) while production's worst reading doubles past 1.0 and the ")
		TEXT("cascade drops the brick above the hole — the whole disagreement about ")
		TEXT("height in one pair"),
		[](FStructure& Out, FString& Why) { return BuildFreeEnd(20, Out, Why); },
		ERelation::OracleStandsProductionFalls, 256.819, 256.821, 1, 0 });

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

	CheckSameLambda(
		*this,
		TEXT("the free-end ladder's height independence (7x10 vs 7x20)"),
		*Short, *Tall, SameNumberRelativeTolerance);

	/*
	 * PRODUCTION'S SIDE, PINNED AS A RATIO AND AS A CROSSING. The ratio alone would pass
	 * if both readings halved; the crossing alone would pass if the tall wall crept over
	 * 1.0 by a hair. Together they say the reading is height-LINEAR and that the line
	 * crosses capacity between these two heights, which is the half of the finding that
	 * makes the LP's non-movement worth reporting at all.
	 */
	CheckReadingRatio(
		*this,
		TEXT("production's free-end reading is height-linear where the LP is height-flat"),
		Tall->WorstUtilisation, Short->WorstUtilisation,
		FreeEndReadingRatioLo, FreeEndReadingRatioHi);

	TestTrue(
		*FString::Printf(
			TEXT("production's free-end reading must CROSS capacity between 10 and 20 ")
			TEXT("courses: 10 courses read %.17g (must be under 1.0) and 20 courses read ")
			TEXT("%.17g (must be over it). If both now sit on one side, the crossing has ")
			TEXT("moved and the ladder's slope is what to re-derive"),
			Short->WorstUtilisation, Tall->WorstUtilisation),
		Short->WorstUtilisation < 1.0 && Tall->WorstUtilisation > 1.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
