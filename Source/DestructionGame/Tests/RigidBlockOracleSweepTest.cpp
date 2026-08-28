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
 *   - STRENGTHS WERE THE CODED CHARACTERISTIC VALUES (f_xk1 = 0.10) WHEN EVERY WINDOW
 *     IN THIS FILE WAS PINNED, and the /6 characteristic-vs-mean discount the §8 rulings
 *     applied on top is SPENT by the 2026-08-14 mean re-anchor flip (TRAPS.md): the discount
 *     now lives in the profile data and must never be applied twice.
 *
 *     >>> MEAN RE-ANCHOR, GREEN-PHASE RE-MEASUREMENT MARKER (2026-08-13) <<<
 *     When the profile rows flip to the mean basis, EVERY strength-governed lambda*
 *     window in this file moves (roughly x4.5 where jamb-bed cohesion binds, x7 where
 *     tension binds — but utilisation does NOT scale linearly across mechanisms: the
 *     cohesion/tension ordering flips on some fixtures, so no window may be re-pinned by
 *     scaling). Production's drop/strand pins move wherever the cascade outcome was
 *     strength-governed (rows 10/19 are unroutability, zero passes — their counts do NOT
 *     move; row 20, 21 and 22's do). Re-pin per this file's established discipline: two
 *     certified readings per window (the pivot-path lesson — never tighter than ~1e-5
 *     relative at 120+ blocks), window + ratio + block-count pins as a set (the N6
 *     rung-flip lesson), and the case-21 probe's AgreeFalls row must be re-checked
 *     against the 1.0 line (its 0.785 scales ~x4.5 to ~3.5 and the enumerator likely
 *     flips to a stand — that is a re-measurement, not a regression). The DRY-STONE rows
 *     (leaning-stack dry ladder, the 1.2372 springing identity family) must come back
 *     BIT-IDENTICAL — dry stone's row does not move. The beam-pair rows transcribe their
 *     own characteristic C24/S275 constants (below) while BeamAcceptanceTest moved to
 *     means 2026-08-13: the re-transcription (24->36, 4->6, 275->290) and the re-measured
 *     windows (member-bound rows expected ~x1.5 / x1.055) are owed in the same pass.
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
 * 4,092 pivots), so C and D live in the OPT-IN sweep at the bottom of this file (in its
 * FULL tier, OracleSweepFull.RigidBlock.WallsAndLadders); neither tier's name contains
 * "DestructionGame", so the documented full-suite command never pays for either — see the
 * two-tier header below for the convention and the per-test costs.
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

	/*
	 * Production's wall-16 / wall-15 worst-joint ratio, re-measured 4.5082330043756453 at
	 * the mean re-anchor (0.0083148340273646662 over 0.0018443665221594297 — both ends are
	 * DESIGN §6 anchors). It was 31.5576 at the characteristic data and the shrink is the
	 * TRAPS axis lesson in one number: the bare side is tension-governed and moved /7, the
	 * loaded side is compression-governed and did not move, so the ratio is NOT
	 * strength-invariant. The acceptance suite's own floor moved 10 -> 3 with the same
	 * measurement.
	 */
	constexpr double SuperimposedReadingRatioLo = 4.5081;
	constexpr double SuperimposedReadingRatioHi = 4.5084;

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
		 * 5.511 / 7.379 / 9.359 across j=2/3/4).
		 *
		 * NOTHING USES THIS TODAY, AND NOTHING CAN: since Slice 0a (2026-08-15) NO FIXTURE
		 * THIS PROJECT OWNS REACHES ANY PHASE-2 REFUSAL ARM. The whole OracleSweep answers,
		 * and the unbreakable lambda-cap tower terminates Optimal rather than unbounded.
		 * The claim that stood here until then — that case 22's family still refused at
		 * 218+ blocks "for a reason that outlived the fix" — was FALSE and had been for a
		 * day: those readings were taken at the characteristic strengths and expired when
		 * the mean re-anchor moved the data under them. Case 22 answers at 8.4149459982.
		 *
		 * The enumerator stays for the next refusal rather than for a known one, and that
		 * is a deliberate choice with a cost: this branch is live code nothing drives. The
		 * cheapest fixture that would drive it again is an ITERATION-CAP refusal, not this
		 * one — case 22 answers at 88.8% of MaxPivots, so a slightly wider member of its
		 * family hits the cap deterministically. CURRENT_STATE carries that as owed work.
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

	/*
	 * MEAN BASIS, re-transcribed 2026-08-14 in step with BeamAcceptanceTest's 2026-08-13
	 * move (JCSS x1.50 on EN 338's f_m,k and f_v,k; JCSS Table A static yield for S275) —
	 * a sweep row that built the beam at the characteristic strengths would be measuring a
	 * different fixture from the catalogue row it exists to cross-examine.
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
	 * SLICE 0c'S CRUSHING-RELAX VALUE. "Relax the crushing cap" means REMOVE the compressive
	 * limit, not zero it: zeroing CompressiveStrengthMPa would forbid the joint from carrying
	 * ANY compression and collapse the wall for a reason that has nothing to do with crushing.
	 * A value 1e5x the profile's real 10 MPa (and ~1e6x the ~0.09 MPa self-weight compression
	 * a residual-lambda* solve ever asks of these joints) makes the crushing row non-binding
	 * without introducing a NaN or an infinity the oracle would refuse. Derived here, not
	 * imported, so it fails against a wrong production convention rather than agreeing with it.
	 */
	constexpr double ResidualUncappedCrushingMPa = 1.0e6;

	/**
	 * THE HEIGHT LINE THAT SPLITS THE JAMB CHAIN into its two bearing courses at the top and
	 * its run down to the ground — the split CURRENT_STATE and the cohesion probe's own header
	 * (OpeningStrengthProbes, "ONE HONEST LIMIT OF PROBE 1") record as owed. The cover
	 * underside is at 4x the pitch for case 21 (CoursesBelow = 1, LadderOpeningCourses = 3),
	 * and the below-cover bed joints occupy the four course levels beneath it. Two pitches up
	 * puts the top two levels (the bearing courses the cover lands on) on one side and the run
	 * to ground on the other, with a full course of clearance on either side of the line so it
	 * never clips a joint it is meant to separate.
	 */
	constexpr double LadderChainBearingSplitZCm(int32 CoursesBelow)
	{
		return double(CoursesBelow + LadderOpeningCourses - 2) * SweepCoursePitchCm;
	}

	/**
	 * SLICE 0c'S RESIDUAL-ATTRIBUTION KNOB: take case 21's residual state (jamb bed cohesion
	 * gone, the state whose lambda* is 4.768) and remove ONE MORE resistance mechanism from a
	 * chosen band of the jamb chain, so the re-solve says whether that mechanism was what held
	 * the residual up.
	 *
	 * It is the SAME SHAPE as ZeroJambBedCohesion — a per-joint override on the oracle problem
	 * only, leaving production reading the untouched wall as the control — extended two ways
	 * the attribution needs and no further:
	 *
	 *   - WHICH mechanism: cohesion (the residual-defining removal), friction (the Coulomb
	 *     mu term — the only shear a cohesionless bed joint has left), the crushing cap
	 *     (relaxed, not zeroed — see ResidualUncappedCrushingMPa), and the flexural TENSILE
	 *     bond (zeroed — the 0.70 MPa the mu=0 optimum re-forms on; probe 5 removes it on top
	 *     of friction to prove the 9.935 was carried by that bond). Any combination, because
	 *     "friction AND crushing" and "friction AND tension" are two of the measurements.
	 *   - WHERE: a Z band [BandLoZCm, BandHiZCm), so the cohesion removal can be confined to
	 *     the bearing courses or to the run to ground and the chain-vs-bearing split is one
	 *     row of this shape rather than a new mechanism.
	 *
	 * @return how many jamb bed joints were rewritten — pinned or cross-checked by the row,
	 *         never trusted, because a band selector that matched nothing measures nothing.
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
	 * THE EIGHT-COURSE-COVER FAMILY — acceptance case 22's shape, parameterised on width.
	 *
	 * WHY IT IS A SEPARATE BUILDER AND NOT A PARAMETER ON THE ONE ABOVE. `LadderCoverCourses`
	 * is held at case 21's two by a file constant, and eleven pinned rungs read it through
	 * `LadderCoverUndersideZCm`; widening that constant into a parameter would put a default
	 * argument under every one of them. This family differs from the ladder in exactly one
	 * dimension — the courses of cover — and its cut is one course band whatever the width,
	 * so a sibling with its own arithmetic costs less than a shared one with a footnote.
	 *
	 * THE SHAPE IS CASE 22'S, DERIVED HERE RATHER THAN TRANSCRIBED. One grounded course, a
	 * three-course opening cut through courses 1..3, jambs of `JambCells` either side, and
	 * `CoverCourses` of masonry over it — so `CoursesHigh = 1 + 3 + CoverCourses` and
	 * `Cells = OpeningCells + 2 * JambCells`. At eight cover courses and a 35-cell opening
	 * between two-cell jambs that is twelve courses of thirty-nine cells and case 22's own
	 * `{ 1, 3, 1.75, 36.25 }` region, which the acceptance file writes as a literal.
	 *
	 * THE CUT-COUNT PIN IS THE PARITY CHECK. Running bond loses `OpeningCells` whole bricks
	 * from an even course and one fewer from an odd one, and the cut starts at course 1
	 * (odd), so it takes (OpeningCells - 1) + OpeningCells + (OpeningCells - 1) bricks. A
	 * builder that quietly disagreed with the bricklayer would otherwise measure a different
	 * wall from the one its name claims.
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

	/**
	 * SLICE 0d's flag-on adjuster, as an AdjustProblem hook: turn the first-crack rows on
	 * and RETURN the count of joints that will carry them — exactly the bonded set
	 * (f_t > 0), which is what keys the rule on DATA, not on material. Pinning that count
	 * through OverriddenJoints makes "a first-crack row for every bonded joint and none for
	 * a dry one" a contract; a dry-only fixture returns 0, writes no rows, and so returns
	 * bit-identical (the crux keyed-on-data invariant). The count is read from the same
	 * f_t > 0 predicate the oracle keys its own rows on (RigidBlockOracle.cpp), so the two
	 * agreeing is the check, not a copy — a keying change on either side moves this count.
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
 * THE LEANING STACK THROUGH THE REAL FIXTURE AND BRIDGE — sweep item (b), plus the
 * composite-depth measurement of sweep item (e) on the fixture where composite depth is
 * the whole defect.
 *
 * WHAT WAS MEASURED, 2026-08-09 at the characteristic bond: lambda* = 4.4582 / 1.2411 /
 * 0.06293 / 0.03443 at 5/8/30/40 courses — matching slice 1's hand-built ladder through
 * the REAL fixture and bridge this time — while production's cascade agrees on every
 * verdict (0 / 0 / 29 / 39 dropped, the tall rows via the interim guard) and its worst
 * JOINT reading is height-INVARIANT (0.138781067 then). That last number is the
 * composite-depth measurement in one line: the credited deep beam's m^2 cancels the
 * demand's m^2, so a fixture whose true margin the LP measures moving 130x reads
 * IDENTICAL to the joint checks, and at 30 courses the joint reading overstates the
 * true margin by a factor of ~115. The verdicts only agree because
 * BreakOverturnedBodies exists — exactly what DESIGN.md §7 step 4 will replace with
 * this LP.
 *
 * RE-MEASURED AT THE MEAN RE-ANCHOR FLIP (2026-08-14): every rung is tension-governed
 * and moved exactly x7 with the bond (31.207384811713876 / 8.6877701307335329 /
 * 0.44049301907204624 / 0.24100815422114152), the drop counts did not move (the guard's
 * verdicts hold at the profile-read 0.70 bond), and the windows below are re-pinned on
 * those measurements at the file's usual relative width.
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
 *     timber), 19.20 (light timber), 17.35 (heavy steel) at the characteristic
 *     strengths. The bearings equilibrate trivially at contact points; the material
 *     pair discriminated 9.8x where production answers all three rows identically.
 *     The heavy-timber reading is NOT a contradiction of the catalogue's PARTS AT
 *     MIDSPAN: the catalogue's figure is uncracked FIRST-CRACK arithmetic, the oracle
 *     reads the plastic stress block (3x first-crack, its documented ceiling) PLUS
 *     rigid-block load redistribution. For BRITTLE timber first-crack is the honest
 *     criterion, so the catalogue's verdict survives the measurement — but the margin
 *     is thin, and lambda* says exactly how thin.
 *
 * RE-TRANSCRIBED AND RE-MEASURED AT THE MEAN RE-ANCHOR (2026-08-14): the beam is now
 * built at BeamAcceptanceTest's mean strengths (C24 36/6, S275 290) and every lambda*
 * moved by exactly the member factor — timber x1.5000 (2.6461037357339725 heavy,
 * 28.801133612618997 light), steel x1.0545 (18.299323934632291) — the glue line's
 * member-strength bound is what binds all three, cleanly. The material discrimination
 * is 6.9x on the mean basis (18.30 vs 2.65; the strengths converged 9.8x -> 8.1x and
 * the LP tracks it). The catalogue's first-crack figure is 2.32x mean f_m — the
 * oracle's 2.65 stand is 3x plastic + redistribution over that same arithmetic.
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
	 * ROW 1 RE-PINNED AT THE FIRST-CRACK PROMOTION (2026-08-28); ROWS 2/3 STILL AGREE-STANDS.
	 * The oracle here is the DEFAULT-OFF oracle, so its lambda* windows do NOT move — row 1 is
	 * still 2.64610374 (plastic stress block + rigid-block redistribution). What moved is
	 * PRODUCTION's half of the diff: below the 200-block cap production now solves with the
	 * first-crack rows live, and the heavy C24 beam's single bonded midspan glue line carries
	 * near-pure bending (N ~ 0 at midspan), so its uncracked peak-fibre capacity binds at
	 * first-crack lambda* = 0.88258 < 1 (the value Beam.Catalogue row 1 asserts) and production
	 * FELLS the beam — the glue line parts and the two half-beams plus the load block come down,
	 * dropping 3. So row 1 is now ORACLE STANDS (2.65, default-off plastic) / PRODUCTION FALLS
	 * (first crack), the honest first-crack disagreement. Rows 2/3 keep AGREE-STANDS: the light
	 * timber (28.80) and the steel (18.30) sit far above 1.0 even at first crack's /3, so
	 * production stands them too (0 dropped). The oracle's material discrimination (18.30 vs 2.65)
	 * is unchanged; the oracle-lambda* window here must NOT move — only production's drop pin does.
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
 *
 * RE-MEASURED AT THE MEAN RE-ANCHOR FLIP (2026-08-14), and the two rungs moved by
 * DIFFERENT factors — the recorded proof that no window here may ever be re-pinned by
 * scaling: A is tension-bound and moved exactly x7 with the bond
 * (20.332907144632795), while B's jamming is bound in the Mohr-Coulomb family and
 * moved x1.506 (333.40338835842761), between the cap's x1.538 and mu's x1.25 —
 * filling now buys 16.4x on the mean basis, not 76x. (CorbelStepsBeforeTensionWins'
 * crossover likewise re-derived to ~124 steps in COMPRESSION at the re-anchor.)
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
	 * SLICE 0d, A MOVER — the SAME bare arm with the first-crack rows on. Corbel A's four
	 * bonded joints all reach first crack, and the ladder cuts to 0.2786 of the control
	 * (measured 5.6638759380840851). It still stands (5.66 >= 1). The flag touches only the
	 * oracle, so production is the untouched arm — its relation and zero drop count carry over.
	 * OverriddenJoints = 4 pins that a first-crack row was written for every one of the arm's
	 * bonded joints (keyed on data).
	 *
	 * WHY 0.2786 IS BELOW THE 1/3 FLOOR, AND IT IS NOT PER-JOINT NET TENSION (review, 2026-08-21):
	 * a SINGLE joint at eccentricity k = |M|/(|N|h) has a first-crack/plastic ratio of
	 * (1 + k)/(1 + 3k), which lives in (1/3, 1] — net tension (finite k) makes the cut LESS
	 * deep, reaching 1/3 only in pure bending (k -> infinity, N -> 0, the beams). So no single
	 * tension-bending joint can go below 1/3, and the earlier "net tension cuts DEEPER than /3"
	 * gloss was wrong. The sub-1/3 result is a MULTI-JOINT REDISTRIBUTION effect: first crack
	 * binds all four joints of the arm at once, and the LP can no longer trade eccentricity
	 * between them to stay in each kern, so the arm's capacity falls further than any one
	 * joint's would. A finding about the mechanism, not a bug — the rows are correct (item 1
	 * of the review re-derived them), and the beams landing exactly at /3 prove the per-joint
	 * floor holds.
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
	 * SLICE 0d, THE COMPRESSION CONTROL — pinned as a CHARACTERISATION, because the invariant
	 * is APPROXIMATE, not exact. Corbel B jams as a block (Mohr-Coulomb, the header's 16.4x),
	 * so PROMOTION_DESIGN Sec 4.5 predicted first crack ~unchanged; MEASURED it moves 8%
	 * (0.9205 of control, 306.88465535549665) because some bonded joints in the block do
	 * reach net tension in bending and their first-crack rows bind. So category-B invariance
	 * is APPROXIMATE here — recorded, not smoothed. The clean bit-identical control is not a
	 * compression fixture at all: every BONDED fixture in the sweep writes rows and moves at
	 * least this much, and only the DRY set (0 bonded joints, no rows) returns bit-identical.
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
		 * THE MOVE IS THE FINDING, not either rung's window. Corbel A's first-crack/control
		 * ratio is the mechanism: a single pure-bending joint reads exactly /3 = 0.3333 and no
		 * single tension-bending joint can read below it ((1+k)/(1+3k) in (1/3, 1]); corbel A's
		 * arm reads 0.2786, BELOW the per-joint floor, because first crack binds all four joints
		 * at once and the LP can no longer redistribute eccentricity between them (see the
		 * corbel-A mover comment above). The MOVE is the signature of that multi-joint effect;
		 * if this ratio moved, the mechanism changed.
		 */
		CheckLambdaLadderRatio(*this,
			TEXT("SLICE 0d: corbel A's four-joint arm under first crack"),
			TEXT("a single pure-bending joint reads /3 = 0.3333 and none reads below it; the ")
			TEXT("four-joint arm cuts to 0.2786 by multi-joint redistribution — the MOVE, not ")
			TEXT("either rung, is the finding"),
			*AOn, *AOff, 0.27854, 0.27858);

		/*
		 * AND THE CONTROL'S SMALL MOVE, pinned so "approximate invariance" is a number and
		 * not a hedge: a compression-governed block still writes first-crack rows and they
		 * bind by 8% (0.9205). A future change that made corbel B move MORE (or bit-identical)
		 * fails here loudly rather than quietly re-attributing the mechanism.
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
	 * MOVED FROM ORACLE-STANDS / PRODUCTION-FALLS TO AGREE-STANDS AT SLICE 3b/4 (2026-08-27); the
	 * oracle lambda* is UNCHANGED at 9592.68. Through 2026-08-09 production dropped BOTH
	 * non-grounded bricks in two passes — pass 1 refused P's relief at 1.4921 and a dry joint
	 * outside the kern read Max(), pass 2 condemned N behind it — the missing no-tension rocking
	 * model, DESIGN's standing-reads-as-falling gap. Below the 200-block cap the equilibrium LP is
	 * now the break authority and finds the edge-contact jamming force system the limit theorem
	 * always guaranteed, so production stands both bricks (drops 0, was 2) and agrees with the
	 * oracle. The lambda* two orders above 1 confirms this is a wide-margin equilibrium, not a
	 * knife edge.
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
 * SLICE 0d — THE FIRST-CRACK ROWS BITE BONDED BENDING AND SPARE DRY JOINTS.
 *
 * WHAT IS UNDER TEST, IN ONE SENTENCE. With FOracleProblem::bFirstCrackRows set, the LP
 * carries the two uncracked-first-crack rows -(n1+n2) + 3|n1-n2| <= f_t*A for every joint
 * with a real tensile bond and ONLY those, cutting a bonded section's plastic bending
 * capacity to a third (so a bonded-bending-governed lambda* falls toward control/3) while
 * every dry-stone joint, having no bond to crack, returns bit-identical.
 *
 * WHY THIS IS THE RED, AND WHY IT FAILS FOR THE RIGHT REASON. bFirstCrackRows is a COMPILE
 * SEAM ONLY today — a defaulted bool the solver does not read yet (RigidBlockOracle.h). So
 * turning it on adds no rows and leaves lambda* exactly where it was. The beam assertions
 * below demand a MEANINGFUL DROP; with the rows absent lambda*(on) == lambda*(off) and they
 * fail because THE ROWS ARE MISSING, not because anything failed to compile. dev-expert
 * makes them green by assembling the rows in SolveRigidBlockOnce, gated on the flag and on
 * f_t > 0. See the predictions record (PROMOTION_DESIGN Sec 4.3/4.5; Slice 0d) for the
 * full per-fixture table this cheap red is the front edge of.
 *
 * THE ASSERTIONS, AND WHY EACH.
 *   - MECHANISM, not outcome: the beam rows assert lambda* MOVED (the rows bite), never a
 *     verdict. Beam row 1's predicted 0.882 crosses 1.0 and would move a catalogue relation
 *     — that is a USER RULING (PROMOTION_DESIGN Sec 4.3, Sec 8's beam caveat), REPORTED here,
 *     never encoded as settled.
 *   - PRECISE window near control/3: BuildBeam lays exactly ONE bonded joint (the glue line)
 *     and a simply-supported span carries N~=0 at midspan, so first crack is at full /3
 *     severity and lambda* -> control/3 near-exactly. The window is wide enough (0.28-0.42 of
 *     control) for arching/shear wobble and green re-measurement, and 1.0 (the no-op) is
 *     wildly outside it, so the red reason is unambiguous. Green MEASURES and re-pins;
 *     never scale.
 *   - BIT-IDENTITY for the dry pair is the crux keyed-on-DATA invariant. It is GREEN ON
 *     ARRIVAL under the no-op seam and stays green when dev keys on f_t > 0 — so it asserts
 *     nothing until proven to bite. Its bite-prover is a GREEN-PHASE mutation: key the rule
 *     on "always" (or on material) and the dry lambda* moves. Recorded, not run here.
 *
 * TIER: DEFAULT SUITE. The beam is microseconds and the dry one-cell solves fast; two solves
 * of each is still cheap. The FULL-SWEEP re-measurement (corbels, case 21, the slow walls,
 * every bonded window moved and every dry identity pinned) is LARGER than one clean red step
 * and is green-phase OracleSweepFull work — it needs measured windows that do not exist until
 * the rows do. NEEDS A TICKING WORLD: NO.
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
	 * Solve one built structure twice: control (flag off) then first-crack (flag on) on the
	 * IDENTICAL bridged problem. Returns false and reports why if the fixture or bridge or
	 * either solve fails, so a red never hides behind a broken fixture.
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

		/*
		 * The override target set is what the keying claims: exactly the ONE bonded glue
		 * line. If this is not 1 the fixture changed and every prediction below is about a
		 * different wall.
		 */
		TestEqual(
			*FString::Printf(
				TEXT("%s: exactly one bonded joint (the glue line) carries a first-crack row"),
				Beam.Name),
			P.Bonded, 1);

		/*
		 * The control must still read its pinned window — otherwise the flag-off path is not
		 * the wall the prediction was made against, and the ratio below is meaningless.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: control lambda* %.9g must sit at the pinned %.9g (flag off is the ")
				TEXT("untouched wall)"),
				Beam.Name, P.Off.Lambda, Beam.ControlLambda),
			FMath::Abs(P.Off.Lambda - Beam.ControlLambda) <= 1.0e-4 * Beam.ControlLambda);

		/*
		 * THE BITE (the red's must-pass): first crack is ADDED, so lambda* can only shrink
		 * (monotone theorem), and where the bonded glue line's bending governs it shrinks
		 * MEANINGFULLY — safely past half, well inside the /3 prediction. With the rows
		 * absent lambda*(on) == lambda*(off) and this fails, which is the whole red.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: FIRST-CRACK ROWS MUST BITE — lambda*(on)=%.9g must fall below ")
				TEXT("0.5x lambda*(off)=%.9g (predicted control/3=%.9g). It did not move, so ")
				TEXT("the rows are absent"),
				Beam.Name, P.On.Lambda, P.Off.Lambda, Beam.ControlLambda / 3.0),
			P.On.Lambda < 0.5 * P.Off.Lambda);

		/*
		 * THE PRECISE ENCODING: N~=0 at midspan puts first crack at full /3 severity, so
		 * lambda*(on) lands near control/3. The window is wide enough for arching/shear and
		 * for green re-measurement; 1.0 (the no-op) is nowhere near it.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda*(on)=%.9g must land near control/3=%.9g (window 0.28-0.42 of ")
				TEXT("control %.9g) — the first-crack factor is exactly 3 in pure bending"),
				Beam.Name, P.On.Lambda, Beam.ControlLambda / 3.0, Beam.ControlLambda),
			P.On.Lambda >= 0.28 * P.Off.Lambda && P.On.Lambda <= 0.42 * P.Off.Lambda);
	}

	/*
	 * USER-DECISION ITEM, REPORTED NOT ASSERTED: beam row 1's predicted 0.882 (2.6461/3)
	 * crosses 1.0, which would move its catalogue relation toward AGREE(falls) — the right
	 * verdict (the member fails in bending) by a slightly wrong route (the glue line cracks).
	 * PROMOTION_DESIGN Sec 4.3/Sec 8 rule this the user's call; the test asserts the drop, not
	 * the flip.
	 */
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

			/*
			 * THE CRUX INVARIANT: no bonded joint => no rows written => lambda* AND the pivot
			 * count return bit-identical with the flag on. GREEN ON ARRIVAL under the no-op
			 * seam; its bite-prover is a green-phase mutation (key the rule on material/always
			 * and this dry lambda* moves), recorded, not run here.
			 */
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
 * THE OPT-IN ORACLE SWEEP, IN TWO TIERS.
 *
 * NEITHER NAME CONTAINS "DestructionGame", so the documented full-suite command —
 * Automation RunTests DestructionGame — NEVER runs either tier and the ~30 s suite budget
 * is untouched. Both tiers substring-match one filter, which is what the names are for:
 *
 *     -ExecCmds="Automation RunTests OracleSweepFast"  10 tests,   ~2 min
 *     -ExecCmds="Automation RunTests OracleSweepFull"   4 tests,  ~24 min
 *     -ExecCmds="Automation RunTests OracleSweep"      14 tests,  ~26 min (both)
 *
 * MEASURED, not assumed (counts re-checked 2026-08-21): those three filters return
 * **10, 4 and 14** tests and the same count of Test Completed lines. The count grew from
 * 7/3/10 as the regional-sandwich, warm-start and Slice 0c attribution rows landed. The
 * command-line filter is a plain SUBSTRING
 * match (AutomationCommandline.cpp, GenerateTestNamesFromCommandLine — "otherwise just
 * substring match"), so the shared OracleSweep stem is the union, and neither tier can be
 * reached by the default suite's DestructionGame filter. Assuming filter semantics is what
 * TRAPS' +-joined-filter entry exists to warn about, which is why these were counted.
 *
 * WHICH TEST GOES WHERE, and it is cost alone — every row in both tiers carries pinned
 * expectations, and a tier buys exclusion from a run and nothing else. Per-test seconds,
 * measured 2026-08-16 from the automation log's timestamps (the two biggest rows read 455
 * and 432 s on the same fixtures a day earlier — this machine varies ~5% under load, which
 * is more than several fast rows cost, so quote the range and not the last number):
 *
 *     FULL   WallsAndLadders                              478 s
 *     FULL   PhaseTwoMustNotRefuseTheCoveredOpeningFamily  455 s
 *     FULL   FeasibilityReformulationCost (other file)     313 s  (344 before its trim)
 *     FULL   WarmStartAtWallScale (other file)            (see OracleWarmStartTest.cpp)
 *     fast   OpeningMechanismLadders                      22.4 s
 *     fast   PhaseTwoMustNotRefuseABoundedProblem         19.6 s
 *     fast   FreeEndHeightLadder                          16.1 s
 *     fast   RepairedRegionalSandwich (other file)         9.6 s
 *     fast   Case21ResidualAttribution                    ~7.0 s
 *     fast   RegionalSandwich (other file)                 7.2 s
 *     fast   SubUnityWallCertificate                      (with the sandwich rows)
 *     fast   WarmStartAfterADeletion (other file)         (see OracleWarmStartTest.cpp)
 *     fast   OpeningStrengthProbes                         4.1 s
 *     fast   RefusalNamesItsReason                         1.0 s
 *
 * **THE FAST TIER IS FOR ITERATION AND IS NOT VERIFICATION.** Three tests are 94% of the
 * cost and they are the three that watch the solver at scale: the wall catalogue's fifteen
 * pinned lambda*, the covered-opening family's two repaired refusals, and the feasibility
 * reformulation's eight-fixture cost table. **OracleSweepFull is mandatory before any
 * commit that touches the LP oracle, and before any commit at all if the solver changed.**
 * A green fast tier says nothing about any of that.
 *
 * AND THE HAZARD THE SPLIT CREATES, stated where the split is: an opt-in tier is a tier
 * that rots. The NonNullRHI visual test rotted through five slices for exactly this reason
 * and produced twelve errors when it was finally run (TRAPS). The mitigation is the rule
 * above plus the cost table beside it — a tier nobody can price is a tier nobody runs.
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
 * four tests as of 2026-08-13, and every surviving row again across the 2026-08-16 tier
 * split — which is the determinism contract holding across the
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
		TEXT("cells, which is what the ruling credited. PRODUCTION NOW AGREES (Slice 3b/4, ")
		TEXT("2026-08-27): below the 200-block cap the equilibrium LP is the break authority ")
		TEXT("and carries the panel the downward-only router could only strand, so production ")
		TEXT("drops 0 (was 12) and strands 0 (was 3) — the absent-mechanism verdict is gone and ")
		TEXT("the row agrees with the oracle. The lambda* window is UNCHANGED; only production's ")
		TEXT("half of the diff moved"),
		Scenario(TEXT("wall-10")),
		/*
		 * RE-PINNED 2026-08-12 for the partial-pricing pivot-path spread (see the
		 * PARTIAL-PRICING RE-PIN note above): midpoint of 35.8172298 (old path) and
		 * 35.817113279469787 (new path), +/-2e-5 relative. THE LAMBDA* WINDOW IS UNTOUCHED
		 * BY SLICE 4 — the solver did not move; only ProductionFallen/Stranded went 12/3 -> 0.
		 */
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
		 * RE-PINNED 2026-08-12 for the partial-pricing pivot-path spread (see the
		 * PARTIAL-PRICING RE-PIN note above wall-10): midpoint of 12.3824832 (old
		 * path) and 12.382629959455667 (new path), +/-2e-5 relative — this fixture
		 * carries the WORST measured pivot-path spread (1.19e-5), which is the
		 * measurement the +/-2e-5 half-width is built from. THE LAMBDA* WINDOW IS
		 * UNTOUCHED BY SLICE 4 — only ProductionFallen/Stranded went 34/6 -> 0.
		 */
		ERelation::AgreeStands, 47.96274, 47.96466, 0, 0 });

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
		ERelation::AgreeStands, 206.8525, 206.8608, 0, 0 });

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
		/*
		 * RE-PINNED 2026-08-12 for the partial-pricing pivot-path spread (see the
		 * PARTIAL-PRICING RE-PIN note above wall-10): midpoint of 36.5639285 (old
		 * path) and 36.563909109648513 (new path), +/-2e-5 relative. Like wall-12 this
		 * row did NOT fail — the new reading sat 24% of the way into the old box — and
		 * is re-pinned so the next same-magnitude pivot-path change cannot arrive
		 * dressed as a masonry regression on the file's least stable relation.
		 */
		ERelation::AgreeStands, 105.6457, 105.6499, 0, 0 });

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
		ERelation::AgreeStands, 456.874, 456.893, 0, 0 });

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
		ERelation::AgreeStands, 634.570, 634.596, 0, 0 });

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
	/*
	 * RE-MEASURED AT THE MEAN RE-ANCHOR FLIP (2026-08-14), and the row's SHAPE moved with
	 * the approved fallout: production now STANDS the wall (worst pre-cascade reading
	 * 0.93542327561664174 — NOT the predicted 3.8429/7 = 0.549, so the governing AXIS moved
	 * and the reading cannot be re-derived by scaling. WHICH axis it moved to is UNVERIFIED:
	 * the "squeezed-edge compression" first written here was the same claim that proved
	 * wrong for case 9's jamb (Mohr-Coulomb shear, 13x over the compression reading) and for
	 * case 22 (shear again), on joints of this same jamb-bed shape — decompose it over
	 * `GetConnectionForce` before believing any attribution (CURRENT_STATE carries the
	 * specified follow-up; TRAPS: an axis claim with no formula beside it is unverified) —
	 * zero passes, zero dropped), so the relation is AgreeStands: LP and production agree
	 * with each other and BOTH now disagree with the catalogue's ruled Collapse, which is
	 * case 21's inverted deliberate red in `Acceptance.Wall.Catalogue`. The LP's stand
	 * moved x3.128 to 17.2389 — NOT the x4.5 a pure jamb-cohesion pricing predicts, so the
	 * 2026-08-13 attribution (jamb bed cohesive shear, ruled a rigid-plastic scope limit)
	 * is incomplete at the mean data: the zeroed-cohesion probe now keeps 27.7% of lambda*
	 * (see OpeningStrengthProbes), and the mean-basis mechanism split is an open
	 * measurement logged in CURRENT_STATE. The prose block above records the
	 * characteristic-era investigation as it stood; its figures are that era's.
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
 * ====================================================================================
 * RE-MEASURED AT THE MEAN RE-ANCHOR FLIP (2026-08-14) — BOTH HALVES OF THE FINDING
 * ABOVE ARE NOW HISTORY, AND THE REPLACEMENTS ARE PINNED BELOW.
 * ====================================================================================
 *
 *   - THE LP'S HEIGHT-IDENTITY DIED: 629.19922011175072 at ten courses against
 *     297.99734009073632 at twenty — a ratio of 2.1114, within 0.4% of production's own
 *     height-linear 2.1198. At the mean strengths the LP's binding constraint is
 *     strength-governed and scales with the stacked load, so the "some local feature the
 *     two walls share" mechanism the characteristic data exposed (256.820018 at BOTH
 *     heights, one ulp apart) is no longer what binds. The identity is replaced by a
 *     ratio pin; re-deriving WHICH constraint now binds belongs with the step-4
 *     promotion design, not this file.
 *   - PRODUCTION'S CROSSING DIED with the re-anchor's free-end loss (CURRENT_STATE):
 *     the readings are the same per-brick-weight multiples of the /7 anchor
 *     (0.13192716796875004 and 0.27966589311306284 — 15.867 and 33.635 bw on
 *     0.0083148340), the RATIO is untouched, but both now sit under 1.0 and the
 *     crossing sits past any buildable height (~412 courses). The 20-course wall no
 *     longer drops its piece, so both rows read AgreeStands and the ladder keeps
 *     trend + agreement only, exactly as the lost-discrimination entry records.
 *
 * NEEDS A TICKING WORLD: NO.
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

	/*
	 * THE LP'S HEIGHT SCALING, PINNED AS A RATIO — the replacement for the retired
	 * height-identity (header). 629.19922011175072 / 297.99734009073632 = 2.1114255...,
	 * within 0.4% of production's own height-linear 2.1198 and pinned separately from it,
	 * so the two models' agreement about height stays a measured fact rather than a
	 * coincidence nobody would notice breaking.
	 */
	CheckReadingRatio(
		*this,
		TEXT("the LP's free-end lambda* now scales with height (7x10 over 7x20)"),
		Short->Oracle.Lambda, Tall->Oracle.Lambda,
		2.11122, 2.11163);

	/*
	 * PRODUCTION'S SIDE, PINNED AS A RATIO AND A CEILING. The characteristic-era crossing
	 * between these heights is gone (the re-anchor moved it past ~412 courses — the
	 * lost-discrimination entry in CURRENT_STATE); what remains assertable is that the
	 * reading is height-LINEAR (the ratio) and that both heights stand clear of capacity,
	 * which is what AgreeStands above already demands of the outcome.
	 */
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
 * WHAT THEY MEASURED, 2026-08-13 — AT THE CHARACTERISTIC STRENGTHS. Historical record:
 * the 2026-08-14 mean re-anchor moved every lambda* and killed the abutment
 * sensitivity; the note above the rows carries the mean-basis measurements and what
 * they change. The experiment below is preserved as it was run.
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
	 * RE-MEASURED AT THE MEAN RE-ANCHOR FLIP (2026-08-14). Two things moved everywhere at
	 * once, so read every rung below with both in mind. (1) PRODUCTION STANDS the whole
	 * family now — the approved case-21 fallout: every rung's drop pin is 0 and every
	 * relation is AgreeStands (the catalogue's ruled Collapse is the inverted deliberate
	 * red in the acceptance suite, not here). The even-parity rungs (s=2, s=4) newly break
	 * ONE joint in one pass and still drop nothing — their worst reading is 1.0433 against
	 * the odd rungs' 0.9354, on an axis NOBODY HAS DECOMPOSED (see the wall-21 row above:
	 * the compression attribution has been checked twice on this joint shape elsewhere and
	 * was wrong both times). (2) THE LADDERS'
	 * CHARACTERISTIC-ERA FINDINGS ARE HISTORY, NOT CURRENT: at the mean data the parity
	 * step reads 0.8875 (was 0.8731), the matched-span identity 1.00088 (was 0.98968), the
	 * pure span step 1.1385 (span-squared predicts 1.128 — still ~0.9% over), and the
	 * ABUTMENT SENSITIVITY COLLAPSED: j=3/j=2 = 1.0378 (was 1.339), j=4/j=2 = 1.0514 (was
	 * 1.698). The depth-flatness held (s=3/s=1 = 1.0000066, s=4/s=2 = 1 to the last bit).
	 * So on the mean basis the mechanism is span-dominated and nearly abutment-blind —
	 * NOT the jamb-priced chain the characteristic data showed — which is the open
	 * mean-basis mechanism split logged in CURRENT_STATE for the step-4 design. The pins
	 * below hold the mean-basis measurements; the header above records the
	 * characteristic-era experiment as it was run.
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
		*S2, *S1, 0.88748, 0.88758);

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
		*J3Trimmed, *J3, 0.98670, 0.98680);

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
		S2->WorstUtilisation, S1->WorstUtilisation, 1.11531, 1.11543);

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
		W17S1->WorstUtilisation, S1->WorstUtilisation, 0.89065, 0.89076);

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
 * SLICE 0d ADDED A FIFTH ROW — the first-crack flag-on window for case 21 (lambda* falls
 * 12.3% to 15.117 and STANDS, the §4 confirmation that bond bending is minor here). It is
 * the same wall and touches the LP only, so it takes the same rung-size and
 * production-reading-vs-control guards the four probes do, plus its own MOVE ratio (0.8769).
 *
 * COST: five solves at 83 blocks, ~6 s. NEEDS A TICKING WORLD: NO.
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
	 * RE-MEASURED AND RE-DERIVED AT THE MEAN RE-ANCHOR FLIP (2026-08-14). The
	 * characteristic-era finding this row closed — jamb bed cohesion is essentially the
	 * whole capacity, its removal collapses lambda* PAST 1.0 (0.785, a factor of 7.02, the
	 * family's one AgreeFalls) — DOES NOT SURVIVE the mean basis. Zeroing the same 36
	 * joints now leaves lambda* at 4.768, a factor of 0.277: the modified wall STANDS,
	 * because the strengths the probe leaves alone (bond tension 0.70 among them) now
	 * carry it. So on the mean basis the jamb cohesion is the dominant SINGLE term (72% of
	 * the stand) but no longer the whole capacity, the AgreeFalls arm is dead, and the
	 * mean-basis mechanism split — consistent with the abutment ladder's collapse to
	 * ~1.05x — is the open measurement CURRENT_STATE logs for the step-4 design. The
	 * 2026-08-13 ruling (Collapse stands; the LP's cohesion credit booked as a
	 * rigid-plastic scope limit) is unchanged by any of this.
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
	 * SLICE 0d — THE FIRST-CRACK FLAG-ON WINDOW FOR CASE 21, and the §4 confirmation.
	 * PROMOTION_DESIGN Sec 4.2/4.3 argue bond bending is a MINOR term here (the tension
	 * probes above move lambda* only a few percent). This row measures it directly: with the
	 * uncracked first-crack rows on EVERY bonded joint (133 of them — the whole wall), lambda*
	 * falls only 12.3%, to 15.116881825572943, and the wall STILL STANDS. It does NOT flip.
	 *
	 * THIS IS THE POINT PROMOTION_DESIGN Sec 4 MAKES, CONFIRMED RATHER THAN SURPRISED: the
	 * brittleness rule is right AND does not close the case-21 disagreement — the residual is
	 * the non-brittle friction+compression mechanism Slice 0c attributed, which first crack
	 * does not touch. A DISAGREEMENT WITH THE WRITTEN PREDICTION, recorded not smoothed: the
	 * predictions record guessed 6-14 ("up to 3x"); measured it moved just 12.3%, so bond
	 * tension is an even more minor term here than estimated. OverriddenJoints = 133 pins that
	 * every bonded joint carried a row (keyed on data, the whole wall is mortared).
	 *
	 * The window is ~+/-2e-5 relative, matching this wall's four sibling probes above rather
	 * than the tighter reading it happens to land on today: the file's pivot-path discipline
	 * (a re-pin that "cost five re-pins once already") says a first-crack pin on the same
	 * 83-block wall must be no tighter than the controls it sits beside, so a future solver-path
	 * change flaps them together or not at all.
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
	 * THE FIRST-CRACK ROW IS THE SAME WALL (83 blocks) and touched the LP only — production
	 * reads the untouched control, exactly like the strength probes. So it takes the same
	 * two guards: same rung size, and a bit-identical production reading against the control.
	 */
	CheckRungSize(*this, TEXT("probe: first-crack on"), *FirstCrack, 83);

	CheckReadingRatio(
		*this,
		TEXT("the first-crack flag changed the LP only (production reading vs control)"),
		FirstCrack->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);

	/*
	 * THE §4 CONFIRMATION, AS A RATIO: case 21 with first crack on falls to 0.8769 of the
	 * control and STAYS ABOVE 1.0. A panel spanning in brittle bond bending would third
	 * (0.3333); case 21 barely moves, which is the measured statement that bond bending is a
	 * minor term and the residual is friction+compression (Slice 0c), not brittle bond. The
	 * verdict does not flip — the finding PROMOTION_DESIGN Sec 4 set this fixture up to make.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("SLICE 0d: case 21 under first crack STANDS and does not flip"),
		TEXT("a bond-bending panel predicts 0.3333 (/3); case 21 measures 0.8769 and stays ")
		TEXT("above 1.0 — bond bending is minor, the residual is friction+compression"),
		*FirstCrack, *Control, 0.87689, 0.87692);

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
	 * AND THE LOW HALF OF THAT LEVER ON ITS OWN. The four-fold ratio above would also be
	 * satisfied by a solver that had stopped responding to strengths altogether and read
	 * the same number by accident on both sides; this one says the x0.5 rung really does
	 * sit 2.7% under the control, so the knob is connected. Its partner (x2 / control) is
	 * the quotient of these two and is deliberately not pinned a third time.
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
 * ====================================================================================
 * SLICE 0c — ATTRIBUTE CASE 21'S RESIDUAL lambda* = 4.768 TO ITS PHYSICAL CAUSE.
 * ====================================================================================
 *
 * WHAT IS UNDER TEST, IN ONE SENTENCE. Case 21's residual — the lambda* that survives when
 * its jamb bed cohesion is zeroed, 4.768, the number OpeningStrengthProbes pins and nobody
 * has explained — is re-solved with the jamb's FRICTION zeroed, its CRUSHING cap relaxed,
 * both, and with the cohesion removal split into the two bearing courses at the top of the
 * jamb versus the run down to the ground, so the readings say WHICH resistance mechanism
 * holds the residual up.
 *
 * WHY THIS SLICE EXISTS. PROMOTION_DESIGN §4.4 makes attributing the 4.768 a PRECONDITION on
 * promoting the LP to cascade authority: the 2026-08-13 ruling booked case 21's stand to the
 * LP mobilising brittle cohesion along a chain, the mean re-anchor refuted that (zeroing the
 * cohesion OUTRIGHT still stands, at 4.768 — the discount family is dead), and "nobody can
 * say what the remaining 4.768 is" is not a state in which to hand the LP a verdict. §4.2
 * asserts the residual is "friction and compression, which are not brittle"; this test is
 * the measurement that either confirms that or refutes it, in exactly the shape §4.4 names —
 * three probe rows plus the chain-vs-bearing split, one afternoon, on the 83-block fixture.
 *
 * THE BRITTLENESS PRECONDITION IS ALREADY SETTLED and this test does not re-litigate it: the
 * cohesion "discount" was refuted by measurement (OpeningStrengthProbes' own re-derivation),
 * so the residual is NOT bond/cohesion. What holds it up instead is the open question, and
 * these four measurements are how it is closed.
 *
 * ------------------------------------------------------------------------------------
 * THE PREDICTIONS, AND WHAT THE RUN MEASURED (the mean-re-anchor derivation-record discipline:
 * the guesses are KEPT beside their refutations, because the disagreement IS the finding)
 * ------------------------------------------------------------------------------------
 *
 * The residual state is jamb bed cohesion = 0. On that state the jamb bed joints' only
 * remaining shear capacity is Coulomb friction min(mu*sigma, cap) — cohesion is gone. The
 * profile is mean GeneralPurposeMortar: mu = 0.75, cap = 2.0 MPa, crushing = 10 MPa,
 * tension = 0.7 MPa. At self-weight the bed compression is ~0.018 MPa, so friction buys
 * ~0.0135 MPa of shear and crushing (10 MPa) is ~500x clear of any demand a residual solve
 * makes. THE DESIGN'S HYPOTHESIS (§4.2) — that friction and compression, both non-brittle,
 * carry the residual — is CONFIRMED: with cohesion gone the residual stands on a no-tension,
 * friction-mobilised compression mechanism with no brittle bond, and crushing is inert (probe
 * 2). The friction probe does NOT expose that mechanism's ceiling; it unlocks a DIFFERENT,
 * higher, bond-tension mechanism, which probe 5 then attributes by measurement:
 *
 *   PROBE 1 (friction also zeroed). PREDICTED: falls (lambda* < 1.0), on the theory that a
 *     cohesionless bed joint's only shear is friction and a thrust chain with no shear path
 *     has nothing left. MEASURED: lambda* RISES to 9.9349181343793767 and STANDS —
 *     AgreeStands, not OracleFalls. THE FINDING: setting mu=0 is NOT the removal of a cap on
 *     the frictional residual. With cohesion already gone it forbids ALL bed-joint shear
 *     (the Coulomb row |v| <= mu*n collapses to |v| <= 0) and in the SAME move lifts the
 *     coupling (n+ >= n-) that had held the joints in net compression, so the 0.70 MPa
 *     flexural tensile bond — which this probe leaves INTACT; it zeroed Coulomb cohesion, not
 *     the tensile bond — becomes usable and the wall re-forms on a DISTINCT bond-tension
 *     mechanism that stands HIGHER (ratio 9.9349181343793767 / 4.7679865256023284 =
 *     2.0836716045719785). The rise is friction disallowing shear and thereby UNLOCKING a
 *     bond-tension mechanism, NOT friction capping a compression arch. Provable: any mu=0
 *     optimum in net compression everywhere would be feasible for the mu=0.75 control and so
 *     bounded by 4.768; that 9.935 > 4.768 therefore means the mu=0 optimum mobilises net
 *     TENSION on at least one jamb bed joint — and probe 5, removing that bond, collapses it.
 *
 *   PROBE 2 (crushing relaxed). PREDICTED and CONFIRMED unchanged: lambda* =
 *     4.7679865256023373, bit-identical to the control's 4.7679865256023284 (rel diff
 *     1.86e-15). Crushing (10 MPa) sits ~500x clear of demand and does not bind. Asserted as
 *     a same-number identity against the control, which would FAIL if crushing were binding
 *     (relaxing a binding cap can only RAISE lambda*). Crushing is inert.
 *
 *   PROBE 3 (both). PREDICTED and CONFIRMED equal to probe 1: lambda* =
 *     9.9349181343793749 = probe 1's 9.9349181343793767 (rel diff 1.79e-16). Once friction
 *     is gone the bed joints carry no shear whatever the crushing cap, so relaxing crushing
 *     on top changes nothing. Asserted as an identity against probe 1.
 *
 *   THE CHAIN-VS-BEARING SPLIT (cohesion zeroed in one band at a time). PREDICTED: the run
 *     to ground is the tighter link (lambda*_run <= lambda*_bearing), because the mechanism
 *     was found to run the full jamb chain to the foundation (thrust is not local —
 *     CURRENT_STATE, the abutment ladder). MEASURED: THE OPPOSITE. Bearing courses only
 *     (18 joints) reads 4.7681626950893188 — essentially the whole residual — while run to
 *     ground only (18 joints) reads 5.9623697933567943. So bearing (4.76816) < run to ground
 *     (5.96237): removing cohesion in the BEARING courses collapses the residual MORE. THE
 *     FINDING: for the COHESION contribution specifically, the bearing courses at the top of
 *     the jamb are the tighter link, NOT the run to ground — this refutes the "thrust runs
 *     to ground" prediction for cohesion. (Friction is a genuine carrier of the residual —
 *     part of the no-tension compression+friction mechanism the control stands on — while
 *     cohesion localises to the bearing; the two mechanisms are distinct and that is the
 *     whole point of keeping the attributions apart.)
 *
 *   PROBE 5 (friction AND jamb-bed tension both zeroed). THE PROOF THAT 9.935 IS BOND
 *     TENSION. Probe 1 left the 0.70 MPa tensile bond intact; probe 5 zeroes it too, so with
 *     both shear (mu=0) and tension (bond=0) forbidden on the jamb bed joints, the wall can
 *     no longer use the bond-tension mechanism. PREDICTED: lambda* COLLAPSES from 9.935 back
 *     to at-or-below the frictional residual — provably <= 4.768, because a bed joint carrying
 *     only compression (no shear, no tension) is a strict capacity subset of the mu=0.75
 *     control, whose optimum bounds it. MEASURED: lambda* = 3.7471595015086909 — it collapsed
 *     to 3.747, BELOW even the control (the control still had friction-shear this probe took
 *     away), so the prediction held and then some. This is the empirical companion to probe 1's
 *     inequality: probe 1 proves the mu=0 optimum needs net tension somewhere; probe 5 removes
 *     the bond that supplies it and watches the mechanism disappear.
 *
 * EVERY WINDOW BELOW IS NOW A MEASURED PIN at this file's bit-window discipline (~2e-5
 * where one path produces the number). The two same-number identities (crushing vs control,
 * both vs friction-alone) are the proof crushing is inert; the friction-raises-lambda
 * relation is the headline and is asserted as a mechanism inequality, not only as two
 * floats, so a solver that merely reproduced the numbers by accident could not satisfy it.
 *
 * NO NEW ORACLE SEAM. Like OpeningStrengthProbes, every probe rewrites FOracleProblem after
 * the bridge and before the solve; RigidBlockOracle.h/.cpp is untouched, which is what keeps
 * the oracle an independently derived second opinion. A contact-force decomposition at the
 * chain joints would have needed a per-joint force seam the result does not expose — so the
 * chain-vs-bearing split is measured by SPATIAL strength probes instead, which the design
 * (§4.4, and the cohesion probe's own "splitting it is one more row of the same shape")
 * names as the clean way to ask the question without one.
 *
 * COST: seven solves at 83 blocks, ~8 s — the reason this is in OracleSweepFast and not the
 * default suite (case 21's full solve is ~1.1 s and the default suite budget is ~30 s).
 * NEEDS A TICKING WORLD: NO — the oracle and the bridge are pure functions.
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
	 * THE RESIDUAL CONTROL. This is not the as-built wall (17.24) — it is case 21 with its
	 * jamb bed cohesion already gone, the 4.768 state whose cause this whole test attributes.
	 * Every probe below is read against THIS. Its value is the one OpeningStrengthProbes pins,
	 * so this row is a KNOWN anchor, not a prediction.
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
	 * PROBE 1 — FRICTION ALSO ZEROED. THE HEADLINE. PREDICTED falls; MEASURED it RISES and
	 * STANDS at 9.9349181343793767 — but NOT because friction was capping a compression arch.
	 * With cohesion already gone, mu=0 forbids all bed-joint shear (|v| <= mu*n becomes |v| <=
	 * 0) and simultaneously lifts the n+ >= n- coupling that had held the joints in net
	 * compression, so the 0.70 MPa flexural tensile bond (left intact — this zeroed Coulomb
	 * cohesion, not the tensile bond) becomes usable and the wall re-forms on a DISTINCT,
	 * higher, bond-tension mechanism (2.08x). The window is a bit-window on the single measured
	 * value; the mechanism inequality (friction-off lambda* > residual control) is asserted
	 * below, and probe 5 is the measurement that names the higher mechanism as bond tension.
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
	 * PROBE 2 — CRUSHING RELAXED. PREDICTED and MEASURED unchanged: lambda* =
	 * 4.7679865256023373, bit-identical to the control (rel diff 1.86e-15). Crushing (10 MPa)
	 * is ~500x clear of the ~0.02-0.09 MPa a residual solve ever asks, so removing the cap
	 * does not move lambda*. The window is the control value and the same-number identity
	 * below is the real claim — it FAILS if crushing were binding, because relaxing a binding
	 * cap can only raise lambda*. Crushing is inert.
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
	 * PROBE 3 — BOTH. PREDICTED and MEASURED equal to probe 1: lambda* =
	 * 9.9349181343793749 = probe 1's 9.9349181343793767 (rel diff 1.79e-16). With friction
	 * gone the bed joints carry no shear whatever the crushing cap, so relaxing crushing on
	 * top changes nothing — the residual stands at probe 1's raised value, NOT a fall. Window
	 * mirrors probe 1's bit-window; the real claim is the same-number identity against probe
	 * 1 below, which FAILS if crushing relaxation revives a friction-killed residual.
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
	 * THE CHAIN-VS-BEARING SPLIT. The full cohesion removal (17.24 -> 4.768) is here split by
	 * height: cohesion zeroed only in the two bearing courses at the top of the jamb, then
	 * only in the run down to the ground. Each band touches 18 of the whole chain's 36 bed
	 * joints (a measured, pinned partition — 18 + 18 = 36). MEASURED: the bearing band drops
	 * lambda* to 4.7681626950893188 — essentially the whole residual — while the run-to-ground
	 * band only reaches 5.9623697933567943, so the BEARING courses are the tighter link for
	 * cohesion. This refutes the "thrust runs to ground" prediction for the COHESION term
	 * (friction is a genuine carrier of the residual — see probe 1's finding — and localises
	 * differently from cohesion; the two attributions stay distinct).
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
	 * PROBE 5 — FRICTION AND THE JAMB-BED TENSILE BOND BOTH ZEROED. THE EMPIRICAL PROOF THAT
	 * 9.935 IS BOND TENSION. Probe 1 zeroed the Coulomb mu but left the 0.70 MPa flexural
	 * tensile bond intact, and the wall re-formed on that bond at 9.935. This row zeroes BOTH
	 * on the same 83-block jamb chain: with shear forbidden (mu=0) AND tension forbidden
	 * (bond=0), the bed joints carry compression only, so the bond-tension mechanism cannot
	 * form. PREDICTED (derivation-record shape, guess kept beside its refutation): lambda*
	 * COLLAPSES from 9.935 back to at-or-below the frictional residual, provably <= 4.768 + a
	 * small margin — a compression-only bed joint is a strict capacity subset of the mu=0.75
	 * control, whose optimum (4.768) bounds it from above. MEASURED (2026-08-21): lambda* =
	 * 3.7471595015086909 — it collapsed to 3.747, not merely to the control but BELOW it (the
	 * control still had friction-shear this probe removed), confirming the prediction and the
	 * bond-tension attribution. The window below is now a bit-window on that measured value;
	 * the collapse is also asserted as a strict inequality against probe 1's 9.935 (finding 5).
	 * NO CLOSED FORM: pinning 9.935 as 0.70 MPa x a bed-joint tension area needs a per-joint
	 * contact-force seam the oracle result does not expose (see this test's "NO NEW ORACLE
	 * SEAM" note), so the ceiling stays a measured number window rather than an identity.
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

	/* All seven rows are one wall — all seven must bridge to the same 83 blocks. */
	CheckRungSize(*this, TEXT("residual control"), *Control, 83);
	CheckRungSize(*this, TEXT("probe 1 friction off"), *FrictionOff, 83);
	CheckRungSize(*this, TEXT("probe 2 crushing relaxed"), *CrushingOff, 83);
	CheckRungSize(*this, TEXT("probe 3 both"), *Both, 83);
	CheckRungSize(*this, TEXT("split bearing"), *Bearing, 83);
	CheckRungSize(*this, TEXT("split run to ground"), *RunToGround, 83);
	CheckRungSize(*this, TEXT("probe 5 friction and tension off"), *FrictionAndTensionOff, 83);

	/*
	 * THE OVERRIDE TOUCHES THE LP ONLY. Production reads the untouched wall on every row, so
	 * every production reading must equal the control's — a moved reading would mean the
	 * fixture moved and the LP's number is uninterpretable.
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
	 * THE SPLIT SELECTOR IS HONEST: the two bands partition the whole chain — their joint
	 * counts sum to the 36 the control touched, and neither is empty. Each band's own count
	 * (18) is now pinned in its row above; this cross-check is the complementary statement
	 * that the two are a partition, catching a band that matched nothing or a pair that
	 * overlapped.
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
	 * FINDING 1 — CRUSHING DOES NOT BIND THE RESIDUAL. The identity is a strictly tighter
	 * claim than probe 2's window: relaxing a NON-binding cap leaves lambda* bit-stable,
	 * relaxing a binding one raises it. If this holds, compression-crushing is not what holds
	 * the residual up.
	 */
	CheckSameLambda(
		*this,
		TEXT("crushing relaxed vs the residual control"),
		*CrushingOff, *Control, 1.0e-6);

	/*
	 * FINDING 2 — CRUSHING CANNOT REVIVE A FRICTION-KILLED RESIDUAL. Probe 3 must read
	 * probe 1: once friction is gone the bed joints carry no shear whatever the crushing cap.
	 * A break here would say the two mechanisms interact, which nothing predicts.
	 */
	CheckSameLambda(
		*this,
		TEXT("both (friction off + crushing relaxed) vs friction off alone"),
		*Both, *FrictionOff, 1.0e-6);

	/*
	 * FINDING 3 — ZEROING FRICTION UNLOCKS A DISTINCT, HIGHER BOND-TENSION MECHANISM. THE
	 * HEADLINE, asserted as a mechanism inequality rather than only as two floats: zeroing
	 * friction on the whole cohesionless chain RAISES lambda* (4.768 -> 9.935). This is NOT
	 * friction being a binding cap on a compression arch. With cohesion already gone, mu=0
	 * forbids all bed-joint shear (|v| <= mu*n collapses to |v| <= 0) AND in the same move
	 * lifts the n+ >= n- coupling that had held the joints in net compression, so the 0.70 MPa
	 * flexural tensile bond (which this probe leaves intact — it zeroed Coulomb cohesion, not
	 * the tensile bond) becomes usable and the wall re-forms on a DIFFERENT, bond-tension
	 * mechanism that stands higher. The rise is friction disallowing shear and thereby
	 * unlocking that mechanism, not friction capping an arch. Provable: any mu=0 optimum in net
	 * compression everywhere would be feasible for the mu=0.75 control and so bounded by 4.768,
	 * so 9.935 > 4.768 means the mu=0 optimum mobilises net TENSION on at least one jamb bed
	 * joint. A solver that had merely reproduced the two window numbers by accident could still
	 * fail this strict inequality. This CONFIRMS PROMOTION_DESIGN §4.2: the residual itself (the
	 * control) is the non-brittle no-tension compression+friction mechanism, and probe 5 (below)
	 * measures that the higher 9.935 is carried by the brittle bond, not by the residual.
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
	 * FINDING 5 — THE 9.935 IS BOND TENSION: REMOVE THE BOND AND IT COLLAPSES. THE EMPIRICAL
	 * COMPANION TO FINDING 3. Probe 1 proved (by inequality) that the mu=0 optimum must mobilise
	 * net tension somewhere; probe 5 zeroes the 0.70 MPa jamb-bed tensile bond ON TOP of mu=0
	 * and measures the mechanism disappear. With both bed-joint shear (mu=0) and tension
	 * (bond=0) forbidden, the joints carry compression only — a strict capacity subset of the
	 * mu=0.75 control — so lambda* MUST fall from probe 1's 9.935 to at-or-below the control's
	 * 4.768. Asserted two ways: strictly below probe 1 (the bond-tension mechanism is gone), and
	 * within a small margin at-or-below the control (it returned to the frictional residual's
	 * ceiling). Reported as an info line first so the value is in the log regardless of verdict.
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
	 * FINDING 4 — FOR COHESION, THE BEARING COURSES ARE THE TIGHTER LINK, NOT THE RUN TO
	 * GROUND. Predicted the reverse (thrust runs the full jamb chain to the foundation, so
	 * the run to ground should govern); MEASURED the opposite. Removing cohesion in the two
	 * bearing courses at the top of the jamb collapses the residual essentially all the way
	 * (4.76816), while removing it in the run to the foundation leaves more standing
	 * (5.96237). So bearing lambda* < run-to-ground lambda*. This refutes the "thrust runs to
	 * ground" prediction for the COHESION contribution specifically — friction (finding 3) is a
	 * genuine carrier of the residual and localises differently, so the two attributions stay
	 * distinct. Reported as an info line first so the two band values are in the log regardless
	 * of the verdict.
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
 *   (b) A BASIS PIVOTED INTO SINGULARITY — the 99-block rung, and a 218-block 12x22 wall
 *       (recorded here as "case 22's own", which was true at the CHARACTERISTIC strengths
 *       these readings were taken at; the 2026-08-14 mean re-anchor moved the data and
 *       case 22 has answered ever since — Slice 0a, 2026-08-15). A column nearly dependent
 *       on the basis is accepted on a ratio-test
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
 * reached — CURRENT_STATE books that as the residual mode. It was ALSO booked here as a
 * live candidate for case 22's refusal, and Slice 0a refuted that twice over: case 22 does
 * not refuse, and across the entire covered-opening sweep the seam fired on NO SOLVE. It is
 * unreached rather than sound, and nothing today can reach it.
 *
 * THE 218-BLOCK RUNG IS DELIBERATELY NOT ASSERTED HERE (143-275 s a solve). It refused
 * after this fix at the characteristic strengths; at today's mean strengths it answers, as
 * does case 22's own 371-block wall (lambda* = 8.4149459982219277, 88,810 pivots). Also
 * recorded so nobody re-measures it: the fallback EARNS ITS KEEP on that rung — without it
 * the pivot path runs past the 100,000-pivot cap (275 s, no answer) where with it the run
 * terminates in 143 s — so nothing here argues for deleting it. And a hazard that outlived
 * the refusal: case 22 now answers at 88.8% of that same cap, with nothing watching it.
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
		 * The midpoint-of-two-certified-readings window, +/-2e-5 relative, on a rung that
		 * has been solved by two different pivot paths. Both zero where no such pair
		 * exists — the neighbours have one certified reading each, and the j=4 rung's pair
		 * is pinned in OpeningMechanismLadders rather than duplicated here.
		 */
		double CertifiedLo = 0.0;
		double CertifiedHi = 0.0;
	};

	/*
	 * MEAN RE-ANCHOR RE-PINS (2026-08-14): every window re-measured at the mean-basis
	 * profiles — the sanity windows keep their ~±1% looseness around the new readings, and
	 * the 99er's certified window is ONE certified reading at ±2e-5 relative
	 * (89.898933555823774; the second-path pair predates the flip and a re-pair is owed
	 * with the next pivot-path change). The MUST-ANSWER halves and the bracket
	 * (107er strictly below the 5-cell neighbour: 18.1243 < 18.1419) held unchanged.
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

/**
 * ====================================================================================
 * A REFUSAL MUST NAME WHICH TERMINATION PRODUCED IT — slice 0a's first red.
 * ====================================================================================
 *
 * WHAT IS UNDER TEST, IN ONE SENTENCE. When `SolveRigidBlock` refuses, the result must
 * carry a machine-readable reason that DISTINGUISHES the terminations it can refuse from —
 * a hit iteration cap, a spurious unbounded ray and a numerical failure being three
 * different events with three different fixes — rather than collapsing them, as today, into
 * the one sentence "phase-2 simplex failed".
 *
 * WHY IT IS A BEHAVIOUR AND NOT A COMMENT. That collapse has cost two instrumented builds
 * inside a week: the 2026-08-13 phase-2 diagnosis needed one to tell mode (a) from mode (b)
 * on the rungs above, and slice 0a needed a second to find out which arm this family reaches
 * now. It also blocks the promotion design's own risk experiment — §11 R4's cheapest test is
 * "a run of the whole catalogue at production scale COUNTING REFUSALS BY REASON, once
 * `WhyNot` distinguishes them", and §5.6 requires production to count fallbacks by reason as
 * a quality metric. A sentence is a poor thing to count by, which is why the reason is an
 * enumerator and the sentence is derived from it rather than the other way round.
 *
 * ------------------------------------------------------------------------------------
 * THE ASSERTION, AND WHY IT IS SHAPED THIS WAY
 * ------------------------------------------------------------------------------------
 *
 * DISTINCTNESS IS ASSERTED OVER THE WHOLE TAXONOMY AND NEEDS NO FIXTURE. Only ONE of the six
 * refusing arms is reachable from a fixture this project owns — validation — so a test built
 * only out of fixtures could not say anything at all about the arms the diagnosis actually had
 * to tell apart. The pairwise-distinct sweep over the enumerators says it for all six, costs
 * nothing, and is the assertion a constant string cannot satisfy.
 *
 * AND THE TAXONOMY IS TIED TO REALITY BY THE FIXTURES BESIDE IT, because a set of distinct
 * strings nobody ever reports is worth as little as one string reported for everything. Each
 * fixture row pins the reason it must produce and cross-checks it against something
 * observable: the answering rung must report None with an empty sentence, and the poisoned
 * problem must report InvalidProblem *and print its phrase*, which is the check that the
 * enumerator and the sentence are one statement rather than two that can drift apart.
 *
 * THE PHASE-2 ROW WAS A PINNED CANARY, IT FIRED THE DAY IT WAS WRITTEN, AND IT IS GONE
 * (2026-08-15). It solved the 8-cell / 128-block wall below and pinned it REFUSING with
 * PhaseTwoNumericalFailure, under the standing instruction that the day the solver answered
 * it the row must be re-pointed at whatever still refuses, or deleted if nothing does. Its
 * sibling `PhaseTwoMustNotRefuseTheCoveredOpeningFamily` was made green the same day (the
 * ratio test's `RelativePivotTol` 1e-11 -> 1e-9), the canary fired exactly as designed, and
 * the check its instruction demanded before deletion was run: **no fixture this project owns
 * reaches any phase-2 arm now**. The whole `OracleSweep` group answers every row, and the
 * one fixture that could reach an unbounded ray — the unbreakable lambda-cap tower — is
 * bounded by the cap row and terminates Optimal. So the row was deleted rather than weakened
 * into "it answers now", which would have cost 75 s of solve to assert what the control above
 * already asserts in a second.
 *
 * WHAT THAT COSTS, RECORDED RATHER THAN GLOSSED: the three PhaseTwo enumerators and
 * PhaseOneFailure and VerificationFailure are now proven DISTINCT but are reported by no
 * fixture, so nothing here would catch a site that set the wrong one of them. CURRENT_STATE
 * carries the owed replacement — a fixture that genuinely reaches a phase-2 arm, which the
 * LU/eta factorisation fuzz specified there is the natural home for, since it is also the only
 * planned exerciser of `ESimplexEnd::NumericalFailure`.
 *
 * ------------------------------------------------------------------------------------
 * WHAT WAS MEASURED, 2026-08-15, AND WHAT IT CONTRADICTED
 * ------------------------------------------------------------------------------------
 *
 * CURRENT_STATE proposed the unbreakable lambda-cap fixture and the case-22 wall as "the two
 * fixtures that reach different arms". The first half is WRONG and the measurement says so:
 * the cap row BOUNDS that LP, so the unbreakable tower terminates Optimal at lambda* =
 * LambdaCap and refuses nothing at all (`Oracle.RigidBlock.ValidationCatalogue` pins it as an
 * ANSWER). It reaches an unbounded ray only under mutation M5, with the cap row deleted, and
 * a mutation is not a fixture.
 *
 * WHAT THAT FAMILY REACHED, instrumented and then reverted: phase 2 returned NumericalFailure,
 * from the periodic refactorisation inside the pivot loop, because `Factorise` found an LU
 * pivot under SingularPivotTol in the last few columns of the basis:
 *
 *     8-cell opening (128 blocks) : position 4018/4021, col 6119, pivot 4.3758616521949456e-12
 *     16-cell opening (200 blocks): position 6322/6325, col 9639, pivot 4.928959752317684e-12
 *
 * NOT the spurious-unbounded arm (no solve in the family reached it) and NOT NB-7 (the
 * artificial pivot-out pass took no pivot under 1e-6 absolute or 1e-7 relative on either
 * wall). Both recorded suspects were refuted; the diagnosis, and the repair that closed it the
 * same day, are in `PhaseTwoMustNotRefuseTheCoveredOpeningFamily`'s header. When this test
 * pinned the 8-cell wall's refusal it reported reason 5 — PhaseTwoNumericalFailure — which is
 * the instrumented finding independently reproduced by the shipped taxonomy, and the only
 * reading of a phase-2 arm this suite has ever taken.
 *
 * COST: two solves under a second each, plus the taxonomy sweep, which needs none.
 * NEEDS A TICKING WORLD: NO — producers and arithmetic, no cascade and no UWorld.
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

	/*
	 * The builder's reason is read into a local BEFORE the message is built: folding the
	 * call into the Printf's argument list is unsequenced, and the message would be made
	 * from the empty string the reason had before the call ran (TRAPS records this).
	 */
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
 * ====================================================================================
 * THE EIGHT-COURSE-COVER FAMILY STILL REFUSES A BOUNDED, FEASIBLE PROBLEM — AND IT IS
 * NOT THE WALL THE RECORD NAMES.
 * ====================================================================================
 *
 * WHAT IS UNDER TEST, IN ONE SENTENCE. An 8-cell opening under eight courses of cover —
 * 128 blocks, SMALLER than the 137- and 218-block members of its own family that answer
 * beside it in this very run — must return an answer rather than "phase-2 simplex failed",
 * and the answer must land between the two ladder neighbours that bracket it.
 *
 * THIS IS THE SAME DEFECT AS `PhaseTwoMustNotRefuseABoundedProblem`'S, ONE NOTCH DOWNSTREAM,
 * AND THAT TEST IS GREEN. The 2026-08-13 fix (`RelativePivotTol`, a ratio-test pivot floor of
 * 1e-11 of the entering column's own largest magnitude) closed the 99- and 107-block rungs
 * and they stay closed; a floor of 1e-11 is not enough for this family. Its header carries
 * the full evidence chain for the earlier round and is the thing to read first; this one
 * records only what is new.
 *
 * ------------------------------------------------------------------------------------
 * THE RECORD WAS STALE, AND THE MEASUREMENT THAT SAYS SO (2026-08-15)
 * ------------------------------------------------------------------------------------
 *
 * CURRENT_STATE, PROMOTION_DESIGN §6/§10 and `WallAcceptanceTest`'s case-22 block all name
 * the refusing fixtures as the 218-, 290- and 371-block members of this family, measured
 * 2026-08-13. Those measurements were taken at the CHARACTERISTIC strengths, one day before
 * the mean re-anchor changed every number in the LP's data. Re-measured at today's profiles,
 * the whole family sweeps like this (opening cells between two-cell jambs, eight courses of
 * cover, one course below — the whole family in one run):
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
 * SO ALL THREE WALLS THE RECORD NAMES ANSWER TODAY — including acceptance case 22 itself,
 * the 35-cell opening at 371 blocks and 900 joints. The blockers are TWO ISOLATED HOLES at
 * 128 and 200 blocks, sitting inside a smooth monotone curve their own neighbours certify
 * from both sides. That is a stronger statement of the defect than the stale record made,
 * not a weaker one: 128 blocks is a THIRD of the size at which the refusal was thought to
 * start, and sits well inside any region the promotion design's §5.3 sandwich could use.
 *
 * ONE THING THE SWEEP FOUND THAT IS NOT THIS DEFECT AND MUST NOT BE READ AS FIXED WITH IT:
 * case 22 answers after 88,810 pivots against a MaxPivots of 100,000, i.e. at 89% of the
 * termination cap. A pivot-path change of any kind could put that wall over the cap and
 * into a DIFFERENT refusal. CURRENT_STATE carries it.
 *
 * WHY THE PROBLEM IS BOUNDED AND FEASIBLE, so that refusing is a defect rather than
 * correctness. Bounded: the lambda-cap row bounds every problem this oracle assembles.
 * Feasible: lambda = 0 with every contact force zero satisfies a gravity-live problem's
 * equalities, and the two neighbours either side of the hole are answered at lambda* = 190.5
 * and 127.7 by the same solver in the same run — a family whose members vary continuously in
 * one dimension does not contain an infeasible island at 8 cells between feasible ones at 7
 * and 9.
 *
 * ------------------------------------------------------------------------------------
 * THE ARM, MEASURED — AND BOTH RECORDED SUSPECTS ARE REFUTED
 * ------------------------------------------------------------------------------------
 *
 * Instrumented 2026-08-15, then reverted. Phase 2 returns NumericalFailure, from the periodic
 * refactorisation inside the pivot loop: `Factorise` finds an LU pivot at or under
 * SingularPivotTol = 1e-11 in the last few columns of the basis and refuses it.
 *
 *     8-cell opening (128 blocks) : position 4018 of 4021, col 6119, pivot 4.3758616521949456e-12
 *    16-cell opening (200 blocks) : position 6322 of 6325, col 9639, pivot 4.928959752317684e-12
 *
 * Both refusals are bit-reproducible across runs, and both land within three columns of the
 * end of the factorisation.
 *
 *   - THE SPURIOUS-UNBOUNDED ARM IS NOT REACHED. CURRENT_STATE records it as the first live
 *     candidate for this family. Instrumentation at the `Leaving == INDEX_NONE` seam fired
 *     on NO solve in the whole sweep above. It remains unreached-rather-than-sound, and this
 *     family is not the fixture that would license reinstating its repair.
 *   - NB-7 IS NOT IT EITHER. The artificial pivot-out pass was instrumented to print any
 *     pivot under 1e-6 absolute or 1e-7 of its own column's largest magnitude — the exact
 *     defect class the 2026-08-13 fix closed one level up — and printed nothing on either
 *     refusing wall. Its absolute `PivotTol` scan is still worth tightening on its own
 *     merits, but it is not what refuses these two walls.
 *
 * ------------------------------------------------------------------------------------
 * TWO ONE-CONSTANT EXPERIMENTS THAT BOTH REACH THE ANSWER — SO THE ANSWER EXISTS
 * ------------------------------------------------------------------------------------
 *
 * Run 2026-08-15 on both refusing walls, each a single constant changed and then reverted.
 * NEITHER IS PROPOSED AS THE FIX; they are what turns "the LP ought to be answerable" into
 * "here is the answer, twice, by two different pivot paths".
 *
 *   RelativePivotTol 1e-11 -> 1e-9 (the ratio test's floor, one notch tighter than the
 *   2026-08-13 fix):  128 blocks answers lambda* = 155.63200561101226 in 77 s / 21,394
 *   pivots; 200 blocks answers 43.132916253688222 in 237 s / 34,825 pivots.
 *
 *   RefactoriseEvery 64 -> 16 (the basis rebuilt four times as often, the ratio test
 *   untouched):  128 blocks answers 155.63200342392039 in 192 s / 20,606 pivots; 200
 *   blocks answers 43.132560867194137 in 745 s / 34,272 pivots.
 *
 * THE TWO AGREE TO 1.4e-8 AND 8.2e-6 RELATIVE, on visibly different pivot paths (20,606 vs
 * 21,394 pivots), and both cleared the 1e-6 post-solve verification gate. That pair is what
 * licenses the certified windows below on a fixture the shipped solver has never solved.
 * The 128-block reading also lands on the family's own L^-2.4 interpolation between its
 * neighbours (~156) — a third derivation, from the physics rather than the solver.
 *
 * WHAT THE PAIR SAYS ABOUT THE CAUSE, AND WHAT IT DOES NOT. Both levers attack the same
 * thing from opposite ends: one refuses weaker pivots into the basis, the other stops the
 * eta file accumulating as much error before the basis is rebuilt. Their agreement says the
 * basis is being driven into LU-singularity by ARITHMETIC OF THE SOLVER'S OWN MAKING and not
 * by anything in the wall. Which lever is the right fix is deliberately left open: a third
 * reading is that the FACTORISATION's fixed column order is the weak link — it pivots left
 * to right through the basis with no fill-reducing or stability-driven ordering, which would
 * explain why both failures land within three columns of the end — and that points at the
 * Markowitz ordering already on the roadmap. The window and the bracket below are what stop
 * a fix that merely stops refusing.
 *
 * WHAT THE FIX MUST NOT DO, carried over verbatim from the sibling test because every clause
 * still applies: it must not reach the answer by loosening the post-solve verification gate;
 * it must not remove the iteration cap, which is the termination proof; and it must not scale
 * the OPTIMALITY tolerance by ||y||, which would report a lambda* too LOW that verification
 * would certify happily.
 *
 * ------------------------------------------------------------------------------------
 * A WINDOW AND A BRACKET, AND WHY BOTH
 * ------------------------------------------------------------------------------------
 *
 * THE WINDOW IS +/-1e-4 RELATIVE, TEN TIMES THIS FILE'S USUAL 2e-5, and the looseness is
 * deliberate rather than lazy: the two certified readings came from two DIAGNOSTIC variants,
 * not from the fixed solver, so whatever pivot path the real fix takes is a third path this
 * pair has not sampled — and the file's own PARTIAL-PRICING RE-PIN note records lambda* at
 * 100+ blocks as reproducible only to ~1e-5 across paths. 1e-4 is ten times that and still
 * four orders tighter than the bracket, so it can catch a wrong answer without flapping on
 * a right one. Tighten it to 2e-5 once the fixed solver has produced a reading of its own.
 *
 * THE BRACKET SAYS SOMETHING THE WINDOW CANNOT, which is why it sits beside it rather than
 * being replaced by it: lambda* falls monotonically with the opening across every rung of
 * this family, so an 8-cell opening cannot be stronger than a 7-cell one nor weaker than a
 * 9-cell one — a statement about masonry, closed by two neighbours measured in THIS run by
 * THIS binary, which is what stops a number carried in from another run doing the work.
 *
 * THE PARITY CONFOUND IS PRESENT AND MEASURED SMALL. TRAPS records that running-bond opening
 * families interleave two fixtures by course parity; here the cut's COURSES are fixed at
 * 1..3 and only the opening's width alternates parity, and the sweep above shows the odd
 * rungs (5, 7, 9) sitting smoothly among the even ones with no zig-zag at all. The bracket
 * spans one cell either side, which is a factor of 1.49 wide — three orders of magnitude
 * more than any parity step could be.
 *
 * AND EVERY RUNG CARRIES A BLOCK-AND-JOINT-COUNT PIN, per TRAPS: a lambda window and a
 * bracket both pass on a rung that quietly built its neighbour's wall, and only a size pin
 * caught the recorded rung-flip.
 *
 * THE SECOND REFUSER (16 cells, 200 blocks) CARRIES A WINDOW BUT NO BRACKET, because its
 * own neighbours cost 166 s and 255 s and the bracket's value — that a number carried in
 * from another run is not doing the work — is already bought once by the 8-cell trio. Two
 * certified readings 8.2e-6 apart are what carry it, and it is here because one refusing
 * fixture would leave the defect looking like an accident of one wall.
 *
 * COST: four solves, ~200 s (52 + 40 + 90 + 18 while the two refusers refuse early; expect
 * ~420 s once they answer, which is the price of the fix being verified rather than claimed).
 * NEEDS A TICKING WORLD: NO.
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
		 * The midpoint of TWO CERTIFIED READINGS taken by different pivot paths, +/-1e-4
		 * relative. Both zero where no such pair exists. See the header for the two
		 * diagnostic variants that produced them and for why the window is 10x this file's
		 * usual 2e-5 rather than equal to it.
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

	/*
	 * THE NEIGHBOURS FIRST: they answer today, and their loose sanity windows are what stop
	 * the bracket below being satisfiable by three pieces of garbage.
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
				TEXT("[%.6g, %.6g] — a deliberately slack floor under the bracket, a thousand ")
				TEXT("times wider than the ~1e-5 a changed pivot path moves it"),
				Rung.Name, Results[Index].Lambda, Rung.SanityLo, Rung.SanityHi),
			Results[Index].Lambda >= Rung.SanityLo && Results[Index].Lambda <= Rung.SanityHi);
	}

	/* --- THE DEFECT ITSELF ----------------------------------------------------------- */

	/*
	 * EVERY REFUSER MUST ANSWER, AND MUST ANSWER THE NUMBER TWO INDEPENDENT PIVOT PATHS
	 * ALREADY AGREED ON. The window is what stops a "fix" that merely stops refusing: a
	 * solver reaching a different optimum fails this as loudly as one that refuses.
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
	 * AND THE BRACKET BESIDE THE WINDOW, which the sibling test's header argues for and
	 * which is a different statement: the window says what the number is, the bracket says
	 * what the PHYSICS allows it to be, closed by two neighbours measured in this same run
	 * by this same binary rather than carried in from another.
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
