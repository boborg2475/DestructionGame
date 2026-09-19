// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Profiles/MaterialProfiles.h"

/**
 * The material profile library — what a piece is made of, as opposed to what holds it on.
 *
 * Same rule as ConnectionProfiles.cpp: every value carries its source.
 *
 * DESIGN.md §4 asks for one well-characterised reference material, calibrated to feel right,
 * with everything else expressed as a ratio of it. Structural concrete C30/37 is that
 * reference, and already the baseline the rest of the suite uses.
 *
 * Density is the units trap: it's g/cm3 because that's what UPhysicalMaterial::Density takes,
 * so published values go in unconverted. The same brick in kg/m3 is 1900 — a thousand times
 * heavier and still a plausible-looking number.
 */
namespace DestructionProfiles
{
	/**
	 * Structural concrete C30/37 — the calibration baseline.
	 *
	 * Density 2.4 g/cm3 — EN 1991-1-1 Table A.1 gives 24 kN/m3 for plain concrete, i.e.
	 * 2.4 t/m3. Matches DESIGN.md §3's unit table.
	 *
	 * Mean-basis strengths (DESIGN.md §3, re-anchored 2026-08-13); for this material the code
	 * publishes the means itself.
	 *
	 * Compressive 38 MPa — EN 1992-1-1's f_cm = f_ck + 8, the mean cylinder strength of the
	 * C30/37 whose characteristic f_ck = 30 gives the class its name.
	 *
	 * Shear 7.6 MPa — concrete has no single published shear strength, so this is the widely
	 * used 0.2 * f_cm; push-off tests on 30 MPa concrete report 6-8 MPa, inside the band.
	 *
	 * Tensile 3 MPa — EN 1992-1-1 gives f_ctm = 2.9 MPa for C30/37, rounded up to 3.0.
	 * Unchanged at the re-anchor: the "m" in f_ctm is "mean", so this row was mean-basis from
	 * the start, and the rounding sits inside the material's own scatter (characteristic
	 * fractiles run 2.0-3.8).
	 *
	 * FrictionCoefficient is zero and MaxShear unbounded: a material is not a sliding
	 * interface — Mohr-Coulomb coupling belongs to joints — so ComputeUtilisation gives three
	 * strictly independent axes.
	 */
	const FMaterialProfile StructuralConcrete{
		/*DensityGramsPerCubicCm*/ 2.4,
		FConnectionStrength{
			/*Compressive*/ 38.0,
			/*ShearCohesion*/ 7.6,
			/*Tensile*/ 3.0,
			/*FrictionCoefficient*/ 0.0
		},
		/*BondFactor*/ 1.0,
		/*bCompressionDominant*/ true
	};

	/**
	 * Fired clay brick — a standard UK metric unit, 215 x 102.5 x 65 mm.
	 *
	 * Density 1.9 g/cm3 — bulk density of fired clay brick runs 1.7-2.1 g/cm3. Cross-check:
	 * 1.9 over the standard unit's 1432 cm3 is 2.72 kg, matching what a brick weighs in the
	 * hand and the mass the structure tests hand-set — this field replaces that hand-setting.
	 *
	 * Compressive 20 MPa — declared compressive strength of an ordinary clay facing unit to
	 * BS EN 771-1, and what Eurocode 6 worked examples use for f_b. Unchanged at the
	 * re-anchor: EN 771-1 / EN 772-1 already declare a normalised mean strength — and it's why
	 * a mean-basis shear ceiling may key off it (GeneralPurposeMortar's cap is 0.1 x this).
	 *
	 * Tensile 2 MPa — roughly 0.10 * f_b, in the usual 3-10% of compressive for clay units.
	 *
	 * Shear 3 MPa — roughly 0.15 * f_b, above tensile as it must be for a brittle ceramic. This
	 * is the unit shearing through, a different failure from the bed joint sliding (the
	 * mortar's business).
	 *
	 * A brick is a compression member: 20 against 2 in tension, which is what makes "the brick
	 * crushes" different from "the mortar gives" — mortar is weaker than this on both axes (10
	 * against 20, 0.7 against 2), DESIGN.md §2's premise for connections being first-class.
	 */
	const FMaterialProfile ClayBrick{
		/*DensityGramsPerCubicCm*/ 1.9,
		FConnectionStrength{
			/*Compressive*/ 20.0,
			/*ShearCohesion*/ 3.0,
			/*Tensile*/ 2.0,
			/*FrictionCoefficient*/ 0.0
		},
		/*BondFactor*/ 1.0,
		/*bCompressionDominant*/ true
	};

	/**
	 * Structural timber, strength class C24 — the deliberate second structural material, and
	 * the softwood the shed's roof and posts are cut from.
	 *
	 * Density 0.42 g/cm3 — EN 338's C24 mean density rho_mean = 420 kg/m3, i.e. 0.42 g/cm3 in
	 * UPhysicalMaterial's own unit — not the 350 kg/m3 characteristic density the same table
	 * lists for connection design. Units trap: 0.42, never 420.
	 *
	 * The strengths are mean axial parallel-to-grain capacities, mapped onto FConnectionStrength
	 * the same way concrete and brick map theirs:
	 *
	 * Compressive 29 MPa — f_c,0 mean.
	 *
	 * Tensile 23 MPa — f_t,0 mean. This is why timber is NOT compression-dominant: wood
	 * genuinely carries tension along the grain at a large fraction of its crushing strength
	 * (23 against 29), where masonry pulls apart at a tenth of it — the whole point of a
	 * second, dissimilar material proving the directional code reads the profile rather than
	 * having masonry's ratios baked in.
	 *
	 * Shear 6.0 MPa — f_v mean, stored in ShearCohesionMPa, the same convention concrete's 7.6
	 * and brick's 3.0 follow.
	 *
	 * Mean basis — the 2026-09-02 re-anchor (item 6a), the last profile row onto the same
	 * footing the masonry rows were flipped to on 2026-08-13/14 (DESIGN.md §3): measured means,
	 * not code characteristics, because verdicts ruled at 5-percentile design values are 3-8x
	 * pessimistic against real members. Retired figures were EN 338 C24's characteristic
	 * f_c,0,k 21 / f_t,0,k 14 / f_v,k 4.0.
	 *
	 * The char -> mean factor is per-property, not a blanket scale, since CoV differs by
	 * property (EN 384/EN 1990: mean = char x exp(1.645 x sqrt(ln(1 + CoV^2)))). JCSS PMC
	 * Part 3.5's per-property CoVs give f_c,0 (0.20) -> 29, f_t,0 (0.30) -> 23, f_v (0.25) ->
	 * 6.0. The shear factor cross-checks against BeamAcceptanceTest: EN 338's f_m,k 24 through
	 * the same factor is 36.0, exactly that fixture's independently derived C24 mean bending
	 * and its C24ShearMPa 6.0 — two files landing on the same shear mean by the same JCSS CoV,
	 * derived apart, anchors it.
	 *
	 * Not the member-bending 36/6 the beam-acceptance fixtures use for the axial fields —
	 * that's a whole-stress-block derivation for a different limit state; this profile is the
	 * axial/shear joint-field convention.
	 *
	 * FrictionCoefficient zero and MaxShear unbounded, like the other materials. BondFactor 1.0
	 * until the connection-to-material pairing rule (SHED_PATH.md B2) makes it live.
	 */
	const FMaterialProfile Timber{
		/*DensityGramsPerCubicCm*/ 0.42,
		FConnectionStrength{
			/*Compressive*/ 29.0,
			/*ShearCohesion*/ 6.0,
			/*Tensile*/ 23.0,
			/*FrictionCoefficient*/ 0.0
		},
		/*BondFactor*/ 1.0,
		/*bCompressionDominant*/ false
	};

	namespace
	{
		/**
		 * The library. Adding a material is adding a row, referencing its named constant rather than
		 * repeating the values.
		 *
		 * Named for its contents rather than just "Library" — see the matching note in
		 * ConnectionProfiles.cpp; an anonymous namespace is private to a translation unit, not a file,
		 * and a unity build merges files, so two anonymous `Library` arrays of different types in
		 * `DestructionProfiles` would redefine each other.
		 *
		 * Timber is the second, dissimilar material DESIGN.md §4 asks for, added exactly once
		 * (SHED_PATH.md B1): it proves the directional code reads the profile rather than having
		 * masonry's numbers baked in, and the sweep's compression-member invariant keys off
		 * bCompressionDominant so it still bites a bad masonry profile without condemning this one.
		 * The BondFactor pairing rule stays deferred to B2.
		 */
		const FNamedMaterialProfile MaterialProfileLibrary[] = {
			{ TEXT("StructuralConcrete"), StructuralConcrete },
			{ TEXT("ClayBrick"),          ClayBrick },
			{ TEXT("Timber"),             Timber },
		};
	}

	TArrayView<const FNamedMaterialProfile> AllMaterialProfiles()
	{
		return TArrayView<const FNamedMaterialProfile>(
			MaterialProfileLibrary, UE_ARRAY_COUNT(MaterialProfileLibrary));
	}
}

namespace DestructionForce
{
	/*
	 * The connection x material weakest-link pairing (SHED_PATH.md B2).
	 *
	 * A bonded joint carries load through its connection against two material faces and gives at
	 * whichever link is weakest. The split across axes is physical, not uniform: tension and
	 * shear-cohesion are a bond that peels/slides, so those axes are the connection's bond capacity
	 * derated by the weaker face's BondFactor, then capped by either material's own tensile/shear
	 * capacity — the block itself can tear or shear through before the bond does. Compression bears
	 * through the face regardless of the bond, so it's never derated, only capped by crushing
	 * strength.
	 *
	 * Friction and the shear ceiling carry from the connection unchanged. Copying the connection
	 * first carries any field this rule doesn't touch verbatim, which is why construction is by
	 * assignment rather than a positional aggregate — no risk of transposing tensile and shear.
	 */
	FConnectionStrength EffectiveBondedStrength(
		const FConnectionStrength& Connection,
		const DestructionProfiles::FMaterialProfile& FaceA,
		const DestructionProfiles::FMaterialProfile& FaceB)
	{
		const double MinBondFactor = FMath::Min(FaceA.BondFactor, FaceB.BondFactor);

		FConnectionStrength Effective = Connection;

		Effective.TensileStrengthMPa = FMath::Min3(
			Connection.TensileStrengthMPa * MinBondFactor,
			FaceA.Strength.TensileStrengthMPa,
			FaceB.Strength.TensileStrengthMPa);

		Effective.ShearCohesionMPa = FMath::Min3(
			Connection.ShearCohesionMPa * MinBondFactor,
			FaceA.Strength.ShearCohesionMPa,
			FaceB.Strength.ShearCohesionMPa);

		Effective.CompressiveStrengthMPa = FMath::Min3(
			Connection.CompressiveStrengthMPa,
			FaceA.Strength.CompressiveStrengthMPa,
			FaceB.Strength.CompressiveStrengthMPa);

		return Effective;
	}
}
