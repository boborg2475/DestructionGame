// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Profiles/ConnectionProfiles.h"

/**
 * THE CONNECTION PROFILE LIBRARY — real published figures, one row per joint type.
 *
 * Every value carries its source. That is not decoration: a strength with no
 * citation is indistinguishable from a number somebody liked, and the whole
 * material x force matrix (DESIGN.md §4) is only grounded if the baseline is.
 *
 * THE STRENGTH BASIS IS THE MEAN, NOT THE CODE CHARACTERISTIC (DESIGN.md §3,
 * decided 2026-08-08, re-anchored 2026-08-13): this project rules verdicts on
 * what a real wall most likely does, not on a 5%-fractile design floor. Rows
 * that were already means by their own standard (declared mortar and unit
 * classes) did not move — uplifting one of those would apply the conversion
 * twice. The characteristic-to-mean scaling is NOT uniform across axes
 * (tension moved x7, cohesion x4.5, compression x1), so never re-derive an
 * old reading by scaling; re-measure.
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
	 * MEAN-BASIS STRENGTHS (DESIGN.md §3, re-anchored 2026-08-13): verdicts in this
	 * project are ruled at MEAN strength for realism, so the bonded axes carry
	 * measured test means rather than code characteristics. The anchor campaign is
	 * Gooch, Masia, Stewart & Lam (2023), Construction and Building Materials
	 * 386:131578 — four clay unit types x up to seven mortar mixes, five specimens
	 * per combination, bond wrench to AS 3700 and unconfined triplet shear to
	 * EN 1052-3 — whose mixes 2 (1:1:6) and 5 (1:0.5:4.5) are BS 5628 designations
	 * (iii) and (ii), i.e. exactly the M4/M6 general-purpose band this row claims.
	 * Its companion is Gooch, Masia, Stewart & Spadari (2025), ConBuildMat
	 * 489:142348 (246 shear tests, 54 direct tension tests). That mix mapping is
	 * what makes these measured means citations rather than analogies.
	 *
	 * Compressive 10 MPa — BS EN 998-2 compressive strength class M10, a standard
	 * general purpose masonry mortar designation. UNCHANGED at the re-anchor:
	 * EN 998-2 / EN 1015-11 declare a MEAN compressive class, so uplifting it
	 * would apply the mean conversion twice. Still comfortably below the brick's
	 * own 20 MPa, so the mortar crushes first.
	 *
	 * Cohesion 0.9 MPa — measured mean shear bond at zero normal stress, two
	 * methods agreeing: the 2023 campaign's M4/M6 unconfined triplet means average
	 * 1.117 MPa (all-mortar extruded-clay average ~1.0), and the 2025 paper's
	 * EN 1052-3 regression intercepts span 0.58-1.04. 0.90 is the centre of the
	 * regression range, just under the triplet average. (EN 1996-1-1 Table 3.4's
	 * f_vk0 = 0.20, which this row used to carry, is a characteristic FLOOR: the
	 * campaign's own per-combination characteristics run 0.08-1.27 and mostly
	 * 0.3-0.9, so nearly every real clay/GP-mortar combination exceeds it.)
	 *
	 * Tensile 0.7 MPa — mean flexural bond strength f_x1 (plane of failure
	 * parallel to the bed joints, i.e. the joint being pulled open), the centre of
	 * two independent routes that bracket it: (a) MEASURED — the campaign's twelve
	 * M4/M6 batch means on extruded clay average 0.571 MPa; (b) INVERTED — UK NA
	 * Table NA.6's characteristic 0.4 x the campaign's own mean/characteristic
	 * ratio 1.89 = 0.76. The mean pair 0.70/0.90 has cohesion/tension = 1.29,
	 * within noise of the campaign's measured mean ratio of 1.34, so the two bond
	 * axes moved together rather than one running away.
	 *
	 * Friction 0.75 — the centre of the measured means: initial (regression)
	 * friction 0.64-1.00 and residual friction 0.60-1.11 (Normal, COV 0.14, over
	 * 246 shear tests; Gooch et al. 2025). NOTE THE ORDERING against DryStone's
	 * 0.7: mortar now slightly exceeds dry stone, and that is not an error to
	 * "fix" — a mortar-to-brick interface and a stone-to-stone one are different
	 * faces, and each figure is mid-range for its own measurements.
	 *
	 * Shear ceiling 2.0 MPa = 0.1 * f_b — a MEAN-basis truncation, forced by
	 * measurement: the campaign's mean UNCONFINED shear bond reaches 1.81 MPa,
	 * above EN 1996-1-1 §3.6.2's characteristic-basis cap of 0.065 * 20 = 1.3
	 * outright, so that coefficient cannot truncate a mean-basis envelope. 2.0
	 * sits above mean cohesion plus the working friction range and below the
	 * unit's own 3.0 MPa shear strength; no published mean-basis equivalent of
	 * the EC6 coefficient exists, so the 0.1 is judgement and says so. It remains
	 * a property of the brick — past the cap the UNIT gives rather than the joint
	 * sliding — which is why it tracks ClayBrick's compressive strength and not
	 * anything in this row. Reached at (2.0 - 0.9) / 0.75 = 1.47 MPa of
	 * compression, far above the 0.005-0.3 MPa these walls develop, so the
	 * Mohr-Coulomb coupling stays live across the whole working range.
	 */
	const FConnectionStrength GeneralPurposeMortar{
		/*Compressive*/ 10.0,
		/*ShearCohesion*/ 0.9,
		/*Tensile*/ 0.7,
		/*FrictionCoefficient*/ 0.75,
		/*MaxShear*/ 2.0
	};

	/**
	 * LIME MORTAR — the soft historic binder, weaker than cement mortar on every
	 * axis that has a bond in it. MEAN-basis, like the row above.
	 *
	 * THE ROW MODELS A GENUINE NHL 2 LIME BINDER, and now says only that. It used
	 * to cite BS 5628 designation (iv) (1:2:9 cement:lime:sand) as well, but those
	 * are different materials — the Newcastle campaign's 1:2:9 mix measures mean
	 * flexural bonds centring ~0.5 MPa, two and a half times the figure below —
	 * and one row cannot claim both. "The soft historic binder" is the identity
	 * this profile has always claimed, so the NHL 2 data is the honest anchor.
	 *
	 * Compressive 2.0 MPa — BS EN 459-1 classifies natural hydraulic limes by the
	 * 28-day compressive strength of the standard mortar, and NHL 2 means 2-7
	 * N/mm2; 2.0 is that class's floor. UNCHANGED at the re-anchor: a class-floor
	 * choice rather than a characteristic to invert, and the unreachable-ceiling
	 * property below depends on it. Non-hydraulic lime is weaker again, at well
	 * under 1 MPa.
	 *
	 * Tensile 0.2 MPa — a measured mean matched to the binder class this row
	 * claims: NHL bond-wrench campaigns report mean masonry flexural strength of
	 * about 0.19 MPa for NHL 2 at 6 months (0.40 for NHL 3.5; 0.09-0.61 across
	 * brick suction and mortar flow). No longer extrapolated from the cement row.
	 *
	 * Cohesion 0.27 MPa — a documented ratio on that measured mean: 1.34 x 0.20,
	 * where 1.34 is the Newcastle campaign's mean shear-bond-to-flexural-bond
	 * ratio (Gooch et al. 2023). No lime-specific triplet cohesion mean was found,
	 * so this figure is one evidence grade below the tension it is derived from,
	 * and says so.
	 *
	 * Friction 0.75 — unchanged from cement mortar on purpose. Bed joint friction
	 * is a property of the roughness of the two faces sliding, not of what binds
	 * them, and the measured data does not separate the binders.
	 *
	 * Shear ceiling 2.0 MPa — tracks the cement row's mean-basis truncation,
	 * because it is a property of the 20 MPa unit rather than of the mortar.
	 *
	 * IT IS PROVABLY UNREACHABLE, and that is the honest outcome rather than an
	 * oversight: 2.0 MPa of capacity needs (2.0 - 0.27) / 0.75 = 2.31 MPa of
	 * compression, past this mortar's own 2.0 MPa crushing limit, so the
	 * compression axis reaches 1.0 before the shear axis reaches its ceiling.
	 * Lime mortar crushes rather than shearing off its cap. The property SURVIVES
	 * the re-anchor only because the cap moved with cohesion — against the old
	 * 1.3 cap, mean cohesion would have pulled the bite point down to 1.37 MPa
	 * and this paragraph would have silently become false.
	 *
	 * So do NOT read Profiles.ConnectionInvariants' Bonded rule — "a friction-coupled
	 * joint needs a real shear ceiling" — as meaningful behavioural coverage here. It
	 * checks the field is set to something finite, which it is; no load can reach it.
	 * If lime's ceiling ever needs to matter, it is the compressive strength that has
	 * to move, not the cap.
	 */
	const FConnectionStrength LimeMortar{
		/*Compressive*/ 2.0,
		/*ShearCohesion*/ 0.27,
		/*Tensile*/ 0.2,
		/*FrictionCoefficient*/ 0.75,
		/*MaxShear*/ 2.0
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
	 * All three are calibrated in C24 softwood at its MEAN density — EN 338
	 * publishes both rows, rho_k = 350 kg/m3 and rho_mean = 420 kg/m3, so the
	 * mean-basis route (DESIGN.md §3, re-anchored 2026-08-13) is the same
	 * EN 1995-1-1 formulae fed with the published mean instead of the fractile:
	 * a documented 1.20 density ratio, with the formulae's own exponents doing
	 * the rest, and no new empiricism. EN 1995-1-1 is used because it is the only
	 * source that publishes fastener capacities at all. That does NOT make a wood
	 * material profile — pairing a connection with the material it bites into is the
	 * deferred weakest-link rule, not this. Honesty note: the fastener STEEL side
	 * (the wire's f_u behind M_y, the bolt's 8.8 shank) has a mean/characteristic
	 * ratio nobody publishes, so the lateral figures are density-only uplifts and
	 * therefore slight under-estimates of the true means.
	 *
	 * FRICTION IS EXACTLY ZERO on all three, and that is the load-bearing fact. A
	 * discrete fastener does not care how hard the two faces are pressed together,
	 * and mu = 0 reduces Mohr-Coulomb to three independent axes EXACTLY — same
	 * formula, no branch, which is what keeps connection types data rather than a
	 * second code path (DESIGN.md §3).
	 *
	 * COMPRESSION IS THE SAME 3.4 MPa FOR ALL THREE, and that is not a copy. When
	 * the joint is squeezed the two faces bear on each other and the fastener
	 * carries none of it, so the limit is the timber crushing perpendicular to the
	 * grain — whatever is holding the pieces together. The MEAN f_c,90 is a read
	 * number rather than a scaled one: the JCSS Probabilistic Model Code (Part 3,
	 * timber) gives E[R_c,90] = 0.008 * E[density] = 0.008 * 420 = 3.36 MPa,
	 * consistent with EN 338's characteristic 2.5 to within the class-floor
	 * rounding. The fastener governs shear and withdrawal only.
	 *
	 * The ORDERING nail < screw < bolt on both fastener-governed axes is the
	 * gameplay hook DESIGN.md §2 promises: "the same wood frame built with nails vs.
	 * screws vs. bolts genuinely behaves differently under load."
	 *
	 * The fastener-governed figures are quoted to TWO SIGNIFICANT FIGURES — the
	 * bolt's axial keeps a third only because both of its factors (the 1.2
	 * characteristic and the 1.34 JCSS ratio) are read numbers — which is already
	 * generous for values derived through a chain of empirical code formulae.
	 * Don't tighten one of them without tightening the rest; mixed precision reads
	 * as though some numbers were measured more carefully.
	 */

	/**
	 * NAIL — 3.35 mm round wire nail, 60 mm penetration, C24.
	 *
	 * Withdrawal, EN 1995-1-1 §8.3.2 at the mean density: f_ax = 20e-6 * rho^2 is
	 * quadratic in density, so the characteristic figure (f_ax,k = 2.45 N/mm2,
	 * F_ax,Rk = 2.45 * 3.35 * 60 = 492 N, 0.049 MPa over 100 cm2) scales by
	 * (420/350)^2 = 1.44 to 0.071 MPa.
	 *
	 * Lateral, EN 1995-1-1 §8.2.2 (Johansen): f_h = 0.082 * rho * d^-0.3 is linear
	 * in density and the double-plastic-hinge capacity goes as sqrt(f_h * M_y), so
	 * the characteristic figure (f_h,k = 19.5 N/mm2, M_y,Rk = 0.3 * f_u * d^2.6 =
	 * 3960 Nmm, F_v,Rk ~ 827 N, 0.083 MPa over 100 cm2) scales by sqrt(1.2) = 1.10
	 * to 0.091 MPa.
	 *
	 * Note a nail holds LATERALLY better than it does in withdrawal, which is why
	 * nailed joints are detailed to load the fasteners in shear.
	 */
	const FConnectionStrength Nail{
		/*Compressive*/ 3.4,
		/*ShearCohesion*/ 0.091,
		/*Tensile*/ 0.071,
		/*FrictionCoefficient*/ 0.0
		/*
		 * MaxShear is left at its unbounded default here and on the two below: with
		 * mu = 0 the capacity never grows with load, so there is nothing to truncate.
		 */
	};

	/**
	 * SCREW — 6 mm diameter, 50 mm effective threaded penetration, C24.
	 *
	 * Withdrawal, EN 1995-1-1 §8.7.2 at the mean density: f_ax = 0.52 * d^-0.5 *
	 * l_ef^-0.1 * rho^0.8, so the characteristic figure (f_ax,k = 15.6 N/mm2,
	 * F_ax,Rk = f_ax,k * d * l_ef = 4670 N, 0.47 MPa over 100 cm2) scales by
	 * (420/350)^0.8 = 1.16 to 0.54 MPa — nearly EIGHT TIMES the nail, which is
	 * the whole reason decking and anything loaded axially gets screwed rather
	 * than nailed.
	 *
	 * Lateral, §8.7.1 treating the screw as a dowel of its effective diameter:
	 * f_h,k = 16.6 N/mm2, M_y,Rk = 0.3 * 600 * 6^2.6 = 17,320 Nmm, F_v,Rk ~ 2130 N
	 * — 0.21 MPa over 100 cm2, scaling by the same sqrt(1.2) = 1.10 as the nail's
	 * Johansen mode to 0.23 MPa.
	 */
	const FConnectionStrength Screw{
		/*Compressive*/ 3.4,
		/*ShearCohesion*/ 0.23,
		/*Tensile*/ 0.54,
		/*FrictionCoefficient*/ 0.0
	};

	/**
	 * BOLT — M12 grade 8.8 through-bolt, 50 mm C24 members, washered.
	 *
	 * Lateral, EN 1995-1-1 §8.5.1 at the mean density: f_h = 0.082 * (1 - 0.01d)
	 * * rho is linear in density, so the characteristic figure (f_h,k = 25.3
	 * N/mm2, M_y,Rk = 0.3 * f_u,k * d^2.6 = 153,500 Nmm, F_v,Rk ~ 10 kN per shear
	 * plane, 1.0 MPa over 100 cm2) scales by 420/350 = 1.10 to 1.1 MPa.
	 *
	 * Axial: the steel is nowhere near the limit (an M12 8.8 shank yields at ~67
	 * kN); what governs is the WASHER bearing on the timber at f_c,90. A 40 mm
	 * washer with the code's bearing enhancement gives roughly 12 kN — 1.2 MPa
	 * over 100 cm2 — scaling with the mean f_c,90 (3.36/2.5 = 1.34) to 1.61 MPa.
	 * This is why a through-bolt outholds both of the above — it spreads its load
	 * over a plate instead of gripping fibres.
	 */
	const FConnectionStrength Bolt{
		/*Compressive*/ 3.4,
		/*ShearCohesion*/ 1.1,
		/*Tensile*/ 1.61,
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

	/**
	 * TEST FIXTURE ONLY — ZERO COHESION AND A REAL TENSILE BOND, WHICH NO MATERIAL HAS.
	 * NOT A MATERIAL, and it carries no citation because there is nothing to cite.
	 *
	 * IT EXISTS BECAUSE `DryStone` CANNOT ANSWER THE QUESTION. Composite vertical action is
	 * meant to need shear transfer between courses, so a joint with no bond should get
	 * little or none of it — and `COMPOSITE_DEPTH_DESIGN.md` slice 5 works that gate out of
	 * cohesion and friction with no per-material branch. But `DryStone.TensileStrengthMPa`
	 * is an EXACT zero, so a dry-laid corbel is condemned at ANY section modulus and no
	 * fixture laid in it can tell a bounded composite depth from an unbounded one, or from
	 * the gate's total absence. This row is `DryStone` with the one field changed that
	 * blinds it: zero cohesion, so the frictional behaviour is real, and a tensile bond, so
	 * the reading is a number rather than an immediate condemnation.
	 *
	 * TENSILE 0.40 MPa, DELIBERATELY NOT 0.70. Matching general purpose mortar would make
	 * the two rows read identically and a test asserting "the ordering follows the profile
	 * numbers" would pass on a model that ignored the profile entirely. Sitting strictly
	 * between lime's 0.20 and cement's 0.70 puts it in the MIDDLE of the ordering, which is
	 * the position an ordering claim is hardest to satisfy by accident. (Fixture
	 * maintenance at the 2026-08-14 mean re-anchor flip, not a derivation: the old 0.08 sat
	 * between the characteristic 0.05 and 0.10 for exactly the same reason.)
	 *
	 * CLASSIFIED TestFixture RATHER THAN Frictional, and the class is what carries that.
	 * `Profiles.ConnectionInvariants` requires a Frictional row to have EXACTLY zero tensile
	 * strength — which is the correct physics for real dry stone and is precisely the
	 * property this row has to break — so claiming that class would be claiming to be a
	 * material it is not. Being a fixture is machine-recognisable, which is the whole reason
	 * the class exists.
	 *
	 * THE OTHER FIGURES ARE DryStone'S, unchanged, so the two rows differ in one number and
	 * a comparison between them measures that number rather than four of them at once.
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
			{ TEXT("CohesionlessBond"),     EConnectionProfileClass::TestFixture,        CohesionlessBond },
		};
	}

	TArrayView<const FNamedConnectionProfile> AllConnectionProfiles()
	{
		return TArrayView<const FNamedConnectionProfile>(
			ConnectionProfileLibrary, UE_ARRAY_COUNT(ConnectionProfileLibrary));
	}
}
