// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/ConnectionStrength.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Connection x material weakest-link pairing (SHED_PATH.md Phase B2), via
 * DestructionForce::EffectiveBondedStrength. Tensile and shear-cohesion capacity derate by the
 * weaker face's BondFactor, since a bond peels off the poorer face. Compression is bearing and is
 * not derated; the compression rows guard against derating every axis. Pure arithmetic, one load
 * axis at a time, asserted on capacity and utilisation.
 */
namespace BondFactorWeakestLinkTestSupport
{
	using namespace DestructionProfiles;

	/** One cm2, so stress = force / 10000. */
	constexpr double UnitAreaSqCm = 1.0;

	/**
	 * Force in uu that loads UnitAreaSqCm to the stress. Not imported from ForceUnitsPerMPaSqCm,
	 * so a wrong constant fails: 1 N = 100 uu and 1 cm2 = 100 mm2 give 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa)
	{
		return MPa * 100.0 * 100.0 * UnitAreaSqCm;
	}

	FConnectionLoad TensionOf(double Force) { FConnectionLoad L; L.Tension = Force; return L; }
	FConnectionLoad ShearOf(double Force) { FConnectionLoad L; L.Shear = Force; return L; }
	FConnectionLoad CompressionOf(double Force) { FConnectionLoad L; L.Compression = Force; return L; }

	/**
	 * Timber with BondFactor overridden to 0.5. Shipped profiles all use 1.0, so this tests the
	 * mechanism, not real timber calibration.
	 */
	constexpr double WoodFaceBondFactor = 0.5;

	FMaterialProfile PoorlyBondingWoodFace()
	{
		FMaterialProfile Face = Timber;
		Face.BondFactor = WoodFaceBondFactor;
		return Face;
	}
}

/**
 * The weakest-link pairing: a poorer-bonding face derates the bond (tension and shear),
 * but never the bearing (compression).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBondFactorWeakestLinkTest,
	"DestructionGame.Core.ConnectionStrength.BondFactorWeakestLink",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBondFactorWeakestLinkTest::RunTest(const FString& Parameters)
{
	using namespace BondFactorWeakestLinkTestSupport;

	// Wood (0.5) on brick (1.0) with general-purpose mortar: min BondFactor 0.5.
	const FConnectionStrength& Connection = GeneralPurposeMortar;
	const FMaterialProfile WoodFace = PoorlyBondingWoodFace();
	const FMaterialProfile& BrickFace = ClayBrick;

	const double MinBondFactor = FMath::Min(WoodFace.BondFactor, BrickFace.BondFactor);

	// Preconditions: profiles still carry the strengths the expectations were derived from.
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: mortar tensile bond must be 0.7 MPa, profile carries %g"),
			Connection.TensileStrengthMPa),
		Connection.TensileStrengthMPa == 0.7);
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: mortar shear cohesion must be 0.9 MPa, profile carries %g"),
			Connection.ShearCohesionMPa),
		Connection.ShearCohesionMPa == 0.9);
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: mortar compressive must be 10 MPa, profile carries %g"),
			Connection.CompressiveStrengthMPa),
		Connection.CompressiveStrengthMPa == 10.0);
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the wood face must be the weaker bond, %g <= %g"),
			WoodFace.BondFactor, BrickFace.BondFactor),
		MinBondFactor == WoodFace.BondFactor && MinBondFactor < 1.0);

	// Both faces' own capacities exceed the derated bond, so BondFactor governs.
	const double DeratedTensileMPa = Connection.TensileStrengthMPa * MinBondFactor;     // 0.35
	const double DeratedShearMPa = Connection.ShearCohesionMPa * MinBondFactor;         // 0.45

	TestTrue(
		TEXT("PRECONDITION: both faces' tensile capacity must exceed the derated bond, so the bond governs"),
		WoodFace.Strength.TensileStrengthMPa > DeratedTensileMPa
			&& BrickFace.Strength.TensileStrengthMPa > DeratedTensileMPa);
	TestTrue(
		TEXT("PRECONDITION: both faces' shear capacity must exceed the derated bond, so the bond governs"),
		WoodFace.Strength.ShearCohesionMPa > DeratedShearMPa
			&& BrickFace.Strength.ShearCohesionMPa > DeratedShearMPa);

	const FConnectionStrength Effective =
		DestructionForce::EffectiveBondedStrength(Connection, WoodFace, BrickFace);

	constexpr double Tolerance = 1e-9;

	// Tension: min(0.7 * 0.5, timber 14, brick 2.0) = 0.35 MPa.
	TestTrue(
		FString::Printf(TEXT("effective TENSILE must be the derated bond %g MPa, got %g"),
			DeratedTensileMPa, Effective.TensileStrengthMPa),
		FMath::IsNearlyEqual(Effective.TensileStrengthMPa, DeratedTensileMPa, Tolerance));

	// Shear cohesion: min(0.9 * 0.5, timber 4.0, brick 3.0) = 0.45 MPa.
	TestTrue(
		FString::Printf(TEXT("effective SHEAR COHESION must be the derated bond %g MPa, got %g"),
			DeratedShearMPa, Effective.ShearCohesionMPa),
		FMath::IsNearlyEqual(Effective.ShearCohesionMPa, DeratedShearMPa, Tolerance));

	// Compression is not derated: min(mortar 10, timber 21, brick 20) = 10 MPa, not 5.
	const double BearingCompressiveMPa = FMath::Min3(
		Connection.CompressiveStrengthMPa,
		WoodFace.Strength.CompressiveStrengthMPa,
		BrickFace.Strength.CompressiveStrengthMPa);                                     // 10

	TestTrue(
		FString::Printf(TEXT("effective COMPRESSIVE must be the un-derated bearing %g MPa, got %g"),
			BearingCompressiveMPa, Effective.CompressiveStrengthMPa),
		FMath::IsNearlyEqual(Effective.CompressiveStrengthMPa, BearingCompressiveMPa, Tolerance));

	TestTrue(
		FString::Printf(TEXT("compression must NOT be derated by BondFactor: %g must differ from %g"),
			Effective.CompressiveStrengthMPa, Connection.CompressiveStrengthMPa * MinBondFactor),
		!FMath::IsNearlyEqual(
			Effective.CompressiveStrengthMPa,
			Connection.CompressiveStrengthMPa * MinBondFactor, Tolerance));

	// Loads between derated and bare capacity: the paired joint fails, the bare one stands.
	struct FGapCase
	{
		const TCHAR* Description;
		FConnectionLoad Load;         // one axis, between derated and bare capacity
		double DeratedCapacityMPa;    // weakest-link capacity
		double BareCapacityMPa;       // bare connection capacity
	};

	/*
	 * Tension: 0.35 < 0.5 < 0.7. Shear: 0.45 < 0.6 < 0.9. Zero compression, so mortar's 2.0 shear
	 * cap never binds.
	 */
	const TArray<FGapCase> GapCases = {
		{
			TEXT("tension: 0.5 MPa parts the derated bond but not the bare connection"),
			TensionOf(ForceForMPa(0.5)), DeratedTensileMPa, Connection.TensileStrengthMPa
		},
		{
			TEXT("shear: 0.6 MPa slides the derated bond but not the bare connection"),
			ShearOf(ForceForMPa(0.6)), DeratedShearMPa, Connection.ShearCohesionMPa
		},
	};

	for (const FGapCase& Case : GapCases)
	{
		// The gap must exist.
		const double AppliedMPa = FMath::Max3(
			Case.Load.Tension, Case.Load.Shear, Case.Load.Compression) / (UnitAreaSqCm * 100.0 * 100.0);
		TestTrue(
			FString::Printf(TEXT("%s: PRECONDITION load %g must lie strictly between %g and %g"),
				Case.Description, AppliedMPa, Case.DeratedCapacityMPa, Case.BareCapacityMPa),
			AppliedMPa > Case.DeratedCapacityMPa && AppliedMPa < Case.BareCapacityMPa);

		const double Utilisation =
			DestructionForce::ComputeUtilisation(Case.Load, Effective, UnitAreaSqCm);

		// The bare connection stands.
		const double BareUtilisation =
			DestructionForce::ComputeUtilisation(Case.Load, Connection, UnitAreaSqCm);
		TestTrue(
			FString::Printf(TEXT("%s: the bare connection must STAND (< 1), got %g"),
				Case.Description, BareUtilisation),
			BareUtilisation < 1.0);

		// The derated bond fails.
		TestTrue(
			FString::Printf(TEXT("%s: the weakest-link joint must FAIL (> 1), got %g"),
				Case.Description, Utilisation),
			Utilisation > 1.0);
	}

	// Half the bearing capacity reads 0.5; BondFactor does not affect compression.
	const double CompressionUtilisation = DestructionForce::ComputeUtilisation(
		CompressionOf(ForceForMPa(BearingCompressiveMPa / 2.0)), Effective, UnitAreaSqCm);
	TestTrue(
		FString::Printf(TEXT("compression at half the bearing capacity must read 0.5 regardless of BondFactor, got %g"),
			CompressionUtilisation),
		FMath::IsNearlyEqual(CompressionUtilisation, 0.5, Tolerance));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
