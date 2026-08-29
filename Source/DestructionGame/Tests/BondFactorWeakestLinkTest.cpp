// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/ConnectionStrength.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SHED_PATH.md Phase B / slice B2 — the connection x material weakest-link pairing.
 *
 * BEHAVIOUR UNDER TEST, in one sentence: a bonded joint's effective TENSILE and
 * SHEAR-COHESION capacity is the connection's own capacity DERATED by the weaker of
 * its two material faces' BondFactor (the bond peels off the poorer face), while its
 * COMPRESSION capacity is bearing and is NOT derated — so a wood-on-brick joint whose
 * wood face bonds poorly gives up under a tension the bare connection would have held.
 *
 * WHY THIS IS THE RED FOR B2. Today a joint reads ONLY its connection's
 * FConnectionStrength (FConnection::Strength -> ComputeUtilisation); the materials'
 * BondFactor is declared and unused (the only reader is ProfileLibraryTest's range
 * check). DestructionForce::EffectiveBondedStrength is the seam that will combine a
 * connection with its two faces, and its B2 stub returns the bare connection unchanged
 * — so every derated expectation below reads the un-derated bare capacity and the
 * tension/shear rows fail. That failure IS the missing behaviour, not a broken fixture.
 *
 * THE JUDGMENT THIS ENCODES (flagged to the reviewer): BondFactor is a bond property.
 * A bond PEELS in tension and slides in shear-cohesion, so those two axes derate by
 * min(BondFactor_A, BondFactor_B). Compression BEARS through the face whatever the
 * bond does — a poorly-bonded block still carries crushing load — so compression is
 * governed by the material's own crush, never by BondFactor. That split is asserted
 * directly: the compression row is a green characterisation pin that must stay
 * un-derated, and it is the guard against a green implementation that derates every
 * axis uniformly.
 *
 * Pure arithmetic on classified loads: no world, no solver, no tick. Gravity is
 * irrelevant by design (one load axis at a time, isolated). The assertion is on the
 * MECHANISM — the effective per-axis capacity and the utilisation ratio it produces —
 * never on anything moving, exactly as DESIGN.md §4 asks of a unit test.
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
	 * The derating wood face: production Timber, but with BondFactor overridden to a
	 * value < 1 to stand in for a poorer-bonding face. PRODUCTION Timber's BondFactor
	 * is 1.0 today (as are ClayBrick's and StructuralConcrete's), so no shipped fixture
	 * derates and every single-material joint stays bit-identical once B2 lands. This
	 * fixture proves the MECHANISM reads BondFactor; the datum (what a mortar bond to
	 * real timber actually is) is a separate calibration question, not this slice's.
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
	 * THE JOINT: a wood-on-brick BONDED joint. The connection is general-purpose
	 * mortar (a real cohesive bond); one face is a poorly-bonding wood face
	 * (BondFactor 0.5), the other is clay brick (BondFactor 1.0). The bond therefore
	 * peels at the WEAKER face, so min(BondFactor) = 0.5.
	 */
	const FConnectionStrength& Connection = GeneralPurposeMortar;
	const FMaterialProfile WoodFace = PoorlyBondingWoodFace();
	const FMaterialProfile& BrickFace = ClayBrick;

	const double MinBondFactor = FMath::Min(WoodFace.BondFactor, BrickFace.BondFactor);

	/*
	 * FIXTURE PRECONDITIONS — the numbers below only mean what they say while the
	 * profiles still carry the strengths they were hand-derived against, and while the
	 * weaker face really is the wood one. A retune must fail loudly here, not quietly
	 * move every expectation.
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
	 * The material OWN capacities must not govern the bond axes here — if they did,
	 * this test would be measuring the material cap rather than BondFactor, and could
	 * pass for the wrong reason. Both faces' tensile and shear strengths sit well above
	 * the derated bond, so min(...) picks the bonded term. Asserted, not assumed.
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
	 * --- MECHANISM: the effective bond capacity is derated by min(BondFactor) --------
	 *
	 * Tension: min(0.7 * 0.5, timber 14, brick 2.0) = 0.35 MPa. Today the stub returns
	 * the bare mortar (0.7) and this fails — which is the B2 red.
	 */
	TestTrue(
		FString::Printf(TEXT("effective TENSILE must be the derated bond %g MPa, got %g"),
			DeratedTensileMPa, Effective.TensileStrengthMPa),
		FMath::IsNearlyEqual(Effective.TensileStrengthMPa, DeratedTensileMPa, Tolerance));

	/*
	 * Shear cohesion: min(0.9 * 0.5, timber 4.0, brick 3.0) = 0.45 MPa. Same red.
	 */
	TestTrue(
		FString::Printf(TEXT("effective SHEAR COHESION must be the derated bond %g MPa, got %g"),
			DeratedShearMPa, Effective.ShearCohesionMPa),
		FMath::IsNearlyEqual(Effective.ShearCohesionMPa, DeratedShearMPa, Tolerance));

	/*
	 * --- THE JUDGMENT: compression BEARS, so BondFactor must NOT derate it ------------
	 *
	 * min(mortar 10, timber 21, brick 20) = 10 MPa, and crucially NOT 10 * 0.5 = 5.
	 * This row is green on arrival (the bare stub already returns 10) and stays green
	 * after B2 — it is the guard against an implementation that derates every axis.
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
	 * --- VERDICT: a load in the gap between derated and un-derated capacity ----------
	 *
	 * Each load below sits ABOVE the derated bond but BELOW the bare connection, so it
	 * FAILS the joint under the weakest-link rule and would STAND on the bare
	 * connection. That crossing of 1.0 is what proves BondFactor is live rather than
	 * merely a different number, and it is asserted on the utilisation ComputeUtilisation
	 * returns — the same mechanism the break authority and the strain readout consume.
	 */
	struct FGapCase
	{
		const TCHAR* Description;
		FConnectionLoad Load;         // one axis, at a stress between derated and bare capacity
		double DeratedCapacityMPa;    // weakest-link capacity for that axis
		double BareCapacityMPa;       // the bare connection capacity (today's answer)
	};

	/*
	 * Tension gap: 0.35 (derated) < 0.5 (load) < 0.7 (bare). Shear gap: 0.45 < 0.6 <
	 * 0.9. Zero compression on both, so no friction is bought and mortar's shear cap
	 * (2.0) never binds — the cohesion is the whole shear capacity, cleanly derated.
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
	 * And the compression counterpart of the verdict: a compression at 0.5 of the
	 * bearing capacity reads 0.5 whether or not the bond is derated — BondFactor does
	 * not touch it. Green on arrival, and it stays green after B2.
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
