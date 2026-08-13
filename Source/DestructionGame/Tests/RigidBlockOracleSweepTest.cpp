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

		/**
		 * The oracle refuses with "PHASE-2 SIMPLEX FAILED", which is a different refusal
		 * from the one above and needed its own enumerator rather than a widened check:
		 * the verification gate rejects an answer it has computed, while this one never
		 * reaches an answer at all. CURRENT_STATE recorded that the eight-course-opening
		 * family (acceptance case 22) refuses this way and that
		 * `OracleRefusesAtThisScale` could not pin it, because that branch asserts the
		 * reason contains "failed verification".
		 *
		 * SCALE WAS NEVER THE EXPLANATION, and the canary discipline paid: the abutment
		 * ladder's j=4 rung refused at 107 blocks while its own j=3 rung (95) and the rise
		 * ladder's s=4 (149) answered, that row failed the day the solver was fixed, and it
		 * was PROMOTED to a measured relation rather than absorbed (2026-08-13 — the
		 * ratio-test relative pivot floor in RigidBlockOracle.cpp; the ladder now reads
		 * 5.511 / 7.379 / 9.359 across j=2/3/4). NO ROW USES THIS TODAY. The machinery
		 * stays because the eight-course-opening family (acceptance case 22) still refuses
		 * this way at 218+ blocks for a reason that outlived the fix — recorded in
		 * CURRENT_STATE — and a row pinning that refusal is the next user of it.
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

		/**
		 * A PRICING EXPERIMENT'S KNOB: rewrite the ORACLE'S PROBLEM after the bridge has
		 * built it and before the LP is solved, returning how many joints it changed.
		 *
		 * WHY THE PROBLEM AND NOT THE STRUCTURE. The question these rows exist to answer is
		 * "which of the LP's capacities is binding" — so the fixture must be held EXACTLY
		 * fixed while one strength moves, and production must be left reading the untouched
		 * wall. Rewriting FStructure would move both methods at once and there would be no
		 * control; rewriting FOracleProblem::Joints[i].Strength moves one and leaves
		 * production as a bit-identical control, which the probe rows then ASSERT.
		 *
		 * NO NEW SEAM WAS NEEDED IN THE ORACLE for this. FOracleProblem is a plain struct of
		 * public arrays and FOracleJoint already carries its own FConnectionStrength row —
		 * the bridge and the solve are separate calls by construction, so a per-joint
		 * strength override is expressible in the test that wants it. Nothing in
		 * RigidBlockOracle.h/.cpp changed to make these rows possible, which is the property
		 * that keeps the oracle an independently derived second opinion.
		 *
		 * THE RETURN VALUE IS THE POINT, not a convenience. A selector that matched nothing
		 * would leave lambda* exactly where it was, and "lambda* did not move" is precisely
		 * the answer one of these experiments can report — so an override that silently
		 * touched zero joints is INDISTINGUISHABLE from a real negative result unless the
		 * count is pinned. OverriddenJoints below is that pin.
		 */
		TFunction<int32(FOracleProblem&)> AdjustProblem;

		/** How many joints the override above must have changed. INDEX_NONE: unasserted. */
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

			/*
			 * THE PRICING EXPERIMENT'S ONE EDIT, between the bridge and the solve. It is
			 * deliberately AFTER the block and joint counts are taken, so those two still
			 * describe the FIXTURE — an override changes strengths, never geometry, and a
			 * rung-size pin has to stay a statement about the wall.
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
		 * AND A PRICING EXPERIMENT MUST HAVE PRICED SOMETHING. Asserted here, ahead of both
		 * refusal branches and the relation, because it is a statement about the PROBLEM
		 * that was posed rather than about the answer — a probe whose selector matched
		 * nothing poses the control problem, reproduces the control's lambda* inside every
		 * window, and reports "the strength I varied does not bind" without ever having
		 * varied it. This is the only assertion that can tell those two apart.
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

		/*
		 * The same canary shape for the OTHER refusal: the solver never reaches an answer
		 * to reject. Kept a separate branch rather than a widened substring test, because
		 * the two refusals mean opposite things about how far the solve got, and a row
		 * pinned to one of them silently passing on the other would lose the distinction.
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

	/* --- The case-21 opening family, parameterised for the two mechanism ladders. ----- */

	/*
	 * There is deliberately no OpeningCells constant here: the MATCHED-SPAN experiment
	 * promoted the opening's width to a rung parameter (see the 17-cell rungs in
	 * OpeningMechanismLadders for why one number of cells cannot ask the question), so
	 * every rung passes its own count and case 21's 18 appears at its call sites.
	 */

	/** Courses the opening is cut through. Held at case 21's three. */
	constexpr int32 LadderOpeningCourses = 3;

	/** Courses of masonry over the opening. Held at the user's two-course minimum. */
	constexpr int32 LadderCoverCourses = 2;

	/**
	 * THE Z OF THE COVER'S UNDERSIDE for a rung with this much masonry below the opening —
	 * the one horizontal line the strength probes below need, and the reason they need no
	 * X test at all.
	 *
	 * BELOW THIS LINE EVERY BED JOINT IN THE WALL IS A JAMB BED JOINT. The opening is a
	 * hole, so between the reveals there is nothing to bed: the course-0-to-course-1 joints
	 * exist only under the jamb bricks, the joints inside the opening's courses likewise,
	 * and the bearing joints that carry the cover sit over the jambs by definition. Above
	 * it the cover is continuous and its bed joint runs the whole width. So "bed joint with
	 * a centre below the cover" IS "jamb bed joint", derived from the geometry rather than
	 * from a hand-placed X window that would have to be re-derived for every jamb width.
	 *
	 * The line has half a centimetre of clearance below it (the bearing joint's mid-plane
	 * sits at the middle of its 1 cm bed) and seven above (the next bed joint up is a whole
	 * course away), so it is nowhere near anything it has to separate.
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
	 * EXPERIMENT ONE: take the cohesion out of the jamb's shear chain and leave everything
	 * else — including its friction, its tension and every other joint in the wall — exactly
	 * as the bridge built it.
	 *
	 * WHAT IT PRICES. The surviving case-21 hypothesis (CURRENT_STATE) is that the LP
	 * confines its thrust to the 15 cm cover and bleeds it into the jamb's bed joints as
	 * Coulomb shear, all the way down to the grounded course — which is the one shape that
	 * survives both measured ladders (flat in the wall below the opening because a stack of
	 * bed joints in series is governed by their AREA and not by how many of them there are;
	 * sensitive to jamb width because that area is the jamb's width). Cohesion is what
	 * dominates that capacity here: 0.2 MPa against mu x precompression of roughly
	 * 0.6 x 0.018 MPa, i.e. roughly 18x. So if the chain binds, taking the cohesion away
	 * takes the mechanism away.
	 *
	 * @return how many joints were rewritten — pinned by the row, never trusted.
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
	 * EXPERIMENT ONE'S CHEAPER SECOND PROBE: scale the tensile strength of the COVER'S HEAD
	 * JOINTS and nothing else.
	 *
	 * WHAT IT PRICES. A cover spanning its opening as a deep bending panel carries that
	 * bending as horizontal tension, and horizontal tension in masonry crosses the HEAD
	 * joints — so the panel's flexural capacity is linear in this one number and lambda*
	 * must TRACK it if the cover's own bond is what stands the wall. It leaves head-joint
	 * COMPRESSION untouched, which is what an arch or a jammed flat arch would use, so the
	 * two mechanisms are separated by the same knob.
	 *
	 * THE PREDICTION FROM WHAT IS ALREADY KNOWN is that it will not track: 5.511 is 66x
	 * what a tension-bond panel can hold at these strengths, so the panel's tension is very
	 * unlikely to be the binding capacity. That makes this a probe whose expected answer is
	 * negative, and the joint-count pin on the row is what stops a negative answer being
	 * confused with an override that matched nothing.
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
	 * ONE RUNG OF EITHER LADDER: case 21's wall with exactly one thing moved.
	 *
	 * `CoursesBelow` is the masonry UNDER the opening — the rise ladder's variable, and
	 * case 21's own value is 1 (the grounded sill course alone). `JambCells` is the
	 * masonry either side of it — the abutment ladder's variable, case 21's value 2.
	 * `OpeningCells` is the cut's width in whole bricks, case 21's value 18 and the
	 * MATCHED-SPAN experiment's variable. Cover and opening height are constants above, so
	 * a rung differs from case 21 in one dimension and no other. `CoverTailCells` is the
	 * disambiguating rung's fourth variable and INDEX_NONE (or anything not smaller than
	 * the jamb) on every other rung — see the trim block below for what it is for.
	 *
	 * THE CLEAR REVEAL THE COVER BEARS ON IS NOT `OpeningCells` x the cell pitch, and the
	 * matched-span rungs turn on the difference. An even course loses `OpeningCells` whole
	 * bricks and bears on (OpeningCells + 1) x 22.5 - 21.5 cm; an odd course loses one
	 * fewer and bears a FULL CELL — 22.5 cm — narrower. The course that matters is the TOP one of the
	 * opening, because that is what the cover sits on — so an 18-cell cut whose top opening
	 * course is odd and a 17-cell cut whose top opening course is even present the cover
	 * with the SAME 383.50 cm, while their cover courses are laid on opposite bond parities.
	 *
	 * IT IS PRODUCTION'S OWN BRICKLAYER (`DestructionWallCases::Build`) DRIVEN BY THIS
	 * FILE'S OWN CONSTANTS, and the s=1/j=2 rung IS case 21 — the ladder test asserts that
	 * rung and the `Scenario("wall-21")` row are THE SAME NUMBER, so the geometry
	 * arithmetic below is checked against the shipped catalogue row rather than trusted.
	 * That is what makes a hand-parameterised builder acceptable for a MEASUREMENT beside
	 * a catalogue that lays every verdict row through the scenario table.
	 *
	 * THE CUT, in the (course, cell) vocabulary the region rule reads (a piece is in when
	 * its course is within the inclusive range and its cell is STRICTLY between the
	 * bounds): courses s..s+2, cells j..j+OpeningCells-1 of the even courses (j..j+17 at
	 * case 21's 18 cells). At 18 cells, s=1, j=2 that is case 21's own
	 * `{ 1, 3, 1.75, 19.25 }`, transcribed nowhere and derived here.
	 *
	 * ONE PARITY CAVEAT, WORTH KNOWING BEFORE READING THE RISE LADDER. Running bond makes
	 * the reveal shape depend on the parity of the opening's courses: an EVEN course loses
	 * 18 whole bricks to this cut and an ODD one 17, so an opening starting at an odd
	 * course (s = 1, 3) takes 17+18+17 = 52 bricks and one starting at an even course
	 * (s = 2, 4) takes 18+17+18 = 53. The rungs therefore alternate between two toothing
	 * patterns, one brick apart in a 52-brick cut. It is a real confound and it is small;
	 * a ladder whose four rungs move by a factor is not being driven by it, and one whose
	 * rungs alternate would be.
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
		 * THE COVER TAIL, WHICH ONLY THE DISAMBIGUATING RUNG USES. Widening the jamb
		 * widens two things at once — the bearing under the cover AND the length of cover
		 * built in over it — so the abutment ladder on its own cannot say which of them
		 * bought the margin. Trimming the cover back to a two-cell tail over a three-cell
		 * jamb separates them: the reaction keeps the wider bearing, the panel loses the
		 * longer tail.
		 *
		 * THE TRIM IS SYMMETRIC AND IT IS NOT EXACTLY ONE CELL. Each end loses one whole
		 * brick from the even cover course and a whole-plus-half-bat from the odd one —
		 * 1.0 and 1.5 cells respectively — because running bond closes its odd courses
		 * with half bats and no cut in this vocabulary can take half of one. Both ends
		 * lose the same thing, so the fixture stays symmetric; the claim it supports is
		 * "the cover overhangs the jamb by about two cells instead of three", not a
		 * millimetre-exact reproduction of case 21's tail.
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

		/*
		 * The parity note above says a rung's cut is 52 or 53 bricks; anything else means
		 * the region arithmetic and the bricklayer have stopped agreeing, which would make
		 * every rung above a measurement of a different fixture than the one named.
		 */
		const int32 WholeCourses = LadderOpeningCourses / 2 + 1;
		const int32 HalfCourses = LadderOpeningCourses - WholeCourses;

		const int32 EvenFirst = OpeningCells * WholeCourses
			+ (OpeningCells - 1) * HalfCourses;
		const int32 OddFirst = OpeningCells * HalfCourses
			+ (OpeningCells - 1) * WholeCourses;

		int32 Wanted = (CoursesBelow % 2 == 0) ? EvenFirst : OddFirst;

		/*
		 * And the trim, which the two cover courses split one even and one odd whatever
		 * the rung's parity: TrimCells bricks off the even course and TrimCells + 1 off
		 * the odd one (the half bat), at each of the two ends.
		 */
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
	 * A LADDER RELATION, PINNED AS A RATIO OF TWO LAMBDA*, with both competing predictions
	 * printed beside the measurement on every run and quoted in the failure.
	 *
	 * WHY A RATIO AND NOT TWO WINDOWS. Two windows say where each rung sits; the ladder's
	 * whole content is how the rungs MOVE relative to one another, and that is a claim
	 * neither window makes. A solver change that shifted every rung by the same factor
	 * would keep the relation and break both windows — which is exactly the split wanted,
	 * because the relation is the physics finding and the windows are the arithmetic.
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
	 * lambda* for a wall nobody asked for. MEASURED TO BE THE ONLY NET THAT CATCHES IT:
	 * under the recorded rung-flip mutation (s=3 secretly built as s=1) the lambda window
	 * AND the depth-ratio pin both PASSED — the two lambdas sit 1e-6 apart inside a 2e-5
	 * window, and the ratio reads exactly the 1.0 the pin wants — so window, ratio and
	 * size pins work as a set, never alone (TRAPS records the lesson).
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
 * free-end ladder alone is ~12 s, which is what makes it the cheap place to prove a
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
 * FreeEndHeightLadder 11.8 s both times (2026-08-13, the case-21 mechanism ladders
 * joining as a third test: 12.2 / 20.8 / 499.6 s = 532.6 s whole-group, so the ladders
 * cost 4% and WallsAndLadders' own figure varied 5% across machines-under-load — quote
 * the range, not the last number; the same day's pricing-experiment slice added the
 * matched-span pair to the ladders and a FOURTH test, OpeningStrengthProbes, for
 * 11.7 / 22.9 / 4.4 / 470.9 = 509.9 s whole-group — 6.5 s of new solving, and a
 * whole-group figure 4% BELOW the previous one because WallsAndLadders varies by more
 * than this slice costs) — against a ~30 s budget for the ENTIRE default
 * suite. (The pre-pricing figures were ~437 s total, 436.7 / 434.9 / 438.1 over three
 * runs, ~415 s of it WallsAndLadders and ~23 s the ladder; the ladder halved while the
 * walls grew, which is the per-fixture trade the WallsAndLadders header works out.)
 * Every run so far has returned
 * bit-identical lambda* and worst readings on every row — thirty-five across the group's
 * four tests as of 2026-08-13 — which is the determinism contract holding across the
 * widest problem set it has ever been asked about. Keep this
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

	/*
	 * THE FIRST ROW IN THIS FILE WHERE THE ORACLE IS THE OUTLIER, AND THE FIRST WHOSE
	 * ACCEPTANCE VERDICT IS `Collapse`. Every previous disagreement here has been the LP
	 * against a production defect, and four catalogue rulings have moved TOWARD the LP.
	 * Case 21 goes the other way: hand statics and production both condemn the wall and
	 * the LP stands it at 5.51x. The row is pinned as a measurement OF THE DISAGREEMENT
	 * ITSELF — not as a verdict about masonry, and NOT as a diagnosis of the LP, because
	 * what the LP is pricing here has not been established.
	 *
	 * WHAT IS ESTABLISHED. 5.511 is 66x the strength of the mechanism the acceptance
	 * verdict condemns: a 15 cm panel on characteristic flexural bond fails at
	 * lambda = 0.10 / 1.2014 = 0.0832. So something other than the bond carries this wall
	 * in the LP's world. WHAT it is has NOT been identified.
	 *
	 * THE TWO MEASURED LADDERS, both run 2026-08-13 (mean span L = 394.75 + (cells - 22)
	 * x 22.5 cm, the opening always four cells narrower than the wall):
	 *
	 *   cover at 22 cells   2 crs 5.511   4 crs 7.683   6 crs 9.183   8 crs REFUSED
	 *   span at 2 courses   18 c 10.408   22 c 5.511   27 c 3.102   32 c 1.985
	 *                       40 c  1.143   45 c 0.863
	 *
	 * THE HYPOTHESIS THAT WAS PRICED, AND WHY IT IS STILL ONLY A HYPOTHESIS. A
	 * full-wall-height arch whose thrust dives through the jambs into the grounded course
	 * prices at 22 cells to H = W L / (8 r) = 936 x 394.75 / 240 = 1,539 N against jamb
	 * bed joints affording (0.2 + 0.6 x 0.018) MPa over ~400 cm^2 = 8,440 N, i.e. 5.48
	 * against the LP's 5.511. ONE RUNG. Walked along the cover ladder it was fitted on,
	 * the same arithmetic gives 5.48 / 4.11 / 3.65 — falling by a third where the
	 * measurement RISES by two thirds, so it is wrong in DIRECTION and out by 1.9x at the
	 * middle rung and 2.5x at the top. Nor do the two shape observations discriminate:
	 * lambda* rising while
	 * sigma falls is what ANY depth-improved mechanism does (a load factor and a stress
	 * move oppositely by construction), and lambda* x L^2 roughly constant is as much a
	 * beam's signature as an arch's — and it is only roughly constant, drifting
	 * 9.67e5 -> 7.18e5, TWENTY-SIX PER CENT, across the full ladder (an earlier draft
	 * quoted 7.62e5, which is the 32-cell rung and stops two short of the end).
	 *
	 * SO WHAT THIS ROW MEASURES IS AN OPEN DISAGREEMENT. It MAY be a third charity beside
	 * the two the file header names (the plastic limit, and full redistribution): the
	 * oracle's ground is immovable, so anything able to tie itself through the foundation
	 * gets that restraint free, and a real footing supplies it only by moving. It may
	 * equally be a real load path the downward-routing solver cannot see. That is a USER
	 * call, it has not been made, and CURRENT_STATE carries it along with the two ladders
	 * that would discriminate — vary the courses BELOW the opening (an arch from the
	 * ground gains rise, a cover-carried mechanism does not) and widen the jambs (an
	 * abutment reaction roughly doubles, a cover-carried one barely moves). Growing this
	 * fixture would not settle the VERDICT either way: lambda* crosses 1.0 only between
	 * 40 and 45 cells, an 8-to-9 METRE opening under two courses of brick, and 0.863 x 6
	 * for the characteristic-to-mean basis stands again — so the acceptance row is sized
	 * on the mean-strength ceiling instead and the disagreement is left standing.
	 *
	 * LAID THROUGH `Scenario(...)` LIKE EVERY OTHER WALL ROW HERE since the wall-21 level
	 * landed on 2026-08-13. It was briefly a hand-rolled `BuildOpeningWall` because the
	 * scenario row did not exist yet; that builder is deleted and the window below is
	 * unchanged across the swap, which is the acceptance sync test's brick-for-brick claim
	 * showing up as a number.
	 */
	Rows.Add({ TEXT("wall-21 eighteen-cell opening, two courses over"),
		TEXT("the COVER counter-case, and the first row here where the LP is the outlier: ")
		TEXT("two courses (15 cm) over a 4.06 m opening is 1.2014 MPa of deep-beam bending, ")
		TEXT("1.50x the 0.8 MPa top of the mean bracket and 12.0x characteristic f_xk1, and ")
		TEXT("production drops the whole cover (45 pieces, 0 stranded) in THREE cascade ")
		TEXT("passes at a worst pre-cascade reading of 3.8429 — a strength verdict, not the ")
		TEXT("zero-pass unroutability of wall-10 and wall-19. The LP stands it at 5.51, which ")
		TEXT("is 66x what the bond can hold, by a mechanism that has NOT been identified — ")
		TEXT("see the block above for the ladder that refuted the first attribution and for ")
		TEXT("the two ladders that would settle it"),
		Scenario(TEXT("wall-21")),
		ERelation::OracleStandsProductionFalls, 5.5109, 5.5111, 45, 0 });

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

/**
 * THE CASE-21 MECHANISM LADDERS — the two discriminating experiments DESIGN §8's
 * 2026-08-13 entry specified and left unrun, and the gate on evolution step 4.
 *
 * THE QUESTION THEY EXIST TO ANSWER. The LP stands acceptance case 21 — two courses of
 * cover over a 4.06 m opening — at lambda* = 5.511, where hand statics read 1.2014 MPa of
 * deep-beam bending (1.50x even the mean-basis ceiling) and production drops the whole
 * cover in three cascade passes. 5.511 is SIXTY-SIX TIMES what a tension-bond flexural
 * panel can hold (0.10 / 1.2014 = 0.0832), so the LP is pricing something else, and WHAT
 * has not been identified. The first attribution — an arch springing from the immovable
 * grounded course, thrust taken by the jamb bed joints — was refuted in review against its
 * own cover ladder: it predicts lambda* FALLING by a third as cover grows 2 -> 6 courses
 * where the measurement RISES by two thirds. These two ladders vary the two things that
 * attribution turns on, one at a time.
 *
 *   THE RISE LADDER varies the courses BELOW the opening, s = 1..4, holding the span at
 *   22 cells and the cover at two courses. An arch that springs from the grounded course
 *   gains rise for every course of wall under the hole (r = 30.0, 37.5, 45.0, 52.5 cm on
 *   the priced hypothesis's own arithmetic, H = W L / (8 r) against a near-flat jamb
 *   capacity), so it predicts lambda* = 5.48 / 6.85 / 8.22 / 9.59 — rising 1.75x across
 *   the ladder. Anything carried by the cover alone predicts FLAT: neither the panel's
 *   demand nor its capacity knows how much masonry is under the hole.
 *
 *   THE ABUTMENT LADDER widens the jambs from 2 cells to 4 with everything else held —
 *   the same 18-cell cut span, the same six courses, the same two of cover. A mechanism
 *   that reacts into the base of the jamb doubles its bearing and its weight, so it
 *   predicts roughly 2x; a cover-carried one predicts FLAT again.
 *
 * ====================================================================================
 * WHAT THEY MEASURED, 2026-08-13, AND WHAT IT DOES AND DOES NOT SETTLE
 * ====================================================================================
 *
 *     rung                       blocks  joints  lambda*              production
 *     wall-21 (the catalogue)      83     133    5.5110095421575718   45 fall, 3 passes
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
 * THE LAST TWO RUNGS ARE THE MATCHED-SPAN EXPERIMENT, ADDED 2026-08-13 to settle what the
 * ~13% "parity" step in the rise ladder actually is. It is the BEARING SPAN: a 17-cell cut
 * starting on an even course presents the cover with case 21's own 383.50 cm reveal while
 * laying the cover on the opposite bond parity, and it reads 0.98968 of case 21 where the
 * unmatched parity step reads 0.873. Production agrees to the last bit (both rungs' worst
 * joint is 3.842883954008586). The block below the ratio pins works it through.
 *
 * THE RISE LADDER REFUTES THE ARCH-FROM-THE-GROUNDED-COURSE HYPOTHESIS OUTRIGHT. The four
 * rungs are not one series: s=1 and s=3 start the opening on an ODD course and s=2 and
 * s=4 on an EVEN one, which toothes the reveal differently, and that parity is the ONLY
 * thing in the ladder that moves lambda* at all. Held at one parity, DEPTH DOES NOTHING:
 *
 *     s=3 / s=1 = 0.999999813    against 1.50 predicted (rise 45.0 cm against 30.0)
 *     s=4 / s=2 = 0.999997152    against 1.40 predicted (rise 52.5 cm against 37.5)
 *
 * Two extra courses of masonry under the opening — 22.5 cm of springing depth, 1.5x the
 * rise the withdrawn attribution was fitted at — move the answer by three parts in ten
 * million. Production agrees to the last bit for its own reason (its worst joint is in the
 * cover and the cover has not changed), which is pinned below so the finding is not the
 * LP's alone. Whatever the LP is pricing, IT DOES NOT KNOW THE WALL UNDER THE OPENING IS
 * THERE, and no mechanism whose capacity is set by the rise can survive that.
 *
 * THE ABUTMENT LADDER REFUTES THE OTHER HALF — a mechanism carried between the reveals.
 * One full cell more jamb either side is worth 34%: j=3 / j=2 = 1.33895751, where flat was
 * the cover-carried prediction and 1.50 the bearing-proportional one. It sits between the
 * two and nearer the second. THE 2x TEST AT j=4 NOW EXISTS AND SAYS THE SAME THING: the
 * oracle refused that rung in phase 2 at 107 blocks — having answered the 149-block s=4
 * rung, which is why the refusal was always a solver finding and not a scale one — and
 * when the ratio test gained its relative pivot floor on 2026-08-13 the canary flipped
 * and the rung answered (the no-entry seam is unreached with the floor in place, and
 * deliberately NOT repaired — see the defect test's header). j=4 / j=2 = 1.698 against 2.00 bearing-proportional and 1.00
 * flat: between the two, nearer the first, and DECELERATING (the first extra two cells of
 * jamb bought 1.339, the second 1.268). The other four rungs answered bit-identically
 * across that repair, so the ladder is one measurement throughout and not two eras of it.
 *
 * AND THE THIRD RUNG SAYS WHY THE SECOND LADDER ALONE COULD NOT HAVE IDENTIFIED ANYTHING.
 * Widening a jamb widens the bearing under the cover and lengthens the cover built in over
 * it, both at once; those are two mechanisms with one prediction. Trimming the cover back
 * to a two-cell tail over the three-cell jamb gives back a little over half the gain
 * (1.339 -> 1.141), so BOTH are live and neither is the mechanism by itself.
 *
 * SO THE MECHANISM IS STILL NOT IDENTIFIED, and this header does not name one. What the
 * ladders bought is a smaller search space: any candidate must be FLAT IN THE WALL BELOW
 * THE OPENING to seven figures and SENSITIVE TO BOTH the jamb's bearing and the cover's
 * built-in tail. The ~13% "parity" step is NOT a third criterion, and since 2026-08-13
 * that is a MEASUREMENT rather than a suspicion: the two parities bear the cover on
 * DIFFERENT CLEAR REVEALS (383.50 vs 406.00 cm — a full cell of span), and the
 * MATCHED-SPAN PAIR added at the bottom of this test — a 17-cell cut whose even bearing
 * course presents case 21's own 383.50 cm — collapses the step from 0.873 to 0.98968
 * while flipping the cover's bond parity. Roughly eleven twelfths of it is the span;
 * the 0.081 residue is an UPPER BOUND on the parity term (the matched-span block below
 * says why). And NOTHING HERE SAYS THE PATH DOES
 * NOT REACH THE GROUND — flat-in-depth refutes pricing BY THE RISE, not every route
 * through the jamb (see the depth block below); the deleted active-set probe measured
 * the path reaching the grounded course through every jamb bed joint, priced by AREA.
 *
 * WHAT IS PINNED IS THE MEASUREMENT, NEVER THE HYPOTHESIS. Each rung carries its own
 * lambda* window and production's drop count, and each ladder carries a RATIO pin — the
 * relation is the finding, and a window around one rung cannot state it. The predictions
 * are printed beside every ratio so a reader sees which one the number refutes; no
 * assertion anywhere asserts a mechanism, because none has been identified.
 *
 * THE CONTROL IS THE FIRST ROW AND IT IS NOT DECORATION. `Scenario("wall-21")` and the
 * hand-parameterised s=1/j=2 rung are asserted to be THE SAME NUMBER at the file's
 * same-number tolerance, and they measured a relative difference of ZERO — same lambda*,
 * same 83 blocks, same 133 joints, same 2,754 pivots. That is what licenses a local
 * builder in a file where every verdict row is laid through the scenario catalogue: if the
 * parameterisation drifted from the shipped fixture by one brick, the control fails and
 * every rung above it is void.
 *
 * WINDOWS ARE +/-2e-5 RELATIVE, the file's pivot-path-honest basis (see the
 * PARTIAL-PRICING RE-PIN note in WallsAndLadders); ratio windows are wider still at ~1e-4,
 * because a ratio carries both readings' spread. Every window here is centred on ONE
 * certified reading, no second pivot path having been measured on these fixtures — EXCEPT
 * the promoted j=4 rung, which is centred on the midpoint of TWO certified readings on the
 * re-pin basis this file already uses: 9.3587256137345118 was measured with the solver's
 * Bland fallback stood aside during the 2026-08-13 diagnosis and 9.3587305816217992 by the
 * repaired solver, 5.3e-7 relative apart, which is a fortieth of the window either way.
 *
 * COST: ten solves, ~28 s in total, the largest rung 149 blocks / 6.8 s. It was ~23 s
 * while the j=4 rung refused after 671 pivots; answering costs it 5,552 and 4.8 s.
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowOpeningLaddersTest,
	"OracleSlowSweep.RigidBlock.OpeningMechanismLadders",
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

	Rows.Add({ TEXT("wall-21 through the scenario catalogue"),
		TEXT("the equivalence control: the shipped case-21 fixture, laid the way every ")
		TEXT("verdict row in this file is laid, so the hand-parameterised rung below can be ")
		TEXT("held against it as the same number"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("wall-21"), Out, Why);
		},
		ERelation::OracleStandsProductionFalls, 5.5108993, 5.5111198, 45, 0 });

	Rows.Add({ TEXT("rise s=1 / abutment j=2 (case 21 itself)"),
		TEXT("both ladders' bottom rung and the equivalence control's partner — the same ")
		TEXT("83 blocks, 133 joints and 2,754 pivots as the catalogue row above, which is ")
		TEXT("what the same-number pin below turns into an assertion"),
		Rung(18, 1, 2, INDEX_NONE),
		ERelation::OracleStandsProductionFalls, 5.5108993, 5.5111198, 45, 0 });

	Rows.Add({ TEXT("rise s=2"),
		TEXT("one more course under the opening: an arch from the ground gains 7.5 cm of ")
		TEXT("rise, a cover-carried mechanism gains nothing. It gains nothing — what moves ")
		TEXT("here is the REVEAL PARITY (an even-first opening), and the depth pins below ")
		TEXT("compare s=3 against s=1 and s=4 against s=2 to keep the two apart"),
		Rung(18, 2, 2, INDEX_NONE),
		ERelation::OracleStandsProductionFalls, 4.8118201, 4.8120126, 45, 0 });

	Rows.Add({ TEXT("rise s=3"),
		TEXT("two more courses under the opening, back on case 21's odd-first parity: the ")
		TEXT("same lambda* to 1.9e-7 relative"),
		Rung(18, 3, 2, INDEX_NONE),
		ERelation::OracleStandsProductionFalls, 5.5108983, 5.5111187, 45, 0 });

	Rows.Add({ TEXT("rise s=4"),
		TEXT("three more courses under the opening: the springing is 22.5 cm deeper than ")
		TEXT("case 21's, which is 1.75x the rise the priced arch was fitted at, and lambda* ")
		TEXT("reads its parity partner s=2 to 2.8e-6 relative"),
		Rung(18, 4, 2, INDEX_NONE),
		ERelation::OracleStandsProductionFalls, 4.8118064, 4.8119989, 45, 0 });

	Rows.Add({ TEXT("abutment j=3"),
		TEXT("half again the jamb, same cut span, same wall height, same reveal parity — ")
		TEXT("and lambda* moves 1.34x, which is the one thing either ladder found that ")
		TEXT("does move it. Production drops 49 rather than 45 because the cover it drops ")
		TEXT("is two cells wider, not because it fares worse"),
		Rung(18, 1, 3, INDEX_NONE),
		ERelation::OracleStandsProductionFalls, 7.3788600, 7.3791552, 49, 0 });

	Rows.Add({ TEXT("abutment j=4"),
		TEXT("twice the jamb: twice the bearing and twice the weight over it, with the ")
		TEXT("opening, the cover and the courses below all unmoved. This rung REFUSED in ")
		TEXT("phase 2 until 2026-08-13 and was pinned as a canary; the ratio test gained ")
		TEXT("its relative pivot floor and the canary flipped, so the 2x test the ladder ")
		TEXT("was built for now has its measurement"),
		Rung(18, 1, 4, INDEX_NONE),
		ERelation::OracleStandsProductionFalls, 9.3585409, 9.3589153, 53, 0 });

	Rows.Add({ TEXT("abutment j=3, cover tail trimmed to two cells"),
		TEXT("THE DISAMBIGUATOR the abutment ladder needs: widening a jamb widens the ")
		TEXT("bearing AND lengthens the cover built in over it, and j=3 alone cannot say ")
		TEXT("which paid. This rung keeps the three-cell jamb and takes the extra cover ")
		TEXT("back off, and it lands BETWEEN the two — 6.290 against 5.511 and 7.379 — so ")
		TEXT("the answer is that both paid and neither is the mechanism on its own"),
		Rung(18, 1, 3, 2),
		ERelation::OracleStandsProductionFalls, 6.2894127, 6.2896643, 43, 0 });

	/*
	 * ================================================================================
	 * THE MATCHED-SPAN PAIR — the experiment that separates the two things the rise
	 * ladder's 13% "parity" step confounds.
	 * ================================================================================
	 *
	 * WHAT THE STEP CANNOT DISTINGUISH AS THE LADDER STANDS. s=2 reads 0.873 of s=1, and
	 * the two rungs differ in TWO ways at once: the cover bears on a clear reveal a full
	 * cell wider (406.00 against 383.50 cm — span-squared alone predicts 0.892), and the
	 * cover's own two courses are laid on the opposite bond parity (even-then-odd against
	 * odd-then-even, so the head joints that carry any panel tension line up differently).
	 * A single number cannot say which of those bought the 13%.
	 *
	 * WHAT THE 17-CELL CUT DOES ABOUT IT. Clear reveal is (cells + 1) x 22.5 - 21.5 cm on
	 * an even course and a full cell (22.5 cm) less on an odd one, so a SEVENTEEN-cell cut whose top
	 * opening course is EVEN (s = 2) presents the cover with 383.50 cm — the very span an
	 * EIGHTEEN-cell cut whose top opening course is ODD (s = 1) presents. Held against each
	 * other those two rungs have the span matched and the cover parity opposite, which is
	 * the one comparison that answers the question:
	 *
	 *     SPAN  hypothesis  lambda*(17, s=2) / lambda*(18, s=1) = 1.00 — parity buys nothing
	 *     PARITY hypothesis lambda*(17, s=2) / lambda*(18, s=1) = 0.873 — the step survives
	 *
	 * THE FOURTH RUNG (17 cells at s=1, a 361.00 cm reveal on the odd parity) is what makes
	 * it a 2x2 rather than one comparison: it is a pure SPAN step at fixed parity against
	 * the 18-cell s=1 rung, so it prices span on its own — span-squared predicts 1.128 —
	 * and without it a matched-span ratio of 1.00 could not be told from a ladder that has
	 * simply stopped responding to anything.
	 *
	 * BOTH RUNGS ARE 21 CELLS WIDE, so their block counts (80 and 100) differ from every
	 * 22-cell rung above and from each other; the size pins below are what refuse a rung
	 * that quietly built its partner's wall (TRAPS: window and ratio pins both pass under
	 * the recorded rung-flip; only the size pin caught it).
	 */
	Rows.Add({ TEXT("matched span: 17 cells, s=1"),
		TEXT("the pure SPAN step at fixed odd-first parity — one cell of opening off case ")
		TEXT("21, so the cover bears on 361.00 cm instead of 383.50 with the toothing and ")
		TEXT("the cover's bond parity unchanged. Span-squared predicted 1.128x case 21 and ")
		TEXT("it measured 1.156, an exponent of 2.391 rather than 2 — the 18-cell parity ")
		TEXT("step independently gives 2.378, two-figure agreement"),
		Rung(17, 1, 2, INDEX_NONE),
		ERelation::OracleStandsProductionFalls, 6.3679868, 6.3682416, 43, 0 });

	Rows.Add({ TEXT("matched span: 17 cells, s=2"),
		TEXT("THE MATCHED-SPAN RUNG: a 17-cell cut starting on an even course bears its ")
		TEXT("cover on the SAME 383.50 cm clear reveal as case 21's 18-cell odd-first one, ")
		TEXT("with the cover's two courses on the opposite bond parity. The span hypothesis ")
		TEXT("predicted 1.00 against case 21 and the cover-bond-parity hypothesis 0.873; it ")
		TEXT("measured 0.990, so the 13% step IS the bearing span. Production says the same ")
		TEXT("thing independently — its worst reading here is case 21's to the last bit"),
		Rung(17, 2, 2, INDEX_NONE),
		ERelation::OracleStandsProductionFalls, 5.4540000, 5.4542182, 43, 0 });

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
	 * ================================================================================
	 * THE RISE LADDER'S FINDING, IN THE TWO PINS THAT SEPARATE ITS TWO EFFECTS.
	 * ================================================================================
	 *
	 * The four rungs do not read as one series, and the reason is the parity note in
	 * BuildOpeningLadderWall: s=1 and s=3 start their opening on an ODD course, s=2 and
	 * s=4 on an even one, and the reveal is toothed differently in the two cases. So the
	 * ladder is read as two pins rather than four rungs in a line:
	 *
	 *   DEPTH, at fixed parity   s=3 / s=1 = 0.99999981   s=4 / s=2 = 0.99999715
	 *   PARITY, at fixed depth   s=2 / s=1 = 0.87314607   s=4 / s=3 = 0.87314375
	 *
	 * WHAT THAT REFUTES, and it is the reason these ladders were specified. An arch
	 * springing from the immovable grounded course gains rise for every course of wall
	 * under the hole: r = 30.0 -> 52.5 cm across the ladder, H = W L / (8 r) falling in
	 * proportion, so it predicts 1.25 and 1.75 where the depth pins measure 1.000000 to
	 * SEVEN significant figures. Two more courses of masonry under the opening — 22.5 cm
	 * of extra springing depth, 1.5x the rise the withdrawn attribution was fitted at —
	 * move lambda* by less than three parts in ten million. The mechanism does not know
	 * the wall below the opening is there.
	 *
	 * AND WHAT IT DOES NOT SETTLE. Flat-in-depth refutes a mechanism priced BY THE RISE.
	 * It does not refute every path through the jamb: a horizontal reaction taken by the
	 * jamb's bed joints is governed by their area, which is the same however many of them
	 * are stacked in series, so that hypothesis predicts flat here too and has to be
	 * separated by the abutment ladder below.
	 *
	 * The parity pins are not decoration either: they are the only movement in the whole
	 * rise ladder, so they are what a "the rise ladder is flat" claim has to survive, and
	 * a solver change that flattened them would be a real change in how the LP reads a
	 * toothed reveal.
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
		*S2, *S1, 0.87309, 0.87320);

	/*
	 * ================================================================================
	 * AND WHAT THE PARITY STEP TURNED OUT TO BE: THE SPAN, MEASURED 2026-08-13.
	 * ================================================================================
	 *
	 * The two 17-cell rungs answer the question the parity pins above could only pose.
	 * Predictions were recorded before the run; both are printed beside their measurement:
	 *
	 *     matched span (17,s=2)/(18,s=1)   span 1.00   parity 0.873   MEASURED 0.98968
	 *     pure span    (17,s=1)/(18,s=1)   span^2 1.128               MEASURED 1.15553
	 *
	 * THE STEP COLLAPSES WHEN THE SPANS MATCH. Two rungs whose covers are laid on OPPOSITE
	 * bond parities — even-then-odd against odd-then-even, so their head joints line up
	 * differently over the hole — and whose bearing reveals are the same 383.50 cm read
	 * within 1.03% of each other, against the 12.7% the same parity flip is worth when the
	 * span moves with it. So the reveal span carries roughly eleven twelfths of the step
	 * ((1 - 0.98968) / (1 - 0.87315) = 0.081 of it survives the span match), and that
	 * residue is an UPPER BOUND on the cover's bond parity rather than its measurement —
	 * the matched pair also differs by one course of wall height and one cell of width,
	 * though the depth ladder's 2e-7 flatness prices the course term at nothing. The
	 * TRAPS entry that called the step "mostly a reveal-span effect" was right and can
	 * now be stated as a measurement.
	 *
	 * PRODUCTION SAYS IT INDEPENDENTLY AND MORE SHARPLY, which is why the reading pins
	 * below are part of this finding rather than bookkeeping: its worst joint on the
	 * matched-span rung is case 21's to the LAST BIT (3.842883954008586 both), while the
	 * 361.00 cm rung reads 0.8949 of it. A downward-routing solver with no equilibrium and
	 * a limit-analysis LP agree that this fixture family is priced by the clear reveal the
	 * cover bears on, and by nothing else the parity carries.
	 *
	 * THE SPAN EXPONENT IS 2.39, NOT 2, AND THE TWO STEPS AGREE ON IT TO TWO FIGURES.
	 * 1.15553 over 383.5/361 is L^-2.391; the 18-cell parity step's 0.873146 over
	 * 406/383.5 is L^-2.378. Two independent cell-steps agreeing to two figures, both
	 * steeper than the span-squared a plain bending panel gives — a real property of
	 * whatever the LP is pricing, recorded as a measurement and not attributed to a
	 * mechanism here.
	 *
	 * The third ratio a 2x2 suggests — (17,s=2)/(17,s=1) = 0.85647 — is NOT pinned, and the
	 * omission is deliberate: it is exactly the quotient of the two pins below, so it can
	 * only fail when one of them already has, and a pin that cannot fail alone is a pin
	 * that dilutes the failure it appears in.
	 *
	 * PROVEN TO BITE by the file's recorded rung-flip, 2026-08-13: the matched-span rung
	 * built secretly as case 21 fires FOUR assertions — the lambda window, the drop count,
	 * the block count and the matched-span ratio, which reads exactly the 1.00 the span
	 * hypothesis predicts. ONE THING IT DOES NOT FIRE, and it is worth knowing: production's
	 * matched-span reading identity below PASSES under that flip, because production reads
	 * the two fixtures identically ON PURPOSE. The LP-side pins are what carry this finding;
	 * the production one corroborates it and cannot police it.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE MATCHED-SPAN PAIR: 17 cells at s=2 against case 21, both bearing 383.50 cm"),
		TEXT("the bearing-span hypothesis predicts 1.00 (parity buys nothing once the spans ")
		TEXT("match), the cover-bond-parity hypothesis predicts the 0.873 step survives"),
		*W17S2, *S1, 0.98958, 0.98978);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE PURE SPAN STEP at fixed odd-first parity: 361.00 cm against 383.50 cm"),
		TEXT("span-squared predicts 1.128 and a ladder that had simply stopped responding ")
		TEXT("predicts 1.00; without this rung a matched-span 1.00 could not be told from ")
		TEXT("an insensitive one"),
		*W17S1, *S1, 1.15541, 1.15564);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE RISE LADDER's parity step, repeated two courses deeper (s=4/s=3)"),
		TEXT("the same reveal-width step, measured again at a different depth: if the ")
		TEXT("parity step were really a depth effect in disguise, these two would differ"),
		*S4, *S3, 0.87309, 0.87320);

	/*
	 * ================================================================================
	 * THE ABUTMENT LADDER'S FINDING, AND THE HALF OF IT THE ORACLE REFUSED TO ANSWER.
	 * ================================================================================
	 *
	 * j=3 reads 1.339 against j=2. That is NOT flat — 34% on a 50% wider jamb, where the
	 * rise ladder moved by parts in a million — so a mechanism confined to the cover
	 * between the reveals is refuted just as the rise-priced arch is. It is also short of
	 * the 1.50 a bearing-proportional reaction predicts, which is what the cohesion-plus-
	 * friction mix would do: cohesion scales with the area and friction with the weight,
	 * and only one of those doubles cleanly.
	 *
	 * j=4 IS THE 2.00 TEST, AND SINCE 2026-08-13 IT EXISTS. The rung refused in phase 2 at
	 * 107 blocks — pinned above as a canary while it did — and the refusal turned out to be
	 * the solver rather than the fixture: the ratio test was accepting pivots ~1e-16 of
	 * their own column's scale, and a column the ratio test could not use at all was being
	 * read as an unbounded ray on a problem the lambda-cap row bounds. The repair is the
	 * relative pivot floor in RigidBlockOracle.cpp — the no-entry seam is UNREACHED with
	 * the floor in place and deliberately not repaired (a set-aside arm was written,
	 * measured inert, and removed; the defect test's header carries the story) — and the
	 * canary flipped, which is what the discipline is for. The
	 * completed ladder reads 5.511 / 7.379 / 9.359 and j=4 / j=2 = 1.698 — SHORT of the
	 * 2.00 a bearing-proportional reaction predicts and far above the 1.00 a cover-carried
	 * mechanism predicts, the same between-the-two verdict the j=3 step already gave, now
	 * stated over a doubled jamb rather than a half-again one. Two cells of jamb bought
	 * 1.339 and the second two bought 1.268, so the gain is real and DECELERATING, which is
	 * what a cohesion-plus-friction mix does: the cohesion term scales with the bed area
	 * that widens, the friction term with a weight that only the jamb's own courses add.
	 *
	 * THE OTHER HALF OF THE FIX BELONGS HERE TOO, because it is the one number a reader
	 * will want and it is NOT pinned anywhere: the four answering rungs beside the repaired
	 * one — s=1..s=4, j=3, and the whole matched-span pair — came back BIT-IDENTICAL after
	 * the repair. A solver change that moved every reading would have invalidated every
	 * window in this file; this one moved exactly the readings that could not be taken.
	 *
	 * THE TRIMMED RUNG IS WHAT MAKES THE j=3 STEP MEAN SOMETHING. Widening the jamb
	 * widens the bearing under the cover and lengthens the cover built in over it at the
	 * same time; those are two different mechanisms with the same prediction, and the
	 * ladder as specified cannot separate them.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE ABUTMENT LADDER's step, three cells of jamb against two"),
		TEXT("a jamb-width-proportional reaction predicts ~1.50, a mechanism carried ")
		TEXT("between the reveals predicts 1.00"),
		*J3, *S1, 1.33885, 1.33906);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE ABUTMENT LADDER's 2x TEST: four cells of jamb against two"),
		TEXT("a bearing-proportional reaction predicts 2.00 and a mechanism carried ")
		TEXT("between the reveals predicts 1.00 — measured 1.698, between them and nearer ")
		TEXT("the first, which is the j=3 step's verdict restated over a doubled jamb"),
		*J4, *S1, 1.69805, 1.69833);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE DISAMBIGUATOR: the three-cell jamb with its cover tail trimmed back"),
		TEXT("if the jamb's bearing bought the 1.34, this stays near 1.34; if the longer ")
		TEXT("cover tail bought it, this falls back toward 1.00 — measured 1.141, which is ")
		TEXT("NEITHER, so both are live and this is the pin that says so"),
		*J3Trimmed, *S1, 1.14117, 1.14137);

	/*
	 * THE SAME MEASUREMENT SAID THE OTHER WAY ROUND, and it is worth both pins because
	 * they fail differently: this one holds the trimmed rung against the UNTRIMMED j=3, so
	 * it states how much of the wider jamb's gain the trim gives back — 0.852, i.e. the
	 * 0.339 of margin j=3 bought over j=2 falls to 0.141, so a little over half of it went
	 * with the cover tail and a little under half stayed with the bearing. ONE CAVEAT
	 * BELONGS WITH THAT SPLIT: trimming the tail also removes the cover's weight from over
	 * the jamb, and that weight is precompression on the very bed joints a jamb reaction
	 * would use, so the trim understates the bearing's share by however much friction
	 * contributes. Cohesion dominates that mix (0.2 MPa against 0.6 x ~0.018), so the
	 * understatement is small — but it is not zero, and the split is quoted as "comparable
	 * halves" rather than as a number for that reason.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE DISAMBIGUATOR against the untrimmed jamb (trimmed j=3 / j=3)"),
		TEXT("all-bearing predicts 1.00 (the trim would change nothing), all-tail predicts ")
		TEXT("0.747 (the whole gain given back)"),
		*J3Trimmed, *J3, 0.85228, 0.85243);

	/*
	 * PRODUCTION'S OWN READING, ON THE SAME TWO SPLITS — because a ladder that only the
	 * LP can see is a ladder about the LP. Production is INDIFFERENT TO DEPTH to the last
	 * bit (its worst joint is in the cover, and the cover does not change) and it sees
	 * the reveal parity exactly as the LP does. That agreement is what says the parity
	 * step is a property of the fixture rather than of the simplex.
	 */
	CheckReadingRatio(
		*this,
		TEXT("production's worst reading does not move with depth either (s=3 / s=1)"),
		S3->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("production sees the reveal parity the LP sees (s=2 / s=1)"),
		S2->WorstUtilisation, S1->WorstUtilisation, 1.11015, 1.11027);

	/*
	 * ================================================================================
	 * PRODUCTION IS BLIND TO THE JAMB, AND THAT IS THE ONE NET THE SIX LAMBDA PINS
	 * CANNOT CAST.
	 * ================================================================================
	 *
	 * Every pin above watches the LP. Production's side is watched by drop counts, which
	 * are counts of BRICKS and move for the trivial reason that a wider wall has a wider
	 * cover to drop (45 / 49 / 53 / 43 across these four rungs) — they say nothing about
	 * what production READ. And what it read is the striking half of the abutment ladder:
	 * where the LP moves 34% for one extra cell of jamb and refuses the next, production's
	 * worst joint does not move AT ALL, on any of the four rungs, in any digit.
	 *
	 * THAT IS A FINDING ABOUT THE ROUTER RATHER THAN A CURIOSITY. Production's worst
	 * reading here is in the cover, and its value is settled by the cover's own span and
	 * courses; the masonry the load is supposed to react into is not a term in it. So the
	 * two methods disagree about whether an abutment exists at all, which is exactly the
	 * disagreement DESIGN §7 step 4 is going to have to resolve — and until it does, this
	 * is the assertion that fires if the production side of it changes while every lambda
	 * pin in this file stays green.
	 *
	 * PINNED AT 1e-9 AS RATIOS, mirroring the depth pin above rather than pinning four
	 * absolutes: the claim is that the four readings are ONE number, which is a strictly
	 * stronger statement than four windows and the only one a same-sized drift on all four
	 * cannot satisfy. The trimmed rung is included deliberately — it takes six bricks of
	 * cover off, which changes what falls without changing what is read.
	 *
	 * PROVEN TO BITE, 2026-08-13, and the mutation was chosen to be one NOTHING ELSE HERE
	 * CATCHES: production's utilisation scaled by (1 + 1e-6 x NumPieces()), i.e. any change
	 * that makes a reading depend on how much masonry stands beside the opening. It fired
	 * exactly five assertions — these three, the depth pin above and the matched-span
	 * reading identity below — while every lambda* window, every ladder ratio, every block
	 * count and every drop count stayed green, which is the claim these pins exist to make.
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
	 * PRODUCTION'S HALF OF THE MATCHED-SPAN FINDING, AND IT IS THE SHARPEST NUMBER IN THIS
	 * TEST. Two rungs one brick different in width, one course different in height and
	 * on opposite cover-bond parities read THE SAME WORST JOINT to the last bit, because
	 * both bear on 383.50 cm — while the 361.00 cm rung, which differs from case 21 in the
	 * span ALONE, reads 0.8949 of it. Pinned as an identity plus a stay-apart ratio, the
	 * pair that a same-number pin always needs (TRAPS: an identity alone passes happily
	 * when two rows have quietly become one fixture).
	 */
	CheckReadingRatio(
		*this,
		TEXT("production prices the matched-span pair identically (17@s=2 / case 21)"),
		W17S2->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("and separates the span step it is matched against (17@s=1 / case 21)"),
		W17S1->WorstUtilisation, S1->WorstUtilisation, 0.89479, 0.89498);

	return true;
}

/**
 * THE CASE-21 PRICING EXPERIMENTS — the first probes that vary a STRENGTH rather than the
 * geometry, and the closing experiment CURRENT_STATE specified after the two mechanism
 * ladders refuted both of the candidate mechanisms they were built to test.
 *
 * WHY A DIFFERENT KIND OF ROW WAS NEEDED. Every ladder rung so far asks "what does lambda*
 * do when the WALL changes", and between them they have narrowed the search space to a
 * mechanism that is flat in the masonry below the opening to seven figures and sensitive to
 * both the jamb's bearing and the cover's built-in tail. That is as far as geometry can
 * take it: two candidates fit all three ladders — a thrust confined to the 15 cm cover and
 * bled into the jamb's bed joints as Coulomb shear down to the ground, and an arch springing
 * at the SILL whose rise is set by the opening's own height — and no rung distinguishes
 * them. Nor, it turned out, do these probes (both shapes deliver their thrust into the same
 * jamb bed joints — see WHAT THAT DOES AND DOES NOT NAME below). What a strength probe CAN
 * reach, and a geometry rung cannot, is which CAPACITY is binding — so these rows hold the
 * fixture exactly still and take one strength away at a time.
 *
 * HOW, AND WHAT IT COST TO BUILD. FOracleProblem is a plain struct whose joints each carry
 * their own FConnectionStrength, and the bridge and the solve are separate calls — so the
 * override is eleven lines in this file's own support namespace and NOTHING IN
 * RigidBlockOracle.h/.cpp CHANGED. That matters beyond convenience: the oracle's whole value
 * is being derived independently of production, and an experiment that had needed a new seam
 * cut into it would have been a change to the instrument in the middle of the measurement.
 *
 * PRODUCTION IS THE CONTROL AND IT IS ASSERTED, NOT ASSUMED. The override rewrites the LP's
 * problem only, so the cascade sees the untouched wall on every row; each probe's production
 * reading is pinned identical to the control's at 1e-9, which is what says a moved lambda*
 * is the strength that moved and not the fixture.
 *
 * THE PREDICTIONS, WRITTEN DOWN BEFORE THE FIRST RUN — this block is a record of what was
 * expected, kept beside what was measured, because a prediction recorded after the fact is
 * worth nothing:
 *
 *   PROBE 1, jamb bed cohesion to zero. If the jamb's shear chain is the binding capacity,
 *   lambda* COLLAPSES TOWARD 1 — cohesion is 0.2 MPa against mu x precompression of about
 *   0.6 x 0.018, roughly 18x, so removing it removes essentially the whole chain. If it
 *   barely moves, the chain is not what stands the wall. (AS FIRST WRITTEN this
 *   prediction went further — a collapse would rule the sill-springing arch out, a null
 *   result would leave it "the survivor". REFUTED IN REVIEW before the pin was trusted:
 *   a sill arch's abutment REACTION is horizontal thrust into these same jamb bed
 *   joints — "rides head-joint compression" describes its internal path, not where its
 *   reaction lands — so the probe prices the capacity BOTH shapes share and separates
 *   neither. The corrected reading is the one the conclusion below uses.)
 *
 *   PROBES 2 AND 3, cover head-joint tension halved and doubled. If the cover carries its
 *   own span as a bending panel, its capacity is linear in this number and the pair reads
 *   4.00x apart. EXPECTED TO BE FLAT: 5.511 is already 66x what a tension-bond panel holds
 *   at these strengths, so the panel's tension almost certainly is not binding — which makes
 *   the joint-count pin on each row load-bearing, since an override that matched nothing
 *   would produce the same flat answer for a reason that is not a finding.
 *
 * ====================================================================================
 * WHAT THEY MEASURED, 2026-08-13, AND IT IDENTIFIES THE BINDING CAPACITY.
 * ====================================================================================
 *
 *     row                            joints  lambda*              vs control  predicted
 *     control (as built)                 0   5.5110095421575718     1.00000     —
 *     jamb bed cohesion zeroed          36   0.78511681229249863    0.14246    ~1 or 1.00
 *     cover head tension x0.5           43   5.2459912546514023     0.95191     0.50 or 1.00
 *     cover head tension x2             43   6.0401281381456675     1.09601     2.00 or 1.00
 *
 * PROBE 1 DID NOT MERELY COLLAPSE lambda* TOWARD 1 — IT TOOK IT BELOW. 5.511 to 0.785, a
 * factor of 7.02, and past the line: with the jamb's bed cohesion gone the LP finds NO
 * admissible equilibrium at self-weight and the MODIFIED wall falls. Said carefully,
 * because this row is read before a ruling: hand statics and production reached their
 * collapse verdicts on the REAL wall, and the LP's verdict here is about a wall those
 * methods never judged — the row's AgreeFalls records two verdicts that agree, NOT a
 * three-method agreement on case 21 (no such agreement exists; that is the open call).
 * What it does establish is that the whole of the case-21 disagreement is bought by one
 * capacity: this row's pinned relation is AgreeFalls where every other case-21 row in
 * this file is ORACLE STANDS / PRODUCTION FALLS. It is the answer the two mechanism
 * ladders were run to set up.
 *
 * WHAT THAT DOES AND DOES NOT NAME. Cohesion in a BED joint is horizontal shear capacity —
 * vertical load crosses a bed joint as compression and does not touch it — so what has been
 * removed is precisely the jamb's ability to carry HORIZONTAL THRUST down to the ground.
 * The LP's mechanism therefore delivers thrust into the jamb's bed joints and prices it on
 * their cohesion, which is consistent with all three earlier ladders at once: flat in the
 * masonry below the opening (a stack of bed joints in series is governed by their area, not
 * by how many are stacked), sensitive to jamb width (that area is the jamb's), and
 * consistent with the deleted active-set probe's "every jamb bed joint active, priced by
 * AREA". IT STILL DOES NOT PICK BETWEEN the two surviving shapes of thrust line — a thrust
 * confined to the cover and one springing at the sill both deliver horizontal force into
 * these same joints — and this header does not pretend otherwise. What it settles is the
 * CAPACITY, which is the half of the question a geometry ladder cannot reach.
 *
 * PROBES 2 AND 3 CAME OUT AS PREDICTED, WHICH IS ALSO A RESULT. A FOUR-FOLD change in the
 * cover's head-joint tension moves lambda* by 15% (6.0401/5.2460 = 1.15138 against 4.00 for
 * a bond-governed panel and 1.00 for none), so the cover's own tensile bond is a minor
 * contributor and definitively not the mechanism — which is what 66x-the-bond said and this
 * measures directly. The 43-joint pin is what makes the near-flat answer trustworthy: an
 * override that had matched nothing would have produced a flatter one.
 *
 * BOTH PROBES ARE PROVEN TO BITE, AND THE TWO MUTATIONS FAIL DIFFERENTLY, which is the
 * evidence that the count pin and the ratio pins are not two spellings of one net:
 *
 *   SELECTOR NEUTERED (the cohesion probe's Z test made unsatisfiable) — FOUR failures:
 *   the joint count (36 wanted, 0 rewritten), the relation, the lambda window, and the
 *   ratio. lambda* comes back at 5.5110095421575718, BIT-IDENTICAL to the control, which
 *   is the proof that the override is the only difference between the two rows.
 *
 *   FACTOR NEUTERED (both tension probes scaled by 1.0 instead of 0.5 and 2.0) — FOUR
 *   failures: both windows and both ratios, while THE 43-JOINT COUNT PIN PASSES. It
 *   should: the override still touched 43 joints, it just wrote the same number back. A
 *   count pin catches a selector that matched nothing and cannot catch a knob that turns
 *   nothing, and that is exactly why the ratios are pinned beside it.
 *
 * ONE HONEST LIMIT OF PROBE 1: it takes the cohesion out of all 36 joints at once,
 * including the two bearing courses at the top of the jamb, so it prices THE CHAIN AS A
 * WHOLE and cannot say whether the bearing or the run down to the ground is the tighter
 * link. Splitting it is one more row of the same shape and was deliberately left for
 * whoever needs that distinction; it is recorded in CURRENT_STATE rather than implied here.
 *
 * COST: four solves at 83 blocks, 4.4 s. NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowOpeningProbesTest,
	"OracleSlowSweep.RigidBlock.OpeningStrengthProbes",
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
		ERelation::OracleStandsProductionFalls, 5.5108993, 5.5111198, 45, 0, nullptr, 0 });

	Rows.Add({ TEXT("probe: jamb bed joints, cohesion zeroed"),
		TEXT("THE CLOSING EXPERIMENT. Every bed joint below the cover — which is every bed ")
		TEXT("joint in the jamb, the bearing under the cover included — loses its cohesion ")
		TEXT("and keeps its friction and its tension. If the jamb's shear chain binds, ")
		TEXT("lambda* collapses toward 1 — and BOTH surviving thrust-line shapes deliver ")
		TEXT("their thrust into these joints, so this prices the capacity, not the shape. ")
		TEXT("IT COLLAPSED PAST 1 — 0.785, a factor of 7.02 — which is why this is the one ")
		TEXT("case-21 row in the file whose relation is AGREE (falls): with its jamb ")
		TEXT("cohesion gone the MODIFIED wall has no equilibrium at self-weight (hand ")
		TEXT("statics and production judged the REAL wall — two verdicts agreeing, not ")
		TEXT("three methods on one fixture)"),
		CaseTwentyOne,
		ERelation::AgreeFalls, 0.7851011, 0.7851325, 45, 0,
		[](FOracleProblem& Problem) { return ZeroJambBedCohesion(1, Problem); }, 36 });

	Rows.Add({ TEXT("probe: cover head-joint tension x0.5"),
		TEXT("half the tensile bond across the cover's own head joints, nothing else ")
		TEXT("touched — the knob a deep bending panel's capacity is linear in. Halving it ")
		TEXT("costs 4.8%, where a panel would lose half"),
		CaseTwentyOne,
		ERelation::OracleStandsProductionFalls, 5.2458863, 5.2460962, 45, 0,
		[](FOracleProblem& Problem)
		{
			return ScaleCoverHeadTension(1, 0.5, Problem);
		}, 43 });

	Rows.Add({ TEXT("probe: cover head-joint tension x2"),
		TEXT("and twice it, so the pair spans a factor of four: a cover carrying its span ")
		TEXT("in bond tension reads 4.00x across the two, and anything else reads 1.00. It ")
		TEXT("reads 1.151 — the bond contributes, and it is not what holds the wall up"),
		CaseTwentyOne,
		ERelation::OracleStandsProductionFalls, 6.0400073, 6.0402489, 45, 0,
		[](FOracleProblem& Problem)
		{
			return ScaleCoverHeadTension(1, 2.0, Problem);
		}, 43 });

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

	if (Control == nullptr || NoCohesion == nullptr || HalfTension == nullptr
		|| DoubleTension == nullptr)
	{
		return true;
	}

	/*
	 * ALL FOUR ROWS ARE ONE WALL, so all four bridge to the same 83 blocks. A probe row
	 * that quietly built a different fixture would otherwise report a moved lambda* and be
	 * read as a strength finding.
	 */
	CheckRungSize(*this, TEXT("probe control"), *Control, 83);
	CheckRungSize(*this, TEXT("probe: cohesion zeroed"), *NoCohesion, 83);
	CheckRungSize(*this, TEXT("probe: tension x0.5"), *HalfTension, 83);
	CheckRungSize(*this, TEXT("probe: tension x2"), *DoubleTension, 83);

	/*
	 * AND ALL FOUR PRODUCTION READINGS ARE ONE NUMBER, because the override never touches
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
		TEXT("the jamb-shear-chain hypothesis predicted a collapse toward 1 (0.18 of the ")
		TEXT("control), anything else predicted 1.00 — measured 0.1425, i.e. PAST 1.0 in ")
		TEXT("absolute terms, so the chain is not merely binding, it is essentially the ")
		TEXT("whole capacity"),
		*NoCohesion, *Control, 0.142449, 0.142478);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE COVER'S OWN BOND, over a four-fold lever (tension x2 / tension x0.5)"),
		TEXT("a cover spanning in bond tension predicts 4.00, a cover whose tension is ")
		TEXT("incidental predicts 1.00 — measured 1.151, so the bond contributes about a ")
		TEXT("seventh of what carrying the span would require"),
		*DoubleTension, *HalfTension, 1.15126, 1.15150);

	/*
	 * AND THE LOW HALF OF THAT LEVER ON ITS OWN. The four-fold ratio above would also be
	 * satisfied by a solver that had stopped responding to strengths altogether and read
	 * 1.151 by accident on both sides; this one says the x0.5 rung really does sit 4.8%
	 * under the control, so the knob is connected. Its partner (x2 / control = 1.09601) is
	 * the quotient of these two and is deliberately not pinned a third time.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("the low half of the tension lever (tension x0.5 / control)"),
		TEXT("a bond-governed cover predicts 0.50, an unaffected one 1.00 — measured 0.952"),
		*HalfTension, *Control, 0.95181, 0.95201);

	return true;
}

/**
 * ====================================================================================
 * THE PHASE-2 REFUSAL IS A SOLVER DEFECT, AND THESE ARE THE TWO SMALLEST FIXTURES THAT
 * PROVE IT: A BOUNDED, FEASIBLE PROBLEM MUST NOT COME BACK "phase-2 simplex failed".
 * ====================================================================================
 *
 * WHAT IS UNDER TEST, IN ONE SENTENCE. Two opening-ladder rungs of 99 and 107 blocks —
 * each SMALLER than answering rungs measured beside them in this very run, and each on an
 * LP the lambda-cap row makes provably bounded — must return an answer rather than
 * refusing, and the answer must land between the two neighbours that bracket it.
 *
 * WHY THIS IS A DEFECT AND NOT FAIL-CLOSED CORRECTNESS. Refusal is the safe direction and
 * the oracle is right to take it when it cannot certify an answer; the finding here is
 * that it CAN certify one. Both fixtures were solved to a verified optimum during the
 * diagnosis — the post-solve verification gate at 1e-6 passed on both — the moment the
 * solver's own anti-cycling fallback was stood down. So an admissible force system exists,
 * the solver can find it, and what stops it is arithmetic of its own making.
 *
 * ------------------------------------------------------------------------------------
 * THE DIAGNOSIS, WITH THE EVIDENCE CHAIN (measured 2026-08-13, instrumented run)
 * ------------------------------------------------------------------------------------
 *
 * ONE REFUSAL STRING HIDES TWO DIFFERENT NUMERICAL FAILURES. `SolveRigidBlock` maps every
 * non-Optimal phase-2 end onto the same "phase-2 simplex failed", and the family reaches
 * that string by two distinct routes:
 *
 *   (a) SPURIOUS UNBOUNDEDNESS — the 107-block j=4 rung. Measured at pivot 671:
 *
 *         entering=215 partner=216 partnerbasic=1 d=-1.1200197386711438e-09
 *         wnnz=1 wmax=1 atrow=91 wAtLargest=-1
 *
 *       The LP splits each shear force v into p - q and each normal into n+ - n-, and for
 *       the shear pair the two columns are the EXACT BITWISE NEGATION of one another in
 *       every row that touches them (the three equilibrium rows, both friction rows, both
 *       ceiling rows). Column 216 was basic, so column 215's exact reduced cost is zero;
 *       what entered it was 1.12e-9 of drift against an ABSOLUTE CostTol of 1e-9. Its
 *       FTRAN is then -e_91 exactly — one nonzero, value -1 — so the ratio test finds no
 *       positive entry and the simplex reports Unbounded on a problem the cap row bounds.
 *
 *   (b) A BASIS PIVOTED INTO SINGULARITY — the 99-block rung, and the case-22 family's own
 *       218-block wall. A column nearly dependent on the basis is accepted on a ratio-test
 *       pivot barely over PivotTol = 1e-9; the next clean refactorisation then finds an LU
 *       pivot below SingularPivotTol = 1e-11 and `Factorise` refuses:
 *
 *         99-block rung  : position 2302/2845, col 3779 (a slack), pivot 1.3227514269178168e-13
 *         218-block 12x22: position 6898/6901, col 10519 (a slack), pivot 5.8808003531153705e-13
 *
 * THE FIRST ATTRIBUTION WAS THE BLAND FALLBACK'S ENTERING RULE, AND MEASUREMENT REFUTED
 * IT. The family stalls degenerately in bulk, which engages `FPartialPricer`'s Bland
 * anti-cycling fallback (172 of 671 pivots on the j=4 rung; 297 of 836 on the 99-block
 * rung), and that fallback takes the FIRST column priced below -CostTol in index order
 * rather than the best — so the story wrote itself: first-past-the-post lets 1e-9 of noise
 * win. Standing the fallback down did make both fixtures answer, which is what made the
 * story convincing. Three measurements taken while implementing the fix say it was wrong:
 *
 *   - MAKING THE FALLBACK'S CANDIDACY TOLERANCE RELATIVE CHANGED NOTHING. Scaling -CostTol
 *     by the largest term in the reduced cost's own dot product left every pivot count on
 *     all six rungs BIT-IDENTICAL, because at the failure those terms are ~1.2e-9 and the
 *     scaled tolerance is the absolute one.
 *
 *   - THE DRIFT IS INHERITED FROM THE DUAL SOLVE, NOT FROM THE COLUMN. At the refusal
 *     ||y||inf = 6.7e6, and machine epsilon against that is 6.7e-10 — the same order as
 *     the 1.12e-9 that entered. No candidacy rule reading that column can tell the
 *     difference, because the noise is not in the column.
 *
 *   - THREE OF THE FOUR NOISE PIVOTS IN MODE (b) WERE ON THE RANKED PATH, NOT BLAND'S.
 *     Measured on the 99-block rung: pivots of 4.7e-9 (Bland), then 6.9e-9, 6.7e-9 and
 *     1.6e-8 with the pricer in charge, all out of columns whose FTRAN image runs to
 *     1.7e5-3.0e7. A fix confined to the fallback could not have closed it.
 *
 * WHAT THE CAUSE ACTUALLY IS: THE RATIO TEST HAD NO SENSE OF SCALE. It accepted any
 * coefficient over an ABSOLUTE PivotTol = 1e-9, and a coefficient of 4.7e-9 out of a column
 * whose largest is 3e7 is a RELATIVE pivot of 1.6e-16 — the rounding of the solve that
 * produced the column. Pivoting on rounding builds an ill-conditioned basis; an
 * ill-conditioned basis is what makes ||y|| 6.7e6 and the LU pivot 1.3e-13. Both modes are
 * that one defect at different distances downstream, which is why ONE change closes both:
 * `RelativePivotTol` (RigidBlockOracle.cpp), a floor of 1e-11 of the entering column's own
 * largest magnitude. Reverting that floor alone reproduces this test's original red
 * exactly — 836 and 671 pivots, both refusals — and it is the recorded bite-prover.
 *
 * WHAT WAS NOT NEEDED, MEASURED RATHER THAN ASSUMED. A repair for the no-entry seam itself
 * (refactorise, re-price, set the column aside, take the next candidate) was written and
 * then REMOVED: with the floor in place, reverting the seam to its plain refusal leaves
 * both fixtures answering with bit-identical lambda* and identical pivot counts, so no
 * failing test drove it. The seam therefore still refuses a bounded problem if it is ever
 * reached — CURRENT_STATE books that as the residual mode and as a live candidate for the
 * case-22 family's continuing refusal.
 *
 * THE 218-BLOCK CASE-22 RUNG IS DELIBERATELY NOT ASSERTED HERE (143-275 s a solve), and it
 * still refuses after this fix. Also recorded so nobody re-measures it: the fallback EARNS
 * ITS KEEP on that rung — without it the pivot path runs past the 100,000-pivot cap (275 s,
 * no answer) where with it the run terminates in 143 s — so nothing here argues for
 * deleting it.
 *
 * WHAT THE FIX MUST NOT DO. It must not reach the answer by loosening the post-solve
 * verification gate, and it must not remove the iteration cap, which is the termination
 * proof (RigidBlockOracle.h says so). Nor may it scale the OPTIMALITY tolerance by ||y||:
 * that would put it at ~6.7e-3, and a simplex stopping early reports a feasible point with
 * a lambda* too LOW — which verification, being a check on admissibility, certifies
 * happily. The lambda brackets below are what stops a fix that merely stops refusing: a
 * solver that answers a wrong number fails these just as loudly as one that refuses.
 *
 * ------------------------------------------------------------------------------------
 * WHY THE EXPECTED VALUES ARE BRACKETS DERIVED IN THE RUN, NOT PINNED NUMBERS
 * ------------------------------------------------------------------------------------
 *
 * A fix can move the pivot path, and this file's own PARTIAL-PRICING RE-PIN note records
 * that lambda* at 100+ blocks is reproducible only to ~1e-5 across paths — the Bland
 * experiment measured exactly that (the j=3 rung read 7.3790076229482224 with the fallback
 * and 7.37893585396119 without). A window pinned on a fixture NOBODY HAS SOLVED YET would
 * therefore pin the algorithm. So each refuser was specified as bracketed by ITS OWN TWO
 * LADDER NEIGHBOURS, measured in the same run by the same solver:
 *
 *     SPAN LADDER (s=3, j=3, opening 8 / 9 / 10 cells). The opening's top course is course
 *     5, which is ODD, so the clear reveal is a full cell (22.5 cm) under the even-course
 *     formula: 158.5 -> 181.0 -> 203.5 cm. lambda* falls with span, so the 9-cell rung must sit
 *     between the 10-cell and the 8-cell readings. Corroboration nobody has to trust the
 *     test for: the family's measured lambda* ~ L^-2.39 predicts 39.12 from the 10-cell
 *     rung and 35.82 from the 8-cell one, straddling the 37.932 both certified solves
 *     produced.
 *
 *     ABUTMENT LADDER (o=18, s=1, jamb 3 / 4 / 5 cells). More jamb is more bearing and
 *     more weight over it, and the ladder reads 7.379 / ? / 11.115, so the 4-cell rung must
 *     sit between them. The measured 9.3587 does, 1.2% above the midpoint.
 *
 * The neighbours carry LOOSE +/-1% sanity windows — a thousand times looser than the 1e-5
 * the path moves by, so they cannot flap — for one reason only: without them a solver
 * returning garbage on all four rungs could satisfy the brackets. They are a floor under
 * the bracket, never a measurement.
 *
 * AND THE 99-BLOCK RUNG NOW CARRIES A REAL WINDOW AS WELL, because the reason not to pin
 * one expired the moment it was solved TWICE by different pivot paths: 37.932472960794136
 * with the Bland fallback stood down during the diagnosis, and 37.932472960794129 by the
 * repaired solver — TWO ULPS apart, a tighter agreement than the j=4 rung's 5.3e-7. The
 * window is the file's standard midpoint-of-two-certified-readings +/-2e-5, which is ~1e4
 * times the spread the two paths actually showed, and it sits BESIDE the bracket rather
 * than replacing it: the bracket states the physics (a wider opening cannot be stronger),
 * the window states the number. This rung is the only fixture in the suite that carries
 * it — o=9/s=3/j=3 appears nowhere else — which is why the pin belongs here and the j=4
 * rung's equivalent lives in OpeningMechanismLadders instead of being duplicated.
 *
 * AND EVERY RUNG CARRIES A BLOCK-COUNT PIN, per TRAPS: a lambda window and a ratio both
 * pass on a rung that quietly built its partner's wall, and only a size pin caught the
 * recorded rung-flip. The counts also carry the finding that SCALE IS NOT THE EXPLANATION
 * — the refusing rungs are 99 and 107 blocks while the 104- and 119-block rungs beside
 * them answer, and wall-01's 375 pieces answer at lambda* 272.20.
 *
 * COST: six solves, ~28 s (it was ~15 s while two of them refused early). The j=3 and j=4
 * rungs are also solved by OpeningMechanismLadders, ~7.3 s of deliberate duplication: a
 * bracket has to be closed by neighbours measured in the SAME RUN by the SAME BINARY, and
 * a number carried across from another test would be exactly the assumption this test
 * exists to avoid making.
 * NEEDS A TICKING WORLD: NO — the oracle is world-free arithmetic over a built FStructure,
 * and this test never runs production's cascade at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockPhaseTwoBoundedTest,
	"OracleSlowSweep.RigidBlock.PhaseTwoMustNotRefuseABoundedProblem",
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
		 * The midpoint-of-two-certified-readings window, +/-2e-5 relative, on a rung that
		 * has been solved by two different pivot paths. Both zero where no such pair
		 * exists — the neighbours have one certified reading each, and the j=4 rung's pair
		 * is pinned in OpeningMechanismLadders rather than duplicated here.
		 */
		double CertifiedLo = 0.0;
		double CertifiedHi = 0.0;
	};

	const FRung Rungs[] =
	{
		{ TEXT("span ladder, 8-cell opening (the wide-lambda neighbour)"),
			8, 3, 3, 94, 206, 48.6984, 49.6823, 0.0, 0.0 },
		{ TEXT("span ladder, 9-cell opening (THE 99-BLOCK REFUSER)"),
			9, 3, 3, 99, 216, 0.0, 0.0, 37.9317143, 37.9332316 },
		{ TEXT("span ladder, 10-cell opening (the narrow-lambda neighbour)"),
			10, 3, 3, 104, 226, 29.2715, 29.8629, 0.0, 0.0 },
		{ TEXT("abutment ladder, 3-cell jamb (the narrow-lambda neighbour)"),
			18, 1, 3, 95, 163, 7.3052, 7.4528, 0.0, 0.0 },
		{ TEXT("abutment ladder, 4-cell jamb (THE 107-BLOCK REFUSER)"),
			18, 1, 4, 107, 193, 0.0, 0.0, 0.0, 0.0 },
		{ TEXT("abutment ladder, 5-cell jamb (the wide-lambda neighbour)"),
			18, 1, 5, 119, 223, 11.0039, 11.2262, 0.0, 0.0 },
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

		/*
		 * The bridge's own reason is READ INTO A LOCAL FIRST, never folded into the
		 * Printf beside the call that writes it: argument evaluation is indeterminately
		 * sequenced, so the message could be built from the empty string it had before
		 * the call ran (TRAPS records the same trap in its TestTrue form).
		 */
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
	 * THE NEIGHBOURS FIRST: they are answering rows today, and their sanity windows are
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
	 * THE ONE RUNG WITH TWO CERTIFIED READINGS GETS A REAL WINDOW, and it sits beside the
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
	 * THE DEFECT ITSELF. Each refuser must answer, and must answer BETWEEN the two ladder
	 * neighbours measured beside it — the pair are named in the message so a failure says
	 * which reading it fell outside rather than only that it did.
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

#endif // WITH_DEV_AUTOMATION_TESTS
