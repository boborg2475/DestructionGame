// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Profiles/ConnectionProfiles.h"

/**
 * The connection profile library — real published figures, one row per joint type.
 *
 * Every value carries its source; a strength with no citation is indistinguishable from a
 * number somebody liked, and the material x force matrix (DESIGN.md §4) is only grounded if
 * the baseline is.
 *
 * Strength basis is the MEAN, not the code characteristic (DESIGN.md §3, decided 2026-08-08,
 * re-anchored 2026-08-13): this project rules verdicts on what a real wall most likely does,
 * not a 5%-fractile design floor. Rows that were already means by their own standard didn't
 * move. The characteristic-to-mean scaling is NOT uniform across axes (tension x7, cohesion
 * x4.5, compression x1) — never re-derive an old reading by scaling; re-measure.
 *
 * Calibration baseline is structural concrete C30/37 (MaterialProfiles.cpp). The masonry
 * joints below are calibrated against the 20 MPa clay brick they're laid with, which is what
 * makes DESIGN.md §2's "the mortar gives before the brick" a property of the data.
 *
 * Units: everything here is SI megapascals so the numbers stay checkable against their
 * source codes. The conversion to Unreal force units happens once, at ForceUnitsPerMPaSqCm
 * in Core/ConnectionStrength.h — nothing in this file may repeat it.
 *
 * No per-material derating here: a joint's real strength depends on the pair of faces it
 * bonds to, deliberately deferred until a second material exists to prove it (see
 * FMaterialProfile::BondFactor and CURRENT_STATE.md).
 */
namespace DestructionProfiles
{
	/**
	 * General purpose cement mortar, laid with a 20 MPa clay brick.
	 *
	 * Mean-basis (DESIGN.md §3, re-anchored 2026-08-13): the bonded axes carry measured test
	 * means rather than code characteristics. Anchor: Gooch, Masia, Stewart & Lam (2023),
	 * Construction and Building Materials 386:131578 (four clay units x up to seven mortar
	 * mixes, bond wrench + triplet shear), whose M4/M6 mixes match this row's general-purpose
	 * band; companion Gooch, Masia, Stewart & Spadari (2025), ConBuildMat 489:142348
	 * (246 shear tests, 54 direct tension tests).
	 *
	 * Compressive 10 MPa — BS EN 998-2 class M10, already mean-basis, comfortably below the
	 * brick's 20 MPa so the mortar crushes first.
	 *
	 * Cohesion 0.9 MPa — measured mean shear bond at zero normal stress (campaign triplet
	 * means ~1.12 MPa, regression intercepts 0.58-1.04). EN 1996-1-1's old f_vk0 = 0.20 is a
	 * characteristic floor, not a mean — most real combinations exceed it.
	 *
	 * Tensile 0.7 MPa — mean flexural bond f_x1, agreeing between measured batch means
	 * (~0.57 MPa) and the UK NA's characteristic inverted by the campaign's own
	 * mean/characteristic ratio (~0.76). Cohesion/tension = 1.29, within noise of the
	 * campaign's measured 1.34.
	 *
	 * Friction 0.75 — centre of the measured means (0.64-1.11 across 246 shear tests); now
	 * slightly exceeds DryStone's 0.7, which is fine since the two are different interfaces.
	 *
	 * Shear ceiling 2.0 MPa = 0.1 * f_b — a mean-basis truncation: the measured mean
	 * unconfined shear bond (1.81 MPa) already exceeds EN 1996-1-1's characteristic cap
	 * (0.065 * 20 = 1.3), so that coefficient can't apply to a mean-basis envelope; 2.0 is
	 * judgement, sitting above cohesion+friction and below the brick's own 3.0 MPa shear
	 * strength. It tracks the brick because past the cap the unit gives, not the joint
	 * sliding. Reached at (2.0 - 0.9) / 0.75 = 1.47 MPa of compression, far above the
	 * 0.005-0.3 MPa these walls develop.
	 */
	const FConnectionStrength GeneralPurposeMortar{
		/*Compressive*/ 10.0,
		/*ShearCohesion*/ 0.9,
		/*Tensile*/ 0.7,
		/*FrictionCoefficient*/ 0.75,
		/*MaxShear*/ 2.0
	};

	/**
	 * General purpose mortar in a perpend — the vertical head joint, the weak link in real
	 * masonry, authored as such (owner-approved item 6b, 2026-09-02).
	 *
	 * A perpend is the vertical mortar joint between units in a course (and at a wall corner).
	 * Unlike a bed joint it isn't compressed by the courses above, is filled by hand against a
	 * face that has already gone off, and shrinks away from the unit as it cures — so in practice
	 * it's frequently unfilled or debonded; EN 1996-1-1 declines to credit a perpend in tension or
	 * shear at all. This row keeps the bed row's bearing (10.0) and friction (0.75) unchanged and
	 * knocks only the two bond axes down: shear cohesion 0.9 -> 0.2, tensile 0.7 -> 0.1, so the
	 * vertical joints give up long before the beds that carry the wall down. MaxShear stays 2.0,
	 * a property of the unit rather than the mortar.
	 */
	const FConnectionStrength GeneralPurposeMortarPerpend{
		/*Compressive*/ 10.0,
		/*ShearCohesion*/ 0.2,
		/*Tensile*/ 0.1,
		/*FrictionCoefficient*/ 0.75,
		/*MaxShear*/ 2.0
	};

	/**
	 * Lime mortar — the soft historic binder, weaker than cement mortar on every bond axis.
	 * Mean-basis, like the row above. Models a genuine NHL 2 binder specifically — it used to
	 * also cite BS 5628 (iv) (1:2:9 cement:lime:sand), but that mix measures mean flexural
	 * bonds ~0.5 MPa, 2.5x the figure below, so one row can't claim both.
	 *
	 * Compressive 2.0 MPa — BS EN 459-1's NHL 2 class means 2-7 N/mm2; 2.0 is the class floor.
	 * Unchanged at the re-anchor (a floor choice, not a characteristic to invert) — the
	 * unreachable-ceiling property below depends on it.
	 *
	 * Tensile 0.2 MPa — measured mean for NHL 2: bond-wrench campaigns report ~0.19 MPa at
	 * 6 months (0.09-0.61 across brick suction and mortar flow).
	 *
	 * Cohesion 0.27 MPa — 1.34 x 0.20, the Newcastle campaign's mean shear-to-flexural-bond
	 * ratio; no lime-specific triplet mean exists, so this is one evidence grade below its
	 * tension.
	 *
	 * Friction 0.75 — unchanged from cement mortar: bed joint friction is a property of face
	 * roughness, not binder, and the data doesn't separate them.
	 *
	 * Shear ceiling 2.0 MPa — tracks the cement row's truncation, a property of the unit.
	 *
	 * Provably unreachable, and that's the honest outcome: 2.0 MPa needs (2.0 - 0.27) / 0.75 =
	 * 2.31 MPa of compression, past this mortar's own 2.0 MPa crushing limit, so compression
	 * maxes out before shear reaches its ceiling — lime crushes rather than shearing off its
	 * cap. This survives the re-anchor only because the cap moved with cohesion.
	 *
	 * So don't read Profiles.ConnectionInvariants' Bonded rule ("a friction-coupled joint needs
	 * a real shear ceiling") as meaningful coverage here — it only checks the field is finite;
	 * no load can reach it. If lime's ceiling ever matters, the compressive strength has to move.
	 */
	const FConnectionStrength LimeMortar{
		/*Compressive*/ 2.0,
		/*ShearCohesion*/ 0.27,
		/*Tensile*/ 0.2,
		/*FrictionCoefficient*/ 0.75,
		/*MaxShear*/ 2.0
	};

	/**
	 * Dry stone — no mortar at all, so the two bond terms are exact zeroes: not small numbers
	 * standing in for a weak bond, there is nothing there. This is the profile Mohr-Coulomb
	 * coupling exists for (DESIGN.md §3) — all resistance to sliding is borrowed from the
	 * weight pressing on it, and can't be modelled with fixed per-axis strengths.
	 *
	 * Compressive 30 MPa — uniaxial strength of building limestone (commonly 30-100 MPa). The
	 * soft end is honest twice over: dry stone walls use whatever is local, and a dry joint
	 * bears on point contacts rather than the full face, so contact stress runs above nominal.
	 *
	 * Friction 0.7 — stone-on-stone sliding friction; measured dry-masonry angles run 30-37
	 * degrees (tan(phi) = 0.58-0.75), mid-range.
	 *
	 * Shear ceiling 6.0 MPa — roughly 0.2 * UCS, the same shear-to-compressive relation as
	 * concrete: past this the stone itself shears. Not the Eurocode 6 masonry truncation
	 * (0.065 * f_b would be 1.95 MPa here) — that's calibrated for a mortared joint splitting
	 * the unit; a dry joint transmits shear through stone-to-stone contact directly, a
	 * different mechanism.
	 */
	const FConnectionStrength DryStone{
		/*Compressive*/ 30.0,
		/*ShearCohesion*/ 0.0,
		/*Tensile*/ 0.0,
		/*FrictionCoefficient*/ 0.7,
		/*MaxShear*/ 6.0
	};

	/**
	 * The mechanical fasteners, and the one approximation this file has to state out loud.
	 *
	 * A fastener's published capacity is a force per fastener (kN); this model compares a
	 * stress against a strength with no concept of fastener count, so each figure is a
	 * characteristic capacity from EN 1995-1-1 smeared over ONE FASTENER PER 100 cm2 — the
	 * joint area the test suite uses. Change that assumption and every number scales with it.
	 *
	 * All three are calibrated in C24 softwood at its MEAN density: EN 338 publishes both
	 * rho_k = 350 and rho_mean = 420 kg/m3, so the mean-basis route (DESIGN.md §3) feeds the
	 * same EN 1995-1-1 formulae the published mean instead of the fractile (a 1.20 density
	 * ratio). EN 1995-1-1 is used because it's the only source publishing fastener capacities
	 * at all — that does NOT make a wood material profile; the fastener steel side has no
	 * published mean/characteristic ratio, so these lateral figures are density-only uplifts
	 * and slight under-estimates of the true means.
	 *
	 * Friction is exactly zero on all three — a discrete fastener doesn't care how hard the
	 * faces press together, and mu = 0 reduces Mohr-Coulomb to three independent axes exactly,
	 * keeping connection types data rather than a second code path.
	 *
	 * Compression is the same 3.4 MPa for all three, and that's not a copy: squeezed, the
	 * faces bear on each other directly, so the limit is timber crushing perpendicular to
	 * grain (JCSS E[R_c,90] = 0.008 * 420 = 3.36 MPa). The fastener governs shear and
	 * withdrawal only.
	 *
	 * The ordering nail < screw < bolt on both fastener-governed axes is the gameplay hook
	 * DESIGN.md §2 promises. Figures are quoted to two significant figures — don't tighten
	 * one without tightening the rest.
	 */

	/**
	 * Nail — 3.35 mm round wire nail, 60 mm penetration, C24.
	 *
	 * Withdrawal, EN 1995-1-1 §8.3.2: f_ax = 20e-6 * rho^2 is quadratic in density, so the
	 * characteristic figure (0.049 MPa over 100 cm2) scales by (420/350)^2 = 1.44 to 0.071 MPa.
	 *
	 * Lateral, §8.2.2 (Johansen): f_h is linear in density and capacity goes as sqrt(f_h * M_y),
	 * so the characteristic figure (0.083 MPa) scales by sqrt(1.2) = 1.10 to 0.091 MPa.
	 *
	 * A nail holds laterally better than in withdrawal, which is why nailed joints are detailed
	 * to load the fasteners in shear.
	 */
	const FConnectionStrength Nail{
		/*Compressive*/ 3.4,
		/*ShearCohesion*/ 0.091,
		/*Tensile*/ 0.071,
		/*FrictionCoefficient*/ 0.0
		// MaxShear stays unbounded here and below — with mu = 0, capacity never grows with load, so there is nothing to truncate.
	};

	/**
	 * Screw — 6 mm diameter, 50 mm effective threaded penetration, C24.
	 *
	 * Withdrawal, EN 1995-1-1 §8.7.2: f_ax = 0.52 * d^-0.5 * l_ef^-0.1 * rho^0.8, so the
	 * characteristic figure (0.47 MPa over 100 cm2) scales by (420/350)^0.8 = 1.16 to 0.54 MPa —
	 * nearly eight times the nail, the whole reason decking gets screwed rather than nailed.
	 *
	 * Lateral, §8.7.1, treating the screw as a dowel of its effective diameter: 0.21 MPa over
	 * 100 cm2, scaling by the same sqrt(1.2) = 1.10 as the nail to 0.23 MPa.
	 */
	const FConnectionStrength Screw{
		/*Compressive*/ 3.4,
		/*ShearCohesion*/ 0.23,
		/*Tensile*/ 0.54,
		/*FrictionCoefficient*/ 0.0
	};

	/**
	 * Bolt — M12 grade 8.8 through-bolt, 50 mm C24 members, washered.
	 *
	 * Lateral, EN 1995-1-1 §8.5.1: f_h is linear in density, so the characteristic figure
	 * (1.0 MPa over 100 cm2) scales by 420/350 = 1.10 to 1.1 MPa.
	 *
	 * Axial: steel is nowhere near the limit (M12 8.8 shank yields ~67 kN); what governs is the
	 * washer bearing on timber at f_c,90. A 40 mm washer gives roughly 1.2 MPa over 100 cm2,
	 * scaling with mean f_c,90 (3.36/2.5 = 1.34) to 1.61 MPa. This is why a through-bolt outholds
	 * both of the above — it spreads its load over a plate instead of gripping fibres.
	 */
	const FConnectionStrength Bolt{
		/*Compressive*/ 3.4,
		/*ShearCohesion*/ 1.1,
		/*Tensile*/ 1.61,
		/*FrictionCoefficient*/ 0.0
	};

	/**
	 * Test fixture only — a joint that never gives. Not a material, and carries no citation
	 * because there is nothing to cite.
	 *
	 * Exists so a test can measure load routing without any plausible implementation breaking a
	 * joint mid-solve. Classified TestFixture precisely so being unbreakable is machine-
	 * recognisable: the well-formedness sweep demands every non-fixture profile give under absurd
	 * load, so an accidentally indestructible wall shows up as a failing test.
	 *
	 * 1e12 MPa rather than DBL_MAX on purpose: eight orders of magnitude past the largest stress
	 * any test applies (unbreakable in practice), while staying nowhere near overflow — cohesion +
	 * mu * stress at DBL_MAX is one careless mu away from producing an infinity.
	 */
	const FConnectionStrength Unbreakable{
		/*Compressive*/ 1.0e12,
		/*ShearCohesion*/ 1.0e12,
		/*Tensile*/ 1.0e12,
		/*FrictionCoefficient*/ 0.0,
		/*MaxShear*/ 1.0e12
	};

	/**
	 * Test fixture only — zero cohesion and a real tensile bond, which no material has. No
	 * citation; there is nothing to cite.
	 *
	 * Exists because DryStone can't answer the question: composite vertical action needs shear
	 * transfer between courses, so a joint with no bond should get little or none of it
	 * (COMPOSITE_DEPTH_DESIGN.md slice 5). But DryStone.TensileStrengthMPa is an exact zero, so
	 * a dry-laid corbel is condemned at any section modulus, and no fixture laid in it can tell
	 * a bounded composite depth from an unbounded one. This row is DryStone with the one field
	 * changed that blinds it: zero cohesion, a real tensile bond.
	 *
	 * Tensile 0.40 MPa, deliberately not 0.70: matching general purpose mortar would make the
	 * two rows read identically, so an ordering test could pass on a model that ignores the
	 * profile. Sitting strictly between lime's 0.20 and cement's 0.70 is where that's hardest
	 * by accident.
	 *
	 * Classified TestFixture rather than Frictional, because Profiles.ConnectionInvariants
	 * requires a Frictional row to have exactly zero tensile strength — the correct physics for
	 * real dry stone, and precisely the property this row has to break.
	 *
	 * The other figures are DryStone's, unchanged, so a comparison measures one number, not four.
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
		 * The library itself. Adding a profile is adding a row here — no new class, no new
		 * branch, DESIGN.md §2's "materials and connection types are data, not code" as a
		 * property of this file. Each row references its named constant rather than repeating
		 * the numbers, and ProfileLibraryTest checks the two paths agree.
		 *
		 * Named for its contents, not just "Library" — an anonymous namespace is private to a
		 * translation unit, not a file, and a unity build puts many files in one; two anonymous
		 * `Library` arrays of different types inside `DestructionProfiles` redefine each other
		 * (verified: C2371). The fix is a distinct name, not a shared namespace — the clash is
		 * on the name. Same reasoning applies to every file-local name in this module.
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
		 * The library is walked rather than the seven externs compared, so a connection profile stays
		 * data — a chain of `== GeneralPurposeMortar` tests would be a branch per profile, and its
		 * failure mode is quiet: a row added to the library would read as no row at all.
		 *
		 * All five fields, and that's the point rather than thoroughness: this library is siblings by
		 * construction (the bed mortar and its perpend differ on two axes; Nail/Screw/Bolt are one
		 * shape at three scales), so comparing fewer fields would name a plausible neighbour, worse
		 * than naming none.
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
