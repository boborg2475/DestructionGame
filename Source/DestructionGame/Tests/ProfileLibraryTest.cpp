// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Connection.h"
#include "Core/ConnectionStrength.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The shared connection and material profile library.
 *
 * Asserts physical relations (compression dominates a bonded joint, dry stone has no
 * bond, a fastener's mu is exactly zero, a shear cap is never below cohesion), not the
 * table's numbers, so retuning stays free. Well-formedness is swept over every row, and
 * behaviour is checked through ComputeUtilisation and FConnection. Pure arithmetic, no world.
 *
 * Named namespace, not anonymous: anonymous helpers collide under unity builds
 * (CURRENT_STATE.md).
 */
namespace ProfileLibraryTestSupport
{
	using namespace DestructionProfiles;

	/** A 10 cm x 10 cm interface. */
	constexpr double JointAreaSqCm = 100.0;

	/**
	 * Force in uu that loads the area to the given stress. Spelled out rather than using
	 * ForceUnitsPerMPaSqCm so a wrong constant fails here: 1 N = 100 uu, 1 cm2 = 100 mm2,
	 * so 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	FConnectionLoad CompressionOf(double Force) { FConnectionLoad L; L.Compression = Force; return L; }
	FConnectionLoad TensionOf(double Force) { FConnectionLoad L; L.Tension = Force; return L; }
	FConnectionLoad ShearOf(double Force) { FConnectionLoad L; L.Shear = Force; return L; }

	/** Worst utilisation with the stress applied on each axis in turn. */
	double WorstUtilisationAtStress(const FConnectionStrength& Strength, double StressMPa)
	{
		const double Force = ForceForMPa(StressMPa, JointAreaSqCm);

		return FMath::Max3(
			DestructionForce::ComputeUtilisation(CompressionOf(Force), Strength, JointAreaSqCm),
			DestructionForce::ComputeUtilisation(ShearOf(Force), Strength, JointAreaSqCm),
			DestructionForce::ComputeUtilisation(TensionOf(Force), Strength, JointAreaSqCm));
	}

	const TCHAR* ClassName(EConnectionProfileClass Class)
	{
		switch (Class)
		{
			case EConnectionProfileClass::Bonded:             return TEXT("Bonded");
			case EConnectionProfileClass::Frictional:         return TEXT("Frictional");
			case EConnectionProfileClass::MechanicalFastener: return TEXT("MechanicalFastener");
			case EConnectionProfileClass::TestFixture:        return TEXT("TestFixture");
		}
		return TEXT("<unknown>");
	}

	const FNamedConnectionProfile* FindConnectionProfile(const TCHAR* Name)
	{
		for (const FNamedConnectionProfile& Row : AllConnectionProfiles())
		{
			if (Row.Name != nullptr && FCString::Strcmp(Row.Name, Name) == 0)
			{
				return &Row;
			}
		}
		return nullptr;
	}

	bool StrengthsMatch(const FConnectionStrength& A, const FConnectionStrength& B)
	{
		return A.CompressiveStrengthMPa == B.CompressiveStrengthMPa
			&& A.ShearCohesionMPa == B.ShearCohesionMPa
			&& A.TensileStrengthMPa == B.TensileStrengthMPa
			&& A.FrictionCoefficient == B.FrictionCoefficient
			&& A.MaxShearStrengthMPa == B.MaxShearStrengthMPa;
	}

	/** A standard UK metric brick, 215 x 102.5 x 65 mm. The material owns only density. */
	constexpr double BrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/**
	 * A brick's real weight, kg: the suite's only external anchor for it. Other tests derive
	 * brick mass from ClayBrick's density; do not add a second literal.
	 */
	constexpr double HandSetBrickMassKg = 2.72;
}

/**
 * Every connection profile is well-formed and obeys its class's invariants. A sweep, so a
 * new row inherits every check.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileLibraryConnectionInvariantsTest,
	"DestructionGame.Core.Profiles.ConnectionInvariants",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FProfileLibraryConnectionInvariantsTest::RunTest(const FString& Parameters)
{
	using namespace ProfileLibraryTestSupport;

	// Must contain at least these, as classified. Not an exact count.
	struct FExpectedProfile
	{
		const TCHAR* Name;
		EConnectionProfileClass Class;
		const FConnectionStrength* Named;
	};

	const TArray<FExpectedProfile> Expected = {
		{ TEXT("GeneralPurposeMortar"), EConnectionProfileClass::Bonded,             &GeneralPurposeMortar },
		{ TEXT("LimeMortar"),           EConnectionProfileClass::Bonded,             &LimeMortar },
		{ TEXT("DryStone"),             EConnectionProfileClass::Frictional,         &DryStone },
		{ TEXT("Nail"),                 EConnectionProfileClass::MechanicalFastener, &Nail },
		{ TEXT("Screw"),                EConnectionProfileClass::MechanicalFastener, &Screw },
		{ TEXT("Bolt"),                 EConnectionProfileClass::MechanicalFastener, &Bolt },
		{ TEXT("Unbreakable"),          EConnectionProfileClass::TestFixture,        &Unbreakable },
	};

	for (const FExpectedProfile& Want : Expected)
	{
		const FNamedConnectionProfile* Row = FindConnectionProfile(Want.Name);

		if (Row == nullptr)
		{
			AddError(FString::Printf(
				TEXT("the library must contain a profile named %s; it does not (library has %d entries)"),
				Want.Name, AllConnectionProfiles().Num()));
			continue;
		}

		TestTrue(
			FString::Printf(TEXT("%s should be classified %s, got %s"),
				Want.Name, ClassName(Want.Class), ClassName(Row->Class)),
			Row->Class == Want.Class);

		// The named constant and its row must not drift apart.
		TestTrue(
			FString::Printf(TEXT("the named constant %s and its library row must be the same data"), Want.Name),
			StrengthsMatch(*Want.Named, Row->Strength));
	}

	// Any real joint gives at this; only a fixture may survive it.
	constexpr double AbsurdStressMPa = 1.0e6;

	for (const FNamedConnectionProfile& Row : AllConnectionProfiles())
	{
		const FConnectionStrength& S = Row.Strength;
		const FString Where = FString::Printf(TEXT("%s (%s)"), Row.Name, ClassName(Row.Class));

		// Well-formedness, every profile.
		TestTrue(*FString::Printf(TEXT("%s: compressive strength must be finite, got %g"), *Where, S.CompressiveStrengthMPa),
			FMath::IsFinite(S.CompressiveStrengthMPa));
		TestTrue(*FString::Printf(TEXT("%s: shear cohesion must be finite, got %g"), *Where, S.ShearCohesionMPa),
			FMath::IsFinite(S.ShearCohesionMPa));
		TestTrue(*FString::Printf(TEXT("%s: tensile strength must be finite, got %g"), *Where, S.TensileStrengthMPa),
			FMath::IsFinite(S.TensileStrengthMPa));
		TestTrue(*FString::Printf(TEXT("%s: friction coefficient must be finite, got %g"), *Where, S.FrictionCoefficient),
			FMath::IsFinite(S.FrictionCoefficient));
		TestTrue(*FString::Printf(TEXT("%s: max shear must be finite, got %g"), *Where, S.MaxShearStrengthMPa),
			FMath::IsFinite(S.MaxShearStrengthMPa));

		// Zero is valid for cohesion and tension (dry stone), never for compression.
		TestTrue(*FString::Printf(TEXT("%s: compressive strength must be positive, got %g"), *Where, S.CompressiveStrengthMPa),
			S.CompressiveStrengthMPa > 0.0);

		TestTrue(*FString::Printf(TEXT("%s: shear cohesion must not be negative, got %g"), *Where, S.ShearCohesionMPa),
			S.ShearCohesionMPa >= 0.0);
		TestTrue(*FString::Printf(TEXT("%s: tensile strength must not be negative, got %g"), *Where, S.TensileStrengthMPa),
			S.TensileStrengthMPa >= 0.0);

		// mu above ~1.5 is a typo; masonry testing gives 0.6-0.8.
		TestTrue(*FString::Printf(TEXT("%s: friction coefficient must be within [0, 1.5], got %g"), *Where, S.FrictionCoefficient),
			S.FrictionCoefficient >= 0.0 && S.FrictionCoefficient <= 1.5);

		// Capacity is min(cohesion + mu*sigma, cap), so a cap below cohesion silently weakens the bond.
		TestTrue(
			*FString::Printf(TEXT("%s: max shear %g must not be below cohesion %g — the cap would silently weaken the bond"),
				*Where, S.MaxShearStrengthMPa, S.ShearCohesionMPa),
			S.MaxShearStrengthMPa >= S.ShearCohesionMPa);

		// Per-class physics.
		switch (Row.Class)
		{
			case EConnectionProfileClass::Bonded:
			{
				/*
				 * Mortar joints are compression members: crushing strength far exceeds shear
				 * bond, which exceeds tension. 5x, not 10x, since the 2026-08-14 mean re-anchor
				 * (lime is 2.0 / 0.27 = 7.4x).
				 */
				TestTrue(
					*FString::Printf(TEXT("%s: compressive %g should be at least 5x cohesion %g"),
						*Where, S.CompressiveStrengthMPa, S.ShearCohesionMPa),
					S.CompressiveStrengthMPa >= 5.0 * S.ShearCohesionMPa);

				TestTrue(
					*FString::Printf(TEXT("%s: cohesion %g should exceed tensile %g — mortar debonds in tension first"),
						*Where, S.ShearCohesionMPa, S.TensileStrengthMPa),
					S.ShearCohesionMPa > S.TensileStrengthMPa);

				TestTrue(
					*FString::Printf(TEXT("%s: a bonded joint must have real cohesion, got %g"), *Where, S.ShearCohesionMPa),
					S.ShearCohesionMPa > 0.0);
				TestTrue(
					*FString::Printf(TEXT("%s: a bonded joint must have real tensile bond, got %g"), *Where, S.TensileStrengthMPa),
					S.TensileStrengthMPa > 0.0);
				TestTrue(
					*FString::Printf(TEXT("%s: a bonded joint must have real friction, got %g"), *Where, S.FrictionCoefficient),
					S.FrictionCoefficient > 0.0);

				// Uncapped, the base of a tall wall would be effectively uncuttable.
				TestTrue(
					*FString::Printf(TEXT("%s: a friction-coupled joint needs a real shear ceiling, got %g"),
						*Where, S.MaxShearStrengthMPa),
					S.MaxShearStrengthMPa < TNumericLimits<double>::Max());
				break;
			}

			case EConnectionProfileClass::Frictional:
			{
				// Dry stone has no mortar: cohesion and tension are exactly zero.
				TestTrue(
					*FString::Printf(TEXT("%s: a frictional joint has NO bond, cohesion must be exactly 0, got %g"),
						*Where, S.ShearCohesionMPa),
					S.ShearCohesionMPa == 0.0);
				TestTrue(
					*FString::Printf(TEXT("%s: a frictional joint has NO bond, tensile must be exactly 0, got %g"),
						*Where, S.TensileStrengthMPa),
					S.TensileStrengthMPa == 0.0);

				TestTrue(
					*FString::Printf(TEXT("%s: a frictional joint carries shear by friction alone, mu must be positive, got %g"),
						*Where, S.FrictionCoefficient),
					S.FrictionCoefficient > 0.0);

				TestTrue(
					*FString::Printf(TEXT("%s: a friction-coupled joint needs a real shear ceiling, got %g"),
						*Where, S.MaxShearStrengthMPa),
					S.MaxShearStrengthMPa < TNumericLimits<double>::Max());
				break;
			}

			case EConnectionProfileClass::MechanicalFastener:
			{
				// Exactly zero: mu = 0 makes Mohr-Coulomb three independent axes.
				TestTrue(
					*FString::Printf(TEXT("%s: a mechanical fastener must have mu exactly 0, got %g"),
						*Where, S.FrictionCoefficient),
					S.FrictionCoefficient == 0.0);

				TestTrue(
					*FString::Printf(TEXT("%s: a fastener must resist shear, got cohesion %g"), *Where, S.ShearCohesionMPa),
					S.ShearCohesionMPa > 0.0);
				TestTrue(
					*FString::Printf(TEXT("%s: a fastener must resist withdrawal, got tensile %g"), *Where, S.TensileStrengthMPa),
					S.TensileStrengthMPa > 0.0);
				break;
			}

			case EConnectionProfileClass::TestFixture:
				break;
		}

		// Only a fixture may be unbreakable.

		const double Utilisation = WorstUtilisationAtStress(S, AbsurdStressMPa);

		TestFalse(*FString::Printf(TEXT("%s: utilisation must never be NaN, got %g"), *Where, Utilisation),
			FMath::IsNaN(Utilisation));

		if (Row.Class == EConnectionProfileClass::TestFixture)
		{
			/*
			 * Fixtures are exempt either way. `Unbreakable` is checked by name below;
			 * `CohesionlessBond` is a deliberately breakable fixture.
			 */
		}
		else
		{
			// Nothing shippable may be indestructible.
			TestTrue(
				*FString::Printf(TEXT("%s: a real joint must give at %g MPa, got utilisation %g"),
					*Where, AbsurdStressMPa, Utilisation),
				Utilisation > 1.0);
		}
	}

	// `Unbreakable` must survive, so routing tests measure routing, not breaks.
	TestTrue(
		FString::Printf(
			TEXT("Unbreakable must survive %g MPa on every axis, it reads %g"),
			AbsurdStressMPa, WorstUtilisationAtStress(Unbreakable, AbsurdStressMPa)),
		WorstUtilisationAtStress(Unbreakable, AbsurdStressMPa) <= 1.0);

	// DESIGN.md §2: nail < screw < bolt. Ordinal only, so magnitudes stay tunable.
	TestTrue(
		FString::Printf(TEXT("nail withdrawal %g < screw %g < bolt %g"),
			Nail.TensileStrengthMPa, Screw.TensileStrengthMPa, Bolt.TensileStrengthMPa),
		Nail.TensileStrengthMPa < Screw.TensileStrengthMPa
		&& Screw.TensileStrengthMPa < Bolt.TensileStrengthMPa);

	TestTrue(
		FString::Printf(TEXT("nail shear %g < screw %g < bolt %g"),
			Nail.ShearCohesionMPa, Screw.ShearCohesionMPa, Bolt.ShearCohesionMPa),
		Nail.ShearCohesionMPa < Screw.ShearCohesionMPa
		&& Screw.ShearCohesionMPa < Bolt.ShearCohesionMPa);

	// Lime mortar is weaker than cement mortar.
	TestTrue(
		FString::Printf(TEXT("lime mortar must be weaker in compression than general-purpose: %g vs %g"),
			LimeMortar.CompressiveStrengthMPa, GeneralPurposeMortar.CompressiveStrengthMPa),
		LimeMortar.CompressiveStrengthMPa < GeneralPurposeMortar.CompressiveStrengthMPa);

	TestTrue(
		FString::Printf(TEXT("lime mortar must have weaker bond than general-purpose: cohesion %g vs %g"),
			LimeMortar.ShearCohesionMPa, GeneralPurposeMortar.ShearCohesionMPa),
		LimeMortar.ShearCohesionMPa < GeneralPurposeMortar.ShearCohesionMPa);

	return true;
}

/** Every material profile is well-formed, and a clay brick weighs what a real brick weighs. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileLibraryMaterialInvariantsTest,
	"DestructionGame.Core.Profiles.MaterialInvariants",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FProfileLibraryMaterialInvariantsTest::RunTest(const FString& Parameters)
{
	using namespace ProfileLibraryTestSupport;

	TestTrue(
		FString::Printf(TEXT("the material library must not be empty, got %d entries"), AllMaterialProfiles().Num()),
		AllMaterialProfiles().Num() >= 2);

	for (const FNamedMaterialProfile& Row : AllMaterialProfiles())
	{
		const FMaterialProfile& M = Row.Profile;
		const FConnectionStrength& S = M.Strength;
		const FString Where = Row.Name != nullptr ? FString(Row.Name) : FString(TEXT("<unnamed>"));

		TestTrue(*FString::Printf(TEXT("%s: a material profile must be named"), *Where), Row.Name != nullptr);

		TestTrue(*FString::Printf(TEXT("%s: density must be finite, got %g"), *Where, M.DensityGramsPerCubicCm),
			FMath::IsFinite(M.DensityGramsPerCubicCm));

		/*
		 * Units trap: density is g/cm3 (UPhysicalMaterial::Density). A brick in kg/m3 is 1900,
		 * 1000x too heavy. Balsa is 0.16, osmium 22.6.
		 */
		TestTrue(
			*FString::Printf(TEXT("%s: density %g g/cm3 is outside the plausible range (0.05, 23) — kg/m3 by mistake?"),
				*Where, M.DensityGramsPerCubicCm),
			M.DensityGramsPerCubicCm > 0.05 && M.DensityGramsPerCubicCm < 23.0);

		TestTrue(*FString::Printf(TEXT("%s: compressive strength must be finite and positive, got %g"),
				*Where, S.CompressiveStrengthMPa),
			FMath::IsFinite(S.CompressiveStrengthMPa) && S.CompressiveStrengthMPa > 0.0);
		TestTrue(*FString::Printf(TEXT("%s: shear strength must be finite and positive, got %g"),
				*Where, S.ShearCohesionMPa),
			FMath::IsFinite(S.ShearCohesionMPa) && S.ShearCohesionMPa > 0.0);
		TestTrue(*FString::Printf(TEXT("%s: tensile strength must be finite and positive, got %g"),
				*Where, S.TensileStrengthMPa),
			FMath::IsFinite(S.TensileStrengthMPa) && S.TensileStrengthMPa > 0.0);

		// A cap below cohesion silently weakens the material.
		TestTrue(
			*FString::Printf(TEXT("%s: max shear %g must not be below shear strength %g"),
				*Where, S.MaxShearStrengthMPa, S.ShearCohesionMPa),
			S.MaxShearStrengthMPa >= S.ShearCohesionMPa);

		/*
		 * Masonry and concrete crush at 5x+ their tensile strength. Scoped to
		 * bCompressionDominant because timber is tension-capable along the grain (C24 is
		 * 21 vs 14 MPa characteristic).
		 */
		if (M.bCompressionDominant)
		{
			TestTrue(
				*FString::Printf(TEXT("%s: compressive %g should be at least 5x tensile %g"),
					*Where, S.CompressiveStrengthMPa, S.TensileStrengthMPa),
				S.CompressiveStrengthMPa >= 5.0 * S.TensileStrengthMPa);
		}

		// Unused, but a bond factor is a derating, so it lies in (0, 1].
		TestTrue(
			*FString::Printf(TEXT("%s: bond factor %g must lie in (0, 1]"), *Where, M.BondFactor),
			FMath::IsFinite(M.BondFactor) && M.BondFactor > 0.0 && M.BondFactor <= 1.0);
	}

	/*
	 * A standard brick at ClayBrick's density must weigh 2.72 kg, within 3%. The suite's
	 * brick masses derive from this density, so this is their external anchor.
	 */
	const double DerivedBrickMassKg = ClayBrick.DensityGramsPerCubicCm * BrickVolumeCubicCm / 1000.0;

	TestTrue(
		FString::Printf(
			TEXT("a standard brick of %g cm3 at %g g/cm3 weighs %g kg; the structure tests assume %g kg"),
			BrickVolumeCubicCm, ClayBrick.DensityGramsPerCubicCm, DerivedBrickMassKg, HandSetBrickMassKg),
		FMath::IsNearlyEqual(DerivedBrickMassKg, HandSetBrickMassKg, 0.03 * HandSetBrickMassKg));

	// Fired clay is porous; concrete is denser.
	TestTrue(
		FString::Printf(TEXT("concrete %g g/cm3 should be denser than fired clay brick %g g/cm3"),
			StructuralConcrete.DensityGramsPerCubicCm, ClayBrick.DensityGramsPerCubicCm),
		StructuralConcrete.DensityGramsPerCubicCm > ClayBrick.DensityGramsPerCubicCm);

	// DESIGN.md §2: mortar gives before the brick. Data ordering only.
	TestTrue(
		FString::Printf(TEXT("mortar must crush before the brick: %g MPa vs %g MPa"),
			GeneralPurposeMortar.CompressiveStrengthMPa, ClayBrick.Strength.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa < ClayBrick.Strength.CompressiveStrengthMPa);

	TestTrue(
		FString::Printf(TEXT("mortar must debond before the brick splits: tensile %g MPa vs %g MPa"),
			GeneralPurposeMortar.TensileStrengthMPa, ClayBrick.Strength.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa < ClayBrick.Strength.TensileStrengthMPa);

	/*
	 * Past the shear cap the brick gives, so mortar's cap tracks the brick. Mean basis uses
	 * 0.1 f_b, not EC6's characteristic 0.065: measured mean shear bond reaches 1.81 MPa,
	 * above 0.065 x 20 = 1.3. Grade C judgement, hence the loose tolerance.
	 */
	constexpr double MeanBasisShearCapCoefficient = 0.1;
	const double MeanBasisCapMPa = MeanBasisShearCapCoefficient * ClayBrick.Strength.CompressiveStrengthMPa;

	TestTrue(
		FString::Printf(
			TEXT("mortar's shear cap %g MPa should be ~0.1 x brick compressive %g MPa = %g MPa"),
			GeneralPurposeMortar.MaxShearStrengthMPa, ClayBrick.Strength.CompressiveStrengthMPa, MeanBasisCapMPa),
		FMath::IsNearlyEqual(GeneralPurposeMortar.MaxShearStrengthMPa, MeanBasisCapMPa, 0.15 * MeanBasisCapMPa));

	return true;
}

/**
 * Dry stone through the real code path: any tension parts it, and its shear capacity is
 * all friction, so it vanishes with the normal load. Loads are multiples of the profile's
 * own numbers, so retuning leaves the test valid.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileLibraryDryStoneBehaviourTest,
	"DestructionGame.Core.Profiles.DryStoneCarriesLoadByFrictionAlone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FProfileLibraryDryStoneBehaviourTest::RunTest(const FString& Parameters)
{
	using namespace ProfileLibraryTestSupport;

	constexpr double Tolerance = 1e-9;

	/*
	 * ComputeUtilisation returns the worst axis, so shear must govern: compression at 0.2
	 * of crushing, shear at 0.5 of capacity.
	 */

	const double CompressionStressMPa = 0.2 * DryStone.CompressiveStrengthMPa;
	const double FrictionCapacityMPa = DryStone.FrictionCoefficient * CompressionStressMPa;

	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: friction capacity %g must stay below the cap %g, or the cap governs"),
			FrictionCapacityMPa, DryStone.MaxShearStrengthMPa),
		FrictionCapacityMPa < DryStone.MaxShearStrengthMPa);

	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: dry stone must have zero cohesion for these numbers to mean friction, got %g"),
			DryStone.ShearCohesionMPa),
		DryStone.ShearCohesionMPa == 0.0);

	// No bond, so even a tiny tension parts it.
	constexpr double WhisperOfTensionMPa = 1.0e-6;

	const double TensileUtilisation = DestructionForce::ComputeUtilisation(
		TensionOf(ForceForMPa(WhisperOfTensionMPa, JointAreaSqCm)), DryStone, JointAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("dry stone must give under ANY tension, %g MPa gave utilisation %g"),
			WhisperOfTensionMPa, TensileUtilisation),
		TensileUtilisation > 1.0);

	// Control: mortar holds the same tension.
	const double MortarUnderSameTension = DestructionForce::ComputeUtilisation(
		TensionOf(ForceForMPa(WhisperOfTensionMPa, JointAreaSqCm)), GeneralPurposeMortar, JointAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("mortar must hold the same tension dry stone gave under, got utilisation %g"),
			MortarUnderSameTension),
		MortarUnderSameTension < 1.0);

	const double CompressionOnlyUtilisation = DestructionForce::ComputeUtilisation(
		CompressionOf(ForceForMPa(0.5 * DryStone.CompressiveStrengthMPa, JointAreaSqCm)),
		DryStone, JointAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("dry stone at half its crushing limit should read 0.5, got %g"), CompressionOnlyUtilisation),
		FMath::IsNearlyEqual(CompressionOnlyUtilisation, 0.5, Tolerance));

	// It resists sliding only while squeezed.

	FConnectionLoad Squeezed;
	Squeezed.Compression = ForceForMPa(CompressionStressMPa, JointAreaSqCm);
	Squeezed.Shear = ForceForMPa(0.5 * FrictionCapacityMPa, JointAreaSqCm);

	const double SqueezedUtilisation =
		DestructionForce::ComputeUtilisation(Squeezed, DryStone, JointAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("squeezed dry stone should carry half its friction capacity at 0.5, got %g"),
			SqueezedUtilisation),
		FMath::IsNearlyEqual(SqueezedUtilisation, 0.5, Tolerance));

	// Same shear with the weight removed: the capacity was all friction, so it parts.
	FConnectionLoad Unweighted;
	Unweighted.Shear = Squeezed.Shear;

	const double UnweightedUtilisation =
		DestructionForce::ComputeUtilisation(Unweighted, DryStone, JointAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("the same shear must part an unweighted dry stone joint, got utilisation %g"),
			UnweightedUtilisation),
		UnweightedUtilisation > 1.0);

	// Through FConnection, so classification is covered. Two joints because giving latches.

	const double BrickWeightUU = HandSetBrickMassKg * 980.0;

	FConnection BedJoint;
	BedJoint.PieceA = 0;
	BedJoint.PieceB = 1;
	BedJoint.InterfaceNormal = FVector(0.0, 0.0, 1.0);
	BedJoint.InterfaceAreaSqCm = JointAreaSqCm;
	BedJoint.Strength = DryStone;

	FConnection HangingJoint = BedJoint;

	// Normal points up at the upper piece; its weight squeezes the joint.
	const double BedUtilisation = BedJoint.ApplyForce(FVector(0.0, 0.0, -BrickWeightUU));

	// Same magnitude reversed: a brick hung from the one below.
	const double HangingUtilisation = HangingJoint.ApplyForce(FVector(0.0, 0.0, BrickWeightUU));

	TestFalse(
		FString::Printf(TEXT("a dry stone bed joint under one brick's weight must hold, utilisation %g"), BedUtilisation),
		BedJoint.HasGiven());

	TestTrue(
		FString::Printf(TEXT("a dry stone joint asked to HANG one brick must give, utilisation %g"), HangingUtilisation),
		HangingJoint.HasGiven());

	return true;
}

/**
 * mu = 0 makes a fastener's shear capacity independent of compression, exactly. Mortar
 * under the same loads is the control.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileLibraryFastenerUncouplingTest,
	"DestructionGame.Core.Profiles.FastenersAreUncoupled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FProfileLibraryFastenerUncouplingTest::RunTest(const FString& Parameters)
{
	using namespace ProfileLibraryTestSupport;

	constexpr double Tolerance = 1e-9;

	/** Shear at 0.5 of cohesion, compression at 0.2 of crushing, so shear governs. */
	auto UtilisationPair = [](const FConnectionStrength& S, double& OutDry, double& OutSqueezed)
	{
		FConnectionLoad Dry;
		Dry.Shear = ForceForMPa(0.5 * S.ShearCohesionMPa, JointAreaSqCm);

		FConnectionLoad Squeezed = Dry;
		Squeezed.Compression = ForceForMPa(0.2 * S.CompressiveStrengthMPa, JointAreaSqCm);

		OutDry = DestructionForce::ComputeUtilisation(Dry, S, JointAreaSqCm);
		OutSqueezed = DestructionForce::ComputeUtilisation(Squeezed, S, JointAreaSqCm);
	};

	int32 FastenersSeen = 0;

	for (const FNamedConnectionProfile& Row : AllConnectionProfiles())
	{
		if (Row.Class != EConnectionProfileClass::MechanicalFastener)
		{
			continue;
		}

		++FastenersSeen;

		double Dry = 0.0;
		double Squeezed = 0.0;
		UtilisationPair(Row.Strength, Dry, Squeezed);

		TestTrue(
			FString::Printf(TEXT("%s: shear at half cohesion should read 0.5 by construction, got %g"),
				Row.Name, Dry),
			FMath::IsNearlyEqual(Dry, 0.5, Tolerance));

		TestTrue(
			FString::Printf(TEXT("%s: squeezing an uncoupled fastener must not change its shear utilisation, %g -> %g"),
				Row.Name, Dry, Squeezed),
			Squeezed == Dry);
	}

	TestTrue(
		FString::Printf(TEXT("the library must contain mechanical fasteners to test, found %d"), FastenersSeen),
		FastenersSeen >= 3);

	// Control: the same loads on mortar must change its utilisation.
	double MortarDry = 0.0;
	double MortarSqueezed = 0.0;
	UtilisationPair(GeneralPurposeMortar, MortarDry, MortarSqueezed);

	TestTrue(
		FString::Printf(TEXT("squeezing a mortar joint must lower its utilisation, %g -> %g"),
			MortarDry, MortarSqueezed),
		MortarSqueezed < MortarDry);

	return true;
}

/**
 * Timber is C24 softwood (EN 338) on a mean basis, wired through ComputeUtilisation.
 *
 * Fields map to parallel-to-grain means: compressive f_c,0 = 29, tensile f_t,0 = 23,
 * shear f_v = 6.0 MPa. Means come from the characteristic values (21/14/4.0) via a
 * lognormal 5-percentile, mean = char x exp(1.645 x sqrt(ln(1 + CoV^2))), with per-property
 * CoVs from JCSS PMC 3.5 (0.20/0.30/0.25). The same factor takes f_m,k 24 to
 * BeamAcceptanceTest's 36 MPa mean bending. Density stays 0.42 g/cm3 (never 420; DESIGN.md §3).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileLibraryTimberProfileTest,
	"DestructionGame.Core.Profiles.TimberIsMeanBasisC24Softwood",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FProfileLibraryTimberProfileTest::RunTest(const FString& Parameters)
{
	using namespace ProfileLibraryTestSupport;

	constexpr double Tolerance = 1e-9;

	// EN 338 C24 characteristic (5-percentile) values: the base the means derive from.
	constexpr double C24CompressiveCharMPa = 21.0;       // f_c,0,k, compression parallel
	constexpr double C24TensileCharMPa = 14.0;           // f_t,0,k, tension parallel
	constexpr double C24ShearCharMPa = 4.0;              // f_v,k

	// JCSS PMC 3.5 Table 2 per-property CoVs for European softwood.
	constexpr double CoVCompression = 0.20;
	constexpr double CoVTension = 0.30;
	constexpr double CoVShear = 0.25;

	// EN 384 / EN 1990 lognormal 5-percentile. Spelled out independently of production code.
	const auto MeanFromCharacteristic = [](double CharMPa, double CoV)
	{
		const double SigmaLn = FMath::Sqrt(FMath::Loge(1.0 + CoV * CoV));
		return CharMPa * FMath::Exp(1.645 * SigmaLn);
	};

	const double C24CompressiveRawMean = MeanFromCharacteristic(C24CompressiveCharMPa, CoVCompression); // ~29.09
	const double C24TensileRawMean = MeanFromCharacteristic(C24TensileCharMPa, CoVTension);             // ~22.69
	const double C24ShearRawMean = MeanFromCharacteristic(C24ShearCharMPa, CoVShear);                   // ~6.00

	// The rounded means the profile must carry.
	constexpr double C24CompressiveMeanMPa = 29.0;
	constexpr double C24TensileMeanMPa = 23.0;
	constexpr double C24ShearMeanMPa = 6.0;

	// Each rounded mean within 3% of its derivation; the gap to characteristic is 27-38%.
	TestTrue(
		FString::Printf(TEXT("pinned compressive mean %g must be within 3%% of the derived %g (char 21, CoV 0.20)"),
			C24CompressiveMeanMPa, C24CompressiveRawMean),
		FMath::IsNearlyEqual(C24CompressiveMeanMPa, C24CompressiveRawMean, 0.03 * C24CompressiveRawMean));
	TestTrue(
		FString::Printf(TEXT("pinned tensile mean %g must be within 3%% of the derived %g (char 14, CoV 0.30)"),
			C24TensileMeanMPa, C24TensileRawMean),
		FMath::IsNearlyEqual(C24TensileMeanMPa, C24TensileRawMean, 0.03 * C24TensileRawMean));
	TestTrue(
		FString::Printf(TEXT("pinned shear mean %g must be within 3%% of the derived %g (char 4.0, CoV 0.25)"),
			C24ShearMeanMPa, C24ShearRawMean),
		FMath::IsNearlyEqual(C24ShearMeanMPa, C24ShearRawMean, 0.03 * C24ShearRawMean));

	// Cross-check: f_m,k 24 at CoV 0.25 gives BeamAcceptanceTest's 36 MPa mean bending.
	const double C24BendingRawMean = MeanFromCharacteristic(24.0, CoVShear);
	TestTrue(
		FString::Printf(TEXT("cross-check: f_m,k 24 x the CoV-0.25 factor should give the file's mean bending 36, got %g"),
			C24BendingRawMean),
		FMath::IsNearlyEqual(C24BendingRawMean, 36.0, 0.03 * 36.0));

	constexpr double C24DensityGramsPerCubicCm = 0.42;   // rho_mean 420 kg/m3, unchanged

	const FNamedMaterialProfile* TimberRow = nullptr;
	for (const FNamedMaterialProfile& Row : AllMaterialProfiles())
	{
		if (Row.Name != nullptr && FCString::Strcmp(Row.Name, TEXT("Timber")) == 0)
		{
			TimberRow = &Row;
			break;
		}
	}

	if (TimberRow == nullptr)
	{
		AddError(FString::Printf(
			TEXT("the material library must contain a profile named Timber; it does not (library has %d entries)"),
			AllMaterialProfiles().Num()));
		return false;
	}

	const FMaterialProfile& M = TimberRow->Profile;
	const FConnectionStrength& S = M.Strength;

	TestTrue(
		FString::Printf(TEXT("Timber density %g g/cm3 must be EN 338 C24 rho_mean %g g/cm3 (0.42, NOT 420 kg/m3)"),
			M.DensityGramsPerCubicCm, C24DensityGramsPerCubicCm),
		FMath::IsNearlyEqual(M.DensityGramsPerCubicCm, C24DensityGramsPerCubicCm, Tolerance));

	TestTrue(
		FString::Printf(TEXT("Timber compressive %g MPa must be C24 MEAN f_c,0 %g MPa (re-anchored off char 21)"),
			S.CompressiveStrengthMPa, C24CompressiveMeanMPa),
		FMath::IsNearlyEqual(S.CompressiveStrengthMPa, C24CompressiveMeanMPa, Tolerance));

	TestTrue(
		FString::Printf(TEXT("Timber tensile %g MPa must be C24 MEAN f_t,0 %g MPa (re-anchored off char 14)"),
			S.TensileStrengthMPa, C24TensileMeanMPa),
		FMath::IsNearlyEqual(S.TensileStrengthMPa, C24TensileMeanMPa, Tolerance));

	TestTrue(
		FString::Printf(TEXT("Timber shear %g MPa must be C24 MEAN f_v %g MPa (re-anchored off char 4.0)"),
			S.ShearCohesionMPa, C24ShearMeanMPa),
		FMath::IsNearlyEqual(S.ShearCohesionMPa, C24ShearMeanMPa, Tolerance));

	// A mean exceeds its characteristic on every axis; catches a half-applied re-anchor.
	TestTrue(
		FString::Printf(TEXT("mean basis: compressive %g must exceed characteristic %g"),
			S.CompressiveStrengthMPa, C24CompressiveCharMPa),
		S.CompressiveStrengthMPa > C24CompressiveCharMPa);
	TestTrue(
		FString::Printf(TEXT("mean basis: tensile %g must exceed characteristic %g"),
			S.TensileStrengthMPa, C24TensileCharMPa),
		S.TensileStrengthMPa > C24TensileCharMPa);
	TestTrue(
		FString::Printf(TEXT("mean basis: shear %g must exceed characteristic %g"),
			S.ShearCohesionMPa, C24ShearCharMPa),
		S.ShearCohesionMPa > C24ShearCharMPa);

	// Timber carries real tension (23 vs 29), so it is not compression-dominant.
	TestFalse(
		FString::Printf(TEXT("timber must stay tension-capable (not compression-dominant): mean %g comp vs %g tens"),
			S.CompressiveStrengthMPa, S.TensileStrengthMPa),
		M.bCompressionDominant);

	/*
	 * Single-axis loads through ComputeUtilisation. 7 MPa gives distinct readings on each
	 * axis (7/29, 7/23, 7/6), so a value in the wrong field is caught.
	 */
	constexpr double ProbeStressMPa = 7.0;

	const double CompUtil = DestructionForce::ComputeUtilisation(
		CompressionOf(ForceForMPa(ProbeStressMPa, JointAreaSqCm)), S, JointAreaSqCm);
	const double ExpectedCompUtil = ProbeStressMPa / C24CompressiveMeanMPa;

	TestTrue(
		FString::Printf(TEXT("Timber at %g MPa compression should read %g (7/29), got %g"),
			ProbeStressMPa, ExpectedCompUtil, CompUtil),
		FMath::IsNearlyEqual(CompUtil, ExpectedCompUtil, Tolerance));

	const double TensUtil = DestructionForce::ComputeUtilisation(
		TensionOf(ForceForMPa(ProbeStressMPa, JointAreaSqCm)), S, JointAreaSqCm);
	const double ExpectedTensUtil = ProbeStressMPa / C24TensileMeanMPa;

	TestTrue(
		FString::Printf(TEXT("Timber at %g MPa tension should read %g (7/23), got %g"),
			ProbeStressMPa, ExpectedTensUtil, TensUtil),
		FMath::IsNearlyEqual(TensUtil, ExpectedTensUtil, Tolerance));

	const double ShearUtil = DestructionForce::ComputeUtilisation(
		ShearOf(ForceForMPa(ProbeStressMPa, JointAreaSqCm)), S, JointAreaSqCm);
	const double ExpectedShearUtil = ProbeStressMPa / C24ShearMeanMPa;

	TestTrue(
		FString::Printf(TEXT("Timber at %g MPa shear should read %g (7/6), got %g"),
			ProbeStressMPa, ExpectedShearUtil, ShearUtil),
		FMath::IsNearlyEqual(ShearUtil, ExpectedShearUtil, Tolerance));

	return true;
}

/**
 * FindConnectionProfileRow(Row.Strength) returns &Row, by address, for every shipped row, and
 * null for any strength the library never shipped.
 *
 * FConnection stores a copy of its profile, so the five numbers are the only route back to the
 * row. Rows are close siblings (DryStone and CohesionlessBond differ on tension alone), so a
 * four-field match lands on a neighbour, and two field-identical rows would alias; the pointer
 * comparison catches both. Negatives: a Screw nudged 0.01 MPa is not a Screw (exact match, not
 * nearest), and a NaN in any field matches no row (fail closed).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileLibraryConnectionRowRoundTripTest,
	"DestructionGame.Core.Profiles.ConnectionRowsRoundTripByValue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FProfileLibraryConnectionRowRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace ProfileLibraryTestSupport;

	// Names the row so a failure is readable.
	const auto NameOfRow = [](const FNamedConnectionProfile* Row)
	{
		if (Row == nullptr)
		{
			return FString(TEXT("<no row>"));
		}

		return Row->Name != nullptr ? FString(Row->Name) : FString(TEXT("<unnamed row>"));
	};

	const auto DescribeStrength = [](const FConnectionStrength& S)
	{
		return FString::Printf(
			TEXT("{c %g, coh %g, t %g, mu %g, cap %g}"),
			S.CompressiveStrengthMPa, S.ShearCohesionMPa, S.TensileStrengthMPa,
			S.FrictionCoefficient, S.MaxShearStrengthMPa);
	};

	// Every shipped row finds itself, by address.

	int32 RowsSwept = 0;

	for (const FNamedConnectionProfile& Row : AllConnectionProfiles())
	{
		++RowsSwept;

		const FNamedConnectionProfile* const Found = FindConnectionProfileRow(Row.Strength);

		TestTrue(
			*FString::Printf(
				TEXT("%s must find ITSELF from its own five numbers %s — a lookup that answers with a "
					 "SIBLING names a joint the player never built, and a library holding two "
					 "field-identical rows makes every joint of the second read as the first. It "
					 "answered %s"),
				*NameOfRow(&Row), *DescribeStrength(Row.Strength), *NameOfRow(Found)),
			Found == &Row);

		/*
		 * Strength is a reference to the shipped extern, not a copy. The material library once held
		 * copies and answered "no such row" for every piece.
		 */
		if (Found != nullptr)
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s: the found row's Strength must BE the shipped constant, by address — a row "
						 "holding a COPY hands every caller a pointer a retune never reaches"),
					*NameOfRow(&Row)),
				&Found->Strength == &Row.Strength);
		}
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the sweep must have read the library, it read %d row(s)"), RowsSwept),
		RowsSwept >= 7);

	// A strength the library never shipped matches no row.

	struct FNotARowCase
	{
		FString Description;
		FConnectionStrength Strength;
	};

	const double NotANumber = std::numeric_limits<double>::quiet_NaN();

	// Screw, one field at a time: it has a sibling fastener on each side for a loose match to hit.
	struct FFieldCase
	{
		const TCHAR* Name;
		double FConnectionStrength::* Field;
	};

	const FFieldCase Fields[] = {
		{ TEXT("compressive strength"), &FConnectionStrength::CompressiveStrengthMPa },
		{ TEXT("shear cohesion"),       &FConnectionStrength::ShearCohesionMPa },
		{ TEXT("tensile strength"),     &FConnectionStrength::TensileStrengthMPa },
		{ TEXT("friction coefficient"), &FConnectionStrength::FrictionCoefficient },
		{ TEXT("shear ceiling"),        &FConnectionStrength::MaxShearStrengthMPa },
	};

	TArray<FNotARowCase> NotRows;

	{
		FConnectionStrength Nudged = Screw;
		Nudged.TensileStrengthMPa += 0.01;

		NotRows.Add({
			TEXT("a Screw whose withdrawal is 0.01 MPa out — a hundredth of a megapascal from a "
				 "shipped row and NOT that row, because the question is which row this IS and never "
				 "which row it is LIKE"),
			Nudged });
	}

	for (const FFieldCase& Field : Fields)
	{
		FConnectionStrength NotFinite = Screw;
		NotFinite.*Field.Field = NotANumber;

		NotRows.Add({
			FString::Printf(TEXT("a Screw whose %s is NaN"), Field.Name),
			NotFinite });
	}

	for (const FNotARowCase& Case : NotRows)
	{
		const FNamedConnectionProfile* const Found = FindConnectionProfileRow(Case.Strength);

		TestTrue(
			*FString::Printf(
				TEXT("%s: it must find NO ROW. Naming the nearest one is worse than naming none — this "
					 "library is siblings by construction, so 'nearest' is a plausible lie, and a "
					 "strength that is not a number must never read as a shipped profile. %s answered "
					 "%s"),
				*Case.Description, *DescribeStrength(Case.Strength), *NameOfRow(Found)),
			Found == nullptr);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
