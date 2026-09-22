// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Connection.h"
#include "Core/ConnectionStrength.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The shared profile library.
 *
 * Connection and material profiles currently exist only as constants copied into
 * three test files. Retune mortar in one and the other two silently keep the old
 * value while everything stays green. This suite specifies the shared library
 * that replaces them.
 *
 * What is deliberately not asserted: the numbers themselves. A test that
 * transcribes 0.2 back out of the table is a change-detector that asserts
 * nothing and blocks retuning. What is asserted instead is
 *
 *   - relations that come from physics rather than the table (compression
 *     dominates a bonded joint; dry stone has no bond at all; a fastener's mu is
 *     exactly zero; a shear cap is never below the cohesion it truncates),
 *   - well-formedness swept over the whole library, so adding a profile is
 *     adding a row and it gets checked for free, and
 *   - behaviour through ComputeUtilisation and FConnection, so a profile is
 *     proven wired into the real code path rather than merely typed.
 *
 * Calibration baseline, per DESIGN.md §4 and §6: structural concrete C30/37 —
 * 30 MPa characteristic compressive strength, ~3 MPa mean tensile (EN 1992-1-1
 * f_ctm), ~6 MPa shear — which is what the existing three test files already use
 * as their baseline. Everything else is a ratio of it.
 *
 * No world, no ticking solver: every assertion is arithmetic over plain structs,
 * so gravity is irrelevant by construction rather than switched off.
 *
 * Named namespace, not anonymous: the other test files declare Mortar and
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
	 * What a brick weighs in the hand, in kg — the only external anchor for it
	 * anywhere in the suite.
	 *
	 * The structure tests now derive their brick mass from ClayBrick's density
	 * rather than hand-setting it, precisely so this is the single place the number
	 * can be wrong. Do not add a second literal elsewhere: two copies is how the
	 * old hand-set 2.72 drifted 1.6 g from the 2.7216 the library actually states.
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
		 * The cap must never truncate the bond itself. Capacity is
		 * min(cohesion + mu*sigma, cap), so a cap below cohesion silently gives
		 * the joint less strength than its own stated cohesion — an unloaded
		 * joint would then be weaker than the profile advertises, with nothing
		 * in the numbers to show it.
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
				 * it crushes at MPa scale, debonds in shear at tenths of one, and
				 * pulls apart at less again. If these come out close the directional
				 * model has nothing to work with.
				 *
				 * 5x rather than the old 10x since the 2026-08-14 mean re-anchor flip:
				 * mean shear bond runs closer to mean compressive class than the
				 * characteristic pair did (lime carries 2.0 / 0.27 = 7.4x; cement
				 * 10 / 0.9 = 11.1x), so 10x stopped being a physics floor and became
				 * a characteristic-basis artefact. 5x still separates a bonded joint
				 * from anything isotropic by a wide margin.
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
				 * The profile that differs in kind: dry stone has no mortar, so
				 * there is no bond to have cohesion or tensile strength. These are
				 * exact zeroes, not small numbers — the whole reason Mohr-Coulomb
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
				 * Exactly zero, not nearly: mu = 0 is what collapses Mohr-Coulomb
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
			/*
			 * A fixture is exempt from the must-break rule (it is not required to
			 * survive). The invariant this branch protects is one-directional:
			 * nothing shippable may be accidentally indestructible. `Unbreakable`
			 * is the row that must never give, asserted below by name, because
			 * that is a fact about one row rather than the class — keying it off
			 * the class would forbid the other useful kind of fixture, a
			 * perfectly breakable joint carrying a combination no real material
			 * has, and `CohesionlessBond` is exactly that: zero cohesion with a
			 * real tensile bond, the only way to tell a bounded composite depth
			 * from `DryStone`'s blanket condemnation.
			 */
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
	 * And `Unbreakable` is the one row that must survive that load, asserted by name: it is
	 * what the whole `TestFixture` class was invented for — a joint no plausible implementation
	 * can break mid-solve, so a routing test measures routing. The claim belongs to the row
	 * rather than the class: a second fixture may legitimately be breakable, and it is
	 * (`CohesionlessBond`), so a sweep keyed on the class would either forbid that row or say
	 * nothing about this one.
	 */
	TestTrue(
		FString::Printf(
			TEXT("Unbreakable must survive %g MPa on every axis, it reads %g"),
			AbsurdStressMPa, WorstUtilisationAtStress(Unbreakable, AbsurdStressMPa)),
		WorstUtilisationAtStress(Unbreakable, AbsurdStressMPa) <= 1.0);

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
		 * Units trap, and the reason for the upper bound: density is g/cm3 because
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
		 * Compression-member materials crush at many times the stress that pulls
		 * them apart — the material-side counterpart of the bonded-joint rule, and
		 * what makes "brick crushes" a different failure mode from "mortar gives".
		 *
		 * Scoped to bCompressionDominant, not asserted over the whole library,
		 * because it is a fact about masonry and concrete rather than materials in
		 * general. Timber is genuinely tension-capable parallel to the grain — C24
		 * is 21 MPa against 14, well under 5x — so applying this to it would be
		 * asserting a masonry ratio of wood. Keying off the trait keeps the check
		 * biting for a mis-specified masonry profile (a StructuralConcrete whose
		 * tensile crept above compressive/5 stays flagged) while exempting the
		 * materials that legitimately carry tension.
		 */
		if (M.bCompressionDominant)
		{
			TestTrue(
				*FString::Printf(TEXT("%s: compressive %g should be at least 5x tensile %g"),
					*Where, S.CompressiveStrengthMPa, S.TensileStrengthMPa),
				S.CompressiveStrengthMPa >= 5.0 * S.TensileStrengthMPa);
		}

		/*
		 * Declared and unused, but not therefore unconstrained: a bond factor is a
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
	 * This is the anchor the whole suite hangs off: the structure tests derive
	 * their brick mass from the same density rather than hand-setting it, so this
	 * row is the one place the figure is checked against the outside world. The 3%
	 * band is what makes it a physical claim rather than a restatement of the
	 * arithmetic.
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
	 * The shear ceiling is not a free parameter: past it the brick gives rather
	 * than the joint sliding, so mortar's cap must track the brick it is laid
	 * with, not a number someone liked.
	 *
	 * Mean basis (re-anchor 2026-08-13): the coefficient is 0.1, not Eurocode 6's
	 * characteristic-basis 0.065 — forced by measurement, because the Newcastle
	 * campaign's mean unconfined shear bond reaches 1.81 MPa, above the old
	 * 0.065 x 20 = 1.3 cap outright, so 0.065 f_b cannot be a mean-basis
	 * truncation. 0.1 x f_b = 2.0 sits above mean cohesion plus the working
	 * friction range and below the unit's own 3.0 MPa shear strength. Grade C,
	 * recorded as such: no published mean-basis equivalent of the EC6 truncation
	 * exists. Generous tolerance: the coefficient is a judgement.
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
 * Dry stone is the profile whose behaviour differs in kind, and this is the test
 * that proves the library is wired into the real code path rather than being a
 * struct someone typed.
 *
 * A dry-stone wall has no mortar. It cannot be pulled apart at all — any tension
 * whatsoever parts it — and everything holding it against sliding is borrowed
 * from the weight pressing on it. Take the weight away and the shear capacity
 * goes with it. That is the behaviour Mohr-Coulomb coupling exists for, and no
 * amount of checking fields would demonstrate it.
 *
 * Every load below is expressed as a multiple of the profile's own numbers, so
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
	 * Which axis governs is the whole risk here: ComputeUtilisation returns the
	 * worst of three axes, so a shear assertion silently becomes a compression
	 * assertion if the compression chosen to create the friction happens to
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
	 * The mechanism behind progressive collapse: identical shear, weight removed —
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

/**
 * Timber is the deliberate second structural material — C24 softwood, EN 338.
 *
 * SHED_PATH.md Phase B / slice B1: the multi-material shed needs a wood profile,
 * and it is the single data addition that proves the directional code genuinely
 * reads the profile rather than having masonry's numbers baked in. This test
 * specifies that the library carries it, with its published EN 338 C24 values, and
 * that those values are wired into the real ComputeUtilisation path rather than
 * merely typed.
 *
 * Why mean parallel-to-grain strengths, and which field each maps to:
 * StructuralConcrete and ClayBrick store a material's own directional strengths in
 * FConnectionStrength's compressive / shear / tensile fields — 38/7.6/3.0 and
 * 20/3.0/2.0 respectively. The matching C24 quantities are its axial
 * parallel-to-grain capacities, so the convention maps cleanly:
 *
 *     CompressiveStrengthMPa <- f_c,0,mean = 29   (compression parallel to grain)
 *     TensileStrengthMPa     <- f_t,0,mean = 23   (tension parallel to grain)
 *     ShearCohesionMPa       <- f_v,mean   = 6.0  (shear)
 *
 * Mean basis — the 2026-09-02 re-anchor (item 6a), bringing the last profile row
 * onto the same footing every masonry row was flipped to on 2026-08-13/14
 * (DESIGN.md §3): the library carries measured means, not code characteristics,
 * because verdicts ruled at 5-percentile design values are 3-8x pessimistic against
 * real members. The retired figures were EN 338 C24's characteristic (5-percentile)
 * f_c,0,k 21 / f_t,0,k 14 / f_v,k 4.0.
 *
 * The char -> mean factor is per-property, not a blanket scale (exactly as the
 * masonry re-anchor was), because the coefficient of variation differs by property.
 * EN 384 / EN 1990 define the characteristic as the 5-percentile of a lognormal
 * fit, so mean = char x exp(1.645 x sqrt(ln(1 + CoV^2))). JCSS PMC Part 3.5 Table 2
 * gives European softwood its per-property CoVs:
 *
 *     f_c,0 : CoV 0.20 -> factor 1.385 -> 21 x 1.385 = 29.1, pinned 29
 *     f_t,0 : CoV 0.30 -> factor 1.621 -> 14 x 1.621 = 22.7, pinned 23
 *     f_v   : CoV 0.25 -> factor 1.499 ->  4 x 1.499 =  6.0, pinned 6.0
 *
 * The shear CoV 0.25 and its factor 1.499 are cross-checked in the test against the
 * bending row the file already trusts: EN 338's f_m,k 24 through the same factor is
 * 24 x 1.499 = 36.0, exactly BeamAcceptanceTest's independently derived C24 mean
 * bending, and exactly its C24ShearMPa 6.0. That the two files land on the same
 * shear mean by the same JCSS CoV, derived apart, is the anchor for the factor.
 *
 * Not the member-bending derivation itself (36/6) for the axial fields: that is a
 * whole-stress-block check for a different limit state and deliberately reaches no
 * joint field. This material profile is the axial/shear joint-field convention, so
 * the axial mean strengths are the right numbers, and matching StructuralConcrete/
 * ClayBrick means these are the values that flow through ComputeUtilisation.
 * Density is unchanged — already the mean rho_mean = 420 kg/m3 = 0.42 g/cm3, because
 * weight is the only thing density does and what a beam weighs is the mean.
 *
 * Units trap (DESIGN.md §3): density is g/cm3 (0.42, never 420), strengths are SI
 * MPa, and ComputeUtilisation needs an area — ForceForMPa spells the 1 N = 100 uu,
 * 1 cm2 = 100 mm2 conversion out independently of the production constant.
 *
 * No world, no ticking solver — arithmetic over plain structs, like its siblings.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileLibraryTimberProfileTest,
	"DestructionGame.Core.Profiles.TimberIsMeanBasisC24Softwood",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FProfileLibraryTimberProfileTest::RunTest(const FString& Parameters)
{
	using namespace ProfileLibraryTestSupport;

	constexpr double Tolerance = 1e-9;

	/*
	 * EN 338 C24 CHARACTERISTIC (5-percentile) parallel-to-grain figures, published.
	 * These are the RETIRED anchors — kept here only as the base the mean is derived
	 * FROM, and to prove the profile has moved OFF them.
	 */
	constexpr double C24CompressiveCharMPa = 21.0;       // f_c,0,k, compression parallel
	constexpr double C24TensileCharMPa = 14.0;           // f_t,0,k, tension parallel
	constexpr double C24ShearCharMPa = 4.0;              // f_v,k

	/*
	 * JCSS PMC Part 3.5 Table 2 per-property CoVs for European structural softwood.
	 * The char->mean factor is NOT blanket: compression is the tightest-varying
	 * property, tension the widest, shear between them — exactly why the masonry
	 * re-anchor was derived per property too.
	 */
	constexpr double CoVCompression = 0.20;
	constexpr double CoVTension = 0.30;
	constexpr double CoVShear = 0.25;

	/*
	 * EN 384 / EN 1990: the characteristic is the 5-percentile of a lognormal fit,
	 * so mean = char x exp(1.645 x sqrt(ln(1 + CoV^2))). Spelled out here, NOT
	 * imported from any production constant, so a wrong factor fails this test rather
	 * than agreeing with the code.
	 */
	const auto MeanFromCharacteristic = [](double CharMPa, double CoV)
	{
		const double SigmaLn = FMath::Sqrt(FMath::Loge(1.0 + CoV * CoV));
		return CharMPa * FMath::Exp(1.645 * SigmaLn);
	};

	const double C24CompressiveRawMean = MeanFromCharacteristic(C24CompressiveCharMPa, CoVCompression); // ~29.09
	const double C24TensileRawMean = MeanFromCharacteristic(C24TensileCharMPa, CoVTension);             // ~22.69
	const double C24ShearRawMean = MeanFromCharacteristic(C24ShearCharMPa, CoVShear);                   // ~6.00

	/*
	 * THE PINNED, ROUNDED MEAN VALUES the profile must carry — the contract this
	 * re-anchor establishes. Rounded to clean figures the way BeamAcceptanceTest
	 * rounds its own C24 means (36.0, 6.0); the RawMean cross-checks below prove each
	 * rounding stays a faithful mean rather than a number someone liked.
	 */
	constexpr double C24CompressiveMeanMPa = 29.0;
	constexpr double C24TensileMeanMPa = 23.0;
	constexpr double C24ShearMeanMPa = 6.0;

	/*
	 * The rounding sanity band: each pinned mean must sit within 3% of the raw
	 * lognormal derivation. This is far tighter than the 27-38% gap up to the
	 * characteristic, so passing it PROVES the pin is the mean and not the retired
	 * 5-percentile figure.
	 */
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

	/*
	 * THE FACTOR, CROSS-CHECKED against the bending row the file already trusts.
	 * EN 338's f_m,k = 24 through the SAME shear CoV 0.25 must reproduce the mean
	 * bending 36.0 that BeamAcceptanceTest derived independently — so the two files
	 * agree on the char->mean factor without sharing a line of it.
	 */
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

	/*
	 * THE RED. No Timber row exists in the library yet, so the lookup returns
	 * nullptr and the whole specification cannot even begin. dev's green step is the
	 * one-entry data addition (an extern const FMaterialProfile Timber + its row in
	 * MaterialProfileLibrary) that makes this findable.
	 */
	if (TimberRow == nullptr)
	{
		AddError(FString::Printf(
			TEXT("the material library must contain a profile named Timber; it does not (library has %d entries)"),
			AllMaterialProfiles().Num()));
		return false;
	}

	const FMaterialProfile& M = TimberRow->Profile;
	const FConnectionStrength& S = M.Strength;

	// --- the mean values, pinned against the derived anchor ---------------------

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

	/*
	 * THE MEAN-BASIS INVARIANT the masonry rows already satisfy: a mean is the
	 * average of a distribution whose 5-percentile is the characteristic, so a
	 * mean-basis profile is STRICTLY STRONGER than its characteristic on every axis.
	 * This is what catches a half-applied re-anchor — one axis left at char while the
	 * others moved — that an exact pin on the other two would miss.
	 */
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

	/*
	 * The re-anchor does NOT touch the trait that makes wood dissimilar from masonry:
	 * timber still carries tension at a large fraction of its crushing strength
	 * (23 against 29), so it stays NOT compression-dominant and remains exempt from
	 * the library's "compressive >= 5x tensile" masonry check.
	 */
	TestFalse(
		FString::Printf(TEXT("timber must stay tension-capable (not compression-dominant): mean %g comp vs %g tens"),
			S.CompressiveStrengthMPa, S.TensileStrengthMPa),
		M.bCompressionDominant);

	/*
	 * --- and the values round-trip through the real code path ------------------
	 *
	 * A profile is behaviour, not a data blob: loading each axis in turn to ONE
	 * absolute stress and reading the utilisation back proves the numbers are wired
	 * into ComputeUtilisation, not merely stored.
	 *
	 * Each load is single-axis (CompressionOf / TensionOf / ShearOf set exactly one
	 * component), so NO cross-axis coupling can silently govern: a compression-only
	 * load reads zero on shear and tension, and a material's FrictionCoefficient is
	 * zero so compression never feeds the shear capacity either. The governing axis
	 * is therefore the one being loaded, by construction.
	 *
	 * 7 MPa is chosen so the three expected utilisations are all DISTINCT —
	 * 7/29 = 0.2414..., 7/23 = 0.3043..., 7/6 = 1.1667... — so a mis-wiring (e.g. the
	 * tensile value landing in the compressive field) moves a reading and is caught,
	 * which a common "half of each limit reads 0.5" probe would hide. Expected values
	 * divide by the PINNED MEAN constant above, independently of what the profile
	 * stores.
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
 * EVERY SHIPPED ROW FINDS ITSELF AGAIN FROM ITS VALUES ALONE — AND NOTHING ELSE DOES.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * `FindConnectionProfileRow(Row.Strength)` answers `&Row` — the SAME row, by address — for every row
 * in `AllConnectionProfiles()`, and answers null for a strength this library never shipped.
 *
 * =====================================================================================
 * WHY THE ROUND TRIP IS THE CLAIM, AND WHY IT IS A POINTER COMPARISON
 * =====================================================================================
 *
 * The lookup exists because a joint's IDENTITY is gone by the time anybody can ask: `FConnection`
 * stores a COPY of the profile it was made with, so the only route back to "which shipped row
 * fastens this" is the five numbers. That route has exactly two ways to be wrong, and one assertion
 * closes both:
 *
 *   - IT MAY FIND THE WRONG ROW. This library is siblings by construction — the bed mortar and its
 *     perpend differ on two axes, Nail/Screw/Bolt are one shape at three scales, and DryStone and
 *     the CohesionlessBond fixture differ on TENSION ALONE. A Find comparing four fields instead of
 *     five answers with a plausible neighbour, and the details window then names a joint the player
 *     did not build while every number beside it stays perfectly believable. Comparing the returned
 *     POINTER against the row the query came from is what catches that: a four-field Find drops
 *     CohesionlessBond onto DryStone's row and this sweep fails on it by name.
 *
 *   - TWO SHIPPED ROWS MAY BE FIELD-IDENTICAL. `FindConnectionProfileRow`'s header states the
 *     ambiguity and says there is nothing the function can do about it — which makes it the
 *     LIBRARY's invariant to hold, and this is where it is held. A retune that collapsed two rows
 *     onto one set of numbers would make every joint of the second read as the first, silently; here
 *     the second row's round trip comes back pointing at the first and says so.
 *
 * A SWEEP OVER `AllConnectionProfiles()` RATHER THAN A LIST, so adding a profile is adding a row and
 * it inherits both claims for free — the rule this whole file is built on.
 *
 * =====================================================================================
 * AND THE NEGATIVES, WHICH ARE WHAT MAKE THE SWEEP MEAN ANYTHING
 * =====================================================================================
 *
 * A Find that returned the first row for everything would pass nothing here, but a Find that matched
 * LOOSELY — a tolerance, a "nearest row" — would pass the sweep above and be exactly the plausible
 * lie the header forbids. So:
 *
 *   - A SCREW WITH ONE FIELD NUDGED BY 0.01 MPa IS NOT A SCREW. It is a hundredth of a megapascal
 *     from a shipped row and it is not that row, because the question is "which row IS this" and not
 *     "which row is this LIKE".
 *
 *   - A SCREW WITH ONE FIELD NaN IS NOT ANY ROW, and that is the fail-closed end. NaN compares equal
 *     to nothing, so the exact comparison rejects it by construction — the property is asserted on
 *     every one of the five fields in turn, because a Find rewritten with `FMath::IsNearlyEqual`
 *     would answer TRUE for a NaN on whichever axis it forgot (`Abs(NaN - x) <= tol` is false, but
 *     the many other shapes this comparison gets rewritten into are not all so lucky), and a joint
 *     whose strength is not a number must never read as a shipped profile.
 *
 * NO WORLD, NO TICKING SOLVER, NOTHING SOLVED. Nine rows and twelve struct copies.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProfileLibraryConnectionRowRoundTripTest,
	"DestructionGame.Core.Profiles.ConnectionRowsRoundTripByValue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FProfileLibraryConnectionRowRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace ProfileLibraryTestSupport;

	/** Which row an answer IS, by address, so a failure names the row rather than five numbers. */
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

	/* --- ONE: every shipped row finds ITSELF, by address ------------------------------------- */

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
		 * AND THE ANSWER'S OWN STRENGTH IS THE LIBRARY'S, BY ADDRESS. FNamedConnectionProfile::Strength
		 * is a REFERENCE to the shipped extern precisely so that `&FindConnectionProfileRow(S)->Strength`
		 * is the library's address rather than a pointer into a private copy — which is the defect the
		 * MATERIAL library had until its own field became a reference, and it answered "no such row"
		 * for every piece in the game while looking perfectly healthy.
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

	/* --- TWO: a strength this library never shipped is NO row -------------------------------- */

	struct FNotARowCase
	{
		FString Description;
		FConnectionStrength Strength;
	};

	const double NotANumber = std::numeric_limits<double>::quiet_NaN();

	/*
	 * A SCREW, ONE FIELD AT A TIME, ADDRESSED BY MEMBER POINTER SO THE FIVE ARE A TABLE RATHER THAN
	 * FIVE COPIES OF ONE PARAGRAPH. Screw is the subject because it is the middle of the three
	 * fasteners — a sibling on either side — so a loose match has somewhere plausible to land.
	 */
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
