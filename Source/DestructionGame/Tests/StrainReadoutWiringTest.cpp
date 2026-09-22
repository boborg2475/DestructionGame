// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Below the block cap the LP is the break authority, so the strain readout must come from the LP
 * too: settling caches a per-joint min-violation readout (SolveMinViolationReadout), keyed to
 * connections through ConnectionOfJoint and exposed as GetConnectionReadout. It must match a hand
 * oracle rather than the router's GetConnectionUtilisation.
 *
 * Fixture: a brick hanging below a grounded anchor from one bed joint whose only finite axis is
 * tension. Hand statics: N = -W (compression positive), M = 0, violation 0,
 * utilisation = W / (f_t * 10000 * A). The router strands the hang and reads ~0, so matching the
 * hand value proves the LP is the source. The readout is a separate solve and breaks nothing.
 *
 * Units derived locally (1 MPa over 1 cm2 = 10000 uu). World-free. Named namespace for unity builds.
 */
namespace StrainReadoutWiringSupport
{
	using namespace DestructionLayout;

	constexpr double ClayDensityGramsPerCubicCm = 1.9;
	constexpr double WytheWidthCm = 10.25;
	constexpr double JointThicknessCm = 1.0;

	/** MassKg * 980 is already a weight in uu; do not apply 1 N = 100 uu again. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 MPa over 1 cm2 is 10000 uu. Not imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/** Above the oracle's UncappedStrengthMPa (1e9), so no row is assembled for it. */
	constexpr double UncappedHere = 1.0e12;

	constexpr double HangPlanXCm = 10.0;
	constexpr double HangHeightCm = 10.0;

	/** Sized for utilisation ~0.37: stands with margin, yet clearly non-zero against the router's ~0. */
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

	/** A weak finite tension bond with every other axis uncapped, so tension alone governs. */
	FConnectionStrength TensionHangBond()
	{
		FConnectionStrength S;
		S.TensileStrengthMPa = TensileMPa;
		S.CompressiveStrengthMPa = UncappedHere;
		S.ShearCohesionMPa = UncappedHere;
		S.FrictionCoefficient = 0.0;
		// MaxShearStrengthMPa is unbounded by default.
		return S;
	}

	struct FTensionHang
	{
		FStructure Structure;

		int32 Anchor = INDEX_NONE; // grounded, above
		int32 Hang = INDEX_NONE;   // free, below

		int32 BedJoint = INDEX_NONE;
	};

	/** Anchor directly above the hang with a 1 cm gap; nothing beneath the hang, so the router strands it. */
	void Build(FTensionHang& Out)
	{
		// Anchor: grounded, bottom face at Z = 35.
		const FPieceBox AnchorBox = MakeBox(/*X*/ 0.0, HangPlanXCm, /*Z*/ 40.0, HangHeightCm);

		// Hang: free, top face at Z = 34.
		const FPieceBox HangBox = MakeBox(/*X*/ 0.0, HangPlanXCm, /*Z*/ 29.0, HangHeightCm);

		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Hang = Out.Structure.AddPiece(BoxMassKg(HangBox), /*bIsGrounded*/ false, HangBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Anchor, AnchorBox, Out.Hang, HangBox, JointThicknessCm, TensionHangBond(), Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}
	}

	// Independent hand oracle, rebuilt from geometry and density.

	double HangMassKg()
	{
		return ClayDensityGramsPerCubicCm * HangPlanXCm * WytheWidthCm * HangHeightCm / 1000.0;
	}
	double HangWeightUu() { return HangMassKg() * GravityCmPerSecondSquared; }

	double JointAreaSqCm() { return HangPlanXCm * WytheWidthCm; }

	double TensionCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm(); }

	// N is compression positive, so tension reads negative.
	double ExpectedNormalUu() { return -HangWeightUu(); }
	double ExpectedViolationUu() { return 0.0; } // within capacity
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

/** Below the cap, the per-joint readout comes from the cached min-violation LP. See the file header. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStrainReadoutWiringTest,
	"DestructionGame.Acceptance.StrainReadout.PerJointReadoutIsLPSourcedBelowTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStrainReadoutWiringTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace StrainReadoutWiringSupport;

	// Topology: two pieces, one bed joint, complete geometry.

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

	// Cross-check 1: the LP stands the hang (lambda* >= 1).

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

	/*
	 * Cross-check 2: the min-violation LP solved directly matches the hand oracle, and
	 * ConnectionOfJoint maps back to the bed connection.
	 */

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

	// Settle a fresh build below the cap, then query the cached readout.

	FTensionHang Below;
	Build(Below);
	Below.Structure.SetEquilibriumGateBlockCap(64); // 2 pieces, well below the cap

	const int32 Passes = Below.Structure.SolveAndBreak();

	// Nothing breaks, and the LP keeps the hang Supported.
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

	// Present and keyed by ConnectionOfJoint.
	TestTrue(TEXT("BELOW CAP [RED]: the bed joint has a cached min-violation readout after settling"),
		Readout.bPresent);

	// The LP's force distribution matches the hand oracle.
	const double TolN = 1.0e-3 * HangWeightUu();
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP [RED]: cached N %.6g == -W %.6g (the LP carries the hang in tension)"),
			Readout.NormalUu, ExpectedNormalUu()),
		Near(Readout.NormalUu, ExpectedNormalUu(), TolN));

	TestTrue(
		*FString::Printf(TEXT("BELOW CAP [RED]: cached utilisation %.6g == W/(f_t*Conv*A) %.6g (LP primal)"),
			Readout.Utilisation, ExpectedUtilisation()),
		Near(Readout.Utilisation, ExpectedUtilisation(), 2.0e-3));

	// Within capacity, so zero violation.
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP: cached violation %.6g ~ 0 (the hang stands)"), Readout.ViolationUu),
		Near(Readout.ViolationUu, ExpectedViolationUu(), 1.0e-3));

	// Source proof: the router reads ~0 for a tension hang, so a sharp difference means the LP is the source.

	TestTrue(
		*FString::Printf(
			TEXT("BELOW CAP [RED]: the cached util %.6g differs sharply from the router's %.6g — the LP is "
				 "the source, not the per-joint sweep"),
			Readout.Utilisation, RouterUtil),
		FMath::Abs(Readout.Utilisation - RouterUtil) > 0.1);

	// Above the cap there is no cached readout; the overlay falls back to the router.

	FTensionHang Above;
	Build(Above);
	Above.Structure.SetEquilibriumGateBlockCap(1); // 2 pieces > cap, so the router decides
	Above.Structure.SolveAndBreak();

	const FStructure::FConnectionReadout AboveReadout = Above.Structure.GetConnectionReadout(Above.BedJoint);

	AddInfo(FString::Printf(TEXT("SETTLED ABOVE CAP: cached readout present %d (must be 0 — below-cap only)"),
		AboveReadout.bPresent ? 1 : 0));

	TestEqual(TEXT("ABOVE CAP: no cached readout — the min-violation LP is solved below the cap only"),
		AboveReadout.bPresent, false);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
