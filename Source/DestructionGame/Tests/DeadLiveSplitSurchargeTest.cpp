// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * With a SURCHARGE's own weight posed live and the rest of a structure's self-weight
 * posed dead, lambda* becomes the finite "how many times the surcharge before it hinges"
 * margin, distinct from the LambdaCap the same fixture reads when every block's gravity
 * is uniform-live.
 *
 * The oracle has a global dead/live switch (FOracleProblem::bGravityIsLive) and a
 * per-force one (FOracleAppliedForce::bLive), but no way to mark a subset of a
 * structure's self-weight live while the rest stays dead. The global switch scales every
 * block's weight by the same lambda, so it cannot tell a surcharge's demand apart from
 * the pre-compression that steadies it — which is why wall-15 and wall-16 (running bond,
 * 10 courses, differing only in which course projects) read one identical
 * lambda* = 868.623736 under uniform gravity while production separates them ~31.6x at
 * the header's own bed joint (RigidBlockOracleSweepTest.cpp pins that identity + ratio).
 * PROMOTION_DESIGN §10/§472 and SHED_PATH Phase A 6d name the fix: make the split
 * load-bearing per block — the prerequisite for the shed's roof-as-surcharge and step 7's
 * live impulses.
 *
 * The fixture is a hand-derived lever, not a wall: the wall-15/16 separation needs a
 * production posing path this slice's dev work adds, so the executable red isolates the
 * mechanism on a tiny hand-built FOracleProblem whose lambda* is exact:
 *
 *   - A grounded seat (block 0) and a header (block 1, mass Mh) resting on it through one
 *     dry bed joint J0 whose contact bearing spans +-h about x = 0. The header centroid
 *     sits dead-centre in the bearing, so its own weight alone overturns nothing. The
 *     joints carry no tensile bond and an effectively infinite compressive strength, so
 *     the only governing constraint is the no-tension bound. Compression is lifted out of
 *     the way on purpose — otherwise 30 MPa crushing at the outboard contact would govern
 *     the uniform pose instead (lambda* ~2551 at k=3, measured) — so the axis under test
 *     is the one that actually governs.
 *   - A surcharge of k identical blocks (mass Ms each) stacked in a centred vertical
 *     column at a lever x = Lx, bearing on the header's tail through a dry joint. The
 *     column's internal joints carry pure centred compression and never govern; its whole
 *     weight k*Ms lands on the header at x = Lx.
 *
 * The hand statics (dry stone, f_t = 0, pure no-tension tipping): the header bed joint J0
 * governs, and everything above it must keep its resultant inside the bearing [-h, +h].
 * With the header weight dead (fixed, at x = 0) and the surcharge weight live (scaled by
 * lambda, at x = Lx), the inboard contact lifts when
 *
 *     lambda * k*Ms * Lx  =  h * (Mh + lambda * k*Ms)      [ resultant reaches +h ]
 *  => lambda* = h * Mh / ( k * Ms * (Lx - h) )              [ the LIVE-SURCHARGE margin ]
 *
 * With h = 10, Mh = 12, Ms = 1, Lx = 30 this is 120 / (20 k) = 6 / k. Contrast the uniform
 * pose (every block live): the resultant is x = k*Ms*Lx / (Mh + k*Ms) = 30k/(12+k),
 * independent of lambda, so the fixture stands at every multiplier (lambda* = LambdaCap,
 * k <= 6) or none (lambda* = 0, k > 6). The split and uniform pose therefore separate —
 * finite margin versus cap — exactly as wall-15 must separate from wall-16.
 *
 * Why it is red: FOracleBlock::bLiveGravity is a compile seam declared for this test; the
 * solver's assembly still branches only on the global bGravityIsLive (RigidBlockOracle.cpp
 * ~line 1548), so the per-block designation is ignored. Under the split pose production
 * therefore treats all gravity as dead — LambdaCap for k <= 6, 0 for k > 6 — never the
 * finite 6/k the split demands. The uniform-pose assertions are correct today and green on
 * arrival; they are the contrast that proves the split, and each row asserts the two poses
 * must differ so a solver that collapsed them back to one number cannot pass.
 *
 * No ticking world needed: pure arithmetic on a hand-built FOracleProblem, like the oracle
 * it validates. Determinism is asserted (two solves, bit-identical) per the oracle's
 * contract. Named namespace: a unity build merges many files into one translation unit.
 */
namespace DeadLiveSplitSurchargeSupport
{
	using namespace RigidBlockOracle;

	// The lever's dimensions and masses, chosen so lambda* = 6/k is a clean hand value.

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

		/* Centred vertical column at x = Lx, each block resting on the one below (or the
		 * header, for the first); centred contacts carry pure compression, so only J0 governs. */
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
		 * The pose. Split: global gravity dead so the per-block bLiveGravity flags above
		 * decide; uniform: global gravity live and the per-block flags are irrelevant (the
		 * solver reads only the global switch either way today — the point of the red).
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

	/*
	 * Fixture preconditions: the strength the hand statics lean on — no tensile bond (pure
	 * no-tension tipping) and compression lifted out of the way (so crushing never
	 * hijacks the governing axis) — asserted rather than trusted.
	 */
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
		 * The biting assertion: the per-block split must yield the finite live-surcharge
		 * margin lambda* = 6/k. Today the solver ignores bLiveGravity, so under the split
		 * pose it reads LambdaCap (k <= 6) or 0 (k > 6) instead.
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
		 * The contrast, correct today: the uniform pose cannot see the surcharge scale, so
		 * it reads the cap (or 0) — the wall-15/16 "identical today, must separate" in
		 * miniature.
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
