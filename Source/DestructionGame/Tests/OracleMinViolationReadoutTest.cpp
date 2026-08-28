// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 6a OF THE STEP-4 PROMOTION — THE MIN-VIOLATION LP THAT SOURCES THE STRAIN READOUT, RED.
 *
 * The shipped strain overlay (the magenta/cyan per-joint utilisation) needs a per-joint number.
 * The maximise-lambda LP cannot give one that is not a lie: at lambda >= 1 its primal is DEFINED
 * as the search for a force system in which no joint reads over 1.0, so feeding those forces
 * through a utilisation reads "misleadingly comfortable" for every joint (PROMOTION_DESIGN §3.1).
 * The readout therefore needs a DIFFERENT solve — the min-violation (goal-programming) LP of §3.5:
 *
 *   fix the load at lambda = 1 (real self-weight, posed with bGravityIsLive = false so gravity is
 *   the dead load); keep the per-block equilibrium EQUALITY rows HARD; add one non-negative slack
 *   s_k >= 0 to every STRENGTH inequality row, relaxing a_k.x <= b_k to a_k.x <= b_k + s_k; and
 *   MINIMISE sum w_k*s_k.
 *
 * Because equilibrium stays hard and only strength is relaxed, a solution ALWAYS exists — the
 * least-infeasible force system. It returns, per joint: (a) a REAL primal (N, M) — the
 * closest-to-admissible equilibrium, NOT vacuous — and (b) the slack, whose ratio to capacity is a
 * genuine per-joint "distance past failure" (0 = within capacity, > 0 = over). A STANDING structure
 * reads all slacks ZERO; an OVER-CAPACITY one reads positive slack EXACTLY on the joints that are
 * over.
 *
 * THESE THREE TESTS, all oracle-level, headless, seeded/deterministic, in the DEFAULT suite:
 *
 *   1. VIOLATION CORRECTNESS against an INDEPENDENT hand oracle. A STANDING stack reads zero
 *      violation on every joint and a real equilibrium (N per joint = the weight it carries). An
 *      OVER-CAPACITY tension hang reads positive violation of a HAND-DERIVED magnitude on the one
 *      over joint and zero on a control joint beside it, and the per-joint utilisation lands on the
 *      0 -> 1 -> >1 scale.
 *   2. PERMUTATION DETERMINISM — the penalty-weight gate (the tau-analog). A weighted-L1 objective
 *      picks *a* sparse violation set rather than *the* set, so ill-chosen weights could name a
 *      permutation-unstable set. On a statically-indeterminate over-capacity fixture with genuine
 *      choice (three piers sharing one overload — the total is fixed but its distribution is a free
 *      family), the named violated set AND the per-joint utilisation magnitudes must be IDENTICAL
 *      across seeded column permutations. A FAILURE HERE MEANS THE PENALTY WEIGHTS ARE ILL-POSED
 *      (a design gate), NOT A TUNING NIT.
 *   3. VERIFICATION fail-closed. The min-violation primal must be a genuine EQUILIBRIUM (the hard
 *      equality rows hold within tolerance even where the joint is over capacity) — asserted via
 *      the resultant balancing the dead load. Specified: if the solve cannot satisfy equilibrium
 *      (it always can — the formulation is feasible by construction) it must REFUSE, not return
 *      garbage.
 *
 * THE INDEPENDENT ORACLE IS RIGID-BODY STATICS BY HAND — demand minus capacity from the free-body,
 * derived a DIFFERENT way than the LP. An oracle that mirrored the LP's own slack would be
 * worthless. Units are derived in this file (1 MPa over 1 cm2 = 10000 uu), never imported, so a
 * wrong production constant disagrees rather than agrees.
 *
 * THE THING TESTED MUST GOVERN. The over-capacity fixture's joint is built with EVERY OTHER
 * strength axis uncapped (>= the oracle's UncappedStrengthMPa, so those rows are not even
 * assembled), leaving TENSION as the only row that can carry slack — so the measured violation is
 * unambiguously the tension over-stress and not friction or crushing quietly governing. The 3-pier
 * gate fixture is likewise pure crushing (dry, f_t = 0, c = 0, central vertical load => zero shear).
 *
 * RED FOR THE RIGHT REASON. FOracleResult::Readout is empty by default (bPresent = false, no joint
 * entries), so every "the readout is present", "N balances the weight", "violation == the hand
 * over-stress", "utilisation > 1", "the base names a non-empty violated set" assertion fails on
 * MISSING behaviour, not a compile error or a broken fixture. The zero-violation arms on a standing
 * joint are green on arrival (0 == 0); each is called out with the mutation that proves it bites
 * once dev implements the formulation.
 *
 * NEEDS A TICKING WORLD: NO. Every fixture is a pure hand-built FOracleProblem fed to
 * SolveRigidBlock — no bridge, no Chaos, no world tick.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace MinViolationReadoutSupport
{
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * UNITS, derived here so a wrong production constant fails rather than agrees.
	 * ================================================================================ */

	/** MassKg * 980 is a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. NOT imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/** Above the oracle's UncappedStrengthMPa (1e9): a strength row this large is not assembled. */
	constexpr double UncappedHere = 1.0e12;

	double WeightUu(double MassKg) { return MassKg * GravityCmPerSecondSquared; }

	/* ================================================================================
	 * BLOCK / JOINT BUILDERS. Hand-built at the oracle level so the statics stay checkable.
	 * ================================================================================ */

	FOracleBlock GroundedBlock(double CentreXCm, double CentreZCm)
	{
		FOracleBlock B;
		B.MassKg = 1.0;
		B.CentroidXCm = CentreXCm;
		B.CentroidZCm = CentreZCm;
		B.bGrounded = true;
		return B;
	}

	FOracleBlock FreeBlock(double MassKg, double CentreXCm, double CentreZCm)
	{
		FOracleBlock B;
		B.MassKg = MassKg;
		B.CentroidXCm = CentreXCm;
		B.CentroidZCm = CentreZCm;
		B.bGrounded = false;
		return B;
	}

	/** A bed joint with the given upward-or-downward unit normal, plus an explicit strength. */
	FOracleJoint Joint(
		int32 BlockA, int32 BlockB, double NormalX, double NormalZ,
		double CentreXCm, double CentreZCm, double HalfLengthCm, double AreaSqCm,
		const FConnectionStrength& Strength)
	{
		FOracleJoint J;
		J.BlockA = BlockA;
		J.BlockB = BlockB;
		J.NormalX = NormalX;
		J.NormalZ = NormalZ;
		J.CentreXCm = CentreXCm;
		J.CentreZCm = CentreZCm;
		J.HalfLengthCm = HalfLengthCm;
		J.AreaSqCm = AreaSqCm;
		J.Strength = Strength;
		return J;
	}

	/** A strong bond: every axis far above any demand these fixtures apply, so violation is zero. */
	FConnectionStrength StrongBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 100.0;   /* clay-brick order, >> the tiny compression here */
		S.ShearCohesionMPa = 100.0;
		S.TensileStrengthMPa = 100.0;
		S.FrictionCoefficient = 0.75;
		/* MaxShearStrengthMPa left at its unbounded default. */
		return S;
	}

	/**
	 * A joint with a WEAK finite tension bond and EVERY OTHER axis uncapped, so the ONLY strength
	 * row that exists is tension. This is what makes the over-capacity fixture's governing axis
	 * unambiguous: no friction or crushing row can carry slack, so the measured violation is the
	 * tension over-stress and nothing else.
	 */
	FConnectionStrength WeakTensionOnly(double TensileStrengthMPa)
	{
		FConnectionStrength S;
		S.TensileStrengthMPa = TensileStrengthMPa; /* finite: tension rows ARE written */
		S.CompressiveStrengthMPa = UncappedHere;   /* no crushing row */
		S.ShearCohesionMPa = UncappedHere;         /* no friction row */
		S.FrictionCoefficient = 0.0;
		/* MaxShearStrengthMPa unbounded by default => no ceiling row. */
		return S;
	}

	/**
	 * A dry-stone-like joint with a finite crushing cap and NO tension/cohesion. Under a central
	 * vertical load (zero shear) the only strength row that can carry slack is crushing — which is
	 * what makes the 3-pier gate's governing axis unambiguous.
	 */
	FConnectionStrength CrushingOnly(double CompressiveStrengthMPa)
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = CompressiveStrengthMPa; /* finite: crushing rows ARE written */
		S.TensileStrengthMPa = 0.0;                        /* dry: no tension column/row at all */
		S.ShearCohesionMPa = 0.0;                          /* friction row |v| <= mu*n, slack-free at v=0 */
		S.FrictionCoefficient = 0.6;
		return S;
	}

	/* ================================================================================
	 * FIXTURE STAND — A CENTRED TWO-BRICK STACK ON A GROUNDED SEAT (FEASIBLE).
	 *
	 * Two free bricks, both centred over a grounded seat, pure compression. Every joint is within
	 * capacity, so the min-violation LP reads ZERO violation on both, and the real primal it hands
	 * back is the ACTUAL load path: the bottom bed joint carries BOTH bricks' weight, the top bed
	 * joint carries the upper brick's. Those N values ARE the per-block equilibrium (sum Fz = 0)
	 * expressed through the joint normal, so asserting them is asserting the primal is a genuine
	 * equilibrium.
	 * ================================================================================ */

	namespace Stand
	{
		constexpr double BrickMassKg = 3.895; /* 20 x 10.25 x 10 cm clay at 1.9 g/cc */
		constexpr double AreaSqCm = 205.0;    /* full 20 x 10.25 bed face */
		constexpr double HalfLenCm = 10.0;

		enum { Seat = 0, Lower = 1, Upper = 2 };
		enum { JLower = 0, JUpper = 1 };

		FOracleProblem Build()
		{
			FOracleProblem P;
			P.bGravityIsLive = false;      /* load fixed at lambda = 1: gravity is the dead load */
			P.bMinViolationReadout = true; /* ask for the strain readout */

			P.Blocks.Add(GroundedBlock(0.0, 5.0));            /* Seat  = 0, top at Z = 10 */
			P.Blocks.Add(FreeBlock(BrickMassKg, 0.0, 15.0));  /* Lower = 1, Z in [10, 20] */
			P.Blocks.Add(FreeBlock(BrickMassKg, 0.0, 25.0));  /* Upper = 2, Z in [20, 30] */

			const FConnectionStrength Bond = StrongBond();
			P.Joints.Add(Joint(Seat, Lower, 0.0, 1.0, 0.0, 10.0, HalfLenCm, AreaSqCm, Bond));   /* JLower = 0 */
			P.Joints.Add(Joint(Lower, Upper, 0.0, 1.0, 0.0, 20.0, HalfLenCm, AreaSqCm, Bond));  /* JUpper = 1 */
			return P;
		}

		/** The bottom joint carries two bricks, the top joint one — compression positive. */
		double ExpectedNormalLowerUu() { return 2.0 * WeightUu(BrickMassKg); }
		double ExpectedNormalUpperUu() { return WeightUu(BrickMassKg); }
	}

	/* ================================================================================
	 * FIXTURE OVER — A TENSION HANG PAST ITS BOND, WITH A COMPRESSION CONTROL JOINT BESIDE IT.
	 *
	 * Substructure 1 (OVER, joint 0): a heavy block hangs BELOW a grounded anchor from one bed
	 * joint whose only strength is a weak tension bond. Under its own weight the joint must carry
	 * the whole weight in TENSION; the bond capacity is far less, so the joint is over capacity.
	 *
	 *   HAND STATICS (independent of the LP):
	 *     demand   = W  (the hang's weight, carried entirely in tension across the joint)
	 *     capacity = f_t * Conv * A   (full-face tension capacity)
	 *     violation = demand - capacity  = W - f_t*Conv*A          (force units, > 0)
	 *     utilisation = demand / capacity = W / (f_t*Conv*A)  = 1 + violation/capacity
	 *   N (compression positive) = -W  (tension reads negative); M = 0 (central load, central joint).
	 *
	 * Substructure 2 (CONTROL, joint 1): a light brick rests on a grounded seat far away in X, pure
	 * compression, strong bond — ZERO violation, N = +W_control. It is the "and zero elsewhere" arm.
	 * ================================================================================ */

	namespace Over
	{
		constexpr double HangMassKg = 30.0;      /* W = 29400 uu */
		constexpr double HangAreaSqCm = 100.0;   /* 10 x 10 face */
		constexpr double HangHalfLenCm = 5.0;
		constexpr double TensileMPa = 0.01;      /* capacity = 0.01 * 10000 * 100 = 10000 uu */

		constexpr double ControlMassKg = 3.895;  /* W = 3817.1 uu */
		constexpr double ControlAreaSqCm = 205.0;
		constexpr double ControlHalfLenCm = 10.0;

		enum { Anchor = 0, Hang = 1, Seat = 2, Rest = 3 };
		enum { JHang = 0, JControl = 1 };

		FOracleProblem Build()
		{
			FOracleProblem P;
			P.bGravityIsLive = false;
			P.bMinViolationReadout = true;

			/* ---- OVER: grounded anchor ABOVE, hang below, normal points anchor -> hang = down. ---- */
			P.Blocks.Add(GroundedBlock(0.0, 40.0));               /* Anchor = 0, bottom at Z = 35 */
			P.Blocks.Add(FreeBlock(HangMassKg, 0.0, 25.0));       /* Hang   = 1, Z in [20, 30] */

			P.Joints.Add(Joint(Anchor, Hang, 0.0, -1.0, 0.0, 32.5, HangHalfLenCm, HangAreaSqCm,
				WeakTensionOnly(TensileMPa)));                    /* JHang = 0 */

			/* ---- CONTROL: seat below, rest brick on top, ordinary compression bed joint. ---- */
			P.Blocks.Add(GroundedBlock(300.0, 5.0));              /* Seat = 2, top at Z = 10 */
			P.Blocks.Add(FreeBlock(ControlMassKg, 300.0, 15.0));  /* Rest = 3, Z in [10, 20] */

			P.Joints.Add(Joint(Seat, Rest, 0.0, 1.0, 300.0, 10.0, ControlHalfLenCm, ControlAreaSqCm,
				StrongBond()));                                   /* JControl = 1 */
			return P;
		}

		double HangWeightUu() { return WeightUu(HangMassKg); }
		double HangCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * HangAreaSqCm; }
		double HangViolationUu() { return HangWeightUu() - HangCapacityUu(); }
		double HangUtilisation() { return HangWeightUu() / HangCapacityUu(); }
		double ControlNormalUu() { return WeightUu(ControlMassKg); }
	}

	/* ================================================================================
	 * FIXTURE PIERS — A RIGID CAP ON THREE PIERS, OVERLOADED IN CRUSHING (THE GATE FIXTURE).
	 *
	 * One heavy free cap block bears on three collinear grounded piers through three bed joints.
	 * The cap's weight exceeds the three joints' combined crushing capacity, so SOME joints must be
	 * over capacity. But the structure is statically INDETERMINATE (three vertical reactions, two
	 * equilibrium equations), so the TOTAL over-stress is fixed while its DISTRIBUTION among the
	 * three joints is a one-parameter family — every distribution with each reaction >= the cap
	 * carries the same total slack. That is genuine choice: a weighted-L1 min-violation lands on a
	 * VERTEX of that optimal face, and which vertex depends on column order unless the readout is
	 * canonicalized. This is the tau-analog the determinism gate must bite on.
	 *
	 * Pure crushing: dry (f_t = 0, c = 0), central symmetric vertical load => zero shear, so the
	 * only strength rows that can carry slack are crushing.
	 * ================================================================================ */

	namespace Piers
	{
		constexpr double CapMassKg = 500.0;      /* W = 490000 uu */
		constexpr double AreaSqCm = 100.0;       /* per joint face */
		constexpr double HalfLenCm = 5.0;
		constexpr double CompressiveMPa = 0.1;   /* per-joint cap = 0.1 * 10000 * 100 = 100000 uu */

		FOracleProblem Build()
		{
			FOracleProblem P;
			P.bGravityIsLive = false;
			P.bMinViolationReadout = true;

			const double PierX[3] = { -100.0, 0.0, 100.0 };
			const int32 Pier0 = P.Blocks.Add(GroundedBlock(PierX[0], 10.0));
			const int32 Pier1 = P.Blocks.Add(GroundedBlock(PierX[1], 10.0));
			const int32 Pier2 = P.Blocks.Add(GroundedBlock(PierX[2], 10.0));
			const int32 Cap = P.Blocks.Add(FreeBlock(CapMassKg, 0.0, 25.0)); /* centroid over pier 1 */

			const FConnectionStrength Bond = CrushingOnly(CompressiveMPa);
			const int32 Piers[3] = { Pier0, Pier1, Pier2 };
			for (int32 I = 0; I < 3; ++I)
			{
				P.Joints.Add(Joint(Piers[I], Cap, 0.0, 1.0, PierX[I], 20.0, HalfLenCm, AreaSqCm, Bond));
			}
			return P;
		}

		double CapWeightUu() { return WeightUu(CapMassKg); }
		double TotalCrushCapacityUu() { return 3.0 * CompressiveMPa * ForceUnitsPerMPaSqCmHere * AreaSqCm; }
		double TotalViolationUu() { return CapWeightUu() - TotalCrushCapacityUu(); }
	}

	/* ================================================================================
	 * FIXTURE TWOGROUPS — TWO INDEPENDENT INDETERMINATE OVER-CAPACITY GROUPS AT DIFFERENT
	 * EVEN-SPREAD LEVELS (THE MULTI-GROUP DETERMINISM GATE).
	 *
	 * Two copies of PIERS — a rigid cap on three symmetric collinear grounded piers, dry /
	 * crushing-governed, central vertical load so shear is zero — placed FAR APART in X so they
	 * are mechanically independent (no joint couples the two caps; each cap balances on its own
	 * three piers). The two groups are deliberately at DIFFERENT over-stress levels:
	 *
	 *   Group A (heavy): cap 500 kg (W_A = 490000 uu), per-joint crushing cap 100000 uu
	 *                    (f_c = 0.1 MPa over 100 cm2). Even spread reads W_A/3 - cap
	 *                    = 163333.33 - 100000 = 63333.33 violation on EACH of its three joints.
	 *   Group B (light): cap 200 kg (W_B = 196000 uu), per-joint crushing cap 40000 uu
	 *                    (f_c = 0.04 MPa over 100 cm2). Even spread reads W_B/3 - cap
	 *                    = 65333.33 - 40000 = 25333.33 violation on EACH of its three joints.
	 *
	 * A's even over-stress level (63333.33) STRICTLY EXCEEDS B's (25333.33). THAT gap is what a
	 * single GLOBAL minimax t cannot canonicalize: the global t is pinned by A's max slack, so
	 * once A is even the objective is INDIFFERENT to B's distribution (every B split with each
	 * slack <= the global t carries the same Sum s_k), and B lands on a permutation-dependent
	 * Bland vertex — its centre joint piling toward A's level (up to ~63333) instead of B's own
	 * even ~25333. The physically-correct answer is each group at ITS OWN centroid (even spread),
	 * which only a PER-GROUP canonicalization (lexicographic minimax / L2) delivers.
	 *
	 * THE INDEPENDENT ORACLE, per group and per joint, is the centroid of the free family: three
	 * symmetric piers under a central load share the cap weight equally, so each joint carries
	 * W/3 in compression (N = W/3, M = 0) and reads violation W/3 - cap_joint, utilisation
	 * (W/3)/cap_joint. Derived from rigid-body statics, NOT from the LP slack.
	 * ================================================================================ */

	namespace TwoGroups
	{
		constexpr double AreaSqCm = 100.0;   /* per joint face */
		constexpr double HalfLenCm = 5.0;
		constexpr double PierOffsetCm = 100.0;

		/* Group A — heavy cap, higher even over-stress level. */
		constexpr double CapMassA = 500.0;        /* W_A = 490000 uu */
		constexpr double CompressiveMPaA = 0.1;   /* cap_joint_A = 0.1 * 10000 * 100 = 100000 uu */
		constexpr double GroupACentreXCm = 0.0;

		/* Group B — light cap, lower even over-stress level, placed far away in X. */
		constexpr double CapMassB = 200.0;        /* W_B = 196000 uu */
		constexpr double CompressiveMPaB = 0.04;  /* cap_joint_B = 0.04 * 10000 * 100 = 40000 uu */
		constexpr double GroupBCentreXCm = 1000.0;

		/* Base-problem joint layout: joints 0,1,2 are group A; joints 3,4,5 are group B. */
		enum { JA0 = 0, JA1 = 1, JA2 = 2, JB0 = 3, JB1 = 4, JB2 = 5 };

		FOracleProblem Build()
		{
			FOracleProblem P;
			P.bGravityIsLive = false;
			P.bMinViolationReadout = true;

			/* ---- Group A: three grounded piers under one heavy cap. ---- */
			const double AX[3] = { GroupACentreXCm - PierOffsetCm, GroupACentreXCm, GroupACentreXCm + PierOffsetCm };
			const int32 PA0 = P.Blocks.Add(GroundedBlock(AX[0], 10.0));
			const int32 PA1 = P.Blocks.Add(GroundedBlock(AX[1], 10.0));
			const int32 PA2 = P.Blocks.Add(GroundedBlock(AX[2], 10.0));
			const int32 CapA = P.Blocks.Add(FreeBlock(CapMassA, GroupACentreXCm, 25.0));

			const FConnectionStrength BondA = CrushingOnly(CompressiveMPaA);
			const int32 PiersA[3] = { PA0, PA1, PA2 };
			for (int32 I = 0; I < 3; ++I)
			{
				P.Joints.Add(Joint(PiersA[I], CapA, 0.0, 1.0, AX[I], 20.0, HalfLenCm, AreaSqCm, BondA));
			}

			/* ---- Group B: three grounded piers under one lighter cap, far away in X. ---- */
			const double BX[3] = { GroupBCentreXCm - PierOffsetCm, GroupBCentreXCm, GroupBCentreXCm + PierOffsetCm };
			const int32 PB0 = P.Blocks.Add(GroundedBlock(BX[0], 10.0));
			const int32 PB1 = P.Blocks.Add(GroundedBlock(BX[1], 10.0));
			const int32 PB2 = P.Blocks.Add(GroundedBlock(BX[2], 10.0));
			const int32 CapB = P.Blocks.Add(FreeBlock(CapMassB, GroupBCentreXCm, 25.0));

			const FConnectionStrength BondB = CrushingOnly(CompressiveMPaB);
			const int32 PiersB[3] = { PB0, PB1, PB2 };
			for (int32 I = 0; I < 3; ++I)
			{
				P.Joints.Add(Joint(PiersB[I], CapB, 0.0, 1.0, BX[I], 20.0, HalfLenCm, AreaSqCm, BondB));
			}

			return P;
		}

		double WeightAUu() { return WeightUu(CapMassA); }
		double WeightBUu() { return WeightUu(CapMassB); }
		double CapJointAUu() { return CompressiveMPaA * ForceUnitsPerMPaSqCmHere * AreaSqCm; }
		double CapJointBUu() { return CompressiveMPaB * ForceUnitsPerMPaSqCmHere * AreaSqCm; }

		/* Each of a group's three joints carries W/3 in the even spread. */
		double EvenNormalAUu() { return WeightAUu() / 3.0; }
		double EvenNormalBUu() { return WeightBUu() / 3.0; }
		double EvenViolationAUu() { return EvenNormalAUu() - CapJointAUu(); }
		double EvenViolationBUu() { return EvenNormalBUu() - CapJointBUu(); }
		double EvenUtilA() { return EvenNormalAUu() / CapJointAUu(); }
		double EvenUtilB() { return EvenNormalBUu() / CapJointBUu(); }

		/* The whole group's over-stress total — a sanity check the fixture is genuinely overloaded. */
		double TotalViolationAUu() { return WeightAUu() - 3.0 * CapJointAUu(); }
		double TotalViolationBUu() { return WeightBUu() - 3.0 * CapJointBUu(); }

		/* True for the three group-A joints (0,1,2) in the BASE problem, false for B (3,4,5). */
		bool IsGroupA(int32 BaseJoint) { return BaseJoint < 3; }
	}

	/* ================================================================================
	 * FIXTURE DEGEN — THE ≥2-DOF INTRA-GROUP DEGENERATE TRADE (the 6a residual probe).
	 *
	 * This is the corner the 6a re-review could not realize as a concrete FOracleProblem: within ONE
	 * independent overload group at ONE stress level, a member that is FORCED critical (its reaction
	 * uniquely determined — irreducible) sitting BESIDE a REDUCIBLE PAIR that can TRADE load within a
	 * fixed subtotal, with MIXED CAPACITIES on the pair so the reduction's min-sum optimal FACE is
	 * NON-SINGLETON. Every owned fixture (STAND/OVER/PIERS/TWOGROUPS) is a symmetric bound-pinned
	 * SINGLETON whose reduction collapses to one even point, so none of them can probe this.
	 *
	 * THE GEOMETRY. A rigid cap bears on THREE grounded piers through three bed joints:
	 *   - CENTRAL pier at x = 0 — its equilibrium column is DISTINCT, so its reaction is UNIQUELY
	 *     determined by moment + vertical equilibrium: R_C = W - S, forced. Over its crushing cap it
	 *     is irreducibly CRITICAL.
	 *   - TWO FLANKING piers at the SAME x = 100 — their normal-force columns are IDENTICAL (same
	 *     coefficient in the cap's Fz and moment rows), so equilibrium sees only their SUM S; the
	 *     split R_L1 vs R_L2 is a FREE 1-DOF family. That is the trade. Point contacts (HalfLen = 0)
	 *     keep the statics a clean 3-reaction model with no per-joint local moment.
	 *
	 * Moment equilibrium about the cap centroid (x_c) pins the flank SUBTOTAL:
	 *     S = R_L1 + R_L2 = ((x_c - x_central)/(x_flank - x_central)) * W = (40/100)*W = 196000 uu,
	 *     R_C = W - S = 294000 uu (W = 500 kg * 980 = 490000 uu).
	 *
	 * MIXED FLANK CAPACITIES are the lever that makes the min-sum reduction FACE non-singleton: with
	 * the pair's whole feasible range keeping BOTH flanks over their (different) caps, the pair's
	 * total slack is CONSTANT along the trade (const = S - cap_L1 - cap_L2), so min Sum s_k is
	 * INDIFFERENT to the split. The simplex lands on a VERTEX of that flat face — and the danger is a
	 * vertex where a flank slack hits t*, getting FALSELY declared critical and pinned there.
	 *
	 * THE REGIME, chosen so the corner actually bites (all in force units):
	 *     t*   = s_C   = R_C - cap_C          = 294000 - 190000 = 104000   (central forced at t*)
	 *     const = S - cap_L1 - cap_L2         = 196000 - 30000 - 50000 = 116000   (pair total slack)
	 *   with  t* <= const <= 2*t*  (104000 <= 116000 <= 208000):
	 *     - const >= t*  => a flank slack CAN reach t* (the false-critical vertex EXISTS), and
	 *     - const <= 2*t* => the EVEN pair slack (const/2 = 58000) sits strictly BELOW t*, so the
	 *       even answer is genuinely reducible and NOT critical. The physically-canonical answer is
	 *       the even (lexicographic-minimax / L2-minimal on the slacks) split: 58000 slack per flank.
	 *
	 * THE INDEPENDENT ORACLE is rigid-body statics by hand: S and R_C from equilibrium, then the even
	 * split of the traded subtotal. The lexicographic-minimax canonicalization evens the SLACKS, so
	 * each flank reads slack const/2 = 58000, normal cap_flank + 58000 (88000 low-cap, 108000
	 * high-cap — EQUAL slacks, UNEQUAL reactions because the caps differ; that inequality is what no
	 * symmetric owned fixture exposes).
	 * ================================================================================ */

	namespace Degen
	{
		constexpr double CapMassKg = 500.0;        /* W = 490000 uu */
		constexpr double AreaSqCm = 100.0;         /* per joint face */
		constexpr double HalfLenCm = 0.0;          /* POINT CONTACT: clean 3-reaction statics, no local moment */

		constexpr double CentralXCm = 0.0;         /* distinct column => R_C uniquely determined */
		constexpr double FlankXCm = 100.0;         /* BOTH flanking piers share this x => identical columns => trade */
		constexpr double CapCentroidXCm = 40.0;    /* sets S = 0.4 W by moment equilibrium */

		constexpr double FcCentral = 0.19;         /* cap_C   = 0.19 * 10000 * 100 = 190000 uu */
		constexpr double FcFlankLow = 0.03;        /* cap_L1  = 0.03 * 10000 * 100 =  30000 uu */
		constexpr double FcFlankHigh = 0.05;       /* cap_L2  = 0.05 * 10000 * 100 =  50000 uu — DIFFERENT: the lever */

		enum { JCentral = 0, JFlankLow = 1, JFlankHigh = 2 };

		FOracleProblem Build()
		{
			FOracleProblem P;
			P.bGravityIsLive = false;
			P.bMinViolationReadout = true;

			const int32 Central = P.Blocks.Add(GroundedBlock(CentralXCm, 10.0));
			const int32 FlankA = P.Blocks.Add(GroundedBlock(FlankXCm, 10.0));
			const int32 FlankB = P.Blocks.Add(GroundedBlock(FlankXCm, 10.0));
			const int32 Cap = P.Blocks.Add(FreeBlock(CapMassKg, CapCentroidXCm, 25.0));

			P.Joints.Add(Joint(Central, Cap, 0.0, 1.0, CentralXCm, 20.0, HalfLenCm, AreaSqCm,
				CrushingOnly(FcCentral)));   /* JCentral = 0 */
			P.Joints.Add(Joint(FlankA, Cap, 0.0, 1.0, FlankXCm, 20.0, HalfLenCm, AreaSqCm,
				CrushingOnly(FcFlankLow)));  /* JFlankLow = 1 */
			P.Joints.Add(Joint(FlankB, Cap, 0.0, 1.0, FlankXCm, 20.0, HalfLenCm, AreaSqCm,
				CrushingOnly(FcFlankHigh))); /* JFlankHigh = 2 */
			return P;
		}

		double CapWeightUu() { return WeightUu(CapMassKg); }

		/* Statics — S and R_C from equilibrium (central at x=0, flanks at x=100). */
		double SubtotalUu() /* S = R_L1 + R_L2 */
		{
			return ((CapCentroidXCm - CentralXCm) / (FlankXCm - CentralXCm)) * CapWeightUu();
		}
		double CentralNormalUu() { return CapWeightUu() - SubtotalUu(); } /* R_C */

		double CapCentralUu() { return FcCentral * ForceUnitsPerMPaSqCmHere * AreaSqCm; }
		double CapFlankLowUu() { return FcFlankLow * ForceUnitsPerMPaSqCmHere * AreaSqCm; }
		double CapFlankHighUu() { return FcFlankHigh * ForceUnitsPerMPaSqCmHere * AreaSqCm; }

		double CentralViolationUu() { return CentralNormalUu() - CapCentralUu(); } /* t* */
		double PairTotalSlackUu() { return SubtotalUu() - CapFlankLowUu() - CapFlankHighUu(); } /* const */
		double EvenFlankSlackUu() { return PairTotalSlackUu() / 2.0; } /* const/2 */

		/* The physically-canonical (even-slack) flank reactions — UNEQUAL because caps differ. */
		double FlankLowNormalUu() { return CapFlankLowUu() + EvenFlankSlackUu(); }
		double FlankHighNormalUu() { return CapFlankHighUu() + EvenFlankSlackUu(); }

		/* Per-joint expected utilisation = N / cap_joint on the canonical answer. */
		double CentralUtil() { return CentralNormalUu() / CapCentralUu(); }
		double FlankLowUtil() { return FlankLowNormalUu() / CapFlankLowUu(); }
		double FlankHighUtil() { return FlankHighNormalUu() / CapFlankHighUu(); }
	}

	/* ================================================================================
	 * READOUT INSPECTION HELPERS.
	 * ================================================================================ */

	bool HasJoint(const FOracleReadout& R, int32 J) { return R.Joints.IsValidIndex(J); }

	double ViolationOf(const FOracleReadout& R, int32 J)
	{
		return R.Joints.IsValidIndex(J) ? R.Joints[J].ViolationUu : 0.0;
	}

	double UtilisationOf(const FOracleReadout& R, int32 J)
	{
		return R.Joints.IsValidIndex(J) ? R.Joints[J].Utilisation : 0.0;
	}

	double NormalOf(const FOracleReadout& R, int32 J)
	{
		return R.Joints.IsValidIndex(J) ? R.Joints[J].NormalUu : 0.0;
	}

	/** The set of oracle-joint indices the readout says are OVER capacity (positive violation). */
	TSet<int32> ViolatedJoints(const FOracleReadout& R, double AbsTolUu)
	{
		TSet<int32> Over;
		for (int32 J = 0; J < R.Joints.Num(); ++J)
		{
			if (R.Joints[J].ViolationUu > AbsTolUu)
			{
				Over.Add(J);
			}
		}
		return Over;
	}

	bool Near(double A, double B, double Tol) { return FMath::Abs(A - B) <= Tol; }

	/* ================================================================================
	 * PERMUTATION — reorder an FOracleProblem's blocks and joints by seeded permutations,
	 * remapping every block reference, so the same physics is posed in a different column
	 * and row order. NewIndex = Perm[OldIndex].
	 * ================================================================================ */

	TArray<int32> SeededPermutation(FRandomStream& Rng, int32 N)
	{
		TArray<int32> Perm;
		Perm.SetNumUninitialized(N);
		for (int32 I = 0; I < N; ++I)
		{
			Perm[I] = I;
		}
		for (int32 I = N - 1; I > 0; --I)
		{
			const int32 J = Rng.RandRange(0, I);
			Swap(Perm[I], Perm[J]);
		}
		TArray<int32> Inv;
		Inv.SetNumUninitialized(N);
		for (int32 New = 0; New < N; ++New)
		{
			Inv[Perm[New]] = New;
		}
		return Inv;
	}

	FOracleProblem Permute(const FOracleProblem& In, const TArray<int32>& BlockPerm, const TArray<int32>& JointPerm)
	{
		FOracleProblem Out;
		Out.bGravityIsLive = In.bGravityIsLive;
		Out.bFirstCrackRows = In.bFirstCrackRows;
		Out.bMinViolationReadout = In.bMinViolationReadout;

		Out.Blocks.SetNum(In.Blocks.Num());
		for (int32 Old = 0; Old < In.Blocks.Num(); ++Old)
		{
			Out.Blocks[BlockPerm[Old]] = In.Blocks[Old];
		}

		Out.Joints.SetNum(In.Joints.Num());
		for (int32 Old = 0; Old < In.Joints.Num(); ++Old)
		{
			FOracleJoint J = In.Joints[Old];
			J.BlockA = BlockPerm[J.BlockA];
			J.BlockB = BlockPerm[J.BlockB];
			Out.Joints[JointPerm[Old]] = J;
		}
		return Out;
	}
}

/* ================================================================================================
 * TEST 1 — VIOLATION CORRECTNESS AGAINST AN INDEPENDENT HAND ORACLE.
 *
 * ASSERTS (against demand-minus-capacity statics worked BY HAND, NOT the LP slack):
 *   - Fixture STAND (feasible): every joint reads ZERO violation, the real primal N per joint
 *     equals the weight it carries (bottom joint = two bricks, top joint = one), M ~ 0, and every
 *     utilisation is <= 1.
 *   - Fixture OVER (tension hang): the over joint reads violation == W - f_t*Conv*A to a
 *     tolerance, utilisation == W / (f_t*Conv*A) > 1, |N| == W with N < 0 (tension); the control
 *     joint reads ZERO violation and utilisation <= 1. Positive violation lands EXACTLY on the
 *     over joint.
 *   - The 0 -> 1 -> >1 mapping: violation > 0 iff utilisation > 1, tying the readout to the scale
 *     FConnection::UtilisationUnder produces so Slice 6b can consume it.
 *
 * RED BECAUSE: FOracleResult::Readout is empty by default, so "present", "N == weight",
 * "violation == the hand over-stress", "utilisation > 1" all fail on missing behaviour. The
 * zero-violation arms on STAND and on OVER's control joint are green on arrival (0 == 0).
 *
 * MUTATION THAT PROVES THE GREEN-ON-ARRIVAL ARMS BITE once dev implements the formulation: charge
 * a spurious constant slack to an in-capacity joint (or drop the min from the objective so a
 * standing joint is relaxed anyway) — STAND's and the control joint's zero-violation assertions go
 * red. The N/violation/utilisation reds above already bite from the empty stub.
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMinViolationCorrectnessTest,
	"DestructionGame.Oracle.RigidBlock.Readout.ViolationMatchesTheHandOracle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMinViolationCorrectnessTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace MinViolationReadoutSupport;

	/* ---------------- FIXTURE STAND: feasible, zero violation, real load path ---------------- */
	{
		const FOracleProblem P = Stand::Build();
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("STAND: answered %d, readout present %d, joints %d; expected N lower %.6g, upper %.6g"),
			R.bAnswered ? 1 : 0, R.Readout.bPresent ? 1 : 0, R.Readout.Joints.Num(),
			Stand::ExpectedNormalLowerUu(), Stand::ExpectedNormalUpperUu()));

		TestTrue(TEXT("STAND: the oracle answers"), R.bAnswered);

		TestTrue(TEXT("STAND [RED]: the min-violation readout is present"), R.Readout.bPresent);
		TestEqual(TEXT("STAND [RED]: one readout entry per joint"), R.Readout.Joints.Num(), P.Joints.Num());

		/* Zero violation everywhere — green on arrival; the mutation that bites is in the header. */
		TestTrue(
			*FString::Printf(TEXT("STAND: bottom joint within capacity, violation %.6g ~ 0"),
				ViolationOf(R.Readout, Stand::JLower)),
			Near(ViolationOf(R.Readout, Stand::JLower), 0.0, 1.0e-3));
		TestTrue(
			*FString::Printf(TEXT("STAND: top joint within capacity, violation %.6g ~ 0"),
				ViolationOf(R.Readout, Stand::JUpper)),
			Near(ViolationOf(R.Readout, Stand::JUpper), 0.0, 1.0e-3));

		/* The real primal IS the load path — asserting N is asserting per-block equilibrium (sum Fz = 0). */
		const double TolN = 1.0e-3 * Stand::ExpectedNormalLowerUu();
		TestTrue(
			*FString::Printf(TEXT("STAND [RED]: bottom joint carries both bricks, N %.6g == %.6g"),
				NormalOf(R.Readout, Stand::JLower), Stand::ExpectedNormalLowerUu()),
			Near(NormalOf(R.Readout, Stand::JLower), Stand::ExpectedNormalLowerUu(), TolN));
		TestTrue(
			*FString::Printf(TEXT("STAND [RED]: top joint carries the upper brick, N %.6g == %.6g"),
				NormalOf(R.Readout, Stand::JUpper), Stand::ExpectedNormalUpperUu()),
			Near(NormalOf(R.Readout, Stand::JUpper), Stand::ExpectedNormalUpperUu(), TolN));

		/* Every utilisation within capacity — the standing side of the 0 -> 1 -> >1 scale. */
		TestTrue(TEXT("STAND: bottom joint utilisation <= 1"), UtilisationOf(R.Readout, Stand::JLower) <= 1.0 + 1.0e-6);
		TestTrue(TEXT("STAND: top joint utilisation <= 1"), UtilisationOf(R.Readout, Stand::JUpper) <= 1.0 + 1.0e-6);
	}

	/* ---------------- FIXTURE OVER: one joint past its tension bond, one control ---------------- */
	{
		const FOracleProblem P = Over::Build();
		const FOracleResult R = SolveRigidBlock(P);

		const double W = Over::HangWeightUu();
		const double Cap = Over::HangCapacityUu();
		const double ExpViolation = Over::HangViolationUu();
		const double ExpUtil = Over::HangUtilisation();

		AddInfo(FString::Printf(
			TEXT("OVER: answered %d, readout present %d, joints %d; HAND hang W %.6g, cap %.6g => "
				 "violation %.6g, utilisation %.6g; control N %.6g"),
			R.bAnswered ? 1 : 0, R.Readout.bPresent ? 1 : 0, R.Readout.Joints.Num(),
			W, Cap, ExpViolation, ExpUtil, Over::ControlNormalUu()));

		TestTrue(TEXT("OVER: the hand over-stress is a clear one, utilisation > 2"), ExpUtil > 2.0);

		TestTrue(TEXT("OVER: the oracle answers"), R.bAnswered);
		TestTrue(TEXT("OVER [RED]: the min-violation readout is present"), R.Readout.bPresent);
		TestEqual(TEXT("OVER [RED]: one readout entry per joint"), R.Readout.Joints.Num(), P.Joints.Num());

		/* The over joint's violation matches the independently-derived demand - capacity. */
		const double TolViolation = 1.0e-3 * FMath::Max(1.0, ExpViolation);
		TestTrue(
			*FString::Printf(TEXT("OVER [RED]: hang joint violation %.6g == hand W - cap %.6g"),
				ViolationOf(R.Readout, Over::JHang), ExpViolation),
			Near(ViolationOf(R.Readout, Over::JHang), ExpViolation, TolViolation));

		/* Utilisation == demand/capacity, on the > 1 side of the scale, and == 1 + violation/cap. */
		TestTrue(
			*FString::Printf(TEXT("OVER [RED]: hang joint utilisation %.6g == W/cap %.6g"),
				UtilisationOf(R.Readout, Over::JHang), ExpUtil),
			Near(UtilisationOf(R.Readout, Over::JHang), ExpUtil, 1.0e-3 * ExpUtil));
		TestTrue(
			*FString::Printf(TEXT("OVER [RED]: hang joint utilisation %.6g == 1 + violation/cap %.6g"),
				UtilisationOf(R.Readout, Over::JHang), 1.0 + ViolationOf(R.Readout, Over::JHang) / Cap),
			Near(UtilisationOf(R.Readout, Over::JHang), 1.0 + ViolationOf(R.Readout, Over::JHang) / Cap, 1.0e-6 * ExpUtil));
		TestTrue(TEXT("OVER [RED]: the hang joint reads OVER capacity (utilisation > 1)"),
			UtilisationOf(R.Readout, Over::JHang) > 1.0);

		/* The equilibrium resultant: magnitude == W, and tension reads negative (compression positive). */
		const double TolN = 1.0e-3 * W;
		TestTrue(
			*FString::Printf(TEXT("OVER [RED]: |N| across the hang joint == W, |%.6g| == %.6g"),
				NormalOf(R.Readout, Over::JHang), W),
			Near(FMath::Abs(NormalOf(R.Readout, Over::JHang)), W, TolN));
		TestTrue(
			*FString::Printf(TEXT("OVER [RED]: the hang joint is in TENSION (N %.6g < 0, compression positive)"),
				NormalOf(R.Readout, Over::JHang)),
			NormalOf(R.Readout, Over::JHang) < 0.0);

		/* The control joint: zero violation, within capacity, N == its brick's weight. Zero ELSEWHERE. */
		TestTrue(
			*FString::Printf(TEXT("OVER: control joint within capacity, violation %.6g ~ 0"),
				ViolationOf(R.Readout, Over::JControl)),
			Near(ViolationOf(R.Readout, Over::JControl), 0.0, 1.0e-3));
		TestTrue(TEXT("OVER: control joint utilisation <= 1"),
			UtilisationOf(R.Readout, Over::JControl) <= 1.0 + 1.0e-6);
		TestTrue(
			*FString::Printf(TEXT("OVER [RED]: control joint N %.6g == its brick weight %.6g"),
				NormalOf(R.Readout, Over::JControl), Over::ControlNormalUu()),
			Near(NormalOf(R.Readout, Over::JControl), Over::ControlNormalUu(),
				1.0e-3 * Over::ControlNormalUu()));

		/* Positive violation EXACTLY on the over joint. */
		const TSet<int32> OverSet = ViolatedJoints(R.Readout, 1.0);
		AddInfo(FString::Printf(TEXT("OVER: violated joint set size %d (expect {%d})"), OverSet.Num(), (int32)Over::JHang));
		TestTrue(TEXT("OVER [RED]: exactly the hang joint is over capacity"),
			OverSet.Num() == 1 && OverSet.Contains((int32)Over::JHang));

		/* The 0 -> 1 -> >1 mapping holds jointwise: violation > 0 iff utilisation > 1. */
		for (int32 J = 0; J < R.Readout.Joints.Num(); ++J)
		{
			const bool bViolated = ViolationOf(R.Readout, J) > 1.0;
			const bool bOverUnity = UtilisationOf(R.Readout, J) > 1.0 + 1.0e-9;
			TestEqual(
				*FString::Printf(TEXT("OVER [RED]: joint %d — violation>0 (%d) iff utilisation>1 (%d)"),
					J, bViolated ? 1 : 0, bOverUnity ? 1 : 0),
				bViolated, bOverUnity);
		}
	}

	return true;
}

/* ================================================================================================
 * TEST 2 — PERMUTATION DETERMINISM ON TWO INDEPENDENT INDETERMINATE GROUPS. THE CANONICALIZATION
 * GATE (THE tau-ANALOG), NOW GENERAL ENOUGH TO BITE THE SINGLE-GLOBAL-t BUG.
 *
 * A FAILURE HERE MEANS THE MIN-VIOLATION OBJECTIVE IS NOT CANONICAL — A DESIGN GATE, NOT A TUNING
 * NIT. §3.5 warns a weighted objective picks *a* sparse violation set rather than *the* set, so an
 * ill-posed objective names a permutation-unstable AND physically-wrong distribution.
 *
 * WHY THE OLD SINGLE-GROUP PIERS FIXTURE WAS NOT ENOUGH. A lone three-pier group is pivot-stable
 * under the current single global minimax t: t minimised drives that one group to its even spread
 * regardless of column order, so the old gate passed even with the canonicalization removed. It
 * could not distinguish a canonicalized objective from a non-canonicalized one — the very thing it
 * exists to test. This fixture puts TWO mechanically-independent indeterminate groups (§ FIXTURE
 * TWOGROUPS) at DIFFERENT even-stress levels in ONE problem. Group A (even 63333.33 per joint) sets
 * the single global t; group B (even 25333.33 per joint) then sits BELOW t, so the objective is
 * indifferent to B's split and B lands on a permutation-dependent Bland vertex whose centre joint
 * piles toward A's level (~63333) instead of B's own even ~25333. Group A alone still exercises the
 * single-group case, so this subsumes the old PIERS gate rather than dropping it.
 *
 * ASSERTS:
 *   - the base readout names a NON-EMPTY violated set (guards vacuous determinism — two empty sets
 *     compare equal);
 *   - each group's total violation matches the hand statics (W - 3*cap), so both are genuinely
 *     overloaded;
 *   - (ii) PHYSICAL CORRECTNESS: each group's per-joint violation, utilisation and normal EQUAL its
 *     OWN centroid oracle (the even spread W/3 - cap, (W/3)/cap, W/3) — NOT merely self-consistent.
 *     Group B is where the single-global-t impl fails: its centre joint is non-even;
 *   - (i) PERMUTATION STABILITY: over >= 8 seeded block+joint permutations, every joint's violation
 *     AND utilisation, mapped back through the permutation inverse, are IDENTICAL to the base
 *     (drift <= 1e-6 * scale). Group B's Bland split flips under permutation, so this drifts.
 *
 * RED AGAINST THE CURRENT SINGLE-t IMPLEMENTATION: group B reads non-even (centre joint ~ up to
 * 63333, sides low) so the group-B (ii) equalities fail, and that split flips with column order so
 * the (i) drift assertions fail. Group A's arms pass — the failure is localised to the second,
 * lower group, which is the exact signature of the bug.
 *
 * IT ALSO REDS UNDER MinimaxWeight -> 0 (bare min Sum s_k, no canonicalization at all): with no t
 * term even GROUP A becomes a free family and lands on an uneven Bland vertex (measured
 * {57000,76000,57000}) that wobbles under permutation — so BOTH groups fail. That the gate reds for
 * BOTH the single-global-t objective and the bare-sum objective, and greens ONLY for a per-group
 * canonicalization, is what the old PIERS-only gate could not do.
 *
 * GREENED BY dev-expert's per-group canonicalization (lexicographic minimax / L2 — a t per
 * independent overload group, or an equivalent even-out that is a function of the physics not the
 * column indices). Seeded, deterministic; the failing seed is printed for reproduction.
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMinViolationPermutationDeterminismTest,
	"DestructionGame.Oracle.RigidBlock.Readout.IsPermutationDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMinViolationPermutationDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace MinViolationReadoutSupport;

	const FOracleProblem Base = TwoGroups::Build();
	const FOracleResult BaseR = SolveRigidBlock(Base);

	TestTrue(TEXT("TWOGROUPS: the oracle answers"), BaseR.bAnswered);
	TestTrue(TEXT("TWOGROUPS [RED]: the min-violation readout is present"), BaseR.Readout.bPresent);
	TestEqual(TEXT("TWOGROUPS [RED]: one readout entry per joint"),
		BaseR.Readout.Joints.Num(), Base.Joints.Num());

	/* The scale for permutation-drift tolerances: the larger group's total over-stress. */
	const double DriftScale = FMath::Max(1.0, TwoGroups::TotalViolationAUu());

	/* Dump the base per-joint readout so a failure shows WHICH group and joint drifted. */
	for (int32 J = 0; J < BaseR.Readout.Joints.Num(); ++J)
	{
		AddInfo(FString::Printf(
			TEXT("TWOGROUPS base joint %d (%s): N %.6g, violation %.6g, utilisation %.6g"),
			J, TwoGroups::IsGroupA(J) ? TEXT("A") : TEXT("B"),
			NormalOf(BaseR.Readout, J), ViolationOf(BaseR.Readout, J), UtilisationOf(BaseR.Readout, J)));
	}

	/* ---- Both groups must be genuinely overloaded, and each total must match the hand statics. ---- */
	TestTrue(TEXT("TWOGROUPS: group A cap outweighs its three joints' combined crushing capacity"),
		TwoGroups::WeightAUu() > 3.0 * TwoGroups::CapJointAUu());
	TestTrue(TEXT("TWOGROUPS: group B cap outweighs its three joints' combined crushing capacity"),
		TwoGroups::WeightBUu() > 3.0 * TwoGroups::CapJointBUu());

	/* Group A's even level must strictly exceed group B's — that gap is what the single global t misses. */
	TestTrue(TEXT("TWOGROUPS: group A's even over-stress level strictly exceeds group B's"),
		TwoGroups::EvenViolationAUu() > TwoGroups::EvenViolationBUu() + 1.0);

	double BaseTotalA = 0.0;
	double BaseTotalB = 0.0;
	for (int32 J = 0; J < BaseR.Readout.Joints.Num(); ++J)
	{
		(TwoGroups::IsGroupA(J) ? BaseTotalA : BaseTotalB) += ViolationOf(BaseR.Readout, J);
	}

	TestTrue(
		*FString::Printf(TEXT("TWOGROUPS [RED]: group A total violation %.6g == hand W_A - 3*cap %.6g"),
			BaseTotalA, TwoGroups::TotalViolationAUu()),
		Near(BaseTotalA, TwoGroups::TotalViolationAUu(), 1.0e-3 * TwoGroups::TotalViolationAUu()));
	TestTrue(
		*FString::Printf(TEXT("TWOGROUPS [RED]: group B total violation %.6g == hand W_B - 3*cap %.6g"),
			BaseTotalB, TwoGroups::TotalViolationBUu()),
		Near(BaseTotalB, TwoGroups::TotalViolationBUu(), 1.0e-3 * TwoGroups::TotalViolationBUu()));

	/* GUARD AGAINST VACUITY: two empty sets compare equal. All six joints must read over. */
	const TSet<int32> BaseViolated = ViolatedJoints(BaseR.Readout, 1.0);
	TestTrue(
		*FString::Printf(TEXT("TWOGROUPS [RED]: the base readout names a NON-EMPTY violated set (%d)"),
			BaseViolated.Num()),
		BaseViolated.Num() >= 1);

	/*
	 * (ii) PHYSICAL CORRECTNESS — each group's per-joint readout EQUALS its OWN centroid oracle.
	 * This pins the even spread, not just any stable point. Group B's joints are where the current
	 * single-global-t implementation reds: its centre joint is non-even.
	 */
	for (int32 J = 0; J < BaseR.Readout.Joints.Num(); ++J)
	{
		const bool bA = TwoGroups::IsGroupA(J);
		const TCHAR* Tag = bA ? TEXT("A") : TEXT("B");
		const double ExpViolation = bA ? TwoGroups::EvenViolationAUu() : TwoGroups::EvenViolationBUu();
		const double ExpUtil = bA ? TwoGroups::EvenUtilA() : TwoGroups::EvenUtilB();
		const double ExpNormal = bA ? TwoGroups::EvenNormalAUu() : TwoGroups::EvenNormalBUu();

		TestTrue(
			*FString::Printf(TEXT("TWOGROUPS [RED]: joint %d (%s) violation %.6g == even W/3-cap %.6g"),
				J, Tag, ViolationOf(BaseR.Readout, J), ExpViolation),
			Near(ViolationOf(BaseR.Readout, J), ExpViolation, 1.0e-3 * FMath::Max(1.0, ExpViolation)));
		TestTrue(
			*FString::Printf(TEXT("TWOGROUPS [RED]: joint %d (%s) utilisation %.6g == even (W/3)/cap %.6g"),
				J, Tag, UtilisationOf(BaseR.Readout, J), ExpUtil),
			Near(UtilisationOf(BaseR.Readout, J), ExpUtil, 1.0e-3 * ExpUtil));
		TestTrue(
			*FString::Printf(TEXT("TWOGROUPS [RED]: joint %d (%s) normal %.6g == even W/3 %.6g"),
				J, Tag, NormalOf(BaseR.Readout, J), ExpNormal),
			Near(NormalOf(BaseR.Readout, J), ExpNormal, 1.0e-3 * ExpNormal));
	}

	/*
	 * (i) PERMUTATION STABILITY — over >= 8 seeded block+joint permutations, every joint's violation
	 * and utilisation, mapped back through the permutation inverse, must be identical to the base.
	 * Group B's Bland split flips under permutation, so this drifts under the single-global-t impl.
	 */
	const int32 BaseSeed = 0x06A17E51;
	const int32 NumPermutations = 8;

	for (int32 Perm = 0; Perm < NumPermutations; ++Perm)
	{
		const int32 Seed = BaseSeed + Perm;
		FRandomStream Rng(Seed);

		const TArray<int32> BlockPerm = SeededPermutation(Rng, Base.Blocks.Num());
		const TArray<int32> JointPerm = SeededPermutation(Rng, Base.Joints.Num());

		const FOracleProblem PermProblem = Permute(Base, BlockPerm, JointPerm);
		const FOracleResult PermR = SolveRigidBlock(PermProblem);

		/* Translate the permuted violated set back to base joint indices. */
		TSet<int32> PermViolatedInBase;
		for (int32 Old = 0; Old < Base.Joints.Num(); ++Old)
		{
			if (ViolationOf(PermR.Readout, JointPerm[Old]) > 1.0)
			{
				PermViolatedInBase.Add(Old);
			}
		}

		const bool bSetSame =
			PermViolatedInBase.Num() == BaseViolated.Num() && PermViolatedInBase.Includes(BaseViolated);
		TestTrue(
			*FString::Printf(
				TEXT("TWOGROUPS seed=%d [GATE]: the violated set is identical under permutation (base %d, permuted %d)"),
				Seed, BaseViolated.Num(), PermViolatedInBase.Num()),
			bSetSame);

		/* Each joint's utilisation and violation must be bit-stable across the permutation. */
		double WorstUtilDrift = 0.0;
		double WorstViolationDrift = 0.0;
		int32 WorstJoint = INDEX_NONE;
		for (int32 Old = 0; Old < Base.Joints.Num(); ++Old)
		{
			const int32 New = JointPerm[Old];
			const double UtilDrift =
				FMath::Abs(UtilisationOf(BaseR.Readout, Old) - UtilisationOf(PermR.Readout, New));
			const double ViolationDrift =
				FMath::Abs(ViolationOf(BaseR.Readout, Old) - ViolationOf(PermR.Readout, New));

			if (ViolationDrift > WorstViolationDrift)
			{
				WorstJoint = Old;
			}
			WorstUtilDrift = FMath::Max(WorstUtilDrift, UtilDrift);
			WorstViolationDrift = FMath::Max(WorstViolationDrift, ViolationDrift);
		}

		TestTrue(
			*FString::Printf(
				TEXT("TWOGROUPS seed=%d [GATE]: per-joint utilisation is permutation-stable (drift %.3e)"),
				Seed, WorstUtilDrift),
			WorstUtilDrift <= 1.0e-6 * DriftScale);
		TestTrue(
			*FString::Printf(
				TEXT("TWOGROUPS seed=%d [GATE]: per-joint violation is permutation-stable (drift %.3e, worst base joint %d)"),
				Seed, WorstViolationDrift, WorstJoint),
			WorstViolationDrift <= 1.0e-6 * DriftScale);
	}

	return true;
}

/* ================================================================================================
 * TEST 3 — VERIFICATION FAIL-CLOSED: THE PRIMAL IS A GENUINE EQUILIBRIUM.
 *
 * The min-violation LP relaxes STRENGTH but keeps EQUILIBRIUM hard, so its primal must balance
 * every block's dead load to solver tolerance EVEN WHERE the joint is over capacity — that is the
 * whole point: a violation is a strength over-stress, not a broken force balance. Asserted through
 * the resultant:
 *   - Fixture STAND: each joint's N equals the weight it carries (sum Fz = 0 per block).
 *   - Fixture OVER: the hang joint carries the FULL hang weight (|N| == W) despite being over
 *     capacity — equilibrium holds and strength is what gave — and the control joint carries its
 *     brick's weight.
 *   - Both readouts are present (bPresent) — a min-violation solve is feasible by construction, so
 *     an absent readout is a failure to solve, not a standing structure.
 *
 * SPECIFIED, not driven here (needs a solver mutation to reach): if the hard equality rows cannot
 * be satisfied — which must never happen, the formulation is always feasible — the solve must
 * REFUSE (bAnswered = false, an EOracleRefusal set) rather than hand back a readout whose forces do
 * not balance. dev-expert wires that fail-closed guard at the min-violation arm, mirroring the
 * primal admissibility gate (§3.6). Asserted here only in the weaker direction: an answered
 * min-violation solve's resultant DOES balance.
 *
 * RED BECAUSE: the empty stub reports no readout and zero forces, so "present" and every
 * "N balances the weight" assertion fails on missing behaviour.
 *
 * MUTATION THAT PROVES IT BITES once dev implements the formulation: relax an EQUALITY row too (add
 * a slack to equilibrium, not just strength) — the resultant no longer balances the weight and the
 * N assertions go red; or make the min-violation arm hand back a readout on a solve it could not
 * verify — the present/refuse contract is violated.
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMinViolationEquilibriumFailClosedTest,
	"DestructionGame.Oracle.RigidBlock.Readout.PrimalIsAGenuineEquilibrium",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMinViolationEquilibriumFailClosedTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace MinViolationReadoutSupport;

	/* ---------------- STAND: every joint's N is the load path (per-block sum Fz = 0). ---------------- */
	{
		const FOracleProblem P = Stand::Build();
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(TEXT("EQUIL STAND: answered %d, present %d, joints %d"),
			R.bAnswered ? 1 : 0, R.Readout.bPresent ? 1 : 0, R.Readout.Joints.Num()));

		TestTrue(TEXT("EQUIL STAND: the oracle answers"), R.bAnswered);
		TestTrue(TEXT("EQUIL STAND [RED]: the readout is present"), R.Readout.bPresent);

		const double TolN = 1.0e-3 * Stand::ExpectedNormalLowerUu();
		TestTrue(
			*FString::Printf(TEXT("EQUIL STAND [RED]: bottom block balances — N %.6g == 2 bricks %.6g"),
				NormalOf(R.Readout, Stand::JLower), Stand::ExpectedNormalLowerUu()),
			Near(NormalOf(R.Readout, Stand::JLower), Stand::ExpectedNormalLowerUu(), TolN));
		TestTrue(
			*FString::Printf(TEXT("EQUIL STAND [RED]: top block balances — N %.6g == 1 brick %.6g"),
				NormalOf(R.Readout, Stand::JUpper), Stand::ExpectedNormalUpperUu()),
			Near(NormalOf(R.Readout, Stand::JUpper), Stand::ExpectedNormalUpperUu(), TolN));
	}

	/* ---------------- OVER: equilibrium holds THROUGH the over-capacity joint. ---------------- */
	{
		const FOracleProblem P = Over::Build();
		const FOracleResult R = SolveRigidBlock(P);

		const double W = Over::HangWeightUu();

		AddInfo(FString::Printf(
			TEXT("EQUIL OVER: answered %d, present %d; hang |N| must == W %.6g despite over capacity"),
			R.bAnswered ? 1 : 0, R.Readout.bPresent ? 1 : 0, W));

		TestTrue(TEXT("EQUIL OVER: the oracle answers"), R.bAnswered);
		TestTrue(TEXT("EQUIL OVER [RED]: the readout is present"), R.Readout.bPresent);

		TestTrue(
			*FString::Printf(
				TEXT("EQUIL OVER [RED]: the hang block balances — |N| %.6g == W %.6g even over capacity"),
				FMath::Abs(NormalOf(R.Readout, Over::JHang)), W),
			Near(FMath::Abs(NormalOf(R.Readout, Over::JHang)), W, 1.0e-3 * W));
		TestTrue(
			*FString::Printf(TEXT("EQUIL OVER [RED]: the control block balances — N %.6g == %.6g"),
				NormalOf(R.Readout, Over::JControl), Over::ControlNormalUu()),
			Near(NormalOf(R.Readout, Over::JControl), Over::ControlNormalUu(),
				1.0e-3 * Over::ControlNormalUu()));

		/* An answered min-violation solve must NOT be sitting on a refusal. */
		TestEqual(TEXT("EQUIL OVER: an answered readout carries no refusal"),
			static_cast<int32>(R.Refusal), static_cast<int32>(EOracleRefusal::None));
	}

	return true;
}

/* ================================================================================================
 * TEST 4 — THE ≥2-DOF INTRA-GROUP DEGENERATE TRADE. THE 6a RESIDUAL PROBE (HARDENING/RED PROBE).
 *
 * This is the corner the 6a re-review flagged but could not realize: within ONE independent overload
 * group at ONE stress level, a FORCED-CRITICAL central member beside a REDUCIBLE PAIR that trades
 * within a fixed subtotal, MIXED-CAPACITY so the reduction's min-sum optimal FACE is NON-SINGLETON.
 * The false-critical hazard: the min-sum reduction, being INDIFFERENT to the flank split, lands on a
 * VERTEX where a flank slack hits t* and gets FALSELY declared critical (pinned at t* = 104000
 * instead of the even 58000), which would be both permutation-dependent AND physically wrong.
 *
 * ASSERTS, over >= 8 seeded block+joint permutations (the same harness the TwoGroups gate uses):
 *   (a) PERMUTATION STABILITY — every joint's ViolationUu and Utilisation, mapped back through the
 *       permutation inverse, are IDENTICAL to the base (drift <= 1e-6 * scale). If the min-sum face
 *       vertex flips with column order, this drifts — the determinism bug the reviewer suspected.
 *   (b) THE CANONICAL EVEN VALUE — each flank reads slack const/2 = 58000 (equal slacks / L2-minimal
 *       split of the traded subtotal), i.e. reactions 88000 (low cap) and 108000 (high cap), NOT a
 *       false-critical vertex (104000 / 12000). The central member reads its forced t* = 104000.
 *
 * TWO OUTCOMES, BOTH VALUABLE:
 *   RED — the split wobbles under permutation ((a) fails) or piles to a vertex ((b) fails): the
 *         determinism bug is real. The per-joint numbers are printed every seed so the failing case
 *         is fully characterized; DO NOT fix it here — hand it back red.
 *   GREEN — every seed evens correctly and is stable: the corner is safe BY DEMONSTRATION and this
 *         stands as a hardening test that closes the 6a residual.
 *
 * Seeded and fully deterministic; each seed is printed so a failing case can be reproduced.
 *
 * NEEDS A TICKING WORLD: NO — a pure hand-built FOracleProblem fed to SolveRigidBlock.
 * ================================================================================================ */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMinViolationDegenerateTradeTest,
	"DestructionGame.Oracle.RigidBlock.Readout.DegenerateIntraGroupTradeIsPermutationStable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMinViolationDegenerateTradeTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace MinViolationReadoutSupport;

	const FOracleProblem Base = Degen::Build();
	const FOracleResult BaseR = SolveRigidBlock(Base);

	TestTrue(TEXT("DEGEN: the oracle answers"), BaseR.bAnswered);
	TestTrue(TEXT("DEGEN: the min-violation readout is present"), BaseR.Readout.bPresent);
	TestEqual(TEXT("DEGEN: one readout entry per joint"), BaseR.Readout.Joints.Num(), Base.Joints.Num());

	if (!BaseR.Readout.bPresent || BaseR.Readout.Joints.Num() != Base.Joints.Num())
	{
		AddError(TEXT("DEGEN: readout absent or wrong arity — cannot probe the trade"));
		return false;
	}

	/* Dump the base readout so a failure shows exactly how the flank pair split. */
	for (int32 J = 0; J < BaseR.Readout.Joints.Num(); ++J)
	{
		AddInfo(FString::Printf(
			TEXT("DEGEN base joint %d: N %.6g, violation %.6g, utilisation %.6g"),
			J, NormalOf(BaseR.Readout, J), ViolationOf(BaseR.Readout, J), UtilisationOf(BaseR.Readout, J)));
	}

	/* ---- The regime that makes the corner bite, asserted so the fixture cannot silently drift. ---- */
	const double TStar = Degen::CentralViolationUu();
	const double Const = Degen::PairTotalSlackUu();
	AddInfo(FString::Printf(
		TEXT("DEGEN regime: t* (central forced) %.6g, pair const %.6g, even flank slack %.6g; "
			 "expect flank N low %.6g / high %.6g"),
		TStar, Const, Degen::EvenFlankSlackUu(), Degen::FlankLowNormalUu(), Degen::FlankHighNormalUu()));

	TestTrue(TEXT("DEGEN: the flank capacities are ASYMMETRIC (the non-singleton-face lever)"),
		Degen::FcFlankLow != Degen::FcFlankHigh);
	TestTrue(TEXT("DEGEN: a flank CAN reach t* (const >= t*), so a false-critical vertex EXISTS"),
		Const >= TStar);
	TestTrue(TEXT("DEGEN: the EVEN flank slack sits below t* (const <= 2 t*), so it is genuinely reducible"),
		Const <= 2.0 * TStar);

	const double DriftScale = FMath::Max(1.0, Degen::CentralNormalUu());

	/*
	 * (b) THE CANONICAL EVEN VALUE — base readout equals the hand statics: central forced at t*, each
	 * flank evened to const/2 (equal slacks, UNEQUAL reactions). A false-critical would read a flank
	 * at ~104000 and its partner at ~12000, failing these.
	 */
	{
		const double TolN = 1.0e-3 * Degen::CentralNormalUu();

		TestTrue(
			*FString::Printf(TEXT("DEGEN [PROBE b]: central N %.6g == forced R_C %.6g"),
				NormalOf(BaseR.Readout, Degen::JCentral), Degen::CentralNormalUu()),
			Near(NormalOf(BaseR.Readout, Degen::JCentral), Degen::CentralNormalUu(), TolN));
		TestTrue(
			*FString::Printf(TEXT("DEGEN [PROBE b]: central violation %.6g == forced t* %.6g"),
				ViolationOf(BaseR.Readout, Degen::JCentral), Degen::CentralViolationUu()),
			Near(ViolationOf(BaseR.Readout, Degen::JCentral), Degen::CentralViolationUu(),
				1.0e-3 * Degen::CentralViolationUu()));

		TestTrue(
			*FString::Printf(TEXT("DEGEN [PROBE b]: low-cap flank N %.6g == even split %.6g (NOT a vertex)"),
				NormalOf(BaseR.Readout, Degen::JFlankLow), Degen::FlankLowNormalUu()),
			Near(NormalOf(BaseR.Readout, Degen::JFlankLow), Degen::FlankLowNormalUu(), TolN));
		TestTrue(
			*FString::Printf(TEXT("DEGEN [PROBE b]: high-cap flank N %.6g == even split %.6g (NOT a vertex)"),
				NormalOf(BaseR.Readout, Degen::JFlankHigh), Degen::FlankHighNormalUu()),
			Near(NormalOf(BaseR.Readout, Degen::JFlankHigh), Degen::FlankHighNormalUu(), TolN));

		TestTrue(
			*FString::Printf(TEXT("DEGEN [PROBE b]: low-cap flank violation %.6g == even const/2 %.6g"),
				ViolationOf(BaseR.Readout, Degen::JFlankLow), Degen::EvenFlankSlackUu()),
			Near(ViolationOf(BaseR.Readout, Degen::JFlankLow), Degen::EvenFlankSlackUu(),
				1.0e-3 * Degen::EvenFlankSlackUu()));
		TestTrue(
			*FString::Printf(TEXT("DEGEN [PROBE b]: high-cap flank violation %.6g == even const/2 %.6g"),
				ViolationOf(BaseR.Readout, Degen::JFlankHigh), Degen::EvenFlankSlackUu()),
			Near(ViolationOf(BaseR.Readout, Degen::JFlankHigh), Degen::EvenFlankSlackUu(),
				1.0e-3 * Degen::EvenFlankSlackUu()));

		TestTrue(
			*FString::Printf(TEXT("DEGEN [PROBE b]: low-cap flank utilisation %.6g == even %.6g"),
				UtilisationOf(BaseR.Readout, Degen::JFlankLow), Degen::FlankLowUtil()),
			Near(UtilisationOf(BaseR.Readout, Degen::JFlankLow), Degen::FlankLowUtil(),
				1.0e-3 * Degen::FlankLowUtil()));
		TestTrue(
			*FString::Printf(TEXT("DEGEN [PROBE b]: high-cap flank utilisation %.6g == even %.6g"),
				UtilisationOf(BaseR.Readout, Degen::JFlankHigh), Degen::FlankHighUtil()),
			Near(UtilisationOf(BaseR.Readout, Degen::JFlankHigh), Degen::FlankHighUtil(),
				1.0e-3 * Degen::FlankHighUtil()));
	}

	/*
	 * (a) PERMUTATION STABILITY — over >= 8 seeded block+joint permutations, every joint's violation
	 * and utilisation mapped back through the inverse must be identical to the base. If the min-sum
	 * vertex flips with column order, this drifts.
	 */
	const int32 BaseSeed = 0x0DE9E2A7;
	const int32 NumPermutations = 8;

	for (int32 Perm = 0; Perm < NumPermutations; ++Perm)
	{
		const int32 Seed = BaseSeed + Perm;
		FRandomStream Rng(Seed);

		const TArray<int32> BlockPerm = SeededPermutation(Rng, Base.Blocks.Num());
		const TArray<int32> JointPerm = SeededPermutation(Rng, Base.Joints.Num());

		const FOracleProblem PermProblem = Permute(Base, BlockPerm, JointPerm);
		const FOracleResult PermR = SolveRigidBlock(PermProblem);

		TestTrue(*FString::Printf(TEXT("DEGEN seed=%d: the permuted oracle answers"), Seed), PermR.bAnswered);
		TestTrue(*FString::Printf(TEXT("DEGEN seed=%d: the permuted readout is present"), Seed),
			PermR.Readout.bPresent);

		double WorstUtilDrift = 0.0;
		double WorstViolationDrift = 0.0;
		int32 WorstJoint = INDEX_NONE;
		for (int32 Old = 0; Old < Base.Joints.Num(); ++Old)
		{
			const int32 New = JointPerm[Old];
			const double UtilDrift =
				FMath::Abs(UtilisationOf(BaseR.Readout, Old) - UtilisationOf(PermR.Readout, New));
			const double ViolationDrift =
				FMath::Abs(ViolationOf(BaseR.Readout, Old) - ViolationOf(PermR.Readout, New));

			if (ViolationDrift > WorstViolationDrift)
			{
				WorstJoint = Old;
			}
			WorstUtilDrift = FMath::Max(WorstUtilDrift, UtilDrift);
			WorstViolationDrift = FMath::Max(WorstViolationDrift, ViolationDrift);
		}

		TestTrue(
			*FString::Printf(
				TEXT("DEGEN seed=%d [PROBE a]: per-joint utilisation is permutation-stable (drift %.3e)"),
				Seed, WorstUtilDrift),
			WorstUtilDrift <= 1.0e-6 * DriftScale);
		TestTrue(
			*FString::Printf(
				TEXT("DEGEN seed=%d [PROBE a]: per-joint violation is permutation-stable (drift %.3e, worst base joint %d)"),
				Seed, WorstViolationDrift, WorstJoint),
			WorstViolationDrift <= 1.0e-6 * DriftScale);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
