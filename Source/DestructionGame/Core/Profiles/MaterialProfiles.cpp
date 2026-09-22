// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Profiles/MaterialProfiles.h"

/**
 * The material profile library: what a piece is made of. Every value cites its source.
 * Concrete C30/37 is the calibration reference (DESIGN.md §4). Strengths are mean-basis
 * (DESIGN.md §3).
 *
 * Units trap: density is g/cm3 (UPhysicalMaterial::Density), so brick is 1.9, not 1900.
 */
namespace DestructionProfiles
{
	/**
	 * Structural concrete C30/37, the calibration baseline.
	 * Density 2.4 g/cm3: EN 1991-1-1 Table A.1, 24 kN/m3.
	 * Compressive 38 MPa: EN 1992-1-1 f_cm = f_ck + 8.
	 * Shear 7.6 MPa: the common 0.2 * f_cm; push-off tests report 6-8 MPa.
	 * Tensile 3 MPa: EN 1992-1-1 f_ctm = 2.9, rounded (already a mean).
	 * Zero friction and unbounded MaxShear: Mohr-Coulomb coupling belongs to joints, not materials.
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
	 * Fired clay brick, UK metric 215 x 102.5 x 65 mm.
	 * Density 1.9 g/cm3: bulk range 1.7-2.1; gives 2.72 kg per brick.
	 * Compressive 20 MPa: BS EN 771-1 declared (normalised mean) f_b, as in Eurocode 6 examples.
	 * Tensile 2 MPa: about 0.10 * f_b. Shear 3 MPa: about 0.15 * f_b (the unit shearing
	 * through, not the bed joint sliding). Mortar is weaker on both axes, which is why
	 * connections are first-class (DESIGN.md §2).
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
	 * Structural timber C24, the second, dissimilar material (shed roof and posts).
	 * Density 0.42 g/cm3: EN 338 rho_mean 420 kg/m3 (units trap: never 420).
	 * Mean parallel-to-grain strengths (re-anchored 2026-09-02 from EN 338 characteristics
	 * 21 / 14 / 4.0), via mean = char x exp(1.645 x sqrt(ln(1 + CoV^2))) with JCSS PMC 3.5
	 * per-property CoVs: compressive f_c,0 29, tensile f_t,0 23, shear f_v 6.0. The shear
	 * mean matches BeamAcceptanceTest's independently derived 6.0.
	 * Not compression-dominant: tension (23) is close to crushing (29), unlike masonry.
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
		 * The library; add a material by adding a row. Named distinctly, not `Library`, because
		 * a unity build would merge it with ConnectionProfiles.cpp's anonymous namespace.
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
	 * Weakest-link pairing of a connection with two material faces (SHED_PATH.md B2). Tension
	 * and cohesion are the bond derated by the weaker BondFactor, capped by either material.
	 * Compression is never derated, only capped by crushing. Other fields copy unchanged.
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
