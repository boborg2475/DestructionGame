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

	// --- the standard brick, for the moment cases ----------------------------

	/*
	 * DESIGN.md's standard clay brick, 21.5 x 10.25 x 6.5 cm at 1.9 g/cm3.
	 *
	 * DENSITY FIRST IN THE PRODUCT, matching Layout's PieceMassKg: that ordering
	 * lands exactly on 2.72163125 kg while volume-first lands one ulp low. Nothing
	 * here is asserted to the ulp, but there is no reason to spell the arithmetic
	 * in the order that is known to drift.
	 */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double BrickMassKg = 1.9 * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 980 cm/s2 — the solver's gravity, in the cm world 1 uu = 1 cm gives us. */
	constexpr double BrickWeightUu = BrickMassKg * 980.0;

	/**
	 * Elastic section modulus of a rectangle, cm3.
	 *
	 * For a rectangle 2*HalfAlong wide and 2*HalfAcross deep, bending across the
	 * second axis: I = (2*HalfAlong)*(2*HalfAcross)^3/12 and the outermost fibre
	 * sits at HalfAcross, so W = I/c = (4/3)*HalfAlong*HalfAcross^2. Ordinary beam
	 * theory rather than a code figure, and said so.
	 */
	constexpr double SectionModulusCm3(double HalfAlongCm, double HalfAcrossCm)
	{
		return (4.0 / 3.0) * HalfAlongCm * HalfAcrossCm * HalfAcrossCm;
	}

	/*
	 * THE HEAD JOINT — the vertical face between two bricks in a course, 10.25 wide
	 * by 6.5 tall. A brick hanging off one of these is what stands in the game
	 * today and should not.
	 *
	 * Its in-plane axes are the wall thickness (u, half-extent 5.125) and the brick
	 * height (v, half-extent 3.25). A brick hanging off the end leans about u, so
	 * the fibre that opens is at the top of the joint, 3.25 cm up.
	 */
	constexpr double HeadJointAreaSqCm = BrickWidthCm * BrickHeightCm;
	constexpr double HeadJointModulusUCm3 = SectionModulusCm3(BrickWidthCm / 2.0, BrickHeightCm / 2.0);
	constexpr double HeadJointModulusVCm3 = SectionModulusCm3(BrickHeightCm / 2.0, BrickWidthCm / 2.0);

	/** Half a brick length: the offset from a head joint to the brick's own centre. */
	constexpr double HeadJointLeverArmCm = BrickLengthCm / 2.0;

	/*
	 * THE HALF-OVERHANGING BED PATCH — a brick corbelled half its own length, so it
	 * bears on 10.75 x 10.25 of the brick below rather than on the full 21.5.
	 *
	 * It leans across the overhang direction, half-extent 5.375, which puts the
	 * load path 5.375 cm off the patch centre — three times outside the middle
	 * third, so part of the joint really does open.
	 */
	constexpr double BedPatchBearingLengthCm = BrickLengthCm / 2.0;
	constexpr double BedPatchAreaSqCm = BedPatchBearingLengthCm * BrickWidthCm;
	constexpr double BedPatchModulusUCm3 =
		SectionModulusCm3(BrickWidthCm / 2.0, BedPatchBearingLengthCm / 2.0);
	constexpr double BedPatchLeverArmCm = BedPatchBearingLengthCm / 2.0;

	/** A load with a bending moment about the joint's first in-plane axis. */
	FConnectionLoad WithMomentU(FConnectionLoad Load, double MomentUuCm)
	{
		Load.BendingMomentUUuCm = MomentUuCm;
		return Load;
	}

	FConnectionLoad WithMomentV(FConnectionLoad Load, double MomentUuCm)
	{
		Load.BendingMomentVUuCm = MomentUuCm;
		return Load;
	}

	/**
	 * THE PRE-MOMENT MODEL, frozen — what ComputeUtilisation answered before
	 * bending existed, transcribed from it and reading none of the moment fields.
	 *
	 * MIRRORING PRODUCTION IS THE POINT HERE, and it is the one place in this suite
	 * where that is true. An oracle that re-derives today's algorithm proves
	 * nothing, but this is a snapshot of YESTERDAY's answer, and the property under
	 * test is literally identity with it: with no eccentricity the bending term
	 * vanishes and a joint must read exactly, bit for bit, what it read before —
	 * which is the only reason the existing suite and both fuzz generators can go
	 * on supplying no geometry at all.
	 *
	 * The conversion factor is spelled out from first principles rather than
	 * imported, so this fails if ForceUnitsPerMPaSqCm is wrong instead of agreeing
	 * with it.
	 */
	double PreMomentUtilisation(
		const FConnectionLoad& Load,
		const FConnectionStrength& Strength,
		double AreaSqCm)
	{
		constexpr double UuPerMPaPerSqCm = 100.0 * 100.0;

		if (!(AreaSqCm > 0.0))
		{
			return TNumericLimits<double>::Max();
		}

		const double CompressiveStress = Load.Compression / (AreaSqCm * UuPerMPaPerSqCm);
		const double TensileStress = Load.Tension / (AreaSqCm * UuPerMPaPerSqCm);
		const double ShearStress = Load.Shear / (AreaSqCm * UuPerMPaPerSqCm);

		const double ShearCapacityMPa = FMath::Min(
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * CompressiveStress,
			Strength.MaxShearStrengthMPa);

		auto Axis = [](double Stress, double CapacityMPa)
		{
			if (!FMath::IsFinite(Stress))
			{
				return TNumericLimits<double>::Max();
			}

			if (CapacityMPa > 0.0)
			{
				return Stress / CapacityMPa;
			}

			return Stress > 0.0 ? TNumericLimits<double>::Max() : 0.0;
		};

		return FMath::Max3(
			Axis(CompressiveStress, Strength.CompressiveStrengthMPa),
			Axis(ShearStress, ShearCapacityMPa),
			Axis(TensileStress, Strength.TensileStrengthMPa));
	}
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

	/*
	 * Read out of the shared profile here rather than at namespace scope, so the
	 * reads happen at test time rather than during cross-TU static initialisation.
	 */
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

		/*
		 * A joint fails on whichever axis gives first, so the worst governs.
		 * Here compression is comfortable at 0.5 but shear is already at its limit.
		 */
		{
			TEXT("the most utilised axis governs a combined load"),
			/*
			 * Captures by reference because the strengths are now read out of the
			 * shared profile into locals rather than declared at namespace scope.
			 */
			[&]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(ConcreteCompressiveMPa / 2.0);
				L.Shear = ForceForMPa(ConcreteShearMPa);
				return L;
			}(),
			UnitAreaSqCm, 1.0
		},

		/*
		 * Strength is stress-based, not force-based: the same force through half
		 * the area is twice the stress. A force-only model would miss this, and
		 * thin joints would wrongly survive.
		 */
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

		/*
		 * The core behaviour, at the MEAN-BASIS mortar row (re-anchor 2026-08-13):
		 * 1 MPa of compression buys mu = 0.75 MPa of extra shear capacity, taking
		 * the joint from its bare 0.90 to 1.65 — so a shear stress of 1.65 that
		 * would be ~1.8x over the bare bond now sits exactly at the limit. 1.65
		 * stays below the 2.0 MPa mean-basis cap (the cap bites only from
		 * (2.0 - 0.90) / 0.75 = 1.467 MPa of compression upward), so what is
		 * measured here really is cohesion + friction, not the ceiling.
		 */
		{
			TEXT("compression raises mortar shear capacity by mu times the normal stress"),
			GeneralPurposeMortar,
			[]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(1.0);
				L.Shear = ForceForMPa(1.65);
				return L;
			}(),
			1.0
		},

		/*
		 * Only compression helps. A joint being pulled open gains nothing, so an
		 * implementation using the magnitude of normal stress — tension included —
		 * would wrongly report this as holding.
		 */
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

		/*
		 * Dry stone is the pure case: zero cohesion, so capacity is entirely
		 * friction. 1 MPa of compression yields 0.7 MPa of capacity.
		 */
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

		/*
		 * Twice the compression, twice the capacity, half the utilisation. This is
		 * the mechanism behind progressive collapse: strip the load above a joint
		 * and its resistance to sliding falls away with it.
		 */
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

	/*
	 * With no bond and nothing pressing on it, a dry stone joint has no shear
	 * capacity whatsoever — any sliding load at all parts it. Asserted as "gives"
	 * rather than a number because the ratio is unbounded here.
	 */
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
 * MEAN-BASIS VALUES (re-anchor 2026-08-13). Mortar's ceiling is 2.0 MPa = 0.1 x
 * the 20 MPa unit — a mean-basis truncation, forced by measurement: the Newcastle
 * campaign (Gooch, Masia, Stewart & Lam 2023, ConBuildMat 386:131578) measures a
 * mean UNCONFINED shear bond of 1.81 MPa, above the old characteristic-basis
 * 0.065·f_b = 1.3 cap, so that cap cannot stand beside mean cohesion. Bare
 * cohesion is 0.90 (measured mean, same campaign: M4/M6 triplet means average
 * 1.117, regression intercepts 0.58-1.04) and mu is 0.75 (Gooch et al. 2025,
 * ConBuildMat 489:142348: initial friction 0.64-1.00, residual 0.60-1.11), so
 * the cap bites from (2.0 - 0.90) / 0.75 = 1.4667 MPa of compression upward.
 *
 * THE CEILING ITSELF IS READ FROM THE PROFILE rather than restated here, so a
 * retune moves the expectation with it. THE COMPRESSIONS ARE NOW DERIVED FROM
 * THE BITE POINT rather than sitting as literals (the old 3.0 / 4.0 sat 0.40 /
 * 0.50 MPa from the new bite point — the retune hazard CURRENT_STATE flagged):
 * both are bite + a margin, so they are past the cap by construction whatever
 * the profile says, and a fixture precondition pins that the compression axis
 * still does not govern at the deeper of the two.
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

	/*
	 * THE CANONICAL RED OF THE MEAN-STRENGTH RE-ANCHOR (2026-08-13), together
	 * with the pins in EdgeStressUnderAMoment below: these four fail until the
	 * profile rows flip to the mean basis DESIGN §3 decided on 2026-08-08.
	 *
	 *  - cohesion 0.90 MPa: measured mean shear bond at zero normal stress,
	 *    grade A — Gooch, Masia, Stewart & Lam 2023 (ConBuildMat 386:131578),
	 *    M4/M6 unconfined triplet means averaging 1.117; Gooch et al. 2025
	 *    (ConBuildMat 489:142348) regression intercepts 0.58-1.04. 0.90 is the
	 *    centre of the regression range, just under the triplet average.
	 *  - mu 0.75: measured initial friction 0.64-1.00 and residual 0.60-1.11
	 *    (Normal, COV 0.14, 246 tests) — the old 0.6 sat at the low end of the
	 *    measured means, not the centre.
	 *  - cap 2.0 MPa = 0.1 x f_b: mean-basis truncation, forced by the measured
	 *    unconfined mean of 1.81 MPa exceeding the characteristic-basis 1.3.
	 *  - compressive 10.0: UNCHANGED — EN 998-2 / EN 1015-11 declare a MEAN
	 *    compressive class, so the uplift must not be applied twice.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION (mean re-anchor): cohesion must be the measured mean 0.90 MPa, profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.9);

	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION (mean re-anchor): mu must be the measured-mean centre 0.75, profile carries %g"),
			GeneralPurposeMortar.FrictionCoefficient),
		GeneralPurposeMortar.FrictionCoefficient == 0.75);

	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION (mean re-anchor): the shear ceiling must be the mean-basis 2.0 MPa (0.1 x f_b), profile carries %g"),
			GeneralPurposeMortar.MaxShearStrengthMPa),
		GeneralPurposeMortar.MaxShearStrengthMPa == 2.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: compressive stays the declared-mean 10 MPa, profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	/*
	 * THE BITE POINT, DERIVED FROM THE PROFILE — the compression at which
	 * cohesion + mu * sigma reaches the ceiling. Every compression below is this
	 * plus a margin, so the blocks measure the cap by construction rather than
	 * relying on literals that a retune could strand on the wrong side of it
	 * (the flagged CURRENT_STATE hazard, now closed).
	 */
	const double BiteMPa =
		(GeneralPurposeMortar.MaxShearStrengthMPa - GeneralPurposeMortar.ShearCohesionMPa)
		/ GeneralPurposeMortar.FrictionCoefficient;

	const double ShallowerCompressionMPa = BiteMPa + 1.0;
	const double DeeperCompressionMPa = BiteMPa + 2.0;

	/*
	 * At bite + 1 the joint would have cohesion + mu more capacity uncapped;
	 * capped, capacity is the ceiling itself, so a shear stress of exactly the
	 * ceiling sits at the limit rather than comfortably inside it.
	 */
	{
		FConnectionLoad Load;
		Load.Compression = ForceForMPa(ShallowerCompressionMPa);
		Load.Shear = ForceForMPa(MortarMaxShearMPa);

		const double Utilisation =
			DestructionForce::ComputeUtilisation(Load, GeneralPurposeMortar, UnitAreaSqCm);

		TestTrue(
			FString::Printf(TEXT("at the cap the joint is exactly at its limit, expected 1.0, got %f"),
				Utilisation),
			FMath::IsNearlyEqual(Utilisation, 1.0, Tolerance));
	}

	/*
	 * Past the cap, extra compression buys nothing. Two different depths in a
	 * wall must shear identically — that is what stops the base of a tall
	 * structure becoming stronger without limit.
	 *
	 * Both compressions are past the bite point by construction, but must stay
	 * well below mortar's own crushing limit: push the compression much higher
	 * and the compression axis overtakes shear and governs the result, so the
	 * assertion would be measuring crushing rather than the cap. Asserted as a
	 * precondition on the DEEPER compression rather than trusted to arithmetic
	 * in a comment.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: shear must govern at the deeper compression — compression axis %g must stay below shear axis %g"),
			DeeperCompressionMPa / GeneralPurposeMortar.CompressiveStrengthMPa,
			1.0 / MortarMaxShearMPa),
		DeeperCompressionMPa / GeneralPurposeMortar.CompressiveStrengthMPa < 1.0 / MortarMaxShearMPa);

	{
		FConnectionLoad Shallower;
		Shallower.Compression = ForceForMPa(ShallowerCompressionMPa);
		Shallower.Shear = ForceForMPa(1.0);

		FConnectionLoad Deeper;
		Deeper.Compression = ForceForMPa(DeeperCompressionMPa);
		Deeper.Shear = ForceForMPa(1.0);

		const double ShallowerUtilisation =
			DestructionForce::ComputeUtilisation(Shallower, GeneralPurposeMortar, UnitAreaSqCm);
		const double DeeperUtilisation =
			DestructionForce::ComputeUtilisation(Deeper, GeneralPurposeMortar, UnitAreaSqCm);

		TestTrue(
			FString::Printf(TEXT("deepening compression past the cap must not change shear utilisation: %f vs %f"),
				ShallowerUtilisation, DeeperUtilisation),
			FMath::IsNearlyEqual(ShallowerUtilisation, DeeperUtilisation, Tolerance));

		/*
		 * And it must be the capped value, not merely equal to each other — two
		 * identically wrong numbers would satisfy the assertion above on its own.
		 */
		TestTrue(
			FString::Printf(TEXT("capped shear utilisation should be 1.0/%f, expected %f, got %f"),
				MortarMaxShearMPa, 1.0 / MortarMaxShearMPa, ShallowerUtilisation),
			FMath::IsNearlyEqual(ShallowerUtilisation, 1.0 / MortarMaxShearMPa, Tolerance));
	}

	/*
	 * Below the cap nothing changes: friction still applies in full. Guards
	 * against a fix that clamps everywhere rather than only at the ceiling. The
	 * shear is DERIVED as the full uncapped capacity at 1 MPa of compression, so
	 * this block stays on the friction side of the bite point whatever the
	 * profile carries — the precondition asserts that rather than assuming it.
	 */
	{
		const double UncappedCapacityMPa =
			GeneralPurposeMortar.ShearCohesionMPa + GeneralPurposeMortar.FrictionCoefficient * 1.0;

		TestTrue(
			FString::Printf(TEXT("FIXTURE PRECONDITION: 1 MPa of compression must sit below the bite point — capacity %g < cap %g"),
				UncappedCapacityMPa, MortarMaxShearMPa),
			UncappedCapacityMPa < MortarMaxShearMPa);

		FConnectionLoad Load;
		Load.Compression = ForceForMPa(1.0);
		Load.Shear = ForceForMPa(UncappedCapacityMPa);

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

		/*
		 * Garbage arriving from upstream. Chaos can produce a NaN velocity in a
		 * pathological contact, which would reach here as a NaN force. Left
		 * unguarded that poisons the result and the joint reports itself INTACT
		 * during a physics blowup — the one moment it should certainly be giving.
		 * An infinite load is the same story with a different value.
		 */
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

	/*
	 * Dry stone is the important one here: real zeroes in two of its three
	 * strengths, so it reaches the degenerate paths without anything being
	 * misconfigured.
	 */
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
					/*
					 * Fail closed. A joint with no interface, or one handed a load
					 * nobody can make sense of, must not report itself intact —
					 * that is the single answer that must never come back.
					 */
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

/**
 * A joint carrying an off-centre load reports the stress at its outermost fibre,
 * not the average across its face.
 *
 * THE AVERAGE IS THE WRONG NUMBER whenever the load path does not run through the
 * joint's centroid, and it is wrong in the direction that leaves things standing.
 * A brick hanging off a single head joint is the case actually visible in the
 * game: gravity runs parallel to that joint, so the mean normal stress is EXACTLY
 * zero, the only axis carrying anything is shear against mortar's cohesion, and
 * the joint reads a comfortable 0.0044 — over two hundred bricks from failure.
 * What is physically happening is that the brick's weight acts 10.75 cm from the
 * joint and levers the top of it open, and mortar's tensile strength is a
 * fourteenth of its compressive strength, so it is nearly thirteen times closer
 * to failing than that. (Mean-basis figures since the 2026-08-13 re-anchor; the
 * characteristic-basis ratios were 0.02, fifty bricks and twenty times.)
 *
 *     sigma_n = (Tension - Compression) / A        signed, positive in tension
 *     sigma_b = |M_u|/W_u + |M_v|/W_v              worst corner, biaxial
 *     peak tension     = max(0, sigma_n + sigma_b)
 *     peak compression = max(0, sigma_b - sigma_n)
 *
 * NO NEW CONVERSION BOUNDARY, and this is worth checking rather than assuming
 * because "moments" sounds like it should introduce one. Length is cm, so M is in
 * uu.cm and M/W with W in cm3 is uu/cm2 — the identical quantity a force over an
 * area already is. It divides by the same 10000 and nothing else. Every
 * expectation below is spelled from published strengths and brick dimensions
 * without touching a production constant, so a factor of 100 in the wrong place
 * shows up here rather than being absorbed.
 *
 * Pure arithmetic on classified loads: no world, no solver, no tick, gravity
 * irrelevant. The assertion is on the mechanism — the utilisation ratio — exactly
 * as DESIGN.md §4 asks of a unit test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthEdgeStressTest,
	"DestructionGame.Core.ConnectionStrength.EdgeStressUnderAMoment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthEdgeStressTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionStrengthTestSupport;

	/*
	 * --- the expectations, and where every digit comes from --------------------
	 *
	 * Brick 21.5 x 10.25 x 6.5 at 1.9 g/cm3 = 2.72163125 kg, so its weight is
	 * 2.72163125 * 980 = 2667.198625 uu.
	 *
	 * (b) HELD BY ONE HEAD JOINT. A = 10.25 * 6.5 = 66.625 cm2 and
	 *     W_u = (4/3) * 5.125 * 3.25^2 = 72.1770833... cm3. The weight acts
	 *     10.75 cm away, so M_u = 2667.198625 * 10.75 = 28672.38521875 uu.cm.
	 *
	 *       sigma_n = 0                                  gravity is parallel to it
	 *       sigma_b = 28672.38521875 / 72.1770833...      = 397.250538... uu/cm2
	 *                                                     = 0.0397250538... MPa
	 *       tension = 0.03972505.. / 0.7 (mean f_x1)      = 0.0567500769231
	 *
	 * (a) CORBELLED HALF ITS OWN LENGTH. Bearing 10.75 * 10.25 = 110.1875 cm2 and
	 *     W = (4/3) * 5.125 * 5.375^2 = 197.4192708... cm3, load path 5.375 cm off
	 *     centre, so M = 2667.198625 * 5.375 = 14336.192609375 uu.cm.
	 *
	 *       sigma_n = -2667.198625 / 110.1875 = -24.206 uu/cm2  (exactly)
	 *       sigma_b =  14336.192609375 / 197.4192708... = 72.618 uu/cm2 (exactly)
	 *       tension =  (72.618 - 24.206) / 10000 / 0.7  = 0.006916  (exactly)
	 *
	 *     It decomposes exactly, which is the check that the model is doing what it
	 *     claims: sigma_b / |sigma_n| = 6e/t = 6 * 5.375 / 10.75 = 3 exactly, so
	 *     the opened edge is 2x the mean, and tension is resisted by a strength
	 *     14.3x smaller. 2 * 14.3 = the ~29x this row moves by, and a brick
	 *     corbelled half its length still stands at 0.7% — which is correct.
	 *
	 * MEAN BASIS (re-anchor 2026-08-13): both tension figures divide by the mean
	 * flexural bond 0.70 MPa where they used to divide by the characteristic 0.10
	 * — a straight /7, because utilisation is linear in 1/strength per axis and
	 * the stress side of both derivations is pure statics. The centred "today"
	 * answers move too: the head joint's shear is bare cohesion (no compression
	 * on a vertical joint), so it divides by 0.90 where it divided by 0.20.
	 */
	constexpr double HangingBrickUtilisation = 0.0567500769231;
	constexpr double CorbelledBrickUtilisation = 0.006916;

	/* What the same two joints answer today, with the load path treated as centred. */
	constexpr double HangingBrickShearUtilisationToday = 0.0040033 / 0.9;
	constexpr double CorbelledBrickCompressionUtilisationToday = 0.00024206;

	/*
	 * --- the fixture has to be capable of measuring what it claims to measure ---
	 *
	 * WHICH AXIS GOVERNS IS THE ENTIRE RISK, and here the governing axis is the
	 * finding: ComputeUtilisation returns the WORST of the axes, so this test only
	 * measures bending in tension while tension really is the worst. Worked
	 * through for case (b), all three, against mean-basis mortar's 10 / 0.9 / 0.7:
	 *
	 *   tension      0.0397250538 MPa / 0.7  = 0.0567501    <- governs
	 *   shear        0.0040033    MPa / 0.9  = 0.0044481     (12.8x smaller)
	 *   compression  0.0397250538 MPa / 10   = 0.0039725     (14.3x smaller)
	 *
	 * The compression edge grows by exactly as much as the tension edge does — the
	 * joint pivots, so one side opens as the other closes — and still never gets
	 * near governing, because the strength resisting it is 100x larger.
	 *
	 * Shear is untouched by any of this. Its capacity is bare cohesion, since the
	 * mean COMPRESSIVE stress is what buys friction and here it is zero.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: bending must beat shear on the head joint, %g vs %g"),
			HangingBrickUtilisation, HangingBrickShearUtilisationToday),
		HangingBrickUtilisation > HangingBrickShearUtilisationToday);

	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: bending in tension must beat the same bending in compression, %g vs %g"),
			HangingBrickUtilisation, 0.0039725054),
		HangingBrickUtilisation > 0.0039725054);

	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: bending must beat the mean compression on the corbel, %g vs %g"),
			CorbelledBrickUtilisation, 0.00096824),
		CorbelledBrickUtilisation > 0.00096824);

	/*
	 * The expectations are ratios of published strengths, so they only mean what
	 * they say while the profile still carries the figures they were derived
	 * against. A retune must fail loudly here rather than quietly moving every
	 * number above.
	 *
	 * MEAN BASIS (re-anchor 2026-08-13, DESIGN §3's 2026-08-08 decision): the
	 * tension figure is the mean flexural bond f_x1 = 0.70 MPa — the centre of
	 * two independent routes on the Newcastle campaign (Gooch, Masia, Stewart &
	 * Lam 2023, ConBuildMat 386:131578): (a) twelve measured M4/M6 batch means
	 * on extruded clay averaging 0.571, and (b) UK NA Table NA.6's
	 * characteristic 0.4 x the campaign's own mean/characteristic ratio 1.89 =
	 * 0.76. Cohesion is the measured mean 0.90 (same campaign; see ShearCap).
	 * The other two pins decide which axis governs.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: derived against mean f_x1 = 0.7 MPa, profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.7);

	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: derived against mean cohesion 0.9 MPa, profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.9);

	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: derived against compressive 10 MPa, profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: friction must buy nothing here, so the cap cannot govern: %g < %g"),
			GeneralPurposeMortar.ShearCohesionMPa, GeneralPurposeMortar.MaxShearStrengthMPa),
		GeneralPurposeMortar.ShearCohesionMPa < GeneralPurposeMortar.MaxShearStrengthMPa);

	const double HeadJointMomentUuCm = BrickWeightUu * HeadJointLeverArmCm;
	const double BedPatchMomentUuCm = BrickWeightUu * BedPatchLeverArmCm;

	struct FEdgeStressCase
	{
		const TCHAR* Description;
		FConnectionLoad Load;
		FJointSection Section;
		double ExpectedUtilisation;
	};

	const TArray<FEdgeStressCase> Cases = {
		/*
		 * THE ROW THAT CARRIES THE WHOLE POINT. A brick held by one head joint,
		 * with the mean normal stress exactly zero. Centred, this reads 0.0044 in
		 * shear and a chain of two hundred would be needed to part it; the fibre
		 * at the top of the joint is at 0.0568 of the mean f_x1 and eighteen
		 * brick weights part it.
		 */
		{
			TEXT("(b) a brick hanging off one head joint peels rather than shears"),
			WithMomentU(ShearOf(BrickWeightUu), HeadJointMomentUuCm),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, HeadJointModulusVCm3),
			HangingBrickUtilisation
		},

		/*
		 * The two in-plane axes are symmetric — neither is privileged, and a joint
		 * leaning the other way is the same joint. Guards an implementation that
		 * wires up only the first modulus, which would read as a plausible zero.
		 */
		{
			TEXT("(b) mirrored onto the other in-plane axis, same answer"),
			WithMomentV(ShearOf(BrickWeightUu), HeadJointMomentUuCm),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusVCm3, HeadJointModulusUCm3),
			HangingBrickUtilisation
		},

		/*
		 * Biaxial bending is the WORST CORNER, so the two contributions ADD. Half
		 * the moment about each of two equal moduli must reach the same edge stress
		 * as all of it about one — which max() or a root-sum-square would not.
		 */
		{
			TEXT("(b) split across both axes, the worst corner adds them"),
			WithMomentV(
				WithMomentU(ShearOf(BrickWeightUu), HeadJointMomentUuCm / 2.0),
				HeadJointMomentUuCm / 2.0),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, HeadJointModulusUCm3),
			HangingBrickUtilisation
		},

		/*
		 * The sign of a moment says which edge opens, not how hard. Leaning the
		 * other way is the same joint seen from the other side, so only the
		 * magnitude reaches the stress.
		 */
		{
			TEXT("(b) leaning the opposite way is just as bad"),
			WithMomentU(ShearOf(BrickWeightUu), -HeadJointMomentUuCm),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, HeadJointModulusVCm3),
			HangingBrickUtilisation
		},

		/*
		 * ZERO ECCENTRICITY IS NOT A SPECIAL CASE, it is e = 0. Take the moment
		 * away from the row above and the joint reads exactly what it reads today.
		 */
		{
			TEXT("(b) with the load path centred, the head joint reads today's shear answer"),
			ShearOf(BrickWeightUu),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, HeadJointModulusVCm3),
			HangingBrickShearUtilisationToday
		},

		/*
		 * (a) THE CORBEL, and the row where the mean normal stress is NOT zero — so
		 * it is the one that pins the subtraction in max(0, sigma_n + sigma_b)
		 * rather than just the bending term. Compression on the joint partly closes
		 * what the lean opens: 72.618 - 24.206 rather than 72.618.
		 *
		 * ITS SECOND MODULUS IS DELIBERATELY LEFT AT ZERO. A brick leans one way
		 * only, so there is no moment about the other axis and no section to resist
		 * it — 0/0. That must contribute nothing rather than a NaN or a failed
		 * joint, which is what forces the branch on the moment to come first.
		 */
		{
			TEXT("(a) a brick corbelled half its length is ~29x worse but still stands"),
			WithMomentU(CompressionOf(BrickWeightUu), BedPatchMomentUuCm),
			FJointSection(BedPatchAreaSqCm, BedPatchModulusUCm3, 0.0),
			CorbelledBrickUtilisation
		},

		{
			TEXT("(a) bedded centrally, the same patch reads today's compression answer"),
			CompressionOf(BrickWeightUu),
			FJointSection(BedPatchAreaSqCm, BedPatchModulusUCm3, 0.0),
			CorbelledBrickCompressionUtilisationToday
		},
	};

	constexpr double Tolerance = 1e-9;

	for (const FEdgeStressCase& Case : Cases)
	{
		const double Utilisation =
			DestructionForce::ComputeUtilisation(Case.Load, GeneralPurposeMortar, Case.Section);

		TestTrue(
			FString::Printf(TEXT("%s: expected utilisation %.10f, got %.10f"),
				Case.Description, Case.ExpectedUtilisation, Utilisation),
			FMath::IsNearlyEqual(Utilisation, Case.ExpectedUtilisation, Tolerance));
	}

	return true;
}

/**
 * A joint with no eccentricity answers EXACTLY what it answered before moments
 * existed — bit for bit, not nearly.
 *
 * This is the claim the whole slice rests on. If sigma = N/A +/- M/W collapses to
 * N/A only approximately, then every fixture in this suite, both fuzz generators
 * and every recorded utilisation in CURRENT_STATE would have to be reworked to
 * carry geometry before anything could move. Because it collapses exactly, a
 * geometry-free fixture is not a special case being tolerated — it is a load path
 * that happens to run through the centroid, the same way FrictionCoefficient = 0
 * reduces Mohr-Coulomb exactly rather than approximately.
 *
 * EXACT EQUALITY IS THE ASSERTION, deliberately. A 1e-9 tolerance here would pass
 * for a rearrangement that moved every joint in the project by a few ulps, and
 * the cascade fuzz has five joints settling at utilisation exactly 1.0 and one at
 * 1 - 1 ulp, so a single ulp of drift is five spurious break-decision failures.
 *
 * GREEN ON ARRIVAL by construction — it pins behaviour that is already correct
 * and must survive. It is a regression net, not a driver.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthZeroMomentTest,
	"DestructionGame.Core.ConnectionStrength.ZeroMomentIsUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthZeroMomentTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionStrengthTestSupport;

	struct FNamedLoad { const TCHAR* Description; FConnectionLoad Load; };
	struct FNamedProfile { const TCHAR* Description; FConnectionStrength Strength; };

	const TArray<FNamedLoad> Loads = {
		{ TEXT("unloaded"), FConnectionLoad() },
		{ TEXT("pure compression"), CompressionOf(ForceForMPa(1.0)) },
		{ TEXT("pure shear"), ShearOf(ForceForMPa(0.15)) },
		{ TEXT("pure tension"), TensionOf(ForceForMPa(0.05)) },

		// Friction coupling in the loop, since it reads the compressive stress too.
		{
			TEXT("compression and shear together"),
			[]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(1.0);
				L.Shear = ForceForMPa(0.8);
				return L;
			}()
		},

		// Past where the Mohr-Coulomb envelope is truncated, so the cap is in the loop.
		{
			TEXT("compression past the shear cap"),
			[]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(4.0);
				L.Shear = ForceForMPa(1.0);
				return L;
			}()
		},

		// A real brick's weight through a real bed joint, rather than round numbers.
		{ TEXT("a brick's weight through a bed patch"), CompressionOf(BrickWeightUu) },
	};

	const TArray<FNamedProfile> Profiles = {
		{ TEXT("concrete (uncoupled)"), ConcreteUncoupled },
		{ TEXT("mortar"), GeneralPurposeMortar },
		{ TEXT("dry stone, zero cohesion and zero tensile strength"), DryStone },
	};

	struct FNamedArea { const TCHAR* Description; double AreaSqCm; };

	const TArray<FNamedArea> Areas = {
		{ TEXT("one square centimetre"), UnitAreaSqCm },
		{ TEXT("a head joint"), HeadJointAreaSqCm },
		{ TEXT("a half-overhanging bed patch"), BedPatchAreaSqCm },
	};

	for (const FNamedArea& Area : Areas)
	{
		for (const FNamedProfile& Profile : Profiles)
		{
			for (const FNamedLoad& Load : Loads)
			{
				const FString Context = FString::Printf(TEXT("%s / %s / %s"),
					Area.Description, Profile.Description, Load.Description);

				const double Expected =
					PreMomentUtilisation(Load.Load, Profile.Strength, Area.AreaSqCm);

				/*
				 * A section that knows its own geometry, but a load path running
				 * straight through its centroid. The moduli are real and non-zero,
				 * so this only passes if the bending term genuinely vanishes rather
				 * than being skipped whenever geometry is absent.
				 */
				const double WithGeometry = DestructionForce::ComputeUtilisation(
					Load.Load, Profile.Strength,
					FJointSection(Area.AreaSqCm, HeadJointModulusUCm3, HeadJointModulusVCm3));

				TestTrue(
					FString::Printf(TEXT("%s: a centred load on a real section must be bit-identical, expected %.17g got %.17g"),
						*Context, Expected, WithGeometry),
					WithGeometry == Expected);

				/*
				 * And a caller that supplies nothing but an area — every fixture in
				 * this suite and both fuzz generators — must reach the same answer
				 * by the same route.
				 */
				const double WithoutGeometry =
					DestructionForce::ComputeUtilisation(Load.Load, Profile.Strength, Area.AreaSqCm);

				TestTrue(
					FString::Printf(TEXT("%s: a geometry-free caller must be bit-identical, expected %.17g got %.17g"),
						*Context, Expected, WithoutGeometry),
					WithoutGeometry == Expected);
			}
		}
	}

	return true;
}

/**
 * A moment with no section to resist it fails closed; no moment at all does not.
 *
 * Same reasoning as the existing area guard, and the same failure mode it exists
 * to prevent: M/W on a zero modulus is an infinity or a NaN, NaN compares false
 * against everything including `> 1.0`, and the joint would report itself INTACT
 * on an input nobody can interpret. A structure quietly refusing to collapse is
 * far harder to diagnose than one that falls apart the moment something is
 * uninitialised.
 *
 * BUT ZERO OVER ZERO MUST NEVER BE EVALUATED. A joint that leans about one axis
 * only has no moment about the other, and a section with no extent on that axis
 * has no modulus either — 0/0, on the ordinary path, for a joint that is
 * perfectly healthy. So the branch is on the MOMENT first: no moment, no bending
 * term, whatever the modulus is. Reversing those two tests turns every
 * single-axis lean into a failed joint.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthDegenerateSectionTest,
	"DestructionGame.Core.ConnectionStrength.DegenerateSectionUnderAMoment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthDegenerateSectionTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionStrengthTestSupport;

	const double MomentUuCm = BrickWeightUu * HeadJointLeverArmCm;

	enum class EExpectation
	{
		/** Fails closed, the same sentinel the area guard returns. */
		ExactlyMax,

		/** Nothing loaded and nothing to resist: the honest answer is zero. */
		ExactlyZero,

		/** No bending term at all, so exactly what the joint read before moments. */
		PreMomentAnswer,

		/** Garbage in: unusable but interpretable, so failed and finite. */
		FailedButFinite,

		/**
		 * A real bending answer, so it must be loaded and it must be holding —
		 * neither a zero from a skipped term nor the fail-closed sentinel.
		 */
		IntactUnderBendingAlone,
	};

	struct FDegenerateCase
	{
		const TCHAR* Description;
		FConnectionLoad Load;
		FJointSection Section;
		EExpectation Expectation;
	};

	const TArray<FDegenerateCase> Cases = {
		{
			TEXT("a moment about u with no modulus about u"),
			WithMomentU(FConnectionLoad(), MomentUuCm),
			FJointSection(HeadJointAreaSqCm, 0.0, HeadJointModulusVCm3),
			EExpectation::ExactlyMax
		},
		{
			TEXT("a moment about v with no modulus about v"),
			WithMomentV(FConnectionLoad(), MomentUuCm),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, 0.0),
			EExpectation::ExactlyMax
		},
		{
			TEXT("a negative section modulus is not a section"),
			WithMomentU(FConnectionLoad(), MomentUuCm),
			FJointSection(HeadJointAreaSqCm, -HeadJointModulusUCm3, HeadJointModulusVCm3),
			EExpectation::ExactlyMax
		},

		/*
		 * The one that matters most: a real load on the same joint. Without the
		 * guard this returns a perfectly plausible 0.02 and nothing downstream can
		 * tell that the geometry it was computed from was never filled in.
		 */
		{
			TEXT("a moment with no modulus must not hide behind a plausible load"),
			WithMomentU(ShearOf(BrickWeightUu), MomentUuCm),
			FJointSection(HeadJointAreaSqCm, 0.0, 0.0),
			EExpectation::ExactlyMax
		},

		/*
		 * And the counterpart, which is why the moment has to be tested first: a
		 * section with no moduli at all is what every existing fixture supplies.
		 */
		{
			TEXT("no moment and no moduli is an unloaded joint, not a failed one"),
			FConnectionLoad(),
			FJointSection(HeadJointAreaSqCm, 0.0, 0.0),
			EExpectation::ExactlyZero
		},
		{
			TEXT("no moment and no moduli under a real load reads today's answer"),
			ShearOf(BrickWeightUu),
			FJointSection(HeadJointAreaSqCm, 0.0, 0.0),
			EExpectation::PreMomentAnswer
		},
		/*
		 * THE ROW THE BRANCH ORDER EXISTS FOR. A brick leans one way, so there is
		 * a moment about u and none about v — and a section with no extent on v has
		 * no modulus there either. Test the modulus before the moment and this
		 * healthy joint evaluates 0/0 and fails closed.
		 */
		{
			TEXT("no moment about v excuses having no modulus about v"),
			WithMomentU(FConnectionLoad(), MomentUuCm),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, 0.0),
			EExpectation::IntactUnderBendingAlone
		},

		/*
		 * Garbage from upstream. Chaos can produce a NaN in a pathological contact,
		 * and once a moment is derived from a position it is one more place for one
		 * to arrive from.
		 */
		{
			TEXT("a NaN moment"),
			WithMomentU(FConnectionLoad(), MakeNaN()),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, HeadJointModulusVCm3),
			EExpectation::FailedButFinite
		},
		{
			TEXT("an infinite moment"),
			WithMomentV(FConnectionLoad(), MakeInfinity()),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, HeadJointModulusVCm3),
			EExpectation::FailedButFinite
		},

		// The area guard still wins: a joint with no face is not a joint.
		{
			TEXT("a moment across no interface at all"),
			WithMomentU(FConnectionLoad(), MomentUuCm),
			FJointSection(0.0, HeadJointModulusUCm3, HeadJointModulusVCm3),
			EExpectation::ExactlyMax
		},
	};

	for (const FDegenerateCase& Case : Cases)
	{
		const double Utilisation =
			DestructionForce::ComputeUtilisation(Case.Load, GeneralPurposeMortar, Case.Section);

		TestFalse(
			FString::Printf(TEXT("%s: utilisation must never be NaN, got %g"), Case.Description, Utilisation),
			FMath::IsNaN(Utilisation));

		TestTrue(
			FString::Printf(TEXT("%s: utilisation must be finite, got %g"), Case.Description, Utilisation),
			FMath::IsFinite(Utilisation));

		switch (Case.Expectation)
		{
		case EExpectation::ExactlyMax:
			TestTrue(
				FString::Printf(TEXT("%s: must fail closed with the same sentinel the area guard returns, got %g"),
					Case.Description, Utilisation),
				Utilisation == TNumericLimits<double>::Max());
			break;

		case EExpectation::ExactlyZero:
			TestTrue(
				FString::Printf(TEXT("%s: must read exactly zero, got %g"), Case.Description, Utilisation),
				Utilisation == 0.0);
			break;

		case EExpectation::PreMomentAnswer:
			{
				const double Expected =
					PreMomentUtilisation(Case.Load, GeneralPurposeMortar, Case.Section.AreaSqCm);

				TestTrue(
					FString::Printf(TEXT("%s: expected %.17g, got %.17g"),
						Case.Description, Expected, Utilisation),
					Utilisation == Expected);
			}
			break;

		case EExpectation::FailedButFinite:
			TestTrue(
				FString::Printf(TEXT("%s: must read as failed, got %g"), Case.Description, Utilisation),
				Utilisation > 1.0);
			break;

		case EExpectation::IntactUnderBendingAlone:
			TestTrue(
				FString::Printf(TEXT("%s: must carry the moment and hold, got %g"),
					Case.Description, Utilisation),
				Utilisation > 0.0 && Utilisation < 1.0);
			break;
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
