// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Profiles/MaterialProfiles.h"

/**
 * THE MATERIAL PROFILE LIBRARY — what a piece is made of, as opposed to what holds
 * it on.
 *
 * Same rule as ConnectionProfiles.cpp: every value carries its source.
 *
 * DESIGN.md §4 asks for one well-characterised reference material, calibrated to
 * feel right, with everything else expressed as a ratio of it. STRUCTURAL CONCRETE
 * C30/37 is that reference, and it is already the baseline the rest of the suite
 * uses.
 *
 * DENSITY IS THE UNITS TRAP. It is g/cm3 because that is what
 * UPhysicalMaterial::Density takes, so published values go in unconverted. The same
 * brick expressed in kg/m3 is 1900 — a thousand times heavier and still a
 * plausible-looking number.
 */
namespace DestructionProfiles
{
	/**
	 * STRUCTURAL CONCRETE C30/37 — the calibration baseline.
	 *
	 * Density 2.4 g/cm3 — EN 1991-1-1 Table A.1 gives 24 kN/m3 for plain concrete
	 * (25 for reinforced), i.e. 2.4 t/m3. Matches the figure DESIGN.md §3's unit
	 * table already names.
	 *
	 * MEAN-BASIS STRENGTHS (DESIGN.md §3, re-anchored 2026-08-13), and for this
	 * material the code publishes the means itself.
	 *
	 * Compressive 38 MPa — EN 1992-1-1 Table 3.1's f_cm = f_ck + 8, the MEAN
	 * cylinder strength of the C30/37 whose characteristic f_ck = 30 gives the
	 * class its name. (The 37 is the equivalent cube strength.)
	 *
	 * Shear 7.6 MPa — concrete has no single published shear strength, so this is
	 * the widely used 0.2 coefficient applied to the mean, 0.2 * f_cm; push-off
	 * tests on 30 MPa concrete report 6-8 MPa, so the figure lands inside the
	 * measured band.
	 *
	 * Tensile 3 MPa — EN 1992-1-1 Table 3.1 gives f_ctm = 2.9 MPa for C30/37,
	 * ROUNDED UP TO 3.0 as the baseline anchor. UNCHANGED at the re-anchor: the
	 * "m" in f_ctm is "mean", so this row was mean-basis from the start and
	 * uplifting it would apply the conversion twice. The rounding is inside the
	 * material's own scatter, not a fudge: the same table's characteristic
	 * fractiles for C30/37 are f_ctk,0.05 = 2.0 and f_ctk,0.95 = 3.8, so 3.0 sits
	 * comfortably within the band, and every document that names this baseline
	 * says "~3 MPa".
	 *
	 * FrictionCoefficient is zero and MaxShear is left unbounded. A material is not
	 * a sliding interface — the Mohr-Coulomb coupling belongs to joints — so running
	 * a material through ComputeUtilisation gives three strictly independent axes.
	 */
	const FMaterialProfile StructuralConcrete{
		/*DensityGramsPerCubicCm*/ 2.4,
		FConnectionStrength{
			/*Compressive*/ 38.0,
			/*ShearCohesion*/ 7.6,
			/*Tensile*/ 3.0,
			/*FrictionCoefficient*/ 0.0
		},
		/*BondFactor*/ 1.0
	};

	/**
	 * FIRED CLAY BRICK — a standard UK metric unit, 215 x 102.5 x 65 mm.
	 *
	 * Density 1.9 g/cm3 — bulk density of fired clay brick runs 1.7-2.1 g/cm3. The
	 * cross-check that makes this the right end of that range: 1.9 over the standard
	 * unit's 1432 cm3 is 2.72 kg, which is what a brick weighs in the hand and
	 * exactly the mass the structure tests hand-set. THIS FIELD EXISTS TO REPLACE
	 * THAT HAND-SETTING, so it had better reproduce it.
	 *
	 * Compressive 20 MPa — the declared compressive strength of an ordinary clay
	 * facing unit to BS EN 771-1. The range is wide (engineering brick class B is
	 * >= 75 MPa, class A >= 125), but 20 N/mm2 is the standard figure for a general
	 * facing brick and the one Eurocode 6 worked examples use for f_b. UNCHANGED
	 * at the 2026-08-14 mean re-anchor flip, and load-bearingly so: EN 771-1 / EN 772-1
	 * declare a normalised MEAN compressive strength, so this row was mean-basis
	 * from the start, and it is why a mean-basis shear ceiling may key off it.
	 * Everything masonry-related does: GeneralPurposeMortar's cap is 0.1 x this.
	 *
	 * Tensile 2 MPa — roughly 0.10 * f_b. Direct tensile strength of clay units is
	 * usually quoted at 3-10% of compressive, and modulus of rupture at 2-5 MPa.
	 *
	 * Shear 3 MPa — roughly 0.15 * f_b, and above the tensile figure as it must be
	 * for a brittle ceramic. This is the UNIT shearing through, a different failure
	 * from the bed joint sliding, which is the mortar profile's business.
	 *
	 * A brick is a compression member and the numbers say so: 20 against 2 in
	 * tension. That order is what makes "the brick crushes" a genuinely different
	 * failure mode from "the mortar gives", and mortar is weaker than this on BOTH
	 * of those axes — 10 against 20, and 0.7 against 2 — which is DESIGN.md §2's
	 * premise for connections being first-class objects.
	 */
	const FMaterialProfile ClayBrick{
		/*DensityGramsPerCubicCm*/ 1.9,
		FConnectionStrength{
			/*Compressive*/ 20.0,
			/*ShearCohesion*/ 3.0,
			/*Tensile*/ 2.0,
			/*FrictionCoefficient*/ 0.0
		},
		/*BondFactor*/ 1.0
	};

	namespace
	{
		/**
		 * The library. Adding a material is adding a row, and each row references
		 * its named constant rather than repeating the values.
		 *
		 * NAMED FOR ITS CONTENTS rather than just "Library" — see the matching note
		 * in ConnectionProfiles.cpp. An anonymous namespace is private to a
		 * translation unit, not to a file, and a unity build merges files; two
		 * anonymous `Library` arrays of different types in `DestructionProfiles`
		 * would be a redefinition error.
		 *
		 * DELIBERATELY NO WOOD. DESIGN.md §4 wants a second, dissimilar material
		 * added exactly ONCE, to prove the directional code genuinely reads the
		 * profile rather than having brick's numbers baked in — run the same shear
		 * scenario on wood and confirm it survives where brick failed. Adding it
		 * here alongside the calibration data would spend that proof for nothing;
		 * it is its own task, together with the connection-to-material pairing rule
		 * that BondFactor is reserved for.
		 */
		const FNamedMaterialProfile MaterialProfileLibrary[] = {
			{ TEXT("StructuralConcrete"), StructuralConcrete },
			{ TEXT("ClayBrick"),          ClayBrick },
		};
	}

	TArrayView<const FNamedMaterialProfile> AllMaterialProfiles()
	{
		return TArrayView<const FNamedMaterialProfile>(
			MaterialProfileLibrary, UE_ARRAY_COUNT(MaterialProfileLibrary));
	}
}
