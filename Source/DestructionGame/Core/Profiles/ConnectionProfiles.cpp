// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Profiles/ConnectionProfiles.h"

/**
 * THE CONNECTION PROFILE LIBRARY — real published figures, one row per joint type.
 *
 * Every value carries its source. That is not decoration: a strength with no
 * citation is indistinguishable from a number somebody liked, and the whole
 * material x force matrix (DESIGN.md §4) is only grounded if the baseline is.
 *
 * CALIBRATION BASELINE is structural concrete C30/37 (see MaterialProfiles.cpp).
 * The masonry joints below are calibrated against the 20 MPa clay brick they are
 * laid with, which is what makes DESIGN.md §2's "the mortar gives before the
 * brick" a property of the data rather than a hope.
 *
 * UNITS. Everything here is SI megapascals, deliberately, so the numbers stay
 * checkable against the codes they came from. The conversion to Unreal force
 * units happens once, at ForceUnitsPerMPaSqCm in Core/ConnectionStrength.h, and
 * nothing in this file may repeat it.
 *
 * WHAT A PROFILE IS NOT. There is no per-material derating here — a joint's real
 * strength depends on the pair of faces it bonds to, and that weakest-link rule is
 * deliberately deferred until a second material exists to prove it (see
 * FMaterialProfile::BondFactor and CURRENT_STATE.md).
 */
namespace DestructionProfiles
{
	/**
	 * GENERAL PURPOSE CEMENT MORTAR, laid with a 20 MPa clay brick.
	 *
	 * Compressive 10 MPa — BS EN 998-2 compressive strength class M10, a standard
	 * general purpose masonry mortar designation. Cross-check against the joint it
	 * actually models: EN 1996-1-1 §3.6.1.2 gives masonry characteristic
	 * compressive strength f_k = K * f_b^0.7 * f_m^0.3, which for K = 0.55 (clay
	 * Group 1, general purpose mortar), f_b = 20 and f_m = 10 is 8.9 MPa. 10 MPa
	 * therefore sits at the top of the band this joint can reach, and — the part
	 * that matters — comfortably below the brick's own 20 MPa, so the mortar
	 * crushes first.
	 *
	 * Cohesion 0.2 MPa — EN 1996-1-1 Table 3.4, initial shear strength f_vk0 of
	 * masonry with clay units. The table gives 0.30 for M10-M20 mortar and 0.20 for
	 * M2.5-M9. 0.20 IS TAKEN DELIBERATELY rather than the 0.30 the M10 row would
	 * allow: site-mixed mortar rarely achieves the laboratory bond, triplet testing
	 * reports the lower figure far more often, and under-strength is the safe
	 * direction to be wrong in for a demolition game.
	 *
	 * Tensile 0.1 MPa — EN 1996-1-1 Table 3.2, characteristic flexural strength
	 * f_xk1 (plane of failure parallel to the bed joints, i.e. the joint being
	 * pulled open) is 0.10 N/mm2 for clay units in general purpose mortar, for both
	 * the f_m < 5 and f_m >= 5 rows. A hundredth of the compressive figure, which is
	 * why tension governs a hanging joint long before anything else does.
	 *
	 * Friction 0.6 — EN 1996-1-1 §3.6.2 uses 0.4 as the DESIGN coefficient, which
	 * is a deliberately conservative code value. Direct measurement of masonry bed
	 * joints gives friction angles of about 31-39 degrees, so tan(phi) = 0.6-0.8.
	 * 0.6 is the low end of measured rather than the code's design figure.
	 *
	 * Shear ceiling 1.3 MPa — EN 1996-1-1 §3.6.2 truncates the Mohr-Coulomb
	 * envelope at 0.065 * f_b, because past that point the UNIT gives rather than
	 * the joint sliding: 0.065 * 20 = 1.3. It is a property of the brick, not of the
	 * mortar, which is why it tracks ClayBrick's compressive strength and not
	 * anything in this row. Reached at (1.3 - 0.2) / 0.6 = 1.83 MPa of compression.
	 */
	const FConnectionStrength GeneralPurposeMortar{
		/*Compressive*/ 10.0,
		/*ShearCohesion*/ 0.2,
		/*Tensile*/ 0.1,
		/*FrictionCoefficient*/ 0.6,
		/*MaxShear*/ 1.3
	};

	/**
	 * LIME MORTAR — the soft historic binder, weaker than cement mortar on every
	 * axis that has a bond in it.
	 *
	 * Compressive 2.0 MPa — BS EN 459-1 classifies natural hydraulic limes by the
	 * 28-day compressive strength of the standard mortar, and NHL 2 means 2-7
	 * N/mm2. 2.0 is that class's floor, and it also matches BS 5628 mortar
	 * designation (iv) (1:2:9 cement:lime:sand), quoted at 2 N/mm2. Non-hydraulic
	 * lime is weaker again, at well under 1 MPa.
	 *
	 * Cohesion 0.1 MPa — EN 1996-1-1 Table 3.4's lowest band, f_vk0 = 0.10 for clay
	 * units in M1-M2 mortar, which is where a 2 MPa lime mortar lands.
	 *
	 * Tensile 0.05 MPa — EXTRAPOLATED, and said so. Table 3.2 does not go below the
	 * 0.10 it gives for general purpose mortar, so this holds the same 1:2
	 * tensile-to-cohesion ratio the cement mortar row has rather than inventing an
	 * independent figure. Flexural bond tests on lime mortar do report roughly half
	 * the cement figure.
	 *
	 * Friction 0.6 — unchanged from cement mortar on purpose. Bed joint friction is
	 * a property of the roughness of the two faces sliding, not of what binds them,
	 * and the measured range does not separate the binders.
	 *
	 * Shear ceiling 1.3 MPa — same EN 1996-1-1 truncation as above, because it is a
	 * property of the 20 MPa unit rather than of the mortar.
	 *
	 * IT IS PROVABLY UNREACHABLE, and that is the honest outcome rather than an
	 * oversight: 1.3 MPa of capacity needs (1.3 - 0.1) / 0.6 = 2.0 MPa of
	 * compression, which is EXACTLY this mortar's own crushing limit, so the
	 * compression axis reaches 1.0 no later than the shear axis reaches its ceiling.
	 * Lime mortar crushes rather than shearing off its cap.
	 *
	 * So do NOT read Profiles.ConnectionInvariants' Bonded rule — "a friction-coupled
	 * joint needs a real shear ceiling" — as meaningful behavioural coverage here. It
	 * checks the field is set to something finite, which it is; no load can reach it.
	 * If lime's ceiling ever needs to matter, it is the compressive strength that has
	 * to move, not the cap.
	 */
	const FConnectionStrength LimeMortar{
		/*Compressive*/ 2.0,
		/*ShearCohesion*/ 0.1,
		/*Tensile*/ 0.05,
		/*FrictionCoefficient*/ 0.6,
		/*MaxShear*/ 1.3
	};

	/**
	 * DRY STONE — no mortar at all, so the two bond terms are EXACT ZEROES.
	 *
	 * Not small numbers standing in for a weak bond: there is nothing there. This
	 * is the profile Mohr-Coulomb coupling exists for (DESIGN.md §3) — every bit of
	 * this wall's resistance to sliding is borrowed from the weight pressing on it,
	 * and it cannot be modelled at all with fixed per-axis strengths.
	 *
	 * Compressive 30 MPa — uniaxial compressive strength of building limestone,
	 * commonly quoted at 30-100 MPa (sandstone 20-170, granite 100-250). The soft
	 * end is the honest choice twice over: dry stone walls are built from whatever
	 * is local rather than from selected stone, and a dry joint bears on point
	 * contacts rather than across the full face, so the contact stress is far above
	 * the nominal one.
	 *
	 * Friction 0.7 — stone-on-stone sliding friction. Measured friction angles for
	 * dry masonry contacts are about 30-37 degrees, tan(phi) = 0.58-0.75; Byerlee's
	 * law puts rock-on-rock nearer 0.85 at low normal stress. 0.7 is mid-range.
	 *
	 * Shear ceiling 6.0 MPa — roughly 0.2 * UCS, the same shear-to-compressive
	 * relation used for concrete, because past this the STONE shears rather than the
	 * faces sliding. NOT the Eurocode 6 masonry truncation (0.065 * f_b would be
	 * 1.95 MPa here): that coefficient is calibrated for a mortared clay joint, where
	 * the failure past the ceiling is the unit splitting under the bed joint. A dry
	 * joint transmits shear directly through stone-to-stone contact, so the limit is
	 * the stone's own shear strength — a different mechanism, so a different relation.
	 */
	const FConnectionStrength DryStone{
		/*Compressive*/ 30.0,
		/*ShearCohesion*/ 0.0,
		/*Tensile*/ 0.0,
		/*FrictionCoefficient*/ 0.7,
		/*MaxShear*/ 6.0
	};

	/**
	 * THE MECHANICAL FASTENERS, and the one modelling approximation in this file
	 * that has to be stated out loud.
	 *
	 * A fastener's published capacity is a FORCE PER FASTENER, in kN. This model
	 * compares a stress against a strength, and has no concept of how many nails
	 * are in a joint — so each figure below is a characteristic capacity from
	 * EN 1995-1-1 smeared over a reference density of ONE FASTENER PER 100 cm2 of
	 * interface, which is the joint area the whole test suite uses. Change that
	 * assumption and every number here scales with it. It is an assumption the data
	 * cannot express today; when fastener count becomes a property of a joint, these
	 * should be re-derived rather than re-tuned.
	 *
	 * All three are calibrated in C24 softwood (EN 338: characteristic density
	 * rho_k = 350 kg/m3, f_c,90,k = 2.5 N/mm2), because EN 1995-1-1 is the only
	 * source that publishes fastener capacities at all. That does NOT make a wood
	 * material profile — pairing a connection with the material it bites into is the
	 * deferred weakest-link rule, not this.
	 *
	 * FRICTION IS EXACTLY ZERO on all three, and that is the load-bearing fact. A
	 * discrete fastener does not care how hard the two faces are pressed together,
	 * and mu = 0 reduces Mohr-Coulomb to three independent axes EXACTLY — same
	 * formula, no branch, which is what keeps connection types data rather than a
	 * second code path (DESIGN.md §3).
	 *
	 * COMPRESSION IS THE SAME 2.5 MPa FOR ALL THREE, and that is not a copy. When
	 * the joint is squeezed the two faces bear on each other and the fastener
	 * carries none of it, so the limit is the timber crushing perpendicular to the
	 * grain — EN 338's f_c,90,k for C24 — whatever is holding the pieces together.
	 * The fastener governs shear and withdrawal only.
	 *
	 * The ORDERING nail < screw < bolt on both fastener-governed axes is the
	 * gameplay hook DESIGN.md §2 promises: "the same wood frame built with nails vs.
	 * screws vs. bolts genuinely behaves differently under load."
	 *
	 * All six fastener-governed figures are quoted to TWO SIGNIFICANT FIGURES, which
	 * is already generous for characteristic values derived through a chain of
	 * empirical code formulae. Don't tighten one of them without tightening the rest;
	 * mixed precision reads as though some numbers were measured more carefully.
	 */

	/**
	 * NAIL — 3.35 mm round wire nail, 60 mm penetration, C24.
	 *
	 * Withdrawal, EN 1995-1-1 §8.3.2: f_ax,k = 20e-6 * rho_k^2 = 2.45 N/mm2, so
	 * F_ax,Rk = f_ax,k * d * t_pen = 2.45 * 3.35 * 60 = 492 N. Over 100 cm2 that is
	 * 0.049 MPa.
	 *
	 * Lateral, EN 1995-1-1 §8.2.2 (Johansen): f_h,k = 0.082 * rho_k * d^-0.3 = 19.5
	 * N/mm2, M_y,Rk = 0.3 * f_u * d^2.6 = 3960 Nmm, giving F_v,Rk ~ 827 N in the
	 * double-plastic-hinge mode. Over 100 cm2 that is 0.083 MPa.
	 *
	 * Note a nail holds LATERALLY better than it does in withdrawal, which is why
	 * nailed joints are detailed to load the fasteners in shear.
	 */
	const FConnectionStrength Nail{
		/*Compressive*/ 2.5,
		/*ShearCohesion*/ 0.083,
		/*Tensile*/ 0.049,
		/*FrictionCoefficient*/ 0.0
		/*
		 * MaxShear is left at its unbounded default here and on the two below: with
		 * mu = 0 the capacity never grows with load, so there is nothing to truncate.
		 */
	};

	/**
	 * SCREW — 6 mm diameter, 50 mm effective threaded penetration, C24.
	 *
	 * Withdrawal, EN 1995-1-1 §8.7.2: f_ax,k = 0.52 * d^-0.5 * l_ef^-0.1 *
	 * rho_k^0.8 = 15.6 N/mm2, so F_ax,Rk = f_ax,k * d * l_ef = 4670 N. Over 100 cm2
	 * that is 0.47 MPa — nearly TEN TIMES the nail, which is the whole reason
	 * decking and anything loaded axially gets screwed rather than nailed.
	 *
	 * Lateral, §8.7.1 treating the screw as a dowel of its effective diameter:
	 * f_h,k = 16.6 N/mm2, M_y,Rk = 0.3 * 600 * 6^2.6 = 17,320 Nmm, F_v,Rk ~ 2130 N.
	 * Over 100 cm2 that is 0.21 MPa.
	 */
	const FConnectionStrength Screw{
		/*Compressive*/ 2.5,
		/*ShearCohesion*/ 0.21,
		/*Tensile*/ 0.47,
		/*FrictionCoefficient*/ 0.0
	};

	/**
	 * BOLT — M12 grade 8.8 through-bolt, 50 mm C24 members, washered.
	 *
	 * Lateral, EN 1995-1-1 §8.5.1: f_h,k = 0.082 * (1 - 0.01d) * rho_k = 25.3
	 * N/mm2, M_y,Rk = 0.3 * f_u,k * d^2.6 = 153,500 Nmm, giving F_v,Rk ~ 10 kN per
	 * shear plane. Over 100 cm2 that is 1.0 MPa.
	 *
	 * Axial: the steel is nowhere near the limit (an M12 8.8 shank yields at ~67
	 * kN); what governs is the WASHER bearing on the timber at f_c,90. A 40 mm
	 * washer with the code's bearing enhancement gives roughly 12 kN. Over 100 cm2
	 * that is 1.2 MPa. This is why a through-bolt outholds both of the above — it
	 * spreads its load over a plate instead of gripping fibres.
	 */
	const FConnectionStrength Bolt{
		/*Compressive*/ 2.5,
		/*ShearCohesion*/ 1.0,
		/*Tensile*/ 1.2,
		/*FrictionCoefficient*/ 0.0
	};

	/**
	 * TEST FIXTURE ONLY — a joint that never gives. NOT A MATERIAL, and it carries
	 * no citation because there is nothing to cite.
	 *
	 * It exists so a test can measure load routing without any plausible
	 * implementation breaking a joint mid-solve, and it is classified TestFixture in
	 * the library below precisely so that being unbreakable is MACHINE-RECOGNISABLE:
	 * the well-formedness sweep demands every non-fixture profile give under absurd
	 * load, so an accidentally indestructible wall shows up as a failing test rather
	 * than as a bug report about the solver.
	 *
	 * 1e12 MPa rather than DBL_MAX on purpose. It is eight orders of magnitude past
	 * the largest stress any test applies, so it is unbreakable in practice, while
	 * leaving the arithmetic nowhere near overflow — cohesion + mu * stress at
	 * DBL_MAX is one careless mu away from producing an infinity.
	 */
	const FConnectionStrength Unbreakable{
		/*Compressive*/ 1.0e12,
		/*ShearCohesion*/ 1.0e12,
		/*Tensile*/ 1.0e12,
		/*FrictionCoefficient*/ 0.0,
		/*MaxShear*/ 1.0e12
	};

	namespace
	{
		/**
		 * The library itself. ADDING A PROFILE IS ADDING A ROW HERE — no new class,
		 * no new branch anywhere, which is DESIGN.md §2's "materials and connection
		 * types are data, not code" stated as a property of this file.
		 *
		 * Each row REFERENCES its named constant rather than repeating the numbers.
		 * A second copy of the values is exactly the drift this library was created
		 * to end, and ProfileLibraryTest checks the two paths agree.
		 *
		 * NAMED FOR ITS CONTENTS, NOT JUST "Library", AND THAT IS LOAD-BEARING. An
		 * anonymous namespace is not private to a file — it is private to a
		 * TRANSLATION UNIT, and a unity build puts many files in one. Two anonymous
		 * `Library` arrays of different types inside `DestructionProfiles` are a
		 * redefinition, verified on this toolchain:
		 *
		 *     error C2371: 'DestructionProfiles::`anonymous-namespace'::Library':
		 *     redefinition; different basic types
		 *
		 * Note that moving both into a shared named namespace would NOT fix it; the
		 * clash is on the name. Distinct names is the fix. The same reasoning applies
		 * to every file-local name in this module, not just to these two.
		 */
		const FNamedConnectionProfile ConnectionProfileLibrary[] = {
			{ TEXT("GeneralPurposeMortar"), EConnectionProfileClass::Bonded,             GeneralPurposeMortar },
			{ TEXT("LimeMortar"),           EConnectionProfileClass::Bonded,             LimeMortar },
			{ TEXT("DryStone"),             EConnectionProfileClass::Frictional,         DryStone },
			{ TEXT("Nail"),                 EConnectionProfileClass::MechanicalFastener, Nail },
			{ TEXT("Screw"),                EConnectionProfileClass::MechanicalFastener, Screw },
			{ TEXT("Bolt"),                 EConnectionProfileClass::MechanicalFastener, Bolt },
			{ TEXT("Unbreakable"),          EConnectionProfileClass::TestFixture,        Unbreakable },
		};
	}

	TArrayView<const FNamedConnectionProfile> AllConnectionProfiles()
	{
		return TArrayView<const FNamedConnectionProfile>(
			ConnectionProfileLibrary, UE_ARRAY_COUNT(ConnectionProfileLibrary));
	}
}
