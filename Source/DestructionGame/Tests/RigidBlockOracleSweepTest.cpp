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
 * slow test's header carries the new measured table. Three scales remain beyond the
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
		const FString Line = FString::Printf(
			TEXT("SWEEP %s: lambda=%.9g answered=%d pivots=%d secs=%.3f blocks=%d joints=%d ")
			TEXT("| production worstU=%.9g passes=%d fallen=%d stranded=%d | expected %s%s%s"),
			Row.Name, R.Oracle.Lambda, R.Oracle.bAnswered ? 1 : 0, R.Oracle.SimplexIterations,
			R.OracleSeconds, R.Blocks, R.Joints,
			R.WorstUtilisation, R.Passes, R.Fallen, R.Stranded, RelationName(Row.Expected),
			R.Oracle.WhyNot.IsEmpty() ? TEXT("") : TEXT(" | whynot: "),
			R.Oracle.WhyNot.IsEmpty() ? TEXT("") : *R.Oracle.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		Test.AddInfo(Line);
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

			if (Row.ProductionFallen != INDEX_NONE)
			{
				Test.TestEqual(
					*FString::Printf(
						TEXT("%s: production's drop count is pinned at %d (the DropsToday ")
						TEXT("pattern) and was %d"),
						Row.Name, Row.ProductionFallen, R.Fallen),
					R.Fallen, Row.ProductionFallen);
			}

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
				TEXT("lambda* %.9g, production dropped %d (stranded %d, worst reading %.9g). ")
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

		if (Row.ProductionFallen != INDEX_NONE)
		{
			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: production's drop count is pinned at %d (the DropsToday ")
					TEXT("pattern) and was %d"),
					Row.Name, Row.ProductionFallen, R.Fallen),
				R.Fallen, Row.ProductionFallen);
		}
	}

	void RunRows(FAutomationTestBase& Test, const TArray<FSweepRow>& Rows)
	{
		for (const FSweepRow& Row : Rows)
		{
			FSweepReading Reading;
			MeasureRow(Row, Reading);
			ReportRow(Test, Row, Reading);
			CheckRow(Test, Row, Reading);
		}
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
		ERelation::AgreeFalls, 0.062925, 0.062930, 29 });

	Rows.Add({ TEXT("leaning stack, 40 courses"),
		TEXT("no equilibrium; production agrees via the interim guard"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(40, Out, Why); },
		ERelation::AgreeFalls, 0.034428, 0.034431, 39 });

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
		ERelation::OracleStandsProductionFalls, 1.76400, 1.76414, 3 });

	Rows.Add({ TEXT("C24 timber beam, light load"),
		TEXT("a sensibly loaded joist; production still drops it whole (bearing Max)"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamC24DensityGramsPerCubicCm, BeamC24BendingMPa,
				BeamC24ShearMPa, 10.0, Out, Why);
		},
		ERelation::OracleStandsProductionFalls, 19.2006, 19.2009, 3 });

	Rows.Add({ TEXT("S275 steel beam, heavy load"),
		TEXT("steel at 17.35 vs timber's 1.76: the oracle discriminates the member ")
		TEXT("material 9.8x; production answers all three rows identically"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamSteelDensityGramsPerCubicCm, BeamS275YieldMPa,
				BeamS275YieldMPa / FMath::Sqrt(3.0), 120.0, Out, Why);
		},
		ERelation::OracleStandsProductionFalls, 17.3527, 17.3529, 3 });

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
		ERelation::OracleStandsProductionFalls, 9592.67, 9592.69, 2 });

	RunRows(*this, Rows);

	return true;
}

/* ====================================================================================
 * THE OPT-IN SLOW SWEEP.
 *
 * ITS NAME DELIBERATELY DOES NOT CONTAIN "DestructionGame", so the documented full-suite
 * command — Automation RunTests DestructionGame — NEVER runs it and the ~40 s suite
 * budget is untouched. Run it deliberately:
 *
 *     -ExecCmds="Automation RunTests OracleSlowSweep"
 *
 * This is the documented convention for a slow path: a distinct top-level group, opt-in
 * by filter, never a silent skip. Every row still carries pinned expectations exactly
 * like the fast rows; slowness buys exclusion from the default run and nothing else.
 * ==================================================================================== */

/**
 * THE WALL CATALOGUE AND THE FREE-END LADDER — sweep items (d) and (e), scoped in the
 * dense era to that solver's measured envelope; since the 2026-08-12 sparse rewrite
 * every scoped-out wall except the 30-course five ANSWERS (measured table above) and
 * awaits pinning in the classification slice. Rows are laid through the SCENARIO
 * CATALOGUE (production
 * data end to end: the same producer, the same cuts the acceptance suite pins),
 * removed, bridged and diffed. Ordered cheap to expensive so a killed run still leaves
 * its measurements in the log (each row logs immediately — which is exactly how the
 * 2026-08-11 measuring run survived being killed at row 15 of 18).
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
 * The free-end row stayed LIVE below as the pinned canary — one refusal kept
 * executable so the envelope was asserted by a test rather than remembered by a
 * comment, and chosen because it is the lambda = 3.464 measurement fixture: the day
 * the solver answers it, the canary fails and the gap-5 measurement it owes becomes
 * takeable. THAT DAY WAS 2026-08-12: the sparse rewrite answered it, the canary
 * failed exactly as written (lambda* 256.820018 against the pinned refusal), and the
 * row below is its promotion.
 *
 * WHAT THE 2026-08-12 SPARSE MEASURING RUN ANSWERED, every residual verified — these
 * are MEASUREMENTS for the next slice to classify and pin, recorded here so the run
 * need not be repeated (production's side quoted where it dropped anything):
 *
 *     wall-17          918.046031    1,608 pv    4.2 s   120 blk / 207 jnt
 *     wall-18          649.464192      791 pv    1.3 s   119 blk / 203 jnt
 *     wall-15          868.623736    3,745 pv   25.7 s   125 blk / 320 jnt
 *     wall-16          868.623736    3,319 pv   23.1 s   125 blk / 320 jnt
 *     wall-19           12.3824832   2,970 pv   16.1 s   119 blk / 308 jnt  (production: 34 fall, 6 stranded)
 *     wall-10           35.8172298   2,640 pv   11.5 s   138 blk / 349 jnt  (production: 12 fall, 3 stranded)
 *     wall-07          296.220581    5,293 pv   62.0 s   140 blk / 350 jnt
 *     wall-06          622.031942    8,819 pv  126.6 s   146 blk / 372 jnt
 *     wall-09           36.5639285   4,885 pv   50.4 s   146 blk / 350 jnt  (production stands at worstU 0.985)
 *     wall-11          128.118261    5,102 pv   45.8 s   128 blk / 326 jnt
 *     wall-12           89.1151243   2,877 pv   16.2 s   128 blk / 326 jnt
 *     wall-20           82.629597    3,387 pv   27.9 s   156 blk / 384 jnt  (production: 9 fall in 1 pass)
 *     free end 7x20    256.820018   2,701 pv   22.5 s   149 blk / 388 jnt  (production: 1 falls, worstU 1.958)
 *
 * Three of those beg interpretation the NEXT slice owes: the 11/12 pier pair now
 * DISCRIMINATES (128.1 vs 89.1, 1.44x) where production reads the two 0.03% apart;
 * the free-end lambda* is HEIGHT-INDEPENDENT to nine digits (256.820018 at both 10
 * and 20 courses) while production crosses 1.0 between them; and the red rows 9, 19
 * and 20 all read lambda* >= 12 — the oracle sides with STANDS against the
 * catalogue's collapse verdicts on 9/19 and against production's drops on 19/10/20,
 * so those classifications need the survivor arithmetic worked, not a reflex pin.
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

	/*
	 * THE PROMOTED CANARY — until 2026-08-12 this row was pinned as
	 * OracleRefusesAtThisScale, the one dense-envelope refusal kept executable so the
	 * envelope was asserted by a test rather than remembered by a comment. The sparse
	 * revised-simplex rewrite answered it (lambda* 256.820018, 879 pivots, ~1 s where
	 * the dense tableau refused after 3,993 pivots and 69 s), the canary failed
	 * exactly as written, and per its own instruction the failure was promoted to
	 * this measured relation rather than deleted. Production's worst joint here reads
	 * 0.9235, which is the RAW HALF-SEAT directly above the removed end brick (0.0582
	 * per brick-weight x ~15.9 bw carried, no abutment on the eccentric side so no
	 * arching relief); the free-end LADDER's published 0.3283 at ten courses is the
	 * corbel-ROOT joint, a different joint in the same wall. Both are true; quoting
	 * the ladder figure as "production's reading" for this fixture was the 2026-08-09
	 * draft's error.
	 */
	Rows.Add({ TEXT("free end, 7 cells x 10 courses"),
		TEXT("the lambda = 3.464 measurement fixture, first answered by the sparse ")
		TEXT("rewrite: the LP prices the ten-course free end at 257x own weight while ")
		TEXT("production's raw half-seat reads 0.9235 of capacity — the lambda-ladder ")
		TEXT("measurement (gap 5) is now takeable, next slice"),
		[](FStructure& Out, FString& Why) { return BuildFreeEnd(10, Out, Why); },
		ERelation::AgreeStands, 256.819, 256.821, 0 });

	Rows.Add({ TEXT("corbel D, ten steps with counterweight"),
		TEXT("global equilibrium prices the counterweight at 22.6x (C's 2.57 -> 58.06) ")
		TEXT("where production reads C and D 0.4% apart (0.34481 vs 0.34348) and the ")
		TEXT("deliberate red CorbelStepsBeforeTensionWins records them crossing 1.0 at ")
		TEXT("36 steps identically"),
		Scenario(TEXT("corbel-d-10-counterweight")),
		ERelation::AgreeStands, 58.0597, 58.0601, 0 });

	RunRows(*this, Rows);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
