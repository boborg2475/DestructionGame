// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 6d OF THE STEP-4 PROMOTION — THE PER-BLOCK DEAD/LIVE SPLIT, RED.
 *
 * THE BEHAVIOUR UNDER TEST, IN ONE SENTENCE: with a SURCHARGE's own weight posed LIVE and the
 * rest of a structure's self-weight posed DEAD, lambda* becomes the finite "how many times the
 * surcharge before it hinges" margin, DISTINCT from the LambdaCap the same fixture reads when
 * every block's gravity is uniform-live.
 *
 * WHY THIS DRIVES A REAL CAPABILITY GAP. The oracle already has a GLOBAL dead/live switch
 * (FOracleProblem::bGravityIsLive) and a per-force one (FOracleAppliedForce::bLive), but NO way
 * to designate a SUBSET of a structure's self-weight live while the rest stays dead. The global
 * switch scales EVERY block's weight by the same lambda, so a multiplier-on-own-weight measure
 * cannot tell a surcharge's DEMAND apart from the pre-compression that STEADIES it — which is
 * exactly why the scenario pair wall-15 and wall-16 (running bond, 10 courses, differing only in
 * which course projects) read one identical lambda* = 868.623736 under uniform gravity while
 * production separates them ~31.6x at the header's own bed joint (RigidBlockOracleSweepTest.cpp
 * pins that identity + ratio). PROMOTION_DESIGN §10/§472 and SHED_PATH Phase A 6d name the fix:
 * make the split load-bearing per block, so wall-15's six projecting-header courses can be marked
 * LIVE. It is the prerequisite for the shed's roof-as-surcharge and step 7's live impulses.
 *
 * THE FIXTURE IS A HAND-DERIVED LEVER, NOT A WALL. The wall-15/16 separation is the eventual
 * OUTCOME driver, but it needs a production posing path (the bridge tagging the surcharge courses)
 * that this slice's dev work adds; posing it today would need a bridge stub too. So the executable
 * red here isolates the MECHANISM on a tiny hand-built FOracleProblem whose lambda* is exact:
 *
 *   - A grounded seat (block 0) and a HEADER (block 1, mass Mh) resting on it through one dry bed
 *     joint J0 whose contact bearing spans +-h about x = 0. The header centroid is at x = 0, so
 *     its OWN weight sits dead-centre in the bearing and, alone, overturns nothing. The joints
 *     carry NO tensile bond (dry stone) and an effectively infinite COMPRESSIVE strength, so the
 *     ONLY governing constraint is the no-tension bound — an inboard contact going into tension.
 *     That matters: leave f_c at dry stone's 30 MPa and CRUSHING at the outboard contact governs
 *     the uniform pose instead (30 MPa over a 200 cm^2 bed gives lambda* ~2551 at k=3, measured),
 *     silently replacing the clean cap contrast with a compression reading. Compression is lifted
 *     out of the way on purpose so the axis under test is the one that actually governs.
 *   - A SURCHARGE of k identical blocks (mass Ms each) stacked in a centred vertical column at a
 *     lever x = Lx, bearing on the header's tail through a dry joint. The column is centred on
 *     itself, so its internal joints carry pure centred compression and never govern; its whole
 *     weight k*Ms lands on the header at x = Lx.
 *
 * THE HAND STATICS (dry stone, f_t = 0, so pure no-tension tipping — g and strength both cancel):
 * the header bed joint J0 governs. Everything above it must keep its resultant inside the bearing
 * [-h, +h]. With the header weight dead (fixed, at x = 0) and the surcharge weight live (scaled by
 * lambda, at x = Lx), the inboard contact lifts when
 *
 *     lambda * k*Ms * Lx  =  h * (Mh + lambda * k*Ms)      [ resultant reaches +h ]
 *  => lambda* = h * Mh / ( k * Ms * (Lx - h) )              [ the LIVE-SURCHARGE margin ]
 *
 * With h = 10, Mh = 12, Ms = 1, Lx = 30 this is 120 / (20 k) = 6 / k. Contrast the UNIFORM pose
 * (every block live): the resultant is x = k*Ms*Lx / (Mh + k*Ms) = 30k/(12+k), INDEPENDENT of
 * lambda, so the fixture either stands at EVERY multiplier (lambda* = LambdaCap, for k <= 6) or at
 * NONE (lambda* = 0, for k > 6). The split and the uniform pose therefore separate — finite margin
 * versus cap — exactly as wall-15 must separate from wall-16.
 *
 * WHY IT IS RED, AND FOR THE RIGHT REASON. FOracleBlock::bLiveGravity is a Slice-6d COMPILE SEAM
 * declared for this test; the solver's assembly still branches only on the GLOBAL bGravityIsLive
 * (RigidBlockOracle.cpp ~line 1548), so the per-block designation is IGNORED. Under the split pose
 * (global gravity dead, surcharge blocks tagged live) production therefore treats ALL gravity as
 * dead: for k <= 6 the dead resultant is inside the bearing so the feasibility formulation stands
 * and reports lambda* = LambdaCap; for k > 6 it is outside so the dead loads admit no equilibrium
 * and lambda* = 0. Either way it is NOT the finite 6/k the split demands. The red is missing
 * BEHAVIOUR (the split ignored), never a type error or a broken fixture — the same shape as the
 * bFirstCrackRows / bMinViolationReadout seams. The UNIFORM-pose assertions are correct today and
 * green on arrival; they are the contrast that proves the split, and each row asserts the two
 * poses MUST differ so a solver that collapsed them back to one number cannot pass.
 *
 * NEEDS A TICKING WORLD: NO. Pure arithmetic on a hand-built FOracleProblem, like the oracle it
 * validates. Determinism is asserted (two solves, bit-identical) per the oracle's contract.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace DeadLiveSplitSurchargeSupport
{
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * THE LEVER'S DIMENSIONS AND MASSES, chosen so lambda* = 6/k is a clean hand value.
	 * ================================================================================ */

	constexpr double HeaderMassKg = 12.0;
	constexpr double SurchargeMassKg = 1.0;

	/** Header centroid on the bearing centre; the surcharge column a lever Lx outboard. */
	constexpr double BearingHalfLengthCm = 10.0;   // h
	constexpr double SurchargeLeverCm = 30.0;       // Lx

	constexpr double BedAreaSqCm = 200.0;
	constexpr double SurchargeAreaSqCm = 100.0;

	/**
	 * Dry stone (no tensile bond, so pure no-tension tipping) with compression lifted to
	 * effectively infinite, so crushing NEVER governs and the axis under test is the one that
	 * actually binds. See the file header for why the unmodified 30 MPa would hijack the pose.
	 */
	FConnectionStrength TippingOnlyStrength()
	{
		FConnectionStrength S = DestructionProfiles::DryStone;
		S.CompressiveStrengthMPa = 1.0e9;
		return S;
	}

	/**
	 * The live-surcharge margin, derived here from the statics in the file header, NOT read
	 * from the oracle: lambda* = h*Mh / (k*Ms*(Lx - h)). For the chosen numbers this is 6/k.
	 */
	double SplitSurchargeLambda(int32 SurchargeCount)
	{
		return BearingHalfLengthCm * HeaderMassKg
			/ (double(SurchargeCount) * SurchargeMassKg * (SurchargeLeverCm - BearingHalfLengthCm));
	}

	/**
	 * The uniform-gravity answer for the SAME fixture: the resultant x = k*Ms*Lx/(Mh + k*Ms) is
	 * independent of lambda, so it is the cap while that sits inside the bearing and 0 once it
	 * walks out. The boundary is k*Ms*Lx = h*(Mh + k*Ms); for the chosen numbers, k = 6.
	 */
	double UniformGravityLambda(int32 SurchargeCount)
	{
		const double ResultantX = double(SurchargeCount) * SurchargeMassKg * SurchargeLeverCm
			/ (HeaderMassKg + double(SurchargeCount) * SurchargeMassKg);

		/* Closed constraint set: the resultant exactly AT the edge still stands (knife edge). */
		return ResultantX <= BearingHalfLengthCm ? LambdaCap : 0.0;
	}

	/**
	 * The lever, built two ways from one geometry. bSplit true poses the HEADER dead and the
	 * SURCHARGE blocks live (the per-block field, global gravity dead); false poses everything
	 * uniform-live (the global switch, no per-block tags). The geometry is byte-for-byte the same
	 * so the only difference measured is the split itself.
	 */
	FOracleProblem BuildLever(int32 SurchargeCount, bool bSplit)
	{
		const FConnectionStrength Strength = TippingOnlyStrength();

		FOracleProblem Problem;

		/* Block 0: the grounded seat. Block 1: the header, centroid on the bearing centre. */
		FOracleBlock Seat;
		Seat.MassKg = HeaderMassKg;
		Seat.CentroidXCm = 0.0;
		Seat.CentroidZCm = 0.0;
		Seat.bGrounded = true;
		Problem.Blocks.Add(Seat);

		FOracleBlock Header;
		Header.MassKg = HeaderMassKg;
		Header.CentroidXCm = 0.0;
		Header.CentroidZCm = 10.0;
		Header.bGrounded = false;
		Header.bLiveGravity = false; // the header's own weight is DEAD under the split
		Problem.Blocks.Add(Header);

		/* J0: the governing dry bed joint, bearing [-h, +h] about x = 0. */
		FOracleJoint Bed;
		Bed.BlockA = 0;
		Bed.BlockB = 1;
		Bed.NormalX = 0.0;
		Bed.NormalZ = 1.0;
		Bed.CentreXCm = 0.0;
		Bed.CentreZCm = 5.0;
		Bed.HalfLengthCm = BearingHalfLengthCm;
		Bed.AreaSqCm = BedAreaSqCm;
		Bed.Strength = Strength;
		Problem.Joints.Add(Bed);

		/*
		 * The surcharge: a centred vertical column at x = Lx, each block resting on the one below
		 * (or on the header, for the first). Centred contacts carry pure compression, so only the
		 * header bed joint J0 can govern.
		 */
		int32 LowerBlock = 1; // the header carries the first surcharge block
		double NextCentroidZ = 20.0;

		for (int32 Course = 0; Course < SurchargeCount; ++Course)
		{
			FOracleBlock Sur;
			Sur.MassKg = SurchargeMassKg;
			Sur.CentroidXCm = SurchargeLeverCm;
			Sur.CentroidZCm = NextCentroidZ;
			Sur.bGrounded = false;
			Sur.bLiveGravity = true; // the surcharge's own weight is LIVE under the split
			const int32 ThisBlock = Problem.Blocks.Add(Sur);

			FOracleJoint Seatlet;
			Seatlet.BlockA = LowerBlock;
			Seatlet.BlockB = ThisBlock;
			Seatlet.NormalX = 0.0;
			Seatlet.NormalZ = 1.0;
			Seatlet.CentreXCm = SurchargeLeverCm;
			Seatlet.CentreZCm = NextCentroidZ - 5.0;
			Seatlet.HalfLengthCm = 5.0;
			Seatlet.AreaSqCm = SurchargeAreaSqCm;
			Seatlet.Strength = Strength;
			Problem.Joints.Add(Seatlet);

			LowerBlock = ThisBlock;
			NextCentroidZ += 10.0;
		}

		/*
		 * THE POSE. Split: global gravity dead so the per-block bLiveGravity flags above decide;
		 * uniform: global gravity live and the per-block flags are irrelevant (the solver reads
		 * only the global switch either way today — which is the point of the red).
		 */
		Problem.bGravityIsLive = !bSplit;

		return Problem;
	}
}

/**
 * THE RED: the per-block dead/live split gives the finite surcharge margin, distinct from the cap
 * the uniform pose reads. Parameterised over the surcharge count so "how many surcharges before it
 * hinges" is data, not code.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeadLiveSplitSurchargeTest,
	"DestructionGame.Oracle.RigidBlock.DeadLiveSplit.LiveSurchargeHingesTheHeader",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDeadLiveSplitSurchargeTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace DeadLiveSplitSurchargeSupport;
	using namespace DestructionProfiles;

	/* FIXTURE PRECONDITIONS: the strength the hand statics lean on — NO tensile bond (so pure
	 * no-tension tipping) and compression lifted out of the way (so crushing never hijacks the
	 * governing axis) — asserted rather than trusted. */
	TestEqual(TEXT("FIXTURE: the lever's strength has no tensile bond (pure no-tension tipping)"),
		TippingOnlyStrength().TensileStrengthMPa, 0.0);
	TestTrue(TEXT("FIXTURE: the lever's compression is lifted so crushing never governs"),
		TippingOnlyStrength().CompressiveStrengthMPa >= 1.0e9);

	struct FRow
	{
		int32 SurchargeCount;
		EOracleOutcome SplitOutcome;
	};

	/* k = 1,3,6 stand (lambda* 6.0, 2.0, 1.0); k = 12 hinges below the real load (lambda* 0.5). */
	const FRow Rows[] = {
		{ 1, EOracleOutcome::Stands },
		{ 3, EOracleOutcome::Stands },
		{ 6, EOracleOutcome::Stands },
		{ 12, EOracleOutcome::Falls },
	};

	for (const FRow& Row : Rows)
	{
		const int32 K = Row.SurchargeCount;
		const double ExpectedSplit = SplitSurchargeLambda(K);
		const double ExpectedUniform = UniformGravityLambda(K);

		const FOracleResult Split = SolveRigidBlock(BuildLever(K, /*bSplit*/ true));
		const FOracleResult Uniform = SolveRigidBlock(BuildLever(K, /*bSplit*/ false));

		AddInfo(FString::Printf(
			TEXT("k=%d: split lambda* %.12g (expected %.12g), uniform lambda* %.12g (expected %.12g)"),
			K, Split.Lambda, ExpectedSplit, Uniform.Lambda, ExpectedUniform));

		/* Both poses must answer at all; a refusal is not the failure this test is about. */
		TestTrue(*FString::Printf(TEXT("k=%d: split pose must answer (said: %s)"), K, *Split.WhyNot),
			Split.bAnswered);
		TestTrue(*FString::Printf(TEXT("k=%d: uniform pose must answer (said: %s)"), K, *Uniform.WhyNot),
			Uniform.bAnswered);

		TestTrue(*FString::Printf(TEXT("k=%d: split lambda* must be finite"), K),
			FMath::IsFinite(Split.Lambda));

		/*
		 * THE BITING ASSERTION: the per-block split must yield the finite live-surcharge margin
		 * lambda* = 6/k. Today the solver ignores bLiveGravity, so under the split pose it reads
		 * LambdaCap (k <= 6) or 0 (k > 6) instead.
		 */
		const double SplitTol = 1.0e-6 * FMath::Max(1.0, ExpectedSplit);
		TestTrue(
			*FString::Printf(
				TEXT("k=%d: SPLIT lambda* must be %.12g (h*Mh/(k*Ms*(Lx-h))) and was %.12g — the ")
				TEXT("per-block surcharge posed live, header dead"),
				K, ExpectedSplit, Split.Lambda),
			FMath::Abs(Split.Lambda - ExpectedSplit) <= SplitTol);

		TestTrue(
			*FString::Printf(
				TEXT("k=%d: SPLIT outcome must be %d and was %d (lambda* %.12g)"),
				K, int32(Row.SplitOutcome), int32(OutcomeOf(Split)), Split.Lambda),
			OutcomeOf(Split) == Row.SplitOutcome);

		/*
		 * THE CONTRAST, correct today: the uniform pose cannot see the surcharge scale, so it
		 * reads the cap (or 0). Pinned so the split's value is measured against the number it must
		 * DIFFER from — the wall-15/16 "identical today, must separate" in miniature.
		 */
		const double UniformTol = 1.0e-6 * FMath::Max(1.0, ExpectedUniform);
		TestTrue(
			*FString::Printf(
				TEXT("k=%d: UNIFORM lambda* must be %.12g and was %.12g"),
				K, ExpectedUniform, Uniform.Lambda),
			FMath::Abs(Uniform.Lambda - ExpectedUniform) <= UniformTol);

		/* The whole point of the slice: the two poses must not read one number. */
		TestTrue(
			*FString::Printf(
				TEXT("k=%d: the split and uniform poses MUST separate (split %.12g vs uniform %.12g)"),
				K, Split.Lambda, Uniform.Lambda),
			Split.Lambda != Uniform.Lambda);

		/* Determinism: the same split problem twice, bit-identical lambda* and pivot path. */
		const FOracleResult SplitAgain = SolveRigidBlock(BuildLever(K, /*bSplit*/ true));
		TestTrue(
			*FString::Printf(
				TEXT("k=%d: DETERMINISM — two split solves must agree to the last bit (%.17g vs %.17g)"),
				K, Split.Lambda, SplitAgain.Lambda),
			Split.Lambda == SplitAgain.Lambda
				&& Split.SimplexIterations == SplitAgain.SimplexIterations);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
