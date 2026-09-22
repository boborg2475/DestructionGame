// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Profiles/ConnectionProfiles.h"

/**
 * The connection profile library: published figures, one row per joint type, each cited.
 *
 * Strength basis is the mean, not the code characteristic (DESIGN.md §3, re-anchored
 * 2026-08-13). The characteristic-to-mean scaling differs per axis (tension x7, cohesion x4.5,
 * compression x1), so never re-derive an old reading by scaling; re-measure.
 *
 * Masonry joints are calibrated against the 20 MPa clay brick, so "the mortar gives before the
 * brick" (DESIGN.md §2) is a property of the data.
 *
 * Units are SI MPa. Conversion to Unreal force units happens only at ForceUnitsPerMPaSqCm in
 * Core/ConnectionStrength.h; never repeat it here. No per-material derating yet (see
 * FMaterialProfile::BondFactor and CURRENT_STATE.md).
 */
namespace DestructionProfiles
{
	/**
	 * General purpose cement mortar, laid with a 20 MPa clay brick. Mean-basis; anchor: Gooch,
	 * Masia, Stewart & Lam (2023), ConBuildMat 386:131578 (M4/M6 mixes), with Gooch et al. (2025),
	 * ConBuildMat 489:142348 (246 shear, 54 tension tests).
	 *
	 * Compressive 10 MPa: BS EN 998-2 class M10, below the brick's 20 so the mortar crushes first.
	 * Cohesion 0.9 MPa: measured mean shear bond at zero normal stress (intercepts 0.58-1.04).
	 * Tensile 0.7 MPa: mean flexural bond f_x1. Friction 0.75: centre of measured 0.64-1.11.
	 *
	 * Shear ceiling 2.0 MPa = 0.1 * f_b, a judgement: EN 1996-1-1's 0.065 * f_b is a
	 * characteristic cap below the measured mean. Sits under the brick's 3.0 MPa shear strength;
	 * reached only at 1.47 MPa compression, far above the 0.005-0.3 MPa walls develop.
	 */
	const FConnectionStrength GeneralPurposeMortar{
		/*Compressive*/ 10.0,
		/*ShearCohesion*/ 0.9,
		/*Tensile*/ 0.7,
		/*FrictionCoefficient*/ 0.75,
		/*MaxShear*/ 2.0
	};

	/**
	 * General purpose mortar in a perpend (vertical head joint), the weak link in real masonry.
	 * Perpends are often unfilled or debonded, and EN 1996-1-1 credits them no tension or shear.
	 * Same bearing and friction as the bed row; only the bond axes drop (cohesion 0.9 -> 0.2,
	 * tensile 0.7 -> 0.1).
	 */
	const FConnectionStrength GeneralPurposeMortarPerpend{
		/*Compressive*/ 10.0,
		/*ShearCohesion*/ 0.2,
		/*Tensile*/ 0.1,
		/*FrictionCoefficient*/ 0.75,
		/*MaxShear*/ 2.0
	};

	/**
	 * Lime mortar (NHL 2), weaker than cement mortar on every bond axis. Mean-basis.
	 *
	 * Compressive 2.0 MPa: the BS EN 459-1 NHL 2 class floor. Tensile 0.2 MPa: measured
	 * bond-wrench mean (~0.19 at 6 months). Cohesion 0.27 MPa: 1.34 x tensile, the measured
	 * shear-to-flexural ratio (no lime triplet data). Friction 0.75: same as cement (face
	 * roughness, not binder). Shear ceiling 2.0 MPa: tracks the cement row.
	 *
	 * The ceiling is unreachable: it needs (2.0 - 0.27) / 0.75 = 2.31 MPa of compression, past
	 * lime's 2.0 MPa crushing limit. Profiles.ConnectionInvariants only checks it is finite.
	 */
	const FConnectionStrength LimeMortar{
		/*Compressive*/ 2.0,
		/*ShearCohesion*/ 0.27,
		/*Tensile*/ 0.2,
		/*FrictionCoefficient*/ 0.75,
		/*MaxShear*/ 2.0
	};

	/**
	 * Dry stone: no mortar, so both bond terms are exactly zero and all sliding resistance comes
	 * from friction (the case Mohr-Coulomb coupling exists for, DESIGN.md §3).
	 *
	 * Compressive 30 MPa: soft end of building limestone (30-100), since dry joints bear on point
	 * contacts. Friction 0.7: measured dry-masonry angles 30-37 degrees. Shear ceiling 6.0 MPa:
	 * ~0.2 * UCS, where the stone itself shears (not Eurocode 6's mortared-joint 0.065 * f_b).
	 */
	const FConnectionStrength DryStone{
		/*Compressive*/ 30.0,
		/*ShearCohesion*/ 0.0,
		/*Tensile*/ 0.0,
		/*FrictionCoefficient*/ 0.7,
		/*MaxShear*/ 6.0
	};

	/*
	 * Mechanical fasteners. Published capacities are per fastener (kN); each figure here is an
	 * EN 1995-1-1 capacity smeared over one fastener per 100 cm2. Change that and every number
	 * scales.
	 *
	 * Calibrated in C24 softwood at mean density (420 vs characteristic 350 kg/m3). The steel
	 * side has no published mean ratio, so lateral figures are density-only uplifts, slightly low.
	 *
	 * Friction is zero (a fastener ignores clamping), which reduces Mohr-Coulomb to independent
	 * axes. Compression is 3.4 MPa for all three: timber crushing perpendicular to grain
	 * (0.008 * 420 = 3.36). Nail < screw < bolt on the fastener axes (DESIGN.md §2). Two
	 * significant figures throughout.
	 */

	/**
	 * Nail: 3.35 mm round wire, 60 mm penetration, C24. Withdrawal (EN 1995-1-1 §8.3.2, quadratic
	 * in density): 0.049 x 1.44 = 0.071 MPa. Lateral (§8.2.2, Johansen): 0.083 x sqrt(1.2) =
	 * 0.091 MPa.
	 */
	const FConnectionStrength Nail{
		/*Compressive*/ 3.4,
		/*ShearCohesion*/ 0.091,
		/*Tensile*/ 0.071,
		/*FrictionCoefficient*/ 0.0
		// MaxShear unbounded here and below: with mu = 0 capacity never grows with load.
	};

	/**
	 * Screw: 6 mm, 50 mm threaded penetration, C24. Withdrawal (EN 1995-1-1 §8.7.2, rho^0.8):
	 * 0.47 x 1.16 = 0.54 MPa, nearly eight times the nail. Lateral (§8.7.1, as a dowel):
	 * 0.21 x 1.10 = 0.23 MPa.
	 */
	const FConnectionStrength Screw{
		/*Compressive*/ 3.4,
		/*ShearCohesion*/ 0.23,
		/*Tensile*/ 0.54,
		/*FrictionCoefficient*/ 0.0
	};

	/**
	 * Bolt: M12 grade 8.8 through-bolt, 50 mm C24 members, washered. Lateral (EN 1995-1-1 §8.5.1):
	 * 1.0 scaled to 1.1 MPa. Axial is governed by the 40 mm washer bearing at f_c,90, not the
	 * steel: 1.2 x 1.34 = 1.61 MPa.
	 */
	const FConnectionStrength Bolt{
		/*Compressive*/ 3.4,
		/*ShearCohesion*/ 1.1,
		/*Tensile*/ 1.61,
		/*FrictionCoefficient*/ 0.0
	};

	/**
	 * Test fixture only: a joint that never gives, so tests can measure load routing. Classified
	 * TestFixture so the sweep requiring every real profile to break can exempt it. 1e12 MPa, not
	 * DBL_MAX, so cohesion + mu * stress cannot overflow to infinity.
	 */
	const FConnectionStrength Unbreakable{
		/*Compressive*/ 1.0e12,
		/*ShearCohesion*/ 1.0e12,
		/*Tensile*/ 1.0e12,
		/*FrictionCoefficient*/ 0.0,
		/*MaxShear*/ 1.0e12
	};

	/**
	 * Test fixture only: DryStone with a real tensile bond, which no material has. DryStone's zero
	 * tension condemns a corbel at any section modulus, so it cannot test composite depth
	 * (COMPOSITE_DEPTH_DESIGN.md slice 5). Tensile 0.40 MPa sits between lime and cement so no
	 * ordering test passes by accident. TestFixture, since a Frictional row must have zero tension.
	 */
	const FConnectionStrength CohesionlessBond{
		/*Compressive*/ 30.0,
		/*ShearCohesion*/ 0.0,
		/*Tensile*/ 0.4,
		/*FrictionCoefficient*/ 0.7,
		/*MaxShear*/ 6.0
	};

	namespace
	{
		/**
		 * The library. Adding a profile is adding a row (DESIGN.md §2: data, not code). Named
		 * distinctly because a unity build merges anonymous namespaces across files (C2371).
		 */
		const FNamedConnectionProfile ConnectionProfileLibrary[] = {
			{ TEXT("GeneralPurposeMortar"),        EConnectionProfileClass::Bonded,             GeneralPurposeMortar },
			{ TEXT("GeneralPurposeMortarPerpend"), EConnectionProfileClass::Bonded,             GeneralPurposeMortarPerpend },
			{ TEXT("LimeMortar"),                  EConnectionProfileClass::Bonded,             LimeMortar },
			{ TEXT("DryStone"),                    EConnectionProfileClass::Frictional,         DryStone },
			{ TEXT("Nail"),                        EConnectionProfileClass::MechanicalFastener, Nail },
			{ TEXT("Screw"),                       EConnectionProfileClass::MechanicalFastener, Screw },
			{ TEXT("Bolt"),                        EConnectionProfileClass::MechanicalFastener, Bolt },
			{ TEXT("Unbreakable"),                 EConnectionProfileClass::TestFixture,        Unbreakable },
			{ TEXT("CohesionlessBond"),            EConnectionProfileClass::TestFixture,        CohesionlessBond },
		};
	}

	TArrayView<const FNamedConnectionProfile> AllConnectionProfiles()
	{
		return TArrayView<const FNamedConnectionProfile>(
			ConnectionProfileLibrary, UE_ARRAY_COUNT(ConnectionProfileLibrary));
	}

	const FNamedConnectionProfile* FindConnectionProfileRow(const FConnectionStrength& Strength)
	{
		/*
		 * Walks the library so new rows are found without new branches. Compares all five fields:
		 * rows are near-siblings, so fewer fields could name a plausible neighbour.
		 */
		for (const FNamedConnectionProfile& Row : AllConnectionProfiles())
		{
			if (Row.Strength.CompressiveStrengthMPa == Strength.CompressiveStrengthMPa
				&& Row.Strength.ShearCohesionMPa == Strength.ShearCohesionMPa
				&& Row.Strength.TensileStrengthMPa == Strength.TensileStrengthMPa
				&& Row.Strength.FrictionCoefficient == Strength.FrictionCoefficient
				&& Row.Strength.MaxShearStrengthMPa == Strength.MaxShearStrengthMPa)
			{
				return &Row;
			}
		}

		return nullptr;
	}
}
