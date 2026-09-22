// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/ConnectionStrength.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named, not anonymous: anonymous namespaces from different test files collide in a unity
 * build. Profiles are bound by reference, not copied, to avoid cross-TU static init order;
 * the field reads happen inside RunTest.
 */
namespace ConnectionStrengthTestSupport
{
	using namespace DestructionProfiles;

	/**
	 * Structural concrete (the DESIGN.md calibration baseline), read from the shared profile.
	 * Its friction coefficient is zero, so shear capacity is plain cohesion and the three axes
	 * stay independent. FrictionCoupling covers the coupled case.
	 */
	const FConnectionStrength& ConcreteUncoupled = StructuralConcrete.Strength;

	constexpr double UnitAreaSqCm = 1.0;

	/**
	 * Force (uu) that loads UnitAreaSqCm to the given stress. Spelled out rather than using the
	 * production constant, so a wrong constant fails here: 1 N = 100 uu, 1 cm2 = 100 mm2 ->
	 * 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa)
	{
		return MPa * 100.0 * 100.0 * UnitAreaSqCm;
	}

	// Volatile so the optimiser cannot fold the values under test away.
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

	/*
	 * DESIGN.md's standard clay brick, 21.5 x 10.25 x 6.5 cm at 1.9 g/cm3. Density first, as in
	 * Layout's PieceMassKg, which lands exactly on 2.72163125 kg (volume-first is one ulp low).
	 */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double BrickMassKg = 1.9 * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 980 cm/s2, the solver's gravity. */
	constexpr double BrickWeightUu = BrickMassKg * 980.0;

	/** Elastic section modulus of a rectangle bending across its second axis, cm3: W = (4/3)*HalfAlong*HalfAcross^2. */
	constexpr double SectionModulusCm3(double HalfAlongCm, double HalfAcrossCm)
	{
		return (4.0 / 3.0) * HalfAlongCm * HalfAcrossCm * HalfAcrossCm;
	}

	/*
	 * Head joint: the vertical face between two bricks in a course, 10.25 wide by 6.5 tall.
	 * In-plane axes are wall thickness (u, half 5.125) and brick height (v, half 3.25). A
	 * hanging brick leans about u, opening the top fibre 3.25 cm up.
	 */
	constexpr double HeadJointAreaSqCm = BrickWidthCm * BrickHeightCm;
	constexpr double HeadJointModulusUCm3 = SectionModulusCm3(BrickWidthCm / 2.0, BrickHeightCm / 2.0);
	constexpr double HeadJointModulusVCm3 = SectionModulusCm3(BrickHeightCm / 2.0, BrickWidthCm / 2.0);

	/** Half a brick length: the offset from a head joint to the brick's own centre. */
	constexpr double HeadJointLeverArmCm = BrickLengthCm / 2.0;

	/*
	 * Bed patch of a brick corbelled half its length: bears on 10.75 x 10.25. The load path is
	 * 5.375 cm off the patch centre, outside the middle third, so part of the joint opens.
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
	 * Frozen snapshot of ComputeUtilisation before bending existed; reads no moment fields.
	 * Mirroring production is deliberate here: the property under test is bit-identity with the
	 * old answer when there is no eccentricity. The conversion factor is spelled out so a wrong
	 * ForceUnitsPerMPaSqCm fails here.
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

/** Load-versus-strength utilisation on classified loads. Pure arithmetic; asserts on the ratio (DESIGN.md §4). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthUtilisationTest,
	"DestructionGame.Core.ConnectionStrength.Utilisation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthUtilisationTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionStrengthTestSupport;

	// Read at test time, not during cross-TU static initialisation.
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

		{
			TEXT("compression at half its limit holds"),
			CompressionOf(ForceForMPa(ConcreteCompressiveMPa / 2.0)), UnitAreaSqCm, 0.5
		},
		{
			TEXT("compression at twice its limit gives"),
			CompressionOf(ForceForMPa(ConcreteCompressiveMPa * 2.0)), UnitAreaSqCm, 2.0
		},

		// Compression at 0.5, shear at its limit: the worst axis governs.
		{
			TEXT("the most utilised axis governs a combined load"),
			[&]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(ConcreteCompressiveMPa / 2.0);
				L.Shear = ForceForMPa(ConcreteShearMPa);
				return L;
			}(),
			UnitAreaSqCm, 1.0
		},

		// Stress-based, not force-based: the same force through half the area is twice the stress.
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
 * DESIGN.md's key check of the directional model: the same force is far more punishing in
 * shear than in compression, by exactly the strength ratio.
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

	const double Force = ForceForMPa(ConcreteShearMPa);

	const double CompressionUtilisation =
		DestructionForce::ComputeUtilisation(CompressionOf(Force), ConcreteUncoupled, UnitAreaSqCm);
	const double ShearUtilisation =
		DestructionForce::ComputeUtilisation(ShearOf(Force), ConcreteUncoupled, UnitAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("shear should be at its limit, got %f"), ShearUtilisation),
		FMath::IsNearlyEqual(ShearUtilisation, 1.0, 1e-9));

	TestTrue(
		FString::Printf(TEXT("the same force in compression should be well short of failure, got %f"),
			CompressionUtilisation),
		CompressionUtilisation < 0.5);

	const double ExpectedRatio = ConcreteCompressiveMPa / ConcreteShearMPa;
	const double ActualRatio = ShearUtilisation / FMath::Max(CompressionUtilisation, UE_DOUBLE_SMALL_NUMBER);

	TestTrue(
		FString::Printf(TEXT("shear should be %fx more punishing than compression, got %fx"),
			ExpectedRatio, ActualRatio),
		FMath::IsNearlyEqual(ActualRatio, ExpectedRatio, 1e-6));

	return true;
}

/**
 * Mohr-Coulomb friction coupling: shear capacity = cohesion + mu * compressive stress. A wall
 * loses shear capacity as the weight above is removed, and dry stone (no bond) stands only
 * on friction. Pure arithmetic.
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
		{
			TEXT("an uncompressed mortar joint gives at its bare cohesion"),
			GeneralPurposeMortar,
			ShearOf(ForceForMPa(MortarCohesionMPa)),
			1.0
		},

		/*
		 * Mean-basis mortar: 1 MPa of compression adds mu = 0.75, so capacity goes 0.90 -> 1.65.
		 * 1.65 is below the 2.0 cap (which bites from 1.467 MPa), so this measures friction.
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

		// Only compression helps; using |normal stress| would wrongly let tension add capacity.
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

		// Dry stone: zero cohesion, so 1 MPa of compression gives 0.7 MPa of capacity.
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

		// Twice the compression, half the utilisation.
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

	// Unloaded dry stone has zero shear capacity; asserted as "gives" since the ratio is unbounded.
	const double UnloadedDryStone =
		DestructionForce::ComputeUtilisation(ShearOf(ForceForMPa(0.01)), DryStone, UnitAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("unloaded dry stone should give under any shear, got %f"), UnloadedDryStone),
		UnloadedDryStone > 1.0);

	return true;
}

/**
 * Friction stops helping past a shear ceiling. Uncapped, joints at the base of a tall
 * building would become unbreakable in shear. Mortar's mean-basis ceiling is 2.0 MPa
 * (0.1 x f_b; Gooch et al. 2023, ConBuildMat 386:131578, measured a 1.81 MPa unconfined
 * mean), so with cohesion 0.90 and mu 0.75 the cap bites from 1.4667 MPa of compression.
 * The ceiling is read from the profile and the test compressions derive from the bite
 * point, so a retune moves them together.
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
	 * Pins for the mean-basis profile (DESIGN §3):
	 *  - cohesion 0.90: centre of measured regression intercepts 0.58-1.04 (Gooch et al. 2025,
	 *    ConBuildMat 489:142348).
	 *  - mu 0.75: centre of measured initial friction 0.64-1.00.
	 *  - cap 2.0 = 0.1 x f_b.
	 *  - compressive 10.0: EN 998-2 already declares a mean class, so no uplift.
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

	// Compression at which cohesion + mu * sigma reaches the ceiling.
	const double BiteMPa =
		(GeneralPurposeMortar.MaxShearStrengthMPa - GeneralPurposeMortar.ShearCohesionMPa)
		/ GeneralPurposeMortar.FrictionCoefficient;

	const double ShallowerCompressionMPa = BiteMPa + 1.0;
	const double DeeperCompressionMPa = BiteMPa + 2.0;

	// Past the bite, capacity is the ceiling, so shear at the ceiling sits exactly at the limit.
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
	 * Past the cap, extra compression buys nothing. The deeper compression must still be below
	 * crushing, or the compression axis would govern instead of the cap.
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

		// Equality alone would pass two identically wrong numbers.
		TestTrue(
			FString::Printf(TEXT("capped shear utilisation should be 1.0/%f, expected %f, got %f"),
				MortarMaxShearMPa, 1.0 / MortarMaxShearMPa, ShallowerUtilisation),
			FMath::IsNearlyEqual(ShallowerUtilisation, 1.0 / MortarMaxShearMPa, Tolerance));
	}

	// Below the cap friction applies in full; guards a clamp applied everywhere.
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
 * Property test over area x load x profile: utilisation is never NaN or infinite, and a
 * degenerate joint reads as failed. NaN compares false, so `Utilisation > 1.0` on a NaN would
 * report the joint intact.
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

		// Chaos can produce NaN/infinite forces in a pathological contact; the joint must not read intact.
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

	// Dry stone has real zeroes in two strengths, so it reaches the degenerate paths legitimately.
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
					// Fail closed: no interface or a malformed load must not read intact.
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
 * An off-centre load reports the stress at the outermost fibre, not the face average. A brick
 * hanging off one head joint has zero mean normal stress and reads 0.0044 in shear, but its
 * weight acts 10.75 cm out and levers the joint open, about 13x closer to failure.
 *
 *     sigma_n = (Tension - Compression) / A        signed, positive in tension
 *     sigma_b = |M_u|/W_u + |M_v|/W_v              worst corner, biaxial
 *     peak tension     = max(0, sigma_n + sigma_b)
 *     peak compression = max(0, sigma_b - sigma_n)
 *
 * No new conversion boundary: M/W is uu.cm / cm3 = uu/cm2, the same as force over area, so it
 * divides by the same 10000. Expectations use published strengths only, so a 100x error shows.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthEdgeStressTest,
	"DestructionGame.Core.ConnectionStrength.EdgeStressUnderAMoment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthEdgeStressTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionStrengthTestSupport;

	/*
	 * Brick weight = 2.72163125 kg * 980 = 2667.198625 uu.
	 *
	 * (b) One head joint: A = 66.625 cm2, W_u = (4/3) * 5.125 * 3.25^2 = 72.1770833 cm3,
	 *     M_u = 2667.198625 * 10.75 = 28672.38521875 uu.cm.
	 *       sigma_n = 0 (gravity is parallel to the joint)
	 *       sigma_b = 397.250538 uu/cm2 = 0.0397250538 MPa
	 *       tension = 0.0397250538 / 0.7 (mean f_x1) = 0.0567500769231
	 *
	 * (a) Corbelled half its length: A = 110.1875 cm2, W = 197.4192708 cm3,
	 *     M = 2667.198625 * 5.375 = 14336.192609375 uu.cm.
	 *       sigma_n = -24.206 uu/cm2, sigma_b = 72.618 uu/cm2 (sigma_b / |sigma_n| = 6e/t = 3)
	 *       tension = (72.618 - 24.206) / 10000 / 0.7 = 0.006916
	 */
	constexpr double HangingBrickUtilisation = 0.0567500769231;
	constexpr double CorbelledBrickUtilisation = 0.006916;

	// The same two joints with the load path treated as centred.
	constexpr double HangingBrickShearUtilisationToday = 0.0040033 / 0.9;
	constexpr double CorbelledBrickCompressionUtilisationToday = 0.00024206;

	/*
	 * ComputeUtilisation returns the worst axis, so this measures bending only while tension
	 * governs. Case (b) against mortar's 10 / 0.9 / 0.7:
	 *
	 *   tension      0.0397250538 MPa / 0.7  = 0.0567501    <- governs
	 *   shear        0.0040033    MPa / 0.9  = 0.0044481
	 *   compression  0.0397250538 MPa / 10   = 0.0039725
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
	 * Pin the profile the expectations were derived against, so a retune fails here. f_x1 =
	 * 0.70 MPa is the mean flexural bond (DESIGN §3; Gooch et al. 2023, ConBuildMat 386:131578).
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
		// Zero mean normal stress: centred reads 0.0044, the top fibre reads 0.0568.
		{
			TEXT("(b) a brick hanging off one head joint peels rather than shears"),
			WithMomentU(ShearOf(BrickWeightUu), HeadJointMomentUuCm),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, HeadJointModulusVCm3),
			HangingBrickUtilisation
		},

		// The in-plane axes are symmetric; guards wiring up only the first modulus.
		{
			TEXT("(b) mirrored onto the other in-plane axis, same answer"),
			WithMomentV(ShearOf(BrickWeightUu), HeadJointMomentUuCm),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusVCm3, HeadJointModulusUCm3),
			HangingBrickUtilisation
		},

		// Biaxial bending adds at the worst corner; max() or root-sum-square would not match.
		{
			TEXT("(b) split across both axes, the worst corner adds them"),
			WithMomentV(
				WithMomentU(ShearOf(BrickWeightUu), HeadJointMomentUuCm / 2.0),
				HeadJointMomentUuCm / 2.0),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, HeadJointModulusUCm3),
			HangingBrickUtilisation
		},

		// The moment's sign picks which edge opens; only its magnitude reaches the stress.
		{
			TEXT("(b) leaning the opposite way is just as bad"),
			WithMomentU(ShearOf(BrickWeightUu), -HeadJointMomentUuCm),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, HeadJointModulusVCm3),
			HangingBrickUtilisation
		},

		// With no moment the joint reads the centred answer.
		{
			TEXT("(b) with the load path centred, the head joint reads today's shear answer"),
			ShearOf(BrickWeightUu),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, HeadJointModulusVCm3),
			HangingBrickShearUtilisationToday
		},

		/*
		 * The corbel has nonzero sigma_n, so it pins the subtraction in max(0, sigma_n + sigma_b):
		 * 72.618 - 24.206. Its second modulus is zero with zero moment (0/0), which must
		 * contribute nothing rather than NaN or a failed joint.
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
 * With no eccentricity a joint reads bit-for-bit what it read before moments existed, so
 * geometry-free fixtures stay valid. Exact equality, not a tolerance: the cascade fuzz has
 * joints settling at exactly 1.0, so one ulp of drift flips break decisions. Regression net.
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

		// Puts friction coupling in the loop.
		{
			TEXT("compression and shear together"),
			[]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(1.0);
				L.Shear = ForceForMPa(0.8);
				return L;
			}()
		},

		// Puts the shear cap in the loop.
		{
			TEXT("compression past the shear cap"),
			[]{
				FConnectionLoad L;
				L.Compression = ForceForMPa(4.0);
				L.Shear = ForceForMPa(1.0);
				return L;
			}()
		},

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

				// Real non-zero moduli, centred load: the bending term must vanish, not be skipped.
				const double WithGeometry = DestructionForce::ComputeUtilisation(
					Load.Load, Profile.Strength,
					FJointSection(Area.AreaSqCm, HeadJointModulusUCm3, HeadJointModulusVCm3));

				TestTrue(
					FString::Printf(TEXT("%s: a centred load on a real section must be bit-identical, expected %.17g got %.17g"),
						*Context, Expected, WithGeometry),
					WithGeometry == Expected);

				// An area-only caller must reach the same answer.
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
 * A moment with no section to resist it fails closed; no moment at all does not. M/W on a
 * zero modulus is inf or NaN, and NaN would read intact. The branch tests the moment first:
 * a single-axis lean has 0/0 on the other axis, and testing the modulus first would fail
 * that healthy joint.
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
		/** Fails closed with the area guard's sentinel. */
		ExactlyMax,

		ExactlyZero,

		/** No bending term, so the pre-moment answer. */
		PreMomentAnswer,

		FailedButFinite,

		/** A real bending answer: loaded and holding, neither zero nor the sentinel. */
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

		// Without the guard this returns a plausible 0.02 from geometry never filled in.
		{
			TEXT("a moment with no modulus must not hide behind a plausible load"),
			WithMomentU(ShearOf(BrickWeightUu), MomentUuCm),
			FJointSection(HeadJointAreaSqCm, 0.0, 0.0),
			EExpectation::ExactlyMax
		},

		// No moduli is what every existing fixture supplies.
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
		// Moment about u only, no modulus about v: testing the modulus first would fail this healthy joint.
		{
			TEXT("no moment about v excuses having no modulus about v"),
			WithMomentU(FConnectionLoad(), MomentUuCm),
			FJointSection(HeadJointAreaSqCm, HeadJointModulusUCm3, 0.0),
			EExpectation::IntactUnderBendingAlone
		},

		// Chaos can produce NaN in a pathological contact.
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

		// The area guard still wins.
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

/**
 * A dry (no-tension) joint whose resultant is outside the kern but inside the face opens
 * partially and bears on the reduced contact. It fails only when the resultant leaves the face
 * (e >= h/2) or the contact stress reaches f_c. Without this, AxisUtilisation(peak tension,
 * f_t = 0) returns Max the moment e > h/6, breaking the shed's DryStone lintel bearings.
 *
 * Bed depth h, width W: kern = h/6, edge = h/2. Past the kern the compressed zone is a
 * triangle of length 3*(h/2 - e), so
 *
 *     sigma_max = 2*sigma_mean*h / (3*(h/2 - e))       equals the linear value at e = h/6
 *     e_crush   = h * (1/2 - (2/3)*(sigma_mean/f_c))
 *
 * Fixture: h = 20, W = 10, sigma_mean = 3 MPa, DryStone f_c = 30, so e_crush = 8.6667.
 *
 *     e = 2.5   inside kern        5.25/30 = 0.175     stands
 *     e = 5.0   cracked, stands    8.0/30 = 0.26667    stands (the linear model would say 0.25)
 *     e = 9.0   cracked, crushes   40/30 = 1.33333     fails, finite
 *     e = 10.5  off the face       >= 1                fails
 *
 * Tension and shear are zero on every row, so compression governs.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthDryEccentricContactTest,
	"DestructionGame.Core.ConnectionStrength.DryJointBearsOnReducedContactPastTheKern",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthDryEccentricContactTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionStrengthTestSupport;
	using namespace DestructionProfiles;

	// 10000 uu per MPa per cm2, spelled out so a wrong production constant fails here.
	constexpr double UuPerMPaPerSqCm = 100.0 * 100.0;

	constexpr double DepthHCm = 20.0;      // the bed depth in the bending direction
	constexpr double WidthWCm = 10.0;      // out-of-plane width
	constexpr double AreaSqCm = WidthWCm * DepthHCm;                     // 200
	constexpr double ModulusUCm3 = WidthWCm * DepthHCm * DepthHCm / 6.0; // 666.6667
	constexpr double MeanCompMPa = 3.0;
	constexpr double CompressionForceUu = MeanCompMPa * UuPerMPaPerSqCm * AreaSqCm; // 6e6

	constexpr double KernCm = DepthHCm / 6.0;   // 3.3333
	constexpr double EdgeCm = DepthHCm / 2.0;   // 10.0
	const double CrushCm =
		DepthHCm * (0.5 - (2.0 / 3.0) * (MeanCompMPa / DryStone.CompressiveStrengthMPa)); // 8.6667

	TestTrue(TEXT("FIXTURE: DryStone is a true no-tension joint — f_t = 0 and cohesion = 0"),
		DryStone.TensileStrengthMPa == 0.0 && DryStone.ShearCohesionMPa == 0.0);
	TestTrue(TEXT("FIXTURE: derived against DryStone f_c = 30 MPa, profile carries it"),
		DryStone.CompressiveStrengthMPa == 30.0);
	TestTrue(TEXT("FIXTURE: the section recovers h = 6 S / A = 20 cm"),
		FMath::IsNearlyEqual(6.0 * ModulusUCm3 / AreaSqCm, DepthHCm, 1e-9));
	TestTrue(FString::Printf(
		TEXT("FIXTURE: the four eccentricities must ladder across the regimes: "
			 "kern %.4f < stands 5 < crush %.4f < crushes 9 < edge %.4f"),
			KernCm, CrushCm, EdgeCm),
		KernCm < 5.0 && 5.0 < CrushCm && CrushCm < 9.0 && 9.0 < EdgeCm);

	enum class ERegime
	{
		InsideKernStands,   // finite, < 1, exact
		CrackedStands,      // finite, < 1, exact
		CrackedCrushes,     // finite, >= 1, exact
		OffFaceFails        // >= 1
	};

	struct FCase
	{
		const TCHAR* Description;
		double EccentricityCm;
		double ExpectedUtilisation;
		ERegime Regime;
	};

	const TArray<FCase> Cases = {
		{
			TEXT("e = h/8, inside the kern: the whole bed bears, linear compression 5.25/30"),
			2.5, 0.175, ERegime::InsideKernStands
		},
		{
			TEXT("e = h/4, outside the kern: the bed opens and bears on reduced contact, 8.0/30 — NOT the linear 0.25"),
			5.0, 8.0 / 30.0, ERegime::CrackedStands
		},
		{
			TEXT("e = 0.45h, reduced-contact compression crosses f_c: crushes at 40/30, still FINITE"),
			9.0, 40.0 / 30.0, ERegime::CrackedCrushes
		},
		{
			TEXT("e = 0.525h, resultant off the face: fails outright"),
			10.5, 0.0, ERegime::OffFaceFails
		},
	};

	constexpr double Tolerance = 1e-9;

	for (const FCase& Case : Cases)
	{
		const double MomentUuCm = CompressionForceUu * Case.EccentricityCm;
		const FConnectionLoad Load = WithMomentU(CompressionOf(CompressionForceUu), MomentUuCm);
		const FJointSection Section(AreaSqCm, ModulusUCm3, 0.0);

		const double Utilisation =
			DestructionForce::ComputeUtilisation(Load, DryStone, Section);

		switch (Case.Regime)
		{
		case ERegime::InsideKernStands:
		case ERegime::CrackedStands:
			TestTrue(
				FString::Printf(TEXT("%s: must be a real reading, not the failure sentinel (Max), got %g"),
					Case.Description, Utilisation),
				FMath::IsFinite(Utilisation) && Utilisation < TNumericLimits<double>::Max());
			TestTrue(
				FString::Printf(TEXT("%s: expected %.10f, got %.10f"),
					Case.Description, Case.ExpectedUtilisation, Utilisation),
				FMath::IsNearlyEqual(Utilisation, Case.ExpectedUtilisation, Tolerance));
			TestTrue(
				FString::Printf(TEXT("%s: bears on reduced contact, so it STANDS (util < 1), got %g"),
					Case.Description, Utilisation),
				Utilisation < 1.0);
			break;

		case ERegime::CrackedCrushes:
			TestTrue(
				FString::Printf(TEXT("%s: crushing is a real, bounded failure — not the failure sentinel (Max), got %g"),
					Case.Description, Utilisation),
				FMath::IsFinite(Utilisation) && Utilisation < TNumericLimits<double>::Max());
			TestTrue(
				FString::Printf(TEXT("%s: expected %.10f, got %.10f"),
					Case.Description, Case.ExpectedUtilisation, Utilisation),
				FMath::IsNearlyEqual(Utilisation, Case.ExpectedUtilisation, Tolerance));
			TestTrue(
				FString::Printf(TEXT("%s: peak compression past f_c, so it FAILS (util >= 1), got %g"),
					Case.Description, Utilisation),
				Utilisation >= 1.0);
			break;

		case ERegime::OffFaceFails:
			TestTrue(
				FString::Printf(TEXT("%s: the resultant has left the face, so the joint FAILS (util >= 1), got %g"),
					Case.Description, Utilisation),
				Utilisation >= 1.0);
			break;
		}
	}

	return true;
}

/**
 * Biaxial branch of the dry-joint reduced-contact rule. The shed is only weakly biaxial, so
 * it would not catch a dropped other-axis term. An axis is cracked when its own bending
 * stress exceeds the mean compression (sigma_b_i > |sigma_n|):
 *   - one axis cracked: PeakCompressive = sigma_c + sigma_b_other, compression governs.
 *   - both cracked: a cut corner the 1-D formula does not cover, so fail closed (Max).
 *   - neither alone cracked but sigma_bU + sigma_bV > |sigma_n|: also fail closed.
 *
 * Fixture: bed 10 x 20, A = 200, S_U = 666.6667, S_V = 333.3333, |sigma_n| = 3 MPa, f_c = 30.
 *   Row 1: sigma_bU 4.5, sigma_bV 1.5 -> sigma_c = 8.0, util = 9.5/30 (8.0/30 if the V term drops).
 *   Row 2: both 4.5 -> Max.
 *   Row 3: both 2.0 (sum 4 > 3) -> Max.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionStrengthDryBiaxialContactTest,
	"DestructionGame.Core.ConnectionStrength.DryJointBiaxialReducedContact",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionStrengthDryBiaxialContactTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionStrengthTestSupport;
	using namespace DestructionProfiles;

	// 10000 uu per MPa per cm2, spelled out so a wrong production constant fails here.
	constexpr double UuPerMPaPerSqCm = 100.0 * 100.0;

	constexpr double DepthHUCm = 20.0;   // U-bending depth
	constexpr double WidthWVCm = 10.0;   // the other dimension
	constexpr double AreaSqCm = WidthWVCm * DepthHUCm;                        // 200
	constexpr double ModulusUCm3 = WidthWVCm * DepthHUCm * DepthHUCm / 6.0;   // 666.6667
	constexpr double ModulusVCm3 = DepthHUCm * WidthWVCm * WidthWVCm / 6.0;   // 333.3333
	constexpr double DepthHVCm = 6.0 * ModulusVCm3 / AreaSqCm;                // 10

	constexpr double MeanCompMPa = 3.0;
	constexpr double CompressionForceUu = MeanCompMPa * UuPerMPaPerSqCm * AreaSqCm; // 6e6

	TestTrue(TEXT("FIXTURE: DryStone is a true no-tension joint — f_t = 0 and cohesion = 0"),
		DryStone.TensileStrengthMPa == 0.0 && DryStone.ShearCohesionMPa == 0.0);
	TestTrue(TEXT("FIXTURE: derived against DryStone f_c = 30 MPa, profile carries it"),
		DryStone.CompressiveStrengthMPa == 30.0);
	TestTrue(TEXT("FIXTURE: the U section recovers h_U = 6 S_U / A = 20 cm"),
		FMath::IsNearlyEqual(6.0 * ModulusUCm3 / AreaSqCm, DepthHUCm, 1e-9));
	TestTrue(TEXT("FIXTURE: the V section recovers h_V = 6 S_V / A = 10 cm"),
		FMath::IsNearlyEqual(DepthHVCm, 10.0, 1e-9));

	// Moment (uu.cm) that loads modulus S to a target bending stress: M = sigma_b * S * conv.
	auto MomentForBendingStress = [&](double BendingStressMPa, double ModulusCm3)
	{
		return BendingStressMPa * ModulusCm3 * UuPerMPaPerSqCm;
	};

	// Reduced-contact stress on a cracked axis, MPa: 2*|sigma_n|*h / (3*(h/2 - e)), e = sigma_b*h / (6*|sigma_n|).
	auto ReducedContactStress = [&](double CrackedBendingMPa, double DepthCm)
	{
		const double Ecc = CrackedBendingMPa * DepthCm / (6.0 * MeanCompMPa);
		return 2.0 * MeanCompMPa * DepthCm / (3.0 * (0.5 * DepthCm - Ecc));
	};

	// Row 1 oracle: reduced contact on U (sigma_bU = 4.5) plus within-kern V (1.5).
	const double Row1Sigma_c = ReducedContactStress(4.5, DepthHUCm);          // 8.0
	const double Row1Util = (Row1Sigma_c + 1.5) / DryStone.CompressiveStrengthMPa; // 9.5/30

	TestTrue(FString::Printf(TEXT("FIXTURE: row 1 reduced-contact stress is 8.0 MPa, derived %g"), Row1Sigma_c),
		FMath::IsNearlyEqual(Row1Sigma_c, 8.0, 1e-9));

	enum class ERegime
	{
		OneCrackedRelieves,   // finite, < 1, exact — compression governs
		FailsClosed           // exactly the Max sentinel — the tension axis governs
	};

	struct FCase
	{
		const TCHAR* Description;
		double BendingUMPa;
		double BendingVMPa;
		double ExpectedUtilisation;   // meaningful only for OneCrackedRelieves
		ERegime Regime;
	};

	const TArray<FCase> Cases = {
		{
			TEXT("ROW 1 one-cracked (U 4.5>3, V 1.5<3): reduced contact on U plus within-kern V bending, 9.5/30"),
			4.5, 1.5, Row1Util, ERegime::OneCrackedRelieves
		},
		{
			TEXT("ROW 2 both-cracked (U 4.5>3, V 4.5>3): a cut corner, the 1-D block does not apply — fail-closed"),
			4.5, 4.5, 0.0, ERegime::FailsClosed
		},
		{
			TEXT("ROW 3 neither-cracked but summed corner in tension (U 2<3, V 2<3, sum 4>3): fail-closed"),
			2.0, 2.0, 0.0, ERegime::FailsClosed
		},
	};

	constexpr double Tolerance = 1e-9;

	for (const FCase& Case : Cases)
	{
		FConnectionLoad Load = CompressionOf(CompressionForceUu);
		Load.BendingMomentUUuCm = MomentForBendingStress(Case.BendingUMPa, ModulusUCm3);
		Load.BendingMomentVUuCm = MomentForBendingStress(Case.BendingVMPa, ModulusVCm3);

		const FJointSection Section(AreaSqCm, ModulusUCm3, ModulusVCm3);

		const double Utilisation =
			DestructionForce::ComputeUtilisation(Load, DryStone, Section);

		switch (Case.Regime)
		{
		case ERegime::OneCrackedRelieves:
			// Tension and shear are zero here, so a finite reading can only be compression.
			TestTrue(
				FString::Printf(TEXT("%s: must be a real compression reading, not the failure sentinel (Max), got %g"),
					Case.Description, Utilisation),
				FMath::IsFinite(Utilisation) && Utilisation < TNumericLimits<double>::Max());
			TestTrue(
				FString::Printf(TEXT("%s: expected %.10f, got %.10f"),
					Case.Description, Case.ExpectedUtilisation, Utilisation),
				FMath::IsNearlyEqual(Utilisation, Case.ExpectedUtilisation, Tolerance));
			TestTrue(
				FString::Printf(TEXT("%s: bears on reduced contact, so it STANDS (util < 1), got %g"),
					Case.Description, Utilisation),
				Utilisation < 1.0);
			break;

		case ERegime::FailsClosed:
			// Only the tension axis against f_t = 0 yields exactly Max; compression would be finite.
			TestEqual(
				FString::Printf(TEXT("%s: must fail closed at exactly the Max sentinel, got %g"),
					Case.Description, Utilisation),
				Utilisation, TNumericLimits<double>::Max());
			break;
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
