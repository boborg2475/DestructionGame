// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 6b FOLLOW-ON RED (i) — FIRST-CRACK ROWS IN THE MIN-VIOLATION STRAIN READOUT.
 *
 * The below-cap break authority uses FIRST CRACK: FStructure::BreakByEquilibrium sets
 * Problem.bFirstCrackRows = true (Structure.cpp), so a BONDED joint (f_t > 0) cracks at its
 * uncracked peak-fibre limit -(n1+n2) + 3|n1-n2| <= f_t*A — roughly THREE TIMES stricter in
 * bending than the plastic rectangular-stress-block limit the no-tension form allows. But the
 * strain READOUT the overlay reads is sourced by a SEPARATE min-violation solve, and that solve
 * uses the PLASTIC capacity, for two independent reasons that BOTH have to be fixed:
 *
 *   (1) RigidBlockOracle::SolveMinViolationReadout IGNORES bFirstCrackRows entirely — it assembles
 *       the tension / friction / crushing / ceiling rows but never the first-crack pair (6a
 *       residue (c)). So even asked for first-crack rows, the readout LP has none.
 *   (2) FStructure::CacheMinViolationReadout explicitly sets ReadoutProblem.bFirstCrackRows = false
 *       on the readout copy (Structure.cpp), so the gate's own first-crack flag never reaches the
 *       readout pose.
 *
 * CONSEQUENCE, and it is a lie the player would see: a bonded joint carrying tension-in-bending
 * BREAKS at its first-crack limit but the overlay reads its PLASTIC utilisation — a joint that is
 * genuinely at ~0.6 of its cracking capacity reads ~0.15, four times too comfortable. The ruling
 * (design decision 1, already made): the readout MUST assemble first-crack rows below the cap so
 * its utilisation is CONSISTENT with what actually cracks-and-breaks.
 *
 * WHY THIS IS THE FIRST TEST TO CATCH IT. Every 6a readout fixture and the 6b core hang are
 * M ~ 0 (pure tension or pure crushing), and at M = 0 the first-crack limit and the plastic limit
 * COINCIDE (-(n1+n2) with n1 = n2 is just -2n = -N, and 3|n1-n2| = 0), so first-crack has been a
 * no-op everywhere so far. THIS fixture forces M != 0 on a bonded joint, where the uncracked
 * peak-fibre limit and the plastic limit measurably diverge (a factor of four here), so the
 * readout's capacity model becomes observable for the first time.
 *
 * THE FIXTURE — A BONDED CORBEL (a short cantilever held by its bond), as a production FStructure.
 * A free brick bears on a narrow grounded pier through one bed joint and OVERHANGS the pier by
 * three joint-half-lengths, so gravity drives a real bending moment through the bond. The bond has
 * a finite TENSION strength and every other axis uncapped (compression / cohesion uncapped,
 * friction zero — 6a's WeakTensionOnly, adapted), so the governing capacity is unambiguously the
 * bond's tension/first-crack and no crushing or friction row can quietly govern instead:
 *
 *   HAND STATICS (independent of the LP, single determinate joint, worked by hand here):
 *     centroid eccentricity   e = x_corbel - x_joint      (= 3h — the corbel overhangs by 3 h)
 *     N (compression positive) = +W                        (the corbel's weight bears down)
 *     |M|                      = W * e = 3 W h              (moment about the joint centre)
 *     |M| / h                  = 3 W                        (so the section is bending-dominated)
 *     tension contact (far edge) magnitude = (|M|/h - N)/2 = W      => reads NEGATIVE (tension)
 *
 *   PLASTIC per-contact tension row:  demand = W,   capacity = f_t*Conv*(A/2)
 *     => plastic utilisation = W / (f_t*Conv*A/2) = 2W / (f_t*Conv*A)
 *
 *   FIRST-CRACK (uncracked peak-fibre) row:  demand = -N + 3|M|/h = -W + 9W = 8W,  capacity = f_t*Conv*A
 *     => first-crack utilisation = 8W / (f_t*Conv*A)
 *
 *   The two DIFFER BY EXACTLY FOUR here: (3e/h - 1)/(e/h - 1) with e/h = 3 is 8/2 = 4. The bond is
 *   sized so BOTH sit BELOW 1 (first-crack ~0.60, plastic ~0.15), so the structure STANDS under the
 *   production first-crack gate and this red is VERDICT-FREE — it asserts the readout's capacity
 *   MODEL, not that the corbel falls. (Violation stays zero on both models; the utilisation is the
 *   sole discriminator, and that is enough to prove which rows are in the readout LP.)
 *
 * WHY IT PROVES FIRST-CRACK ROWS ARE IN THE READOUT. The cached utilisation on the bonded joint can
 * only read the first-crack ~0.60 if the readout LP assembled the first-crack rows AND the cache
 * asked for them; today it reads the plastic ~0.15. Asserting it equals first-crack AND differs
 * from plastic makes the two failures above the only way to be red.
 *
 * UNITS are derived here (1 MPa over 1 cm2 = 100 * 100 = 10000 uu), never imported, so a wrong
 * production constant disagrees rather than agrees.
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on the ordinary way (weight is MassKg * 980 inside
 * FStructure), everything is connected, and every assertion is on the oracle, the cached readout, or
 * solver support state — the same footing as the 6b core wiring test.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace FirstCrackReadoutSupport
{
	using namespace DestructionLayout;

	/* ================================================================================
	 * UNITS AND GEOMETRY, derived here so a wrong production constant fails rather than agrees.
	 * ================================================================================ */

	/** Fired clay, 1.9 g/cm3 — the same figure every wall fixture uses. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** The single wythe: every piece is this deep on Y, so every joint's Y overlap is full. */
	constexpr double WytheWidthCm = 10.25;

	/** A 1 cm mortar bed joint — the separation the pier and the corbel are formed across. */
	constexpr double JointThicknessCm = 1.0;

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. NOT imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/** Above the oracle's UncappedStrengthMPa (1e9): a strength row this large is not assembled. */
	constexpr double UncappedHere = 1.0e12;

	/* ---- The narrow grounded pier the corbel bears on. Its width IS the joint width. ---- */
	constexpr double PierCentreXCm = 0.0;
	constexpr double PierSizeXCm = 10.0;   /* the bearing width => joint length 2h = 10, h = 5 */
	constexpr double PierCentreZCm = 10.0;
	constexpr double PierSizeZCm = 20.0;   /* top face at Z = 20 */

	/* ---- The overhanging free corbel: 40 cm long, its centroid 15 cm past the joint centre. ---- */
	constexpr double CorbelCentreXCm = 15.0; /* e = 15 = 3h => bending-dominated */
	constexpr double CorbelSizeXCm = 40.0;
	constexpr double CorbelSizeZCm = 10.0;
	constexpr double CorbelCentreZCm = 26.0; /* bottom face at Z = 21 => a 1 cm bed joint above the pier */

	/**
	 * The bond, sized so BOTH capacity models sit below 1 (first-crack ~0.60, plastic ~0.15): the
	 * corbel STANDS under the production first-crack gate (so the readout stays additive, no verdict
	 * moves) while the two utilisations still diverge by a factor of four. f_t > 0 is what makes it a
	 * BONDED joint and keys the first-crack rows on.
	 */
	constexpr double TensileMPa = 0.1;

	FPieceBox MakeBox(double CentreX, double SizeX, double CentreZ, double SizeZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(SizeX, WytheWidthCm, SizeZ) * 0.5;
		Box.CentreCm = FVector(CentreX, 0.0, CentreZ);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/**
	 * A joint with a finite TENSION bond and every other axis uncapped, so the ONLY strength rows
	 * that can carry demand are the bond's tension row and (once assembled) its first-crack pair —
	 * which is what makes the corbel's governing capacity unambiguous. Mirrors 6a's WeakTensionOnly.
	 */
	FConnectionStrength BondTensionOnly()
	{
		FConnectionStrength S;
		S.TensileStrengthMPa = TensileMPa;       /* finite: the tension AND first-crack rows ARE written */
		S.CompressiveStrengthMPa = UncappedHere; /* no crushing row */
		S.ShearCohesionMPa = UncappedHere;       /* no friction row */
		S.FrictionCoefficient = 0.0;
		/* MaxShearStrengthMPa unbounded by default => no ceiling row. */
		return S;
	}

	struct FBondedCorbel
	{
		FStructure Structure;

		int32 Pier = INDEX_NONE;   /* grounded, narrow, below */
		int32 Corbel = INDEX_NONE; /* free, overhanging, above */

		int32 BedJoint = INDEX_NONE; /* the one joint: Pier below, Corbel above */
	};

	/**
	 * Lay the pier, the corbel and the one bed joint. The pier is narrower than the corbel and
	 * fully under it, so the joint is the pier's own 10 cm width (a padstone-style bearing); the
	 * corbel's centroid sits 15 cm past the joint centre, so its weight bends the bond.
	 */
	void Build(FBondedCorbel& Out)
	{
		const FPieceBox PierBox =
			MakeBox(PierCentreXCm, PierSizeXCm, PierCentreZCm, PierSizeZCm);
		const FPieceBox CorbelBox =
			MakeBox(CorbelCentreXCm, CorbelSizeXCm, CorbelCentreZCm, CorbelSizeZCm);

		Out.Pier = Out.Structure.AddPiece(BoxMassKg(PierBox), /*bIsGrounded*/ true, PierBox.CentreCm);
		Out.Corbel = Out.Structure.AddPiece(BoxMassKg(CorbelBox), /*bIsGrounded*/ false, CorbelBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Pier, PierBox, Out.Corbel, CorbelBox, JointThicknessCm, BondTensionOnly(), Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}
	}

	/* ================================================================================
	 * THE INDEPENDENT HAND ORACLE — rigid-body statics by hand, derived a DIFFERENT way than
	 * the LP. Area, weight, eccentricity and both capacity models are rebuilt from geometry here.
	 * ================================================================================ */

	double CorbelMassKg()
	{
		return ClayDensityGramsPerCubicCm * CorbelSizeXCm * WytheWidthCm * CorbelSizeZCm / 1000.0;
	}
	double CorbelWeightUu() { return CorbelMassKg() * GravityCmPerSecondSquared; }

	/** The bearing face: the pier's 10 cm width by the full wythe on Y. */
	double JointAreaSqCm() { return PierSizeXCm * WytheWidthCm; }

	/** Half the joint length, cm — the section's half-depth in bending. */
	double JointHalfLengthCm() { return PierSizeXCm / 2.0; }

	/** The corbel centroid's eccentricity from the joint centre (= overlap midpoint = pier centre). */
	double EccentricityCm() { return CorbelCentreXCm - PierCentreXCm; }

	/* N is COMPRESSION POSITIVE: the corbel bears down, so the resultant normal reads +W. */
	double ExpectedNormalUu() { return CorbelWeightUu(); }

	/* |M| = W * e about the joint centre. */
	double ExpectedMomentMagnitudeUu() { return CorbelWeightUu() * EccentricityCm(); }

	/* The bending fibre force |M|/h, the term that makes first-crack and plastic diverge. */
	double BendingFibreUu() { return ExpectedMomentMagnitudeUu() / JointHalfLengthCm(); }

	/* ---- Plastic model: the far contact's tension against the per-contact (A/2) tension cap. ---- */
	double PlasticTensionDemandUu() { return (BendingFibreUu() - ExpectedNormalUu()) / 2.0; }
	double PlasticTensionCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm() / 2.0; }
	double ExpectedPlasticUtilisation() { return PlasticTensionDemandUu() / PlasticTensionCapacityUu(); }

	/* ---- First-crack model: the uncracked peak-fibre resultant against the full-face cap. ---- */
	double FirstCrackDemandUu() { return -ExpectedNormalUu() + 3.0 * BendingFibreUu(); }
	double FirstCrackCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm(); }
	double ExpectedFirstCrackUtilisation() { return FirstCrackDemandUu() / FirstCrackCapacityUu(); }

	/* The corbel is within BOTH capacities => zero violation on either model (a verdict-free red). */
	double ExpectedViolationUu() { return 0.0; }

	bool Near(double A, double B, double Tol) { return FMath::Abs(A - B) <= Tol; }

	bool AnyJointBrokeUnderLoad(const FStructure& S)
	{
		for (int32 Joint = 0; Joint < S.NumConnections(); ++Joint)
		{
			if (S.GetBreakPass(Joint) != INDEX_NONE)
			{
				return true;
			}
		}
		return false;
	}
}

/**
 * Below the cap the cached strain readout uses the FIRST-CRACK capacity on a bonded bending joint,
 * not the plastic capacity — the same model the break authority cracks it at.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFirstCrackReadoutTest,
	"DestructionGame.Acceptance.StrainReadout.ReadoutUsesFirstCrackForBondedBendingBelowTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFirstCrackReadoutTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace FirstCrackReadoutSupport;

	/* ------------------------------------------------------------------ *
	 * BUILD, AND CHECK THE TOPOLOGY IS THE ONE CLAIMED: two pieces, one bed
	 * joint, complete geometry, the corbel overhanging the pier it bears on.
	 * ------------------------------------------------------------------ */

	FBondedCorbel Probe;
	Build(Probe);

	if (Probe.BedJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit the pier-to-corbel bed joint"));
		return false;
	}

	TestEqual(TEXT("FIXTURE: two pieces — a grounded pier and one overhanging corbel"),
		Probe.Structure.NumPieces(), 2);
	TestEqual(TEXT("FIXTURE: one joint — the bed joint the corbel bends across"),
		Probe.Structure.NumConnections(), 1);
	TestTrue(TEXT("FIXTURE: every piece and joint must know where it is, or there is no bridge"),
		Probe.Structure.HasCompleteGeometry());

	AddInfo(FString::Printf(
		TEXT("HAND ORACLE: W %.6g uu, area %.6g cm2, h %.6g cm, e %.6g cm, f_t %.6g MPa; "
			 "N %.6g, |M| %.6g; PLASTIC demand %.6g / cap %.6g => util %.6g; "
			 "FIRST-CRACK demand %.6g / cap %.6g => util %.6g (ratio %.6g)"),
		CorbelWeightUu(), JointAreaSqCm(), JointHalfLengthCm(), EccentricityCm(), TensileMPa,
		ExpectedNormalUu(), ExpectedMomentMagnitudeUu(),
		PlasticTensionDemandUu(), PlasticTensionCapacityUu(), ExpectedPlasticUtilisation(),
		FirstCrackDemandUu(), FirstCrackCapacityUu(), ExpectedFirstCrackUtilisation(),
		ExpectedFirstCrackUtilisation() / ExpectedPlasticUtilisation()));

	/* The two models MUST measurably diverge, or the fixture proves nothing about which is used. */
	TestTrue(
		*FString::Printf(TEXT("FIXTURE: first-crack util %.6g must exceed plastic util %.6g (M != 0 divergence)"),
			ExpectedFirstCrackUtilisation(), ExpectedPlasticUtilisation()),
		ExpectedFirstCrackUtilisation() - ExpectedPlasticUtilisation() > 0.1);

	/* ------------------------------------------------------------------ *
	 * CROSS-CHECK 1 — THE FIXTURE STANDS UNDER THE PRODUCTION FIRST-CRACK
	 * GATE. Posed exactly as FStructure::BreakByEquilibrium poses it below the
	 * cap (gravity dead, bFirstCrackRows on), the LP finds an admissible force
	 * system, so the gate stands the corbel and the readout is purely additive.
	 * ------------------------------------------------------------------ */

	FOracleProblem Feasibility;
	FString BridgeWhy;

	const bool bBridged = BuildRigidBlockProblem(Probe.Structure, Feasibility, BridgeWhy);
	TestTrue(*FString::Printf(TEXT("CROSS-CHECK: the oracle bridge must accept this 2D corbel (%s)"), *BridgeWhy),
		bBridged);

	if (!bBridged)
	{
		return false;
	}

	Feasibility.bGravityIsLive = false;
	Feasibility.bFirstCrackRows = true; /* exactly the production below-cap gate pose */
	const FOracleResult FeasResult = SolveRigidBlock(Feasibility);

	AddInfo(FString::Printf(
		TEXT("CROSS-CHECK: first-crack feasibility answered %d, lambda* %.10g — Stands means lambda* >= 1"),
		FeasResult.bAnswered ? 1 : 0, FeasResult.Lambda));

	TestTrue(TEXT("CROSS-CHECK: the oracle must ANSWER the first-crack feasibility pose"), FeasResult.bAnswered);
	TestEqual(TEXT("CROSS-CHECK: the first-crack LP STANDS the corbel (both models < 1) — additive readout"),
		static_cast<int32>(OutcomeOf(FeasResult)),
		static_cast<int32>(EOracleOutcome::Stands));

	/* Find the oracle joint the bridge mapped from the bed connection — the readout's key. */
	int32 OracleJointForBed = INDEX_NONE;
	for (int32 J = 0; J < Feasibility.ConnectionOfJoint.Num(); ++J)
	{
		if (Feasibility.ConnectionOfJoint[J] == Probe.BedJoint)
		{
			OracleJointForBed = J;
			break;
		}
	}
	TestTrue(TEXT("CROSS-CHECK: ConnectionOfJoint maps some oracle joint back to the bed connection"),
		OracleJointForBed != INDEX_NONE);

	/* ------------------------------------------------------------------ *
	 * CROSS-CHECK 2 [RED] — THE ORACLE-SIDE READOUT, ASKED FOR FIRST-CRACK,
	 * MUST USE IT. Pose the min-violation readout with bFirstCrackRows on (the
	 * flag production's cache will have to set). This isolates fix part (1):
	 * SolveMinViolationReadout ignoring bFirstCrackRows. RED today — the readout
	 * assembles no first-crack rows, so it reads the plastic utilisation.
	 * ------------------------------------------------------------------ */

	FOracleProblem ReadoutPose;
	FString ReadoutWhy;
	TestTrue(TEXT("CROSS-CHECK: the bridge accepts the same fixture for the readout pose"),
		BuildRigidBlockProblem(Probe.Structure, ReadoutPose, ReadoutWhy));
	ReadoutPose.bGravityIsLive = false;
	ReadoutPose.bMinViolationReadout = true;
	ReadoutPose.bFirstCrackRows = true; /* ASK for first crack, exactly as the fixed cache will */

	const FOracleResult OracleReadout = SolveRigidBlock(ReadoutPose);

	TestTrue(TEXT("CROSS-CHECK: the oracle-side min-violation readout is present (6a is built)"),
		OracleReadout.Readout.bPresent);

	if (OracleReadout.Readout.bPresent && OracleReadout.Readout.Joints.IsValidIndex(OracleJointForBed))
	{
		const FOracleJointReadout& JR = OracleReadout.Readout.Joints[OracleJointForBed];
		AddInfo(FString::Printf(
			TEXT("CROSS-CHECK: oracle readout on the bed joint — N %.6g, M %.6g, violation %.6g, util %.6g"),
			JR.NormalUu, JR.MomentUuCm, JR.ViolationUu, JR.Utilisation));

		/* Equilibrium is determinate, so N and |M| are forced regardless of the capacity model —
		 * green on arrival, proving the fixture genuinely BENDS (M != 0, unlike every prior readout). */
		const double TolN = 1.0e-3 * CorbelWeightUu();
		TestTrue(*FString::Printf(TEXT("CROSS-CHECK: oracle N %.6g == +W %.6g (the corbel bears down)"), JR.NormalUu, ExpectedNormalUu()),
			Near(JR.NormalUu, ExpectedNormalUu(), TolN));
		TestTrue(*FString::Printf(TEXT("CROSS-CHECK: oracle |M| %.6g == W*e %.6g (M != 0 — this fixture bends)"),
				FMath::Abs(JR.MomentUuCm), ExpectedMomentMagnitudeUu()),
			Near(FMath::Abs(JR.MomentUuCm), ExpectedMomentMagnitudeUu(), 1.0e-3 * ExpectedMomentMagnitudeUu()));

		/* RED — the readout must read the FIRST-CRACK utilisation, not the plastic one. */
		TestTrue(
			*FString::Printf(TEXT("CROSS-CHECK [RED]: oracle util %.6g == first-crack %.6g (not plastic %.6g)"),
				JR.Utilisation, ExpectedFirstCrackUtilisation(), ExpectedPlasticUtilisation()),
			Near(JR.Utilisation, ExpectedFirstCrackUtilisation(), 2.0e-3));
		TestFalse(
			*FString::Printf(TEXT("CROSS-CHECK [RED]: oracle util %.6g is NOT the plastic %.6g"),
				JR.Utilisation, ExpectedPlasticUtilisation()),
			Near(JR.Utilisation, ExpectedPlasticUtilisation(), 2.0e-3));
	}

	/* ------------------------------------------------------------------ *
	 * THE RED — SETTLE BELOW THE CAP, THEN QUERY THE CACHED READOUT. The cache
	 * must (a) source the readout from the min-violation LP AND (b) ask it for
	 * first-crack rows, so its utilisation matches what the break authority
	 * cracks the bond at. This drives BOTH fix parts: the oracle honouring
	 * bFirstCrackRows and the cache setting it.
	 * ------------------------------------------------------------------ */

	FBondedCorbel Below;
	Build(Below);
	Below.Structure.SetEquilibriumGateBlockCap(64); /* 2 pieces << cap => below cap, gate authoritative */

	const int32 Passes = Below.Structure.SolveAndBreak();

	/* Additivity: nothing may break (both models stand it), and the corbel reads Supported. */
	TestEqual(TEXT("ADDITIVE: settling breaks nothing — the corbel stands, the readout is a separate solve"),
		AnyJointBrokeUnderLoad(Below.Structure), false);
	TestTrue(TEXT("ADDITIVE: the corbel reads Supported below the cap (the LP carries it)"),
		Below.Structure.GetPieceSupport(Below.Corbel) == EPieceSupport::Supported);

	const FStructure::FConnectionReadout Readout = Below.Structure.GetConnectionReadout(Below.BedJoint);
	const double RouterUtil = Below.Structure.GetConnectionUtilisation(Below.BedJoint);

	AddInfo(FString::Printf(
		TEXT("SETTLED BELOW CAP: passes %d; cached readout present %d — N %.6g, M %.6g, violation %.6g, util %.6g; "
			 "router util %.6g. Expected first-crack util %.6g, plastic util %.6g"),
		Passes, Readout.bPresent ? 1 : 0,
		Readout.NormalUu, Readout.MomentUuCm, Readout.ViolationUu, Readout.Utilisation,
		RouterUtil, ExpectedFirstCrackUtilisation(), ExpectedPlasticUtilisation()));

	/* The core 6b wiring already caches a readout here (M ~ 0 there); it must be present. */
	TestTrue(TEXT("BELOW CAP: the bed joint has a cached min-violation readout after settling"),
		Readout.bPresent);

	/* Equilibrium is forced, so N and |M| are the same on either capacity model — green sanity that
	 * the cached fixture is the bending one, not that first-crack is wired. */
	const double TolN = 1.0e-3 * CorbelWeightUu();
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP: cached N %.6g == +W %.6g (the corbel bears down)"),
			Readout.NormalUu, ExpectedNormalUu()),
		Near(Readout.NormalUu, ExpectedNormalUu(), TolN));
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP: cached |M| %.6g == W*e %.6g (the joint bends)"),
			FMath::Abs(Readout.MomentUuCm), ExpectedMomentMagnitudeUu()),
		Near(FMath::Abs(Readout.MomentUuCm), ExpectedMomentMagnitudeUu(), 1.0e-3 * ExpectedMomentMagnitudeUu()));

	/* RED — the cached utilisation must be the FIRST-CRACK one the break authority uses. Today the
	 * cache sets bFirstCrackRows = false and the readout LP has no first-crack rows, so it reads the
	 * plastic utilisation, four times too comfortable. */
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP [RED]: cached util %.6g == first-crack %.6g (the model the bond cracks at)"),
			Readout.Utilisation, ExpectedFirstCrackUtilisation()),
		Near(Readout.Utilisation, ExpectedFirstCrackUtilisation(), 2.0e-3));

	TestFalse(
		*FString::Printf(TEXT("BELOW CAP [RED]: cached util %.6g is NOT the plastic %.6g (which is what it reads today)"),
			Readout.Utilisation, ExpectedPlasticUtilisation()),
		Near(Readout.Utilisation, ExpectedPlasticUtilisation(), 2.0e-3));

	/* Within both capacities => zero violation. Green on arrival (0 ~ 0); it bites once first-crack
	 * rows are in the readout and a slightly heavier corbel is posed — kept verdict-free here. */
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP: cached violation %.6g ~ 0 (the corbel stands on both models)"), Readout.ViolationUu),
		Near(Readout.ViolationUu, ExpectedViolationUu(), 1.0e-3));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
