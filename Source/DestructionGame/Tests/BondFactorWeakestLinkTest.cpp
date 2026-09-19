// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/ConnectionStrength.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The connection x material weakest-link pairing (SHED_PATH.md Phase B2).
 *
 * A bonded joint's effective TENSILE and SHEAR-COHESION capacity is the connection's
 * own capacity DERATED by the weaker of its two material faces' BondFactor (the bond
 * peels off the poorer face), while its COMPRESSION capacity is bearing and is NOT
 * derated — so a wood-on-brick joint whose wood face bonds poorly gives up under a
 * tension the bare connection would have held. DestructionForce::EffectiveBondedStrength
 * is the seam that combines a connection with its two faces.
 *
 * THE JUDGMENT THIS ENCODES: a bond PEELS in tension and slides in shear-cohesion, so
 * those two axes derate by min(BondFactor_A, BondFactor_B). Compression BEARS through
 * the face whatever the bond does — a poorly-bonded block still carries crushing load —
 * so it is governed by the material's own crush, never by BondFactor. The compression
 * row below is the guard against an implementation that derates every axis uniformly.
 *
 * Pure arithmetic on classified loads: no world, no solver, no tick. Gravity is
 * irrelevant by design (one load axis at a time). Assertions are on the MECHANISM — the
 * effective per-axis capacity and the utilisation ratio it produces — never on anything
 * moving.
 */
namespace BondFactorWeakestLinkTestSupport
{
	using namespace DestructionProfiles;

	/** One square centimetre keeps stress = force / 10000 and the arithmetic legible. */
	constexpr double UnitAreaSqCm = 1.0;

	/**
	 * Force, in Unreal units, that loads UnitAreaSqCm to the given stress in MPa.
	 *
	 * Spelled out from first principles rather than importing ForceUnitsPerMPaSqCm, so
	 * this test fails if that constant is wrong instead of silently agreeing with it.
	 * 1 N = 100 uu, 1 cm2 = 100 mm2, 1 MPa = 1 N/mm2 -> 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa)
	{
		return MPa * 100.0 * 100.0 * UnitAreaSqCm;
	}

	FConnectionLoad TensionOf(double Force) { FConnectionLoad L; L.Tension = Force; return L; }
	FConnectionLoad ShearOf(double Force) { FConnectionLoad L; L.Shear = Force; return L; }
	FConnectionLoad CompressionOf(double Force) { FConnectionLoad L; L.Compression = Force; return L; }

	/**
	 * The derating wood face: production Timber with BondFactor overridden below 1 to
	 * stand in for a poorer-bonding face. Production BondFactor is 1.0 for Timber,
	 * ClayBrick and StructuralConcrete today, so no shipped fixture derates; this proves
	 * the MECHANISM reads BondFactor, not the real-timber datum, which is a separate
	 * calibration question.
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

	/*
	 * THE JOINT: a wood-on-brick BONDED joint via general-purpose mortar. One face is
	 * poorly-bonding wood (BondFactor 0.5), the other clay brick (BondFactor 1.0), so
	 * the bond peels at the weaker face: min(BondFactor) = 0.5.
	 */
	const FConnectionStrength& Connection = GeneralPurposeMortar;
	const FMaterialProfile WoodFace = PoorlyBondingWoodFace();
	const FMaterialProfile& BrickFace = ClayBrick;

	const double MinBondFactor = FMath::Min(WoodFace.BondFactor, BrickFace.BondFactor);

	/*
	 * FIXTURE PRECONDITIONS: these numbers only mean what they say while the profiles
	 * carry the strengths they were hand-derived against, and while wood really is the
	 * weaker face. A retune must fail loudly here, not quietly move every expectation.
	 */
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

	/*
	 * The materials' OWN capacities must not govern the bond axes, or this test would
	 * measure the material cap rather than BondFactor. Both faces sit well above the
	 * derated bond on tension and shear, so min(...) picks the bonded term — asserted,
	 * not assumed.
	 */
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

	/*
	 * MECHANISM: effective bond capacity derates by min(BondFactor). Tension:
	 * min(0.7 * 0.5, timber 14, brick 2.0) = 0.35 MPa.
	 */
	TestTrue(
		FString::Printf(TEXT("effective TENSILE must be the derated bond %g MPa, got %g"),
			DeratedTensileMPa, Effective.TensileStrengthMPa),
		FMath::IsNearlyEqual(Effective.TensileStrengthMPa, DeratedTensileMPa, Tolerance));

	// Shear cohesion: min(0.9 * 0.5, timber 4.0, brick 3.0) = 0.45 MPa.
	TestTrue(
		FString::Printf(TEXT("effective SHEAR COHESION must be the derated bond %g MPa, got %g"),
			DeratedShearMPa, Effective.ShearCohesionMPa),
		FMath::IsNearlyEqual(Effective.ShearCohesionMPa, DeratedShearMPa, Tolerance));

	/*
	 * THE JUDGMENT: compression BEARS, so BondFactor must NOT derate it. min(mortar 10,
	 * timber 21, brick 20) = 10 MPa, not 10 * 0.5 = 5 — the guard against an
	 * implementation that derates every axis.
	 */
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

	/*
	 * VERDICT: each load below sits above the derated bond but below the bare
	 * connection, so it FAILS the weakest-link joint but would STAND on the bare
	 * connection. That crossing of 1.0 proves BondFactor is live, asserted on the
	 * utilisation ComputeUtilisation returns.
	 */
	struct FGapCase
	{
		const TCHAR* Description;
		FConnectionLoad Load;         // one axis, at a stress between derated and bare capacity
		double DeratedCapacityMPa;    // weakest-link capacity for that axis
		double BareCapacityMPa;       // the bare connection capacity (today's answer)
	};

	/*
	 * Tension gap: 0.35 < 0.5 < 0.7. Shear gap: 0.45 < 0.6 < 0.9. Zero compression on
	 * both, so mortar's shear cap (2.0) never binds — cohesion is the whole capacity.
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
		/* The gap must actually exist, or the row proves nothing. */
		const double AppliedMPa = FMath::Max3(
			Case.Load.Tension, Case.Load.Shear, Case.Load.Compression) / (UnitAreaSqCm * 100.0 * 100.0);
		TestTrue(
			FString::Printf(TEXT("%s: PRECONDITION load %g must lie strictly between %g and %g"),
				Case.Description, AppliedMPa, Case.DeratedCapacityMPa, Case.BareCapacityMPa),
			AppliedMPa > Case.DeratedCapacityMPa && AppliedMPa < Case.BareCapacityMPa);

		const double Utilisation =
			DestructionForce::ComputeUtilisation(Case.Load, Effective, UnitAreaSqCm);

		/* Bare-connection sanity: the same load on the un-paired connection stands. */
		const double BareUtilisation =
			DestructionForce::ComputeUtilisation(Case.Load, Connection, UnitAreaSqCm);
		TestTrue(
			FString::Printf(TEXT("%s: the bare connection must STAND (< 1), got %g"),
				Case.Description, BareUtilisation),
			BareUtilisation < 1.0);

		/* The weakest-link verdict: the derated bond FAILS under the same load. */
		TestTrue(
			FString::Printf(TEXT("%s: the weakest-link joint must FAIL (> 1), got %g"),
				Case.Description, Utilisation),
			Utilisation > 1.0);
	}

	/*
	 * Compression counterpart of the verdict: a load at half the bearing capacity reads
	 * 0.5 whether or not the bond is derated — BondFactor does not touch it.
	 */
	const double CompressionUtilisation = DestructionForce::ComputeUtilisation(
		CompressionOf(ForceForMPa(BearingCompressiveMPa / 2.0)), Effective, UnitAreaSqCm);
	TestTrue(
		FString::Printf(TEXT("compression at half the bearing capacity must read 0.5 regardless of BondFactor, got %g"),
			CompressionUtilisation),
		FMath::IsNearlyEqual(CompressionUtilisation, 0.5, Tolerance));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
