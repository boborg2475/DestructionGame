// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/ConnectionStrength.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Structural concrete, the calibration baseline DESIGN.md asks for: real
	 * published figures, with every other material eventually expressed as a
	 * ratio of these rather than as independently invented numbers.
	 *
	 * Compression dominates by design — concrete is roughly five times stronger
	 * in shear resistance terms and ten times in tension. That spread is the
	 * whole point of the directional model.
	 */
	constexpr double ConcreteCompressiveMPa = 30.0;
	constexpr double ConcreteShearMPa = 6.0;
	constexpr double ConcreteTensileMPa = 3.0;

	const FConnectionStrength Concrete{
		ConcreteCompressiveMPa,
		ConcreteShearMPa,
		ConcreteTensileMPa
	};

	/** One square centimetre keeps the arithmetic legible. */
	constexpr double UnitAreaSqCm = 1.0;

	/**
	 * Force, in Unreal units, that loads UnitAreaSqCm to the given stress.
	 *
	 * Spelled out here rather than reusing the production constant so the test
	 * fails if that constant is wrong, instead of agreeing with it.
	 * 1 N = 100 uu, 1 cm2 = 100 mm2, 1 MPa = 1 N/mm2 -> 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa)
	{
		return MPa * 100.0 * 100.0 * UnitAreaSqCm;
	}

	FConnectionLoad CompressionOf(double Force) { FConnectionLoad L; L.Compression = Force; return L; }
	FConnectionLoad TensionOf(double Force) { FConnectionLoad L; L.Tension = Force; return L; }
	FConnectionLoad ShearOf(double Force) { FConnectionLoad L; L.Shear = Force; return L; }
}

/**
 * Unit test for load-versus-strength utilisation.
 *
 * Pure arithmetic on already-classified loads: no world, no solver, no ticking,
 * so gravity is irrelevant by design. Per DESIGN.md the assertion is on the
 * mechanism — the utilisation ratio itself — never on anything moving.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthUtilisationTest,
	"DestructionGame.Core.ConnectionStrength.Utilisation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthUtilisationTest::RunTest(const FString& Parameters)
{
	struct FUtilisationCase
	{
		const TCHAR* Description;
		FConnectionLoad Load;
		double AreaSqCm;
		double ExpectedUtilisation;
	};

	const TArray<FUtilisationCase> Cases = {
		{
			TEXT("an unloaded joint is at zero utilisation"),
			FConnectionLoad(), UnitAreaSqCm, 0.0
		},

		// Each axis is checked against its own strength, independently.
		{
			TEXT("compression exactly at its limit is fully utilised"),
			CompressionOf(ForceForMPa(ConcreteCompressiveMPa)), UnitAreaSqCm, 1.0
		},
		{
			TEXT("shear exactly at its limit is fully utilised"),
			ShearOf(ForceForMPa(ConcreteShearMPa)), UnitAreaSqCm, 1.0
		},
		{
			TEXT("tension exactly at its limit is fully utilised"),
			TensionOf(ForceForMPa(ConcreteTensileMPa)), UnitAreaSqCm, 1.0
		},

		// Below the limit the joint holds; above it, it gives.
		{
			TEXT("compression at half its limit holds"),
			CompressionOf(ForceForMPa(ConcreteCompressiveMPa / 2.0)), UnitAreaSqCm, 0.5
		},
		{
			TEXT("compression at twice its limit gives"),
			CompressionOf(ForceForMPa(ConcreteCompressiveMPa * 2.0)), UnitAreaSqCm, 2.0
		},

		// A joint fails on whichever axis gives first, so the worst governs.
		// Here compression is comfortable at 0.5 but shear is already at its limit.
		{
			TEXT("the most utilised axis governs a combined load"),
			[]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(ConcreteCompressiveMPa / 2.0);
				L.Shear = ForceForMPa(ConcreteShearMPa);
				return L;
			}(),
			UnitAreaSqCm, 1.0
		},

		// Strength is stress-based, not force-based: the same force through half
		// the area is twice the stress. A force-only model would miss this, and
		// thin joints would wrongly survive.
		{
			TEXT("halving the interface area doubles the utilisation"),
			CompressionOf(ForceForMPa(ConcreteCompressiveMPa)), UnitAreaSqCm / 2.0, 2.0
		},
	};

	constexpr double Tolerance = 1e-9;

	for (const FUtilisationCase& Case : Cases)
	{
		const double Utilisation = DestructionForce::ComputeUtilisation(Case.Load, Concrete, Case.AreaSqCm);

		TestTrue(
			FString::Printf(TEXT("%s: expected utilisation %f, got %f"),
				Case.Description, Case.ExpectedUtilisation, Utilisation),
			FMath::IsNearlyEqual(Utilisation, Case.ExpectedUtilisation, Tolerance));
	}

	return true;
}

/**
 * DESIGN.md's key validation of the directional model.
 *
 * The same force must be far more punishing in shear than in compression. If
 * these two numbers come out close, the directional logic is not really working
 * — something is collapsing the three strengths into one, which is precisely the
 * Chaos behaviour this system replaces.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthDirectionalAsymmetryTest,
	"DestructionGame.Core.ConnectionStrength.DirectionalAsymmetry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthDirectionalAsymmetryTest::RunTest(const FString& Parameters)
{
	// One force magnitude, applied two different ways against the same joint.
	const double Force = ForceForMPa(ConcreteShearMPa);

	const double CompressionUtilisation =
		DestructionForce::ComputeUtilisation(CompressionOf(Force), Concrete, UnitAreaSqCm);
	const double ShearUtilisation =
		DestructionForce::ComputeUtilisation(ShearOf(Force), Concrete, UnitAreaSqCm);

	// Shear is at its limit while compression has plenty of headroom left.
	TestTrue(
		FString::Printf(TEXT("shear should be at its limit, got %f"), ShearUtilisation),
		FMath::IsNearlyEqual(ShearUtilisation, 1.0, 1e-9));

	TestTrue(
		FString::Printf(TEXT("the same force in compression should be well short of failure, got %f"),
			CompressionUtilisation),
		CompressionUtilisation < 0.5);

	// The gap must match the strength ratio, not merely be present.
	const double ExpectedRatio = ConcreteCompressiveMPa / ConcreteShearMPa;
	const double ActualRatio = ShearUtilisation / FMath::Max(CompressionUtilisation, UE_DOUBLE_SMALL_NUMBER);

	TestTrue(
		FString::Printf(TEXT("shear should be %fx more punishing than compression, got %fx"),
			ExpectedRatio, ActualRatio),
		FMath::IsNearlyEqual(ActualRatio, ExpectedRatio, 1e-6));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
