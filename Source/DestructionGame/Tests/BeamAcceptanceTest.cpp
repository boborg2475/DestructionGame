// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The beam acceptance set — a simply supported beam under a heavy weight, where the member is what
 * should fail and no joint should.
 *
 * The acceptance anchor for two PROJECT_REVIEW.md §2 gaps that meet here: item 3, a piece on two or
 * more supports has its bending moment zeroed by the area split, so "a lintel or floor slab on two
 * walls can never fail in midspan bending"; and item 7, pieces never fail, only joints do — fatal
 * for wood, whose primary mode is member bending. Evolution steps 5 and 6.
 *
 * FStructure pieces carry only mass, so a member's own strength never enters the solve and there is
 * no path for "the beam snapped". A joint does carry a strength profile, so the beam is two
 * collinear segments meeting at midspan whose joint carries the member material's own published
 * strengths rather than a mortar's; snapping in midspan bending is that glue line parting.
 * Everything else — two bearings, two contacts under the weight — is bond-free resting contact,
 * which is what a beam sitting on a pier is.
 *
 * Two segments, not three or five, and the solver forced it: an even count puts a glue line at
 * exact midspan, and more than two leaves the middle ones seatless, so ReseatSpannedGroups forms
 * them into a spanning group and ApplyArchingThrust pushes the bearings apart with
 * H = 3W/(8 d_e/L). Measured on a five-segment draft, H/V = 3L/(4 d_e) came out at 7.2 against dry
 * stone's friction coefficient of 0.7 — the beam sheared off both bearings, a collapse at the right
 * load for entirely the wrong reason, which would have made the "wood falls" row pass while saying
 * nothing about members. Two segments have a seat each and take no phantom thrust.
 *
 * What the solver does with this fixture, measured: each half-beam is seated on its own pier, so
 * neither one's SupportConnections uses the midspan joint and ConnectionForces leaves the glue line
 * at exactly zero. So the red is that the member carries nothing — a beam under three and a half
 * times its published bending capacity whose midspan section reads a bending moment of exactly 0.0.
 * The bearings do carry the load (they sum to the total weight, asserted below), each eccentrically.
 *
 * Those bearing readings changed on 2026-08-09 (the one-cell thrust gate, commit 08abcfd). Before
 * it, FConnection::ArchingMomentScale capped each bearing's eccentric moment to the kern edge and
 * the light rows stood at ~0.31/~0.34; the gate grants that cap only where the springing can carry
 * the implied thrust, which these dry bearings cannot, so the uncapped moment on a zero-tension
 * joint reads Max() and all three rows unzip in one pass (3 fallen, including the light rows whose
 * real-world verdict is STANDS). The rigid-block LP oracle stands all three (lambda* 1.76 / 19.2 /
 * 17.4 — RigidBlockOracleSweepTest), so this is production's missing global equilibrium, not the
 * fixture's physics — accepted as a known cost until evolution step 4 promotes equilibrium to the
 * cascade authority (DESIGN.md §8, 2026-08-11), and pinned per row by `FBeamCase::DropsToday`.
 *
 * Displacement is never the break assertion (DESIGN.md §4). What is read is whether a piece still
 * has a path to the earth after the cascade, plus — for the row that must fail — the mechanism,
 * that the glue line itself gave: a beam that came down by sliding off its bearings is a wrong
 * answer an outcome-only assertion would pass.
 *
 * Needs a ticking world: no. Gravity is on and the assertions are on outcome and solver state; the
 * only thing a world adds is the wire to Chaos, which Tests/StructureIntegrationTest.cpp covers
 * identically for all three rows. Named, not anonymous, namespace: an anonymous namespace is
 * private to a translation unit, and a unity build merges many files into one.
 *
 * Nothing is imported from the code under test except the producer — section modulus, statics, the
 * newton-to-Unreal conversion and every published strength are re-derived below, so a wrong
 * constant in production makes this file disagree with it rather than agree. The one exception is
 * Layout::MakeInterface, since re-implementing it would re-implement part of the thing under test,
 * and even that is checked joint by joint against interface areas computed here.
 */
namespace BeamAcceptanceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * THE GEOMETRY. Every length is centimetres, at Unreal's default 1 uu = 1 cm.
	 * ================================================================================ */

	/**
	 * A 100 x 100 mm sawn section, a real timber size and a real steel bar size.
	 *
	 * Width is across the beam (Y), depth is vertical (Z) — the pairing the section modulus below
	 * depends on, since bending about Y is resisted across the depth and the depth is what gets
	 * squared. Swapping them gives a plausible number against the wrong section.
	 */
	constexpr double SectionWidthCm = 10.0;
	constexpr double SectionDepthCm = 10.0;

	/** Two segments meeting at x = 0, so the glue line sits at exact midspan. */
	constexpr double SegmentLengthCm = 220.0;
	constexpr double BeamLengthCm = SegmentLengthCm * 2.0;

	/** Bearing centre to bearing centre. The beam oversails each bearing centre by 20 cm. */
	constexpr double SpanCm = 400.0;

	/** How much of the beam actually lands on a pier, along the beam. */
	constexpr double BearingLengthCm = 40.0;

	/*
	 * The piers run past the beam's end on the outside — a shape chosen to sidestep a MakeInterface
	 * defect since fixed (2026-08-08): it computed axis overlap as `extentA + extentB - distance`,
	 * which over-reported under containment, and now takes the true interval intersection, with
	 * Layout.Interface's contained-pier rows pinning that case. The oversailing piers stay because
	 * the readings below are anchored to this geometry; moving them inboard needs re-derived
	 * numbers as its own slice, not a drive-by.
	 */
	constexpr double PierLengthCm = 60.0;
	constexpr double PierHeightCm = 40.0;

	/**
	 * The weight: a steel plate laid along the beam, its thickness matching the beam's width.
	 *
	 * Matching the beam's width is the same containment rule one axis over: a wider block would
	 * contain the beam on Y, and its two contact patches would be emitted 50 cm wide, not 10.
	 */
	constexpr double BlockLengthCm = 200.0;
	constexpr double BlockWidthCm = SectionWidthCm;

	/* ================================================================================
	 * UNITS AND PUBLISHED MATERIAL DATA. Every figure is cited; none is imported.
	 * ================================================================================ */

	/**
	 * 980 cm/s2. With 1 uu = 1 cm and mass in kilograms, MassKg * 980 is already a weight in Unreal
	 * force units — DESIGN.md §3's 1 N = 100 uu is inside that number, and applying it a second time
	 * is the 100x error the units section exists to prevent.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * Unreal force units that load one square centimetre to one megapascal.
	 *
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa (= 1 N/mm2) over 1 cm2 is 10000 uu. Deliberately
	 * not DestructionForce::ForceUnitsPerMPaSqCm: this file has to fail if that constant is wrong.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/* --- C24 softwood, EN 338 (strength class table) ------------------------------------- */

	/**
	 * The bending capacity this whole file turns on, applied to both extreme fibres.
	 *
	 * Both fibres is a decision, not an oversight. EN 338 also publishes f_t,0,k = 14 N/mm2 and
	 * f_c,0,k = 21 N/mm2, but those are axial capacities; a section in pure bending is checked in
	 * EN 1995-1-1 §6.1.6 by the single ratio sigma_m,d / f_m,d over the whole stress block, and a
	 * simply supported beam under gravity develops no axial force. Using f_c,0,k = 21 against the
	 * bending compression edge would fail every timber beam about 14% early and move the governing
	 * axis off the tension face, where timber actually splinters. Both figures are quoted rather
	 * than declared because neither is used, and a constant nobody reads can drift.
	 */
	/*
	 * Mean basis since the 2026-08-13 re-anchor: 24.0 was EN 338's characteristic f_m,k, and
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
	 * Mean, not characteristic: rho_k = 350 kg/m3 is the 5-percentile used for fastener design, but
	 * what a beam weighs is the mean, and weight is all density does here. Unreal's g/cm3 is the
	 * published unit divided by 1000.
	 */
	constexpr double C24DensityGramsPerCubicCm = 0.42;

	/* --- S275 structural steel, EN 10025-2 ------------------------------------------------ */

	/**
	 * f_y = 275 N/mm2, the nominal yield of grade S275 (EN 10025-2, t <= 16 mm).
	 *
	 * EN 1993-1-1 Table 3.1 steps the design value down for thick product — 255 N/mm2 for
	 * 40 < t <= 80 mm — and this fixture's section is 100 mm. Taking 255 instead moves the steel
	 * row from 0.335 to 0.362 of capacity and changes no verdict, so the nominal grade figure is
	 * used and the reduction is recorded rather than applied. Both fibres again, needing no
	 * argument here: steel yields at f_y in tension and compression alike.
	 */
	/*
	 * Mean (static) yield since the 2026-08-13 re-anchor: JCSS PMC Part 3 Table A,
	 * E[f_y] = f_y,sp * alpha * exp(-u v) - C with v = 0.07, u in [-1.5, -2.0], alpha = 1.0
	 * for flanges and C = 20 MPa (the mill-test to static-yield correction): 285-296 MPa for
	 * S275. 290 is the centre. (The old 275.0 was EN 10025-2's nominal.)
	 */
	constexpr double S275YieldMPa = 290.0;

	/**
	 * f_y / sqrt(3) = 158.771 N/mm2 — the von Mises shear yield, EN 1993-1-1 §6.2.6. Written as the
	 * division so it moves with f_y and cannot drift from it.
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
	 * None of this mirrors production, which is the only reason it is worth having: the solver
	 * accumulates weight down a support graph, while this is the free-body diagram of a simply
	 * supported beam. The two are derived differently and are meant to be compared.
	 */

	/** W = b d^2 / 6 for a rectangle. b is the width across the beam; d is the depth being bent. */
	constexpr double SectionModulusCm3 =
		SectionWidthCm * SectionDepthCm * SectionDepthCm / 6.0;

	/** The face the two segments meet across: the beam's own cross-section. */
	constexpr double SectionAreaSqCm = SectionWidthCm * SectionDepthCm;

	/**
	 * Where the block's weight lands on each half of the beam: the block spans
	 * -BlockLengthCm/2 .. +BlockLengthCm/2 and the beam parts at x = 0, so each half takes half the
	 * block over a contact whose centroid is a quarter of the block's length from midspan.
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
	 * The reaction at each bearing, uu. Symmetric load on a symmetric span, so each pier takes half
	 * of everything. The piers' own weight is not in it: they are grounded, and the earth takes it.
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
	 * Ordinary statics: what a beam's midspan section carries, and the number the model has no way
	 * to produce.
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
	 * Exists so "bending governs" is measured rather than asserted: a fixture whose shear happened
	 * to be worse would be testing something else while still going red. 1.5 * V / A is the peak of
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
		 * How many live pieces the model drops here today: a characterisation of a wrong answer, not
		 * an expectation, and it endorses nothing (exactly per FWallCase::DropsToday in
		 * WallAcceptanceTest.cpp, which this mirrors). `Verdict` above is what a real beam does;
		 * this is what the solver does instead, measured off a run and written down.
		 *
		 * It is set on every row rather than only the PartsAtMidspan one, because the wrong answer
		 * is not particular to case 1: since the one-cell thrust gate (08abcfd, 2026-08-09) refused
		 * the kern cap to this fixture's dry bearings, the uncapped eccentric moment on a
		 * zero-tension joint reads Max() on every bearing and the two STANDS rows unzip exactly as
		 * case 1 does. The rigid-block LP oracle stands all three (lambda* 1.76 / 19.2 / 17.4 —
		 * RigidBlockOracleSweepTest), so this is production's missing global equilibrium (DESIGN.md
		 * §7 gap 1 / evolution step 4), never the fixture's physics.
		 *
		 * The pin exists because that gate flipped every row's bearing reading and nobody saw it
		 * until the 2026-08-11 oracle sweep, since nothing here pinned what production does today.
		 * Accepted as a known cost until evolution step 4 promotes equilibrium to the cascade
		 * authority (DESIGN.md §8, decided 2026-08-11). When that lands, delete DropsToday and
		 * PassesToday in the same edit — they will fail, and that failure is the reminder; never
		 * "update" them to a new wrong number without saying why the answer moved.
		 */
		int32 DropsToday = INDEX_NONE;

		/**
		 * How many breaking passes the cascade ran here today — the same characterisation as
		 * DropsToday, read the same way and covered by its delete-when-fixed rule. Kept separate so
		 * a change in the shape of the known-wrong answer (the same piece count over a different
		 * number of passes) is visible rather than hidden by an unchanged count.
		 */
		int32 PassesToday = INDEX_NONE;
	};

	/**
	 * The member's glue line, as a connection profile.
	 *
	 * Friction is exactly zero, and that is load-bearing rather than tidy. Mohr-Coulomb's
	 * `cohesion + mu * compressive stress` describes an interface whose resistance to sliding grows
	 * as you squeeze it; a cross-section through a solid member is not an interface, and its shear
	 * strength is a material property indifferent to axial stress. mu = 0 reduces the envelope to
	 * three independent axes exactly (DESIGN.md §3), and it keeps the shear axis from being inflated
	 * by the compression sharing the section, which would flatter the shear check this file uses to
	 * prove bending governs. No shear ceiling for the same reason: nothing left to truncate.
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
		 * The DropsToday / PassesToday pins are gone since slice 3b/4 (2026-08-27). All three rows
		 * used to drop 3 in 1 pass because the one-cell thrust gate let their dry bearings read
		 * Max() outside the kern. Below the 200-block cap the equilibrium LP is now the sole break
		 * authority and stands all three bearings (the oracle sweep reads lambda* 2.65 / 28.80 /
		 * 18.30, all >= 1), so production drops 0 in 0 passes and the characterisation is deleted
		 * per FBeamCase::DropsToday's own rule. Rows 2 and 3 (Stands) are green. Row 1 stays red on
		 * its member-failure assertions alone (|M| = 0 at midspan, the beam does not part): fixing
		 * the bearings gives the solver no way to fail the member, which is evolution step 6.
		 */
		TArray<FBeamCase> Cases;

		/*
		 * Row 1 — the member-failure case, and the reason the file exists. A 1884 kg steel plate on
		 * a 100 x 100 mm C24 joist over a 4 m span: midspan moment 139,288,968 uu.cm = 13,928.9 N.m
		 * against a section modulus of 166.67 cm3, so the extreme fibre sits at 83.57 MPa against
		 * the mean 36 MPa — 2.32 times capacity (3.48x the retired characteristic 24; the
		 * 2026-08-14 re-anchor moved the margin, not the verdict). The beam breaks, and nothing else
		 * in the fixture is within two orders of magnitude of its own limit.
		 */
		Cases.Add({
			1, TEXT("C24 timber beam, heavy load"), TEXT("member material (vs case 3)"),
			TEXT("C24 timber"), C24DensityGramsPerCubicCm, C24BendingMPa, C24ShearMPa,
			/*BlockHeightCm*/ 120.0, EVerdict::PartsAtMidspan,
			/*DropsToday*/ INDEX_NONE, /*PassesToday*/ INDEX_NONE });

		/*
		 * Row 2 — the same beam well inside capacity, differing from row 1 only in block height.
		 * 157 kg, midspan moment 12,354,468 uu.cm, extreme fibre 7.41 MPa, 0.206 of the mean 36
		 * (0.309 of the retired characteristic 24). A joist under a sensible load: it must stand.
		 */
		Cases.Add({
			2, TEXT("C24 timber beam, light load"), TEXT("load magnitude (vs case 1)"),
			TEXT("C24 timber"), C24DensityGramsPerCubicCm, C24BendingMPa, C24ShearMPa,
			/*BlockHeightCm*/ 10.0, EVerdict::Stands,
			/*DropsToday*/ INDEX_NONE, /*PassesToday*/ INDEX_NONE });

		/*
		 * Row 3 — the steel twin: identical geometry and block, only the member material changes.
		 * The heavier beam raises the midspan moment to 153,706,140 uu.cm and the extreme fibre to
		 * 92.22 MPa, but S275's mean static yield is 290, so it reads 0.318 and holds (0.335 against
		 * the old nominal 275). Wood failing where steel holds under the same load is the whole
		 * data-drivenness claim. Material discrimination is 290/36 = 8.1x (was 9.8x on the
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
	 * What this guards changed on 2026-08-08: production used to compute
	 * `extentA + extentB - distance`, wrong under containment, so this was a tripwire. MakeInterface
	 * now uses the same interval intersection, so what remains is a plain consistency check that the
	 * fixture's bookkeeping matches what the producer emitted. An independent re-derivation of
	 * production's arithmetic therefore needs a different expression for the same set:
	 * `min(2·extentA, 2·extentB, extentA + extentB - |centre difference|)` per axis, the oracle
	 * Layout.InterfaceFuzz is specified to use (see CURRENT_STATE.md, Layout producer).
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
	 * Joint thickness is zero everywhere, because every joint here is dry contact: beam on piers,
	 * block on beam, the two member halves butting. MakeInterface accepts a zero-thickness face
	 * deliberately — dry stone has faces touching with no gap — and it keeps every centroid an exact
	 * binary number, which is what lets the expected values above be quoted to the last digit.
	 */
	void LayBeam(const FBeamCase& Case, FBeam& OutBeam)
	{
		const double BeamBottomZCm = PierHeightCm;
		const double BeamCentreZCm = BeamBottomZCm + SectionDepthCm / 2.0;
		const double BeamTopZCm = BeamBottomZCm + SectionDepthCm;

		/*
		 * The pier reaches from outside the beam's end to BearingLengthCm inboard, so the bearing
		 * patch is centred exactly SpanCm/2 from midspan and the boxes overlap partially rather than
		 * one containing the other. See PierLengthCm.
		 */
		const double BeamEndCm = BeamLengthCm / 2.0;
		const double PierInnerCm = BeamEndCm - BearingLengthCm;
		const double PierCentreCm = PierInnerCm + PierLengthCm / 2.0;

		const auto AddBox = [&OutBeam](
			const FString& Name, const FPieceBox& Box, double DensityGramsPerCubicCm, bool bGrounded)
		{
			/*
			 * The box's centre is the centre of mass: every piece here is a homogeneous solid whose
			 * mass came off the same box. Without it there is no eccentricity anywhere and the
			 * bearings read a centred load they are not carrying — the state HasCompleteGeometry
			 * exists to make askable, asserted below as a fixture precondition.
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
		 * Every pair is offered and MakeInterface refuses the ones that are not faces — pier to
		 * pier, block to pier, half-beam to far pier. Deciding which pairs touch is the producer's
		 * job; what this fixture checks below is the area of everything it accepted. Only the
		 * segment-to-segment joint carries the member's own strengths; everything else is dry.
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
	 * The readings come from SolveLoads and the verdict from SolveAndBreak, in that order. Solving
	 * is non-destructive by contract, so the readings describe the beam as built rather than what is
	 * left of it — a report of the surviving joints would not explain what broke.
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
	 * Everything a row must satisfy before its verdict means anything. A beam the solver could not
	 * route, or whose joints were emitted at the wrong size, gives a verdict about the fixture
	 * rather than the physics — and a red for that reason sends a reader chasing a bug that is not
	 * there.
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
		 * Every emitted area against one computed here. See SharedFaceAreaSqCm: this pins the
		 * fixture's dimensions to the shape it claims to be.
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
		 * Bending has to be the axis that decides, or this fixture measures something other than
		 * what it claims. Worked for all three rows: the ratio is 9.96, 9.58 and 32.5.
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
 * The catalogue: three configurations, each with the verdict a real beam gives.
 *
 * What the three rows are for is the pairs: case 1 versus case 2 varies only the block's height,
 * case 1 versus case 3 only the member material. Case 1 is the red the file was written for — it
 * must PartsAtMidspan and the mechanism half never fires. Each row's `DropsToday`/`PassesToday`
 * pins whatever wrong answer production gives so a further regression cannot hide inside an
 * already-red row; see `FBeamCase::DropsToday` for what those pins are and are not.
 *
 * Row 1's assertion is two-sided and needs both sides. The mechanism half — the midspan glue line
 * must have given — is what makes this a member-failure test rather than a collapse test: a beam
 * that came down by sliding off its bearings, or by the block punching through its contact, is a
 * wrong answer an outcome-only assertion would pass. The outcome half — the beam and its load lose
 * the earth while the piers keep it — stops a single severed joint being read as a failure
 * (DESIGN.md §4).
 *
 * That outcome half needs more than member failure: once the glue line parts, each half-beam still
 * sits on its own pier and this solver cannot say that a half-beam pivots off its bearing and
 * falls (PROJECT_REVIEW.md §2 item 1, missing global equilibrium). Row 1 is therefore anchored on
 * step 6 as a whole, deliberately, because the real-world verdict is what an acceptance test is
 * for.
 *
 * Needs a ticking world: no. See the file header.
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
	 * The contact profile this fixture assumes, checked rather than trusted. A beam resting on a
	 * pier has no bond: all it has against sliding is friction bought by its own weight, which is
	 * what DryStone is in the library to express. Both numbers are read by the fixture's behaviour,
	 * so a retune of either should turn this row red rather than silently move every case below.
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
		 * The reactions have to add up, whatever else happens: the two bearings between them carry
		 * the block and the beam and nothing else holds either up. Asserted on every row so a beam
		 * whose load went somewhere else cannot reach a verdict at all.
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
		 * The known-wrong-answer pin, run before the verdict-specific assertions below and on every
		 * row whatever its catalogue verdict, because the wrong answer has the same shape on all
		 * three. What it adds is that the shape is fixed: an already-red row is otherwise free to
		 * drop a different count or run a different number of passes unnoticed. See
		 * `FBeamCase::DropsToday`.
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
			 * Both halves: "nothing fell" alone passes a beam that severed its glue line and sat
			 * there in two pieces, and "no joint gave" alone passes a beam whose load never reached
			 * anything.
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
		 * The mechanism: the member is what must fail, and the midspan glue line is the member's
		 * critical section. A red here says the beam is over its published bending capacity and the
		 * model has nothing that can express it.
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
		 * The outcome, two-sided: the beam and what it carried come down and the piers do not. A
		 * one-sided assertion would be satisfied by a model that simply drops everything.
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
 * The midspan section must carry the beam's bending moment.
 *
 * The sharpest and most actionable statement of the gap: PROJECT_REVIEW.md §2 item 3 says a piece
 * on two or more supports has its moment zeroed by the area split, and this is that sentence turned
 * into a number. Each half-beam is seated on its own pier, so the midspan glue line supports nobody
 * and carries exactly nothing — the bending action is missing, not merely under-reported.
 *
 * Asserted against the statics rather than against "non-zero", which any accident would satisfy:
 * the expected value is the free-body moment about x = 0, worked in MidspanMomentUuCm from the
 * reaction, the block's two contact patches and the half-beam's own weight, none of which is how
 * the solver computes anything.
 *
 * The ten percent is not a tolerance on arithmetic — both numbers are exact. The band exists
 * because a solver computing internal member actions might legitimately place the load's resultant
 * slightly differently across a 200 cm contact, and this file should not dictate that. It is
 * nowhere near wide enough to admit today's answer, which is zero.
 *
 * All three rows, because the gap is a property of the routing, not of the member material: a fix
 * that closed it for the timber rows only would be reading material somewhere it must not.
 *
 * Needs a ticking world: no.
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
 * The member material has to decide the outcome.
 *
 * Cases 1 and 3 are the same geometry under the same block and differ only in what the beam is made
 * of: timber at 3.48 of capacity must fail and steel at 0.34 must not. Worth its own test because
 * the catalogue can be passed by a model that always answers "falls" for a heavy load and "stands"
 * for a light one — load magnitude is a variable it already reads. This row says the answer must
 * change when the load does not, the one form of the data-drivenness claim a model with no concept
 * of member strength cannot fake. DESIGN.md §4: "run the same scenario on wood and confirm it
 * survives where brick failed. If that passes, the system is proven data-driven."
 *
 * Green since the first-crack promotion (2026-08-28), by the glue-line route, and the verdict moved
 * twice on the way. Until slice 3b/4 both rows fell (3 pieces each) because the dry bearings read
 * Max(); slice 3b/4 made the equilibrium LP the break authority below the cap, which stood the
 * bearings, so both rows stood (0 each) and the material still changed nothing. First-crack (D1,
 * 2026-08-28) breaks that tie: a bonded joint cracks at first crack, so the weaker C24 glue line
 * parts and the beam falls (3, lambda* 0.883) while the stronger S275 one holds (0, lambda* 6.10).
 * The route is the joint cracking, not the member snapping — the true member-bending mechanism is
 * still evolution step 6, and its driving red
 * (`Beam.MidspanCarriesTheMembersBendingMoment`, |M| = 0) is intact.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBeamAcceptanceMemberMaterialTest,
	"DestructionGame.Acceptance.Beam.TheMemberMaterialDecidesTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBeamAcceptanceMemberMaterialTest::RunTest(const FString& Parameters)
{
	using namespace BeamAcceptanceTestSupport;

	const TArray<FBeamCase> Cases = AllBeamCases();

	/*
	 * TimberCase, not Timber: a block-local `Timber` shadows the global profile
	 * DestructionProfiles::Timber, tripping C4459 (warning-as-error) whenever this file's transitive
	 * using-directives bring that global into scope. The rename is behaviour-neutral.
	 */
	const FBeamCase& TimberCase = Cases[0];
	const FBeamCase& Steel = Cases[2];

	TestEqual(TEXT("FIXTURE: the pair carries the same block, so only the member differs"),
		TimberCase.BlockHeightCm, Steel.BlockHeightCm);

	TestTrue(TEXT("FIXTURE: the pair really is two different member materials"),
		TimberCase.MemberBendingMPa != Steel.MemberBendingMPa);

	FBeam TimberBeam;
	FBeamResult TimberResult;
	RunBeamCase(*this, TimberCase, TimberBeam, TimberResult);
	ReportBeamCase(*this, TimberCase, TimberBeam, TimberResult);

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
				TimberCase.BlockHeightCm, TimberCase.MemberDensityGramsPerCubicCm, TimberCase.MemberBendingMPa),
			MidspanBendingUtilisation(
				Steel.BlockHeightCm, Steel.MemberDensityGramsPerCubicCm, Steel.MemberBendingMPa),
			TimberResult.Fallen.Num(), SteelResult.Fallen.Num()),
		TimberResult.Fallen.Num() > SteelResult.Fallen.Num());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
