// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * With a surcharge's weight posed live (FOracleBlock::bLiveGravity) and the rest dead, lambda* is
 * the finite "how many times the surcharge before it hinges" margin, distinct from the LambdaCap
 * the uniform-live pose reads. The global switch alone cannot separate surcharge demand from the
 * pre-compression that steadies it, which is why wall-15 and wall-16 read one identical lambda*
 * under uniform gravity (PROMOTION_DESIGN §10, SHED_PATH Phase A 6d).
 *
 * Fixture: a hand-built lever with an exact lambda*.
 *   - A grounded seat and a header (mass Mh) on one dry bed joint J0 bearing +-h about x = 0,
 *     header centred. No tensile bond, and compression lifted to effectively infinite so crushing
 *     (lambda* ~2551 at k=3 otherwise) cannot govern instead of tipping.
 *   - k surcharge blocks (mass Ms) in a centred column at x = Lx on the header's tail.
 *
 * With the header dead and the surcharge live, the inboard contact lifts when
 *
 *     lambda * k*Ms * Lx  =  h * (Mh + lambda * k*Ms)      [ resultant reaches +h ]
 *  => lambda* = h * Mh / ( k * Ms * (Lx - h) )              [ the LIVE-SURCHARGE margin ]
 *
 * With h = 10, Mh = 12, Ms = 1, Lx = 30 that is 6 / k. Uniform-live, the resultant 30k/(12+k) does
 * not depend on lambda, so it reads LambdaCap for k <= 6 and 0 above. Each row asserts the two
 * poses differ.
 *
 * World-free. Determinism asserted (two solves, bit-identical).
 */
namespace DeadLiveSplitSurchargeSupport
{
	using namespace RigidBlockOracle;

	// The lever's dimensions and masses, chosen so lambda* = 6/k is a clean hand value.

	constexpr double HeaderMassKg = 12.0;
	constexpr double SurchargeMassKg = 1.0;

	constexpr double BearingHalfLengthCm = 10.0;   // h
	constexpr double SurchargeLeverCm = 30.0;       // Lx

	constexpr double BedAreaSqCm = 200.0;
	constexpr double SurchargeAreaSqCm = 100.0;

	/** Dry stone with compression made effectively infinite, so only tipping can govern. */
	FConnectionStrength TippingOnlyStrength()
	{
		FConnectionStrength S = DestructionProfiles::DryStone;
		S.CompressiveStrengthMPa = 1.0e9;
		return S;
	}

	/** Hand-derived live-surcharge margin, h*Mh / (k*Ms*(Lx - h)) = 6/k. */
	double SplitSurchargeLambda(int32 SurchargeCount)
	{
		return BearingHalfLengthCm * HeaderMassKg
			/ (double(SurchargeCount) * SurchargeMassKg * (SurchargeLeverCm - BearingHalfLengthCm));
	}

	/** Uniform-live answer: LambdaCap while the lambda-independent resultant is in the bearing, else 0. */
	double UniformGravityLambda(int32 SurchargeCount)
	{
		const double ResultantX = double(SurchargeCount) * SurchargeMassKg * SurchargeLeverCm
			/ (HeaderMassKg + double(SurchargeCount) * SurchargeMassKg);

		/* A resultant exactly at the edge still stands. */
		return ResultantX <= BearingHalfLengthCm ? LambdaCap : 0.0;
	}

	/**
	 * The lever. bSplit: header dead, surcharge live via per-block flags. Otherwise everything live
	 * via the global switch. Geometry is identical either way.
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

		// Centred column at x = Lx; its contacts carry pure compression, so only J0 governs.
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

		// Split: global gravity dead, so the per-block flags decide. Uniform: everything live.
		Problem.bGravityIsLive = !bSplit;

		return Problem;
	}
}

/** The per-block dead/live split gives the finite surcharge margin, distinct from the uniform pose's cap. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDeadLiveSplitSurchargeTest,
	"DestructionGame.Oracle.RigidBlock.DeadLiveSplit.LiveSurchargeHingesTheHeader",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDeadLiveSplitSurchargeTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace DeadLiveSplitSurchargeSupport;
	using namespace DestructionProfiles;

	// Fixture: the strength assumptions the hand statics rely on.
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

		TestTrue(*FString::Printf(TEXT("k=%d: split pose must answer (said: %s)"), K, *Split.WhyNot),
			Split.bAnswered);
		TestTrue(*FString::Printf(TEXT("k=%d: uniform pose must answer (said: %s)"), K, *Uniform.WhyNot),
			Uniform.bAnswered);

		TestTrue(*FString::Printf(TEXT("k=%d: split lambda* must be finite"), K),
			FMath::IsFinite(Split.Lambda));

		// The split must give the finite margin 6/k; ignoring bLiveGravity would read the cap or 0.
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

		// The contrast: the uniform pose reads the cap or 0.
		const double UniformTol = 1.0e-6 * FMath::Max(1.0, ExpectedUniform);
		TestTrue(
			*FString::Printf(
				TEXT("k=%d: UNIFORM lambda* must be %.12g and was %.12g"),
				K, ExpectedUniform, Uniform.Lambda),
			FMath::Abs(Uniform.Lambda - ExpectedUniform) <= UniformTol);

		TestTrue(
			*FString::Printf(
				TEXT("k=%d: the split and uniform poses MUST separate (split %.12g vs uniform %.12g)"),
				K, Split.Lambda, Uniform.Lambda),
			Split.Lambda != Uniform.Lambda);

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
