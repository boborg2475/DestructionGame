// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/ConnectionStrength.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous. An anonymous namespace here is the same
 * namespace as an anonymous one in any other test file the moment a unity build
 * merges the two translation units, which is a redefinition error waiting for
 * UBT's adaptive unity to stop keeping them apart. See CURRENT_STATE.md.
 *
 * WHY THE PROFILES ARE READ THROUGH A REFERENCE rather than copied into scalar
 * constants at namespace scope: the profiles live in another translation unit,
 * and copying their fields here would be a dynamic initialiser reading across
 * TUs. Binding a reference to an object with static storage duration is constant
 * initialisation, so it is safe whatever the order; the field reads then happen
 * inside RunTest, long after everything is initialised.
 */
namespace ConnectionStrengthTestSupport
{
	using namespace DestructionProfiles;

	/**
	 * Structural concrete, the calibration baseline DESIGN.md asks for. NOW THE
	 * SHARED PROFILE rather than a private copy of the figures: retuning concrete
	 * in Core/Profiles retunes it here, which is exactly what three divergent
	 * private copies could not do.
	 *
	 * Compression dominates by design — concrete is roughly five times stronger in
	 * shear resistance terms and ten times in tension. That spread is the whole
	 * point of the directional model.
	 *
	 * DELIBERATELY UNCOUPLED: the material profile's friction coefficient is zero,
	 * and that is a property of the data rather than of this fixture. A material is
	 * not a sliding interface, so with mu = 0 the Mohr-Coulomb shear capacity
	 * collapses to plain cohesion and the three axes stay strictly independent.
	 * Every expectation in the Utilisation and DirectionalAsymmetry tests below is
	 * therefore a regression guard proving that adding friction coupling did not
	 * disturb the uncoupled case — which is also the correct model for mechanical
	 * fasteners like bolts and screws.
	 *
	 * Coupled behaviour is covered separately in the FrictionCoupling test, using
	 * the mortar and dry stone connection profiles.
	 */
	const FConnectionStrength& ConcreteUncoupled = StructuralConcrete.Strength;

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

	/**
	 * Built at runtime through volatile locals so the optimiser cannot fold them
	 * into constants, which would let the very values under test disappear.
	 */
	double MakeNaN()
	{
		volatile double Zero = 0.0;
		return Zero / Zero;
	}

	double MakeInfinity()
	{
		volatile double Zero = 0.0;
		return 1.0 / Zero;
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
	using namespace ConnectionStrengthTestSupport;

	// Read out of the shared profile here rather than at namespace scope, so the
	// reads happen at test time rather than during cross-TU static initialisation.
	const double ConcreteCompressiveMPa = ConcreteUncoupled.CompressiveStrengthMPa;
	const double ConcreteShearMPa = ConcreteUncoupled.ShearCohesionMPa;
	const double ConcreteTensileMPa = ConcreteUncoupled.TensileStrengthMPa;

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
			// Captures by reference because the strengths are now read out of the
			// shared profile into locals rather than declared at namespace scope.
			[&]{
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
		const double Utilisation = DestructionForce::ComputeUtilisation(Case.Load, ConcreteUncoupled, Case.AreaSqCm);

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
	using namespace ConnectionStrengthTestSupport;

	const double ConcreteCompressiveMPa = ConcreteUncoupled.CompressiveStrengthMPa;
	const double ConcreteShearMPa = ConcreteUncoupled.ShearCohesionMPa;

	// One force magnitude, applied two different ways against the same joint.
	const double Force = ForceForMPa(ConcreteShearMPa);

	const double CompressionUtilisation =
		DestructionForce::ComputeUtilisation(CompressionOf(Force), ConcreteUncoupled, UnitAreaSqCm);
	const double ShearUtilisation =
		DestructionForce::ComputeUtilisation(ShearOf(Force), ConcreteUncoupled, UnitAreaSqCm);

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

/**
 * Mohr-Coulomb friction coupling: shear capacity grows with compression.
 *
 * The counterpart to the Utilisation test above. That one uses a deliberately
 * uncoupled profile (mu = 0) and proves the axes stay independent; this one uses
 * profiles with real friction and proves capacity responds to load.
 *
 *     shear capacity = cohesion + mu * compressive stress
 *
 * Why it matters: a masonry joint resists sliding partly because it is being
 * squeezed, so a wall sheds shear capacity as the weight above it is removed.
 * Without this, every joint holds a fixed strength no matter what is happening
 * around it, and dry stone — which has no bond at all — cannot stand up.
 *
 * Still pure arithmetic on classified loads. No world, no solver, gravity
 * irrelevant, assertions on the utilisation ratio itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthFrictionCouplingTest,
	"DestructionGame.Core.ConnectionStrength.FrictionCoupling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthFrictionCouplingTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionStrengthTestSupport;

	const double MortarCohesionMPa = GeneralPurposeMortar.ShearCohesionMPa;

	struct FCouplingCase
	{
		const TCHAR* Description;
		FConnectionStrength Strength;
		FConnectionLoad Load;
		double ExpectedUtilisation;
	};

	const TArray<FCouplingCase> Cases = {
		// Baseline: with nothing pressing on it, a mortar joint has only its bond.
		{
			TEXT("an uncompressed mortar joint gives at its bare cohesion"),
			GeneralPurposeMortar,
			ShearOf(ForceForMPa(MortarCohesionMPa)),
			1.0
		},

		// The core behaviour. 1 MPa of compression buys 0.6 MPa of extra shear
		// capacity, taking the joint from 0.2 to 0.8 — so a shear stress of 0.8
		// that would be four times over the bare limit now sits exactly at it.
		{
			TEXT("compression raises mortar shear capacity by mu times the normal stress"),
			GeneralPurposeMortar,
			[]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(1.0);
				L.Shear = ForceForMPa(0.8);
				return L;
			}(),
			1.0
		},

		// Only compression helps. A joint being pulled open gains nothing, so an
		// implementation using the magnitude of normal stress — tension included —
		// would wrongly report this as holding.
		{
			TEXT("tension buys no friction benefit"),
			GeneralPurposeMortar,
			[&]{
				FConnectionLoad L;
				L.Tension = ForceForMPa(0.05);
				L.Shear = ForceForMPa(MortarCohesionMPa);
				return L;
			}(),
			1.0
		},

		// Dry stone is the pure case: zero cohesion, so capacity is entirely
		// friction. 1 MPa of compression yields 0.7 MPa of capacity.
		{
			TEXT("dry stone holds only because it is compressed"),
			DryStone,
			[]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(1.0);
				L.Shear = ForceForMPa(0.5);
				return L;
			}(),
			0.5 / 0.7
		},

		// Twice the compression, twice the capacity, half the utilisation. This is
		// the mechanism behind progressive collapse: strip the load above a joint
		// and its resistance to sliding falls away with it.
		{
			TEXT("doubling the compression halves the dry stone utilisation"),
			DryStone,
			[]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(2.0);
				L.Shear = ForceForMPa(0.5);
				return L;
			}(),
			0.5 / 1.4
		},
	};

	constexpr double Tolerance = 1e-9;

	for (const FCouplingCase& Case : Cases)
	{
		const double Utilisation =
			DestructionForce::ComputeUtilisation(Case.Load, Case.Strength, UnitAreaSqCm);

		TestTrue(
			FString::Printf(TEXT("%s: expected utilisation %f, got %f"),
				Case.Description, Case.ExpectedUtilisation, Utilisation),
			FMath::IsNearlyEqual(Utilisation, Case.ExpectedUtilisation, Tolerance));
	}

	// With no bond and nothing pressing on it, a dry stone joint has no shear
	// capacity whatsoever — any sliding load at all parts it. Asserted as "gives"
	// rather than a number because the ratio is unbounded here.
	const double UnloadedDryStone =
		DestructionForce::ComputeUtilisation(ShearOf(ForceForMPa(0.01)), DryStone, UnitAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("unloaded dry stone should give under any shear, got %f"), UnloadedDryStone),
		UnloadedDryStone > 1.0);

	return true;
}

/**
 * Friction stops helping past the material's own limit.
 *
 * Mohr-Coulomb envelopes are truncated in reality: squeeze a joint hard enough
 * and the material gives rather than the faces sliding. Left uncapped, capacity
 * climbs forever with depth, so joints at the base of a tall building become
 * effectively unbreakable in shear — backwards physically, and backwards for a
 * demolition game, where the base is exactly where cutting should work.
 *
 * Mortar's ceiling is EN 1996-1-1's 0.065 x the unit's compressive strength, which
 * for the 20 MPa clay brick in the material library is 1.3 MPa. Bare cohesion is
 * 0.2 and mu is 0.6, so the cap bites from (1.3 - 0.2) / 0.6 = 1.83 MPa of
 * compression upward, and everything below uses more compression than that — so the
 * cap really is what is under test.
 *
 * THE CEILING ITSELF IS READ FROM THE PROFILE rather than restated here, so a
 * retune moves the expectation with it. The compressions stay literals because they
 * are test INPUTS rather than copies of production data — but they are only doing
 * their job while they sit between where the cap bites and where the compression
 * axis takes over, which the arithmetic above and below pins down.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthShearCapTest,
	"DestructionGame.Core.ConnectionStrength.ShearCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthShearCapTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionStrengthTestSupport;

	const double MortarMaxShearMPa = GeneralPurposeMortar.MaxShearStrengthMPa;

	constexpr double Tolerance = 1e-9;

	// 4 MPa of compression would buy 0.2 + 0.6 x 4.0 = 2.6 MPa uncapped. Capped,
	// capacity is 1.3, so a shear stress of exactly 1.3 sits at the limit rather
	// than at a comfortable 0.5.
	{
		FConnectionLoad Load;
		Load.Compression = ForceForMPa(4.0);
		Load.Shear = ForceForMPa(MortarMaxShearMPa);

		const double Utilisation =
			DestructionForce::ComputeUtilisation(Load, GeneralPurposeMortar, UnitAreaSqCm);

		TestTrue(
			FString::Printf(TEXT("at the cap the joint is exactly at its limit, expected 1.0, got %f"),
				Utilisation),
			FMath::IsNearlyEqual(Utilisation, 1.0, Tolerance));
	}

	// Past the cap, extra compression buys nothing. Two different depths in a
	// wall must shear identically — that is what stops the base of a tall
	// structure becoming stronger without limit.
	//
	// Both compressions are past the 1.83 MPa where the cap bites, but kept well
	// below mortar's own 10 MPa crushing limit on purpose: push the compression
	// much higher and the compression axis overtakes shear and governs the
	// result, so the assertion would be measuring crushing rather than the cap. At
	// 4 MPa the compression axis is at 0.4 against shear's 0.77, so shear governs
	// with room to spare.
	{
		FConnectionLoad Shallower;
		Shallower.Compression = ForceForMPa(3.0);
		Shallower.Shear = ForceForMPa(1.0);

		FConnectionLoad Deeper;
		Deeper.Compression = ForceForMPa(4.0);
		Deeper.Shear = ForceForMPa(1.0);

		const double ShallowerUtilisation =
			DestructionForce::ComputeUtilisation(Shallower, GeneralPurposeMortar, UnitAreaSqCm);
		const double DeeperUtilisation =
			DestructionForce::ComputeUtilisation(Deeper, GeneralPurposeMortar, UnitAreaSqCm);

		TestTrue(
			FString::Printf(TEXT("doubling compression past the cap must not change shear utilisation: %f vs %f"),
				ShallowerUtilisation, DeeperUtilisation),
			FMath::IsNearlyEqual(ShallowerUtilisation, DeeperUtilisation, Tolerance));

		// And it must be the capped value, not merely equal to each other — two
		// identically wrong numbers would satisfy the assertion above on its own.
		TestTrue(
			FString::Printf(TEXT("capped shear utilisation should be 1.0/%f, expected %f, got %f"),
				MortarMaxShearMPa, 1.0 / MortarMaxShearMPa, ShallowerUtilisation),
			FMath::IsNearlyEqual(ShallowerUtilisation, 1.0 / MortarMaxShearMPa, Tolerance));
	}

	// Below the cap nothing changes: friction still applies in full. Guards
	// against a fix that clamps everywhere rather than only at the ceiling.
	{
		FConnectionLoad Load;
		Load.Compression = ForceForMPa(1.0);
		Load.Shear = ForceForMPa(0.8);

		const double Utilisation =
			DestructionForce::ComputeUtilisation(Load, GeneralPurposeMortar, UnitAreaSqCm);

		TestTrue(
			FString::Printf(TEXT("below the cap friction is untouched, expected 1.0, got %f"), Utilisation),
			FMath::IsNearlyEqual(Utilisation, 1.0, Tolerance));
	}

	return true;
}

/**
 * Degenerate inputs must never produce NaN, never produce infinity, and never
 * let a broken joint pass for an intact one.
 *
 * This is a property test, not an example test: it sweeps every combination of
 * interface area, load shape and material profile and asserts invariants that
 * must hold across all of them, rather than checking particular numbers. The
 * numbers are covered by Utilisation and FrictionCoupling above.
 *
 * The invariant that matters is the direction of failure. NaN compares false
 * against everything, so `Utilisation > 1.0` on a NaN reports the joint as
 * INTACT — and a structure quietly refusing to collapse is far harder to
 * diagnose than one that falls apart the instant something is uninitialised.
 * These assertions exist to keep that failure mode from coming back.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthDegenerateInputTest,
	"DestructionGame.Core.ConnectionStrength.DegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionStrengthTestSupport;

	struct FNamedLoad { const TCHAR* Description; FConnectionLoad Load; };
	struct FNamedProfile { const TCHAR* Description; FConnectionStrength Strength; };
	struct FNamedArea { const TCHAR* Description; double AreaSqCm; bool bIsValidJoint; };

	const TArray<FNamedLoad> Loads = {
		{ TEXT("unloaded"), FConnectionLoad() },
		{ TEXT("compression only"), CompressionOf(ForceForMPa(1.0)) },
		{ TEXT("shear only"), ShearOf(ForceForMPa(1.0)) },
		{ TEXT("tension only"), TensionOf(ForceForMPa(0.05)) },
		{
			TEXT("compression and shear together"),
			[]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(1.0);
				L.Shear = ForceForMPa(0.5);
				return L;
			}()
		},

		// Garbage arriving from upstream. Chaos can produce a NaN velocity in a
		// pathological contact, which would reach here as a NaN force. Left
		// unguarded that poisons the result and the joint reports itself INTACT
		// during a physics blowup — the one moment it should certainly be giving.
		// An infinite load is the same story with a different value.
		{
			TEXT("a NaN load from upstream"),
			[]{
				FConnectionLoad L;
				L.Compression = MakeNaN();
				return L;
			}()
		},
		{
			TEXT("an infinite load from upstream"),
			[]{
				FConnectionLoad L;
				L.Shear = MakeInfinity();
				return L;
			}()
		},
	};

	// Dry stone is the important one here: real zeroes in two of its three
	// strengths, so it reaches the degenerate paths without anything being
	// misconfigured.
	const TArray<FNamedProfile> Profiles = {
		{ TEXT("concrete (uncoupled)"), ConcreteUncoupled },
		{ TEXT("mortar"), GeneralPurposeMortar },
		{ TEXT("dry stone, zero cohesion and zero tensile strength"), DryStone },
	};

	const TArray<FNamedArea> Areas = {
		{ TEXT("zero area"), 0.0, false },
		{ TEXT("negative area"), -UnitAreaSqCm, false },
		{ TEXT("valid area"), UnitAreaSqCm, true },
	};

	for (const FNamedArea& Area : Areas)
	{
		for (const FNamedProfile& Profile : Profiles)
		{
			for (const FNamedLoad& Load : Loads)
			{
				const double Utilisation =
					DestructionForce::ComputeUtilisation(Load.Load, Profile.Strength, Area.AreaSqCm);

				const FString Context = FString::Printf(TEXT("%s / %s / %s"),
					Area.Description, Profile.Description, Load.Description);

				TestFalse(
					FString::Printf(TEXT("%s: utilisation must never be NaN, got %f"), *Context, Utilisation),
					FMath::IsNaN(Utilisation));

				TestTrue(
					FString::Printf(TEXT("%s: utilisation must be finite, got %f"), *Context, Utilisation),
					FMath::IsFinite(Utilisation));

				const bool bLoadIsWellFormed =
					FMath::IsFinite(Load.Load.Compression)
					&& FMath::IsFinite(Load.Load.Tension)
					&& FMath::IsFinite(Load.Load.Shear);

				if (!Area.bIsValidJoint || !bLoadIsWellFormed)
				{
					// Fail closed. A joint with no interface, or one handed a load
					// nobody can make sense of, must not report itself intact —
					// that is the single answer that must never come back.
					TestTrue(
						FString::Printf(TEXT("%s: a degenerate joint must read as failed, got %f"),
							*Context, Utilisation),
						Utilisation > 1.0);
				}
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
