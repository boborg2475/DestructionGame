// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Below the cap, the min-violation strain readout must use first-crack rows, matching the break
 * authority (BreakByEquilibrium sets bFirstCrackRows). Otherwise a bonded joint in bending breaks
 * at first crack while the overlay shows its plastic utilisation, four times too low. This needed
 * two fixes: SolveMinViolationReadout honouring bFirstCrackRows, and CacheMinViolationReadout
 * setting it. At M = 0 the two models coincide, so this is the first fixture with M != 0.
 *
 * Fixture: a bonded corbel, a free brick overhanging a narrow grounded pier by e = 3h through one
 * bed joint whose only finite strength is tension. Hand statics:
 *   N = +W (compression positive), |M| = W * e = 3 W h, |M|/h = 3 W
 *   Plastic:     demand (|M|/h - N)/2 = W,    capacity f_t*Conv*A/2  => util 2W / (f_t*Conv*A)
 *   First crack: demand -N + 3|M|/h = 8W,     capacity f_t*Conv*A    => util 8W / (f_t*Conv*A)
 * They differ by exactly 4. Both stay below 1 (about 0.60 and 0.15), so the corbel stands and the
 * test checks only the readout's capacity model, not a verdict.
 *
 * Units derived here (1 MPa on 1 cm2 = 10000 uu), not imported. No ticking world needed. Named
 * namespace, not anonymous, for unity builds.
 */
namespace FirstCrackReadoutSupport
{
	using namespace DestructionLayout;

	/** Fired clay, g/cm3. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Single wythe depth on Y, so every joint's Y overlap is full. */
	constexpr double WytheWidthCm = 10.25;

	/** Mortar bed joint thickness. */
	constexpr double JointThicknessCm = 1.0;

	/** MassKg * 980 is already a weight in uu; do not apply the 100x factor again. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa on 1 cm2 is 10000 uu. Not imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/** Above the oracle's UncappedStrengthMPa (1e9), so no row is assembled. */
	constexpr double UncappedHere = 1.0e12;

	// Narrow grounded pier; its width is the joint width.
	constexpr double PierCentreXCm = 0.0;
	constexpr double PierSizeXCm = 10.0;   // joint length 2h = 10, h = 5
	constexpr double PierCentreZCm = 10.0;
	constexpr double PierSizeZCm = 20.0;   // top face at Z = 20

	// Overhanging free corbel, 40 cm long.
	constexpr double CorbelCentreXCm = 15.0; // e = 15 = 3h
	constexpr double CorbelSizeXCm = 40.0;
	constexpr double CorbelSizeZCm = 10.0;
	constexpr double CorbelCentreZCm = 26.0; // bottom face at Z = 21, a 1 cm bed joint

	/** Bond strength: both models stay below 1 (0.60, 0.15). f_t > 0 enables first-crack rows. */
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

	/** Finite tension, all else uncapped, so only tension and first-crack rows can govern. */
	FConnectionStrength BondTensionOnly()
	{
		FConnectionStrength S;
		S.TensileStrengthMPa = TensileMPa;       // tension and first-crack rows are written
		S.CompressiveStrengthMPa = UncappedHere; // no crushing row
		S.ShearCohesionMPa = UncappedHere;       // no friction row
		S.FrictionCoefficient = 0.0;
		// MaxShearStrengthMPa is unbounded by default, so no ceiling row.
		return S;
	}

	struct FBondedCorbel
	{
		FStructure Structure;

		int32 Pier = INDEX_NONE;   // grounded, below
		int32 Corbel = INDEX_NONE; // free, overhanging, above

		int32 BedJoint = INDEX_NONE; // Pier below, Corbel above
	};

	/** Lays the pier, corbel and bed joint; the joint is the pier's 10 cm width. */
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

	// Hand oracle: statics and both capacity models rebuilt from geometry, independent of the LP.

	double CorbelMassKg()
	{
		return ClayDensityGramsPerCubicCm * CorbelSizeXCm * WytheWidthCm * CorbelSizeZCm / 1000.0;
	}
	double CorbelWeightUu() { return CorbelMassKg() * GravityCmPerSecondSquared; }

	/** Bearing face: pier width by wythe depth. */
	double JointAreaSqCm() { return PierSizeXCm * WytheWidthCm; }

	/** Half the joint length, cm. */
	double JointHalfLengthCm() { return PierSizeXCm / 2.0; }

	/** Corbel centroid's offset from the joint centre. */
	double EccentricityCm() { return CorbelCentreXCm - PierCentreXCm; }

	// Compression positive: +W.
	double ExpectedNormalUu() { return CorbelWeightUu(); }

	// |M| = W * e.
	double ExpectedMomentMagnitudeUu() { return CorbelWeightUu() * EccentricityCm(); }

	// |M|/h, the term that makes the two models diverge.
	double BendingFibreUu() { return ExpectedMomentMagnitudeUu() / JointHalfLengthCm(); }

	// Plastic: far contact's tension against the per-contact (A/2) cap.
	double PlasticTensionDemandUu() { return (BendingFibreUu() - ExpectedNormalUu()) / 2.0; }
	double PlasticTensionCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm() / 2.0; }
	double ExpectedPlasticUtilisation() { return PlasticTensionDemandUu() / PlasticTensionCapacityUu(); }

	// First crack: uncracked peak-fibre demand against the full-face cap.
	double FirstCrackDemandUu() { return -ExpectedNormalUu() + 3.0 * BendingFibreUu(); }
	double FirstCrackCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm(); }
	double ExpectedFirstCrackUtilisation() { return FirstCrackDemandUu() / FirstCrackCapacityUu(); }

	// Within both capacities, so zero violation on either model.
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

/** Below the cap, the cached readout uses first-crack capacity on a bonded bending joint, as the break authority does. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFirstCrackReadoutTest,
	"DestructionGame.Acceptance.StrainReadout.ReadoutUsesFirstCrackForBondedBendingBelowTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFirstCrackReadoutTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace FirstCrackReadoutSupport;

	// Build, and check the topology: two pieces, one bed joint, complete geometry.

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

	// The models must diverge, or the fixture cannot tell which is used.
	TestTrue(
		*FString::Printf(TEXT("FIXTURE: first-crack util %.6g must exceed plastic util %.6g (M != 0 divergence)"),
			ExpectedFirstCrackUtilisation(), ExpectedPlasticUtilisation()),
		ExpectedFirstCrackUtilisation() - ExpectedPlasticUtilisation() > 0.1);

	/*
	 * Cross-check 1: posed as BreakByEquilibrium poses it below the cap, the LP stands the
	 * corbel, so the readout is purely additive.
	 */

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
	Feasibility.bFirstCrackRows = true; // the production below-cap gate pose
	const FOracleResult FeasResult = SolveRigidBlock(Feasibility);

	AddInfo(FString::Printf(
		TEXT("CROSS-CHECK: first-crack feasibility answered %d, lambda* %.10g — Stands means lambda* >= 1"),
		FeasResult.bAnswered ? 1 : 0, FeasResult.Lambda));

	TestTrue(TEXT("CROSS-CHECK: the oracle must ANSWER the first-crack feasibility pose"), FeasResult.bAnswered);
	TestEqual(TEXT("CROSS-CHECK: the first-crack LP STANDS the corbel (both models < 1) — additive readout"),
		static_cast<int32>(OutcomeOf(FeasResult)),
		static_cast<int32>(EOracleOutcome::Stands));

	// The oracle joint mapped from the bed connection.
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

	/*
	 * Cross-check 2: the oracle readout, asked for first-crack rows, must use them. Isolates
	 * SolveMinViolationReadout honouring bFirstCrackRows.
	 */

	FOracleProblem ReadoutPose;
	FString ReadoutWhy;
	TestTrue(TEXT("CROSS-CHECK: the bridge accepts the same fixture for the readout pose"),
		BuildRigidBlockProblem(Probe.Structure, ReadoutPose, ReadoutWhy));
	ReadoutPose.bGravityIsLive = false;
	ReadoutPose.bMinViolationReadout = true;
	ReadoutPose.bFirstCrackRows = true; // as the cache sets it

	const FOracleResult OracleReadout = SolveRigidBlock(ReadoutPose);

	TestTrue(TEXT("CROSS-CHECK: the oracle-side min-violation readout is present (6a is built)"),
		OracleReadout.Readout.bPresent);

	if (OracleReadout.Readout.bPresent && OracleReadout.Readout.Joints.IsValidIndex(OracleJointForBed))
	{
		const FOracleJointReadout& JR = OracleReadout.Readout.Joints[OracleJointForBed];
		AddInfo(FString::Printf(
			TEXT("CROSS-CHECK: oracle readout on the bed joint — N %.6g, M %.6g, violation %.6g, util %.6g"),
			JR.NormalUu, JR.MomentUuCm, JR.ViolationUu, JR.Utilisation));

		// N and |M| are determinate whatever the model; confirms the fixture bends.
		const double TolN = 1.0e-3 * CorbelWeightUu();
		TestTrue(*FString::Printf(TEXT("CROSS-CHECK: oracle N %.6g == +W %.6g (the corbel bears down)"), JR.NormalUu, ExpectedNormalUu()),
			Near(JR.NormalUu, ExpectedNormalUu(), TolN));
		TestTrue(*FString::Printf(TEXT("CROSS-CHECK: oracle |M| %.6g == W*e %.6g (M != 0 — this fixture bends)"),
				FMath::Abs(JR.MomentUuCm), ExpectedMomentMagnitudeUu()),
			Near(FMath::Abs(JR.MomentUuCm), ExpectedMomentMagnitudeUu(), 1.0e-3 * ExpectedMomentMagnitudeUu()));

		// First-crack utilisation, not plastic.
		TestTrue(
			*FString::Printf(TEXT("CROSS-CHECK [RED]: oracle util %.6g == first-crack %.6g (not plastic %.6g)"),
				JR.Utilisation, ExpectedFirstCrackUtilisation(), ExpectedPlasticUtilisation()),
			Near(JR.Utilisation, ExpectedFirstCrackUtilisation(), 2.0e-3));
		TestFalse(
			*FString::Printf(TEXT("CROSS-CHECK [RED]: oracle util %.6g is NOT the plastic %.6g"),
				JR.Utilisation, ExpectedPlasticUtilisation()),
			Near(JR.Utilisation, ExpectedPlasticUtilisation(), 2.0e-3));
	}

	/*
	 * Main check: settle below the cap, then read the cached readout. It must come from the
	 * min-violation LP with first-crack rows, covering both fixes.
	 */

	FBondedCorbel Below;
	Build(Below);
	Below.Structure.SetEquilibriumGateBlockCap(64); // 2 pieces, well below the cap

	const int32 Passes = Below.Structure.SolveAndBreak();

	// Additive: nothing breaks and the corbel reads Supported.
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

	// A readout must be cached.
	TestTrue(TEXT("BELOW CAP: the bed joint has a cached min-violation readout after settling"),
		Readout.bPresent);

	// N and |M| are model-independent; confirms the cached joint bends.
	const double TolN = 1.0e-3 * CorbelWeightUu();
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP: cached N %.6g == +W %.6g (the corbel bears down)"),
			Readout.NormalUu, ExpectedNormalUu()),
		Near(Readout.NormalUu, ExpectedNormalUu(), TolN));
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP: cached |M| %.6g == W*e %.6g (the joint bends)"),
			FMath::Abs(Readout.MomentUuCm), ExpectedMomentMagnitudeUu()),
		Near(FMath::Abs(Readout.MomentUuCm), ExpectedMomentMagnitudeUu(), 1.0e-3 * ExpectedMomentMagnitudeUu()));

	// The cached utilisation must be first-crack, as the break authority uses, not plastic.
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP [RED]: cached util %.6g == first-crack %.6g (the model the bond cracks at)"),
			Readout.Utilisation, ExpectedFirstCrackUtilisation()),
		Near(Readout.Utilisation, ExpectedFirstCrackUtilisation(), 2.0e-3));

	TestFalse(
		*FString::Printf(TEXT("BELOW CAP [RED]: cached util %.6g is NOT the plastic %.6g (which is what it reads today)"),
			Readout.Utilisation, ExpectedPlasticUtilisation()),
		Near(Readout.Utilisation, ExpectedPlasticUtilisation(), 2.0e-3));

	// Within both capacities, so zero violation.
	TestTrue(
		*FString::Printf(TEXT("BELOW CAP: cached violation %.6g ~ 0 (the corbel stands on both models)"), Readout.ViolationUu),
		Near(Readout.ViolationUu, ExpectedViolationUu(), 1.0e-3));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
