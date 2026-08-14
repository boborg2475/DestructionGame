// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Tests/CorbelCaseTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE INTERIM OVERTURNING GUARD MUST RESTORE A BEARING AT THE PROFILE'S OWN MEAN BOND, AND THIS IS
 * THE ONE FIXTURE THAT CAN TELL 0.6 FROM 0.70.
 *
 * WHY IT EXISTS. `FStructure::BreakOverturnedBodies` (DESIGN.md §7 evolution step 2) prices a free
 * body's overturning against the bond its bearing rectangle can restore, and it now does that
 * against the GP mortar profile's own `TensileStrengthMPa` — which this test is what drove.
 * Until 2026-08-14 the guard carried a SECOND COPY of that strength, a guard-local 0.6 literal,
 * and the duplication was forced: the profile carried the CHARACTERISTIC 0.10, a 5%-fractile
 * design floor no overturning verdict could honestly be ruled at, and the guard's own comment
 * records that reading it would have condemned corbel A and broken a standing user ruling. Since
 * the mean-strength re-anchor the profile's figure IS a measured mean, 0.70, so the guard reading
 * it is the recorded intended end state and the second copy is exactly the drift the profile
 * library exists to end. **Everything below is written against the two candidate bases** — the
 * landed 0.70 and the retired 0.6 — because the contrast is what the assertions turn on; where a
 * ratio is quoted "at the guard's 0.6" it means the value the retired literal would have given.
 *
 * WHY IT IS A TEST AND NOT A ONE-CHARACTER EDIT. Moving 0.6 to 0.70 moves every overturning verdict
 * by 16.7%, and until this file existed NOTHING IN THE SUITE COULD TELL THE TWO VALUES APART: the
 * leaning-stack ladder brackets the guard 18x wide and holds at both ends, and the specified 8/11
 * interim-guard corbel pair answers identically at both by construction. Under the project's
 * non-waivable TDD rule that makes the swap production logic with no failing test behind it. This
 * is that test, and it is deliberately built ON the flapping rung the other fixtures avoid, because
 * here the flap IS the discrimination.
 *
 * =====================================================================================
 * THE FIXTURE, AND WHY A BARE ARM RATHER THAN ANYTHING ELSE
 * =====================================================================================
 *
 * `DestructionCorbel::Build` with `bFilled` false lays corbel case A: an immovable two-cell base and
 * then ONE brick per course, each stepping half a cell (11.25 cm) further out than the one below.
 * Three properties make it the only shape that can ask this question cleanly:
 *
 *   ONE BEARING, AND THE GUARD'S FLOOD PROVES IT. Every arm brick touches only the brick below it,
 *   and the lowest one touches only the base's outermost brick, over a 10.25 x 10.25 patch. So the
 *   guard's "maximal bonded body with exactly one bearing" walk closes over the whole arm — there
 *   is no second load path around the root joint and no other joint in the structure it can fire
 *   on. A filled corbel aborts that walk within a few hops, which is exactly why the whole corbel
 *   family is untouched by the guard and why the family is its regression suite.
 *
 *   THE ORDINARY JOINT CHECKS CANNOT DECIDE IT. The root bed joint reads 0.0223041 of capacity AT
 *   EVERY STEP COUNT — measured 5 through 13 and identical to the last bit — because the composite
 *   depth the solver credits grows with the arm exactly as fast as the moment does. (It is the same
 *   height-flat reading the leaning stack exposed, and the same reason the guard had to exist.) So
 *   nothing but the guard can bring either rung down, and the assertion below that says so is not
 *   decoration: without it a rung might fall for a reason this test is not about.
 *
 *   THE DEMAND IS A CLEAN QUADRATIC IN THE STEP COUNT, so rungs can be chosen rather than hunted.
 *
 * =====================================================================================
 * THE ARITHMETIC, DERIVED HERE AND NOWHERE ELSE
 * =====================================================================================
 *
 * A brick is 1.9 g/cm3 x 21.5 x 10.25 x 6.5 cm, so 2.72163125 kg and 2667.198625 uu of weight
 * (mass x 980; DESIGN.md §3's 1 N = 100 uu is already inside that, and applying it again is the
 * 100x error). The arm's m bricks sit at 11.25 cm centres, so their centroid is 5.625(m-1) past the
 * lowest brick's centre; the root joint's rectangle is centred half a step INBOARD of that brick,
 * at -5.625, and reaches 5.125 cm to its own edge. The lever about that edge is therefore
 *
 *     lever(m) = 5.625 m - 5.125 cm            (5.625 = half a step, 5.125 = half the overlap)
 *     overturning(m) = m x 2667.198625 x lever(m)          uu.cm
 *
 * and what restores it is the bearing rectangle at the guard's bond, as the elastic section modulus
 * about that same edge:
 *
 *     W = (4/3) x 5.125 x 5.125^2 = 179.4817708 cm3
 *     restoring(f) = f x 10000 uu per MPa.cm2 x W
 *
 * That gives the ladder, and the whole point of this test is in two of its rows:
 *
 *      steps   overturning uu.cm    ratio at 0.6     ratio at 0.70
 *          8       850,836.36          0.7901           0.6772
 *          9     1,092,217.84          1.0142           0.8693   <- FIRES at 0.6, STANDS at 0.70
 *         10     1,363,605.30          1.2662           1.0854   <- FIRES at both
 *         11     1,664,998.74          1.5461           1.3252
 *
 * A LOWER BOND RESTORES LESS, so the ratio rises as the bond falls and the guard fires MORE
 * readily at 0.6 than at 0.70 — which is the opposite of what CURRENT_STATE's spec for this test
 * assumed when it nominated the 10-step arm. The 10-step arm fires at BOTH values and
 * discriminates nothing; NINE is the rung whose verdict actually turns on the basis, and both
 * figures above are measured, not scaled.
 *
 * =====================================================================================
 * THE TWO RUNGS, AND WHAT THEY PIN TOGETHER
 * =====================================================================================
 *
 *   NINE STEPS MUST STAND. Ratio 0.8693 at the profile's 0.70: the bond restores 15% more than the
 *   arm asks for. RED TODAY — the guard's own 0.6 reads 1.0142 and severs the bearing, and the arm
 *   comes down whole. This is the discriminator, and it goes green when the guard reads the profile.
 *
 *   TEN STEPS MUST FALL. Ratio 1.0854 at 0.70: the arm asks 8.5% more than the bond can restore.
 *   GREEN TODAY, and green at both bases. It is here because a lower bound alone is half a pin —
 *   "nine stands" is satisfied by ANY bond above 0.6085, including an absurd one, and the pair
 *   together brackets the guard's effective bond into
 *
 *       0.60854 MPa  <  f  <  0.75975 MPa
 *
 *   which contains the profile's 0.70 and excludes the retired 0.6 literal. That bracket IS the
 *   assertion; neither row alone makes it.
 *
 * HOW SENSITIVE THIS IS, STATED HONESTLY RATHER THAN DISCOVERED LATER. The bracket is 25% wide and
 * the two bases are 16.7% apart, so ANY rung that discriminates them is by construction close to a
 * line — there is no version of this test with comfortable margins on both sides. At the profile's
 * 0.70 the margins are 13% (nine stands) and 8.5% (ten falls), and 8.5% is the tighter one: a
 * future re-anchor that raised the mean flexural bond above 0.75975 MPa would stop the ten-step arm
 * falling, and one that lowered it below 0.60854 would start the nine-step arm falling. Both are
 * inside the honest 0.6-1.0 mean bracket for clay in general-purpose mortar, so this is a REAL
 * possibility and not a theoretical one. WHAT TO DO IF IT HAPPENS: re-derive the rungs from the
 * ladder above at the new strength — the demand is a known quadratic and the crossing moves
 * predictably — and never nudge either verdict. If no integer step count lands inside the new
 * bracket, the fixture wants `FCorbelSpec::Scale`, which multiplies every length and therefore
 * moves the ratio LINEARLY (weight goes as s^3, lever as s, modulus as s^3).
 *
 * WHAT THIS TEST IS NOT. It is not a claim that the guard's free-body rule is right — that rule is
 * interim by design and dies at evolution step 4, taking this file with it. It is a claim about
 * ONE constant: that the strength a bearing is restored at is the one in the profile, and not a
 * second copy of a number that happens to be near it.
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on because weight is mass x 980, everything is connected,
 * and every assertion is on solver state. Displacement is never read.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 *
 * NOTHING IS IMPORTED FROM THE CODE UNDER TEST EXCEPT THE PRODUCER (`DestructionCorbel::Build`,
 * through the shared corbel fixture header) and the mortar profile, whose one figure this test
 * turns on is asserted as a fixture precondition. The statics, the section modulus and the unit
 * conversion are derived here, so a wrong production constant DISAGREES with this file rather than
 * agreeing with it.
 */
namespace OverturningGuardBasisTestSupport
{
	using namespace CorbelCaseTestSupport;

	/** Half a step out and half the overlap back: the lever about the bearing's own edge, cm. */
	inline double GuardLeverCm(const FCorbelSpec& Spec, int32 Steps)
	{
		const double HalfStepCm = CorbelStepOf(Spec) / 2.0;
		const double HalfOverlapCm =
			(CorbelBrickLengthOf(Spec) - CorbelStepOf(Spec)) / 2.0;

		return Steps * HalfStepCm - HalfOverlapCm;
	}

	/** The bearing rectangle's elastic section modulus about the edge the arm tips over, cm3. */
	inline double GuardBearingModulusCm3(const FCorbelSpec& Spec)
	{
		const double HalfOverlapCm =
			(CorbelBrickLengthOf(Spec) - CorbelStepOf(Spec)) / 2.0;

		/* The across-axis half-extent is half the brick's width: the joint is the full depth. */
		return CorbelSectionModulusCm3(CorbelBrickWidthOf(Spec) / 2.0, HalfOverlapCm);
	}

	/** What the whole arm asks of its one bearing, uu.cm. */
	inline double GuardOverturningUuCm(const FCorbelSpec& Spec, int32 Steps)
	{
		return Steps * CorbelBrickWeightUu(Spec) * GuardLeverCm(Spec, Steps);
	}

	/** Overturning over restoring at a given bond: at or under 1.0 the bearing holds. */
	inline double GuardRatioAt(const FCorbelSpec& Spec, int32 Steps, double BondMPa)
	{
		return GuardOverturningUuCm(Spec, Steps)
			/ (BondMPa * CorbelForceUnitsPerMPaPerSqCm * GuardBearingModulusCm3(Spec));
	}

	/** The bare arm: one brick per course off the standard two-cell immovable base. */
	inline FCorbelSpec BareArm(int32 Steps)
	{
		FCorbelSpec Spec;
		Spec.BaseCells = 2;
		Spec.LeftOriginCm = 0.0;
		Spec.Steps = Steps;
		Spec.bFilled = false;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		return Spec;
	}

	/** How many of the arm's bricks the solver declined to route round a loop. */
	inline int32 StrandedArmPieces(const FCorbelStructure& Built)
	{
		int32 Stranded = 0;

		for (const int32 Piece : Built.ArmPieces)
		{
			if (!Built.Structure.IsPieceRemoved(Piece)
				&& Built.Structure.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}

		return Stranded;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOverturningGuardBasisTest,
	"DestructionGame.Core.Structure.TheOverturningGuardRestoresAtTheProfilesMeanBond",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOverturningGuardBasisTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace OverturningGuardBasisTestSupport;

	/**
	 * THE BOND THE GUARD MUST RESTORE AT, ASSERTED AGAINST THE PROFILE RATHER THAN READ OFF IT.
	 *
	 * This is the whole subject of the test, so it is written out and then checked: if the profile
	 * moves, this row fails and the rungs below are re-derived from the header's ladder at the new
	 * value. A test that read `GeneralPurposeMortar.TensileStrengthMPa` here would follow a retune
	 * silently and assert nothing about which number the guard uses.
	 */
	constexpr double ProfileMeanBondMPa = 0.70;

	TestEqual(
		TEXT("FIXTURE: general purpose mortar's mean flexural bond is 0.70 MPa — the figure the ")
		TEXT("guard must restore a bearing at, and the one the rungs below are derived from"),
		GeneralPurposeMortar.TensileStrengthMPa, ProfileMeanBondMPa);

	/* --- the two rungs, derived --------------------------------------------------------------- */

	constexpr int32 StandingSteps = 9;
	constexpr int32 FallingSteps = 10;

	const FCorbelSpec StandingSpec = BareArm(StandingSteps);
	const FCorbelSpec FallingSpec = BareArm(FallingSteps);

	const double StandingRatio = GuardRatioAt(StandingSpec, StandingSteps, ProfileMeanBondMPa);
	const double FallingRatio = GuardRatioAt(FallingSpec, FallingSteps, ProfileMeanBondMPa);

	/*
	 * THE FIXTURE IS THE FIXTURE IT SAYS IT IS, CHECKED BEFORE ANY VERDICT IS READ. Both rungs are
	 * chosen to straddle 1.0 at the profile's bond; a step count edited without re-deriving would
	 * otherwise leave two rows quietly asserting the same side of the line.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the %d-step arm must be UNDER its bearing's capacity at %.8g MPa — it ")
			TEXT("asks %.8g uu.cm against %.8g restored, a ratio of %.8g"),
			StandingSteps, ProfileMeanBondMPa,
			GuardOverturningUuCm(StandingSpec, StandingSteps),
			ProfileMeanBondMPa * CorbelForceUnitsPerMPaPerSqCm
				* GuardBearingModulusCm3(StandingSpec),
			StandingRatio),
		StandingRatio < 1.0);

	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the %d-step arm must be OVER it at the same bond — it asks %.8g uu.cm ")
			TEXT("against %.8g restored, a ratio of %.8g"),
			FallingSteps,
			GuardOverturningUuCm(FallingSpec, FallingSteps),
			ProfileMeanBondMPa * CorbelForceUnitsPerMPaPerSqCm
				* GuardBearingModulusCm3(FallingSpec),
			FallingRatio),
		FallingRatio > 1.0);

	/*
	 * AND THE BRACKET THE PAIR PINS THE GUARD'S BOND INTO IS PRINTED, because that is what the two
	 * verdicts below jointly assert and it is the number a reader wants when one of them fails.
	 */
	const double BracketLowMPa =
		GuardOverturningUuCm(StandingSpec, StandingSteps)
		/ (CorbelForceUnitsPerMPaPerSqCm * GuardBearingModulusCm3(StandingSpec));

	const double BracketHighMPa =
		GuardOverturningUuCm(FallingSpec, FallingSteps)
		/ (CorbelForceUnitsPerMPaPerSqCm * GuardBearingModulusCm3(FallingSpec));

	AddInfo(FString::Printf(
		TEXT("the two rungs bracket the guard's effective bond into (%.8g, %.8g) MPa: the profile's ")
		TEXT("%.8g is inside it and the guard's own 0.6 literal is below it"),
		BracketLowMPa, BracketHighMPa, ProfileMeanBondMPa));

	/* --- what each rung does ------------------------------------------------------------------ */

	struct FRung
	{
		int32 Steps;
		bool bMustStand;
	};

	const FRung Rungs[] = { { StandingSteps, true }, { FallingSteps, false } };

	double RootReading[UE_ARRAY_COUNT(Rungs)] = {};
	bool bRootRead[UE_ARRAY_COUNT(Rungs)] = {};

	for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(Rungs)); ++Index)
	{
		const FRung& Rung = Rungs[Index];

		const FCorbelSpec Spec = BareArm(Rung.Steps);

		FCorbelStructure Built;

		if (!TestTrue(
				*FString::Printf(TEXT("FIXTURE: the %d-step bare arm must lay"), Rung.Steps),
				CorbelBuild(Spec, Built)))
		{
			continue;
		}

		/*
		 * ONE BRICK PER COURSE, WHICH IS THE WHOLE OF THE BARE ARM AND THE WHOLE OF WHY THE GUARD
		 * CAN SEE THIS BODY AT ALL. A filled course would give the arm a second path around its
		 * root and the guard would stand aside, leaving both rungs standing for a reason that has
		 * nothing to do with the bond.
		 */
		TestEqual(
			*FString::Printf(
				TEXT("FIXTURE: the %d-step arm must be %d bricks, one per course"),
				Rung.Steps, Rung.Steps),
			Built.ArmPieces.Num(), Rung.Steps);

		/*
		 * AND THE JOINT CHECKS MUST NOT BE ABLE TO DECIDE THIS. The root bed joint reads a few
		 * percent of capacity and — the part that matters — reads the SAME few percent at both
		 * rungs, because the composite depth grows with the arm as fast as the moment does. So
		 * whatever separates the two verdicts below, it is not a joint over capacity.
		 */
		Built.Structure.SolveLoads();

		int32 WorstJoint = INDEX_NONE;
		const double WorstBefore = CorbelWorstUtilisation(Built, WorstJoint);

		RootReading[Index] = Built.Structure.GetConnectionUtilisation(Built.RootJoint);
		bRootRead[Index] = true;

		TestTrue(
			*FString::Printf(
				TEXT("%d steps: NO JOINT MAY BE OVER CAPACITY, or the verdict below is a strength ")
				TEXT("verdict and this test is not measuring the guard at all — the worst joint in ")
				TEXT("the structure reads %.17g and the root bearing reads %.17g"),
				Rung.Steps, WorstBefore, RootReading[Index]),
			WorstBefore < 1.0);

		const int32 Passes = Built.Structure.SolveAndBreak();

		const int32 Lost = CorbelArmPiecesWithNoPath(Built);
		const int32 Stranded = StrandedArmPieces(Built);

		AddInfo(FString::Printf(
			TEXT("%d steps: ratio %.8g at %.8g MPa / %.8g at the retired 0.6 — %d pass(es), %d of ")
			TEXT("%d arm brick(s) lost their path, %d stranded"),
			Rung.Steps, GuardRatioAt(Spec, Rung.Steps, ProfileMeanBondMPa), ProfileMeanBondMPa,
			GuardRatioAt(Spec, Rung.Steps, 0.6), Passes, Lost, Built.ArmPieces.Num(), Stranded));

		/*
		 * NOTHING MAY BE Stranded ON EITHER RUNG, per DESIGN.md §4: a piece the solver declined to
		 * route round a loop is not a piece the bearing failed to hold, and either verdict reached
		 * that way would be a statement about the router.
		 */
		TestEqual(
			*FString::Printf(
				TEXT("%d steps: no arm brick may be Stranded — a solver limitation must not be able ")
				TEXT("to wear either verdict's clothes"),
				Rung.Steps),
			Stranded, 0);

		if (Rung.bMustStand)
		{
			/*
			 * THE DISCRIMINATOR, AND THE RED. At the profile's mean bond this arm's bearing
			 * restores 15% more than the arm asks of it, so nothing breaks and every brick keeps
			 * its path to the earth. At the guard's own 0.6 the same arithmetic reads 1.0142, the
			 * guard severs the bearing and the whole arm comes down — which is what fails here
			 * today, and the only thing in the suite that can tell the two bonds apart.
			 */
			TestEqual(
				*FString::Printf(
					TEXT("%d steps: THE ARM MUST STAND — its bearing restores %.8g MPa of bond ")
					TEXT("against the %.8g MPa the arm asks for, a ratio of %.8g at the profile's ")
					TEXT("mean. %d of %d brick(s) lost their path to the earth in %d cascade ")
					TEXT("pass(es). A guard restoring at less than %.8g MPa condemns this arm, and ")
					TEXT("that is the whole discrimination"),
					Rung.Steps, ProfileMeanBondMPa, BracketLowMPa, StandingRatio,
					Lost, Built.ArmPieces.Num(), Passes, BracketLowMPa),
				Lost, 0);

			TestEqual(
				*FString::Printf(
					TEXT("%d steps: and no joint may have given — the bearing holds, so the cascade ")
					TEXT("has nothing to break; it ran %d pass(es)"),
					Rung.Steps, Passes),
				Passes, 0);
		}
		else
		{
			/*
			 * THE UPPER BRACKET. Green today and green after the swap: this rung asks 8.5% more
			 * than the profile's bond restores, so it must come down at either value. Without it
			 * "nine stands" is satisfiable by any bond at all above 0.6085, and the pair would pin
			 * the guard's constant from one side only.
			 */
			TestEqual(
				*FString::Printf(
					TEXT("%d steps: THE ARM MUST COME DOWN — it asks %.8g MPa of its bearing where ")
					TEXT("the profile restores %.8g, a ratio of %.8g, and %d of %d brick(s) lost ")
					TEXT("their path in %d cascade pass(es). This rung is what stops the row above ")
					TEXT("being satisfied by a guard that never fires"),
					Rung.Steps, BracketHighMPa, ProfileMeanBondMPa, FallingRatio,
					Lost, Built.ArmPieces.Num(), Passes),
				Lost, Built.ArmPieces.Num());
		}
	}

	/*
	 * AND THE ROOT READING IS THE SAME AT BOTH HEIGHTS, asserted rather than described.
	 *
	 * This is the height-flat joint reading the leaning stack exposed, standing here as the proof
	 * that the two rungs' verdicts CANNOT have come from the joint checks: they read the same
	 * number and answer differently. If a future slice gives the joint checks a height term, this
	 * fails and the test above has to be re-argued rather than re-tuned.
	 */
	if (bRootRead[0] && bRootRead[1])
	{
		TestTrue(
			*FString::Printf(
				TEXT("THE JOINT CHECKS ARE HEIGHT-BLIND HERE: the %d-step arm's root bearing reads ")
				TEXT("%.17g and the %d-step arm's reads %.17g. They are equal, so the two verdicts ")
				TEXT("above are the guard's and nothing else's"),
				StandingSteps, RootReading[0], FallingSteps, RootReading[1]),
			FMath::IsNearlyEqual(RootReading[0], RootReading[1], 1.0e-12));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
