// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE BEAM ACCEPTANCE SET — a simply supported beam with a heavy weight on it, where the MEMBER
 * is what should fail and no joint should.
 *
 * The user asked for this on 2026-08-08, and it is deliberately outside what the solver can do
 * today. PROJECT_REVIEW.md §2 ranks two gaps that meet exactly here: item 3, a piece on two or
 * more supports has its bending moment zeroed by the area split, so "a lintel or floor slab on
 * two walls can never fail in midspan bending"; and item 7, pieces never fail — only joints do —
 * "masked today because mortar is weaker than brick by data; fatal for wood, where the MEMBER
 * bending failure is the primary mode". This file is the acceptance anchor for both, i.e. for
 * evolution steps 5 and 6.
 *
 * HOW A MEMBER FAILURE IS EXPRESSIBLE AT ALL, GIVEN THAT FStructure PIECES CARRY ONLY MASS. A
 * piece's own material strength never enters the solve, so there is no direct code path for "the
 * beam snapped". What there IS is a joint carrying a strength profile — so the beam is modelled
 * as TWO COLLINEAR SEGMENTS meeting at midspan, and the joint between them carries the MEMBER
 * MATERIAL's own published strengths rather than any mortar's. A beam snapping in midspan bending
 * is then exactly that glue line parting under the bending tension the solver computes for it.
 * Everything else in the fixture — the two bearings and the two contacts under the weight — is
 * resting contact with no bond at all, which is what a beam sitting on a pier actually is.
 *
 * WHY TWO SEGMENTS AND NOT THREE OR FIVE, WHICH IS A FIXTURE DECISION THE SOLVER FORCED. An EVEN
 * segment count is what puts a glue line at EXACT midspan; an odd one puts a segment centre there
 * and the glue lines either side of it. More than two segments leaves the middle ones with no bed
 * joint at all, which is the SEATLESS case: FStructure::ReseatSpannedGroups forms them into a
 * spanning group, re-seats them onto the end segments, and ApplyArchingThrust then pushes the two
 * bearings apart with H = 3W/(8 d_e/L). Measured on a five-segment draft of this fixture the
 * thrust ratio H/V = 3L/(4 d_e) came out at 7.2 against dry stone's friction coefficient of 0.7,
 * so the beam sheared off both its bearings — a collapse, at the right load, for entirely the
 * wrong reason, and one that would have made the "wood falls" row PASS while saying nothing about
 * members. Two segments have a seat each, form no group, and take no phantom thrust. That is the
 * adjustment, and it is recorded so nobody re-derives the five-segment version.
 *
 * WHAT THE SOLVER ACTUALLY DOES WITH THIS FIXTURE, MEASURED. Each half-beam is seated on its own
 * pier, so SupportConnections for each is its own bearing and NEITHER of them uses the midspan
 * joint. The glue line is therefore the support of nobody, and ConnectionForces leaves it at
 * exactly zero — the beam's whole bending action is invisible. The two bearings do carry the load
 * correctly (they sum to the total weight, asserted below), and each reads a large eccentric
 * moment.
 *
 * HOW THE BEARINGS READ CHANGED ON 2026-08-09 (the one-cell thrust gate, commit 08abcfd), and
 * this header's original "nothing anywhere is close to failing, at any load" is no longer true.
 * Before the gate, FConnection::ArchingMomentScale capped each bearing's eccentric moment to the
 * kern edge and the light rows stood at ~0.31/~0.34. The gate now grants that cap only where the
 * springing can carry the implied thrust — and these DRY bearings cannot — so the uncapped
 * eccentric moment on a zero-tension joint reads Max(), and ALL THREE rows unzip at the bearings
 * in one pass (3 fallen, including the light rows whose real-world verdict is STANDS). The rigid-
 * block LP oracle stands all three (lambda* 1.76 / 19.2 / 17.4 — RigidBlockOracleSweepTest), so
 * the falling is production's missing global equilibrium, not the fixture's physics.
 *
 * THE RULING IS NOW MADE (DESIGN.md §8, 2026-08-11): beams-unzip-on-dry-bearings is ACCEPTED AS
 * A KNOWN COST until evolution step 4 promotes equilibrium to the cascade authority. What the
 * oracle sweep pinned externally is now pinned IN THIS FILE — every `FBeamCase::DropsToday` /
 * `PassesToday` (measured 3 fallen, 1 breaking pass, all three rows) — so a further regression
 * inside this already-known failure cannot hide the way this one did for two days.
 *
 * SO THE RED IS: THE MEMBER CARRIES NOTHING. Not a stranded fixture, not a refused route, not a
 * joint failing early — a beam under three and a half times its published bending capacity whose
 * midspan section reads a bending moment of exactly 0.0.
 *
 * DISPLACEMENT IS NOT USED AS A BREAK ASSERTION, per DESIGN.md §4. What is read is whether a
 * piece still has a path to the earth after the cascade, plus — for the row that must fail — the
 * MECHANISM, that the midspan glue line itself gave. Both halves are needed: a beam that came
 * down by sliding off its bearings is a different and wrong answer, and an outcome-only assertion
 * would call it a pass.
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on (weight is mass x 980 and there is no way to switch it
 * off), everything is connected and the assertions are on outcome and on solver state. The one
 * thing a world would add is the wire from the solver's answer to Chaos, which
 * Tests/StructureIntegrationTest.cpp already covers and which is identical for all three rows.
 *
 * NAMED NAMESPACE, not anonymous: an anonymous namespace is private to a TRANSLATION UNIT rather
 * than to a file, and a unity build merges many files into one.
 *
 * NOTHING IS IMPORTED FROM THE CODE UNDER TEST EXCEPT THE PRODUCER. The section modulus, the
 * statics, the newton-to-Unreal conversion and every published strength are re-derived below, so
 * a wrong constant in production makes this file DISAGREE with it rather than agree with it. The
 * one deliberate exception is Layout::MakeInterface, which decides whether two boxes share a face
 * — re-implementing that would be re-implementing part of the thing under test — and even that is
 * checked, joint by joint, against interface areas this file computes for itself.
 */
namespace BeamAcceptanceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * THE GEOMETRY. Every length is centimetres, at Unreal's default 1 uu = 1 cm.
	 * ================================================================================ */

	/**
	 * A 100 x 100 mm sawn section, which is a real timber size and a real steel bar size.
	 *
	 * WIDTH IS ACROSS THE BEAM (Y) AND DEPTH IS VERTICAL (Z), which is the pairing the section
	 * modulus below depends on: bending about the Y axis is resisted across the DEPTH, so the
	 * depth is the term that gets squared. Swapping the two would be a plausible number against
	 * the wrong section.
	 */
	constexpr double SectionWidthCm = 10.0;
	constexpr double SectionDepthCm = 10.0;

	/** Two segments meeting at x = 0, so the glue line sits at EXACT midspan. */
	constexpr double SegmentLengthCm = 220.0;
	constexpr double BeamLengthCm = SegmentLengthCm * 2.0;

	/** Bearing centre to bearing centre. The beam oversails each bearing centre by 20 cm. */
	constexpr double SpanCm = 400.0;

	/** How much of the beam actually lands on a pier, along the beam. */
	constexpr double BearingLengthCm = 40.0;

	/*
	 * The piers run PAST the beam's end on the outside — a shape chosen to sidestep a
	 * MakeInterface defect that has since been FIXED (2026-08-08): it used to compute an axis
	 * overlap as `extentA + extentB - distance`, which over-reported whenever one box's span
	 * wholly CONTAINED the other's; it now takes the true interval intersection, and
	 * Layout.Interface's contained-pier rows pin exactly the case this fixture used to avoid.
	 * The oversailing piers are kept because the readings below are anchored to this geometry —
	 * moving them inboard would be the suite's only contained bearing exercised through a real
	 * solve, which is worth doing as its own slice with re-derived numbers, not as a drive-by.
	 */
	constexpr double PierLengthCm = 60.0;
	constexpr double PierHeightCm = 40.0;

	/**
	 * The weight: a steel plate laid along the beam, its thickness matching the beam's width.
	 *
	 * MATCHING THE BEAM'S WIDTH IS THE SAME CONTAINMENT RULE ONE AXIS OVER. A block wider than
	 * the beam would contain the beam on Y, and its two contact patches would be emitted 50 cm
	 * wide instead of 10.
	 */
	constexpr double BlockLengthCm = 200.0;
	constexpr double BlockWidthCm = SectionWidthCm;

	/* ================================================================================
	 * UNITS AND PUBLISHED MATERIAL DATA. Every figure is cited; none is imported.
	 * ================================================================================ */

	/**
	 * 980 cm/s2. With 1 uu = 1 cm and mass in kilograms, MassKg * 980 IS a weight in Unreal force
	 * units — DESIGN.md §3's 1 N = 100 uu is already inside that number, and applying it a second
	 * time is the 100x error the units section exists to prevent.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * Unreal force units that load one square centimetre to one megapascal.
	 *
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa (= 1 N/mm2) over 1 cm2 is 10000 uu. DELIBERATELY
	 * NOT DestructionForce::ForceUnitsPerMPaSqCm: this file has to fail if that constant is wrong
	 * rather than agree with it.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/* --- C24 softwood, EN 338 (strength class table) ------------------------------------- */

	/**
	 * f_m,k = 24 N/mm2 — the CHARACTERISTIC BENDING STRENGTH, and the capacity this whole file
	 * turns on.
	 *
	 * IT IS APPLIED TO BOTH EXTREME FIBRES, WHICH IS A DECISION AND NOT AN OVERSIGHT. EN 338 also
	 * publishes f_t,0,k = 14 N/mm2 (tension parallel) and f_c,0,k = 21 N/mm2 (compression
	 * parallel), and it is tempting to hand those to the joint's tensile and compressive fields.
	 * They are AXIAL capacities. A section in pure bending is checked in EN 1995-1-1 §6.1.6 by the
	 * single ratio sigma_m,d / f_m,d, which covers the whole bending stress block — both edges —
	 * and this section carries no axial force at all, since a simply supported beam under gravity
	 * develops none. Using f_c,0,k = 21 against a bending compression edge would make every timber
	 * beam fail about 14% earlier than the code says, and would move the governing axis off the
	 * tension face, which is where a timber beam actually splinters. Both figures are quoted here
	 * rather than declared, because neither is used and a constant nobody reads is a constant that
	 * can drift.
	 */
	/*
	 * MEAN BASIS since the 2026-08-13 re-anchor: 24.0 was EN 338's characteristic f_m,k, and
	 * JCSS PMC Part 3.5 Table 2 gives bending for European softwood as Lognormal with COV 0.25,
	 * so mean / 5%-fractile = exp(1.645 x 0.2462) = 1.4993 and the mean is 24 x 1.50 = 36.0.
	 */
	constexpr double C24BendingMPa = 36.0;

	/*
	 * Mean shear: JCSS states COV[R_v] = COV[R_m], so the same x1.50 applies to EN 338's
	 * f_v,k = 4.0. (JCSS's own E[R_v] = 0.2 E[R_m] would give 7.2; 6.0 keeps the row's
	 * identity as C24 — the derivation doc records the 20% disagreement.)
	 */
	constexpr double C24ShearMPa = 6.0;

	/**
	 * EN 338: rho_mean = 420 kg/m3 for C24, which is 0.42 g/cm3.
	 *
	 * MEAN AND NOT CHARACTERISTIC. rho_k = 350 kg/m3 is the 5-percentile used for fastener design;
	 * what a beam actually WEIGHS is the mean, and weight is the only thing density does here.
	 * Density is g/cm3 in Unreal, which is the published unit divided by 1000 and nothing else.
	 */
	constexpr double C24DensityGramsPerCubicCm = 0.42;

	/* --- S275 structural steel, EN 10025-2 ------------------------------------------------ */

	/**
	 * f_y = 275 N/mm2, the nominal yield of grade S275 (EN 10025-2, t <= 16 mm).
	 *
	 * EN 1993-1-1 Table 3.1 steps the design value down for thick product — 255 N/mm2 for
	 * 40 < t <= 80 mm — and this fixture's section is 100 mm. Taking 255 instead moves the steel
	 * row from 0.335 to 0.362 of capacity and changes no verdict, so the nominal grade figure is
	 * used and the reduction is recorded rather than applied.
	 *
	 * BOTH FIBRES AGAIN, AND HERE IT NEEDS NO ARGUMENT: steel yields at f_y in tension and in
	 * compression alike.
	 */
	/*
	 * MEAN (static) yield since the 2026-08-13 re-anchor: JCSS PMC Part 3 Table A,
	 * E[f_y] = f_y,sp * alpha * exp(-u v) - C with v = 0.07, u in [-1.5, -2.0], alpha = 1.0
	 * for flanges and C = 20 MPa (the mill-test to static-yield correction): 285-296 MPa for
	 * S275. 290 is the centre. (The old 275.0 was EN 10025-2's nominal.)
	 */
	constexpr double S275YieldMPa = 290.0;

	/**
	 * f_y / sqrt(3) = 158.771 N/mm2 — the von Mises shear yield, EN 1993-1-1 §6.2.6.
	 *
	 * Written as the division rather than as 158.771 so it moves with f_y and cannot drift from it.
	 */
	const double S275ShearMPa = S275YieldMPa / FMath::Sqrt(3.0);

	/** 7850 kg/m3, the conventional density of structural steel, in Unreal's g/cm3. */
	constexpr double SteelDensityGramsPerCubicCm = 7.85;

	/** 2400 kg/m3, normal-weight concrete (EN 1991-1-1 Table A.1). The piers only, and they are grounded. */
	constexpr double ConcreteDensityGramsPerCubicCm = 2.4;

	/* ================================================================================
	 * THE INDEPENDENT ORACLE: elastic beam statics, worked here from first principles.
	 * ================================================================================
	 *
	 * NONE OF THIS MIRRORS PRODUCTION, WHICH IS THE ONLY REASON IT IS WORTH HAVING. The solver
	 * accumulates weight down a support graph; this is the free-body diagram of a simply supported
	 * beam, which is where the expected verdicts come from. The two are derived differently and
	 * are meant to be compared.
	 */

	/** W = b d^2 / 6 for a rectangle. b is the width across the beam; d is the depth being bent. */
	constexpr double SectionModulusCm3 =
		SectionWidthCm * SectionDepthCm * SectionDepthCm / 6.0;

	/** The face the two segments meet across: the beam's own cross-section. */
	constexpr double SectionAreaSqCm = SectionWidthCm * SectionDepthCm;

	/**
	 * Where the weight of the block actually lands on each half of the beam.
	 *
	 * The block spans -BlockLengthCm/2 .. +BlockLengthCm/2 and the beam parts at x = 0, so each
	 * half takes half the block over a contact whose centroid is a quarter of the block's length
	 * from midspan.
	 */
	constexpr double BlockContactOffsetCm = BlockLengthCm / 4.0;

	/** Mass of a box of this material, in kilograms. cm3 x g/cm3 is grams; grams / 1000 is kg. */
	double BoxMassKg(double XCm, double YCm, double ZCm, double DensityGramsPerCubicCm)
	{
		return DensityGramsPerCubicCm * XCm * YCm * ZCm / 1000.0;
	}

	/** What one row's beam weighs, both segments together. */
	double BeamMassKg(double DensityGramsPerCubicCm)
	{
		return BoxMassKg(BeamLengthCm, SectionWidthCm, SectionDepthCm, DensityGramsPerCubicCm);
	}

	/** What one row's block weighs. */
	double BlockMassKg(double BlockHeightCm)
	{
		return BoxMassKg(BlockLengthCm, BlockWidthCm, BlockHeightCm, SteelDensityGramsPerCubicCm);
	}

	/**
	 * The reaction at each bearing, uu.
	 *
	 * Symmetric load on a symmetric span, so each pier takes half of everything. The piers'
	 * own weight is not in it: they are grounded, and the earth takes what reaches them.
	 */
	double BearingReactionUu(double BlockHeightCm, double BeamDensityGramsPerCubicCm)
	{
		return (BlockMassKg(BlockHeightCm) + BeamMassKg(BeamDensityGramsPerCubicCm))
			* GravityCmPerSecondSquared / 2.0;
	}

	/**
	 * The bending moment at midspan, uu.cm, by taking moments about x = 0 over the left half.
	 *
	 *     M = R * (L/2)                    the reaction, a half span away
	 *       - (P/2) * (a/4)                half the block, at its contact centroid
	 *       - (W/2) * (l/2)                the left half of the beam, at its own centre
	 *
	 * ORDINARY STATICS AND NOTHING ELSE. This is what a beam's midspan section carries, and it is
	 * the number the model has no way to produce.
	 */
	double MidspanMomentUuCm(double BlockHeightCm, double BeamDensityGramsPerCubicCm)
	{
		const double BlockWeightUu = BlockMassKg(BlockHeightCm) * GravityCmPerSecondSquared;
		const double BeamWeightUu =
			BeamMassKg(BeamDensityGramsPerCubicCm) * GravityCmPerSecondSquared;

		return BearingReactionUu(BlockHeightCm, BeamDensityGramsPerCubicCm) * (SpanCm / 2.0)
			- (BlockWeightUu / 2.0) * BlockContactOffsetCm
			- (BeamWeightUu / 2.0) * (SegmentLengthCm / 2.0);
	}

	/** Extreme-fibre bending stress at midspan as a fraction of the member's bending strength. */
	double MidspanBendingUtilisation(
		double BlockHeightCm, double BeamDensityGramsPerCubicCm, double BendingStrengthMPa)
	{
		return MidspanMomentUuCm(BlockHeightCm, BeamDensityGramsPerCubicCm) / SectionModulusCm3
			/ (BendingStrengthMPa * ForceUnitsPerMPaSqCmHere);
	}

	/**
	 * Peak shear stress just inside a bearing as a fraction of the member's shear strength.
	 *
	 * THIS EXISTS SO THAT "BENDING GOVERNS" IS MEASURED RATHER THAN ASSERTED. The whole point of
	 * the fixture is a MEMBER BENDING failure, and a fixture whose shear happened to be worse
	 * would be testing something else entirely while still going red. 1.5 * V / A is the peak of
	 * the parabolic shear distribution over a rectangle.
	 */
	double BearingShearUtilisation(
		double BlockHeightCm, double BeamDensityGramsPerCubicCm, double ShearStrengthMPa)
	{
		return 1.5 * BearingReactionUu(BlockHeightCm, BeamDensityGramsPerCubicCm) / SectionAreaSqCm
			/ (ShearStrengthMPa * ForceUnitsPerMPaSqCmHere);
	}

	/* ================================================================================
	 * THE TABLE.
	 * ================================================================================ */

	enum class EVerdict : uint8
	{
		/** The member breaks at midspan and the beam and its load come down. The piers do not. */
		PartsAtMidspan,

		/** Nothing anywhere reaches capacity. */
		Stands,
	};

	const TCHAR* VerdictName(EVerdict Verdict)
	{
		return Verdict == EVerdict::PartsAtMidspan ? TEXT("PARTS AT MIDSPAN") : TEXT("STANDS");
	}

	struct FBeamCase
	{
		int32 Number = 0;
		const TCHAR* Title = nullptr;

		/** What this row's matched pair varies. Printed on failure. */
		const TCHAR* Isolates = nullptr;

		const TCHAR* MemberName = nullptr;
		double MemberDensityGramsPerCubicCm = 0.0;

		/** The member's own published bending strength, applied at both extreme fibres. */
		double MemberBendingMPa = 0.0;

		/** The member's own published shear strength. */
		double MemberShearMPa = 0.0;

		/** The one thing that varies between rows 1 and 2. */
		double BlockHeightCm = 0.0;

		EVerdict Verdict = EVerdict::Stands;

		/**
		 * HOW MANY LIVE PIECES THE MODEL DROPS HERE TODAY — a CHARACTERISATION OF A WRONG ANSWER,
		 * set on ALL THREE ROWS and INDEX_NONE nowhere in this file.
		 *
		 * IT IS NOT AN EXPECTATION AND IT ENDORSES NOTHING, exactly per FWallCase::DropsToday in
		 * WallAcceptanceTest.cpp, which this mirrors. `Verdict` above is what a real beam does;
		 * this is what the solver does instead, measured off a run and written down. Set on every
		 * row rather than only the PartsAtMidspan one, because the wrong answer is not particular
		 * to case 1: since the one-cell thrust gate (08abcfd, 2026-08-09) refused the kern cap to
		 * this fixture's DRY bearings, the uncapped eccentric moment on a zero-tension joint reads
		 * Max() on every bearing, and the two STANDS rows unzip exactly as case 1 does. The
		 * rigid-block LP oracle stands all three (lambda* 1.76 / 19.2 / 17.4 —
		 * RigidBlockOracleSweepTest), so this is production's missing global equilibrium (DESIGN.md
		 * §7 gap 1 / evolution step 4), never the fixture's physics.
		 *
		 * THIS IS WHY THE PIN EXISTS AT ALL: the 2026-08-09 gate flipped every row's bearing
		 * reading and nobody saw it until the 2026-08-11 oracle sweep, because nothing in this file
		 * pinned what production does today. The ruling and its costs: DESIGN.md §8.
		 *
		 * ACCEPTED AS A KNOWN COST until evolution step 4 promotes equilibrium to the cascade
		 * authority (the beam user ruling, DESIGN.md §8, decided 2026-08-11). WHEN THAT ROW LANDS,
		 * DELETE DropsToday AND PassesToday IN THE SAME EDIT — they will fail, and that failure is
		 * the reminder, never "updated" to a new wrong number without saying why the answer moved.
		 */
		int32 DropsToday = INDEX_NONE;

		/**
		 * HOW MANY BREAKING PASSES THE CASCADE RAN HERE TODAY — the same characterisation as
		 * DropsToday, and read the same way. Kept separate so a change to the SHAPE of the known-
		 * wrong answer (the same piece count reached over a different number of passes) is visible
		 * too, rather than hiding behind an unchanged piece count. See DropsToday for what this is
		 * not and the delete-when-fixed instruction, which covers this field as well.
		 */
		int32 PassesToday = INDEX_NONE;
	};

	/**
	 * THE MEMBER'S GLUE LINE, AS A CONNECTION PROFILE.
	 *
	 * FRICTION IS EXACTLY ZERO, and that is load-bearing rather than tidy. Mohr-Coulomb's
	 * `cohesion + mu * compressive stress` describes an INTERFACE whose resistance to sliding
	 * grows as you squeeze it. A cross-section through a solid member is not an interface; its
	 * shear strength is a material property and does not care what the axial stress is. mu = 0
	 * reduces the envelope to three independent axes EXACTLY (DESIGN.md §3), which is the same
	 * reason the fastener profiles carry it — and it also keeps the shear axis from being quietly
	 * inflated by the compression that shares the section, which would flatter the shear check
	 * this file uses to prove bending governs.
	 *
	 * NO SHEAR CEILING, for the same reason: with mu = 0 there is nothing for a truncation to
	 * truncate.
	 */
	FConnectionStrength MemberStrength(double BendingMPa, double ShearMPa)
	{
		FConnectionStrength Strength;
		Strength.CompressiveStrengthMPa = BendingMPa;
		Strength.ShearCohesionMPa = ShearMPa;
		Strength.TensileStrengthMPa = BendingMPa;
		Strength.FrictionCoefficient = 0.0;

		return Strength;
	}

	TArray<FBeamCase> AllBeamCases()
	{
		/*
		 * THE DropsToday / PassesToday PINS ARE GONE SINCE SLICE 3b/4 (2026-08-27). All three rows
		 * used to drop 3 in 1 pass because the one-cell thrust gate let their DRY bearings read
		 * Max() outside the kern. Below the 200-block cap the equilibrium LP is now the sole break
		 * authority, it stands all three bearings (the oracle sweep reads lambda* 2.65 / 28.80 /
		 * 18.30, all >= 1), and production drops 0 in 0 passes — so the known-red characterisation
		 * has nothing left to characterise and is deleted per FBeamCase::DropsToday's own rule.
		 * Rows 2 and 3 (Stands) are now GREEN. Row 1 stays RED, but on its MEMBER-FAILURE
		 * assertions alone (|M| = 0 at midspan, the beam does not part) — evolution step 6, not this
		 * slice; the bearings being fixed does not give the solver a way to fail the member.
		 */
		TArray<FBeamCase> Cases;

		/*
		 * ROW 1 — THE MEMBER-FAILURE CASE, and the reason the file exists.
		 *
		 * A 1884 kg steel plate on a 100 x 100 mm C24 joist over a 4 m span. Midspan moment is
		 * 139,288,968 uu.cm = 13,928.9 N.m against a section modulus of 166.67 cm3, so the
		 * extreme fibre sits at 83.57 MPa against the mean 36 MPa: 2.32 times capacity (3.48x
		 * the retired characteristic 24 — the 2026-08-14 re-anchor flip moved the margin, not the
		 * verdict). The beam breaks. Nothing else in the fixture is within two orders of
		 * magnitude of its own limit.
		 */
		Cases.Add({
			1, TEXT("C24 timber beam, heavy load"), TEXT("member material (vs case 3)"),
			TEXT("C24 timber"), C24DensityGramsPerCubicCm, C24BendingMPa, C24ShearMPa,
			/*BlockHeightCm*/ 120.0, EVerdict::PartsAtMidspan,
			/*DropsToday*/ INDEX_NONE, /*PassesToday*/ INDEX_NONE });

		/*
		 * ROW 2 — THE SAME BEAM WELL INSIDE CAPACITY, differing from row 1 in the block's height
		 * and in nothing else. 157 kg, midspan moment 12,354,468 uu.cm, extreme fibre 7.41 MPa,
		 * 0.206 of the mean 36 (0.309 of the retired characteristic 24). A joist carrying a
		 * sensible load, and it must simply stand.
		 */
		Cases.Add({
			2, TEXT("C24 timber beam, light load"), TEXT("load magnitude (vs case 1)"),
			TEXT("C24 timber"), C24DensityGramsPerCubicCm, C24BendingMPa, C24ShearMPa,
			/*BlockHeightCm*/ 10.0, EVerdict::Stands,
			/*DropsToday*/ INDEX_NONE, /*PassesToday*/ INDEX_NONE });

		/*
		 * ROW 3 — THE STEEL TWIN. Identical geometry, identical block, only the member material
		 * changes. The heavier beam raises the midspan moment to 153,706,140 uu.cm and the extreme
		 * fibre to 92.22 MPa, but S275's mean static yield is 290, so it reads 0.318 and holds
		 * (0.335 against the old nominal 275). Wood fails where steel holds under the same load:
		 * that is the whole of the data-drivenness claim, and today the model answers both
		 * identically. The material discrimination is 290/36 = 8.1x (was 9.8x on the
		 * characteristic/nominal pair) — the sweep's pinned figure moves with it.
		 */
		Cases.Add({
			3, TEXT("S275 steel beam, heavy load"), TEXT("member material (vs case 1)"),
			TEXT("S275 steel"), SteelDensityGramsPerCubicCm, S275YieldMPa, S275ShearMPa,
			/*BlockHeightCm*/ 120.0, EVerdict::Stands,
			/*DropsToday*/ INDEX_NONE, /*PassesToday*/ INDEX_NONE });

		return Cases;
	}

	/* ================================================================================
	 * THE FIXTURE.
	 * ================================================================================ */

	/** One beam, with every handle the assertions need named rather than guessed at. */
	struct FBeam
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
		TArray<FString> Names;

		int32 LeftPier = INDEX_NONE;
		int32 RightPier = INDEX_NONE;
		int32 LeftSegment = INDEX_NONE;
		int32 RightSegment = INDEX_NONE;
		int32 Block = INDEX_NONE;

		int32 MidspanJoint = INDEX_NONE;
		int32 LeftBearingJoint = INDEX_NONE;
		int32 RightBearingJoint = INDEX_NONE;
	};

	/** The joint naming these two pieces, either way round, or INDEX_NONE. */
	int32 JointBetween(const FBeam& Beam, int32 First, int32 Second)
	{
		for (int32 Index = 0; Index < Beam.Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Beam.Structure.GetConnection(Index);

			if ((Joint.PieceA == First && Joint.PieceB == Second)
				|| (Joint.PieceA == Second && Joint.PieceB == First))
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	/*
	 * The true area two boxes share, computed the way an interval intersection actually works.
	 *
	 * WHAT THIS GUARDS CHANGED ON 2026-08-08. When it was written, production computed
	 * `extentA + extentB - distance`, which is wrong under containment, so this check was a
	 * tripwire: a dimension edited into a containment read as a FIXTURE failure instead of a
	 * silently oversized bearing. MakeInterface now uses this same interval intersection, so the
	 * two agree on every topology and the tripwire is gone — what remains is a plain consistency
	 * check that the fixture's own bookkeeping matches what the producer emitted. An independent
	 * re-derivation of production's arithmetic now needs a DIFFERENT expression for the same set:
	 * `min(2·extentA, 2·extentB, extentA + extentB - |centre difference|)` per axis, which is the
	 * oracle Layout.InterfaceFuzz is specified to use (see CURRENT_STATE.md, Layout producer).
	 */
	double SharedFaceAreaSqCm(const FPieceBox& A, const FPieceBox& B, int32 SeparationAxis)
	{
		double AreaSqCm = 1.0;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Axis == SeparationAxis)
			{
				continue;
			}

			const double LowCm = FMath::Max(A.CentreCm[Axis] - A.ExtentCm[Axis],
				B.CentreCm[Axis] - B.ExtentCm[Axis]);
			const double HighCm = FMath::Min(A.CentreCm[Axis] + A.ExtentCm[Axis],
				B.CentreCm[Axis] + B.ExtentCm[Axis]);

			AreaSqCm *= HighCm - LowCm;
		}

		return AreaSqCm;
	}

	/**
	 * Lay one row's beam.
	 *
	 * JOINT THICKNESS IS ZERO EVERYWHERE, because every joint here is dry contact: the beam sits
	 * on the piers, the block sits on the beam, and the two halves of the member butt against each
	 * other. Layout::MakeInterface accepts a zero-thickness face deliberately — dry stone has
	 * faces touching with no gap — and it keeps every centroid in this fixture an exact binary
	 * number, which is what lets the expected values above be quoted to the last digit.
	 */
	void LayBeam(const FBeamCase& Case, FBeam& OutBeam)
	{
		const double BeamBottomZCm = PierHeightCm;
		const double BeamCentreZCm = BeamBottomZCm + SectionDepthCm / 2.0;
		const double BeamTopZCm = BeamBottomZCm + SectionDepthCm;

		/*
		 * The pier reaches from outside the beam's end to BearingLengthCm inboard of it, so the
		 * bearing patch is centred exactly SpanCm/2 from midspan and the two boxes overlap
		 * partially rather than one containing the other. See PierLengthCm.
		 */
		const double BeamEndCm = BeamLengthCm / 2.0;
		const double PierInnerCm = BeamEndCm - BearingLengthCm;
		const double PierCentreCm = PierInnerCm + PierLengthCm / 2.0;

		const auto AddBox = [&OutBeam](
			const FString& Name, const FPieceBox& Box, double DensityGramsPerCubicCm, bool bGrounded)
		{
			/*
			 * THE BOX'S CENTRE IS THE CENTRE OF MASS, because every piece here is a homogeneous
			 * solid and its mass came off the same box. Without it there is no eccentricity
			 * anywhere and the bearings read a centred load they are not carrying — which is
			 * exactly the state HasCompleteGeometry exists to make askable, and it is asserted as
			 * a fixture precondition.
			 */
			const double MassKg = BoxMassKg(
				Box.ExtentCm.X * 2.0, Box.ExtentCm.Y * 2.0, Box.ExtentCm.Z * 2.0,
				DensityGramsPerCubicCm);

			const int32 Handle = OutBeam.Structure.AddPiece(MassKg, bGrounded, Box.CentreCm);

			OutBeam.Boxes.Add(Box);
			OutBeam.Names.Add(Name);

			return Handle;
		};

		FPieceBox PierBox;
		PierBox.ExtentCm = FVector(PierLengthCm, SectionWidthCm, PierHeightCm) * 0.5;

		PierBox.CentreCm = FVector(-PierCentreCm, 0.0, PierHeightCm / 2.0);
		OutBeam.LeftPier = AddBox(
			TEXT("left pier"), PierBox, ConcreteDensityGramsPerCubicCm, /*bGrounded*/ true);

		PierBox.CentreCm = FVector(PierCentreCm, 0.0, PierHeightCm / 2.0);
		OutBeam.RightPier = AddBox(
			TEXT("right pier"), PierBox, ConcreteDensityGramsPerCubicCm, /*bGrounded*/ true);

		FPieceBox SegmentBox;
		SegmentBox.ExtentCm = FVector(SegmentLengthCm, SectionWidthCm, SectionDepthCm) * 0.5;

		SegmentBox.CentreCm = FVector(-SegmentLengthCm / 2.0, 0.0, BeamCentreZCm);
		OutBeam.LeftSegment = AddBox(
			TEXT("left half-beam"), SegmentBox, Case.MemberDensityGramsPerCubicCm, false);

		SegmentBox.CentreCm = FVector(SegmentLengthCm / 2.0, 0.0, BeamCentreZCm);
		OutBeam.RightSegment = AddBox(
			TEXT("right half-beam"), SegmentBox, Case.MemberDensityGramsPerCubicCm, false);

		FPieceBox BlockBox;
		BlockBox.ExtentCm = FVector(BlockLengthCm, BlockWidthCm, Case.BlockHeightCm) * 0.5;
		BlockBox.CentreCm = FVector(0.0, 0.0, BeamTopZCm + Case.BlockHeightCm / 2.0);
		OutBeam.Block = AddBox(TEXT("block"), BlockBox, SteelDensityGramsPerCubicCm, false);

		/*
		 * EVERY PAIR IS OFFERED AND MakeInterface REFUSES THE ONES THAT ARE NOT FACES — the piers
		 * to each other, the block to the piers, each half-beam to the far pier. Deciding which
		 * pairs touch is the producer's job and this fixture does not second-guess it; what it
		 * does check, below, is the AREA of everything the producer accepted.
		 *
		 * ONLY THE SEGMENT-TO-SEGMENT JOINT CARRIES THE MEMBER'S OWN STRENGTHS. Everything else is
		 * dry contact.
		 */
		const FConnectionStrength Member = MemberStrength(Case.MemberBendingMPa, Case.MemberShearMPa);

		for (int32 First = 0; First < OutBeam.Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < OutBeam.Boxes.Num(); ++Second)
			{
				const bool bIsTheMember =
					(First == OutBeam.LeftSegment && Second == OutBeam.RightSegment)
					|| (First == OutBeam.RightSegment && Second == OutBeam.LeftSegment);

				FConnection Joint;

				if (MakeInterface(
						First, OutBeam.Boxes[First], Second, OutBeam.Boxes[Second],
						/*JointThicknessCm*/ 0.0, bIsTheMember ? Member : DryStone, Joint))
				{
					OutBeam.Structure.AddConnection(Joint);
				}
			}
		}

		OutBeam.MidspanJoint = JointBetween(OutBeam, OutBeam.LeftSegment, OutBeam.RightSegment);
		OutBeam.LeftBearingJoint = JointBetween(OutBeam, OutBeam.LeftPier, OutBeam.LeftSegment);
		OutBeam.RightBearingJoint = JointBetween(OutBeam, OutBeam.RightPier, OutBeam.RightSegment);
	}

	/** Which live pieces have lost their path to the earth. Stranded counts as fallen. */
	TArray<int32> FallenPieces(const FBeam& Beam)
	{
		TArray<int32> Fallen;

		for (int32 Piece = 0; Piece < Beam.Structure.NumPieces(); ++Piece)
		{
			if (Beam.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Beam.Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				Fallen.Add(Piece);
			}
		}

		return Fallen;
	}

	/** How many live pieces the solver could not route at all. A precondition, never a verdict. */
	int32 StrandedCount(const FBeam& Beam)
	{
		int32 Stranded = 0;

		for (int32 Piece = 0; Piece < Beam.Structure.NumPieces(); ++Piece)
		{
			if (!Beam.Structure.IsPieceRemoved(Piece)
				&& Beam.Structure.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}

		return Stranded;
	}

	FString DescribePieces(const FBeam& Beam, const TArray<int32>& Pieces)
	{
		if (Pieces.Num() == 0)
		{
			return TEXT("{}");
		}

		FString Line = TEXT("{");

		for (int32 Index = 0; Index < Pieces.Num(); ++Index)
		{
			Line += (Index == 0 ? TEXT("") : TEXT(", "));
			Line += Beam.Names[Pieces[Index]];
		}

		return Line + TEXT("}");
	}

	/** What one row's beam did, as built and then after the cascade. */
	struct FBeamResult
	{
		bool bLaid = false;

		/** Read from the non-destructive solve, before anything is allowed to break. */
		double LeftBearingUu = 0.0;
		double RightBearingUu = 0.0;
		double MidspanForceUu = 0.0;
		double MidspanMomentUuCm = 0.0;
		double MidspanUtilisation = 0.0;
		double WorstUtilisation = 0.0;
		int32 WorstJoint = INDEX_NONE;

		/** Passes that broke at least one joint. */
		int32 Passes = 0;

		TArray<int32> Fallen;
		int32 Stranded = 0;
		bool bMidspanGave = false;
	};

	/**
	 * Lay it, read it, then let the cascade run.
	 *
	 * THE READINGS COME FROM SolveLoads AND THE VERDICT FROM SolveAndBreak, in that order.
	 * Solving is non-destructive by contract, so the readings describe the beam AS BUILT rather
	 * than whatever is left of it — which matters the day a row starts breaking things, because a
	 * report of the surviving joints would not explain what broke.
	 */
	void RunBeamCase(
		FAutomationTestBase& Test, const FBeamCase& Case, FBeam& OutBeam, FBeamResult& OutResult)
	{
		LayBeam(Case, OutBeam);

		const FString Where =
			FString::Printf(TEXT("case %d (%s)"), Case.Number, Case.Title);

		if (OutBeam.MidspanJoint == INDEX_NONE
			|| OutBeam.LeftBearingJoint == INDEX_NONE
			|| OutBeam.RightBearingJoint == INDEX_NONE)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: FIXTURE the producer did not emit the midspan glue line and both ")
				TEXT("bearings; it emitted %d joint(s) in total"),
				*Where, OutBeam.Structure.NumConnections()));

			return;
		}

		OutResult.bLaid = true;

		OutBeam.Structure.SolveLoads();

		OutResult.LeftBearingUu =
			FMath::Abs(OutBeam.Structure.GetConnectionForce(OutBeam.LeftBearingJoint).Z);
		OutResult.RightBearingUu =
			FMath::Abs(OutBeam.Structure.GetConnectionForce(OutBeam.RightBearingJoint).Z);
		OutResult.MidspanForceUu =
			OutBeam.Structure.GetConnectionForce(OutBeam.MidspanJoint).Size();
		OutResult.MidspanMomentUuCm =
			OutBeam.Structure.GetConnectionMoment(OutBeam.MidspanJoint).Size();
		OutResult.MidspanUtilisation =
			OutBeam.Structure.GetConnectionUtilisation(OutBeam.MidspanJoint);

		for (int32 Index = 0; Index < OutBeam.Structure.NumConnections(); ++Index)
		{
			const double Utilisation = OutBeam.Structure.GetConnectionUtilisation(Index);

			if (Utilisation > OutResult.WorstUtilisation)
			{
				OutResult.WorstUtilisation = Utilisation;
				OutResult.WorstJoint = Index;
			}
		}

		OutResult.Passes = OutBeam.Structure.SolveAndBreak();
		OutResult.Fallen = FallenPieces(OutBeam);
		OutResult.Stranded = StrandedCount(OutBeam);
		OutResult.bMidspanGave =
			OutBeam.Structure.GetConnection(OutBeam.MidspanJoint).HasGiven();
	}

	/** Everything the solver read, printed whether the row passes or not. */
	void ReportBeamCase(
		FAutomationTestBase& Test, const FBeamCase& Case, const FBeam& Beam, const FBeamResult& Result)
	{
		if (!Result.bLaid)
		{
			return;
		}

		const double DerivedMomentUuCm =
			MidspanMomentUuCm(Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm);
		const double DerivedBending = MidspanBendingUtilisation(
			Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm, Case.MemberBendingMPa);
		const double DerivedShear = BearingShearUtilisation(
			Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm, Case.MemberShearMPa);

		Test.AddInfo(FString::Printf(
			TEXT("case %d (%s) [%s]: %s. block %.4g kg, beam %.4g kg. ")
			TEXT("DERIVED midspan M %.10g uu.cm, bending %.10g of f_m, shear %.10g of f_v. ")
			TEXT("SOLVER bearings %.10g + %.10g = %.10g uu (total weight %.10g uu); ")
			TEXT("midspan |F| %.10g uu, |M| %.10g uu.cm, utilisation %.10g; ")
			TEXT("worst joint %d at %.10g; passes %d; fallen %s; stranded %d"),
			Case.Number, Case.Title, Case.MemberName, VerdictName(Case.Verdict),
			BlockMassKg(Case.BlockHeightCm), BeamMassKg(Case.MemberDensityGramsPerCubicCm),
			DerivedMomentUuCm, DerivedBending, DerivedShear,
			Result.LeftBearingUu, Result.RightBearingUu,
			Result.LeftBearingUu + Result.RightBearingUu,
			(BlockMassKg(Case.BlockHeightCm) + BeamMassKg(Case.MemberDensityGramsPerCubicCm))
				* GravityCmPerSecondSquared,
			Result.MidspanForceUu, Result.MidspanMomentUuCm, Result.MidspanUtilisation,
			Result.WorstJoint, Result.WorstUtilisation, Result.Passes,
			*DescribePieces(Beam, Result.Fallen), Result.Stranded));
	}

	/**
	 * Everything a row must satisfy before its verdict means anything.
	 *
	 * A beam the solver could not route, or one whose joints were emitted at the wrong size, would
	 * produce a verdict that is about the fixture rather than about the physics — and a red for
	 * that reason sends whoever reads it chasing a bug that is not there.
	 */
	void CheckFixture(
		FAutomationTestBase& Test, const FBeamCase& Case, const FBeam& Beam, const FBeamResult& Result)
	{
		const FString Where = FString::Printf(TEXT("case %d (%s)"), Case.Number, Case.Title);

		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: FIXTURE the producer must emit exactly five faces — one glue line, two ")
				TEXT("bearings, two contacts under the block"),
				*Where),
			Beam.Structure.NumConnections(), 5);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: FIXTURE every piece and every joint must know where it is, or there are ")
				TEXT("no moments anywhere and the bearings read a centred load"),
				*Where),
			Beam.Structure.HasCompleteGeometry());

		/*
		 * EVERY EMITTED AREA AGAINST ONE COMPUTED HERE. See SharedFaceAreaSqCm: the two
		 * arithmetics agree only while neither box's span contains the other's, so this is what
		 * pins the fixture's dimensions to the shape it claims to be.
		 */
		for (int32 Index = 0; Index < Beam.Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Beam.Structure.GetConnection(Index);

			int32 SeparationAxis = INDEX_NONE;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (FMath::Abs(Joint.InterfaceNormal[Axis]) > 0.5)
				{
					SeparationAxis = Axis;
				}
			}

			const double TrueAreaSqCm = SharedFaceAreaSqCm(
				Beam.Boxes[Joint.PieceA], Beam.Boxes[Joint.PieceB], SeparationAxis);

			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: FIXTURE joint %d (%s-%s) must be the face those two boxes really ")
					TEXT("share"),
					*Where, Index, *Beam.Names[Joint.PieceA], *Beam.Names[Joint.PieceB]),
				Joint.InterfaceAreaSqCm, TrueAreaSqCm);
		}

		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: FIXTURE no piece may be Stranded — a verdict decided by the solver ")
				TEXT("declining to divide load round a loop is not a verdict about a beam"),
				*Where),
			Result.Stranded, 0);

		/*
		 * BENDING HAS TO BE THE AXIS THAT DECIDES, or this fixture is measuring something other
		 * than what it claims. Worked for all three rows: the ratio is 9.96, 9.58 and 32.5.
		 */
		const double DerivedBending = MidspanBendingUtilisation(
			Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm, Case.MemberBendingMPa);
		const double DerivedShear = BearingShearUtilisation(
			Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm, Case.MemberShearMPa);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: FIXTURE midspan bending (%.10g) must govern the member by a clear ")
				TEXT("margin over shear at the bearing (%.10g)"),
				*Where, DerivedBending, DerivedShear),
			DerivedBending > DerivedShear * 5.0);
	}
}

/**
 * THE CATALOGUE: three configurations, each with the verdict a real beam gives.
 *
 * ALL THREE ROWS ARE RED TODAY, AND NOT FOR THE SAME REASON. Case 1 was always the red the file
 * was written for: it must PartsAtMidspan and the mechanism half never fires (the file header's
 * "SO THE RED IS" paragraph). Cases 2 and 3 were green on arrival until the one-cell thrust gate
 * (08abcfd, 2026-08-09) — see the file header's "HOW THE BEARINGS READ CHANGED" paragraph — and
 * now fail their STANDS assertion the same way case 1 fails its PartsAtMidspan one: the dry
 * bearings unzip regardless of the member's own capacity. What the three rows are for is still the
 * PAIRS: case 1 versus case 2 varies only the block's height; case 1 versus case 3 varies only the
 * member material. Each row's `DropsToday`/`PassesToday` pins today's wrong answer so a further
 * regression cannot hide inside an already-red row (see `FBeamCase::DropsToday`).
 *
 * ROW 1'S ASSERTION IS TWO-SIDED AND IT NEEDS BOTH SIDES. The mechanism half — the glue line at
 * midspan must have GIVEN — is what makes this a member-failure test rather than a collapse test:
 * a beam that came down by sliding off its bearings, or by the block punching through its
 * contact, is a different and wrong answer that an outcome-only assertion would pass. The outcome
 * half — the beam and its load lose the earth while the piers keep it — is what stops a single
 * severed joint being mistaken for a failure, per DESIGN.md §4.
 *
 * AND THE OUTCOME HALF NEEDS MORE THAN MEMBER FAILURE. Once the glue line parts, each half-beam
 * is still sitting on its own pier and this solver has no way to say that a half-beam pivots off
 * its bearing and falls — that is PROJECT_REVIEW.md §2 item 1, the missing global equilibrium
 * check. So row 1 is anchored on step 6 as a whole rather than on member failure alone, and it is
 * written that way deliberately: it is the real-world verdict, which is what an acceptance test
 * is for.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBeamAcceptanceCatalogueTest,
	"DestructionGame.Acceptance.Beam.Catalogue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBeamAcceptanceCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace BeamAcceptanceTestSupport;

	/*
	 * THE CONTACT PROFILE THIS FIXTURE ASSUMES, CHECKED RATHER THAN TAKEN ON TRUST. A beam resting
	 * on a pier has NO BOND: everything it has against sliding is friction bought by its own
	 * weight, which is the one thing DryStone is in the library to express. Both numbers are read
	 * by the fixture's behaviour, so a retune of either should turn this row red rather than
	 * silently move every case below.
	 */
	TestEqual(TEXT("FIXTURE: dry contact has no cohesion at all"),
		DryStone.ShearCohesionMPa, 0.0);

	TestEqual(TEXT("FIXTURE: dry contact carries shear by friction alone, at mu = 0.7"),
		DryStone.FrictionCoefficient, 0.7);

	/* The section modulus every expected value above divides by: b d^2 / 6 = 166.666... cm3. */
	TestEqual(TEXT("FIXTURE: the section modulus of a 100 x 100 mm section is b d^2 / 6"),
		SectionModulusCm3, 10.0 * 10.0 * 10.0 / 6.0);

	const TArray<FBeamCase> Cases = AllBeamCases();

	TestEqual(TEXT("FIXTURE: the catalogue is three cases"), Cases.Num(), 3);

	for (const FBeamCase& Case : Cases)
	{
		FBeam Beam;
		FBeamResult Result;

		RunBeamCase(*this, Case, Beam, Result);
		ReportBeamCase(*this, Case, Beam, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		CheckFixture(*this, Case, Beam, Result);

		const FString Where = FString::Printf(
			TEXT("case %d (%s) [isolates %s]"), Case.Number, Case.Title, Case.Isolates);

		/*
		 * THE REACTIONS HAVE TO ADD UP, whatever else does or does not happen. This is the half of
		 * the force transfer the model genuinely does — the two bearings between them carry the
		 * block and the beam, and nothing else is holding either up — and it is asserted for every
		 * row so that a beam whose load went somewhere else cannot reach a verdict at all.
		 */
		const double TotalWeightUu =
			(BlockMassKg(Case.BlockHeightCm) + BeamMassKg(Case.MemberDensityGramsPerCubicCm))
			* GravityCmPerSecondSquared;

		TestNearlyEqual(
			*FString::Printf(
				TEXT("%s: the two bearings must carry the whole of the beam and its load; they ")
				TEXT("carry %.10g + %.10g against a total weight of %.10g uu"),
				*Where, Result.LeftBearingUu, Result.RightBearingUu, TotalWeightUu),
			Result.LeftBearingUu + Result.RightBearingUu, TotalWeightUu,
			TotalWeightUu * 1.0e-9);

		/*
		 * ALL THREE ROWS ARE PINNED TO THE WRONG ANSWER THE MODEL GIVES TODAY, BEFORE the
		 * verdict-specific assertions below — this must run whether the row's own catalogue verdict
		 * is Stands or PartsAtMidspan, because since the one-cell thrust gate the wrong answer is
		 * the SAME shape on every row (see `FBeamCase::DropsToday`).
		 *
		 * THIS ASSERTS NOTHING ABOUT PHYSICS AND ENDORSES NOTHING, exactly as the wall catalogue's
		 * `DropsToday` pin does not. What it adds is that the KNOWN-WRONG failure has a FIXED SHAPE:
		 * a row that is already red (case 1) or about to go red for the first time here (cases 2
		 * and 3) is otherwise a hole in the net, free to drop a different count or run a different
		 * number of passes without anybody noticing, in exactly the way the 2026-08-09 gate change
		 * hid for two days before the 2026-08-11 oracle sweep found it.
		 */
		if (Case.DropsToday != INDEX_NONE)
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: CHARACTERISATION of a KNOWN RED — since the one-cell thrust gate ")
					TEXT("(08abcfd, 2026-08-09) these DRY bearings lose the kern cap and read Max(), ")
					TEXT("so the model drops %d piece(s) here today regardless of the catalogue's %s ")
					TEXT("verdict. It dropped %d: %s. ACCEPTED AS A KNOWN COST until evolution step 4 ")
					TEXT("(DESIGN.md §7's path; the beam user ruling, DESIGN.md §8, decided ")
					TEXT("2026-08-11) — global equilibrium is what fixes the bearings honestly, not ")
					TEXT("this fixture. If a slice just fixed this row, DELETE its DropsToday and ")
					TEXT("PassesToday in the same edit; if nothing here was meant to change, the ")
					TEXT("model's answer has moved and something else moved it."),
					*Where, Case.DropsToday, VerdictName(Case.Verdict),
					Result.Fallen.Num(), *DescribePieces(Beam, Result.Fallen)),
				Result.Fallen.Num(), Case.DropsToday);

			TestEqual(
				*FString::Printf(
					TEXT("%s: CHARACTERISATION of a KNOWN RED — the cascade runs %d breaking pass(es) ")
					TEXT("here today; see DropsToday just above for the mechanism and the ")
					TEXT("delete-when-fixed instruction, which covers this pin too."),
					*Where, Case.PassesToday),
				Result.Passes, Case.PassesToday);
		}

		if (Case.Verdict == EVerdict::Stands)
		{
			/*
			 * BOTH HALVES. "Nothing fell" alone passes for a beam that severed its glue line and
			 * sat there in two pieces; "no joint gave" alone passes for a beam whose load never
			 * reached anything.
			 */
			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means nothing lost the earth; %s did"),
					*Where, *DescribePieces(Beam, Result.Fallen)),
				Result.Fallen.Num(), 0);

			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means no joint gave; the cascade ran %d breaking pass(es)"),
					*Where, Result.Passes),
				Result.Passes, 0);

			continue;
		}

		/*
		 * THE MECHANISM: the MEMBER is what must fail, and the glue line at midspan IS the member's
		 * critical section. A red here says the beam is over its published bending capacity and
		 * the model has nothing that can express it.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: the member must break at midspan; the glue line %s given, and it read ")
				TEXT("|M| = %.10g uu.cm against the %.10g uu.cm a beam that shape carries there"),
				*Where, Result.bMidspanGave ? TEXT("has") : TEXT("has NOT"),
				Result.MidspanMomentUuCm,
				MidspanMomentUuCm(Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm)),
			Result.bMidspanGave);

		/*
		 * THE OUTCOME, TWO-SIDED: the beam and what it was carrying come down, and the piers do
		 * not. A one-sided assertion would be satisfied by a model that simply drops everything.
		 */
		const TArray<int32> MustFall{ Beam.LeftSegment, Beam.RightSegment, Beam.Block };
		const TArray<int32> MustStand{ Beam.LeftPier, Beam.RightPier };

		TArray<int32> WronglyStanding;
		for (const int32 Piece : MustFall)
		{
			if (!Result.Fallen.Contains(Piece))
			{
				WronglyStanding.Add(Piece);
			}
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s: a broken beam takes itself and its load down; %s kept the earth"),
				*Where, *DescribePieces(Beam, WronglyStanding)),
			WronglyStanding.Num(), 0);

		TArray<int32> WronglyFallen;
		for (const int32 Piece : MustStand)
		{
			if (Result.Fallen.Contains(Piece))
			{
				WronglyFallen.Add(Piece);
			}
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s: the piers are not what failed; %s lost the earth"),
				*Where, *DescribePieces(Beam, WronglyFallen)),
			WronglyFallen.Num(), 0);
	}

	return true;
}

/**
 * THE MIDSPAN SECTION MUST CARRY THE BEAM'S BENDING MOMENT.
 *
 * The sharpest single statement of the gap, and the most actionable: PROJECT_REVIEW.md §2 item 3
 * says a piece on two or more supports has its moment zeroed by the area split, and this is that
 * sentence turned into a number. Each half-beam is seated on its own pier, so the midspan glue
 * line is the support of nobody and carries exactly nothing — the beam's entire bending action is
 * missing, not merely under-reported.
 *
 * ASSERTED AGAINST THE STATICS RATHER THAN AGAINST "NON-ZERO", because "non-zero" would be
 * satisfied by any accident. The expected value is the free-body moment about x = 0, worked in
 * MidspanMomentUuCm from the reaction, the block's two contact patches and the half-beam's own
 * weight — none of which is how the solver computes anything.
 *
 * TEN PERCENT, WHICH IS NOT A TOLERANCE ON ARITHMETIC. Both numbers are exact; the band exists
 * because a solver that computed internal member actions might legitimately place the load's
 * resultant slightly differently across a 200 cm contact, and this file should not be the thing
 * that dictates that. It is nowhere near wide enough to admit the answer today, which is zero.
 *
 * ALL THREE ROWS, because the gap is a property of the ROUTING and has nothing to do with which
 * material the member is made of. A fix that closed it for the timber rows only would be reading
 * the material somewhere it must not.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBeamAcceptanceMidspanMomentTest,
	"DestructionGame.Acceptance.Beam.MidspanCarriesTheMembersBendingMoment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBeamAcceptanceMidspanMomentTest::RunTest(const FString& Parameters)
{
	using namespace BeamAcceptanceTestSupport;

	for (const FBeamCase& Case : AllBeamCases())
	{
		FBeam Beam;
		FBeamResult Result;

		RunBeamCase(*this, Case, Beam, Result);
		ReportBeamCase(*this, Case, Beam, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		const double ExpectedUuCm =
			MidspanMomentUuCm(Case.BlockHeightCm, Case.MemberDensityGramsPerCubicCm);

		TestTrue(
			*FString::Printf(
				TEXT("case %d (%s): the midspan section carries the beam's bending moment; it ")
				TEXT("reads %.10g uu.cm against %.10g, a ratio of %.10g"),
				Case.Number, Case.Title, Result.MidspanMomentUuCm, ExpectedUuCm,
				ExpectedUuCm == 0.0 ? 0.0 : Result.MidspanMomentUuCm / ExpectedUuCm),
			FMath::Abs(Result.MidspanMomentUuCm - ExpectedUuCm) <= ExpectedUuCm * 0.1);
	}

	return true;
}

/**
 * THE MEMBER MATERIAL HAS TO DECIDE THE OUTCOME.
 *
 * Cases 1 and 3 are the same geometry under the same block; the only thing that differs is what
 * the beam is made of, and steel is eleven times stronger in bending than C24. Timber at 3.48 of
 * capacity must fail and steel at 0.34 must not.
 *
 * WHY IT IS WORTH A TEST OF ITS OWN RATHER THAN BEING IMPLIED BY THE CATALOGUE. The catalogue can
 * be made to pass by a model that always answers "falls" for a heavy load and "stands" for a
 * light one — load magnitude is a variable it already reads. This row says the answer must change
 * when the LOAD DOES NOT, which is the only form of the data-drivenness claim a model with no
 * concept of member strength cannot fake. DESIGN.md §4: "run the same scenario on wood and
 * confirm it survives where brick failed. If that passes, the system is proven data-driven."
 *
 * TODAY BOTH ROWS ANSWER IDENTICALLY, which is the finding, not an accident.
 *
 * THE MECHANISM OF THAT IDENTITY MOVED AT SLICE 3b/4 (2026-08-27) BUT THE FINDING DID NOT. Until
 * then both rows FELL (3 pieces each) because the dry bearings read Max(); now the equilibrium LP
 * is the break authority below the cap and stands the bearings, so both rows STAND (0 fallen
 * each). Either way the member material changes nothing, because the solver still has no member-
 * failure mechanism — so timber over its bending capacity does not part where steel does not, and
 * this assertion stays RED until evolution step 6. The count in the message is dynamic; it now
 * reads 0 and 0 rather than 3 and 3.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBeamAcceptanceMemberMaterialTest,
	"DestructionGame.Acceptance.Beam.TheMemberMaterialDecidesTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBeamAcceptanceMemberMaterialTest::RunTest(const FString& Parameters)
{
	using namespace BeamAcceptanceTestSupport;

	const TArray<FBeamCase> Cases = AllBeamCases();

	const FBeamCase& Timber = Cases[0];
	const FBeamCase& Steel = Cases[2];

	TestEqual(TEXT("FIXTURE: the pair carries the same block, so only the member differs"),
		Timber.BlockHeightCm, Steel.BlockHeightCm);

	TestTrue(TEXT("FIXTURE: the pair really is two different member materials"),
		Timber.MemberBendingMPa != Steel.MemberBendingMPa);

	FBeam TimberBeam;
	FBeamResult TimberResult;
	RunBeamCase(*this, Timber, TimberBeam, TimberResult);
	ReportBeamCase(*this, Timber, TimberBeam, TimberResult);

	FBeam SteelBeam;
	FBeamResult SteelResult;
	RunBeamCase(*this, Steel, SteelBeam, SteelResult);
	ReportBeamCase(*this, Steel, SteelBeam, SteelResult);

	if (!TimberResult.bLaid || !SteelResult.bLaid)
	{
		return false;
	}

	TestTrue(
		*FString::Printf(
			TEXT("the same load on the same beam must part C24 (%.10g of f_m,k) and not S275 ")
			TEXT("(%.10g of f_y); the model gave the timber %d fallen piece(s) and the steel %d"),
			MidspanBendingUtilisation(
				Timber.BlockHeightCm, Timber.MemberDensityGramsPerCubicCm, Timber.MemberBendingMPa),
			MidspanBendingUtilisation(
				Steel.BlockHeightCm, Steel.MemberDensityGramsPerCubicCm, Steel.MemberBendingMPa),
			TimberResult.Fallen.Num(), SteelResult.Fallen.Num()),
		TimberResult.Fallen.Num() > SteelResult.Fallen.Num());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
