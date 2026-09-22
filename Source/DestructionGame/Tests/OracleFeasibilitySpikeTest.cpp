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
 * Slice 0b, the latency-spike gate (PROMOTION_DESIGN.md §6, §11 R1/R2; user ruling D2,
 * 2026-08-14). Measurement only, no production code. Every reading lands pinned; an
 * unmeasured pin is the slice's red.
 *
 * The question: can an 84-block region answer feasibility in ~50 ms, two solves inside a
 * 100 ms budget? If not, synchronous LP authority at scenario scale is off the table.
 *
 * MEASUREMENT 1 — does abandoning lambda* pay? The live pose maximises lambda and starts
 * trivially feasible (every equality RHS zero), so phase 1 never runs and the cost is all
 * phase 2 climbing to lambda* (wall-01: 58,605 pivots, ~252 s). Production wants
 * feasibility at lambda = 1: posing dead (bGravityIsLive = false) puts gravity in the
 * equality rows as a constant, so phase 1 runs and phase 2 has nothing to climb. Feasible
 * reports lambda = LambdaCap, infeasible reports 0; a value between means a live load leaked
 * into a dead pose. Cross-validated: the dead-pose boolean must agree with the same run's
 * lambda* >= 1, and the leaning-stack arm (lambda* 0.44, infeasible) keeps it from being
 * vacuous.
 *
 * MEASUREMENT 2 — the regional sandwich. The roadmap's "a deleted brick cannot affect
 * masonry beyond a bonded distance" is false here (§5.3). A region is solved two ways:
 * boundary blocks pinned grounded (optimistic — infeasible here means the whole structure
 * is), and boundary blocks free (pessimistic — feasible here means the whole is); agreement
 * certifies, disagreement grows the region. The extractor is test-side, so the oracle stays
 * independently derived. Predictions are recorded per row in the code (§7.3).
 *
 * WHAT WAS MEASURED — 2026-08-15, one run each, tree at HEAD 9dcbd76.
 *
 * MEASUREMENT 1, speedups live pose over dead:
 *
 *     fixture           blocks   live pv / s     dead pv / s     pivots   seconds
 *     leaning stack 5        5        4 / 0.000      22 / 0.000    0.18x        —
 *     leaning stack 30      30       29 / 0.006     163 / 0.006    0.18x    0.99x
 *     8x10 intact wall      84    2,606 / 0.749     491 / 0.065    5.31x    11.5x
 *     corbel D    (t)       90    9,490 / 10.21   1,022 / 0.274    9.29x    37.3x
 *     wall-18     (t)      119    3,885 / 2.765     468 / 0.062    8.30x    44.6x
 *     wall-15     (t)      125    6,076 / 3.819     803 / 0.204    7.57x    18.7x
 *     wall-06     (t)      146   14,209 / 16.10     876 / 0.314   16.22x    51.2x
 *     wall-01              375   58,806 / 280.5   5,407 / 25.99   10.88x    10.8x
 *
 * (t) = trimmed 2026-08-16: these four no longer re-solve the live pose (lambda* pinned in
 * OracleSweepFull.RigidBlock.WallsAndLadders), so their live columns are cross-run. The
 * lever weakens with scale (dead pivots 2.2x the equality-row count at 84 blocks, ~5x at
 * 375) and small fixtures cost more dead than live, so the design's 10-100x holds in
 * neither direction.
 *
 * MEASUREMENT 1b — the early exit refuted. §3.2/§5.2 claimed feasibility can stop the
 * moment phase 1's infeasibility sum reaches tolerance, with most of the saving there.
 * Measured: 58 of 9,245 pooled phase-1 pivots happen after first feasibility, 0.63% (3.85%
 * worst). Phase 1's objective IS the infeasibility sum, so "reaches tolerance" and "phase 1
 * optimal" coincide — no tail to skip, the saving is already spent. All eight live poses
 * report 0/0.
 *
 * MEASUREMENT 2. The conservation prediction held: a free-boundary region with no grounded
 * block is infeasible at every radius (its vertical rows sum to 0 = -W_region), so the
 * sandwich can only certify a ground-connected region:
 *
 *     12-course x 12-cell wall, one brick out of course 6, 149 blocks, FEASIBLE, 965 pivots
 *     radius   region blocks   grounded-boundary   free-boundary   closes
 *          2              44           feasible      INFEASIBLE        no
 *          4             114           feasible      INFEASIBLE        no
 *          5             142           feasible        feasible       YES
 *          6             149           feasible        feasible       yes
 *
 * Closes at radius 5, 142 of 149 (95%), and its two solves cost 753 + 872 = 1,625 pivots
 * against 965 — 1.68x the work it avoids. Over ten deletions at radius 4, five certify
 * (courses 1-5, whose region reaches the foundation).
 *
 * And the pessimistic side is not a bound (Part D). The stack's bottom band (courses 0..5,
 * grounded, 24 courses dropped) is feasible both ways, so the sandwich certifies "full
 * structure feasible" about a stack whose lambda* is 0.4405. Dropping material removes its
 * weight with its restraint, so §5.3 does not hold. Not a knife edge: the band's own
 * lambda* is 18.481256459924058, 41.96x the load under which the certified structure has no
 * equilibrium; only carrying the omitted weight rescues it (§12 D2').
 *
 * THE GATE VERDICT. R1 (one solve): the 84-block fixture answers in 0.065 s against a
 * ~0.050 s target, a 1.3x gap (design stated 12x), warm starts unbuilt — R1 does not fire.
 * R2 (regional decomposition): fires. The sandwich certifies only when its region reaches
 * the ground, closes at 95-100% of the structure, costs 1.68x a global solve, and its
 * pessimistic side can certify a structure with no equilibrium. Scenario scale needs
 * decomposition (wall-01's 375 blocks take 26 s), so R2 is what changes the design's shape.
 *
 * PINNED EXACTLY: per-fixture block/joint counts, pivot counts, the feasibility verdict,
 * the pose lambda (LambdaCap or 0), the early-exit indices and pooled totals, region block
 * counts, closing radius, agreement count, and that every solve ANSWERED. A refusal returns
 * bAnswered = false, lambda = 0, bit-identical to the infeasibility arm (the arm that
 * refused 128-/200-block members a commit ago), so an unguarded one reads as "pessimistic
 * side infeasible" — this file's headline shape. PINNED IN A WINDOW: live-pose lambda*,
 * file-wide +/-2e-5 relative. NOT PINNED: wall-clock time (it measures the machine), except
 * one order-of-magnitude ceiling on the gate fixture.
 *
 * COST: four opt-in tests, split 2026-08-16 into OracleSweepFast (iteration) and
 * OracleSweepFull (3 tests, ~21 min, mandatory before any commit touching the LP oracle).
 * Default suite untouched at 173 = 167 green + 6 deliberate reds.
 *
 * Every conclusion has a recorded mutation (§9.5), TRAPS' registry X1-X11:
 *
 *     X1  ladder deletes from course 5 not 6              ->  7 assertions
 *     X2  gate fixture built 8x11 not 8x10               ->  5 assertions
 *     X3  PhaseOnePivots reported as constant 0          ->  the phase-1 pins
 *     X4  feasibility watch disabled                     ->  the first-feasible pins
 *     X5  watch records at pivot 1 unconditionally       -> 10 assertions
 *     X6  extractor keeps out-of-region blocks           -> 14 here + 12 in Repaired
 *     X7  a region solve forced to REFUSE                ->  1 assertion (zero before the guard)
 *     X8  surcharge at the contact's X not the CoG       ->  3 here + 4 in Repaired; re-creates
 *                                                            the false certificate
 *     X9  carried set computed empty                     ->  5 here + 8 in Repaired
 *    X10  Carried rule charges every component           ->  0 here, 7 in Repaired
 *    X11  sub-1.0 chimney built with no lean             ->  SubUnityWallCertificate's rows
 *
 * Named namespace, not anonymous, every constant Spike-prefixed: a unity build shares
 * file-scope names (TRAPS). Needs a ticking world: no.
 */
namespace OracleFeasibilitySpikeSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	/* The brick and grid, derived here so a wrong production constant disagrees rather than echoes. */

	constexpr double SpikeBrickLengthCm = 21.5;
	constexpr double SpikeBrickWidthCm = 10.25;
	constexpr double SpikeBrickHeightCm = 6.5;
	constexpr double SpikeClayDensityGramsPerCubicCm = 1.9;
	constexpr double SpikeJointCm = 1.0;

	/** Course pitch and cell pitch on the coordinating grid: 7.5 cm and 22.5 cm. */
	constexpr double SpikeCoursePitchCm = SpikeBrickHeightCm + SpikeJointCm;
	constexpr double SpikeCellPitchCm = SpikeBrickLengthCm + SpikeJointCm;

	/** Density-first multiplication order — the PieceMassKg contract; 2.72163125 kg. */
	constexpr double SpikeBrickMassKg = SpikeClayDensityGramsPerCubicCm
		* SpikeBrickLengthCm * SpikeBrickWidthCm * SpikeBrickHeightCm / 1000.0;

	/**
	 * Slack outside an exact grid multiple for radius membership: 1 cm, far below the 22.5 cm
	 * it discriminates and above centroid noise, so a half-cell-offset half bat is not rounded
	 * in or out.
	 */
	constexpr double SpikeRadiusSlackCm = 1.0;

	/** The file-wide relative window on a certified lambda*, per the sweep's discipline. */
	constexpr double SpikeLambdaRelativeWindow = 2.0e-5;

	/**
	 * Unmeasured sentinel for the one pin whose measured values include INDEX_NONE:
	 * PivotsToFirstFeasible reports INDEX_NONE genuinely (an infeasible problem never reaches
	 * feasibility), so its unmeasured state needs a value the solver cannot produce.
	 */
	constexpr int32 SpikeUnmeasured = -2;

	/* Fixture builders: production's own producers, transcribed from the sweep file's translation unit. */

	/** Lay a scenario row's structure and apply its cut — production data end to end. */
	bool SpikeBuildScenario(const TCHAR* ScenarioName, FStructure& Out, FString& OutWhy)
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

	/** The 84-block gate wall: the acceptance producer at 8 x 10, wall-01 shrunk. */
	bool SpikeBuildIntactWall(int32 Courses, int32 Cells, FStructure& Out, FString& OutWhy)
	{
		DestructionWallCases::FWallSpec Spec;
		Spec.BrickSizeCm = FVector(SpikeBrickLengthCm, SpikeBrickWidthCm, SpikeBrickHeightCm);
		Spec.JointThicknessCm = SpikeJointCm;
		Spec.DensityGramsPerCubicCm = SpikeClayDensityGramsPerCubicCm;
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

	/**
	 * The leaning-stack acceptance fixture (LeaningStackAcceptanceTest): course i at
	 * (10*i, 0, 3.25 + 7.5*i), base grounded, 1 cm mortar bed. The only cheap fixture the LP
	 * prices below 1.0, so the infeasible arm of the cross-validation.
	 */
	bool SpikeBuildLeaningStack(int32 Courses, FStructure& Out, FString& OutWhy)
	{
		TArray<FPieceBox> Boxes;

		for (int32 Course = 0; Course < Courses; ++Course)
		{
			FPieceBox Box;
			Box.ExtentCm =
				FVector(SpikeBrickLengthCm, SpikeBrickWidthCm, SpikeBrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				double(Course) * 10.0, 0.0,
				SpikeBrickHeightCm / 2.0 + double(Course) * SpikeCoursePitchCm);

			Out.AddPiece(SpikeBrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			Boxes.Add(Box);
		}

		for (int32 First = 0; First < Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(First, Boxes[First], Second, Boxes[Second],
						SpikeJointCm, GeneralPurposeMortar, Joint))
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

	/* One solve, both poses. */

	struct FPoseReading
	{
		bool bAnswered = false;
		double Lambda = 0.0;
		int32 Pivots = 0;
		int64 Scans = 0;
		double Seconds = 0.0;
		FString WhyNot;

		/*
		 * The early exit's two readings: what stopping at feasibility would save. Both pinned per
		 * row, so the 0.63% conclusion cannot be inverted by a pricing or tolerance change while
		 * every test stays green.
		 */
		int32 PhaseOnePivots = INDEX_NONE;
		int32 PivotsToFirstFeasible = INDEX_NONE;
	};

	FPoseReading SpikeSolve(const FOracleProblem& Problem)
	{
		FPoseReading Out;

		const double Started = FPlatformTime::Seconds();
		const FOracleResult Result = SolveRigidBlock(Problem);
		Out.Seconds = FPlatformTime::Seconds() - Started;

		Out.bAnswered = Result.bAnswered;
		Out.Lambda = Result.Lambda;
		Out.Pivots = Result.SimplexIterations;
		Out.Scans = Result.PricingColumnScans;
		Out.WhyNot = Result.WhyNot;
		Out.PhaseOnePivots = Result.PhaseOnePivots;
		Out.PivotsToFirstFeasible = Result.PivotsToFirstFeasible;

		return Out;
	}

	/**
	 * The feasibility verdict as production reads it: answered, and lambda >= 1. In the dead
	 * pose lambda is only LambdaCap or 0, a boolean in a double's clothes.
	 */
	bool SpikeIsFeasible(const FPoseReading& Reading)
	{
		return Reading.bAnswered && Reading.Lambda >= 1.0;
	}

	/* The region extractor: test-side, one flag apart between the two boundary conditions. */

	struct FRegionCounts
	{
		int32 Core = 0;
		int32 Shell = 0;
		int32 Blocks = 0;
		int32 Joints = 0;

		/** Blocks in the region that are grounded, the only reaction a region can have. */
		int32 Grounded = 0;

		/*
		 * Membership and remap, published so the surcharge reuses the exact membership the region
		 * was cut with rather than deriving it a second time.
		 */
		TArray<bool> bInRegion;

		/** Full-problem block index -> region block index; INDEX_NONE for an omitted block. */
		TArray<int32> Remap;
	};

	/**
	 * Cut a region out of a whole-structure problem. Core is the caller's mask; shell is every
	 * block outside it sharing a joint with it, the boundary the two sandwich sides disagree on:
	 * bGroundTheShell true makes the shell earth (optimistic), false keeps its grounded flag so
	 * it writes equilibrium rows and carries its weight (pessimistic). Joints leaving the region
	 * are dropped, as is one between two grounded blocks. Output is posed dead.
	 */
	void SpikeExtractRegion(
		const FOracleProblem& Full,
		const TArray<bool>& bInCore,
		bool bGroundTheShell,
		FOracleProblem& Out,
		FRegionCounts& Counts)
	{
		Out = FOracleProblem();
		Counts = FRegionCounts();

		const int32 NumBlocks = Full.Blocks.Num();

		TArray<bool> bIsShell;
		bIsShell.Init(false, NumBlocks);

		for (const FOracleJoint& Joint : Full.Joints)
		{
			const bool bA = bInCore[Joint.BlockA];
			const bool bB = bInCore[Joint.BlockB];

			if (bA && !bB)
			{
				bIsShell[Joint.BlockB] = true;
			}
			else if (bB && !bA)
			{
				bIsShell[Joint.BlockA] = true;
			}
		}

		TArray<int32> Remap;
		Remap.Init(INDEX_NONE, NumBlocks);

		for (int32 Block = 0; Block < NumBlocks; ++Block)
		{
			if (!bInCore[Block] && !bIsShell[Block])
			{
				continue;
			}

			FOracleBlock Copy = Full.Blocks[Block];

			if (bIsShell[Block] && bGroundTheShell)
			{
				Copy.bGrounded = true;
			}

			Remap[Block] = Out.Blocks.Num();
			Out.Blocks.Add(Copy);

			if (bInCore[Block])
			{
				++Counts.Core;
			}
			else
			{
				++Counts.Shell;
			}

			if (Copy.bGrounded)
			{
				++Counts.Grounded;
			}
		}

		for (const FOracleJoint& Joint : Full.Joints)
		{
			const int32 A = Remap[Joint.BlockA];
			const int32 B = Remap[Joint.BlockB];

			if (A == INDEX_NONE || B == INDEX_NONE)
			{
				continue;
			}

			if (Out.Blocks[A].bGrounded && Out.Blocks[B].bGrounded)
			{
				continue;
			}

			FOracleJoint Copy = Joint;
			Copy.BlockA = A;
			Copy.BlockB = B;
			Out.Joints.Add(Copy);
		}

		Out.bGravityIsLive = false;

		Counts.Blocks = Out.Blocks.Num();
		Counts.Joints = Out.Joints.Num();

		Counts.bInRegion.Init(false, NumBlocks);

		for (int32 Block = 0; Block < NumBlocks; ++Block)
		{
			Counts.bInRegion[Block] = bInCore[Block] || bIsShell[Block];
		}

		Counts.Remap = MoveTemp(Remap);
	}

	/** A box mask on the coordinating grid: R courses up and down, R cells left and right. */
	void SpikeBoxMask(
		const FOracleProblem& Full,
		double CentreXCm,
		double CentreZCm,
		int32 RadiusCourses,
		int32 RadiusCells,
		TArray<bool>& OutMask)
	{
		OutMask.Init(false, Full.Blocks.Num());

		const double ReachZ = double(RadiusCourses) * SpikeCoursePitchCm + SpikeRadiusSlackCm;
		const double ReachX = double(RadiusCells) * SpikeCellPitchCm + SpikeRadiusSlackCm;

		for (int32 Block = 0; Block < Full.Blocks.Num(); ++Block)
		{
			const FOracleBlock& B = Full.Blocks[Block];

			OutMask[Block] = FMath::Abs(B.CentroidZCm - CentreZCm) <= ReachZ
				&& FMath::Abs(B.CentroidXCm - CentreXCm) <= ReachX;
		}
	}

	/** A chain mask for the leaning stack: every block within R courses of a height. */
	void SpikeCourseBandMask(
		const FOracleProblem& Full,
		double CentreZCm,
		int32 RadiusCourses,
		TArray<bool>& OutMask)
	{
		OutMask.Init(false, Full.Blocks.Num());

		const double ReachZ = double(RadiusCourses) * SpikeCoursePitchCm + SpikeRadiusSlackCm;

		for (int32 Block = 0; Block < Full.Blocks.Num(); ++Block)
		{
			OutMask[Block] =
				FMath::Abs(Full.Blocks[Block].CentroidZCm - CentreZCm) <= ReachZ;
		}
	}

	/*
	 * REPAIR (1) — the omitted material's weight, kept as a dead surcharge (§12 D2').
	 *
	 * The refuted form dropped an omitted block's restraint and weight together, and the weight
	 * mattered: the stack's 24 omitted courses carried the destabilising moment, so dropping them
	 * turned a no-equilibrium structure into a short stack that stands (Part D). The repair keeps
	 * the demand and still refuses the restraint: a block bearing on the region contributes its
	 * weight as a dead force, writing no equilibrium row.
	 *
	 * "Bears on the region" must be right both ways: charge too little and the false certificate
	 * survives, charge everything and a strip carries the whole building. The rule, no geometry:
	 * delete the region from the joint graph; an omitted block whose remaining component reaches
	 * no grounded block is carried, and every block in that component is charged. Material with
	 * its own ground path is not.
	 *
	 * Where the load is applied is what the false certificate turns on. A component's weight is
	 * delivered at its interface joints, shared by area, on the vertical through its centre of
	 * gravity, not the contact's X. A vertical force's moment is independent of the Z of its
	 * application point, so one line reproduces force and moment exactly; the contact centre would
	 * drop the overturning moment (the omitted courses weigh on a line at X = 175 cm, their
	 * contact at X = 55).
	 *
	 * Pessimistic side only: the optimistic side's soundness is a restriction argument a surcharge
	 * row would break. Known limit: exact when omitted material is wholly carried or wholly
	 * self-supporting; it under-charges material with its own ground path that also bears on the
	 * region, which is why the wall regions below are full height by construction.
	 */

	enum class ESpikeSurcharge : uint8
	{
		/** The refuted form: omitted material contributes nothing at all. */
		None,

		/** Only what the region carries — the component-reaches-no-ground rule above. */
		Carried,

		/**
		 * Every omitted block, whatever it stands on. Deliberately too pessimistic, a control
		 * so "charging everything never certifies" is measured, not argued.
		 */
		EveryOmittedBlock
	};

	struct FSurchargeCounts
	{
		int32 ChargedBlocks = 0;
		int32 ChargedComponents = 0;
		int32 InterfaceJoints = 0;

		/**
		 * A charged component sharing no joint with the region: its weight is silently lost, the
		 * refuted form by the back door. Asserted zero.
		 */
		int32 OrphanComponents = 0;

		double ChargedWeightUu = 0.0;
		double AppliedWeightUu = 0.0;

		/**
		 * Surcharge delivered onto a grounded region block, which writes no equilibrium row and
		 * discards the force silently. As fatal as an orphan component. Asserted zero.
		 */
		double DiscardedOntoGroundedUu = 0.0;
	};

	void SpikeAddSurcharge(
		const FOracleProblem& Full,
		const FRegionCounts& Region,
		ESpikeSurcharge Rule,
		FOracleProblem& InOut,
		FSurchargeCounts& Out)
	{
		Out = FSurchargeCounts();

		if (Rule == ESpikeSurcharge::None)
		{
			return;
		}

		const int32 NumBlocks = Full.Blocks.Num();

		/*
		 * Adjacency among omitted blocks only. The region is deleted here, so "can this block
		 * reach the ground" means "without the region's help".
		 */
		TArray<TArray<int32>> Adjacency;
		Adjacency.SetNum(NumBlocks);

		for (const FOracleJoint& Joint : Full.Joints)
		{
			if (Region.bInRegion[Joint.BlockA] || Region.bInRegion[Joint.BlockB])
			{
				continue;
			}

			Adjacency[Joint.BlockA].Add(Joint.BlockB);
			Adjacency[Joint.BlockB].Add(Joint.BlockA);
		}

		TArray<int32> Component;
		Component.Init(INDEX_NONE, NumBlocks);

		TArray<TArray<int32>> Members;

		for (int32 Seed = 0; Seed < NumBlocks; ++Seed)
		{
			if (Region.bInRegion[Seed] || Component[Seed] != INDEX_NONE)
			{
				continue;
			}

			const int32 Label = Members.Num();
			Members.AddDefaulted();
			Component[Seed] = Label;

			TArray<int32> Pending;
			Pending.Add(Seed);

			while (Pending.Num() > 0)
			{
				const int32 At = Pending.Pop();
				Members[Label].Add(At);

				for (const int32 Next : Adjacency[At])
				{
					if (Component[Next] == INDEX_NONE)
					{
						Component[Next] = Label;
						Pending.Add(Next);
					}
				}
			}
		}

		for (int32 Label = 0; Label < Members.Num(); ++Label)
		{
			bool bReachesGround = false;

			for (const int32 Block : Members[Label])
			{
				bReachesGround = bReachesGround || Full.Blocks[Block].bGrounded;
			}

			if (Rule == ESpikeSurcharge::Carried && bReachesGround)
			{
				continue;
			}

			double WeightUu = 0.0;
			double WeightedXCm = 0.0;

			for (const int32 Block : Members[Label])
			{
				const double W =
					Full.Blocks[Block].MassKg * OracleGravityCmPerSecondSquared;

				WeightUu += W;
				WeightedXCm += W * Full.Blocks[Block].CentroidXCm;
			}

			if (!(WeightUu > 0.0))
			{
				continue;
			}

			const double GravityLineXCm = WeightedXCm / WeightUu;

			++Out.ChargedComponents;
			Out.ChargedBlocks += Members[Label].Num();
			Out.ChargedWeightUu += WeightUu;

			/* The contacts where this component meets the region, in joint index order. */
			TArray<int32> Interface;
			double TotalAreaSqCm = 0.0;

			for (int32 J = 0; J < Full.Joints.Num(); ++J)
			{
				const FOracleJoint& Joint = Full.Joints[J];

				const bool bAIn = Region.bInRegion[Joint.BlockA];
				const bool bBIn = Region.bInRegion[Joint.BlockB];

				const bool bTouches =
					(bAIn && !bBIn && Component[Joint.BlockB] == Label)
					|| (bBIn && !bAIn && Component[Joint.BlockA] == Label);

				if (bTouches)
				{
					Interface.Add(J);
					TotalAreaSqCm += Joint.AreaSqCm;
				}
			}

			if (Interface.Num() == 0 || !(TotalAreaSqCm > 0.0))
			{
				++Out.OrphanComponents;
				continue;
			}

			Out.InterfaceJoints += Interface.Num();

			for (const int32 J : Interface)
			{
				const FOracleJoint& Joint = Full.Joints[J];

				const int32 RegionSide =
					Region.bInRegion[Joint.BlockA] ? Joint.BlockA : Joint.BlockB;

				const int32 Target = Region.Remap[RegionSide];
				const double ShareUu = WeightUu * (Joint.AreaSqCm / TotalAreaSqCm);

				FOracleAppliedForce Applied;
				Applied.Block = Target;
				Applied.ForceXUu = 0.0;
				Applied.ForceZUu = -ShareUu;
				Applied.AtXCm = GravityLineXCm;
				Applied.AtZCm = Joint.CentreZCm;
				Applied.bLive = false;

				InOut.AppliedForces.Add(Applied);
				Out.AppliedWeightUu += ShareUu;

				if (InOut.Blocks[Target].bGrounded)
				{
					Out.DiscardedOntoGroundedUu += ShareUu;
				}
			}
		}
	}

	/** The repaired extractor: the same cut, plus repair (1) on the pessimistic side only. */
	void SpikeExtractRepairedRegion(
		const FOracleProblem& Full,
		const TArray<bool>& bInCore,
		bool bGroundTheShell,
		ESpikeSurcharge Rule,
		FOracleProblem& Out,
		FRegionCounts& Counts,
		FSurchargeCounts& Surcharge)
	{
		SpikeExtractRegion(Full, bInCore, bGroundTheShell, Out, Counts);

		Surcharge = FSurchargeCounts();

		if (bGroundTheShell)
		{
			return;
		}

		SpikeAddSurcharge(Full, Counts, Rule, Out, Surcharge);
	}

	/*
	 * REPAIR (2) — regions that reach the ground by construction.
	 *
	 * The free side is provably infeasible for any region with no grounded block (sum its
	 * vertical rows: 0 = -W_region), so the growth parameter cannot be a radius (a ball reaches
	 * the foundation only by accident, why the refuted form certified five of ten). It becomes
	 * the width of a full-height strip: every block within (0.5 + w) cells of the deletion, at
	 * every height. The half cell matters: running bond offsets alternate courses by half a cell,
	 * so a whole-cell window takes a ladder of disconnected rungs where the half cell gives a
	 * connected strip at every w including 0.
	 */

	void SpikeGroundStripMask(
		const FOracleProblem& Full,
		double CentreXCm,
		int32 HalfWidthCells,
		TArray<bool>& OutMask)
	{
		OutMask.Init(false, Full.Blocks.Num());

		const double ReachX =
			(0.5 + double(HalfWidthCells)) * SpikeCellPitchCm + SpikeRadiusSlackCm;

		for (int32 Block = 0; Block < Full.Blocks.Num(); ++Block)
		{
			OutMask[Block] =
				FMath::Abs(Full.Blocks[Block].CentroidXCm - CentreXCm) <= ReachX;
		}
	}

	/** Everything from the foundation up to a height — the chain fixtures' ground strip. */
	void SpikeGroundBandMask(
		const FOracleProblem& Full,
		double TopZCm,
		TArray<bool>& OutMask)
	{
		OutMask.Init(false, Full.Blocks.Num());

		for (int32 Block = 0; Block < Full.Blocks.Num(); ++Block)
		{
			OutMask[Block] = Full.Blocks[Block].CentroidZCm <= TopZCm + SpikeRadiusSlackCm;
		}
	}

	/* Picking a brick to delete, deterministically. */

	/**
	 * The live, ungrounded piece nearest the middle of course k (grounded bottom course = 0).
	 * Index order breaks ties, so the fixture is the same wall on every run.
	 */
	/**
	 * One fixture, both poses. Every pin is INDEX_NONE or negative until measured; an unmeasured
	 * pin is this slice's red.
	 */
	struct FCostRow
	{
		const TCHAR* Name = nullptr;

		/** What revision 1 of the derivation record predicted, printed beside the result. */
		const TCHAR* Prediction = nullptr;

		TFunction<bool(FStructure&, FString&)> Build;

		int32 Blocks = INDEX_NONE;
		int32 Joints = INDEX_NONE;

		/** lambda* of the gravity-LIVE pose, in the file's +/-2e-5 relative window. */
		double LambdaLo = -1.0;
		double LambdaHi = -1.0;

		int32 LivePivots = INDEX_NONE;
		int32 DeadPivots = INDEX_NONE;

		/**
		 * The early exit, pinned: dead-pose phase-1 pivots, and the pivot at which the
		 * problem first read feasible. Their difference is what an early exit would buy.
		 */
		int32 DeadPhaseOnePivots = INDEX_NONE;

		/**
		 * INDEX_NONE is a measured value here, not a sentinel: an infeasible problem never reaches
		 * feasibility and reports exactly that, so "unmeasured" needs its own value, SpikeUnmeasured.
		 */
		int32 DeadFirstFeasible = SpikeUnmeasured;

		/** 1 feasible at lambda = 1, 0 infeasible. INDEX_NONE is unmeasured. */
		int32 Feasible = INDEX_NONE;

		/*
		 * Whether this row re-solves the gravity-live pose. Four do not (the 2026-08-16 tier
		 * split): corbel D, wall-06, wall-15 and wall-18 have lambda* pinned bit-identically in
		 * OracleSweepFull.RigidBlock.WallsAndLadders in the same tier, so re-solving bought a
		 * window that already exists and ~33 s per run. Kept live: wall-01 (its only pinned
		 * lambda*), the 84-block gate fixture, and both leaning stacks (microseconds, one the
		 * infeasible arm). The dead pose is untouched everywhere, so the pooled early-exit totals
		 * and every verdict stay bit-identical.
		 */
		bool bSolveLivePose = true;
	};

	/**
	 * The sandwich's pins, as struct members not local constants: a pin the compiler can fold
	 * turns its unmeasured branch into dead code and stops the measurement being taken.
	 */
	struct FSandwichPins
	{
		int32 LadderBlocks = 149;
		int32 GlobalFeasible = 1;
		int32 GlobalPivots = 965;

		/*
		 * Measured 2026-08-15. Brick out of course 6 of a 12-course wall; the sandwich first
		 * closes at radius 5, the first reaching the foundation (the conservation lemma, not the
		 * design's "small radius"). Region there: 142 of 149.
		 */
		int32 ClosingRadius = 5;
		int32 ClosingRegionBlocks = 142;
		int32 ClosingOptimisticPivots = 753;
		int32 ClosingPessimisticPivots = 872;

		/*
		 * R2's answer. Ten deletions, courses 1..10, radius 4: five certify. Predicted four; the
		 * miss is the shell reaching one course past the core, so a radius-4 box around a course-5
		 * brick touches the foundation.
		 */
		int32 Agreements = 5;

		/*
		 * The collapse arm: the 30-course stack's optimistic region is still feasible at radius 13
		 * (29 of 30) and only infeasible at radius 14, all 30 blocks.
		 */
		int32 StackCertifyingRadius = 14;
		int32 StackCertifyingRegionBlocks = 30;

		/* Part D's band: courses 0..4 core plus course 5 shell, six blocks and five joints. */
		int32 BandBlocks = 6;

		/*
		 * How wrong the false certificate is: the band's live-pose lambda*, file-wide +/-2e-5
		 * window (negative is unmeasured). Measured 2026-08-15 at 18.481256459924058; the stack is
		 * 0.44048, so the sandwich certified a region standing at 41.96x the load under which the
		 * certified structure has no equilibrium. Review's independent (N-1)^-2.29 fit gave ~18.7,
		 * 1.2% off.
		 */
		double BandLiveLambdaLo = 18.48088;
		double BandLiveLambdaHi = 18.48163;
	};

	/** One rung of the radius ladder, pinned by size and by both verdicts. */
	struct FRadiusPin
	{
		int32 Radius = INDEX_NONE;
		int32 Blocks = INDEX_NONE;
		int32 Optimistic = INDEX_NONE;
		int32 Pessimistic = INDEX_NONE;
	};

	/** One rung of the REPAIRED sandwich's strip ladder. INDEX_NONE is unmeasured. */
	struct FStripPin
	{
		int32 HalfWidthCells = INDEX_NONE;
		int32 Blocks = INDEX_NONE;

		/** Blocks the Carried rule charges as a surcharge at this width. */
		int32 Charged = INDEX_NONE;

		int32 Optimistic = INDEX_NONE;
		int32 Pessimistic = INDEX_NONE;
	};

	/**
	 * The repaired sandwich's pins. Every one is INDEX_NONE or negative until measured; an
	 * unmeasured pin is this slice's red.
	 */
	struct FRepairPins
	{
		/* The refuted form's fixture, pinned again so the two are the same wall. */
		int32 WallBlocks = 149;
		int32 WallGlobalPivots = 965;

		/*
		 * P3: closes at half-width 0 (a two-cell zig-zag column), 41 of 149 blocks, 28% vs the
		 * refuted form's 142 / 95%. Measured 2026-08-15: half-width 0, 41 blocks, 27.5% — hit.
		 * P5 predicted cost 0.3-0.6x global; measured 49 + 262 = 311 vs 965, 0.322x, the
		 * optimistic side nearly free (grounding the shell makes most of the boundary earth).
		 */
		int32 ClosingHalfWidth = 0;
		int32 ClosingRegionBlocks = 41;
		int32 ClosingOptimisticPivots = 49;
		int32 ClosingPessimisticPivots = 262;

		/*
		 * P4: ten of ten deletions certify (a ground-anchored strip reaches the foundation at
		 * every height, so the course stops mattering). Measured 10 — hit. Named Agreements, not
		 * Certificates, to keep it distinct from the file-wide count of closed sandwiches (20).
		 */
		int32 Agreements = 10;

		/*
		 * P8: the over-inclusive control (charge every omitted block) should read infeasible at
		 * the closing width. Measured feasible — a miss, the most useful in the run. Charging all
		 * 108 omitted blocks (271,309.925 uu) still leaves the 41-block strip standing: the wall
		 * arm has too much margin to discriminate surcharge rules, so the "too pessimistic to
		 * certify" mode is untested here and the wall arm is evidence about repair (2) only.
		 */
		int32 ControlPessimistic = 1;

		/*
		 * P7: a ground-anchored region's size scales with height. Same wall at 18 courses:
		 * predicted half-width 0 and ~62 blocks; measured 224 blocks, half-width 0, region 62 —
		 * 1.512x, 27.7% vs 27.5%, hit. The fraction is height-invariant, the count is not: a
		 * 30-course strip is ~102 blocks, larger than the 84-block fixture. Its two pivot counts
		 * are pinned as the input to that extrapolation (79 + 474 = 553 against the 12-course
		 * 49 + 262 = 311, the 1.78x-per-1.5x-height slope scenario scale rests on).
		 */
		int32 TallWallBlocks = 224;
		int32 TallClosingHalfWidth = 0;
		int32 TallClosingRegionBlocks = 62;
		int32 TallClosingOptimisticPivots = 79;
		int32 TallClosingPessimisticPivots = 474;

		/*
		 * The collapse arm. P9: the repaired pessimistic side is infeasible at every band short
		 * of the whole stack (the surcharge restores the destabilising moment), and the optimistic
		 * side goes infeasible only with all 30, so the sandwich closes at the whole structure and
		 * nowhere below — a false certificate turned into a refusal, at no saving. Measured exactly
		 * that: only band 0..29 closes. Hit.
		 */
		int32 StackClosingTopCourse = 29;
		int32 StackClosingRegionBlocks = 30;

		/*
		 * What the collapse arm costs. At closure the two solves are 163 + 163 = 326 against a
		 * 163-pivot global solve (2.0x), and the whole growth ladder from 0..5 up is 1,300 pivots
		 * (8x). 2.0x is worse than the 1.68x the refuted form was condemned for, on the arm a
		 * player's deletion cares about. Pinned so the ratio is a contract.
		 */
		int32 StackClosingOptimisticPivots = 163;
		int32 StackClosingPessimisticPivots = 163;
		int32 StackLadderPivots = 1300;

		/*
		 * The surcharge where it works: band 0..5 (7 blocks) charges the 23 courses above. Pinned
		 * because a reachability walk charging the wrong set would still give an infeasible band
		 * with every verdict green.
		 */
		int32 StackBandChargedAtFive = 23;
	};

	int32 SpikePieceInCourse(const FStructure& Wall, int32 Course, double& OutXCm, double& OutZCm)
	{
		double LowestZ = TNumericLimits<double>::Max();
		double LowX = TNumericLimits<double>::Max();
		double HighX = -TNumericLimits<double>::Max();

		for (int32 Piece = 0; Piece < Wall.NumPieces(); ++Piece)
		{
			if (Wall.IsPieceRemoved(Piece))
			{
				continue;
			}

			const FVector Centre = Wall.GetPiece(Piece).CentreOfMassCm;
			LowestZ = FMath::Min(LowestZ, Centre.Z);
			LowX = FMath::Min(LowX, Centre.X);
			HighX = FMath::Max(HighX, Centre.X);
		}

		const double WantZ = LowestZ + double(Course) * SpikeCoursePitchCm;
		const double WantX = 0.5 * (LowX + HighX);

		int32 Best = INDEX_NONE;
		double BestScore = TNumericLimits<double>::Max();

		for (int32 Piece = 0; Piece < Wall.NumPieces(); ++Piece)
		{
			if (Wall.IsPieceRemoved(Piece) || Wall.GetPiece(Piece).bIsGrounded)
			{
				continue;
			}

			const FVector Centre = Wall.GetPiece(Piece).CentreOfMassCm;

			if (FMath::Abs(Centre.Z - WantZ) > 0.5 * SpikeCoursePitchCm)
			{
				continue;
			}

			const double Score = FMath::Abs(Centre.X - WantX);

			if (Score < BestScore)
			{
				BestScore = Score;
				Best = Piece;
			}
		}

		if (Best != INDEX_NONE)
		{
			OutXCm = Wall.GetPiece(Best).CentreOfMassCm.X;
			OutZCm = Wall.GetPiece(Best).CentreOfMassCm.Z;
		}

		return Best;
	}

	/*
	 * THE SUB-1.0 WALL — a wall carrying a leaning chimney.
	 *
	 * Why it exists: the repaired pessimistic side has never been shown to be a bound, and cannot
	 * be on any fixture the project owns (§5.3, §11 R2). Of the repaired test's twenty
	 * certificates, nineteen are about already-feasible structures and the twentieth is the whole
	 * structure, so none is about a proper subset of an infeasible one — the only shape a false
	 * certificate takes. The fixture needs the failure to live outside the strip (a wall failing
	 * at its own foundation makes the strip infeasible too and teaches nothing).
	 *
	 * The mechanism (§5.3's second route): a projecting mass that overturns globally while a
	 * ground-anchored strip stays locally stable. A 30-course leaning chimney (10 cm lean per
	 * course) mortared onto the rightmost full brick of the top course leans out past the wall's
	 * end; its bed joints cannot carry the eccentric load, the chain the LP prices at
	 * lambda* = 0.44048, and the wall cannot help because the chain sees the same loads either way.
	 *
	 * Three routes not taken: widen an opening to the crossing (500+ blocks, against cost
	 * discipline); a corbel (priced at 17-161, reaching 1.0 is a search); a weak course (zeroing
	 * bond does not make masonry fall). Limitation: the chimney makes the structure infeasible
	 * before the deletion, so this counterexample is not deletion-caused — fine, the pessimistic
	 * claim is unconditional; a deletion-caused non-local failure needs the wide-opening fixture,
	 * a specified follow-up.
	 */

	constexpr int32 SpikeChimneyCourses = 30;

	/**
	 * Chimney lean per course, cm. The leaning stack's 10 cm on a 21.5 cm brick, so the two
	 * chains are the same and the one priced at 0.44048 is expected to price the other.
	 */
	constexpr double SpikeChimneyLeanCm = 10.0;

	/**
	 * The bypassed root bed joint's margin (capacity over demand), from the two-contact form.
	 * Measured 2026-08-18 at 1.54375, pinned in a ~6e-5 window: the claim is "comfortably over
	 * 1.0", not the exact digits.
	 */
	constexpr double SpikeBypassedMarginLo = 1.5437;
	constexpr double SpikeBypassedMarginHi = 1.5438;

	struct FChimneyWall
	{
		FStructure Structure;

		/** The brick removed from the wall, and where it sat. */
		int32 Victim = INDEX_NONE;
		double DeleteXCm = 0.0;
		double DeleteZCm = 0.0;

		/** The wall brick the chimney stands on, which is the far end of the structure. */
		double RootXCm = 0.0;
		double RootZCm = 0.0;

		int32 ChimneyBlocks = 0;
	};

	/**
	 * Lay the wall, choose the victim, mortar the chimney onto the top course, then delete.
	 * ChimneyCourses = 0 lays the same wall with no chimney through the same code, making the
	 * two comparable rather than similar.
	 */
	bool SpikeBuildChimneyWall(
		int32 Courses,
		int32 Cells,
		int32 ChimneyCourses,
		int32 DeleteCourse,
		FChimneyWall& Out,
		FString& OutWhy)
	{
		Out = FChimneyWall();

		if (!SpikeBuildIntactWall(Courses, Cells, Out.Structure, OutWhy))
		{
			return false;
		}

		/*
		 * The victim is chosen before the chimney exists: SpikePieceInCourse takes the middle of
		 * the X extent, and a chimney reaching ~290 cm past the wall would drag that middle to the
		 * far end, putting the deletion beside the failing part. Choosing first also makes it the
		 * same brick RepairedRegionalSandwich deletes, so the two region problems are comparable.
		 */
		Out.Victim = SpikePieceInCourse(Out.Structure, DeleteCourse, Out.DeleteXCm, Out.DeleteZCm);

		if (Out.Victim == INDEX_NONE)
		{
			OutWhy = FString::Printf(
				TEXT("no brick to delete in course %d of a %d x %d wall"),
				DeleteCourse, Courses, Cells);
			return false;
		}

		if (ChimneyCourses > 0)
		{
			double TopZCm = -TNumericLimits<double>::Max();

			for (int32 Piece = 0; Piece < Out.Structure.NumPieces(); ++Piece)
			{
				if (!Out.Structure.IsPieceRemoved(Piece))
				{
					TopZCm = FMath::Max(TopZCm, Out.Structure.GetPiece(Piece).CentreOfMassCm.Z);
				}
			}

			/*
			 * The root must be a full brick, and mass is how this knows: a flush running-bond
			 * course ends in a half bat (10.25 cm, not 21.5). MakeInterface reconstructs the box
			 * from the standard brick size, which for a half bat would hand the solver a joint
			 * twice the real area, so selecting on mass keeps the reconstruction sound.
			 */
			int32 Root = INDEX_NONE;
			double RootXCm = -TNumericLimits<double>::Max();

			for (int32 Piece = 0; Piece < Out.Structure.NumPieces(); ++Piece)
			{
				if (Out.Structure.IsPieceRemoved(Piece))
				{
					continue;
				}

				const FStructurePiece& Candidate = Out.Structure.GetPiece(Piece);

				if (FMath::Abs(Candidate.CentreOfMassCm.Z - TopZCm) > 0.5 * SpikeCoursePitchCm)
				{
					continue;
				}

				if (FMath::Abs(Candidate.MassKg - SpikeBrickMassKg) > 1.0e-9 * SpikeBrickMassKg)
				{
					continue;
				}

				if (Candidate.CentreOfMassCm.X > RootXCm)
				{
					RootXCm = Candidate.CentreOfMassCm.X;
					Root = Piece;
				}
			}

			if (Root == INDEX_NONE)
			{
				OutWhy = TEXT("the top course holds no FULL brick to stand a chimney on");
				return false;
			}

			const FVector FullExtentCm =
				FVector(SpikeBrickLengthCm, SpikeBrickWidthCm, SpikeBrickHeightCm) * 0.5;

			TArray<int32> Handles;
			TArray<FPieceBox> Boxes;

			FPieceBox RootBox;
			RootBox.CentreCm = Out.Structure.GetPiece(Root).CentreOfMassCm;
			RootBox.ExtentCm = FullExtentCm;

			Handles.Add(Root);
			Boxes.Add(RootBox);

			Out.RootXCm = RootBox.CentreCm.X;
			Out.RootZCm = RootBox.CentreCm.Z;

			for (int32 Course = 0; Course < ChimneyCourses; ++Course)
			{
				FPieceBox Box;
				Box.ExtentCm = FullExtentCm;
				Box.CentreCm = FVector(
					RootBox.CentreCm.X + double(Course) * SpikeChimneyLeanCm,
					RootBox.CentreCm.Y,
					RootBox.CentreCm.Z + double(Course + 1) * SpikeCoursePitchCm);

				const int32 Handle = Out.Structure.AddPiece(
					SpikeBrickMassKg, /*bIsGrounded*/ false, Box.CentreCm);

				if (Handle == INDEX_NONE)
				{
					OutWhy = FString::Printf(TEXT("chimney course %d was refused"), Course);
					return false;
				}

				Handles.Add(Handle);
				Boxes.Add(Box);
			}

			/*
			 * All pairs, as the leaning stack's builder does, so the joint count is a check: an
			 * N-course chimney must emit N joints (root bed plus N-1 chain beds), and a lost
			 * overlap shows up as a wrong count rather than a quietly missing joint.
			 */
			int32 Made = 0;

			for (int32 First = 0; First < Handles.Num(); ++First)
			{
				for (int32 Second = First + 1; Second < Handles.Num(); ++Second)
				{
					FConnection Joint;

					if (MakeInterface(Handles[First], Boxes[First], Handles[Second], Boxes[Second],
							SpikeJointCm, GeneralPurposeMortar, Joint))
					{
						Out.Structure.AddConnection(Joint);
						++Made;
					}
				}
			}

			if (Made != ChimneyCourses)
			{
				OutWhy = FString::Printf(
					TEXT("the chimney emitted %d joints for %d courses"), Made, ChimneyCourses);
				return false;
			}

			Out.ChimneyBlocks = ChimneyCourses;
		}

		if (!Out.Structure.RemovePiece(Out.Victim))
		{
			OutWhy = FString::Printf(TEXT("piece %d could not be removed"), Out.Victim);
			return false;
		}

		return true;
	}

	/**
	 * The sub-1.0 wall's pins. Every one is INDEX_NONE or negative until measured; an unmeasured
	 * pin is this slice's red.
	 */
	struct FSubUnityPins
	{
		/*
		 * Measured 2026-08-16. The plain half is RepairedRegionalSandwich's fixture at its own
		 * pinned numbers (149 blocks, 965 pivots), measured here in the same run not transcribed,
		 * because the argument is that the two are the same wall.
		 */
		int32 PlainBlocks = 149;
		int32 PlainJoints = 385;
		int32 PlainGlobalPivots = 965;

		int32 CompositeBlocks = 179;
		int32 CompositeJoints = 415;
		int32 CompositeGlobalPivots = 1308;

		/*
		 * lambda* of the whole composite, gravity-live, file-wide +/-2e-5 window. Measured
		 * 0.44049301907204619, inside the bare 30-course stack's [0.440484, 0.440502] — Q1: the
		 * binding joint is inside the chain, which does not care whether it stands on earth or a
		 * wall. The named fallback (root joint at ~1.5) did not fire.
		 */
		double CompositeLambdaLo = 0.4404842;
		double CompositeLambdaHi = 0.4405019;

		/**
		 * How many of the six strip widths closed while the structure has no equilibrium. Measured
		 * 4: w = 0, 1, 2 and 3, the smallest 41 of 179 blocks.
		 */
		int32 FalseCertificates = 4;

		/*
		 * The certified strip's own lambda*, how false the certificate is. Measured
		 * 621.95652149729517 against 0.44049, 1,412x, where Part D's refuted form was wrong by 41.96x.
		 */
		double StripLambdaLo = 621.9441;
		double StripLambdaHi = 621.9689;

		/*
		 * The contrast strip, cut at the chimney's root instead of the deletion: the chimney above
		 * has no ground path, so its 27 blocks are charged, the pessimistic side goes infeasible
		 * and nothing is certified. The repair does real work where it can see the failing material.
		 */
		int32 ContrastBlocks = 38;
		int32 ContrastCharged = 27;
		int32 ContrastOptimistic = 1;
		int32 ContrastPessimistic = 0;
	};
}

/*
 * TEST 1 — the feasibility reformulation's cost.
 *
 * The lambda* pose is almost all of it (wall-01 alone is 280 s); the group cost 344 s measured
 * 2026-08-15, dead poses 53 s of it, so opt-in full tier. The 2026-08-16 trim: four rows
 * (corbel D, wall-06, wall-15, wall-18) no longer re-solve the live pose, since
 * OracleSweepFull.RigidBlock.WallsAndLadders pins each lambda* bit-identically in the same tier
 * (344 s -> 313 s). Wall-01, the gate fixture and both stacks stay live (see
 * FCostRow::bSolveLivePose). No dead solve was touched, so the pooled early-exit totals and
 * every verdict are bit-identical. Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleFeasibilityCostTest,
	"OracleSweepFull.RigidBlock.FeasibilityReformulationCost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleFeasibilityCostTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleFeasibilitySpikeSupport;

	TArray<FCostRow> Rows;

	/*
	 * Every row carries its prediction beside its measurement: a prediction deleted once checked
	 * leaves nobody able to tell an agreement from a transcription. Measured 2026-08-15, HEAD 9dcbd76.
	 */

	Rows.Add({ TEXT("leaning stack, 5 courses"),
		TEXT("PREDICTED 10-40 dead-pose pivots; feasible (lambda* 31.2). MEASURED 22 — HIT"),
		[](FStructure& Out, FString& Why) { return SpikeBuildLeaningStack(5, Out, Why); },
		5, 4, 31.2067, 31.2081, 4, 22, 21, 21, 1 });

	Rows.Add({ TEXT("leaning stack, 30 courses"),
		TEXT("PREDICTED 100-400 dead-pose pivots; INFEASIBLE (lambda* 0.4405) — the arm ")
		TEXT("that stops the boolean being vacuous. MEASURED 163, infeasible — HIT both ways"),
		[](FStructure& Out, FString& Why) { return SpikeBuildLeaningStack(30, Out, Why); },
		30, 29, 0.440484, 0.440502, 29, 163, 163, INDEX_NONE, 0 });

	Rows.Add({ TEXT("8x10 intact wall (THE GATE FIXTURE, 84 blocks)"),
		TEXT("PREDICTED 400-800 dead-pose pivots against 1,942 live, ~3-5x, ~0.15 s ")
		TEXT("against a 50 ms target. MEASURED 491 pivots (HIT) against 2,606 live — the ")
		TEXT("1,942 in the prediction was a 2026-08-12 characteristic-era figure and ")
		TEXT("today's live path is 2,606. 5.31x on pivots, 11.5x on seconds, 0.065 s"),
		[](FStructure& Out, FString& Why) { return SpikeBuildIntactWall(8, 10, Out, Why); },
		84, 207, 1128.4217, 1128.4670, 2606, 491, 490, 487, 1 });

	Rows.Add({ TEXT("corbel D, ten steps with counterweight"),
		TEXT("PREDICTED 400-900 dead-pose pivots against 8,439 live, ~10-20x. MEASURED ")
		TEXT("1,022 — MISS, 14% over the top of the range; 9.29x on pivots, 37.3x on ")
		TEXT("seconds. LIVE POSE NOT RE-SOLVED HERE since 2026-08-16: lambda* 161.14 is ")
		TEXT("pinned in WallsAndLadders at [161.1375, 161.1439] and the 9,490-pivot live ")
		TEXT("solve was 10.2 s of duplication"),
		[](FStructure& Out, FString& Why)
		{
			return SpikeBuildScenario(TEXT("corbel-d-10-counterweight"), Out, Why);
		},
		90, 200, -1.0, -1.0, INDEX_NONE, 1022, 1021, 1021, 1, /*bSolveLivePose*/ false });

	Rows.Add({ TEXT("wall-18 stack bond, one brick out"),
		TEXT("PREDICTED 400-900 dead-pose pivots against 791 live — ABOUT 1x, the row ")
		TEXT("chosen to test the design's 10-100x estimate where the climb was already ")
		TEXT("short. MEASURED 468 dead pivots (HIT) but 3,885 LIVE, not 791 — THE ")
		TEXT("PREDICTION'S PREMISE WAS STALE (791 was a 2026-08-12 Dantzig-path, ")
		TEXT("characteristic-era reading), so 8.30x on pivots and 44.6x on seconds. No ")
		TEXT("fixture in this list has a short climb today and the 'about 1x' hypothesis ")
		TEXT("is UNTESTED rather than refuted. LIVE POSE NOT RE-SOLVED HERE since ")
		TEXT("2026-08-16: lambda* 897.73 is pinned in WallsAndLadders at [897.712, ")
		TEXT("897.749]"),
		[](FStructure& Out, FString& Why)
		{
			return SpikeBuildScenario(TEXT("wall-18"), Out, Why);
		},
		119, 203, -1.0, -1.0, INDEX_NONE, 468, 467, 449, 1, /*bSolveLivePose*/ false });

	Rows.Add({ TEXT("wall-15 header with six courses on top"),
		TEXT("PREDICTED 500-1,200 dead-pose pivots against 3,745 live, ~3-7x. MEASURED ")
		TEXT("803 (HIT); 7.57x on pivots against 6,076 live, 18.7x on seconds. LIVE POSE ")
		TEXT("NOT RE-SOLVED HERE since 2026-08-16: lambda* 868.6237 is pinned in ")
		TEXT("WallsAndLadders at [868.62287, 868.62461], and as the 15/16 cross-row ")
		TEXT("identity beside it"),
		[](FStructure& Out, FString& Why)
		{
			return SpikeBuildScenario(TEXT("wall-15"), Out, Why);
		},
		125, 320, -1.0, -1.0, INDEX_NONE, 803, 802, 799, 1, /*bSolveLivePose*/ false });

	Rows.Add({ TEXT("wall-06 two-cell opening, deep cover"),
		TEXT("PREDICTED 600-1,500 dead-pose pivots against 8,819 live, ~6-15x. MEASURED ")
		TEXT("876 (HIT); 16.2x on pivots against 14,209 live, 51.2x on seconds — the ")
		TEXT("largest speedup in the set, and the one the 2026-08-16 trim makes a ")
		TEXT("CROSS-RUN reading: LIVE POSE NOT RE-SOLVED HERE, lambda* 634.58 pinned in ")
		TEXT("WallsAndLadders at [634.570, 634.596], 16.1 s of duplication dropped"),
		[](FStructure& Out, FString& Why)
		{
			return SpikeBuildScenario(TEXT("wall-06"), Out, Why);
		},
		146, 372, -1.0, -1.0, INDEX_NONE, 876, 875, 866, 1, /*bSolveLivePose*/ false });

	Rows.Add({ TEXT("wall-01 thirty courses (THE HEADLINE, 375 blocks)"),
		TEXT("PREDICTED 1,500-4,000 dead-pose pivots against 58,605 live, ~15-40x. ")
		TEXT("MEASURED 5,407 — MISS, 35% over the top; 10.9x on pivots and 10.8x on ")
		TEXT("seconds, 280.5 s -> 26.0 s. The phase-1 pivot count is NOT O(rows) with a ")
		TEXT("small constant: it is 2.2x the equality-row count at 84 blocks and ~5x it ")
		TEXT("here, so the lever WEAKENS with scale rather than strengthening"),
		[](FStructure& Out, FString& Why)
		{
			return SpikeBuildScenario(TEXT("wall-01"), Out, Why);
		},
		375, 1030, 272.1994, 272.2104, 58806, 5407, 5406, 5381, 1 });

	/* The gate fixture's own reading, kept out of the loop for the arithmetic below. */
	double GateDeadSeconds = -1.0;
	double GateLiveSeconds = -1.0;

	/*
	 * The early exit's pooled totals, accumulated across rows and pinned after the loop. The
	 * per-row pins already fix these sums; pinning the pooled 0.63% gives the conclusion its own
	 * assertion.
	 */
	int32 PooledPhaseOnePivots = 0;
	int32 PooledSkippedPivots = 0;

	for (const FCostRow& Row : Rows)
	{
		FStructure Structure;
		FString Why;

		/*
		 * Each call is its own statement before its message: folding a call that writes Why into
		 * the Printf that reads it is unsequenced, and MSVC evaluates the condition last (TRAPS).
		 */
		const bool bLaid = Row.Build(Structure, Why);

		if (!TestTrue(
				*FString::Printf(TEXT("%s: the producer must lay it (it said: %s)"),
					Row.Name, *Why),
				bLaid))
		{
			continue;
		}

		FOracleProblem Live;

		const bool bBridged = BuildRigidBlockProblem(Structure, Live, Why);

		if (!TestTrue(
				*FString::Printf(TEXT("%s: the bridge must represent it (it said: %s)"),
					Row.Name, *Why),
				bBridged))
		{
			continue;
		}

		/*
		 * The two poses are one problem and one flag. Copying rather than rebuilding: a
		 * separately-built dead pose would make a producer difference read as a reformulation one.
		 */
		FOracleProblem Dead = Live;
		Dead.bGravityIsLive = false;

		/*
		 * The live pose is solved only where its answer is not already pinned in the same tier
		 * (see FCostRow::bSolveLivePose). A skipped row leaves LiveRead default and every reader
		 * below is inside the same flag; the log says NOT RE-SOLVED, not a zero that reads like a
		 * refusal.
		 */
		const FPoseReading LiveRead = Row.bSolveLivePose ? SpikeSolve(Live) : FPoseReading();
		const FPoseReading DeadRead = SpikeSolve(Dead);

		/*
		 * The early-exit saving, printed per row: the fraction of phase-1 pivots spent after first
		 * feasibility, which stopping there would skip. Negative means the infeasible arm (never
		 * reached feasibility), reported -1 rather than a plausible zero.
		 */
		const double EarlyExitSaving =
			DeadRead.PhaseOnePivots > 0 && DeadRead.PivotsToFirstFeasible >= 0
				? double(DeadRead.PhaseOnePivots - DeadRead.PivotsToFirstFeasible)
					/ double(DeadRead.PhaseOnePivots)
				: -1.0;

		const FString LiveText = Row.bSolveLivePose
			? FString::Printf(
				TEXT("LIVE lambda*=%.17g answered=%d pivots=%d secs=%.3f | pivot ratio %.3f ")
				TEXT("| time ratio %.3f | LIVE phase1=%d firstfeasible=%d"),
				LiveRead.Lambda, LiveRead.bAnswered ? 1 : 0, LiveRead.Pivots, LiveRead.Seconds,
				DeadRead.Pivots > 0 ? double(LiveRead.Pivots) / double(DeadRead.Pivots) : 0.0,
				DeadRead.Seconds > 0.0 ? LiveRead.Seconds / DeadRead.Seconds : 0.0,
				LiveRead.PhaseOnePivots, LiveRead.PivotsToFirstFeasible)
			: FString(
				TEXT("LIVE POSE NOT RE-SOLVED — lambda* is pinned in ")
				TEXT("OracleSweepFull.RigidBlock.WallsAndLadders, which runs in the same tier"));

		const FString Line = FString::Printf(
			TEXT("SPIKE %s: blocks=%d joints=%d | %s ")
			TEXT("| DEAD feasible=%d lambda=%.17g answered=%d pivots=%d secs=%.3f ")
			TEXT("| DEAD phase1=%d firstfeasible=%d ")
			TEXT("earlyexit saving %.4f | %s%s%s"),
			Row.Name, Live.Blocks.Num(), Live.Joints.Num(), *LiveText,
			SpikeIsFeasible(DeadRead) ? 1 : 0, DeadRead.Lambda, DeadRead.bAnswered ? 1 : 0,
			DeadRead.Pivots, DeadRead.Seconds,
			DeadRead.PhaseOnePivots, DeadRead.PivotsToFirstFeasible, EarlyExitSaving,
			Row.Prediction,
			DeadRead.WhyNot.IsEmpty() ? TEXT("") : TEXT(" | dead whynot: "),
			DeadRead.WhyNot.IsEmpty() ? TEXT("") : *DeadRead.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		if (FCString::Strstr(Row.Name, TEXT("GATE FIXTURE")) != nullptr)
		{
			GateDeadSeconds = DeadRead.Seconds;
			GateLiveSeconds = LiveRead.Seconds;
		}

		/* ---- The size pins, first, so a row cannot secretly solve another wall. ---- */

		if (Row.Blocks == INDEX_NONE || Row.Joints == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("%s: UNMEASURED SIZE — pin blocks=%d joints=%d"),
				Row.Name, Live.Blocks.Num(), Live.Joints.Num()));
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("%s: block count"), Row.Name),
				Live.Blocks.Num(), Row.Blocks);
			TestEqual(*FString::Printf(TEXT("%s: joint count"), Row.Name),
				Live.Joints.Num(), Row.Joints);
		}

		/* ---- The live pose, whose lambda* is the cross-validation's other half. ---- */

		if (Row.bSolveLivePose)
		{
			if (!TestTrue(
					*FString::Printf(TEXT("%s: the LIVE pose must answer (it said: %s)"),
						Row.Name, *LiveRead.WhyNot),
					LiveRead.bAnswered))
			{
				continue;
			}

			if (Row.LambdaLo < 0.0)
			{
				AddError(FString::Printf(
					TEXT("%s: UNMEASURED lambda* — measured %.17g"), Row.Name, LiveRead.Lambda));
			}
			else
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: lambda* must lie in [%.9g, %.9g] and was %.17g"),
						Row.Name, Row.LambdaLo, Row.LambdaHi, LiveRead.Lambda),
					LiveRead.Lambda >= Row.LambdaLo && LiveRead.Lambda <= Row.LambdaHi);
			}
		}

		/* ---- The dead pose: the feasibility answer, and what it costs. ---- */

		if (!TestTrue(
				*FString::Printf(TEXT("%s: the DEAD pose must answer (it said: %s)"),
					Row.Name, *DeadRead.WhyNot),
				DeadRead.bAnswered))
		{
			continue;
		}

		/*
		 * How a test knows the pose is dead. With no live load the lambda column's only constraint
		 * is the cap, so feasible reports exactly LambdaCap and infeasible exactly 0; anything
		 * between means a live load survived into a dead pose.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: a DEAD pose carries no live load, so lambda must be exactly ")
				TEXT("LambdaCap (%.17g) or exactly 0, and was %.17g"),
				Row.Name, LambdaCap, DeadRead.Lambda),
			DeadRead.Lambda == LambdaCap || DeadRead.Lambda == 0.0);

		/*
		 * The cross-validation: an admissible equilibrium at lambda = 1 exists exactly when the
		 * lambda* pose reads >= 1. Within-run, so it is inside the live-pose flag rather than run
		 * against a transcribed lambda* (comparing a measured boolean to a typed number would
		 * assert the typing). The rows span both arms, so the boolean is not vacuous.
		 */
		const bool bFeasible = SpikeIsFeasible(DeadRead);

		if (Row.bSolveLivePose)
		{
			const bool bStandsByLambda = LiveRead.Lambda >= 1.0;

			TestTrue(
				*FString::Printf(
					TEXT("%s: CROSS-VALIDATION — feasibility at lambda = 1 (%d) must agree with ")
					TEXT("lambda* >= 1 (%d, lambda* = %.17g). A disagreement means the ")
					TEXT("reformulation does not pose the question the design says it poses."),
					Row.Name, bFeasible ? 1 : 0, bStandsByLambda ? 1 : 0, LiveRead.Lambda),
				bFeasible == bStandsByLambda);
		}

		if (Row.Feasible == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("%s: UNMEASURED FEASIBILITY — measured %d"), Row.Name, bFeasible ? 1 : 0));
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("%s: feasibility verdict"), Row.Name),
				bFeasible ? 1 : 0, Row.Feasible);
		}

		/*
		 * The two pivot pins are separate guards: a row not re-solving the live pose has no live
		 * pivot count, and folding them together would take the dead pin — the whole cost
		 * measurement — with it.
		 */
		if (Row.bSolveLivePose)
		{
			if (Row.LivePivots == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s: UNMEASURED LIVE COST — pin the live pivot count. Measured: ")
					TEXT("%d pivots in %.3f s. %s"),
					Row.Name, LiveRead.Pivots, LiveRead.Seconds, Row.Prediction));
			}
			else
			{
				TestEqual(
					*FString::Printf(
						TEXT("%s: the LIVE pose's pivot count is deterministic and pinned"),
						Row.Name),
					LiveRead.Pivots, Row.LivePivots);
			}
		}

		if (Row.DeadPivots == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("%s: UNMEASURED DEAD COST — pin the dead pivot count. Measured: ")
				TEXT("%d pivots in %.3f s. %s"),
				Row.Name, DeadRead.Pivots, DeadRead.Seconds, Row.Prediction));
		}
		else
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: the DEAD pose's pivot count is deterministic and pinned — this ")
					TEXT("is the reformulation's cost and the whole of what slice 0b buys"),
					Row.Name),
				DeadRead.Pivots, Row.DeadPivots);
		}

		/* ---- The early exit, pinned by VALUE and not merely by ordering. ---- */

		/*
		 * Why exact pins, not a range. Both are deterministic integers, free on a fixture solved
		 * anyway, and they decide a published conclusion (0.63%, refuting §5.2). A population-and-
		 * ordering check (0 <= first <= phase1) passes on a wrong-but-in-range index: move
		 * wall-18's 449 to 40 and the lever reads 91%, the conclusion inverts, nothing goes red.
		 */
		if (Row.DeadPhaseOnePivots == INDEX_NONE || Row.DeadFirstFeasible == SpikeUnmeasured)
		{
			AddError(FString::Printf(
				TEXT("%s: UNMEASURED EARLY EXIT — pin phase1=%d firstfeasible=%d"),
				Row.Name, DeadRead.PhaseOnePivots, DeadRead.PivotsToFirstFeasible));
		}
		else
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: the DEAD pose's PHASE-1 pivot count"), Row.Name),
				DeadRead.PhaseOnePivots, Row.DeadPhaseOnePivots);

			TestEqual(
				*FString::Printf(
					TEXT("%s: the pivot at which the DEAD pose FIRST read feasible — the ")
					TEXT("difference from the line above is the whole of what an early exit ")
					TEXT("would save, and INDEX_NONE is the measured answer on an infeasible ")
					TEXT("problem rather than a missing reading"),
					Row.Name),
				DeadRead.PivotsToFirstFeasible, Row.DeadFirstFeasible);

			/*
			 * The ordering property is kept beside the values, not instead: it is the one
			 * thing they cannot say, that the two fields are consistent whatever they read.
			 */
			TestTrue(
				*FString::Printf(
					TEXT("%s: first-feasible (%d) must lie in [0, phase-1 pivots] (%d) or be ")
					TEXT("INDEX_NONE for an infeasible problem"),
					Row.Name, DeadRead.PivotsToFirstFeasible, DeadRead.PhaseOnePivots),
				DeadRead.PivotsToFirstFeasible == INDEX_NONE
					|| (DeadRead.PivotsToFirstFeasible >= 0
						&& DeadRead.PivotsToFirstFeasible <= DeadRead.PhaseOnePivots));

			PooledPhaseOnePivots += DeadRead.PhaseOnePivots;

			if (DeadRead.PivotsToFirstFeasible >= 0)
			{
				PooledSkippedPivots +=
					DeadRead.PhaseOnePivots - DeadRead.PivotsToFirstFeasible;
			}
		}

		/*
		 * The live pose must report 0/0 on every row that solves one: a gravity-live problem is
		 * feasible before a single pivot, so phase 1 never runs. Also a second guard on the pose,
		 * catching a live load leaking into a dead one. Four rows no longer re-solve the live pose.
		 */
		if (!Row.bSolveLivePose)
		{
			continue;
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s: the LIVE pose starts feasible, so phase 1 spends no pivots"),
				Row.Name),
			LiveRead.PhaseOnePivots, 0);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the LIVE pose is feasible at pivot zero, so first-feasible is 0 ")
				TEXT("— not INDEX_NONE, which would mean it never got there"),
				Row.Name),
			LiveRead.PivotsToFirstFeasible, 0);
	}

	/*
	 * The early exit's headline, asserted as one number. 58 of 9,245 pooled (0.63%) is the figure
	 * §5.2 records as refuting the lever. Pinned as a sum so the conclusion has its own assertion:
	 * a change moving two rows opposite ways would leave the per-row pins red like noise, where
	 * this line says what changed.
	 */
	{
		AddInfo(FString::Printf(
			TEXT("EARLY EXIT POOLED: %d of %d phase-1 pivots happen after the problem first ")
			TEXT("reads feasible = %.4f%%. §5.2 predicted 'most of a standing structure's ")
			TEXT("saving'."),
			PooledSkippedPivots, PooledPhaseOnePivots,
			PooledPhaseOnePivots > 0
				? 100.0 * double(PooledSkippedPivots) / double(PooledPhaseOnePivots)
				: 0.0));

		TestEqual(
			TEXT("EARLY EXIT: the pooled phase-1 pivot count across the eight fixtures"),
			PooledPhaseOnePivots, 9245);

		TestEqual(
			TEXT("EARLY EXIT: the pooled pivots an early exit would SKIP — 58 of 9,245, ")
			TEXT("0.63%, which is the measurement that refutes §5.2's largest remaining ")
			TEXT("claim. Phase 1's objective IS the infeasibility sum, so there is no ")
			TEXT("optimality-proof tail to skip and the saving is already spent."),
			PooledSkippedPivots, 58);
	}

	/*
	 * The gate arithmetic, written out where it is measured. §5.3's target: ~50 ms to fit two
	 * solves inside a 100 ms budget, 12x needed against the 0.61 s the 84-block wall cost as a
	 * lambda* solve. Measured 2026-08-15: the same 84 blocks answer feasibility in 0.065 s, a 1.3x
	 * gap, not 12x, with the reformulation alone and no warm start. The kill is conditioned on
	 * "cannot be brought under ~50 ms by the reformulation plus warm starts", and warm starts are
	 * unbuilt, so this does not fire it. Whether 84 blocks is a legitimate unit of work is the
	 * second test's question, and the answer is worse. No timing is pinned; the one assertion below
	 * is an order-of-magnitude ceiling ~30x above the measurement.
	 */
	{
		constexpr double GateCatastropheCeilingSeconds = 2.0;

		AddInfo(FString::Printf(
			TEXT("GATE ARITHMETIC: 84 blocks answer feasibility in %.4f s (lambda* pose ")
			TEXT("%.4f s). Target 0.050 s for one solve / 0.100 s for two. Measured shortfall ")
			TEXT("%.2fx. Design's stated shortfall was 12x."),
			GateDeadSeconds, GateLiveSeconds,
			GateDeadSeconds > 0.0 ? GateDeadSeconds / 0.050 : 0.0));

		TestTrue(
			*FString::Printf(
				TEXT("GATE ORDER OF MAGNITUDE: the 84-block feasibility solve must stay ")
				TEXT("under %.1f s — ~30x the 0.065 s measured, so this is a catastrophe ")
				TEXT("ceiling and not a timing pin — and took %.4f s"),
				GateCatastropheCeilingSeconds, GateDeadSeconds),
			GateDeadSeconds >= 0.0 && GateDeadSeconds < GateCatastropheCeilingSeconds);
	}

	/*
	 * The early exit — the seam, re-solved. §3.2/§5.2: a feasible problem is proved the moment
	 * phase 1's infeasibility sum reaches tolerance. Not measurable from outside, so this was the
	 * slice's one deliberate red until PhaseOnePivots and PivotsToFirstFeasible were populated; the
	 * seam landed in the same slice, so the red is gone. What remains, worth 0.065 s: a second
	 * solve reports the same two numbers. The fields are read from solver state nothing else
	 * depends on, so an un-reset accumulation between solves would show here first.
	 */
	{
		FStructure Wall;
		FString Why;

		const bool bLaid = SpikeBuildIntactWall(8, 10, Wall, Why);

		if (TestTrue(
				*FString::Printf(TEXT("early exit: the gate wall must lay (it said: %s)"), *Why),
				bLaid))
		{
			FOracleProblem Problem;

			const bool bBridged = BuildRigidBlockProblem(Wall, Problem, Why);

			if (TestTrue(
					*FString::Printf(TEXT("early exit: the bridge must represent it (%s)"), *Why),
					bBridged))
			{
				Problem.bGravityIsLive = false;

				const FOracleResult Result = SolveRigidBlock(Problem);

				TestTrue(
					*FString::Printf(
						TEXT("EARLY EXIT SEAM: the re-solve must ANSWER (it said: %s)"),
						*Result.WhyNot),
					Result.bAnswered);

				TestEqual(
					TEXT("EARLY EXIT SEAM: a second solve of the identical gate problem must ")
					TEXT("report the identical phase-1 pivot count (490) — the two fields are ")
					TEXT("read from state nothing else depends on, so a stale accumulation ")
					TEXT("between solves would show here first"),
					Result.PhaseOnePivots, 490);

				TestEqual(
					TEXT("EARLY EXIT SEAM: and the identical first-feasible pivot (487), so ")
					TEXT("the 0.61% saving on this fixture is reproducible rather than a ")
					TEXT("property of one solve's history"),
					Result.PivotsToFirstFeasible, 487);
			}
		}
	}

	return true;
}

/*
 * TEST 2 — the regional sandwich.
 *
 * Cost: one global feasibility solve plus two region solves per radius, three per deletion for
 * the ten-deletion agreement ladder (the third is the whole cut wall, so its certificates are
 * checked not just counted), the leaning-stack arms, and Part D's band in both poses. Measured
 * 2026-08-15: 7.2 s, the ten global solves ~half of it. Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleRegionalSandwichTest,
	"OracleSweepFast.RigidBlock.RegionalSandwich",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleRegionalSandwichTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleFeasibilitySpikeSupport;

	/*
	 * A refusal is not an infeasibility, and nothing else here tells them apart: Refuse() returns
	 * bAnswered = false, lambda = 0, bit-for-bit the dead-load infeasibility arm, so a refused
	 * region solve reads as "no admissible equilibrium" (this test's finding) while every verdict
	 * pin stays green. Not hypothetical: NumericalFailure refused 128-/200-block members a commit
	 * ago, and regions here are 44-149 blocks. Today's readings are clean.
	 */
	const auto MustAnswer = [this](const FPoseReading& Read, const FString& Where)
	{
		return TestTrue(
			*FString::Printf(
				TEXT("%s: the solve must ANSWER — a REFUSAL and an INFEASIBILITY are ")
				TEXT("indistinguishable downstream (both are bAnswered=false, lambda=0), so ")
				TEXT("an unguarded refusal reads as 'no admissible equilibrium'. It said: %s"),
				*Where, Read.WhyNot.IsEmpty() ? TEXT("(nothing)") : *Read.WhyNot),
			Read.bAnswered);
	};

	/* PART A — the radius ladder on one wall, one deletion. */

	constexpr int32 LadderCourses = 12;
	constexpr int32 LadderCells = 12;
	constexpr int32 LadderDeleteCourse = 6;

	/* Pins: INDEX_NONE is UNMEASURED and is this slice's red. */
	const FSandwichPins Pins;

	FStructure Wall;
	FString Why;

	const bool bLadderLaid = SpikeBuildIntactWall(LadderCourses, LadderCells, Wall, Why);

	if (!TestTrue(
			*FString::Printf(TEXT("sandwich: the ladder wall must lay (it said: %s)"), *Why),
			bLadderLaid))
	{
		return true;
	}

	double DeleteXCm = 0.0;
	double DeleteZCm = 0.0;
	const int32 Victim = SpikePieceInCourse(Wall, LadderDeleteCourse, DeleteXCm, DeleteZCm);

	if (!TestTrue(
			TEXT("sandwich: there must be a brick to delete in the chosen course"),
			Victim != INDEX_NONE))
	{
		return true;
	}

	if (!TestTrue(TEXT("sandwich: the brick must be removable"), Wall.RemovePiece(Victim)))
	{
		return true;
	}

	FOracleProblem Full;

	const bool bFullBridged = BuildRigidBlockProblem(Wall, Full, Why);

	if (!TestTrue(
			*FString::Printf(TEXT("sandwich: the bridge must represent the cut wall (%s)"), *Why),
			bFullBridged))
	{
		return true;
	}

	if (Pins.LadderBlocks == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sandwich ladder: UNMEASURED SIZE — %d courses x %d cells with one brick ")
			TEXT("out at course %d gives %d blocks / %d joints; the deleted brick sat at ")
			TEXT("X = %.4f, Z = %.4f"),
			LadderCourses, LadderCells, LadderDeleteCourse,
			Full.Blocks.Num(), Full.Joints.Num(), DeleteXCm, DeleteZCm));
	}
	else
	{
		TestEqual(TEXT("sandwich ladder: block count"), Full.Blocks.Num(), Pins.LadderBlocks);
	}

	/* The truth the sandwich is judged against: the whole cut wall, posed dead. */
	FOracleProblem Global = Full;
	Global.bGravityIsLive = false;

	const FPoseReading GlobalRead = SpikeSolve(Global);
	const bool bGlobalFeasible = SpikeIsFeasible(GlobalRead);

	{
		const FString Line = FString::Printf(
			TEXT("SANDWICH GLOBAL: blocks=%d joints=%d feasible=%d lambda=%.17g pivots=%d ")
			TEXT("secs=%.3f whynot='%s'"),
			Full.Blocks.Num(), Full.Joints.Num(), bGlobalFeasible ? 1 : 0,
			GlobalRead.Lambda, GlobalRead.Pivots, GlobalRead.Seconds, *GlobalRead.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);
	}

	MustAnswer(GlobalRead, TEXT("sandwich ladder: the whole cut wall"));

	if (Pins.GlobalFeasible == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sandwich ladder: UNMEASURED GLOBAL VERDICT — measured feasible=%d"),
			bGlobalFeasible ? 1 : 0));
	}
	else
	{
		TestEqual(TEXT("sandwich ladder: the global verdict"),
			bGlobalFeasible ? 1 : 0, Pins.GlobalFeasible);

		TestEqual(
			TEXT("sandwich ladder: the whole wall's feasibility pivot count, which is what ")
			TEXT("the regional lever has to beat"),
			GlobalRead.Pivots, Pins.GlobalPivots);
	}

	int32 ClosingRadius = INDEX_NONE;
	int32 ClosingRegionBlocks = INDEX_NONE;
	int32 ClosingOptimisticPivots = INDEX_NONE;
	int32 ClosingPessimisticPivots = INDEX_NONE;

	/*
	 * 2/4/8 is the design's ladder; 5/6 are added because it skips the answer. On a 12-course wall
	 * a radius-8 box around a mid-height brick is the whole wall, so "closes at 8" could not be
	 * told from "closes only at the whole structure". The brick is course 6 and the shell reaches
	 * one course past the core, so the first radius touching the foundation is 5, where the
	 * conservation lemma says the free side can first certify. Closing region 142 of 149.
	 */
	TArray<FRadiusPin> RadiusPins;
	RadiusPins.Add({ 2,  44, 1, 0 });
	RadiusPins.Add({ 4, 114, 1, 0 });
	RadiusPins.Add({ 5, 142, 1, 1 });
	RadiusPins.Add({ 6, 149, 1, 1 });
	RadiusPins.Add({ 8, 149, 1, 1 });

	for (const FRadiusPin& Pin : RadiusPins)
	{
		const int32 Radius = Pin.Radius;

		TArray<bool> Mask;
		SpikeBoxMask(Full, DeleteXCm, DeleteZCm, Radius, Radius, Mask);

		FOracleProblem Optimistic;
		FRegionCounts OptimisticCounts;
		SpikeExtractRegion(Full, Mask, /*bGroundTheShell*/ true, Optimistic, OptimisticCounts);

		FOracleProblem Pessimistic;
		FRegionCounts PessimisticCounts;
		SpikeExtractRegion(Full, Mask, /*bGroundTheShell*/ false, Pessimistic, PessimisticCounts);

		const FPoseReading OptimisticRead = SpikeSolve(Optimistic);
		const FPoseReading PessimisticRead = SpikeSolve(Pessimistic);

		const bool bOptimistic = SpikeIsFeasible(OptimisticRead);
		const bool bPessimistic = SpikeIsFeasible(PessimisticRead);
		const bool bCloses = bOptimistic == bPessimistic;

		if (bCloses && ClosingRadius == INDEX_NONE)
		{
			ClosingRadius = Radius;
			ClosingRegionBlocks = OptimisticCounts.Blocks;
			ClosingOptimisticPivots = OptimisticRead.Pivots;
			ClosingPessimisticPivots = PessimisticRead.Pivots;
		}

		const FString Line = FString::Printf(
			TEXT("SANDWICH r=%d: core=%d shell=%d blocks=%d joints=%d | GROUNDED-boundary ")
			TEXT("grounded=%d feasible=%d pivots=%d secs=%.3f whynot='%s' | FREE-boundary ")
			TEXT("grounded=%d feasible=%d pivots=%d secs=%.3f whynot='%s' | closes=%d"),
			Radius, OptimisticCounts.Core, OptimisticCounts.Shell, OptimisticCounts.Blocks,
			OptimisticCounts.Joints, OptimisticCounts.Grounded, bOptimistic ? 1 : 0,
			OptimisticRead.Pivots, OptimisticRead.Seconds, *OptimisticRead.WhyNot,
			PessimisticCounts.Grounded, bPessimistic ? 1 : 0,
			PessimisticRead.Pivots, PessimisticRead.Seconds, *PessimisticRead.WhyNot,
			bCloses ? 1 : 0);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		/* ---- Both solves must have ANSWERED before any verdict is read off them. ---- */

		MustAnswer(OptimisticRead,
			FString::Printf(TEXT("r=%d GROUNDED-boundary region"), Radius));
		MustAnswer(PessimisticRead,
			FString::Printf(TEXT("r=%d FREE-boundary region"), Radius));

		/* ---- The rung's own pins: size first, then both verdicts. ---- */

		TestEqual(
			*FString::Printf(TEXT("r=%d: region block count"), Radius),
			OptimisticCounts.Blocks, Pin.Blocks);

		TestEqual(
			*FString::Printf(
				TEXT("r=%d: the GROUNDED-boundary (optimistic) verdict — infeasible here ")
				TEXT("would certify that the whole structure is infeasible"),
				Radius),
			bOptimistic ? 1 : 0, Pin.Optimistic);

		TestEqual(
			*FString::Printf(
				TEXT("r=%d: the FREE-boundary (pessimistic) verdict — feasible here is what ")
				TEXT("the design says certifies the whole structure"),
				Radius),
			bPessimistic ? 1 : 0, Pin.Pessimistic);

		/*
		 * The conservation lemma, made executable. Sum the vertical equilibrium rows of a
		 * free-boundary region: internal contact forces cancel pairwise and leaving joints are
		 * dropped, so 0 = -W_region. A region with no grounded block has no admissible force system
		 * at any radius, in any wall, so it is asserted rather than argued. It is also the line the
		 * refusal guard protects: a refused solve satisfies !bPessimistic just as an infeasible one
		 * does, so without MustAnswer the lemma reads as confirmed by a solve that never happened.
		 */
		if (PessimisticCounts.Grounded == 0)
		{
			TestTrue(
				*FString::Printf(
					TEXT("r=%d: a FREE-boundary region containing no grounded block cannot be ")
					TEXT("feasible — summing its vertical rows gives 0 = -W_region — and it ")
					TEXT("read feasible=%d"),
					Radius, bPessimistic ? 1 : 0),
				!bPessimistic);
		}

		/*
		 * Where the sandwich closes on this fixture, it agrees with the whole wall. Pinned as a
		 * characterisation of this fixture, not an invariant: Part D measures a closed sandwich
		 * whose certificate is wrong, so "closure implies correctness" is false in general and
		 * asserting it as a law would assert the design's claim rather than check it.
		 */
		if (bCloses)
		{
			TestTrue(
				*FString::Printf(
					TEXT("r=%d: on THIS fixture the closed sandwich reads feasible=%d and the ")
					TEXT("whole wall reads feasible=%d — see PART D for the fixture where a ")
					TEXT("closed sandwich certifies the wrong answer"),
					Radius, bOptimistic ? 1 : 0, bGlobalFeasible ? 1 : 0),
				bOptimistic == bGlobalFeasible);
		}
	}

	if (Pins.ClosingRadius == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sandwich ladder: UNMEASURED CLOSING RADIUS — measured %d at %d blocks ")
			TEXT("(%d + %d pivots); INDEX_NONE means the sandwich did not close at any ")
			TEXT("radius in the ladder"),
			ClosingRadius, ClosingRegionBlocks,
			ClosingOptimisticPivots, ClosingPessimisticPivots));
	}
	else
	{
		TestEqual(TEXT("sandwich ladder: the closing radius"),
			ClosingRadius, Pins.ClosingRadius);

		/*
		 * How big the region is when it closes, the point of a decomposition lever: a radius
		 * without its block count says nothing about neighbourhood vs building.
		 */
		TestEqual(
			TEXT("sandwich ladder: the region at the closing radius is 142 of the wall's ")
			TEXT("149 blocks — a decomposition that must take 95% of the structure before ")
			TEXT("it can certify is not a decomposition"),
			ClosingRegionBlocks, Pins.ClosingRegionBlocks);

		TestEqual(TEXT("sandwich ladder: optimistic pivots at the closing radius"),
			ClosingOptimisticPivots, Pins.ClosingOptimisticPivots);

		TestEqual(TEXT("sandwich ladder: pessimistic pivots at the closing radius"),
			ClosingPessimisticPivots, Pins.ClosingPessimisticPivots);

		/*
		 * The cost comparison, in pivots (deterministic; seconds are not). At the closing radius
		 * the two solves cost 753 + 872 = 1,625 pivots against 965 for the whole wall, 1.68x the
		 * work it avoids. A strict inequality, not a ratio, states the finding not the last digits.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("sandwich ladder: THE SANDWICH COSTS MORE THAN THE GLOBAL SOLVE — its ")
				TEXT("two solves at the closing radius take %d + %d = %d pivots against the ")
				TEXT("whole wall's %d. If this ever reverses, the lever has become worth ")
				TEXT("something and this line is where that is noticed."),
				ClosingOptimisticPivots, ClosingPessimisticPivots,
				ClosingOptimisticPivots + ClosingPessimisticPivots, GlobalRead.Pivots),
			ClosingOptimisticPivots + ClosingPessimisticPivots > GlobalRead.Pivots);
	}

	/*
	 * PART B — R2's question: ten deletions, radius fixed at 4. If the free-boundary side never
	 * certifies, the sandwich degenerates to "grow to the whole structure" and the lever is
	 * worthless. A fixed ladder of the same ten bricks every run, each printed, so a disagreeing
	 * row lifts out into a named regression fixture.
	 */

	constexpr int32 AgreementRadius = 4;

	int32 Agreements = 0;

	for (int32 Course = 1; Course <= 10; ++Course)
	{
		FStructure Cut;
		FString CutWhy;

		const bool bCutLaid = SpikeBuildIntactWall(LadderCourses, LadderCells, Cut, CutWhy);

		if (!TestTrue(
				*FString::Printf(TEXT("agreement course %d: the wall must lay (%s)"),
					Course, *CutWhy),
				bCutLaid))
		{
			continue;
		}

		double XCm = 0.0;
		double ZCm = 0.0;
		const int32 Brick = SpikePieceInCourse(Cut, Course, XCm, ZCm);
		const bool bRemoved = Brick != INDEX_NONE && Cut.RemovePiece(Brick);

		if (!TestTrue(
				*FString::Printf(TEXT("agreement course %d: a brick must exist there"), Course),
				bRemoved))
		{
			continue;
		}

		FOracleProblem CutProblem;

		const bool bCutBridged = BuildRigidBlockProblem(Cut, CutProblem, CutWhy);

		if (!TestTrue(
				*FString::Printf(TEXT("agreement course %d: the bridge must represent it (%s)"),
					Course, *CutWhy),
				bCutBridged))
		{
			continue;
		}

		TArray<bool> Mask;
		SpikeBoxMask(CutProblem, XCm, ZCm, AgreementRadius, AgreementRadius, Mask);

		FOracleProblem Optimistic;
		FRegionCounts OptimisticCounts;
		SpikeExtractRegion(CutProblem, Mask, true, Optimistic, OptimisticCounts);

		FOracleProblem Pessimistic;
		FRegionCounts PessimisticCounts;
		SpikeExtractRegion(CutProblem, Mask, false, Pessimistic, PessimisticCounts);

		/*
		 * The whole cut wall, solved per deletion: a certificate is only worth its answer. Part A
		 * checks its closed sandwich against the global verdict and Part B did not, leaving five
		 * certificates of nothing. One 149-block dead solve per course.
		 */
		FOracleProblem CutGlobal = CutProblem;
		CutGlobal.bGravityIsLive = false;

		const FPoseReading CutGlobalRead = SpikeSolve(CutGlobal);
		const bool bCutGlobalFeasible = SpikeIsFeasible(CutGlobalRead);

		const FPoseReading OptimisticRead = SpikeSolve(Optimistic);
		const FPoseReading PessimisticRead = SpikeSolve(Pessimistic);

		const bool bOptimistic = SpikeIsFeasible(OptimisticRead);
		const bool bPessimistic = SpikeIsFeasible(PessimisticRead);
		const bool bAgree = bOptimistic == bPessimistic;

		if (bAgree)
		{
			++Agreements;
		}

		const FString Line = FString::Printf(
			TEXT("SANDWICH AGREEMENT course=%d (brick at X=%.4f Z=%.4f): blocks=%d ")
			TEXT("grounded(grounded-boundary)=%d grounded(free-boundary)=%d | optimistic=%d ")
			TEXT("pessimistic=%d agree=%d | global=%d | secs %.3f + %.3f | whynot opt='%s' ")
			TEXT("pess='%s' global='%s'"),
			Course, XCm, ZCm, OptimisticCounts.Blocks, OptimisticCounts.Grounded,
			PessimisticCounts.Grounded, bOptimistic ? 1 : 0, bPessimistic ? 1 : 0,
			bAgree ? 1 : 0, bCutGlobalFeasible ? 1 : 0,
			OptimisticRead.Seconds, PessimisticRead.Seconds,
			*OptimisticRead.WhyNot, *PessimisticRead.WhyNot, *CutGlobalRead.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		MustAnswer(OptimisticRead,
			FString::Printf(TEXT("agreement course %d GROUNDED-boundary region"), Course));
		MustAnswer(PessimisticRead,
			FString::Printf(TEXT("agreement course %d FREE-boundary region"), Course));
		MustAnswer(CutGlobalRead,
			FString::Printf(TEXT("agreement course %d whole cut wall"), Course));

		/*
		 * Where this deletion's sandwich closes, what it certifies must hold. A characterisation of
		 * these ten fixtures, not the law "closure implies correctness" (Part D has a closed
		 * sandwich wrong by ~40x). If one of these five ever certifies wrong, this line says so.
		 */
		if (bAgree)
		{
			TestTrue(
				*FString::Printf(
					TEXT("agreement course %d: the closed sandwich certifies feasible=%d and ")
					TEXT("the whole cut wall reads feasible=%d"),
					Course, bOptimistic ? 1 : 0, bCutGlobalFeasible ? 1 : 0),
				bOptimistic == bCutGlobalFeasible);
		}
	}

	if (Pins.Agreements == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sandwich agreement: UNMEASURED — at radius %d over ten deletions the two ")
			TEXT("sides agreed %d times. Revision 1 of the derivation record predicted 4 ")
			TEXT("(courses 1-4, the ones whose region reaches the ground)."),
			AgreementRadius, Agreements));
	}
	else
	{
		TestEqual(TEXT("sandwich agreement: how many of ten deletions certify at radius 4"),
			Agreements, Pins.Agreements);
	}

	/*
	 * PART C — the other arm: a structure that genuinely has no equilibrium. Every wall fixture is
	 * LP-feasible, so Parts A and B only exercise the standing side. The 30-course stack is the one
	 * cheap sub-1.0 fixture (lambda* 0.4405), a chain, so "radius in courses" is exact. Measured:
	 * the smallest radius at which the optimistic side (boundary grounded) is still infeasible,
	 * where a region can certify a collapse.
	 */

	FStructure Stack;
	FString StackWhy;

	const bool bStackLaid = SpikeBuildLeaningStack(30, Stack, StackWhy);

	if (TestTrue(
			*FString::Printf(TEXT("stack sandwich: the 30-course stack must lay (%s)"), *StackWhy),
			bStackLaid))
	{
		FOracleProblem StackProblem;

		const bool bStackBridged = BuildRigidBlockProblem(Stack, StackProblem, StackWhy);

		if (TestTrue(
				*FString::Printf(TEXT("stack sandwich: the bridge must represent it (%s)"),
					*StackWhy),
				bStackBridged))
		{
			const double MiddleZCm =
				SpikeBrickHeightCm / 2.0 + 15.0 * SpikeCoursePitchCm;

			int32 CertifyingRadius = INDEX_NONE;
			int32 CertifyingRegionBlocks = INDEX_NONE;

			const int32 StackRadii[] = { 2, 4, 8, 10, 12, 13, 14, 15 };

			for (const int32 Radius : StackRadii)
			{
				TArray<bool> Mask;
				SpikeCourseBandMask(StackProblem, MiddleZCm, Radius, Mask);

				FOracleProblem Optimistic;
				FRegionCounts OptimisticCounts;
				SpikeExtractRegion(StackProblem, Mask, true, Optimistic, OptimisticCounts);

				FOracleProblem Pessimistic;
				FRegionCounts PessimisticCounts;
				SpikeExtractRegion(StackProblem, Mask, false, Pessimistic, PessimisticCounts);

				const FPoseReading OptimisticRead = SpikeSolve(Optimistic);
				const FPoseReading PessimisticRead = SpikeSolve(Pessimistic);

				const bool bOptimistic = SpikeIsFeasible(OptimisticRead);
				const bool bPessimistic = SpikeIsFeasible(PessimisticRead);

				if (!bOptimistic && CertifyingRadius == INDEX_NONE)
				{
					CertifyingRadius = Radius;
					CertifyingRegionBlocks = OptimisticCounts.Blocks;
				}

				const FString Line = FString::Printf(
					TEXT("STACK SANDWICH r=%d: core=%d shell=%d blocks=%d joints=%d | ")
					TEXT("optimistic feasible=%d pivots=%d | pessimistic feasible=%d ")
					TEXT("pivots=%d | secs %.3f + %.3f | whynot opt='%s' pess='%s'"),
					Radius, OptimisticCounts.Core, OptimisticCounts.Shell,
					OptimisticCounts.Blocks, OptimisticCounts.Joints,
					bOptimistic ? 1 : 0, OptimisticRead.Pivots,
					bPessimistic ? 1 : 0, PessimisticRead.Pivots,
					OptimisticRead.Seconds, PessimisticRead.Seconds,
					*OptimisticRead.WhyNot, *PessimisticRead.WhyNot);

				UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
				AddInfo(Line);

				/*
				 * The certifying radius is read off the optimistic side going infeasible, so a
				 * refusal here would manufacture a certificate from a failed solve.
				 */
				MustAnswer(OptimisticRead,
					FString::Printf(TEXT("stack r=%d GROUNDED-boundary region"), Radius));
				MustAnswer(PessimisticRead,
					FString::Printf(TEXT("stack r=%d FREE-boundary region"), Radius));
			}

			if (Pins.StackCertifyingRadius == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("stack sandwich: UNMEASURED — the smallest radius at which the ")
					TEXT("OPTIMISTIC region is still infeasible (and so certifies the ")
					TEXT("collapse) measured %d, at %d blocks"),
					CertifyingRadius, CertifyingRegionBlocks));
			}
			else
			{
				TestEqual(TEXT("stack sandwich: the certifying radius"),
					CertifyingRadius, Pins.StackCertifyingRadius);

				/*
				 * The region at that radius is the whole structure, all 30 blocks. The optimistic
				 * side is still feasible at radius 13 (29 of 30), so no proper subset certifies
				 * the collapse: for a global mechanism there is no smaller region.
				 */
				TestEqual(
					TEXT("stack sandwich: the certifying region is all 30 blocks — the WHOLE ")
					TEXT("structure; the 29-block region one radius below is still feasible"),
					CertifyingRegionBlocks, Pins.StackCertifyingRegionBlocks);

				TestEqual(
					TEXT("stack sandwich: and 30 blocks IS the whole fixture, so 'certifies ")
					TEXT("at radius 14' means 'certifies only with everything'"),
					CertifyingRegionBlocks, StackProblem.Blocks.Num());
			}

			/*
			 * PART D — the free boundary is not pessimistic, and a closed sandwich can certify
			 * the wrong answer.
			 *
			 * §5.3 justifies the free-boundary side as "if this problem is feasible, the full
			 * structure is, because the found force system extends." It does not: dropping material
			 * outside the region removes its restraint and its weight together, so the free-boundary
			 * region is a bound in neither direction. The cheapest witness is the 30-course stack
			 * (lambda* = 0.4405): its bottom band (courses 0..5, grounded, the 24 above dropped) is
			 * feasible both free- and grounded-boundary, so the sandwich closes and certifies "the
			 * full structure is feasible" — it is not.
			 *
			 * A boolean trio only says "wrong", so two things were added (§9.5): the band's own live
			 * lambda* (wrong by a factor no extractor tightening closes), and X6 (an extractor
			 * keeping out-of-region blocks must turn the band infeasible).
			 *
			 * Re-aimed 2026-08-15 when the repair landed. The rows above still measure the unrepaired
			 * extractor (the 41.96x false certificate must stay executable) and must not be "fixed"
			 * to restore green; the rows below measure the repaired extractor on the identical band
			 * and claim it reads infeasible, so no certificate is issued. (A pin notices only changes
			 * through the code it calls, so the repair built beside SpikeExtractRegion left Part D
			 * green until re-aimed deliberately.)
			 */
			{
				/* Core = courses 0..4, shell = course 5; twenty-four courses dropped. */
				constexpr int32 BandCentreCourse = 2;
				constexpr int32 BandRadius = 2;

				const double BandCentreZCm = SpikeBrickHeightCm / 2.0
					+ double(BandCentreCourse) * SpikeCoursePitchCm;

				TArray<bool> BandMask;
				SpikeCourseBandMask(StackProblem, BandCentreZCm, BandRadius, BandMask);

				FOracleProblem BandFree;
				FRegionCounts BandFreeCounts;
				SpikeExtractRegion(StackProblem, BandMask, false, BandFree, BandFreeCounts);

				FOracleProblem BandGrounded;
				FRegionCounts BandGroundedCounts;
				SpikeExtractRegion(StackProblem, BandMask, true, BandGrounded, BandGroundedCounts);

				const FPoseReading BandFreeRead = SpikeSolve(BandFree);
				const FPoseReading BandGroundedRead = SpikeSolve(BandGrounded);

				FOracleProblem WholeStack = StackProblem;
				WholeStack.bGravityIsLive = false;
				const FPoseReading WholeRead = SpikeSolve(WholeStack);

				/*
				 * The same free-boundary band posed live, the only way to ask how feasible the
				 * false certificate's region is. One flag apart, copying not rebuilding.
				 */
				FOracleProblem BandFreeLive = BandFree;
				BandFreeLive.bGravityIsLive = true;
				const FPoseReading BandLiveRead = SpikeSolve(BandFreeLive);

				const bool bBandFree = SpikeIsFeasible(BandFreeRead);
				const bool bBandGrounded = SpikeIsFeasible(BandGroundedRead);
				const bool bWhole = SpikeIsFeasible(WholeRead);

				const FString Line = FString::Printf(
					TEXT("STACK BOTTOM BAND: core=%d shell=%d blocks=%d joints=%d ")
					TEXT("grounded(free)=%d | free-boundary feasible=%d | grounded-boundary ")
					TEXT("feasible=%d | WHOLE STACK feasible=%d | sandwich closes=%d | band ")
					TEXT("LIVE lambda*=%.17g answered=%d pivots=%d | whynot free='%s' ")
					TEXT("grounded='%s' whole='%s' live='%s'"),
					BandFreeCounts.Core, BandFreeCounts.Shell, BandFreeCounts.Blocks,
					BandFreeCounts.Joints, BandFreeCounts.Grounded, bBandFree ? 1 : 0,
					bBandGrounded ? 1 : 0, bWhole ? 1 : 0,
					bBandFree == bBandGrounded ? 1 : 0,
					BandLiveRead.Lambda, BandLiveRead.bAnswered ? 1 : 0, BandLiveRead.Pivots,
					*BandFreeRead.WhyNot, *BandGroundedRead.WhyNot, *WholeRead.WhyNot,
					*BandLiveRead.WhyNot);

				UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
				AddInfo(Line);

				MustAnswer(BandFreeRead, TEXT("PART D: the band with a FREE boundary"));
				MustAnswer(BandGroundedRead, TEXT("PART D: the band with a GROUNDED boundary"));
				MustAnswer(WholeRead, TEXT("PART D: the whole 30-course stack"));
				MustAnswer(BandLiveRead, TEXT("PART D: the band posed LIVE"));

				/* The band's size, pinned before anything is read off it. */
				TestEqual(
					TEXT("PART D: the band is 6 blocks — five core courses and one shell"),
					BandFreeCounts.Blocks, Pins.BandBlocks);

				TestTrue(
					TEXT("PART D: the bottom band solved with a FREE boundary must be ")
					TEXT("FEASIBLE — it is a short leaning stack standing on the ground, and ")
					TEXT("dropping the twenty-four courses above it drops their WEIGHT as well ")
					TEXT("as their restraint"),
					bBandFree);

				TestTrue(
					TEXT("PART D: the same band with a GROUNDED boundary must also be feasible ")
					TEXT("— so the two sides AGREE and the design's rule certifies"),
					bBandGrounded);

				TestTrue(
					TEXT("PART D: and the WHOLE stack must be INFEASIBLE (lambda* = 0.4405). ")
					TEXT("These three lines together are the refutation: a closed sandwich ")
					TEXT("certified 'the full structure is feasible' about a structure that has ")
					TEXT("no admissible equilibrium. The free-boundary side is not a ")
					TEXT("pessimistic bound and PROMOTION_DESIGN §5.3's 'the found force ")
					TEXT("system extends' does not hold."),
					!bWhole);

				/*
				 * How wrong the certificate is, as a number. The band's own lambda* says by how
				 * much: nowhere near the boundary, so the failure is not a knife edge a tighter
				 * extractor could tune away. Pinned in the file-wide +/-2e-5 window, measured not
				 * derived (this band is a different problem from the 5-course fixture's 31.2).
				 */
				if (Pins.BandLiveLambdaLo < 0.0)
				{
					AddError(FString::Printf(
						TEXT("PART D: UNMEASURED BAND lambda* — the free-boundary band's live ")
						TEXT("pose read %.17g in %d pivots. Pin it, then state the ratio ")
						TEXT("against the whole stack's 0.4405."),
						BandLiveRead.Lambda, BandLiveRead.Pivots));
				}
				else
				{
					TestTrue(
						*FString::Printf(
							TEXT("PART D: the certified band's OWN lambda* must lie in ")
							TEXT("[%.9g, %.9g] and was %.17g. The whole stack is 0.4405, so ")
							TEXT("the false certificate is not a knife edge — the region the ")
							TEXT("sandwich certified stands at many times its own weight ")
							TEXT("while the structure it certified falls under one. No ")
							TEXT("tightening of the extractor closes a gap of that size; only ")
							TEXT("carrying the omitted weight does (§12 D2')."),
							Pins.BandLiveLambdaLo, Pins.BandLiveLambdaHi, BandLiveRead.Lambda),
						BandLiveRead.Lambda >= Pins.BandLiveLambdaLo
							&& BandLiveRead.Lambda <= Pins.BandLiveLambdaHi);
				}

				/*
				 * THE RE-AIM: the same band, repaired. One thing changes, repair (1): the 24
				 * omitted courses' weight is applied as a dead surcharge on the vertical through
				 * their centre of gravity, with no restraint. Blocks, joints and boundary are
				 * identical to the free-boundary problem above, so a difference in the answer can
				 * only be the surcharge. The claim: the repaired band now agrees with the whole
				 * stack. The 41.96x certificate is withdrawn, not tightened — the two sides
				 * disagree, which means grow the region.
				 */
				{
					FOracleProblem RepairedBand;
					FRegionCounts RepairedCounts;
					FSurchargeCounts RepairedSurcharge;

					SpikeExtractRepairedRegion(
						StackProblem, BandMask, /*bGroundTheShell*/ false,
						ESpikeSurcharge::Carried,
						RepairedBand, RepairedCounts, RepairedSurcharge);

					const FPoseReading RepairedRead = SpikeSolve(RepairedBand);
					const bool bRepaired = SpikeIsFeasible(RepairedRead);

					/*
					 * The omitted courses' weight, derived from the brick mass and count rather
					 * than read off the extractor, so a surcharge charging the wrong set cannot
					 * agree with a total it computed itself.
					 */
					const double ExpectedChargedUu =
						24.0 * SpikeBrickMassKg * OracleGravityCmPerSecondSquared;

					/*
					 * Where it acts, derived a second time from the fixture builder's own
					 * expression. Placement is the load-bearing half of repair (1): the force is
					 * easy, the moment is what the refuted form lost. X8 moves the load to the
					 * contact's X (~55 cm) and flips the verdict, but a wrong-but-nearer X might
					 * pass in silence, so the line of action is recomputed from the fixture's own
					 * course spacing, not the extractor's weighted mean. Two derivations, one
					 * number: 175.0 cm, 120 cm outboard of the band's joint at X = 55.
					 */
					double IndependentWeightUu = 0.0;
					double IndependentMomentUuCm = 0.0;

					for (int32 Course = 6; Course < 30; ++Course)
					{
						const double CourseWeightUu =
							SpikeBrickMassKg * OracleGravityCmPerSecondSquared;

						IndependentWeightUu += CourseWeightUu;
						IndependentMomentUuCm += CourseWeightUu * (double(Course) * 10.0);
					}

					const double ExpectedLineXCm = IndependentMomentUuCm / IndependentWeightUu;

					const FString RepairLine = FString::Printf(
						TEXT("STACK BOTTOM BAND, REPAIRED: blocks=%d joints=%d charged=%d ")
						TEXT("components=%d interface=%d orphans=%d chargedW=%.9g ")
						TEXT("appliedW=%.9g discardedOntoGrounded=%.9g forces=%d | free+")
						TEXT("surcharge feasible=%d pivots=%d secs=%.3f | WHOLE STACK ")
						TEXT("feasible=%d | whynot='%s'"),
						RepairedCounts.Blocks, RepairedCounts.Joints,
						RepairedSurcharge.ChargedBlocks, RepairedSurcharge.ChargedComponents,
						RepairedSurcharge.InterfaceJoints, RepairedSurcharge.OrphanComponents,
						RepairedSurcharge.ChargedWeightUu, RepairedSurcharge.AppliedWeightUu,
						RepairedSurcharge.DiscardedOntoGroundedUu,
						RepairedBand.AppliedForces.Num(), bRepaired ? 1 : 0,
						RepairedRead.Pivots, RepairedRead.Seconds, bWhole ? 1 : 0,
						*RepairedRead.WhyNot);

					UE_LOG(LogTemp, Display, TEXT("%s"), *RepairLine);
					AddInfo(RepairLine);

					MustAnswer(RepairedRead, TEXT("PART D: the REPAIRED band"));

					TestEqual(
						TEXT("PART D repaired: the region itself is unchanged — same six ")
						TEXT("blocks, so the only difference from the row above is the ")
						TEXT("surcharge"),
						RepairedCounts.Blocks, BandFreeCounts.Blocks);

					TestEqual(
						TEXT("PART D repaired: the twenty-four courses above the band cannot ")
						TEXT("reach the ground without it, so all twenty-four are charged"),
						RepairedSurcharge.ChargedBlocks, 24);

					TestEqual(
						TEXT("PART D repaired: they are one connected component and it meets ")
						TEXT("the band at exactly one joint"),
						RepairedSurcharge.InterfaceJoints, 1);

					TestEqual(
						TEXT("PART D repaired: no charged component may be an ORPHAN — a ")
						TEXT("component with no joint into the region loses its weight in ")
						TEXT("silence, which is the refuted form coming back by the back door"),
						RepairedSurcharge.OrphanComponents, 0);

					TestEqual(
						TEXT("PART D repaired: and none of it may land on a GROUNDED region ")
						TEXT("block, which writes no equilibrium row and would swallow it"),
						RepairedSurcharge.DiscardedOntoGroundedUu, 0.0);

					TestTrue(
						*FString::Printf(
							TEXT("PART D repaired: the applied surcharge must equal the ")
							TEXT("charged blocks' own weight, %.9g uu, and was %.9g uu"),
							ExpectedChargedUu, RepairedSurcharge.AppliedWeightUu),
						FMath::Abs(RepairedSurcharge.AppliedWeightUu - ExpectedChargedUu)
							<= 1.0e-9 * ExpectedChargedUu);

					/* ---- The line of action, the repair's actual content. ---- */

					if (TestEqual(
							TEXT("PART D repaired: one component meeting the band at one joint ")
							TEXT("makes exactly one applied force"),
							RepairedBand.AppliedForces.Num(), 1))
					{
						const FOracleAppliedForce& Applied = RepairedBand.AppliedForces[0];

						TestTrue(
							TEXT("PART D repaired: the surcharge is DEAD — a live surcharge ")
							TEXT("would scale with lambda and the pose would stop being the ")
							TEXT("feasibility question it is posed as"),
							!Applied.bLive);

						TestEqual(
							TEXT("PART D repaired: the omitted material's weight is vertical, ")
							TEXT("so the surcharge carries no horizontal component"),
							Applied.ForceXUu, 0.0);

						TestTrue(
							*FString::Printf(
								TEXT("PART D repaired: and it acts DOWNWARD at %.9g uu against ")
								TEXT("the charged weight %.9g uu"),
								-Applied.ForceZUu, ExpectedChargedUu),
							FMath::Abs(-Applied.ForceZUu - ExpectedChargedUu)
								<= 1.0e-9 * ExpectedChargedUu);

						TestTrue(
							*FString::Printf(
								TEXT("PART D repaired, THE LINE OF ACTION: the surcharge must ")
								TEXT("act on the vertical through the CHARGED COMPONENT'S OWN ")
								TEXT("CENTRE OF GRAVITY — independently derived here as %.9g cm ")
								TEXT("from the fixture builder's own course spacing — and it ")
								TEXT("acted at %.9g cm. The contact it is delivered through ")
								TEXT("sits at X = 55, so this pin is what separates the repair ")
								TEXT("from the mutation that keeps the force and loses the ")
								TEXT("moment. A boolean verdict alone cannot: a wrong X nearer ")
								TEXT("than the contact's might not flip the band at all."),
								ExpectedLineXCm, Applied.AtXCm),
							FMath::Abs(Applied.AtXCm - ExpectedLineXCm)
								<= 1.0e-9 * FMath::Abs(ExpectedLineXCm));
					}

					TestTrue(
						TEXT("PART D repaired, THE CLAIM: the band that falsely certified a ")
						TEXT("collapsing stack must now read INFEASIBLE, so it AGREES with the ")
						TEXT("whole stack instead of contradicting it. The false certificate is ")
						TEXT("withdrawn rather than tightened — the two sides now disagree, ")
						TEXT("which means GROW THE REGION and issue no certificate at all."),
						!bRepaired);

					TestTrue(
						TEXT("PART D repaired: stated as the agreement itself, so the row says ")
						TEXT("what it is for — the repaired band's verdict equals the whole ")
						TEXT("stack's verdict"),
						bRepaired == bWhole);
				}
			}
		}
	}

	return true;
}

/*
 * TEST 3 — the repaired regional sandwich (§12 D2', ruled 2026-08-15).
 *
 * What was refuted: the designed two-sided sandwich (solve a region boundary-grounded, again
 * boundary-free, certify if they agree) closes only at 95-100% of the structure, costs 1.68x a
 * global solve, and its pessimistic side is not a bound — a closed sandwich certified a leaning
 * stack (lambda* 0.44048) as standing while the certified band's lambda* was
 * 18.481256459924058, wrong by 41.96x. Cause: dropping material outside a region removed its
 * weight with its restraint, and the omitted 24 courses carried the destabilising moment.
 *
 * What survives, unrepaired: the optimistic side. Restricting an admissible force system to a
 * grounded-shell region stays admissible, so optimistic-infeasible => globally infeasible.
 *
 * The two user-ruled repairs (defined at their code): (1) the pessimistic side keeps the omitted
 * weight as a dead surcharge while removing its restraint, charging material whose only road to
 * ground runs through the region, on the vertical through the charged component's centre of
 * gravity; (2) regions grow ground-anchored by construction — the growth parameter is the width
 * of a full-height strip, since the free side is provably infeasible for any region with no
 * grounded block. The question: how small can a certified region be, and is it cheap enough for
 * two solves in ~100 ms?
 *
 * WHAT WAS MEASURED — 2026-08-15, HEAD 0564bf5. (Predictions P1-P10 are recorded at their pins.)
 *
 * The wall arm (12x12 wall, one brick out of course 6, 149 blocks, 965-pivot global solve):
 *
 *     half-width   blocks   % of wall   charged   optimistic   pessimistic   closes
 *              0       41       27.5%         0     feasible      feasible      YES
 *              1       65       43.6%         0     feasible      feasible      yes
 *              5      149      100.0%         0     feasible      feasible      yes
 *
 * Closes at the first rung, 41 of 149 (27.5%) vs the refuted form's 142 / 95%; its two solves
 * take 49 + 262 = 311 pivots against 965 (0.322x vs 1.68x); ten deletions certify ten of ten.
 *
 * The collapse arm (ground-anchored bands of the 30-course stack): every false certificate is
 * withdrawn (bands disagree, meaning grow the region) and the arm is no cheaper — it certifies
 * only with all thirty blocks, because the optimistic side is what certifies a collapse and the
 * repair does not touch it. Its cost is 2.0x a global solve at closure, 8x over the whole ladder,
 * worse than the 1.68x the refuted form was condemned for. Height scales the region (18 courses:
 * 224 blocks, closes at the same half-width with 62 blocks, 27.7%), so a certified region is
 * small enough at 12 courses, marginal at 30; region size, not the sandwich, decides scenario
 * scale. Two predictions missed: P5's point estimate (0.48x vs 0.322x, the optimistic side
 * nearly free) and P8 outright (charging all 108 omitted blocks still leaves the strip feasible,
 * the wall arm has too much margin to discriminate surcharge rules).
 *
 * THE CAVEAT. "20 certificates, 0 false" is no evidence of soundness: nineteen are about
 * already-feasible structures and the twentieth is the whole stack, so none is about a proper
 * subset of an infeasible structure — the only shape a false certificate takes. At least three
 * unsoundnesses remain, none exercised by any fixture here:
 *
 *   1. omitted material with its own ground path that also bears on the region: charged nothing.
 *   2. the interface joint is never checked, so the transmitting contact's capacity is never
 *      asked (review's stack: joint 5-6 needs T = 636,000 uu against a 412,562 uu cap).
 *   3. the charged component's internal joints vanish, so a component failing internally leaves
 *      the region certifying.
 *
 * All three are specified follow-ups; TEST 4 settles (1) directly. Every conclusion has a
 * recorded mutation (TRAPS X6/X8/X9/X10); Agreements == 10 and the three surcharge-integrity
 * guards still lack a bite-prover. Cost: 9.4 s, opt-in fast tier; default suite untouched.
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleRepairedSandwichTest,
	"OracleSweepFast.RigidBlock.RepairedRegionalSandwich",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleRepairedSandwichTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleFeasibilitySpikeSupport;

	const FRepairPins Pins;

	const double Started = FPlatformTime::Seconds();

	/*
	 * A refusal is not an infeasibility, and SpikeIsFeasible cannot tell them apart: Refuse()
	 * returns bAnswered = false, lambda = 0, bit-for-bit the dead-load infeasibility arm. Every
	 * region solve is guarded, since an unguarded refusal reads as "no admissible equilibrium".
	 */
	const auto MustAnswer = [this](const FPoseReading& Read, const FString& Where)
	{
		return TestTrue(
			*FString::Printf(
				TEXT("%s: the solve must ANSWER — a REFUSAL and an INFEASIBILITY are ")
				TEXT("indistinguishable downstream (both are bAnswered=false, lambda=0). ")
				TEXT("It said: %s"),
				*Where, Read.WhyNot.IsEmpty() ? TEXT("(nothing)") : *Read.WhyNot),
			Read.bAnswered);
	};

	/** One region, both boundary conditions, one verdict. */
	struct FSandwich
	{
		int32 Blocks = 0;
		int32 Joints = 0;
		int32 GroundedFree = 0;
		bool bOptimistic = false;
		bool bPessimistic = false;
		bool bCloses = false;
		int32 OptimisticPivots = 0;
		int32 PessimisticPivots = 0;
		double Seconds = 0.0;
		FSurchargeCounts Surcharge;
	};

	const auto PoseSandwich = [this, &MustAnswer](
		const FOracleProblem& Full,
		const TArray<bool>& Mask,
		ESpikeSurcharge Rule,
		const FString& Where) -> FSandwich
	{
		FSandwich Out;

		FOracleProblem Optimistic;
		FRegionCounts OptimisticCounts;
		FSurchargeCounts OptimisticSurcharge;

		SpikeExtractRepairedRegion(
			Full, Mask, /*bGroundTheShell*/ true, Rule,
			Optimistic, OptimisticCounts, OptimisticSurcharge);

		FOracleProblem Pessimistic;
		FRegionCounts PessimisticCounts;
		FSurchargeCounts PessimisticSurcharge;

		SpikeExtractRepairedRegion(
			Full, Mask, /*bGroundTheShell*/ false, Rule,
			Pessimistic, PessimisticCounts, PessimisticSurcharge);

		/*
		 * The surcharge is the pessimistic side's alone, asserted not trusted: it would
		 * silently unsound the half review verified, and the extractor is one line from it.
		 */
		TestEqual(
			*FString::Printf(
				TEXT("%s: the OPTIMISTIC side must carry no surcharge — its soundness is a ")
				TEXT("restriction argument and a restriction satisfies no row that was not ")
				TEXT("in the original problem"),
				*Where),
			Optimistic.AppliedForces.Num(), 0);

		const FPoseReading OptimisticRead = SpikeSolve(Optimistic);
		const FPoseReading PessimisticRead = SpikeSolve(Pessimistic);

		MustAnswer(OptimisticRead, FString::Printf(TEXT("%s GROUNDED-boundary"), *Where));
		MustAnswer(PessimisticRead, FString::Printf(TEXT("%s FREE-boundary"), *Where));

		Out.Blocks = OptimisticCounts.Blocks;
		Out.Joints = OptimisticCounts.Joints;
		Out.GroundedFree = PessimisticCounts.Grounded;
		Out.bOptimistic = SpikeIsFeasible(OptimisticRead);
		Out.bPessimistic = SpikeIsFeasible(PessimisticRead);
		Out.bCloses = Out.bOptimistic == Out.bPessimistic;
		Out.OptimisticPivots = OptimisticRead.Pivots;
		Out.PessimisticPivots = PessimisticRead.Pivots;
		Out.Seconds = OptimisticRead.Seconds + PessimisticRead.Seconds;
		Out.Surcharge = PessimisticSurcharge;

		/*
		 * Repair (2), made executable. A region with no grounded block is free-side infeasible
		 * (sum its vertical rows: 0 = -W_region), so "ground-anchored by construction" is what the
		 * strip parameterisation guarantees. If it fails, every certificate below is vacuous.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: a ground-anchored region must actually contain the earth, and this ")
				TEXT("one holds %d grounded blocks"),
				*Where, Out.GroundedFree),
			Out.GroundedFree > 0);

		/* The surcharge's own integrity, everywhere it is applied. */
		TestEqual(
			*FString::Printf(TEXT("%s: no ORPHAN charged component"), *Where),
			Out.Surcharge.OrphanComponents, 0);

		TestEqual(
			*FString::Printf(
				TEXT("%s: no surcharge delivered onto a GROUNDED block, which writes no ")
				TEXT("equilibrium row and would swallow it"),
				*Where),
			Out.Surcharge.DiscardedOntoGroundedUu, 0.0);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the applied surcharge (%.9g uu) must equal the charged blocks' own ")
				TEXT("weight (%.9g uu)"),
				*Where, Out.Surcharge.AppliedWeightUu, Out.Surcharge.ChargedWeightUu),
			FMath::Abs(Out.Surcharge.AppliedWeightUu - Out.Surcharge.ChargedWeightUu)
				<= 1.0e-9 * (1.0 + Out.Surcharge.ChargedWeightUu));

		return Out;
	};

	/* Every closed sandwich in this test is checked against the truth it certifies. */
	int32 Certificates = 0;
	int32 FalseCertificates = 0;

	const auto CheckCertificate = [this, &Certificates, &FalseCertificates](
		const FSandwich& Sandwich, bool bGlobalFeasible, const FString& Where)
	{
		if (!Sandwich.bCloses)
		{
			return;
		}

		++Certificates;

		if (Sandwich.bOptimistic != bGlobalFeasible)
		{
			++FalseCertificates;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: the closed sandwich certifies feasible=%d and the whole structure ")
				TEXT("reads feasible=%d. A disagreement here is a FALSE CERTIFICATE — the ")
				TEXT("failure the repair exists to remove, and the one thing this slice ")
				TEXT("cannot be allowed to reproduce quietly."),
				*Where, Sandwich.bOptimistic ? 1 : 0, bGlobalFeasible ? 1 : 0),
			Sandwich.bOptimistic == bGlobalFeasible);
	};

	/*
	 * PART 1 — the wall, and the strip-width ladder. The refuted form's fixture: a 12x12 wall,
	 * one brick out of course 6, 149 blocks, 965 pivots for whole-wall feasibility. Same wall and
	 * deletion, so only the region shape and surcharge differ.
	 */

	constexpr int32 WallCourses = 12;
	constexpr int32 WallCells = 12;
	constexpr int32 WallDeleteCourse = 6;

	FStructure Wall;
	FString Why;

	/*
	 * Each producer call is its own statement before its message: folding a call that writes Why
	 * into the Printf that reads it is unsequenced, and MSVC evaluates the condition last (TRAPS).
	 */
	const bool bWallLaid = SpikeBuildIntactWall(WallCourses, WallCells, Wall, Why);

	if (!TestTrue(
			*FString::Printf(TEXT("repaired: the wall must lay (it said: %s)"), *Why),
			bWallLaid))
	{
		return true;
	}

	double DeleteXCm = 0.0;
	double DeleteZCm = 0.0;
	const int32 Victim = SpikePieceInCourse(Wall, WallDeleteCourse, DeleteXCm, DeleteZCm);

	if (!TestTrue(TEXT("repaired: there must be a brick to delete"), Victim != INDEX_NONE)
		|| !TestTrue(TEXT("repaired: the brick must be removable"), Wall.RemovePiece(Victim)))
	{
		return true;
	}

	FOracleProblem Full;

	const bool bBridged = BuildRigidBlockProblem(Wall, Full, Why);

	if (!TestTrue(
			*FString::Printf(TEXT("repaired: the bridge must represent the cut wall (%s)"), *Why),
			bBridged))
	{
		return true;
	}

	TestEqual(TEXT("repaired: the wall is the refuted form's own fixture"),
		Full.Blocks.Num(), Pins.WallBlocks);

	FOracleProblem Global = Full;
	Global.bGravityIsLive = false;

	const FPoseReading GlobalRead = SpikeSolve(Global);
	const bool bGlobalFeasible = SpikeIsFeasible(GlobalRead);

	MustAnswer(GlobalRead, TEXT("repaired: the whole cut wall"));

	TestEqual(
		TEXT("repaired: the whole wall's feasibility pivot count — the number the regional ")
		TEXT("lever has to beat, and the refuted form did not"),
		GlobalRead.Pivots, Pins.WallGlobalPivots);

	int32 ClosingHalfWidth = INDEX_NONE;
	int32 ClosingRegionBlocks = INDEX_NONE;
	int32 ClosingOptimisticPivots = INDEX_NONE;
	int32 ClosingPessimisticPivots = INDEX_NONE;
	double ClosingSeconds = -1.0;

	/*
	 * Per rung. The Charged column is P1's test: 0 at every width, because a full-height strip
	 * strands nothing in a grounded wall. Measured 2026-08-15: block count 24w + 41 up to the wall
	 * width, the strip closes at the first rung, Charged reads 0 on all six — P1 held.
	 */
	TArray<FStripPin> StripPins;
	StripPins.Add({ 0,  41, 0, 1, 1 });
	StripPins.Add({ 1,  65, 0, 1, 1 });
	StripPins.Add({ 2,  89, 0, 1, 1 });
	StripPins.Add({ 3, 113, 0, 1, 1 });
	StripPins.Add({ 4, 137, 0, 1, 1 });
	StripPins.Add({ 5, 149, 0, 1, 1 });

	for (const FStripPin& Pin : StripPins)
	{
		TArray<bool> Mask;
		SpikeGroundStripMask(Full, DeleteXCm, Pin.HalfWidthCells, Mask);

		const FSandwich Sandwich = PoseSandwich(
			Full, Mask, ESpikeSurcharge::Carried,
			FString::Printf(TEXT("strip w=%d"), Pin.HalfWidthCells));

		if (Sandwich.bCloses && ClosingHalfWidth == INDEX_NONE)
		{
			ClosingHalfWidth = Pin.HalfWidthCells;
			ClosingRegionBlocks = Sandwich.Blocks;
			ClosingOptimisticPivots = Sandwich.OptimisticPivots;
			ClosingPessimisticPivots = Sandwich.PessimisticPivots;
			ClosingSeconds = Sandwich.Seconds;
		}

		const FString Line = FString::Printf(
			TEXT("REPAIRED STRIP w=%d: blocks=%d (%.1f%% of %d) joints=%d groundedFree=%d | ")
			TEXT("charged=%d components=%d interface=%d chargedW=%.9g | optimistic=%d ")
			TEXT("pivots=%d | pessimistic=%d pivots=%d | closes=%d | secs=%.3f"),
			Pin.HalfWidthCells, Sandwich.Blocks,
			100.0 * double(Sandwich.Blocks) / double(Full.Blocks.Num()), Full.Blocks.Num(),
			Sandwich.Joints, Sandwich.GroundedFree,
			Sandwich.Surcharge.ChargedBlocks, Sandwich.Surcharge.ChargedComponents,
			Sandwich.Surcharge.InterfaceJoints, Sandwich.Surcharge.ChargedWeightUu,
			Sandwich.bOptimistic ? 1 : 0, Sandwich.OptimisticPivots,
			Sandwich.bPessimistic ? 1 : 0, Sandwich.PessimisticPivots,
			Sandwich.bCloses ? 1 : 0, Sandwich.Seconds);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		CheckCertificate(Sandwich, bGlobalFeasible,
			FString::Printf(TEXT("strip w=%d"), Pin.HalfWidthCells));

		if (Pin.Blocks == INDEX_NONE || Pin.Charged == INDEX_NONE
			|| Pin.Optimistic == INDEX_NONE || Pin.Pessimistic == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("strip w=%d: UNMEASURED RUNG — pin blocks=%d charged=%d optimistic=%d ")
				TEXT("pessimistic=%d"),
				Pin.HalfWidthCells, Sandwich.Blocks, Sandwich.Surcharge.ChargedBlocks,
				Sandwich.bOptimistic ? 1 : 0, Sandwich.bPessimistic ? 1 : 0));
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("strip w=%d: region block count"),
				Pin.HalfWidthCells), Sandwich.Blocks, Pin.Blocks);

			TestEqual(
				*FString::Printf(
					TEXT("strip w=%d: how many omitted blocks the region CARRIES — P1 ")
					TEXT("predicted zero at every width, because a full-height strip in a ")
					TEXT("grounded wall leaves nothing stranded"),
					Pin.HalfWidthCells),
				Sandwich.Surcharge.ChargedBlocks, Pin.Charged);

			TestEqual(*FString::Printf(TEXT("strip w=%d: GROUNDED-boundary verdict"),
				Pin.HalfWidthCells), Sandwich.bOptimistic ? 1 : 0, Pin.Optimistic);

			TestEqual(*FString::Printf(TEXT("strip w=%d: FREE-boundary verdict"),
				Pin.HalfWidthCells), Sandwich.bPessimistic ? 1 : 0, Pin.Pessimistic);
		}
	}

	if (Pins.ClosingHalfWidth == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("repaired ladder: UNMEASURED CLOSING WIDTH — measured half-width %d at %d ")
			TEXT("of %d blocks (%.1f%%), %d + %d pivots against the whole wall's %d ")
			TEXT("(ratio %.3f), %.4f s for the pair. PREDICTED (P3/P5/P6): half-width 0, 41 ")
			TEXT("blocks, 28%%, ~0.48x, under 100 ms. INDEX_NONE means it did not close at ")
			TEXT("any width in the ladder."),
			ClosingHalfWidth, ClosingRegionBlocks, Full.Blocks.Num(),
			ClosingRegionBlocks > 0
				? 100.0 * double(ClosingRegionBlocks) / double(Full.Blocks.Num()) : -1.0,
			ClosingOptimisticPivots, ClosingPessimisticPivots, GlobalRead.Pivots,
			GlobalRead.Pivots > 0
				? double(ClosingOptimisticPivots + ClosingPessimisticPivots)
					/ double(GlobalRead.Pivots)
				: -1.0,
			ClosingSeconds));
	}
	else
	{
		TestEqual(TEXT("repaired ladder: the closing strip half-width"),
			ClosingHalfWidth, Pins.ClosingHalfWidth);

		TestEqual(
			TEXT("repaired ladder: HOW BIG the certified region is — the number the refuted ")
			TEXT("form answered with 142 of 149, and the whole point of a decomposition"),
			ClosingRegionBlocks, Pins.ClosingRegionBlocks);

		TestEqual(TEXT("repaired ladder: optimistic pivots at the closing width"),
			ClosingOptimisticPivots, Pins.ClosingOptimisticPivots);

		TestEqual(TEXT("repaired ladder: pessimistic pivots at the closing width"),
			ClosingPessimisticPivots, Pins.ClosingPessimisticPivots);

		/*
		 * The cost comparison, in pivots (deterministic; seconds not). The refuted form's two
		 * solves cost 1.68x the global solve; this strict inequality says which side of 1.0
		 * the repaired form lands on. If it crosses back, the lever is worthless, noticed here.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("repaired ladder: THE SANDWICH MUST COST LESS THAN THE GLOBAL SOLVE — ")
				TEXT("its two solves at the closing width take %d + %d = %d pivots against ")
				TEXT("the whole wall's %d (ratio %.3f; the refuted form's was 1.68x)"),
				ClosingOptimisticPivots, ClosingPessimisticPivots,
				ClosingOptimisticPivots + ClosingPessimisticPivots, GlobalRead.Pivots,
				double(ClosingOptimisticPivots + ClosingPessimisticPivots)
					/ double(GlobalRead.Pivots)),
			ClosingOptimisticPivots + ClosingPessimisticPivots < GlobalRead.Pivots);

		/*
		 * The region must be a neighbourhood, not the building. Half the structure is the
		 * loosest reading of "much smaller" still worth calling a decomposition; the refuted
		 * form's 95% failed it by a mile.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("repaired ladder: the certified region must be under half the wall — %d ")
				TEXT("of %d blocks, %.1f%%"),
				ClosingRegionBlocks, Full.Blocks.Num(),
				100.0 * double(ClosingRegionBlocks) / double(Full.Blocks.Num())),
			2 * ClosingRegionBlocks < Full.Blocks.Num());
	}

	/*
	 * PART 2 — ten deletions at the closing width. The refuted form certified five of ten (those
	 * whose ball reached the foundation); a ground-anchored strip reaches it by construction, so
	 * the deletion's height stops mattering (P4: ten of ten). A fixed ladder, the same ten every run.
	 */

	int32 Agreements = 0;

	if (ClosingHalfWidth == INDEX_NONE)
	{
		AddError(TEXT("repaired agreement: skipped — the ladder never closed, so there is no ")
			TEXT("width to run ten deletions at"));
	}
	else
	{
		for (int32 Course = 1; Course <= 10; ++Course)
		{
			FStructure Cut;
			FString CutWhy;

			const bool bCutLaid = SpikeBuildIntactWall(WallCourses, WallCells, Cut, CutWhy);

			if (!TestTrue(
					*FString::Printf(TEXT("repaired course %d: the wall must lay (%s)"),
						Course, *CutWhy),
					bCutLaid))
			{
				continue;
			}

			double XCm = 0.0;
			double ZCm = 0.0;
			const int32 Brick = SpikePieceInCourse(Cut, Course, XCm, ZCm);
			const bool bRemoved = Brick != INDEX_NONE && Cut.RemovePiece(Brick);

			if (!TestTrue(
					*FString::Printf(TEXT("repaired course %d: a brick must exist there"), Course),
					bRemoved))
			{
				continue;
			}

			FOracleProblem CutProblem;

			const bool bCutBridged = BuildRigidBlockProblem(Cut, CutProblem, CutWhy);

			if (!TestTrue(
					*FString::Printf(TEXT("repaired course %d: the bridge must represent it (%s)"),
						Course, *CutWhy),
					bCutBridged))
			{
				continue;
			}

			FOracleProblem CutGlobal = CutProblem;
			CutGlobal.bGravityIsLive = false;

			const FPoseReading CutGlobalRead = SpikeSolve(CutGlobal);
			const bool bCutFeasible = SpikeIsFeasible(CutGlobalRead);

			MustAnswer(CutGlobalRead,
				FString::Printf(TEXT("repaired course %d: the whole cut wall"), Course));

			TArray<bool> Mask;
			SpikeGroundStripMask(CutProblem, XCm, ClosingHalfWidth, Mask);

			const FSandwich Sandwich = PoseSandwich(
				CutProblem, Mask, ESpikeSurcharge::Carried,
				FString::Printf(TEXT("repaired course %d"), Course));

			if (Sandwich.bCloses)
			{
				++Agreements;
			}

			const FString Line = FString::Printf(
				TEXT("REPAIRED AGREEMENT course=%d (brick at X=%.4f Z=%.4f): blocks=%d ")
				TEXT("charged=%d | optimistic=%d pessimistic=%d closes=%d | global=%d | ")
				TEXT("pivots %d + %d against %d | secs=%.3f"),
				Course, XCm, ZCm, Sandwich.Blocks, Sandwich.Surcharge.ChargedBlocks,
				Sandwich.bOptimistic ? 1 : 0, Sandwich.bPessimistic ? 1 : 0,
				Sandwich.bCloses ? 1 : 0, bCutFeasible ? 1 : 0,
				Sandwich.OptimisticPivots, Sandwich.PessimisticPivots, CutGlobalRead.Pivots,
				Sandwich.Seconds);

			UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
			AddInfo(Line);

			CheckCertificate(Sandwich, bCutFeasible,
				FString::Printf(TEXT("repaired course %d"), Course));
		}
	}

	if (Pins.Agreements == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("repaired agreement: UNMEASURED — at the closing width, %d of ten deletions ")
			TEXT("certified. PREDICTED (P4) ten; the refuted form managed five at radius 4 ")
			TEXT("with a region of 114 blocks."),
			Agreements));
	}
	else
	{
		TestEqual(
			TEXT("repaired agreement: how many of ten deletions certify at the closing width ")
			TEXT("— distinct from the file-wide count of closed sandwiches, which is 20"),
			Agreements, Pins.Agreements);
	}

	/*
	 * PART 3 — the over-inclusive control. "Keep the omitted weight" has an obvious reading, keep
	 * all of it, the second way to kill the lever: a too-pessimistic side never certifies.
	 * Measured at the same width and wall, so the only difference from the certified row is the rule.
	 */

	if (ClosingHalfWidth != INDEX_NONE)
	{
		TArray<bool> Mask;
		SpikeGroundStripMask(Full, DeleteXCm, ClosingHalfWidth, Mask);

		const FSandwich Control = PoseSandwich(
			Full, Mask, ESpikeSurcharge::EveryOmittedBlock, TEXT("control (charge everything)"));

		const FString Line = FString::Printf(
			TEXT("REPAIRED CONTROL w=%d, EVERY omitted block charged: blocks=%d charged=%d ")
			TEXT("components=%d interface=%d chargedW=%.9g | optimistic=%d pessimistic=%d ")
			TEXT("closes=%d"),
			ClosingHalfWidth, Control.Blocks, Control.Surcharge.ChargedBlocks,
			Control.Surcharge.ChargedComponents, Control.Surcharge.InterfaceJoints,
			Control.Surcharge.ChargedWeightUu, Control.bOptimistic ? 1 : 0,
			Control.bPessimistic ? 1 : 0, Control.bCloses ? 1 : 0);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		if (Pins.ControlPessimistic == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("repaired control: UNMEASURED — charging every omitted block leaves the ")
				TEXT("FREE-boundary side reading feasible=%d (charged %d blocks, %.9g uu). ")
				TEXT("PREDICTED (P8) 0 — infeasible, so the over-inclusive rule never ")
				TEXT("certifies."),
				Control.bPessimistic ? 1 : 0, Control.Surcharge.ChargedBlocks,
				Control.Surcharge.ChargedWeightUu));
		}
		else
		{
			TestEqual(
				TEXT("repaired control: the FREE-boundary verdict when EVERY omitted block is ")
				TEXT("charged — the too-pessimistic failure mode, measured on the same wall ")
				TEXT("and the same width as the certificate above"),
				Control.bPessimistic ? 1 : 0, Pins.ControlPessimistic);
		}
	}

	/*
	 * PART 4 — height scales the region, which decides scenario scale. A ground-anchored region
	 * reaches the foundation, so its size is set by height, not a bonded distance around the
	 * deletion. Scenario walls are 30 courses; this measures the slope with the same wall at 18.
	 */

	{
		constexpr int32 TallCourses = 18;

		FStructure Tall;
		FString TallWhy;

		const bool bTallLaid = SpikeBuildIntactWall(TallCourses, WallCells, Tall, TallWhy);

		if (TestTrue(
				*FString::Printf(TEXT("repaired tall: the wall must lay (%s)"), *TallWhy),
				bTallLaid))
		{
			double TallXCm = 0.0;
			double TallZCm = 0.0;
			const int32 TallVictim = SpikePieceInCourse(Tall, WallDeleteCourse, TallXCm, TallZCm);
			const bool bTallRemoved = TallVictim != INDEX_NONE && Tall.RemovePiece(TallVictim);

			FOracleProblem TallProblem;

			const bool bTallBridged = bTallRemoved
				&& BuildRigidBlockProblem(Tall, TallProblem, TallWhy);

			if (TestTrue(TEXT("repaired tall: the brick must be removable"), bTallRemoved)
				&& TestTrue(
					*FString::Printf(TEXT("repaired tall: the bridge must represent it (%s)"),
						*TallWhy),
					bTallBridged))
			{
				FOracleProblem TallGlobal = TallProblem;
				TallGlobal.bGravityIsLive = false;

				const FPoseReading TallGlobalRead = SpikeSolve(TallGlobal);
				const bool bTallFeasible = SpikeIsFeasible(TallGlobalRead);

				MustAnswer(TallGlobalRead, TEXT("repaired tall: the whole cut wall"));

				int32 TallClosing = INDEX_NONE;
				int32 TallClosingBlocks = INDEX_NONE;
				int32 TallClosingOptimisticPivots = INDEX_NONE;
				int32 TallClosingPessimisticPivots = INDEX_NONE;

				for (int32 HalfWidth = 0; HalfWidth <= 2; ++HalfWidth)
				{
					TArray<bool> Mask;
					SpikeGroundStripMask(TallProblem, TallXCm, HalfWidth, Mask);

					const FSandwich Sandwich = PoseSandwich(
						TallProblem, Mask, ESpikeSurcharge::Carried,
						FString::Printf(TEXT("tall strip w=%d"), HalfWidth));

					if (Sandwich.bCloses && TallClosing == INDEX_NONE)
					{
						TallClosing = HalfWidth;
						TallClosingBlocks = Sandwich.Blocks;
						TallClosingOptimisticPivots = Sandwich.OptimisticPivots;
						TallClosingPessimisticPivots = Sandwich.PessimisticPivots;
					}

					const FString Line = FString::Printf(
						TEXT("REPAIRED TALL (%d courses) w=%d: blocks=%d (%.1f%% of %d) ")
						TEXT("charged=%d | optimistic=%d pessimistic=%d closes=%d | pivots ")
						TEXT("%d + %d against %d | secs=%.3f"),
						TallCourses, HalfWidth, Sandwich.Blocks,
						100.0 * double(Sandwich.Blocks) / double(TallProblem.Blocks.Num()),
						TallProblem.Blocks.Num(), Sandwich.Surcharge.ChargedBlocks,
						Sandwich.bOptimistic ? 1 : 0, Sandwich.bPessimistic ? 1 : 0,
						Sandwich.bCloses ? 1 : 0, Sandwich.OptimisticPivots,
						Sandwich.PessimisticPivots, TallGlobalRead.Pivots, Sandwich.Seconds);

					UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
					AddInfo(Line);

					CheckCertificate(Sandwich, bTallFeasible,
						FString::Printf(TEXT("tall strip w=%d"), HalfWidth));
				}

				if (Pins.TallWallBlocks == INDEX_NONE
					|| Pins.TallClosingHalfWidth == INDEX_NONE
					|| Pins.TallClosingRegionBlocks == INDEX_NONE)
				{
					AddError(FString::Printf(
						TEXT("repaired tall: UNMEASURED — an %d-course wall is %d blocks and ")
						TEXT("closes at half-width %d with a region of %d blocks (%.1f%%), ")
						TEXT("costing %d + %d pivots against the whole wall's %d. PREDICTED ")
						TEXT("(P7) half-width 0 and ~62 blocks, 1.5x the 12-course region for ")
						TEXT("1.5x the height."),
						TallCourses, TallProblem.Blocks.Num(), TallClosing, TallClosingBlocks,
						TallClosingBlocks > 0
							? 100.0 * double(TallClosingBlocks) / double(TallProblem.Blocks.Num())
							: -1.0,
						TallClosingOptimisticPivots, TallClosingPessimisticPivots,
						TallGlobalRead.Pivots));
				}
				else
				{
					TestEqual(TEXT("repaired tall: the taller wall's block count"),
						TallProblem.Blocks.Num(), Pins.TallWallBlocks);

					TestEqual(TEXT("repaired tall: its closing half-width"),
						TallClosing, Pins.TallClosingHalfWidth);

					TestEqual(
						TEXT("repaired tall: and the certified region's size, which is the ")
						TEXT("measurement that says whether this lever survives a 30-course ")
						TEXT("scenario wall"),
						TallClosingBlocks, Pins.TallClosingRegionBlocks);

					/*
					 * The cost at height, pinned: the second point the scenario-scale
					 * extrapolation runs through. 79 + 474 = 553 here against 49 + 262 = 311 at
					 * twelve courses is 1.78x cost for 1.5x height.
					 */
					TestEqual(
						TEXT("repaired tall: optimistic pivots at the closing width"),
						TallClosingOptimisticPivots, Pins.TallClosingOptimisticPivots);

					TestEqual(
						TEXT("repaired tall: pessimistic pivots at the closing width — with ")
						TEXT("the line above, the INPUT to the scenario-scale extrapolation"),
						TallClosingPessimisticPivots, Pins.TallClosingPessimisticPivots);
				}
			}
		}
	}

	/*
	 * PART 5 — the collapse arm. Every wall is LP-feasible, so Parts 1-4 exercise only the standing
	 * side. The 30-course stack is the one cheap sub-1.0 fixture (lambda* 0.44048) that exposed the
	 * false certificate. Here the regions are ground-anchored bands (courses 0..k), repair (2)'s
	 * shape for a chain, each posed with repair (1) live.
	 */

	{
		FStructure Stack;
		FString StackWhy;

		const bool bStackLaid = SpikeBuildLeaningStack(30, Stack, StackWhy);

		if (TestTrue(
				*FString::Printf(TEXT("repaired stack: it must lay (%s)"), *StackWhy),
				bStackLaid))
		{
			FOracleProblem StackProblem;

			const bool bStackBridged = BuildRigidBlockProblem(Stack, StackProblem, StackWhy);

			if (TestTrue(
					*FString::Printf(TEXT("repaired stack: the bridge must represent it (%s)"),
						*StackWhy),
					bStackBridged))
			{
				FOracleProblem StackGlobal = StackProblem;
				StackGlobal.bGravityIsLive = false;

				const FPoseReading StackGlobalRead = SpikeSolve(StackGlobal);
				const bool bStackFeasible = SpikeIsFeasible(StackGlobalRead);

				MustAnswer(StackGlobalRead, TEXT("repaired stack: the whole stack"));

				TestTrue(
					TEXT("repaired stack: the whole 30-course stack must be INFEASIBLE — it is ")
					TEXT("the collapse arm and without it every certificate below is about a ")
					TEXT("structure that stands"),
					!bStackFeasible);

				int32 StackClosingCourse = INDEX_NONE;
				int32 StackClosingBlocks = INDEX_NONE;
				int32 StackClosingOptimisticPivots = INDEX_NONE;
				int32 StackClosingPessimisticPivots = INDEX_NONE;

				/*
				 * Every pivot the growth ladder spends before it certifies: a decomposition that
				 * must grow is charged for the whole walk, not only the rung it stops on.
				 */
				int32 StackLadderPivots = 0;

				const int32 TopCourses[] = { 5, 10, 15, 20, 25, 29 };

				for (const int32 TopCourse : TopCourses)
				{
					const double TopZCm =
						SpikeBrickHeightCm / 2.0 + double(TopCourse) * SpikeCoursePitchCm;

					TArray<bool> Mask;
					SpikeGroundBandMask(StackProblem, TopZCm, Mask);

					const FSandwich Sandwich = PoseSandwich(
						StackProblem, Mask, ESpikeSurcharge::Carried,
						FString::Printf(TEXT("stack band 0..%d"), TopCourse));

					StackLadderPivots +=
						Sandwich.OptimisticPivots + Sandwich.PessimisticPivots;

					if (Sandwich.bCloses && StackClosingCourse == INDEX_NONE)
					{
						StackClosingCourse = TopCourse;
						StackClosingBlocks = Sandwich.Blocks;
						StackClosingOptimisticPivots = Sandwich.OptimisticPivots;
						StackClosingPessimisticPivots = Sandwich.PessimisticPivots;
					}

					const FString Line = FString::Printf(
						TEXT("REPAIRED STACK BAND 0..%d: blocks=%d (%.1f%% of %d) charged=%d ")
						TEXT("components=%d interface=%d chargedW=%.9g | optimistic=%d ")
						TEXT("pessimistic=%d closes=%d | pivots %d + %d against %d"),
						TopCourse, Sandwich.Blocks,
						100.0 * double(Sandwich.Blocks) / double(StackProblem.Blocks.Num()),
						StackProblem.Blocks.Num(), Sandwich.Surcharge.ChargedBlocks,
						Sandwich.Surcharge.ChargedComponents, Sandwich.Surcharge.InterfaceJoints,
						Sandwich.Surcharge.ChargedWeightUu, Sandwich.bOptimistic ? 1 : 0,
						Sandwich.bPessimistic ? 1 : 0, Sandwich.bCloses ? 1 : 0,
						Sandwich.OptimisticPivots, Sandwich.PessimisticPivots,
						StackGlobalRead.Pivots);

					UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
					AddInfo(Line);

					CheckCertificate(Sandwich, bStackFeasible,
						FString::Printf(TEXT("stack band 0..%d"), TopCourse));

					/*
					 * The surcharge's own set, pinned where it does the work: a reachability walk
					 * charging the wrong blocks would still leave this band infeasible with every
					 * verdict green, so the count is asserted, not inferred.
					 */
					if (TopCourse == 5)
					{
						TestEqual(
							TEXT("repaired stack: the band 0..5 is seven blocks and CARRIES the ")
							TEXT("twenty-three courses above it — the weight the refuted form ")
							TEXT("deleted along with their restraint"),
							Sandwich.Surcharge.ChargedBlocks, Pins.StackBandChargedAtFive);

						TestEqual(
							TEXT("repaired stack: and they are one component meeting the band at ")
							TEXT("exactly one joint, which is what a chain must give"),
							Sandwich.Surcharge.InterfaceJoints, 1);
					}
				}

				if (Pins.StackClosingTopCourse == INDEX_NONE
					|| Pins.StackClosingRegionBlocks == INDEX_NONE)
				{
					AddError(FString::Printf(
						TEXT("repaired stack: UNMEASURED — the smallest ground-anchored band ")
						TEXT("whose two sides AGREE is courses 0..%d, a region of %d of %d ")
						TEXT("blocks, costing %d + %d pivots against the whole stack's %d and ")
						TEXT("%d over the whole growth ladder. PREDICTED (P9) only the whole ")
						TEXT("structure: the repair withdraws the false certificate without ")
						TEXT("making collapse cheaper."),
						StackClosingCourse, StackClosingBlocks, StackProblem.Blocks.Num(),
						StackClosingOptimisticPivots, StackClosingPessimisticPivots,
						StackGlobalRead.Pivots, StackLadderPivots));
				}
				else
				{
					TestEqual(TEXT("repaired stack: the closing band's top course"),
						StackClosingCourse, Pins.StackClosingTopCourse);

					TestEqual(TEXT("repaired stack: and the region there"),
						StackClosingBlocks, Pins.StackClosingRegionBlocks);

					/* ---- And what the collapse arm costs, the other half. ---- */

					TestEqual(TEXT("repaired stack: optimistic pivots at closure"),
						StackClosingOptimisticPivots, Pins.StackClosingOptimisticPivots);

					TestEqual(TEXT("repaired stack: pessimistic pivots at closure"),
						StackClosingPessimisticPivots, Pins.StackClosingPessimisticPivots);

					TestEqual(
						TEXT("repaired stack: and every pivot the growth ladder spends before ")
						TEXT("it gets there — a decomposition that must GROW is charged for the ")
						TEXT("whole walk, not only the rung it stops on"),
						StackLadderPivots, Pins.StackLadderPivots);

					/*
					 * The ratio, as the inequality it is. The wall arm asserts the sandwich
					 * costs less than the global solve; the collapse arm goes the other way,
					 * and both are asserted so neither ratio is quoted without the other.
					 */
					TestTrue(
						*FString::Printf(
							TEXT("repaired stack: THE COLLAPSE ARM COSTS MORE THAN THE GLOBAL ")
							TEXT("SOLVE — %d + %d = %d pivots at closure against %d (%.2fx), ")
							TEXT("and %d over the whole ladder (%.2fx). The refuted form was ")
							TEXT("condemned at 1.68x. If this ever drops below 1.0 the lever ")
							TEXT("has become worth something on the arm that matters, and this ")
							TEXT("is where that is noticed."),
							StackClosingOptimisticPivots, StackClosingPessimisticPivots,
							StackClosingOptimisticPivots + StackClosingPessimisticPivots,
							StackGlobalRead.Pivots,
							double(StackClosingOptimisticPivots + StackClosingPessimisticPivots)
								/ double(StackGlobalRead.Pivots),
							StackLadderPivots,
							double(StackLadderPivots) / double(StackGlobalRead.Pivots)),
						StackClosingOptimisticPivots + StackClosingPessimisticPivots
							> StackGlobalRead.Pivots);
				}
			}
		}
	}

	/*
	 * The one assertion this slice would be worthless without. The refuted form failed by
	 * certifying a structure with no equilibrium, not by certifying too rarely. Every closed
	 * sandwich is checked against the whole structure's verdict, and the count is asserted here so
	 * "no false certificate" is one number, not a scattering of rows an early return could skip.
	 */
	{
		const FString Line = FString::Printf(
			TEXT("REPAIRED TOTALS: %d certificates issued, %d of them FALSE. Test took %.3f s."),
			Certificates, FalseCertificates, FPlatformTime::Seconds() - Started);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		TestEqual(
			TEXT("REPAIRED: not one closed sandwich in this test may disagree with the whole ")
			TEXT("structure it certifies. The refuted form produced one that was wrong by ")
			TEXT("41.96x; this is the count that says whether the repair removed it."),
			FalseCertificates, 0);

		TestTrue(
			*FString::Printf(
				TEXT("REPAIRED: and at least one certificate must actually be issued (%d), or ")
				TEXT("'no false certificate' is true of a lever that certifies nothing"),
				Certificates),
			Certificates > 0);
	}

	return true;
}

/*
 * TEST 4 — the sub-1.0 wall: is the repaired pessimistic side a bound, or a heuristic?
 *
 * The gating follow-up of §5.3's box and §11 R2, and the only fixture shape that can answer it.
 * The repaired sandwich's "20 certificates, 0 false" cannot fire in the direction that matters:
 * none of its certificates is about a proper subset of an infeasible structure. The fixture (and
 * three routes not taken) is documented at SpikeBuildChimneyWall: the 12x12 wall
 * RepairedRegionalSandwich measures, with a 30-course leaning chimney mortared onto the top
 * course, leaning 10 cm per course past the wall's end. The chimney is the leaning stack's chain,
 * so the structure has no equilibrium; the failure lives at the far end, so a strip through a
 * mid-wall deletion can certify while the structure falls.
 *
 * Predictions Q1-Q10 are recorded at their pins (§7.3). Either outcome is pinned as measured;
 * nothing is tuned to produce the other.
 *
 * WHAT WAS MEASURED — 2026-08-16, HEAD 74bb081. The certificate is false; the repaired
 * pessimistic side is not a bound.
 *
 *     composite   179 blocks / 415 joints, lambda* = 0.44049301907204619, INFEASIBLE
 *     plain wall  149 blocks / 385 joints, FEASIBLE — the chimney is the whole difference
 *
 *     w   blocks   % of 179   charged   optimistic   pessimistic   closes   FALSE
 *     0       41      22.9%         0     feasible      feasible      YES     YES
 *     3      113      63.1%        30     feasible      feasible      yes     yes
 *     4      139      77.7%        28     feasible    INFEASIBLE       no       —
 *
 * Four false certificates, the smallest 41 of 179 blocks (22.9%). The certified strip's own
 * lambda* is 621.95652149729517 against 0.44049 (1,412x, where Part D's was 41.96x). Q1-Q9 hit;
 * the useful miss is Q7, which predicted charged = 0 at every closing width. At w = 3 the rule is
 * not blind: it charges all thirty chimney blocks and the strip still certifies. Why — hole (3)
 * of §5.3's box, not hole (1):
 *
 *   - at w = 0..2 the chimney's root sits outside the region, reaches ground down the wall's
 *     columns, and is charged nothing (hole (1)).
 *   - at w = 3 the root is a shell block, so the chimney above is charged in full; the load goes
 *     to the root brick and the interface joint is dropped, never checked (hole (1) again). Hand
 *     statics say it stood anyway — two contacts at +/- 10.75 cm, thirty courses at e = 145 cm
 *     give T = 499,634.5 uu tension against 771,312.5 uu of bond, a ratio of 1.5438 (pinned
 *     below); tension governs at 0.648. But the chain's 29 failing internal joints are in no
 *     problem (hole (3)), so a component failing internally leaves the region certifying.
 *   - at w = 4 the region swallows a failing chain joint and the pessimistic side goes infeasible:
 *     the region sees the failure only when it contains the failing joint, not its weight.
 *
 * So the fixture exercises two of the three unsoundnesses; the second is sharper — a surcharge
 * carries a force and a moment, no strength. Smaller misses: Q1's cost reasoning (a short lambda
 * climb is not a short pivot climb — the composite's live pose is 2,321 pivots / 4.2 s), and Q7's
 * mechanism (widths right, reason not).
 *
 * WHAT THIS SETTLES: §11 R2's open half. A closed sandwich is a heuristic, not a certificate, and
 * §5.6's fail-closed fallback is mandatory on every region solve. It does not settle how to repair
 * it — the w = 3 rung shows charging the weight correctly is not enough; the region would have to
 * carry the charged component's internal and interface joints too (§5.3's "different and larger
 * repair"). Limitation: this structure is infeasible before the deletion, so the counterexample is
 * not deletion-caused; the refuted claim is unconditional so that does not weaken it, but a
 * deletion-caused non-local failure (the wide-opening cover) would be stronger, a specified
 * follow-up in CURRENT_STATE.
 *
 * Cost: 8.0-10.3 s, opt-in fast tier; default suite untouched. Green on arrival, three bite-provers
 * (§9.5): X11 (chimney with no lean, so the structure stands -> 14 assertions), X6 (extractor keeps
 * out-of-region blocks -> 26 assertions), X13 (chimney one course short -> 9 assertions). Still no
 * prover: the plain wall's 149/385 pins and its feasible control. Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleSubUnityWallCertificateTest,
	"OracleSweepFast.RigidBlock.SubUnityWallCertificate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleSubUnityWallCertificateTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleFeasibilitySpikeSupport;

	const FSubUnityPins Pins;
	const double Started = FPlatformTime::Seconds();

	/*
	 * A refusal is not an infeasibility, and SpikeIsFeasible cannot tell them apart. Every
	 * conclusion is a verdict read off a solve, so a refusal manufactures or destroys the finding
	 * in silence.
	 */
	const auto MustAnswer = [this](const FPoseReading& Read, const FString& Where)
	{
		return TestTrue(
			*FString::Printf(
				TEXT("%s: the solve must ANSWER — a REFUSAL and an INFEASIBILITY are ")
				TEXT("indistinguishable downstream (both are bAnswered=false, lambda=0). ")
				TEXT("It said: %s"),
				*Where, Read.WhyNot.IsEmpty() ? TEXT("(nothing)") : *Read.WhyNot),
			Read.bAnswered);
	};

	struct FStrip
	{
		int32 Blocks = 0;
		int32 Joints = 0;
		int32 Charged = 0;
		bool bOptimistic = false;
		bool bPessimistic = false;
		bool bCloses = false;
		int32 OptimisticPivots = 0;
		int32 PessimisticPivots = 0;
		double Seconds = 0.0;
	};

	/*
	 * Both boundary conditions on one mask, repair (1) live on the pessimistic side and
	 * absent from the optimistic one — the same posing RepairedRegionalSandwich uses, written
	 * locally so a shared helper's change cannot silently move the other test's measurement.
	 */
	const auto PoseStrip = [this, &MustAnswer](
		const FOracleProblem& Full, const TArray<bool>& Mask, const FString& Where) -> FStrip
	{
		FStrip Out;

		FOracleProblem Optimistic;
		FRegionCounts OptimisticCounts;
		FSurchargeCounts OptimisticSurcharge;

		SpikeExtractRepairedRegion(
			Full, Mask, /*bGroundTheShell*/ true, ESpikeSurcharge::Carried,
			Optimistic, OptimisticCounts, OptimisticSurcharge);

		FOracleProblem Pessimistic;
		FRegionCounts PessimisticCounts;
		FSurchargeCounts PessimisticSurcharge;

		SpikeExtractRepairedRegion(
			Full, Mask, /*bGroundTheShell*/ false, ESpikeSurcharge::Carried,
			Pessimistic, PessimisticCounts, PessimisticSurcharge);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the OPTIMISTIC side must carry no surcharge — its soundness is a ")
				TEXT("restriction argument, and a restriction satisfies no row that was not in ")
				TEXT("the original problem"),
				*Where),
			Optimistic.AppliedForces.Num(), 0);

		const FPoseReading OptimisticRead = SpikeSolve(Optimistic);
		const FPoseReading PessimisticRead = SpikeSolve(Pessimistic);

		MustAnswer(OptimisticRead, FString::Printf(TEXT("%s GROUNDED-boundary"), *Where));
		MustAnswer(PessimisticRead, FString::Printf(TEXT("%s FREE-boundary"), *Where));

		Out.Blocks = OptimisticCounts.Blocks;
		Out.Joints = OptimisticCounts.Joints;
		Out.Charged = PessimisticSurcharge.ChargedBlocks;
		Out.bOptimistic = SpikeIsFeasible(OptimisticRead);
		Out.bPessimistic = SpikeIsFeasible(PessimisticRead);
		Out.bCloses = Out.bOptimistic == Out.bPessimistic;
		Out.OptimisticPivots = OptimisticRead.Pivots;
		Out.PessimisticPivots = PessimisticRead.Pivots;
		Out.Seconds = OptimisticRead.Seconds + PessimisticRead.Seconds;

		/* Repair (2)'s own precondition: a strip that does not reach the earth certifies nothing. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: a ground-anchored region must actually contain the earth, and this one ")
				TEXT("holds %d grounded blocks"),
				*Where, PessimisticCounts.Grounded),
			PessimisticCounts.Grounded > 0);

		TestEqual(
			*FString::Printf(TEXT("%s: no ORPHAN charged component"), *Where),
			PessimisticSurcharge.OrphanComponents, 0);

		TestEqual(
			*FString::Printf(
				TEXT("%s: no surcharge delivered onto a GROUNDED block, which writes no ")
				TEXT("equilibrium row and would swallow it"),
				*Where),
			PessimisticSurcharge.DiscardedOntoGroundedUu, 0.0);

		return Out;
	};

	/* PART 1 — the fixture, and the two global verdicts that make it one. */

	constexpr int32 WallCourses = 12;
	constexpr int32 WallCells = 12;
	constexpr int32 WallDeleteCourse = 6;

	FChimneyWall Composite;
	FString Why;

	const bool bCompositeLaid = SpikeBuildChimneyWall(
		WallCourses, WallCells, SpikeChimneyCourses, WallDeleteCourse, Composite, Why);

	if (!TestTrue(
			*FString::Printf(TEXT("sub-1.0: the chimney wall must lay (it said: %s)"), *Why),
			bCompositeLaid))
	{
		return true;
	}

	FChimneyWall Plain;
	FString PlainWhy;

	const bool bPlainLaid = SpikeBuildChimneyWall(
		WallCourses, WallCells, /*ChimneyCourses*/ 0, WallDeleteCourse, Plain, PlainWhy);

	if (!TestTrue(
			*FString::Printf(TEXT("sub-1.0: the plain wall must lay (it said: %s)"), *PlainWhy),
			bPlainLaid))
	{
		return true;
	}

	FOracleProblem CompositeFull;
	FOracleProblem PlainFull;

	const bool bCompositeBridged = BuildRigidBlockProblem(Composite.Structure, CompositeFull, Why);
	const bool bPlainBridged = BuildRigidBlockProblem(Plain.Structure, PlainFull, PlainWhy);

	if (!TestTrue(
			*FString::Printf(TEXT("sub-1.0: the bridge must represent the chimney wall (%s)"), *Why),
			bCompositeBridged)
		|| !TestTrue(
			*FString::Printf(TEXT("sub-1.0: the bridge must represent the plain wall (%s)"),
				*PlainWhy),
			bPlainBridged))
	{
		return true;
	}

	/*
	 * The two fixtures must be the same wall: the same brick is deleted from the same place in
	 * both, so any difference below is the chimney. A within-run comparison, not a transcription.
	 */
	TestEqual(
		TEXT("sub-1.0: the same brick is deleted from both fixtures (X)"),
		Composite.DeleteXCm, Plain.DeleteXCm);

	TestEqual(
		TEXT("sub-1.0: the same brick is deleted from both fixtures (Z)"),
		Composite.DeleteZCm, Plain.DeleteZCm);

	FOracleProblem CompositeGlobal = CompositeFull;
	CompositeGlobal.bGravityIsLive = false;

	FOracleProblem PlainGlobal = PlainFull;
	PlainGlobal.bGravityIsLive = false;

	const FPoseReading CompositeGlobalRead = SpikeSolve(CompositeGlobal);
	const FPoseReading PlainGlobalRead = SpikeSolve(PlainGlobal);
	const FPoseReading CompositeLiveRead = SpikeSolve(CompositeFull);

	const bool bCompositeFeasible = SpikeIsFeasible(CompositeGlobalRead);
	const bool bPlainFeasible = SpikeIsFeasible(PlainGlobalRead);

	{
		const FString Line = FString::Printf(
			TEXT("SUB-1.0 FIXTURE: composite blocks=%d joints=%d (chimney %d courses, root at ")
			TEXT("X=%.4f Z=%.4f, deletion at X=%.4f Z=%.4f) | composite DEAD feasible=%d ")
			TEXT("pivots=%d secs=%.3f | composite LIVE lambda*=%.17g answered=%d pivots=%d ")
			TEXT("secs=%.3f | plain blocks=%d joints=%d DEAD feasible=%d pivots=%d | whynot ")
			TEXT("dead='%s' live='%s' plain='%s'"),
			CompositeFull.Blocks.Num(), CompositeFull.Joints.Num(), Composite.ChimneyBlocks,
			Composite.RootXCm, Composite.RootZCm, Composite.DeleteXCm, Composite.DeleteZCm,
			bCompositeFeasible ? 1 : 0, CompositeGlobalRead.Pivots, CompositeGlobalRead.Seconds,
			CompositeLiveRead.Lambda, CompositeLiveRead.bAnswered ? 1 : 0,
			CompositeLiveRead.Pivots, CompositeLiveRead.Seconds,
			PlainFull.Blocks.Num(), PlainFull.Joints.Num(), bPlainFeasible ? 1 : 0,
			PlainGlobalRead.Pivots, *CompositeGlobalRead.WhyNot, *CompositeLiveRead.WhyNot,
			*PlainGlobalRead.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);
	}

	MustAnswer(CompositeGlobalRead, TEXT("sub-1.0: the whole composite, posed dead"));
	MustAnswer(CompositeLiveRead, TEXT("sub-1.0: the whole composite, posed live"));
	MustAnswer(PlainGlobalRead, TEXT("sub-1.0: the plain wall, posed dead"));

	/* ---- Sizes first, so nothing below can secretly be solving another structure. ---- */

	if (Pins.CompositeBlocks == INDEX_NONE || Pins.PlainBlocks == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sub-1.0: UNMEASURED SIZES — composite %d blocks / %d joints, plain %d / %d. ")
			TEXT("PREDICTED (Q3) 179/415 and 149/385."),
			CompositeFull.Blocks.Num(), CompositeFull.Joints.Num(),
			PlainFull.Blocks.Num(), PlainFull.Joints.Num()));
	}
	else
	{
		TestEqual(TEXT("sub-1.0: composite block count"),
			CompositeFull.Blocks.Num(), Pins.CompositeBlocks);
		TestEqual(TEXT("sub-1.0: composite joint count"),
			CompositeFull.Joints.Num(), Pins.CompositeJoints);
		TestEqual(TEXT("sub-1.0: the plain wall is RepairedRegionalSandwich's own fixture"),
			PlainFull.Blocks.Num(), Pins.PlainBlocks);
		TestEqual(TEXT("sub-1.0: and its joint count"),
			PlainFull.Joints.Num(), Pins.PlainJoints);

		TestEqual(
			TEXT("sub-1.0: the chimney is the ONLY difference — thirty blocks more, and thirty ")
			TEXT("joints more (its root bed plus twenty-nine chain beds)"),
			CompositeFull.Blocks.Num() - PlainFull.Blocks.Num(), SpikeChimneyCourses);
	}

	/* ---- The two verdicts that make this a fixture rather than a wall. ---- */

	TestTrue(
		*FString::Printf(
			TEXT("sub-1.0, THE WHOLE POINT: the composite must have NO admissible equilibrium at ")
			TEXT("lambda = 1, and it read feasible=%d. Without this the test is measuring a ")
			TEXT("standing wall and can say nothing about the pessimistic side."),
			bCompositeFeasible ? 1 : 0),
		!bCompositeFeasible);

	TestTrue(
		TEXT("sub-1.0: and the SAME WALL WITHOUT THE CHIMNEY must be FEASIBLE — so the chimney ")
		TEXT("is measurably the whole of the difference, rather than a structure that was ")
		TEXT("failing for some reason nobody attributed"),
		bPlainFeasible);

	if (Pins.CompositeLambdaLo < 0.0)
	{
		AddError(FString::Printf(
			TEXT("sub-1.0: UNMEASURED COMPOSITE lambda* — the live pose read %.17g in %d pivots ")
			TEXT("(%.3f s). PREDICTED (Q1) the bare stack's [0.440484, 0.440502], because the ")
			TEXT("binding joint is inside the chain; named fallback ~1.5 if the chimney's ROOT ")
			TEXT("joint governs instead."),
			CompositeLiveRead.Lambda, CompositeLiveRead.Pivots, CompositeLiveRead.Seconds));
	}
	else
	{
		TestTrue(
			*FString::Printf(
				TEXT("sub-1.0: the composite's lambda* must lie in [%.9g, %.9g] and was %.17g. ")
				TEXT("This is how far below 1.0 the structure the strip certifies actually is."),
				Pins.CompositeLambdaLo, Pins.CompositeLambdaHi, CompositeLiveRead.Lambda),
			CompositeLiveRead.Lambda >= Pins.CompositeLambdaLo
				&& CompositeLiveRead.Lambda <= Pins.CompositeLambdaHi);
	}

	if (Pins.CompositeGlobalPivots == INDEX_NONE || Pins.PlainGlobalPivots == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sub-1.0: UNMEASURED GLOBAL COST — the composite answers feasibility in %d ")
			TEXT("pivots and the plain wall in %d; these are what a regional lever exists to ")
			TEXT("avoid"),
			CompositeGlobalRead.Pivots, PlainGlobalRead.Pivots));
	}
	else
	{
		TestEqual(TEXT("sub-1.0: the composite's feasibility pivot count"),
			CompositeGlobalRead.Pivots, Pins.CompositeGlobalPivots);
		TestEqual(TEXT("sub-1.0: the plain wall's feasibility pivot count"),
			PlainGlobalRead.Pivots, Pins.PlainGlobalPivots);
	}

	/*
	 * PART 2 — the strip ladder, and what each rung certifies. A closed sandwich here claims the
	 * whole structure feasible about one Part 1 just measured as having no equilibrium, so every
	 * closure is a false certificate and their count is the headline — pinned as a measurement,
	 * not asserted zero, since a test demanding zero would assert the design's claim.
	 */

	int32 FalseCertificates = 0;
	int32 ClosingWidth = INDEX_NONE;

	FStrip ClosingStrip;
	TArray<bool> ClosingMask;

	/*
	 * The rung that charges the failing material in full and certifies anyway, kept out of the
	 * loop because it is a different finding.
	 */
	int32 ChargedCertifierWidth = INDEX_NONE;
	FStrip ChargedCertifier;

	/*
	 * Q7: closes at w = 0..3, not 4 or 5 (w = 4 is the first strip containing the chimney root,
	 * from where the chimney above has no ground path, gets charged, and sinks the pessimistic
	 * side). Measured 2026-08-16: widths and count as predicted, the charged column not — w = 3
	 * already charges all thirty chimney blocks and certifies anyway (hole (3): the chain's 29
	 * failing internal joints are in no problem). Block counts are 24w + 41 up to w = 3.
	 */
	TArray<FStripPin> StripPins;
	StripPins.Add({ 0,  41,  0, 1, 1 });
	StripPins.Add({ 1,  65,  0, 1, 1 });
	StripPins.Add({ 2,  89,  0, 1, 1 });
	StripPins.Add({ 3, 113, 30, 1, 1 });
	StripPins.Add({ 4, 139, 28, 1, 0 });
	StripPins.Add({ 5, 153, 26, 1, 0 });

	for (const FStripPin& Pin : StripPins)
	{
		TArray<bool> Mask;
		SpikeGroundStripMask(CompositeFull, Composite.DeleteXCm, Pin.HalfWidthCells, Mask);

		const FStrip Strip = PoseStrip(
			CompositeFull, Mask, FString::Printf(TEXT("sub-1.0 strip w=%d"), Pin.HalfWidthCells));

		const bool bFalseCertificate = Strip.bCloses && (Strip.bOptimistic != bCompositeFeasible);

		if (bFalseCertificate)
		{
			++FalseCertificates;
		}

		if (Strip.bCloses && ClosingWidth == INDEX_NONE)
		{
			ClosingWidth = Pin.HalfWidthCells;
			ClosingStrip = Strip;
			ClosingMask = Mask;
		}

		if (bFalseCertificate && Strip.Charged > 0 && ChargedCertifierWidth == INDEX_NONE)
		{
			ChargedCertifierWidth = Pin.HalfWidthCells;
			ChargedCertifier = Strip;
		}

		const FString Line = FString::Printf(
			TEXT("SUB-1.0 STRIP w=%d: blocks=%d (%.1f%% of %d) joints=%d charged=%d | ")
			TEXT("optimistic=%d pivots=%d | pessimistic=%d pivots=%d | closes=%d | WHOLE ")
			TEXT("STRUCTURE feasible=%d | FALSE CERTIFICATE=%d | secs=%.3f"),
			Pin.HalfWidthCells, Strip.Blocks,
			100.0 * double(Strip.Blocks) / double(CompositeFull.Blocks.Num()),
			CompositeFull.Blocks.Num(), Strip.Joints, Strip.Charged,
			Strip.bOptimistic ? 1 : 0, Strip.OptimisticPivots,
			Strip.bPessimistic ? 1 : 0, Strip.PessimisticPivots,
			Strip.bCloses ? 1 : 0, bCompositeFeasible ? 1 : 0,
			bFalseCertificate ? 1 : 0, Strip.Seconds);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		if (Pin.Blocks == INDEX_NONE || Pin.Charged == INDEX_NONE
			|| Pin.Optimistic == INDEX_NONE || Pin.Pessimistic == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("sub-1.0 strip w=%d: UNMEASURED RUNG — pin blocks=%d charged=%d ")
				TEXT("optimistic=%d pessimistic=%d"),
				Pin.HalfWidthCells, Strip.Blocks, Strip.Charged,
				Strip.bOptimistic ? 1 : 0, Strip.bPessimistic ? 1 : 0));
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("sub-1.0 strip w=%d: region block count"),
				Pin.HalfWidthCells), Strip.Blocks, Pin.Blocks);

			TestEqual(
				*FString::Printf(
					TEXT("sub-1.0 strip w=%d: how many omitted blocks the region is CHARGED for ")
					TEXT("— zero while the chimney can still reach the ground without the strip, ")
					TEXT("and non-zero from the width that swallows its root"),
					Pin.HalfWidthCells),
				Strip.Charged, Pin.Charged);

			TestEqual(*FString::Printf(TEXT("sub-1.0 strip w=%d: GROUNDED-boundary verdict"),
				Pin.HalfWidthCells), Strip.bOptimistic ? 1 : 0, Pin.Optimistic);

			TestEqual(*FString::Printf(TEXT("sub-1.0 strip w=%d: FREE-boundary verdict"),
				Pin.HalfWidthCells), Strip.bPessimistic ? 1 : 0, Pin.Pessimistic);
		}
	}

	if (Pins.FalseCertificates == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sub-1.0: UNMEASURED FALSE-CERTIFICATE COUNT — %d of the six strip widths ")
			TEXT("closed while the whole structure has no equilibrium. PREDICTED (Q6/Q7) FOUR: ")
			TEXT("w = 0, 1, 2 and 3. Zero would mean the repaired pessimistic side held on the ")
			TEXT("only fixture shape that could catch it lying."),
			FalseCertificates));
	}
	else
	{
		TestEqual(
			TEXT("sub-1.0, THE HEADLINE: how many of the six ground-anchored strips certify ")
			TEXT("'the whole structure is feasible' about a structure that has no equilibrium. ")
			TEXT("This is pinned as a MEASUREMENT, not asserted to be zero — a test that ")
			TEXT("demanded zero would be asserting the design's claim instead of checking it."),
			FalseCertificates, Pins.FalseCertificates);
	}

	/*
	 * The second unsoundness, which the prediction did not contain. Q7 expected every false
	 * certificate from the carried-set rule being blind (hole (1)); one rung is not blind — it
	 * charges the whole chimney on the vertical through its centre of gravity and certifies
	 * anyway. Hole (1) is present too (the interface joint is dropped, not exercised) but does not
	 * matter (it stood at 1.5438 of bond). The certificate rests on hole (3) alone: the charged
	 * component's internal joints vanish, so the 29 failing chain joints are in no problem. A
	 * surcharge carries a force and a moment, no strength.
	 */
	if (Pins.FalseCertificates != INDEX_NONE && Pins.FalseCertificates > 0)
	{
		TestTrue(
			*FString::Printf(
				TEXT("sub-1.0: at least one FALSE certificate must come from a strip that ")
				TEXT("CHARGED the failing material in full and certified anyway — measured at ")
				TEXT("half-width %d, %d blocks charged, pessimistic feasible=%d. If this ever ")
				TEXT("stops holding, the surcharge has started carrying the charged component's ")
				TEXT("own strength and hole (3) of PROMOTION_DESIGN §5.3's box has moved."),
				ChargedCertifierWidth, ChargedCertifier.Charged,
				ChargedCertifier.bPessimistic ? 1 : 0),
			ChargedCertifierWidth != INDEX_NONE && ChargedCertifier.Charged > 0);

		/*
		 * The bypassed joint, priced by hand so the attribution is a pin, not a sentence:
		 * attributing the certificate to hole (3) alone claims the bypassed joint would have stood,
		 * arithmetic on the fixture's constants that can disagree with the solver rather than echo
		 * it. Two-contact form: contacts at +/- h with area A/2 capped at f_t x A/2; n1 + n2 = W and
		 * h x (n2 - n1) = W x e, so the far contact carries T = W x (e/h - 1)/2 tension. The other
		 * two axes were worked first (ComputeUtilisation returns the worst of three, TRAPS):
		 * near-contact compression is ~0.053 of the 10 MPa cap and a vertical surcharge puts no
		 * shear through a bed joint, so tension governs at 0.648.
		 */
		const double ChimneyWeightUu =
			double(SpikeChimneyCourses) * SpikeBrickMassKg * OracleGravityCmPerSecondSquared;

		/* Course c sits c x lean outboard of the root, so the mean of 0..N-1 is the lever. */
		const double LeverArmCm =
			0.5 * double(SpikeChimneyCourses - 1) * SpikeChimneyLeanCm;

		const double HalfLengthCm = 0.5 * SpikeBrickLengthCm;
		const double ContactAreaSqCm = 0.5 * SpikeBrickLengthCm * SpikeBrickWidthCm;

		/* 1 MPa = 100 N/cm2 and 1 N = 100 uu, derived here rather than imported. */
		const double UuPerMPaSqCm = 100.0 * 100.0;

		const double TensionDemandUu =
			ChimneyWeightUu * (LeverArmCm / HalfLengthCm - 1.0) * 0.5;
		const double TensionCapacityUu =
			GeneralPurposeMortar.TensileStrengthMPa * ContactAreaSqCm * UuPerMPaSqCm;
		const double CompressionDemandUu =
			ChimneyWeightUu * (LeverArmCm / HalfLengthCm + 1.0) * 0.5;
		const double CrushCapacityUu =
			GeneralPurposeMortar.CompressiveStrengthMPa * ContactAreaSqCm * UuPerMPaSqCm;

		const double BypassedMargin = TensionCapacityUu / TensionDemandUu;

		const FString BypassLine = FString::Printf(
			TEXT("SUB-1.0 BYPASSED JOINT (the root bed joint the surcharge never crosses): ")
			TEXT("W=%.17g uu at e=%.17g cm, h=%.17g cm, contact area=%.17g cm2 | TENSION ")
			TEXT("demand=%.17g capacity=%.17g margin=%.17g | COMPRESSION demand=%.17g ")
			TEXT("capacity=%.17g utilisation=%.6g"),
			ChimneyWeightUu, LeverArmCm, HalfLengthCm, ContactAreaSqCm,
			TensionDemandUu, TensionCapacityUu, BypassedMargin,
			CompressionDemandUu, CrushCapacityUu, CompressionDemandUu / CrushCapacityUu);

		UE_LOG(LogTemp, Display, TEXT("%s"), *BypassLine);
		AddInfo(BypassLine);

		TestTrue(
			*FString::Printf(
				TEXT("sub-1.0: the axis must be TENSION, not crushing — the near contact reads ")
				TEXT("%.6g of its cap against the far contact's %.6g, and an attribution made ")
				TEXT("against the wrong axis is how this project has been wrong before"),
				CompressionDemandUu / CrushCapacityUu, TensionDemandUu / TensionCapacityUu),
			TensionDemandUu / TensionCapacityUu > CompressionDemandUu / CrushCapacityUu);

		TestTrue(
			*FString::Printf(
				TEXT("sub-1.0: the BYPASSED interface joint would have STOOD — %.17g uu of ")
				TEXT("tension against %.17g uu of two-contact bond, a margin of %.17g. This is ")
				TEXT("what licenses attributing the false certificate to hole (3) ALONE: hole ")
				TEXT("(1) is present (the joint is dropped, never checked) and is not what the ")
				TEXT("certificate rests on."),
				TensionDemandUu, TensionCapacityUu, BypassedMargin),
			BypassedMargin > 1.0);

		TestTrue(
			*FString::Printf(
				TEXT("sub-1.0: and that margin is pinned in [%.9g, %.9g] rather than left as a ")
				TEXT("greater-than, because a bare 'it stands' would survive the fixture's ")
				TEXT("geometry moving by a factor. It read %.17g."),
				SpikeBypassedMarginLo, SpikeBypassedMarginHi, BypassedMargin),
			BypassedMargin >= SpikeBypassedMarginLo && BypassedMargin <= SpikeBypassedMarginHi);
	}

	/*
	 * PART 3 — why it happens, as an identity rather than a sentence. The certified region problem
	 * is the same whether or not a collapsing chimney is attached: the extractor drops every
	 * chimney block and joint, and the Carried rule charges nothing (the chimney reaches ground
	 * down the wall's columns). Same blocks, joints, verdicts, pivots — a region whose answer
	 * cannot depend on the material it dropped cannot be a bound on a structure that includes it.
	 */

	/*
	 * A skip must be loud. Parts 3 and 4 hang off a sandwich having closed, and a run where none
	 * does would otherwise pass with the identity and the "how false" number never evaluated.
	 * Found by mutation X11.
	 */
	if (ClosingWidth == INDEX_NONE)
	{
		AddError(
			TEXT("sub-1.0: NO STRIP CLOSED at any width, so the IDENTITY and the certified ")
			TEXT("strip's own lambda* were never evaluated. That is either the finding ")
			TEXT("inverting — the repaired pessimistic side holding on this fixture, which is a ")
			TEXT("result and must be pinned as one — or a fixture that stopped being what it ")
			TEXT("was built to be. It is never a pass."));
	}

	if (ClosingWidth != INDEX_NONE)
	{
		TArray<bool> PlainMask;
		SpikeGroundStripMask(PlainFull, Plain.DeleteXCm, ClosingWidth, PlainMask);

		const FStrip PlainStrip = PoseStrip(
			PlainFull, PlainMask,
			FString::Printf(TEXT("sub-1.0 CHIMNEY-FREE strip w=%d"), ClosingWidth));

		const FString Line = FString::Printf(
			TEXT("SUB-1.0 IDENTITY w=%d: with chimney blocks=%d joints=%d charged=%d ")
			TEXT("optimistic=%d/%d pessimistic=%d/%d | WITHOUT chimney blocks=%d joints=%d ")
			TEXT("charged=%d optimistic=%d/%d pessimistic=%d/%d"),
			ClosingWidth, ClosingStrip.Blocks, ClosingStrip.Joints, ClosingStrip.Charged,
			ClosingStrip.bOptimistic ? 1 : 0, ClosingStrip.OptimisticPivots,
			ClosingStrip.bPessimistic ? 1 : 0, ClosingStrip.PessimisticPivots,
			PlainStrip.Blocks, PlainStrip.Joints, PlainStrip.Charged,
			PlainStrip.bOptimistic ? 1 : 0, PlainStrip.OptimisticPivots,
			PlainStrip.bPessimistic ? 1 : 0, PlainStrip.PessimisticPivots);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		TestEqual(
			TEXT("sub-1.0 IDENTITY: the certified region has the same block count with the ")
			TEXT("chimney attached as without it"),
			ClosingStrip.Blocks, PlainStrip.Blocks);

		TestEqual(
			TEXT("sub-1.0 IDENTITY: and the same joint count — the extractor drops every ")
			TEXT("chimney joint"),
			ClosingStrip.Joints, PlainStrip.Joints);

		TestEqual(
			TEXT("sub-1.0 IDENTITY: and charges the same nothing — the Carried rule sees a ")
			TEXT("chimney that reaches the ground down the wall's own columns"),
			ClosingStrip.Charged, PlainStrip.Charged);

		TestEqual(
			TEXT("sub-1.0 IDENTITY: the GROUNDED-boundary side takes the same pivot path"),
			ClosingStrip.OptimisticPivots, PlainStrip.OptimisticPivots);

		TestEqual(
			TEXT("sub-1.0 IDENTITY, THE MECHANISM: the FREE-boundary side takes the same pivot ")
			TEXT("path and returns the same verdict whether or not a collapsing chimney is ")
			TEXT("attached to the wall. A region whose answer CANNOT DEPEND on the material it ")
			TEXT("dropped cannot be a bound on a structure that includes it — that is why the ")
			TEXT("certificate above is what it is, stated as an equation rather than as prose."),
			ClosingStrip.PessimisticPivots, PlainStrip.PessimisticPivots);

		TestEqual(
			TEXT("sub-1.0 IDENTITY: and the same FREE-boundary verdict"),
			ClosingStrip.bPessimistic ? 1 : 0, PlainStrip.bPessimistic ? 1 : 0);

		/*
		 * PART 4 — how false, as a number. Part D's precedent: a boolean trio only says the
		 * certificate is on the wrong side of 1.0. The certified region's own lambda* says whether
		 * the failure is a knife edge a tighter extractor could tune away, or a gap no boundary rule
		 * can close.
		 */

		FOracleProblem CertifiedLive;
		FRegionCounts CertifiedCounts;
		FSurchargeCounts CertifiedSurcharge;

		SpikeExtractRepairedRegion(
			CompositeFull, ClosingMask, /*bGroundTheShell*/ false, ESpikeSurcharge::Carried,
			CertifiedLive, CertifiedCounts, CertifiedSurcharge);

		CertifiedLive.bGravityIsLive = true;

		const FPoseReading CertifiedLiveRead = SpikeSolve(CertifiedLive);

		MustAnswer(CertifiedLiveRead, TEXT("sub-1.0: the certified strip, posed live"));

		const FString RatioLine = FString::Printf(
			TEXT("SUB-1.0 HOW FALSE: the certified strip's own lambda*=%.17g (%d pivots) against ")
			TEXT("the whole structure's %.17g — the region the sandwich certified stands at ")
			TEXT("%.1fx the load under which the structure it certified has no equilibrium"),
			CertifiedLiveRead.Lambda, CertifiedLiveRead.Pivots, CompositeLiveRead.Lambda,
			CompositeLiveRead.Lambda > 0.0
				? CertifiedLiveRead.Lambda / CompositeLiveRead.Lambda : -1.0);

		UE_LOG(LogTemp, Display, TEXT("%s"), *RatioLine);
		AddInfo(RatioLine);

		if (Pins.StripLambdaLo < 0.0)
		{
			AddError(FString::Printf(
				TEXT("sub-1.0: UNMEASURED CERTIFIED-STRIP lambda* — it read %.17g in %d pivots. ")
				TEXT("PREDICTED (Q8) 400-1,100, a ratio of ~1,000-2,500x against the structure's ")
				TEXT("own lambda*, an order of magnitude past PART D's 41.96x."),
				CertifiedLiveRead.Lambda, CertifiedLiveRead.Pivots));
		}
		else
		{
			TestTrue(
				*FString::Printf(
					TEXT("sub-1.0: the certified strip's OWN lambda* must lie in [%.9g, %.9g] and ")
					TEXT("was %.17g. Measured on this exact region and never scaled from another ")
					TEXT("fixture — the point of the number is that the certified region is ")
					TEXT("nowhere near the boundary, so no tightening of a boundary rule closes ")
					TEXT("a gap of that size."),
					Pins.StripLambdaLo, Pins.StripLambdaHi, CertifiedLiveRead.Lambda),
				CertifiedLiveRead.Lambda >= Pins.StripLambdaLo
					&& CertifiedLiveRead.Lambda <= Pins.StripLambdaHi);
		}
	}

	/*
	 * PART 5 — the contrast: the same structure, cut where the failure is. The repair is not
	 * useless; this row says where it works. Cut the strip at the chimney's root instead of the
	 * deletion and the chimney above becomes a component with no ground path, so the Carried rule
	 * charges it and the pessimistic side reads infeasible — the sandwich opens, no certificate,
	 * correct. So the same structure gives a false certificate at one cut and a correct refusal at
	 * another, differing exactly in whether the failing material has a ground path the region does
	 * not provide (unsoundness (1) of §5.3's box, demonstrated).
	 */

	{
		TArray<bool> ContrastMask;
		SpikeGroundStripMask(CompositeFull, Composite.RootXCm, /*HalfWidthCells*/ 0, ContrastMask);

		const FStrip Contrast = PoseStrip(
			CompositeFull, ContrastMask, TEXT("sub-1.0 CONTRAST strip at the chimney root"));

		const FString Line = FString::Printf(
			TEXT("SUB-1.0 CONTRAST (strip cut at the chimney root X=%.4f, w=0): blocks=%d ")
			TEXT("joints=%d charged=%d | optimistic=%d pivots=%d | pessimistic=%d pivots=%d | ")
			TEXT("closes=%d"),
			Composite.RootXCm, Contrast.Blocks, Contrast.Joints, Contrast.Charged,
			Contrast.bOptimistic ? 1 : 0, Contrast.OptimisticPivots,
			Contrast.bPessimistic ? 1 : 0, Contrast.PessimisticPivots,
			Contrast.bCloses ? 1 : 0);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		if (Pins.ContrastBlocks == INDEX_NONE || Pins.ContrastCharged == INDEX_NONE
			|| Pins.ContrastOptimistic == INDEX_NONE || Pins.ContrastPessimistic == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("sub-1.0 CONTRAST: UNMEASURED — pin blocks=%d charged=%d optimistic=%d ")
				TEXT("pessimistic=%d. PREDICTED (Q9) ~27 charged, optimistic feasible, ")
				TEXT("pessimistic INFEASIBLE, so no certificate is issued."),
				Contrast.Blocks, Contrast.Charged,
				Contrast.bOptimistic ? 1 : 0, Contrast.bPessimistic ? 1 : 0));
		}
		else
		{
			TestEqual(TEXT("sub-1.0 CONTRAST: region block count"),
				Contrast.Blocks, Pins.ContrastBlocks);

			TestEqual(
				TEXT("sub-1.0 CONTRAST: the chimney above this strip has no ground path of its ")
				TEXT("own, so the Carried rule charges it — this is repair (1) doing the work it ")
				TEXT("was ruled for, on the same structure where it charged nothing"),
				Contrast.Charged, Pins.ContrastCharged);

			TestEqual(TEXT("sub-1.0 CONTRAST: GROUNDED-boundary verdict"),
				Contrast.bOptimistic ? 1 : 0, Pins.ContrastOptimistic);

			TestEqual(
				TEXT("sub-1.0 CONTRAST: FREE-boundary verdict — the two sides disagree, so the ")
				TEXT("rule says GROW THE REGION and no certificate is issued"),
				Contrast.bPessimistic ? 1 : 0, Pins.ContrastPessimistic);
		}
	}

	{
		const FString Line = FString::Printf(
			TEXT("SUB-1.0 TOTALS: %d false certificates over six strip widths. Test took %.3f s."),
			FalseCertificates, FPlatformTime::Seconds() - Started);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
