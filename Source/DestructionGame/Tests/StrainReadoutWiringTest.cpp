// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 6b OF THE STEP-4 PROMOTION — THE MIN-VIOLATION STRAIN READOUT WIRED INTO PRODUCTION, RED.
 *
 * Slice 6a built and reviewed the ORACLE-SIDE min-violation readout
 * (RigidBlockOracle::SolveMinViolationReadout, routed on FOracleProblem::bMinViolationReadout) and
 * proved it sound, deterministic and permutation-invariant. But it is set NOWHERE in production:
 * FStructure never asks for it, and the strain overlay still reads the ROUTER's per-joint estimate
 * (InspectPiece -> Structure.GetConnectionUtilisation -> FConnection::UtilisationUnder of the
 * router's routed force). Structure.cpp's own comment at the cascade seam admits it — below the cap
 * "the forces it would read still sit in ConnectionForces from the solve above, so the utilisation
 * overlay is unchanged". Below the cap the LP is the break AUTHORITY, but the overlay a player reads
 * is still the sweep. 6b re-bases the overlay onto the LP.
 *
 * 6b IS: a solve-on-settle path that, when a structure settles BELOW the block cap, solves the
 * min-violation readout ONCE and CACHES the per-joint result, keyed back to production connections
 * through the bridge's ConnectionOfJoint provenance; and the overlay consuming that cached per-joint
 * Utilisation/ViolationUu below the cap. This test is the FIRST red — the CORE WIRING: a new
 * production-queryable per-connection strain readout on FStructure (GetConnectionReadout), sourced
 * from the cached min-violation LP and keyed by ConnectionOfJoint, whose value on the governing
 * joint matches an INDEPENDENT hand oracle and is NOT the router's per-joint estimate.
 *
 * THE FIXTURE — A DETERMINATE TENSION HANG (the 6a hand-oracle shape, as a production FStructure).
 * A single free brick hangs BELOW a grounded anchor from one bed joint whose ONLY finite strength
 * axis is TENSION (compression / cohesion uncapped, friction zero — exactly 6a's WeakTensionOnly),
 * so the governing axis is unambiguous and the statics are determinate:
 *
 *   HAND STATICS (independent of the LP, worked by hand here):
 *     N (compression positive)   = -W          (the whole weight crosses the joint in TENSION)
 *     M                          = 0            (central load, central joint — symmetric)
 *     tension capacity           = f_t * Conv * A
 *     ViolationUu                = 0            (bond holds W with margin => the structure STANDS)
 *     Utilisation                = W / (f_t*Conv*A)   (< 1, tension is the sole capped axis)
 *
 * WHY IT PROVES THE LP IS THE SOURCE, NOT THE SWEEP. A tension hang has NOTHING beneath it, so the
 * router's downward flood cannot route it — it is stranded, its routed joint force is ~0, and
 * GetConnectionUtilisation reads ~0. The LP carries the weight in tension and reads Utilisation
 * W/(f_t*Conv*A). So the cached readout's utilisation matches the hand oracle AND differs sharply
 * from the router estimate on the SAME connection — which is only possible if the readout is sourced
 * from the LP.
 *
 * WHY IT IS ADDITIVE (no verdict moves). The bond holds the hang with a comfortable margin, so the
 * below-cap equilibrium GATE already STANDS it today (Slice 3b/4 made the LP the break authority
 * below the cap). The readout is a SEPARATE cached solve; SolveAndBreak breaks nothing and the hang
 * reads Supported in both the before and after worlds. Because M = 0 the tension utilisation is the
 * same whether or not first-crack rows are assembled in the readout LP, so this red does not depend
 * on the follow-on first-crack-rows-in-readout decision.
 *
 * UNITS are derived here (1 MPa over 1 cm2 = 100 * 100 = 10000 uu), never imported, so a wrong
 * production constant disagrees rather than agrees.
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on the ordinary way (weight is MassKg * 980 inside
 * FStructure), everything is connected, and every assertion is on solver state, the oracle, or the
 * cached readout — the same footing as the support-authority and two-load-path acceptance tests.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace StrainReadoutWiringSupport
{
	using namespace DestructionLayout;

	/* ================================================================================
	 * UNITS AND GEOMETRY, derived here so a wrong production constant fails rather than agrees.
	 * ================================================================================ */

	/** Fired clay, 1.9 g/cm3 — the same figure every wall fixture uses. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** The single wythe: every piece is this deep on Y, so every joint's Y overlap is full. */
	constexpr double WytheWidthCm = 10.25;

	/** A 1 cm mortar bed joint — the separation the anchor and the hang are formed across. */
	constexpr double JointThicknessCm = 1.0;

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. NOT imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/** Above the oracle's UncappedStrengthMPa (1e9): a strength row this large is not assembled. */
	constexpr double UncappedHere = 1.0e12;

	/** The hang's plan face: 10 cm on X, the full wythe on Y. */
	constexpr double HangPlanXCm = 10.0;
	constexpr double HangHeightCm = 10.0;

	/**
	 * The tension bond, sized so Utilisation = W / (f_t*Conv*A) is a comfortable ~0.37: the hang
	 * STANDS with a wide margin (so the gate never fells it — the readout stays additive) yet the
	 * utilisation is a substantial, clearly-non-zero number the router's stranded ~0 cannot match.
	 */
	constexpr double TensileMPa = 0.005;

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
	 * A joint with a WEAK finite TENSION bond and every other axis uncapped, so the ONLY strength
	 * row that can carry demand is tension — which is what makes the hang's governing axis
	 * unambiguous. Mirrors 6a's WeakTensionOnly exactly.
	 */
	FConnectionStrength TensionHangBond()
	{
		FConnectionStrength S;
		S.TensileStrengthMPa = TensileMPa;      /* finite: the tension row IS written */
		S.CompressiveStrengthMPa = UncappedHere; /* no crushing row */
		S.ShearCohesionMPa = UncappedHere;       /* no friction row */
		S.FrictionCoefficient = 0.0;
		/* MaxShearStrengthMPa unbounded by default => no ceiling row. */
		return S;
	}

	struct FTensionHang
	{
		FStructure Structure;

		int32 Anchor = INDEX_NONE; /* grounded, above */
		int32 Hang = INDEX_NONE;   /* free, below */

		int32 BedJoint = INDEX_NONE; /* the one joint: Anchor above, Hang below */
	};

	/**
	 * Lay the two pieces and the one bed joint. The anchor sits directly above the hang with a 1 cm
	 * gap; the hang has NOTHING beneath it, so the router can only strand it and the LP must carry
	 * its weight across the bed joint in tension.
	 */
	void Build(FTensionHang& Out)
	{
		/* Anchor: grounded, bottom face at Z = 35. */
		const FPieceBox AnchorBox = MakeBox(/*X*/ 0.0, HangPlanXCm, /*Z*/ 40.0, HangHeightCm);

		/* Hang: free, top face at Z = 34 — one 1 cm bed joint below the anchor. */
		const FPieceBox HangBox = MakeBox(/*X*/ 0.0, HangPlanXCm, /*Z*/ 29.0, HangHeightCm);

		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Hang = Out.Structure.AddPiece(BoxMassKg(HangBox), /*bIsGrounded*/ false, HangBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Anchor, AnchorBox, Out.Hang, HangBox, JointThicknessCm, TensionHangBond(), Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}
	}

	/* ================================================================================
	 * THE INDEPENDENT HAND ORACLE — demand-minus-capacity statics, derived a DIFFERENT way
	 * than the LP. Area, weight and capacity are all rebuilt from geometry and density here.
	 * ================================================================================ */

	double HangMassKg()
	{
		return ClayDensityGramsPerCubicCm * HangPlanXCm * WytheWidthCm * HangHeightCm / 1000.0;
	}
	double HangWeightUu() { return HangMassKg() * GravityCmPerSecondSquared; }

	/** The bed face: 10 cm on X by the full wythe on Y. */
	double JointAreaSqCm() { return HangPlanXCm * WytheWidthCm; }

	double TensionCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm(); }

	/* N is COMPRESSION POSITIVE, so the whole weight crossing in tension reads negative. */
	double ExpectedNormalUu() { return -HangWeightUu(); }
	double ExpectedViolationUu() { return 0.0; } /* holds with margin => within capacity */
	double ExpectedUtilisation() { return HangWeightUu() / TensionCapacityUu(); }

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
 * The strain overlay's per-joint readout is sourced from the cached min-violation LP below the cap.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStrainReadoutWiringTest,
	"DestructionGame.Acceptance.StrainReadout.PerJointReadoutIsLPSourcedBelowTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStrainReadoutWiringTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace StrainReadoutWiringSupport;

	/* ------------------------------------------------------------------ *
	 * BUILD, AND CHECK THE TOPOLOGY IS THE ONE CLAIMED: two pieces, one bed
	 * joint, complete geometry, the hang held only by the overhead joint.
	 * ------------------------------------------------------------------ */

	FTensionHang Probe;
	Build(Probe);

	if (Probe.BedJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit the anchor-to-hang bed joint"));
		return false;
	}

	TestEqual(TEXT("FIXTURE: two pieces — a grounded anchor and one hang below it"),
		Probe.Structure.NumPieces(), 2);
	TestEqual(TEXT("FIXTURE: one joint — the overhead bed joint the hang tension-hangs from"),
		Probe.Structure.NumConnections(), 1);
	TestTrue(TEXT("FIXTURE: every piece and joint must know where it is, or there is no bridge"),
		Probe.Structure.HasCompleteGeometry());

	AddInfo(FString::Printf(
		TEXT("HAND ORACLE: W %.6g uu, area %.6g cm2, f_t %.6g MPa => capacity %.6g uu, "
			 "expected N %.6g, violation %.6g, utilisation %.6g"),
		HangWeightUu(), JointAreaSqCm(), TensileMPa, TensionCapacityUu(),
		ExpectedNormalUu(), ExpectedViolationUu(), ExpectedUtilisation()));

	/* ------------------------------------------------------------------ *
	 * CROSS-CHECK 1 — THE FIXTURE STANDS. The LP finds an admissible force
	 * system at self-weight (Stands, lambda* >= 1), so the below-cap gate
	 * already stands it and the readout wiring is purely additive.
	 * ------------------------------------------------------------------ */

	FOracleProblem Feasibility;
	FString BridgeWhy;

	const bool bBridged = BuildRigidBlockProblem(Probe.Structure, Feasibility, BridgeWhy);
	TestTrue(*FString::Printf(TEXT("CROSS-CHECK: the oracle bridge must accept this 2D hang (%s)"), *BridgeWhy),
		bBridged);

	if (!bBridged)
	{
		return false;
	}

	Feasibility.bGravityIsLive = false;
	const FOracleResult FeasResult = SolveRigidBlock(Feasibility);

	AddInfo(FString::Printf(
		TEXT("CROSS-CHECK: feasibility answered %d, lambda* %.10g — Stands means lambda* >= 1"),
		FeasResult.bAnswered ? 1 : 0, FeasResult.Lambda));

	TestTrue(TEXT("CROSS-CHECK: the oracle must ANSWER the feasibility pose"), FeasResult.bAnswered);
	TestEqual(TEXT("CROSS-CHECK: the LP must STAND the hang (bond holds W with margin) — additive readout"),
		static_cast<int32>(OutcomeOf(FeasResult)),
		static_cast<int32>(EOracleOutcome::Stands));

	/* ------------------------------------------------------------------ *
	 * CROSS-CHECK 2 — THE 6a ORACLE READOUT MATCHES THE HAND ORACLE. Solving
	 * the min-violation LP directly (bMinViolationReadout, built and reviewed
	 * in 6a) is the SOURCE 6b must cache. Confirming it here proves the hand
	 * oracle is right, and that the ONLY thing missing is the production
	 * caching/query wiring — and that the provenance maps joint 0 to the bed
	 * connection, which is the key the cached readout must use.
	 * ------------------------------------------------------------------ */

	FOracleProblem ReadoutPose;
	FString ReadoutWhy;
	TestTrue(TEXT("CROSS-CHECK: the bridge accepts the same fixture for the readout pose"),
		BuildRigidBlockProblem(Probe.Structure, ReadoutPose, ReadoutWhy));
	ReadoutPose.bGravityIsLive = false;
	ReadoutPose.bMinViolationReadout = true;

	const FOracleResult OracleReadout = SolveRigidBlock(ReadoutPose);

	TestTrue(TEXT("CROSS-CHECK: the oracle-side min-violation readout is present (6a is built)"),
		OracleReadout.Readout.bPresent);

	int32 OracleJointForBed = INDEX_NONE;
	for (int32 J = 0; J < ReadoutPose.ConnectionOfJoint.Num(); ++J)
	{
		if (ReadoutPose.ConnectionOfJoint[J] == Probe.BedJoint)
		{
			OracleJointForBed = J;
			break;
		}
	}

	TestTrue(TEXT("CROSS-CHECK: ConnectionOfJoint maps some oracle joint back to the bed connection"),
		OracleJointForBed != INDEX_NONE);

	if (OracleReadout.Readout.bPresent && OracleReadout.Readout.Joints.IsValidIndex(OracleJointForBed))
	{
		const FOracleJointReadout& JR = OracleReadout.Readout.Joints[OracleJointForBed];
		AddInfo(FString::Printf(
			TEXT("CROSS-CHECK: oracle readout on the bed joint — N %.6g, M %.6g, violation %.6g, util %.6g"),
			JR.NormalUu, JR.MomentUuCm, JR.ViolationUu, JR.Utilisation));

		const double TolN = 1.0e-3 * HangWeightUu();
		TestTrue(*FString::Printf(TEXT("CROSS-CHECK: oracle N %.6g == -W %.6g (tension)"), JR.NormalUu, ExpectedNormalUu()),
			Near(JR.NormalUu, ExpectedNormalUu(), TolN));
		TestTrue(*FString::Printf(TEXT("CROSS-CHECK: oracle violation %.6g ~ 0 (stands)"), JR.ViolationUu),
			Near(JR.ViolationUu, ExpectedViolationUu(), 1.0e-3));
		TestTrue(*FString::Printf(TEXT("CROSS-CHECK: oracle util %.6g == W/(f_t*Conv*A) %.6g"), JR.Utilisation, ExpectedUtilisation()),
			Near(JR.Utilisation, ExpectedUtilisation(), 1.0e-3));
	}

	/* ------------------------------------------------------------------ *
	 * THE RED — SETTLE BELOW THE CAP, THEN QUERY THE CACHED READOUT. Fresh
	 * build so SolveAndBreak stamps cleanly; the cap is well above 2 pieces,
	 * so the equilibrium gate is authoritative and (per 6b) must solve and
	 * cache the min-violation readout keyed by ConnectionOfJoint.
	 * ------------------------------------------------------------------ */

	FTensionHang Below;
	Build(Below);
	Below.Structure.SetEquilibriumGateBlockCap(64); /* 2 pieces << cap => below cap */

	const int32 Passes = Below.Structure.SolveAndBreak();

	/* Additivity: nothing may break, and the hang reads Supported (LP-authoritative below cap). */
	TestEqual(TEXT("ADDITIVE: settling breaks nothing — the readout is a separate cached solve"),
		AnyJointBrokeUnderLoad(Below.Structure), false);
	TestTrue(TEXT("ADDITIVE: the hang reads Supported below the cap (the LP carries it)"),
		Below.Structure.GetPieceSupport(Below.Hang) == EPieceSupport::Supported);

	const FStructure::FConnectionReadout Readout = Below.Structure.GetConnectionReadout(Below.BedJoint);
	const double RouterUtil = Below.Structure.GetConnectionUtilisation(Below.BedJoint);

	AddInfo(FString::Printf(
		TEXT("SETTLED BELOW CAP: passes %d; cached readout present %d — N %.6g, M %.6g, violation %.6g, util %.6g; "
			 "router GetConnectionUtilisation %.6g. Expected util %.6g"),
		Passes, Readout.bPresent ? 1 : 0,
		Readout.NormalUu, Readout.MomentUuCm, Readout.ViolationUu, Readout.Utilisation,
		RouterUtil, ExpectedUtilisation()));

	/* Present and keyed by ConnectionOfJoint — the core wiring. RED: the stub returns absent. */
	TestTrue(TEXT("BELOW CAP [RED]: the bed joint has a cached min-violation readout after settling"),
		Readout.bPresent);

	/* The LP's own force distribution — matches the hand oracle. RED: the stub returns zero. */
	const double TolN = 1.0e-3 * HangWeightUu();
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP [RED]: cached N %.6g == -W %.6g (the LP carries the hang in tension)"),
			Readout.NormalUu, ExpectedNormalUu()),
		Near(Readout.NormalUu, ExpectedNormalUu(), TolN));

	TestTrue(
		*FString::Printf(TEXT("BELOW CAP [RED]: cached utilisation %.6g == W/(f_t*Conv*A) %.6g (LP primal)"),
			Readout.Utilisation, ExpectedUtilisation()),
		Near(Readout.Utilisation, ExpectedUtilisation(), 2.0e-3));

	/* Within capacity => zero violation. Green on arrival (0 ~ 0); bites once the cache is real
	 * (a mutation charging a spurious slack would push this off zero). */
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP: cached violation %.6g ~ 0 (the hang stands)"), Readout.ViolationUu),
		Near(Readout.ViolationUu, ExpectedViolationUu(), 1.0e-3));

	/* ------------------------------------------------------------------ *
	 * THE SOURCE PROOF — the cached utilisation is the LP's, NOT the router's.
	 * The router cannot route an upward tension hang: it strands the hang and
	 * reads ~0 on this joint. So the readout matching the hand oracle while
	 * differing sharply from GetConnectionUtilisation on the SAME connection is
	 * only possible if the readout is sourced from the LP. RED: on the empty
	 * stub the cached util is 0, equal to the router's ~0, so this fails.
	 * ------------------------------------------------------------------ */

	TestTrue(
		*FString::Printf(
			TEXT("BELOW CAP [RED]: the cached util %.6g differs sharply from the router's %.6g — the LP is "
				 "the source, not the per-joint sweep"),
			Readout.Utilisation, RouterUtil),
		FMath::Abs(Readout.Utilisation - RouterUtil) > 0.1);

	/* ------------------------------------------------------------------ *
	 * SCOPING — ABOVE THE CAP THERE IS NO CACHED READOUT. The gate declines,
	 * nothing solves the min-violation LP, and the overlay falls back to the
	 * router. Green on arrival (the stub is always absent); it bites once dev
	 * solves the readout, guarding against solving it above the cap too.
	 * ------------------------------------------------------------------ */

	FTensionHang Above;
	Build(Above);
	Above.Structure.SetEquilibriumGateBlockCap(1); /* 2 pieces > cap => declines to router */
	Above.Structure.SolveAndBreak();

	const FStructure::FConnectionReadout AboveReadout = Above.Structure.GetConnectionReadout(Above.BedJoint);

	AddInfo(FString::Printf(TEXT("SETTLED ABOVE CAP: cached readout present %d (must be 0 — below-cap only)"),
		AboveReadout.bPresent ? 1 : 0));

	TestEqual(TEXT("ABOVE CAP: no cached readout — the min-violation LP is solved below the cap only"),
		AboveReadout.bPresent, false);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
