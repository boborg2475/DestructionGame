// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Connection.h"
#include "Core/ConnectionStrength.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SHARED PROFILE LIBRARY.
 *
 * Connection and material profiles currently exist only as constants copied into
 * three test files. Retune mortar in one and the other two silently keep the old
 * value while everything stays green. This suite specifies the shared library
 * that replaces them.
 *
 * WHAT IS DELIBERATELY NOT ASSERTED: the numbers themselves. A test that
 * transcribes 0.2 back out of the table is a change-detector that asserts
 * nothing and blocks retuning. What is asserted instead is
 *
 *   - RELATIONS that come from physics rather than from the table (compression
 *     dominates a bonded joint; dry stone has no bond at all; a fastener's mu is
 *     exactly zero; a shear cap is never below the cohesion it truncates),
 *   - WELL-FORMEDNESS swept over the whole library, so adding a profile is
 *     adding a row and it gets checked for free, and
 *   - BEHAVIOUR through ComputeUtilisation and FConnection, so a profile is
 *     proven wired into the real code path rather than merely typed.
 *
 * CALIBRATION BASELINE, per DESIGN.md §4 and §6: structural concrete C30/37 —
 * 30 MPa characteristic compressive strength, ~3 MPa mean tensile (EN 1992-1-1
 * f_ctm), ~6 MPa shear — which is what the existing three test files already use
 * as their baseline. Everything else is a ratio of it.
 *
 * NO WORLD, NO TICKING SOLVER. Every assertion is arithmetic over plain structs,
 * so gravity is irrelevant by construction rather than switched off.
 *
 * NAMED NAMESPACE, not anonymous: the other test files declare Mortar and
 * MakeNaN in anonymous namespaces, which collide the moment a unity build merges
 * two of them. See CURRENT_STATE.md's unity-build gotcha.
 */
namespace ProfileLibraryTestSupport
{
	using namespace DestructionProfiles;

	/** A 10 cm x 10 cm interface. Round, so the arithmetic stays checkable by eye. */
	constexpr double JointAreaSqCm = 100.0;

	/**
	 * Force, in Unreal units, that loads the given area to the given stress.
	 *
	 * Spelled out here rather than reusing ForceUnitsPerMPaSqCm so the test fails
	 * if that constant is wrong instead of agreeing with it.
	 * 1 N = 100 uu, 1 cm2 = 100 mm2, 1 MPa = 1 N/mm2 -> 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	FConnectionLoad CompressionOf(double Force) { FConnectionLoad L; L.Compression = Force; return L; }
	FConnectionLoad TensionOf(double Force) { FConnectionLoad L; L.Tension = Force; return L; }
	FConnectionLoad ShearOf(double Force) { FConnectionLoad L; L.Shear = Force; return L; }

	/**
	 * The worst a profile does when loaded to the given stress on each axis in
	 * turn. Compression and tension are separate loads because FConnectionLoad
	 * guarantees at most one of them is non-zero.
	 */
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

	/**
	 * A standard UK metric brick, 215 x 102.5 x 65 mm. Spelled here rather than in
	 * the library because dimensions are the brick actor's business (phase 3), not
	 * the material's — the material only owns density.
	 */
	constexpr double BrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/**
	 * WHAT A BRICK WEIGHS IN THE HAND, in kg — and the only external anchor for it
	 * anywhere in the suite.
	 *
	 * The structure tests now DERIVE their brick mass from ClayBrick's density rather
	 * than hand-setting it, precisely so this is the single place the number can be
	 * wrong. Do not add a second literal elsewhere: two copies is how the old hand-set
	 * 2.72 drifted 1.6 g from the 2.7216 the library actually states.
	 */
	constexpr double HandSetBrickMassKg = 2.72;
}

/**
 * Every connection profile is well-formed, and each obeys the invariants of the
 * KIND of joint it claims to be.
 *
 * A parameterised sweep over the whole library rather than a test per profile:
 * adding a profile is adding a row and it inherits every check here for free.
 * The per-class rules are the point — "compression dominates" is a statement
 * about a mortared masonry joint, and asserting it over a bolt would be wrong.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileLibraryConnectionInvariantsTest,
	"DestructionGame.Core.Profiles.ConnectionInvariants",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FProfileLibraryConnectionInvariantsTest::RunTest(const FString& Parameters)
{
	using namespace ProfileLibraryTestSupport;

	/*
	 * The library must contain at least these, classified as stated. Deliberately
	 * NOT an exact count — an eighth profile should be a row, not a test edit.
	 */
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

		/*
		 * Two ways to reach the same profile must not drift apart. Without this a
		 * retune could land on one path and leave the other stale, which is the
		 * exact failure the library exists to close.
		 */
		TestTrue(
			FString::Printf(TEXT("the named constant %s and its library row must be the same data"), Want.Name),
			StrengthsMatch(*Want.Named, Row->Strength));
	}

	/*
	 * Absurd enough that any real joint gives, small enough to stay far from
	 * overflow. Only a fixture may survive it.
	 */
	constexpr double AbsurdStressMPa = 1.0e6;

	for (const FNamedConnectionProfile& Row : AllConnectionProfiles())
	{
		const FConnectionStrength& S = Row.Strength;
		const FString Where = FString::Printf(TEXT("%s (%s)"), Row.Name, ClassName(Row.Class));

		// --- well-formedness, every profile, every class -----------------------

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

		/*
		 * A joint that resists nothing being crushed is not a joint. Zero is a
		 * physical claim on cohesion and tension (dry stone) but never here.
		 */
		TestTrue(*FString::Printf(TEXT("%s: compressive strength must be positive, got %g"), *Where, S.CompressiveStrengthMPa),
			S.CompressiveStrengthMPa > 0.0);

		TestTrue(*FString::Printf(TEXT("%s: shear cohesion must not be negative, got %g"), *Where, S.ShearCohesionMPa),
			S.ShearCohesionMPa >= 0.0);
		TestTrue(*FString::Printf(TEXT("%s: tensile strength must not be negative, got %g"), *Where, S.TensileStrengthMPa),
			S.TensileStrengthMPa >= 0.0);

		// mu above ~1.5 is not a joint, it is a typo. Masonry testing gives 0.6-0.8.
		TestTrue(*FString::Printf(TEXT("%s: friction coefficient must be within [0, 1.5], got %g"), *Where, S.FrictionCoefficient),
			S.FrictionCoefficient >= 0.0 && S.FrictionCoefficient <= 1.5);

		/*
		 * THE CAP MUST NEVER TRUNCATE THE BOND ITSELF. Capacity is
		 * min(cohesion + mu*sigma, cap), so a cap below cohesion silently gives
		 * the joint less strength than its own stated cohesion — an unloaded
		 * joint would then be weaker than the profile advertises, with nothing
		 * in the numbers to show it. Flagged as an untested edge in
		 * CURRENT_STATE.md; this is what closes it.
		 */
		TestTrue(
			*FString::Printf(TEXT("%s: max shear %g must not be below cohesion %g — the cap would silently weaken the bond"),
				*Where, S.MaxShearStrengthMPa, S.ShearCohesionMPa),
			S.MaxShearStrengthMPa >= S.ShearCohesionMPa);

		// --- per-class physics -------------------------------------------------

		switch (Row.Class)
		{
			case EConnectionProfileClass::Bonded:
			{
				/*
				 * A mortared masonry joint is overwhelmingly a compression member:
				 * it crushes at tens of MPa, debonds in shear at tenths of one, and
				 * pulls apart at less again. If these come out close the directional
				 * model has nothing to work with.
				 */
				TestTrue(
					*FString::Printf(TEXT("%s: compressive %g should be at least 10x cohesion %g"),
						*Where, S.CompressiveStrengthMPa, S.ShearCohesionMPa),
					S.CompressiveStrengthMPa >= 10.0 * S.ShearCohesionMPa);

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

				/*
				 * An uncapped envelope makes the base of a tall wall effectively
				 * uncuttable — backwards for a demolition game. Every profile whose
				 * capacity grows with load needs a real ceiling.
				 */
				TestTrue(
					*FString::Printf(TEXT("%s: a friction-coupled joint needs a real shear ceiling, got %g"),
						*Where, S.MaxShearStrengthMPa),
					S.MaxShearStrengthMPa < TNumericLimits<double>::Max());
				break;
			}

			case EConnectionProfileClass::Frictional:
			{
				/*
				 * THE PROFILE THAT DIFFERS IN KIND. Dry stone has no mortar, so
				 * there is no bond to have cohesion or tensile strength. These are
				 * exact zeroes, not small numbers: the whole reason Mohr-Coulomb
				 * coupling is in the model is that this wall cannot stand without it.
				 */
				TestTrue(
					*FString::Printf(TEXT("%s: a frictional joint has NO bond, cohesion must be exactly 0, got %g"),
						*Where, S.ShearCohesionMPa),
					S.ShearCohesionMPa == 0.0);
				TestTrue(
					*FString::Printf(TEXT("%s: a frictional joint has NO bond, tensile must be exactly 0, got %g"),
						*Where, S.TensileStrengthMPa),
					S.TensileStrengthMPa == 0.0);

				// With no cohesion, friction is the only thing it has.
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
				/*
				 * EXACTLY zero, not nearly. mu = 0 is what collapses Mohr-Coulomb
				 * to three independent axes, which is how a bolt stays data rather
				 * than a second code path. A small non-zero mu would leave a bolt
				 * quietly gaining shear strength from the weight above it.
				 */
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

		// --- a fixture, and only a fixture, may be unbreakable -----------------

		const double Utilisation = WorstUtilisationAtStress(S, AbsurdStressMPa);

		TestFalse(*FString::Printf(TEXT("%s: utilisation must never be NaN, got %g"), *Where, Utilisation),
			FMath::IsNaN(Utilisation));

		if (Row.Class == EConnectionProfileClass::TestFixture)
		{
			TestTrue(
				*FString::Printf(TEXT("%s: a test fixture is meant to be unbreakable, but %g MPa broke it (utilisation %g)"),
					*Where, AbsurdStressMPa, Utilisation),
				Utilisation <= 1.0);
		}
		else
		{
			/*
			 * Nothing shippable may be accidentally indestructible — that is a
			 * wall the player cannot demolish, and it looks exactly like a bug in
			 * the solver rather than a bad number.
			 */
			TestTrue(
				*FString::Printf(TEXT("%s: a real joint must give at %g MPa, got utilisation %g"),
					*Where, AbsurdStressMPa, Utilisation),
				Utilisation > 1.0);
		}
	}

	/*
	 * The gameplay hook DESIGN.md §2 promises: "the same wood frame built with
	 * nails vs. screws vs. bolts genuinely behaves differently under load." That
	 * is only true if the three are ordered, and the ordering is the published one
	 * — a screw's threads outhold a nail's plain shank, and a through-bolt outholds
	 * both. Ordinal, so retuning the magnitudes leaves it alone.
	 */
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

	/*
	 * Lime mortar is the soft historic binder; general-purpose cement mortar is
	 * stronger on every axis. Two profiles that came out equal would mean one was
	 * copied from the other.
	 */
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

/**
 * Every material profile is well-formed, and its density is a density.
 *
 * Density is the one field with a hard external check available: a clay brick at
 * its published density and its standard dimensions must weigh what the existing
 * structure tests hand-set for a brick. That ties the new data to the suite that
 * already exists instead of to itself.
 */
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
		 * UNITS TRAP, and the reason for the upper bound. Density is g/cm3 because
		 * that is what UPhysicalMaterial::Density takes, so published values go in
		 * unconverted. The same brick in kg/m3 is 1900 — a thousand times heavier
		 * and still a plausible-looking number. Balsa is 0.16; osmium is 22.6.
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

		/*
		 * Same edge as the connection sweep: a cap below cohesion silently weakens
		 * the material below its own stated shear strength.
		 */
		TestTrue(
			*FString::Printf(TEXT("%s: max shear %g must not be below shear strength %g"),
				*Where, S.MaxShearStrengthMPa, S.ShearCohesionMPa),
			S.MaxShearStrengthMPa >= S.ShearCohesionMPa);

		/*
		 * Masonry and concrete are compression members: they crush at ten times
		 * the stress that pulls them apart. This is the material-side counterpart
		 * of the bonded-joint rule, and it is what makes "brick crushes" a
		 * different failure mode from "mortar gives".
		 */
		TestTrue(
			*FString::Printf(TEXT("%s: compressive %g should be at least 5x tensile %g"),
				*Where, S.CompressiveStrengthMPa, S.TensileStrengthMPa),
			S.CompressiveStrengthMPa >= 5.0 * S.TensileStrengthMPa);

		/*
		 * DECLARED AND UNUSED, but not therefore unconstrained: a bond factor is a
		 * derating of a connection against this face, so it lives in (0, 1].
		 */
		TestTrue(
			*FString::Printf(TEXT("%s: bond factor %g must lie in (0, 1]"), *Where, M.BondFactor),
			FMath::IsFinite(M.BondFactor) && M.BondFactor > 0.0 && M.BondFactor <= 1.0);
	}

	/*
	 * A brick has to weigh what a brick weighs. 215 x 102.5 x 65 mm at the clay
	 * brick's own density must reproduce the 2.72 kg a brick weighs in the hand.
	 *
	 * THIS IS THE ANCHOR THE WHOLE SUITE HANGS OFF. The structure tests derive their
	 * brick mass from the same density rather than hand-setting it, so this row is the
	 * one place the figure is checked against the outside world. The 3% band is what
	 * makes it a physical claim rather than a restatement of the arithmetic.
	 */
	const double DerivedBrickMassKg = ClayBrick.DensityGramsPerCubicCm * BrickVolumeCubicCm / 1000.0;

	TestTrue(
		FString::Printf(
			TEXT("a standard brick of %g cm3 at %g g/cm3 weighs %g kg; the structure tests assume %g kg"),
			BrickVolumeCubicCm, ClayBrick.DensityGramsPerCubicCm, DerivedBrickMassKg, HandSetBrickMassKg),
		FMath::IsNearlyEqual(DerivedBrickMassKg, HandSetBrickMassKg, 0.03 * HandSetBrickMassKg));

	/*
	 * Fired clay is porous; structural concrete is not. Two densities that came
	 * out equal would mean one was copied.
	 */
	TestTrue(
		FString::Printf(TEXT("concrete %g g/cm3 should be denser than fired clay brick %g g/cm3"),
			StructuralConcrete.DensityGramsPerCubicCm, ClayBrick.DensityGramsPerCubicCm),
		StructuralConcrete.DensityGramsPerCubicCm > ClayBrick.DensityGramsPerCubicCm);

	/*
	 * DESIGN.md §2's premise for connections being first-class: "a brick wall
	 * usually fails because the MORTAR gives before the brick". If mortar were the
	 * stronger of the two the whole connection layer would be modelling the wrong
	 * failure. This is a data-ordering claim only — the weakest-link pairing rule
	 * itself is deliberately deferred until a second material exists to prove it.
	 */
	TestTrue(
		FString::Printf(TEXT("mortar must crush before the brick: %g MPa vs %g MPa"),
			GeneralPurposeMortar.CompressiveStrengthMPa, ClayBrick.Strength.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa < ClayBrick.Strength.CompressiveStrengthMPa);

	TestTrue(
		FString::Printf(TEXT("mortar must debond before the brick splits: tensile %g MPa vs %g MPa"),
			GeneralPurposeMortar.TensileStrengthMPa, ClayBrick.Strength.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa < ClayBrick.Strength.TensileStrengthMPa);

	/*
	 * The shear ceiling is not a free parameter. Eurocode 6 truncates the
	 * Mohr-Coulomb envelope at roughly 0.065 times the UNIT's compressive
	 * strength, because past that the brick gives rather than the joint sliding —
	 * so mortar's cap must track the brick it is laid with, not a number someone
	 * liked. Generous tolerance: the coefficient is a code simplification.
	 */
	constexpr double EurocodeShearCapCoefficient = 0.065;
	const double EurocodeCapMPa = EurocodeShearCapCoefficient * ClayBrick.Strength.CompressiveStrengthMPa;

	TestTrue(
		FString::Printf(
			TEXT("mortar's shear cap %g MPa should be ~0.065 x brick compressive %g MPa = %g MPa"),
			GeneralPurposeMortar.MaxShearStrengthMPa, ClayBrick.Strength.CompressiveStrengthMPa, EurocodeCapMPa),
		FMath::IsNearlyEqual(GeneralPurposeMortar.MaxShearStrengthMPa, EurocodeCapMPa, 0.15 * EurocodeCapMPa));

	return true;
}

/**
 * DRY STONE IS THE PROFILE WHOSE BEHAVIOUR DIFFERS IN KIND, and this is the test
 * that proves the library is wired into the real code path rather than being a
 * struct someone typed.
 *
 * A dry-stone wall has no mortar. It cannot be pulled apart at all — any tension
 * whatsoever parts it — and everything holding it against sliding is borrowed
 * from the weight pressing on it. Take the weight away and the shear capacity
 * goes with it. That is the behaviour Mohr-Coulomb coupling exists for, and no
 * amount of checking fields would demonstrate it.
 *
 * Every load below is expressed as a MULTIPLE OF THE PROFILE'S OWN NUMBERS, so
 * retuning dry stone leaves these expectations exactly where they are.
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
	 * --- the fixture has to be capable of measuring what it claims to measure ---
	 *
	 * WHICH AXIS GOVERNS IS THE WHOLE RISK HERE. ComputeUtilisation returns the
	 * worst of three axes, so a shear assertion silently becomes a compression
	 * assertion if the compression chosen to CREATE the friction happens to
	 * utilise more. Compression is therefore held at 0.2 of the crushing limit
	 * and shear at 0.5 of the resulting capacity: 0.2 < 0.5, so shear governs by
	 * construction. These preconditions fail loudly if a retune breaks that.
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

	// --- it cannot be pulled apart, at all -------------------------------------

	/*
	 * A thousandth of the weakest stress anywhere else in the suite. There is no
	 * bond, so there is no such thing as a small tension.
	 */
	constexpr double WhisperOfTensionMPa = 1.0e-6;

	const double TensileUtilisation = DestructionForce::ComputeUtilisation(
		TensionOf(ForceForMPa(WhisperOfTensionMPa, JointAreaSqCm)), DryStone, JointAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("dry stone must give under ANY tension, %g MPa gave utilisation %g"),
			WhisperOfTensionMPa, TensileUtilisation),
		TensileUtilisation > 1.0);

	/*
	 * The control that makes the assertion above mean something: a mortared joint
	 * has a real tensile bond and shrugs the same load off. Same code path, same
	 * area, same load — only the profile differs.
	 */
	const double MortarUnderSameTension = DestructionForce::ComputeUtilisation(
		TensionOf(ForceForMPa(WhisperOfTensionMPa, JointAreaSqCm)), GeneralPurposeMortar, JointAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("mortar must hold the same tension dry stone gave under, got utilisation %g"),
			MortarUnderSameTension),
		MortarUnderSameTension < 1.0);

	// --- but it is perfectly happy being squeezed ------------------------------

	const double CompressionOnlyUtilisation = DestructionForce::ComputeUtilisation(
		CompressionOf(ForceForMPa(0.5 * DryStone.CompressiveStrengthMPa, JointAreaSqCm)),
		DryStone, JointAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("dry stone at half its crushing limit should read 0.5, got %g"), CompressionOnlyUtilisation),
		FMath::IsNearlyEqual(CompressionOnlyUtilisation, 0.5, Tolerance));

	// --- and it resists sliding only while it is squeezed ----------------------

	FConnectionLoad Squeezed;
	Squeezed.Compression = ForceForMPa(CompressionStressMPa, JointAreaSqCm);
	Squeezed.Shear = ForceForMPa(0.5 * FrictionCapacityMPa, JointAreaSqCm);

	const double SqueezedUtilisation =
		DestructionForce::ComputeUtilisation(Squeezed, DryStone, JointAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("squeezed dry stone should carry half its friction capacity at 0.5, got %g"),
			SqueezedUtilisation),
		FMath::IsNearlyEqual(SqueezedUtilisation, 0.5, Tolerance));

	/*
	 * THE MECHANISM BEHIND PROGRESSIVE COLLAPSE. Identical shear, weight removed:
	 * capacity was entirely borrowed and is now gone, so the joint parts. A model
	 * with fixed per-axis strengths cannot produce this, and a dry-stone wall
	 * cannot stand without it.
	 */
	FConnectionLoad Unweighted;
	Unweighted.Shear = Squeezed.Shear;

	const double UnweightedUtilisation =
		DestructionForce::ComputeUtilisation(Unweighted, DryStone, JointAreaSqCm);

	TestTrue(
		FString::Printf(TEXT("the same shear must part an unweighted dry stone joint, got utilisation %g"),
			UnweightedUtilisation),
		UnweightedUtilisation > 1.0);

	/*
	 * --- through FConnection, so classification is in the loop too -------------
	 *
	 * Two separate joints because giving LATCHES: reusing one would have the
	 * second call read zero regardless of the load.
	 */

	const double BrickWeightUU = HandSetBrickMassKg * 980.0;

	FConnection BedJoint;
	BedJoint.PieceA = 0;
	BedJoint.PieceB = 1;
	BedJoint.InterfaceNormal = FVector(0.0, 0.0, 1.0);
	BedJoint.InterfaceAreaSqCm = JointAreaSqCm;
	BedJoint.Strength = DryStone;

	FConnection HangingJoint = BedJoint;

	/*
	 * Normal points up at the upper piece, and the force is that piece's weight,
	 * so the joint is squeezed — a brick resting on a brick.
	 */
	const double BedUtilisation = BedJoint.ApplyForce(FVector(0.0, 0.0, -BrickWeightUU));

	/*
	 * Same joint, same magnitude, pulled the other way: a brick hung from the one
	 * below it. Dry stone offers nothing.
	 */
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
 * mu = 0 makes a mechanical fastener three independent axes, exactly.
 *
 * A bolt does not care how hard the pieces are pressed together, and that is what
 * keeps connection types data rather than a second code path. The assertion is
 * exact equality, not approximate: compression must contribute NOTHING to shear
 * capacity, and a small non-zero mu would leave a bolt quietly strengthening
 * under the weight of the wall above it.
 *
 * Mortar under the identical construction is the control. If both came out equal
 * the test would be measuring nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileLibraryFastenerUncouplingTest,
	"DestructionGame.Core.Profiles.FastenersAreUncoupled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FProfileLibraryFastenerUncouplingTest::RunTest(const FString& Parameters)
{
	using namespace ProfileLibraryTestSupport;

	constexpr double Tolerance = 1e-9;

	/**
	 * Shear at half the profile's cohesion and compression at a fifth of its
	 * crushing limit: shear utilisation is 0.5 and compression 0.2 BY
	 * CONSTRUCTION, whatever the numbers are, so shear governs and the assertion
	 * cannot silently become a compression assertion.
	 */
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

	/*
	 * THE CONTROL. Identical construction on a friction-coupled joint must move
	 * the number, or the equality above proves nothing about mu.
	 */
	double MortarDry = 0.0;
	double MortarSqueezed = 0.0;
	UtilisationPair(GeneralPurposeMortar, MortarDry, MortarSqueezed);

	TestTrue(
		FString::Printf(TEXT("squeezing a mortar joint must lower its utilisation, %g -> %g"),
			MortarDry, MortarSqueezed),
		MortarSqueezed < MortarDry);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
