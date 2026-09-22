// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Min-violation LP that sources the strain readout (PROMOTION_DESIGN §3.5). The maximise-lambda
 * primal is unusable for per-joint strain (at lambda >= 1 it finds forces where no joint exceeds
 * 1.0, §3.1). This LP fixes lambda = 1, keeps equilibrium rows hard, adds a slack s_k >= 0 to each
 * strength row, and minimises sum w_k*s_k. It is always feasible and returns a real primal plus a
 * per-joint slack: zero within capacity, positive only on over-capacity joints.
 *
 * Tests: (1) violation matches a hand oracle (standing stack reads zero; tension hang reads
 * W - f_t*Conv*A); (2) the violated set and magnitudes are identical across seeded permutations,
 * a failure meaning the penalty weights are ill-posed; (3) the primal is a genuine equilibrium.
 *
 * The oracle is hand statics, not the LP's slack. Units are derived here (1 MPa over 1 cm2 =
 * 10000 uu) so a wrong production constant disagrees. Each fixture leaves only the tested axis
 * capped (tension for the hang, crushing for the piers) so it alone can carry slack. Pure
 * FOracleProblem fixtures; no world tick. Named namespace for unity builds.
 */
namespace MinViolationReadoutSupport
{
	using namespace RigidBlockOracle;

	// Units, derived here so a wrong production constant fails rather than agrees.

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. NOT imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/** Above the oracle's UncappedStrengthMPa (1e9): a strength row this large is not assembled. */
	constexpr double UncappedHere = 1.0e12;

	double WeightUu(double MassKg) { return MassKg * GravityCmPerSecondSquared; }

	// Block and joint builders.

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

	/** A bed joint with the given unit normal and strength. */
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

	/** Every axis far above any demand here, so violation is zero. */
	FConnectionStrength StrongBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 100.0;
		S.ShearCohesionMPa = 100.0;
		S.TensileStrengthMPa = 100.0;
		S.FrictionCoefficient = 0.75;
		return S;
	}

	/** Weak finite tension, every other axis uncapped: tension is the only strength row. */
	FConnectionStrength WeakTensionOnly(double TensileStrengthMPa)
	{
		FConnectionStrength S;
		S.TensileStrengthMPa = TensileStrengthMPa;
		S.CompressiveStrengthMPa = UncappedHere;   // no crushing row
		S.ShearCohesionMPa = UncappedHere;         // no friction row
		S.FrictionCoefficient = 0.0;
		return S;
	}

	/** Dry joint with a finite crushing cap and no tension or cohesion; under zero shear only crushing carries slack. */
	FConnectionStrength CrushingOnly(double CompressiveStrengthMPa)
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = CompressiveStrengthMPa;
		S.TensileStrengthMPa = 0.0;
		S.ShearCohesionMPa = 0.0;                          // friction row is slack-free at zero shear
		S.FrictionCoefficient = 0.6;
		return S;
	}

	/*
	 * Fixture STAND: two bricks centred on a grounded seat, pure compression, within capacity.
	 * Zero violation; the bottom joint carries both bricks and the top joint one.
	 */

	namespace Stand
	{
		constexpr double BrickMassKg = 3.895; // 20 x 10.25 x 10 cm clay at 1.9 g/cc
		constexpr double AreaSqCm = 205.0;
		constexpr double HalfLenCm = 10.0;

		enum { Seat = 0, Lower = 1, Upper = 2 };
		enum { JLower = 0, JUpper = 1 };

		FOracleProblem Build()
		{
			FOracleProblem P;
			P.bGravityIsLive = false;      // load fixed at lambda = 1: gravity is the dead load
			P.bMinViolationReadout = true;

			P.Blocks.Add(GroundedBlock(0.0, 5.0));            // top at Z = 10
			P.Blocks.Add(FreeBlock(BrickMassKg, 0.0, 15.0));
			P.Blocks.Add(FreeBlock(BrickMassKg, 0.0, 25.0));

			const FConnectionStrength Bond = StrongBond();
			P.Joints.Add(Joint(Seat, Lower, 0.0, 1.0, 0.0, 10.0, HalfLenCm, AreaSqCm, Bond));
			P.Joints.Add(Joint(Lower, Upper, 0.0, 1.0, 0.0, 20.0, HalfLenCm, AreaSqCm, Bond));
			return P;
		}

		/** Compression positive. */
		double ExpectedNormalLowerUu() { return 2.0 * WeightUu(BrickMassKg); }
		double ExpectedNormalUpperUu() { return WeightUu(BrickMassKg); }
	}

	/*
	 * Fixture OVER. Joint 0: a block hangs below a grounded anchor on a weak tension-only bond.
	 * By hand: violation = W - f_t*Conv*A, utilisation = W / (f_t*Conv*A), N = -W (tension), M = 0.
	 * Joint 1: a control brick in compression far away in X, zero violation, N = +W_control.
	 */

	namespace Over
	{
		constexpr double HangMassKg = 30.0;      // W = 29400 uu
		constexpr double HangAreaSqCm = 100.0;
		constexpr double HangHalfLenCm = 5.0;
		constexpr double TensileMPa = 0.01;      // capacity = 0.01 * 10000 * 100 = 10000 uu

		constexpr double ControlMassKg = 3.895;
		constexpr double ControlAreaSqCm = 205.0;
		constexpr double ControlHalfLenCm = 10.0;

		enum { Anchor = 0, Hang = 1, Seat = 2, Rest = 3 };
		enum { JHang = 0, JControl = 1 };

		FOracleProblem Build()
		{
			FOracleProblem P;
			P.bGravityIsLive = false;
			P.bMinViolationReadout = true;

			// Anchor above the hang, so the normal (anchor -> hang) points down.
			P.Blocks.Add(GroundedBlock(0.0, 40.0));
			P.Blocks.Add(FreeBlock(HangMassKg, 0.0, 25.0));

			P.Joints.Add(Joint(Anchor, Hang, 0.0, -1.0, 0.0, 32.5, HangHalfLenCm, HangAreaSqCm,
				WeakTensionOnly(TensileMPa)));

			P.Blocks.Add(GroundedBlock(300.0, 5.0));
			P.Blocks.Add(FreeBlock(ControlMassKg, 300.0, 15.0));

			P.Joints.Add(Joint(Seat, Rest, 0.0, 1.0, 300.0, 10.0, ControlHalfLenCm, ControlAreaSqCm,
				StrongBond()));
			return P;
		}

		double HangWeightUu() { return WeightUu(HangMassKg); }
		double HangCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * HangAreaSqCm; }
		double HangViolationUu() { return HangWeightUu() - HangCapacityUu(); }
		double HangUtilisation() { return HangWeightUu() / HangCapacityUu(); }
		double ControlNormalUu() { return WeightUu(ControlMassKg); }
	}

	/*
	 * Fixture PIERS: a heavy cap on three collinear grounded piers, overloaded in crushing. Three
	 * reactions, two equations: the total over-stress is fixed but its distribution is a free
	 * family, so a weighted-L1 LP lands on an order-dependent vertex unless the readout is
	 * canonicalized. This is the determinism gate. Pure crushing (dry, zero shear).
	 */

	namespace Piers
	{
		constexpr double CapMassKg = 500.0;      // W = 490000 uu
		constexpr double AreaSqCm = 100.0;
		constexpr double HalfLenCm = 5.0;
		constexpr double CompressiveMPa = 0.1;   // per-joint cap = 0.1 * 10000 * 100 = 100000 uu

		FOracleProblem Build()
		{
			FOracleProblem P;
			P.bGravityIsLive = false;
			P.bMinViolationReadout = true;

			const double PierX[3] = { -100.0, 0.0, 100.0 };
			const int32 Pier0 = P.Blocks.Add(GroundedBlock(PierX[0], 10.0));
			const int32 Pier1 = P.Blocks.Add(GroundedBlock(PierX[1], 10.0));
			const int32 Pier2 = P.Blocks.Add(GroundedBlock(PierX[2], 10.0));
			const int32 Cap = P.Blocks.Add(FreeBlock(CapMassKg, 0.0, 25.0)); // centroid over pier 1

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

	/*
	 * Fixture TWOGROUPS: two independent PIERS copies far apart in X at different over-stress
	 * levels. Even spread: A reads 163333 - 100000 = 63333 per joint, B reads 25333. A single
	 * global minimax t is pinned by A, leaving B's split free, so B lands on an order-dependent
	 * vertex. Only per-group canonicalization gives each group its own even spread. The hand
	 * oracle is W/3 per joint (N = W/3, M = 0).
	 */

	namespace TwoGroups
	{
		constexpr double AreaSqCm = 100.0;
		constexpr double HalfLenCm = 5.0;
		constexpr double PierOffsetCm = 100.0;

		constexpr double CapMassA = 500.0;        // W_A = 490000 uu
		constexpr double CompressiveMPaA = 0.1;   // cap_joint_A = 100000 uu
		constexpr double GroupACentreXCm = 0.0;

		constexpr double CapMassB = 200.0;        // W_B = 196000 uu
		constexpr double CompressiveMPaB = 0.04;  // cap_joint_B = 40000 uu
		constexpr double GroupBCentreXCm = 1000.0;

		// Joints 0,1,2 are group A; 3,4,5 are group B.
		enum { JA0 = 0, JA1 = 1, JA2 = 2, JB0 = 3, JB1 = 4, JB2 = 5 };

		FOracleProblem Build()
		{
			FOracleProblem P;
			P.bGravityIsLive = false;
			P.bMinViolationReadout = true;

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

		double EvenNormalAUu() { return WeightAUu() / 3.0; }
		double EvenNormalBUu() { return WeightBUu() / 3.0; }
		double EvenViolationAUu() { return EvenNormalAUu() - CapJointAUu(); }
		double EvenViolationBUu() { return EvenNormalBUu() - CapJointBUu(); }
		double EvenUtilA() { return EvenNormalAUu() / CapJointAUu(); }
		double EvenUtilB() { return EvenNormalBUu() / CapJointBUu(); }

		// Sanity check that each group is genuinely overloaded.
		double TotalViolationAUu() { return WeightAUu() - 3.0 * CapJointAUu(); }
		double TotalViolationBUu() { return WeightBUu() - 3.0 * CapJointBUu(); }

		bool IsGroupA(int32 BaseJoint) { return BaseJoint < 3; }
	}

	/*
	 * Fixture DEGEN: a forced-critical member beside a reducible pair that trades load within a
	 * fixed subtotal. A cap bears on a central pier at x = 0 (reaction fixed, R_C = W - S) and two
	 * flanking piers both at x = 100 (identical columns, so only their sum S is fixed). Moment
	 * equilibrium gives S = 0.4 W = 196000 uu and R_C = 294000 uu.
	 *
	 * Mixed flank caps keep the pair's total slack constant along the trade, so min-sum is
	 * indifferent to the split and the simplex may land on a vertex where one flank reaches t* and
	 * is falsely pinned critical. Regime: t* = 294000 - 190000 = 104000, const = 196000 - 30000 -
	 * 50000 = 116000, t* <= const <= 2t*. The canonical answer evens the slacks at 58000 per flank,
	 * so reactions differ (88000 / 108000). No symmetric fixture exposes this.
	 */

	namespace Degen
	{
		constexpr double CapMassKg = 500.0;        // W = 490000 uu
		constexpr double AreaSqCm = 100.0;
		constexpr double HalfLenCm = 0.0;          // point contact: no local moment

		constexpr double CentralXCm = 0.0;
		constexpr double FlankXCm = 100.0;         // both flanks share this x, so their columns are identical
		constexpr double CapCentroidXCm = 40.0;    // sets S = 0.4 W

		constexpr double FcCentral = 0.19;         // cap_C  = 190000 uu
		constexpr double FcFlankLow = 0.03;        // cap_L1 =  30000 uu
		constexpr double FcFlankHigh = 0.05;       // cap_L2 =  50000 uu

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
				CrushingOnly(FcCentral)));
			P.Joints.Add(Joint(FlankA, Cap, 0.0, 1.0, FlankXCm, 20.0, HalfLenCm, AreaSqCm,
				CrushingOnly(FcFlankLow)));
			P.Joints.Add(Joint(FlankB, Cap, 0.0, 1.0, FlankXCm, 20.0, HalfLenCm, AreaSqCm,
				CrushingOnly(FcFlankHigh)));
			return P;
		}

		double CapWeightUu() { return WeightUu(CapMassKg); }

		double SubtotalUu() // S = R_L1 + R_L2
		{
			return ((CapCentroidXCm - CentralXCm) / (FlankXCm - CentralXCm)) * CapWeightUu();
		}
		double CentralNormalUu() { return CapWeightUu() - SubtotalUu(); } // R_C

		double CapCentralUu() { return FcCentral * ForceUnitsPerMPaSqCmHere * AreaSqCm; }
		double CapFlankLowUu() { return FcFlankLow * ForceUnitsPerMPaSqCmHere * AreaSqCm; }
		double CapFlankHighUu() { return FcFlankHigh * ForceUnitsPerMPaSqCmHere * AreaSqCm; }

		double CentralViolationUu() { return CentralNormalUu() - CapCentralUu(); } // t*
		double PairTotalSlackUu() { return SubtotalUu() - CapFlankLowUu() - CapFlankHighUu(); } // const
		double EvenFlankSlackUu() { return PairTotalSlackUu() / 2.0; }

		// Canonical even-slack reactions; unequal because the caps differ.
		double FlankLowNormalUu() { return CapFlankLowUu() + EvenFlankSlackUu(); }
		double FlankHighNormalUu() { return CapFlankHighUu() + EvenFlankSlackUu(); }

		double CentralUtil() { return CentralNormalUu() / CapCentralUu(); }
		double FlankLowUtil() { return FlankLowNormalUu() / CapFlankLowUu(); }
		double FlankHighUtil() { return FlankHighNormalUu() / CapFlankHighUu(); }
	}

	/*
	 * Fixture TRIDEGEN: DEGEN with three flanks (distinct caps 20000 / 25000 / 35000 uu) at x = 100
	 * beside the forced central pier. Same statics: S = 196000, R_C = 294000, t* = 104000, trio
	 * const = 116000. The canonical answer evens the slacks at const/3 = 38666.67, so reactions are
	 * 58666.67 / 63666.67 / 73666.67.
	 *
	 * Measured by mutation: reverting the inner fixed-point loop (commit 18911a7) to one reduction
	 * breaks DEGEN but not this fixture; the three-way trade resolves in one reduction. So this does
	 * not guard the fixed point. It guards the per-group level recursion's even split on a 3-member
	 * distinct-cap family: a single global t would pin the flanks at t* instead of const/3. The regime
	 * assertions only keep the fixture inside t* <= const <= 2t*.
	 */

	namespace TriDegen
	{
		constexpr double CapMassKg = 500.0;        // W = 490000 uu
		constexpr double AreaSqCm = 100.0;
		constexpr double HalfLenCm = 0.0;          // point contact: no local moment

		constexpr double CentralXCm = 0.0;
		constexpr double FlankXCm = 100.0;         // all three flanks share this x, so their columns are identical
		constexpr double CapCentroidXCm = 40.0;    // sets S = 0.4 W

		constexpr double FcCentral = 0.19;         // cap_C = 190000 uu
		constexpr double FcFlankLow = 0.02;        // cap_1 =  20000 uu
		constexpr double FcFlankMid = 0.025;       // cap_2 =  25000 uu
		constexpr double FcFlankHigh = 0.035;      // cap_3 =  35000 uu

		enum { JCentral = 0, JFlankLow = 1, JFlankMid = 2, JFlankHigh = 3 };

		FOracleProblem Build()
		{
			FOracleProblem P;
			P.bGravityIsLive = false;
			P.bMinViolationReadout = true;

			const int32 Central = P.Blocks.Add(GroundedBlock(CentralXCm, 10.0));
			const int32 FlankA = P.Blocks.Add(GroundedBlock(FlankXCm, 10.0));
			const int32 FlankB = P.Blocks.Add(GroundedBlock(FlankXCm, 10.0));
			const int32 FlankC = P.Blocks.Add(GroundedBlock(FlankXCm, 10.0));
			const int32 Cap = P.Blocks.Add(FreeBlock(CapMassKg, CapCentroidXCm, 25.0));

			P.Joints.Add(Joint(Central, Cap, 0.0, 1.0, CentralXCm, 20.0, HalfLenCm, AreaSqCm,
				CrushingOnly(FcCentral)));
			P.Joints.Add(Joint(FlankA, Cap, 0.0, 1.0, FlankXCm, 20.0, HalfLenCm, AreaSqCm,
				CrushingOnly(FcFlankLow)));
			P.Joints.Add(Joint(FlankB, Cap, 0.0, 1.0, FlankXCm, 20.0, HalfLenCm, AreaSqCm,
				CrushingOnly(FcFlankMid)));
			P.Joints.Add(Joint(FlankC, Cap, 0.0, 1.0, FlankXCm, 20.0, HalfLenCm, AreaSqCm,
				CrushingOnly(FcFlankHigh)));
			return P;
		}

		double CapWeightUu() { return WeightUu(CapMassKg); }

		double SubtotalUu() // S = R_1 + R_2 + R_3
		{
			return ((CapCentroidXCm - CentralXCm) / (FlankXCm - CentralXCm)) * CapWeightUu();
		}
		double CentralNormalUu() { return CapWeightUu() - SubtotalUu(); } // R_C

		double CapCentralUu() { return FcCentral * ForceUnitsPerMPaSqCmHere * AreaSqCm; }
		double CapFlankLowUu() { return FcFlankLow * ForceUnitsPerMPaSqCmHere * AreaSqCm; }
		double CapFlankMidUu() { return FcFlankMid * ForceUnitsPerMPaSqCmHere * AreaSqCm; }
		double CapFlankHighUu() { return FcFlankHigh * ForceUnitsPerMPaSqCmHere * AreaSqCm; }

		double CentralViolationUu() { return CentralNormalUu() - CapCentralUu(); } // t*
		double TrioTotalSlackUu() // const
		{
			return SubtotalUu() - CapFlankLowUu() - CapFlankMidUu() - CapFlankHighUu();
		}
		double EvenFlankSlackUu() { return TrioTotalSlackUu() / 3.0; }

		double FlankLowNormalUu() { return CapFlankLowUu() + EvenFlankSlackUu(); }
		double FlankMidNormalUu() { return CapFlankMidUu() + EvenFlankSlackUu(); }
		double FlankHighNormalUu() { return CapFlankHighUu() + EvenFlankSlackUu(); }

		double CentralUtil() { return CentralNormalUu() / CapCentralUu(); }
		double FlankLowUtil() { return FlankLowNormalUu() / CapFlankLowUu(); }
		double FlankMidUtil() { return FlankMidNormalUu() / CapFlankMidUu(); }
		double FlankHighUtil() { return FlankHighNormalUu() / CapFlankHighUu(); }

		// Smallest flank cap: its stranded vertex is the easiest to reach at t*.
		double MinFlankCapUu()
		{
			return FMath::Min3(CapFlankLowUu(), CapFlankMidUu(), CapFlankHighUu());
		}
	}

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

	/** Joints the readout reports over capacity. */
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

	// Reorders blocks and joints by seeded permutation, remapping block references. NewIndex = Perm[OldIndex].

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

/*
 * Test 1: violation matches hand statics. STAND reads zero violation with N equal to the weight
 * carried. OVER's hang reads violation W - f_t*Conv*A, utilisation > 1 and N = -W; its control
 * joint reads zero. Violation > 0 iff utilisation > 1, the scale UtilisationUnder uses. The
 * zero-violation arms bite if a spurious slack is charged to an in-capacity joint.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMinViolationCorrectnessTest,
	"DestructionGame.Oracle.RigidBlock.Readout.ViolationMatchesTheHandOracle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMinViolationCorrectnessTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace MinViolationReadoutSupport;

	// STAND
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

		TestTrue(
			*FString::Printf(TEXT("STAND: bottom joint within capacity, violation %.6g ~ 0"),
				ViolationOf(R.Readout, Stand::JLower)),
			Near(ViolationOf(R.Readout, Stand::JLower), 0.0, 1.0e-3));
		TestTrue(
			*FString::Printf(TEXT("STAND: top joint within capacity, violation %.6g ~ 0"),
				ViolationOf(R.Readout, Stand::JUpper)),
			Near(ViolationOf(R.Readout, Stand::JUpper), 0.0, 1.0e-3));

		// Asserting N asserts per-block equilibrium.
		const double TolN = 1.0e-3 * Stand::ExpectedNormalLowerUu();
		TestTrue(
			*FString::Printf(TEXT("STAND [RED]: bottom joint carries both bricks, N %.6g == %.6g"),
				NormalOf(R.Readout, Stand::JLower), Stand::ExpectedNormalLowerUu()),
			Near(NormalOf(R.Readout, Stand::JLower), Stand::ExpectedNormalLowerUu(), TolN));
		TestTrue(
			*FString::Printf(TEXT("STAND [RED]: top joint carries the upper brick, N %.6g == %.6g"),
				NormalOf(R.Readout, Stand::JUpper), Stand::ExpectedNormalUpperUu()),
			Near(NormalOf(R.Readout, Stand::JUpper), Stand::ExpectedNormalUpperUu(), TolN));

		TestTrue(TEXT("STAND: bottom joint utilisation <= 1"), UtilisationOf(R.Readout, Stand::JLower) <= 1.0 + 1.0e-6);
		TestTrue(TEXT("STAND: top joint utilisation <= 1"), UtilisationOf(R.Readout, Stand::JUpper) <= 1.0 + 1.0e-6);
	}

	// OVER
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

		const double TolViolation = 1.0e-3 * FMath::Max(1.0, ExpViolation);
		TestTrue(
			*FString::Printf(TEXT("OVER [RED]: hang joint violation %.6g == hand W - cap %.6g"),
				ViolationOf(R.Readout, Over::JHang), ExpViolation),
			Near(ViolationOf(R.Readout, Over::JHang), ExpViolation, TolViolation));

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

		// Compression positive, so tension reads negative.
		const double TolN = 1.0e-3 * W;
		TestTrue(
			*FString::Printf(TEXT("OVER [RED]: |N| across the hang joint == W, |%.6g| == %.6g"),
				NormalOf(R.Readout, Over::JHang), W),
			Near(FMath::Abs(NormalOf(R.Readout, Over::JHang)), W, TolN));
		TestTrue(
			*FString::Printf(TEXT("OVER [RED]: the hang joint is in TENSION (N %.6g < 0, compression positive)"),
				NormalOf(R.Readout, Over::JHang)),
			NormalOf(R.Readout, Over::JHang) < 0.0);

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

		const TSet<int32> OverSet = ViolatedJoints(R.Readout, 1.0);
		AddInfo(FString::Printf(TEXT("OVER: violated joint set size %d (expect {%d})"), OverSet.Num(), (int32)Over::JHang));
		TestTrue(TEXT("OVER [RED]: exactly the hang joint is over capacity"),
			OverSet.Num() == 1 && OverSet.Contains((int32)Over::JHang));

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

/*
 * Test 2: canonicalization gate on TWOGROUPS. A failure means the objective is not canonical, a
 * design problem rather than tuning (§3.5). A lone PIERS group is even under a single global t
 * regardless, so it could not tell canonicalized from not; two groups at different levels can.
 *
 * Asserts a non-empty violated set (two empty sets compare equal), each group's total matches
 * W - 3*cap, each joint equals its own group's even spread, and every joint is identical across
 * >= 8 seeded permutations. A single global t fails group B only; bare min-sum (MinimaxWeight -> 0)
 * fails both groups (A measured {57000,76000,57000}). Only per-group canonicalization passes.
 */
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

	const double DriftScale = FMath::Max(1.0, TwoGroups::TotalViolationAUu());

	for (int32 J = 0; J < BaseR.Readout.Joints.Num(); ++J)
	{
		AddInfo(FString::Printf(
			TEXT("TWOGROUPS base joint %d (%s): N %.6g, violation %.6g, utilisation %.6g"),
			J, TwoGroups::IsGroupA(J) ? TEXT("A") : TEXT("B"),
			NormalOf(BaseR.Readout, J), ViolationOf(BaseR.Readout, J), UtilisationOf(BaseR.Readout, J)));
	}

	TestTrue(TEXT("TWOGROUPS: group A cap outweighs its three joints' combined crushing capacity"),
		TwoGroups::WeightAUu() > 3.0 * TwoGroups::CapJointAUu());
	TestTrue(TEXT("TWOGROUPS: group B cap outweighs its three joints' combined crushing capacity"),
		TwoGroups::WeightBUu() > 3.0 * TwoGroups::CapJointBUu());

	// The gap between the groups' levels is what a single global t misses.
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

	// Guards vacuous determinism: two empty sets compare equal.
	const TSet<int32> BaseViolated = ViolatedJoints(BaseR.Readout, 1.0);
	TestTrue(
		*FString::Printf(TEXT("TWOGROUPS [RED]: the base readout names a NON-EMPTY violated set (%d)"),
			BaseViolated.Num()),
		BaseViolated.Num() >= 1);

	// Each joint equals its own group's even spread, not just any stable point.
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

	// Every joint, mapped back through the inverse, must match the base under each permutation.
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

/*
 * Test 3: the primal is a genuine equilibrium. Strength is relaxed but equilibrium is hard, so N
 * balances each block's weight even through an over-capacity joint (OVER's hang reads |N| = W).
 * The readout must be present, since the LP is feasible by construction. If equilibrium could not
 * be met the solve must refuse (§3.6); only the weaker direction is asserted here. Bites if a
 * slack is added to an equality row.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMinViolationEquilibriumFailClosedTest,
	"DestructionGame.Oracle.RigidBlock.Readout.PrimalIsAGenuineEquilibrium",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMinViolationEquilibriumFailClosedTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace MinViolationReadoutSupport;

	// STAND
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

	// OVER: equilibrium holds through the over-capacity joint.
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

		TestEqual(TEXT("EQUIL OVER: an answered readout carries no refusal"),
			static_cast<int32>(R.Refusal), static_cast<int32>(EOracleRefusal::None));
	}

	return true;
}

/*
 * Test 4: DEGEN's degenerate trade. Min-sum is indifferent to the flank split and may pin a flank
 * falsely critical at t* = 104000 instead of the even 58000. Asserts (a) every joint is identical
 * across >= 8 seeded permutations and (b) each flank reads slack const/2 = 58000 (reactions 88000
 * and 108000) with the central at t*. Seeds are printed for reproduction.
 */
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

	for (int32 J = 0; J < BaseR.Readout.Joints.Num(); ++J)
	{
		AddInfo(FString::Printf(
			TEXT("DEGEN base joint %d: N %.6g, violation %.6g, utilisation %.6g"),
			J, NormalOf(BaseR.Readout, J), ViolationOf(BaseR.Readout, J), UtilisationOf(BaseR.Readout, J)));
	}

	// Assert the regime so the fixture cannot silently drift out of it.
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

	// (b) Canonical value. A false-critical vertex would read the flanks ~104000 / ~12000.
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

	// (a) Permutation stability.
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

/*
 * Test 5: TRIDEGEN's three-way trade, a regression guard on the level recursion's even split (see
 * the fixture header; it does not guard the fixed-point loop). Asserts (a) permutation stability
 * and (b) each flank reads slack const/3 = 38666.67 (reactions 58666.67 / 63666.67 / 73666.67)
 * with the central at t* = 104000.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMinViolationThreeWayTradeTest,
	"DestructionGame.Oracle.RigidBlock.Readout.ThreeWayReducibleTradeIsPermutationStable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMinViolationThreeWayTradeTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace MinViolationReadoutSupport;

	const FOracleProblem Base = TriDegen::Build();
	const FOracleResult BaseR = SolveRigidBlock(Base);

	TestTrue(TEXT("TRIDEGEN: the oracle answers"), BaseR.bAnswered);
	TestTrue(TEXT("TRIDEGEN: the min-violation readout is present"), BaseR.Readout.bPresent);
	TestEqual(TEXT("TRIDEGEN: one readout entry per joint"), BaseR.Readout.Joints.Num(), Base.Joints.Num());

	if (!BaseR.Readout.bPresent || BaseR.Readout.Joints.Num() != Base.Joints.Num())
	{
		AddError(TEXT("TRIDEGEN: readout absent or wrong arity — cannot probe the trade"));
		return false;
	}

	for (int32 J = 0; J < BaseR.Readout.Joints.Num(); ++J)
	{
		AddInfo(FString::Printf(
			TEXT("TRIDEGEN base joint %d: N %.6g, violation %.6g, utilisation %.6g"),
			J, NormalOf(BaseR.Readout, J), ViolationOf(BaseR.Readout, J), UtilisationOf(BaseR.Readout, J)));
	}

	// Assert the regime so the fixture cannot silently drift out of it.
	const double TStar = TriDegen::CentralViolationUu();
	const double Const = TriDegen::TrioTotalSlackUu();
	const double Even = TriDegen::EvenFlankSlackUu();
	AddInfo(FString::Printf(
		TEXT("TRIDEGEN regime: t* (central forced) %.6g, trio const %.6g, even flank slack %.6g; "
			 "expect flank N low %.6g / mid %.6g / high %.6g"),
		TStar, Const, Even, TriDegen::FlankLowNormalUu(), TriDegen::FlankMidNormalUu(), TriDegen::FlankHighNormalUu()));

	TestTrue(TEXT("TRIDEGEN: the three flank capacities are mutually DISTINCT (the non-singleton-face lever)"),
		TriDegen::FcFlankLow != TriDegen::FcFlankMid
			&& TriDegen::FcFlankMid != TriDegen::FcFlankHigh
			&& TriDegen::FcFlankLow != TriDegen::FcFlankHigh);

	TestTrue(
		*FString::Printf(TEXT("TRIDEGEN: central forced level t* %.6g >= flank even level %.6g"), TStar, Even),
		TStar >= Even);

	TestTrue(
		*FString::Printf(TEXT("TRIDEGEN: the even flank slack %.6g sits below t* %.6g (genuinely reducible)"), Even, TStar),
		Even < TStar);

	TestTrue(
		*FString::Printf(TEXT("TRIDEGEN: the even flank slack %.6g is positive (each flank over its cap)"), Even),
		Even > 0.0);

	// Stranding at t* is possible here but does not occur (see header); this only pins the band.
	TestTrue(
		*FString::Printf(TEXT("TRIDEGEN: a flank COULD reach t* (S - cap_min = %.6g >= t* %.6g); posed in the band"),
			TriDegen::SubtotalUu() - TriDegen::MinFlankCapUu(), TStar),
		TriDegen::SubtotalUu() - TriDegen::MinFlankCapUu() >= TStar);

	const double DriftScale = FMath::Max(1.0, TriDegen::CentralNormalUu());

	// (b) Canonical value: one even slack, three unequal reactions.
	{
		const double TolN = 1.0e-3 * TriDegen::CentralNormalUu();

		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: central N %.6g == forced R_C %.6g"),
				NormalOf(BaseR.Readout, TriDegen::JCentral), TriDegen::CentralNormalUu()),
			Near(NormalOf(BaseR.Readout, TriDegen::JCentral), TriDegen::CentralNormalUu(), TolN));
		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: central violation %.6g == forced t* %.6g"),
				ViolationOf(BaseR.Readout, TriDegen::JCentral), TriDegen::CentralViolationUu()),
			Near(ViolationOf(BaseR.Readout, TriDegen::JCentral), TriDegen::CentralViolationUu(),
				1.0e-3 * TriDegen::CentralViolationUu()));

		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: low-cap flank N %.6g == even split %.6g"),
				NormalOf(BaseR.Readout, TriDegen::JFlankLow), TriDegen::FlankLowNormalUu()),
			Near(NormalOf(BaseR.Readout, TriDegen::JFlankLow), TriDegen::FlankLowNormalUu(), TolN));
		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: mid-cap flank N %.6g == even split %.6g"),
				NormalOf(BaseR.Readout, TriDegen::JFlankMid), TriDegen::FlankMidNormalUu()),
			Near(NormalOf(BaseR.Readout, TriDegen::JFlankMid), TriDegen::FlankMidNormalUu(), TolN));
		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: high-cap flank N %.6g == even split %.6g"),
				NormalOf(BaseR.Readout, TriDegen::JFlankHigh), TriDegen::FlankHighNormalUu()),
			Near(NormalOf(BaseR.Readout, TriDegen::JFlankHigh), TriDegen::FlankHighNormalUu(), TolN));

		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: low-cap flank violation %.6g == even const/3 %.6g"),
				ViolationOf(BaseR.Readout, TriDegen::JFlankLow), Even),
			Near(ViolationOf(BaseR.Readout, TriDegen::JFlankLow), Even, 1.0e-3 * Even));
		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: mid-cap flank violation %.6g == even const/3 %.6g"),
				ViolationOf(BaseR.Readout, TriDegen::JFlankMid), Even),
			Near(ViolationOf(BaseR.Readout, TriDegen::JFlankMid), Even, 1.0e-3 * Even));
		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: high-cap flank violation %.6g == even const/3 %.6g"),
				ViolationOf(BaseR.Readout, TriDegen::JFlankHigh), Even),
			Near(ViolationOf(BaseR.Readout, TriDegen::JFlankHigh), Even, 1.0e-3 * Even));

		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: low-cap flank utilisation %.6g == even %.6g"),
				UtilisationOf(BaseR.Readout, TriDegen::JFlankLow), TriDegen::FlankLowUtil()),
			Near(UtilisationOf(BaseR.Readout, TriDegen::JFlankLow), TriDegen::FlankLowUtil(),
				1.0e-3 * TriDegen::FlankLowUtil()));
		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: mid-cap flank utilisation %.6g == even %.6g"),
				UtilisationOf(BaseR.Readout, TriDegen::JFlankMid), TriDegen::FlankMidUtil()),
			Near(UtilisationOf(BaseR.Readout, TriDegen::JFlankMid), TriDegen::FlankMidUtil(),
				1.0e-3 * TriDegen::FlankMidUtil()));
		TestTrue(
			*FString::Printf(TEXT("TRIDEGEN [PROBE b]: high-cap flank utilisation %.6g == even %.6g"),
				UtilisationOf(BaseR.Readout, TriDegen::JFlankHigh), TriDegen::FlankHighUtil()),
			Near(UtilisationOf(BaseR.Readout, TriDegen::JFlankHigh), TriDegen::FlankHighUtil(),
				1.0e-3 * TriDegen::FlankHighUtil()));
	}

	// (a) Permutation stability.
	const int32 BaseSeed = 0x3B17A9C5;
	const int32 NumPermutations = 8;

	for (int32 Perm = 0; Perm < NumPermutations; ++Perm)
	{
		const int32 Seed = BaseSeed + Perm;
		FRandomStream Rng(Seed);

		const TArray<int32> BlockPerm = SeededPermutation(Rng, Base.Blocks.Num());
		const TArray<int32> JointPerm = SeededPermutation(Rng, Base.Joints.Num());

		const FOracleProblem PermProblem = Permute(Base, BlockPerm, JointPerm);
		const FOracleResult PermR = SolveRigidBlock(PermProblem);

		TestTrue(*FString::Printf(TEXT("TRIDEGEN seed=%d: the permuted oracle answers"), Seed), PermR.bAnswered);
		TestTrue(*FString::Printf(TEXT("TRIDEGEN seed=%d: the permuted readout is present"), Seed),
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
				TEXT("TRIDEGEN seed=%d [PROBE a]: per-joint utilisation is permutation-stable (drift %.3e)"),
				Seed, WorstUtilDrift),
			WorstUtilDrift <= 1.0e-6 * DriftScale);
		TestTrue(
			*FString::Printf(
				TEXT("TRIDEGEN seed=%d [PROBE a]: per-joint violation is permutation-stable (drift %.3e, worst base joint %d)"),
				Seed, WorstViolationDrift, WorstJoint),
			WorstViolationDrift <= 1.0e-6 * DriftScale);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
