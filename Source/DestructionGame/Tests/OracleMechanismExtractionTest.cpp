// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Collapse-mechanism extraction from the oracle (PROMOTION_DESIGN.md §12 D7). Three tests:
 *
 *   1. Correctness against hand-derived rigid-body kinematics (not the LP dual): an overhanging
 *      brick, a body on two load paths, and a feasible brick whose mechanism must be empty.
 *   2. Permutation determinism (the design gate): the canonical named set must not change when
 *      block/joint order is permuted. A failure stops the slice (D7).
 *   3. Farkas fail-closed: a reported mechanism must be certified and do positive gravity work
 *      (yb > 0); a feasible problem never reports one.
 *
 * Fixtures use the feasibility pose (gravity dead, PROMOTION_DESIGN §3.2/§3.3), where phase 1's dual
 * on the infeasible arm is the Farkas certificate. Units are derived here (1 MPa over 1 cm2 =
 * 10000 uu), not imported, so a wrong production constant disagrees. No ticking world. Named
 * namespace because unity builds merge anonymous ones.
 */
namespace OracleMechanismExtractionSupport
{
	using namespace RigidBlockOracle;
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. Deliberately not imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	constexpr double ClayDensityGramsPerCubicCm = 1.9;
	constexpr double WytheWidthCm = 10.25;

	/** A grounded seat block: writes no equilibrium rows and must read a zero triple. */
	FOracleBlock GroundedSeat(double CentreXCm, double CentreZCm)
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

	/** A bed joint with an upward normal between a lower seat (A) and an upper body (B). */
	FOracleJoint BedJoint(
		int32 BlockA, int32 BlockB, double CentreXCm, double CentreZCm, double HalfLengthCm, double AreaSqCm)
	{
		FOracleJoint J;
		J.BlockA = BlockA;
		J.BlockB = BlockB;
		J.NormalX = 0.0;
		J.NormalZ = 1.0;
		J.CentreXCm = CentreXCm;
		J.CentreZCm = CentreZCm;
		J.HalfLengthCm = HalfLengthCm;
		J.AreaSqCm = AreaSqCm;
		J.Strength = GeneralPurposeMortar;
		return J;
	}

	/*
	 * Fixture A: a bar on one narrow grounded seat, centroid far to the right, so it rotates about
	 * the seat's right edge and the left contact lifts.
	 *
	 *   body centroid (X = 97.5) --------------------------------+
	 *   +--------------------------------------------------------+   one free bar
	 *   | [seat]                                                 |   overhang -->
	 *   +--##----------------------------------------------------+
	 *      earth      seat spans X in [-5, +5], fulcrum at X = +5
	 *
	 * Expected: only the body moves and its centroid descends; the joint opens; gravity does
	 * positive work. Overturning outruns the most charitable plastic bond by ~4x (worked in RunTest).
	 */

	constexpr double A_SeatHalfXCm = 5.0;           /* seat spans X in [-5, +5] */
	constexpr double A_SeatCentreXCm = 0.0;
	constexpr double A_FulcrumXCm = A_SeatCentreXCm + A_SeatHalfXCm; /* right contact = +5 */

	constexpr double A_BodyLeftXCm = -5.0;
	constexpr double A_BodyRightXCm = 200.0;
	constexpr double A_BodyCentreXCm = (A_BodyLeftXCm + A_BodyRightXCm) / 2.0; /* 97.5 */
	constexpr double A_BodyThicknessZCm = 40.0;
	constexpr double A_BodyCentreZCm = 41.0;

	constexpr double A_JointAreaSqCm = (2.0 * A_SeatHalfXCm) * WytheWidthCm; /* full seat face */
	constexpr double A_JointCentreZCm = 20.5;

	double A_BodyMassKg()
	{
		return ClayDensityGramsPerCubicCm
			* (A_BodyRightXCm - A_BodyLeftXCm) * WytheWidthCm * A_BodyThicknessZCm / 1000.0;
	}

	double A_BodyWeightUu() { return A_BodyMassKg() * GravityCmPerSecondSquared; }

	double A_OverturningMomentUuCm()
	{
		return A_BodyWeightUu() * (A_BodyCentreXCm - A_FulcrumXCm);
	}

	/** Most charitable plastic restoring moment: the left contact at f_t over its half-area. */
	double A_MaxPlasticRestoringUuCm(double BondMPa)
	{
		const double PerContactUu = BondMPa * ForceUnitsPerMPaSqCmHere * (A_JointAreaSqCm / 2.0);
		const double LeftLeverCm = A_FulcrumXCm - (A_SeatCentreXCm - A_SeatHalfXCm); /* +5 - (-5) = 10 */
		return PerContactUu * LeftLeverCm;
	}

	/** Fixture A block indices. One joint, index 0. */
	enum { A_Seat = 0, A_Body = 1 };

	FOracleProblem BuildFixtureA()
	{
		FOracleProblem P;
		P.bGravityIsLive = false; /* feasibility formulation: gravity dead, phase 1 runs */

		P.Blocks.Add(GroundedSeat(A_SeatCentreXCm, 10.0)); /* A_Seat = 0 */
		P.Blocks.Add(FreeBlock(A_BodyMassKg(), A_BodyCentreXCm, A_BodyCentreZCm)); /* A_Body = 1 */

		P.Joints.Add(BedJoint(A_Seat, A_Body, A_SeatCentreXCm, A_JointCentreZCm, A_SeatHalfXCm, A_JointAreaSqCm));
		return P;
	}

	// Fixture C: a brick centred on a wide seat. Feasible, so its mechanism must be empty.

	constexpr double C_SeatHalfXCm = 30.0;   /* wide seat, X in [-30, +30] */
	constexpr double C_BodyHalfXCm = 20.0;   /* narrower body, centred */
	constexpr double C_BodyThicknessZCm = 20.0;
	constexpr double C_JointAreaSqCm = (2.0 * C_BodyHalfXCm) * WytheWidthCm;

	double C_BodyMassKg()
	{
		return ClayDensityGramsPerCubicCm
			* (2.0 * C_BodyHalfXCm) * WytheWidthCm * C_BodyThicknessZCm / 1000.0;
	}

	FOracleProblem BuildFixtureC()
	{
		FOracleProblem P;
		P.bGravityIsLive = false;

		P.Blocks.Add(GroundedSeat(0.0, 10.0));                    /* seat = 0 */
		P.Blocks.Add(FreeBlock(C_BodyMassKg(), 0.0, 30.0));       /* body = 1, centred */

		P.Joints.Add(BedJoint(0, 1, 0.0, 20.5, C_BodyHalfXCm, C_JointAreaSqCm));
		return P;
	}

	/*
	 * Fixture B: a body on two grounded seats, both left of its centroid (TwoLoadPathOverturning's
	 * topology), built as an FStructure so the bridge and its provenance maps are exercised. The body
	 * rotates about the pivot seat's right edge and descends; both seats stay still.
	 */

	constexpr double B_SeatHeightCm = 20.0;
	constexpr double B_BedJointThicknessCm = 1.0;

	constexpr double B_AnchorCentreXCm = 0.0;
	constexpr double B_AnchorWidthXCm = 5.0;
	constexpr double B_PivotCentreXCm = 10.0;
	constexpr double B_PivotWidthXCm = 10.0;
	constexpr double B_FulcrumXCm = B_PivotCentreXCm + B_PivotWidthXCm / 2.0; /* right edge of pivot = 15 */

	constexpr double B_BodyLeftXCm = B_AnchorCentreXCm - B_AnchorWidthXCm / 2.0;
	constexpr double B_BodyLengthXCm = 300.0;
	constexpr double B_BodyCentreXCm = B_BodyLeftXCm + B_BodyLengthXCm / 2.0;
	constexpr double B_BodyThicknessZCm = 40.0;
	constexpr double B_BodyBottomZCm = B_SeatHeightCm + B_BedJointThicknessCm;
	constexpr double B_BodyCentreZCm = B_BodyBottomZCm + B_BodyThicknessZCm / 2.0;

	FPieceBox MakeBox(double CentreX, double WidthX, double CentreZ, double ThicknessZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(WidthX, WytheWidthCm, ThicknessZ) * 0.5;
		Box.CentreCm = FVector(CentreX, 0.0, CentreZ);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	struct FTwoPathBody
	{
		FStructure Structure;
		int32 Anchor = INDEX_NONE;
		int32 Pivot = INDEX_NONE;
		int32 Body = INDEX_NONE;
		int32 AnchorJoint = INDEX_NONE;
		int32 PivotJoint = INDEX_NONE;
	};

	void BuildFixtureB(FTwoPathBody& Out)
	{
		const FPieceBox AnchorBox = MakeBox(B_AnchorCentreXCm, B_AnchorWidthXCm, B_SeatHeightCm / 2.0, B_SeatHeightCm);
		const FPieceBox PivotBox = MakeBox(B_PivotCentreXCm, B_PivotWidthXCm, B_SeatHeightCm / 2.0, B_SeatHeightCm);
		const FPieceBox BodyBox = MakeBox(B_BodyCentreXCm, B_BodyLengthXCm, B_BodyCentreZCm, B_BodyThicknessZCm);

		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Pivot = Out.Structure.AddPiece(BoxMassKg(PivotBox), /*bIsGrounded*/ true, PivotBox.CentreCm);
		Out.Body = Out.Structure.AddPiece(BoxMassKg(BodyBox), /*bIsGrounded*/ false, BodyBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Anchor, AnchorBox, Out.Body, BodyBox,
				B_BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.AnchorJoint = Out.Structure.AddConnection(Joint);
		}
		if (MakeInterface(Out.Pivot, PivotBox, Out.Body, BodyBox,
				B_BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.PivotJoint = Out.Structure.AddConnection(Joint);
		}
	}

	/*
	 * Fixture D: makes the determinism gate non-vacuous. A and B have one free block each, so their
	 * moving set is trivial. D has two disconnected dry-stone stacks: a leaning one past tipping,
	 * whose free blocks all move, and a centred one that stands and must stay out of the set. The
	 * named set is then a strict subset chosen from a non-unique dual.
	 *
	 * Measured 2026-08-27: 0 Bland entries. Its degeneracy is dual-multiplier non-uniqueness, not
	 * the Bland regime; the Bland-degenerate fixture D7 needs is still owed (CURRENT_STATE).
	 */

	FOracleJoint DryBedJoint(
		int32 BlockA, int32 BlockB, double CentreXCm, double CentreZCm, double HalfLengthCm, double AreaSqCm)
	{
		FOracleJoint J;
		J.BlockA = BlockA;
		J.BlockB = BlockB;
		J.NormalX = 0.0;
		J.NormalZ = 1.0;
		J.CentreXCm = CentreXCm;
		J.CentreZCm = CentreZCm;
		J.HalfLengthCm = HalfLengthCm;
		J.AreaSqCm = AreaSqCm;
		J.Strength = DryStone;
		return J;
	}

	FOracleProblem BuildFixtureD()
	{
		FOracleProblem P;
		P.bGravityIsLive = false;

		const double BrickWidthXCm = 20.0;
		const double BrickHeightZCm = 10.0;
		const double BrickMassKg =
			ClayDensityGramsPerCubicCm * BrickWidthXCm * WytheWidthCm * BrickHeightZCm / 1000.0;

		// The falling leaning stack: grounded base, then free courses offset +X.
		const int32 NumLeaning = 24;
		const double OffsetXCm = 4.0;                 /* per course */
		const double OverlapXCm = BrickWidthXCm - OffsetXCm;
		const double LeanJointAreaSqCm = OverlapXCm * WytheWidthCm;

		const int32 LeanBase = P.Blocks.Add(GroundedSeat(0.0, BrickHeightZCm / 2.0));
		int32 Below = LeanBase;
		double BelowXCm = 0.0;

		for (int32 Course = 1; Course <= NumLeaning; ++Course)
		{
			const double CentreXCm = Course * OffsetXCm;
			const double CentreZCm = BrickHeightZCm / 2.0 + Course * BrickHeightZCm;
			const int32 Above = P.Blocks.Add(FreeBlock(BrickMassKg, CentreXCm, CentreZCm));

			P.Joints.Add(DryBedJoint(Below, Above,
				(BelowXCm + CentreXCm) / 2.0, Course * BrickHeightZCm, OverlapXCm / 2.0, LeanJointAreaSqCm));

			Below = Above;
			BelowXCm = CentreXCm;
		}

		// The standing stack, centred on its seat, far away in X.
		const double StandCentreXCm = 400.0;
		const int32 NumStanding = 3;
		const double StandJointAreaSqCm = BrickWidthXCm * WytheWidthCm;

		const int32 StandBase = P.Blocks.Add(GroundedSeat(StandCentreXCm, BrickHeightZCm / 2.0));
		int32 StandBelow = StandBase;

		for (int32 Course = 1; Course <= NumStanding; ++Course)
		{
			const double CentreZCm = BrickHeightZCm / 2.0 + Course * BrickHeightZCm;
			const int32 Above = P.Blocks.Add(FreeBlock(BrickMassKg, StandCentreXCm, CentreZCm));

			P.Joints.Add(DryBedJoint(StandBelow, Above,
				StandCentreXCm, Course * BrickHeightZCm, BrickWidthXCm / 2.0, StandJointAreaSqCm));

			StandBelow = Above;
		}

		return P;
	}

	/** Oracle-block indices the canonical mechanism says move. */
	TSet<int32> MovingBlocks(const FOracleMechanism& M)
	{
		TSet<int32> Moving;
		for (int32 B = 0; B < M.Blocks.Num(); ++B)
		{
			if (M.Blocks[B].bMoves)
			{
				Moving.Add(B);
			}
		}
		return Moving;
	}

	/** Oracle-joint indices the canonical mechanism says open or slide. */
	TSet<int32> OpeningJoints(const FOracleMechanism& M)
	{
		TSet<int32> Opening;
		for (int32 J = 0; J < M.JointOpensOrSlides.Num(); ++J)
		{
			if (M.JointOpensOrSlides[J])
			{
				Opening.Add(J);
			}
		}
		return Opening;
	}

	/** Bounds-checked: block B is flagged moving. */
	bool BlockMoves(const FOracleMechanism& M, int32 B)
	{
		return M.Blocks.IsValidIndex(B) && M.Blocks[B].bMoves;
	}

	/** Bounds-checked: joint J is flagged open or sliding. */
	bool JointOpens(const FOracleMechanism& M, int32 J)
	{
		return M.JointOpensOrSlides.IsValidIndex(J) && M.JointOpensOrSlides[J];
	}

	/**
	 * Gravity's virtual work on the mechanism, sum of -W_i * VirtualUz_i. Positive for a real
	 * collapse: the yb > 0 half of Farkas, computed independently of the LP.
	 */
	double GravityVirtualWork(const FOracleProblem& P, const FOracleMechanism& M)
	{
		double Work = 0.0;
		const int32 N = FMath::Min(P.Blocks.Num(), M.Blocks.Num());
		for (int32 B = 0; B < N; ++B)
		{
			const double WeightUu = P.Blocks[B].MassKg * GravityCmPerSecondSquared;
			Work += (-WeightUu) * M.Blocks[B].VirtualUz;
		}
		return Work;
	}

	/** The largest triple magnitude among blocks not in the given set. */
	double MaxTripleMagnitudeOutside(const FOracleMechanism& M, const TSet<int32>& Named)
	{
		double Worst = 0.0;
		for (int32 B = 0; B < M.Blocks.Num(); ++B)
		{
			if (Named.Contains(B))
			{
				continue;
			}
			const FOracleMechanismBlock& T = M.Blocks[B];
			Worst = FMath::Max(Worst,
				FMath::Abs(T.VirtualUx) + FMath::Abs(T.VirtualUz) + FMath::Abs(T.VirtualOmega));
		}
		return Worst;
	}

	// Seeded permutation, returned as OldIndex -> NewIndex.
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
		// Invert NewIndex -> OldIndex to OldIndex -> NewIndex.
		TArray<int32> Inv;
		Inv.SetNumUninitialized(N);
		for (int32 New = 0; New < N; ++New)
		{
			Inv[Perm[New]] = New;
		}
		return Inv;
	}

	/** The same problem with blocks and joints reordered and every block reference remapped. */
	FOracleProblem Permute(const FOracleProblem& In, const TArray<int32>& BlockPerm, const TArray<int32>& JointPerm)
	{
		FOracleProblem Out;
		Out.bGravityIsLive = In.bGravityIsLive;
		Out.bFirstCrackRows = In.bFirstCrackRows;

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

		for (const FOracleAppliedForce& F : In.AppliedForces)
		{
			FOracleAppliedForce G = F;
			G.Block = BlockPerm[F.Block];
			Out.AppliedForces.Add(G);
		}
		return Out;
	}
}

/*
 * Mechanism correctness against hand-derived kinematics. A: body moves and descends, seat still,
 * joint opens, positive work. C: empty mechanism. B: body moves, seats still, and the bridge fills
 * PieceOfBlock/ConnectionOfJoint. B's body is found by centroid so a wrong provenance map cannot
 * mask a wrong mechanism. Bite-prover: clearing the mechanism at the infeasible arm turns it red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMechanismCorrectnessTest,
	"DestructionGame.Oracle.RigidBlock.Mechanism.NamesTheHandDerivedCollapse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMechanismCorrectnessTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace DestructionProfiles;
	using namespace OracleMechanismExtractionSupport;

	// Fixture A: single overhang.
	TestEqual(TEXT("FIXTURE A: bonded with the mean-basis 0.70 flexural bond"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.7);

	const double A_Over = A_OverturningMomentUuCm();
	const double A_Rest = A_MaxPlasticRestoringUuCm(GeneralPurposeMortar.TensileStrengthMPa);
	const double A_Ratio = A_Over / A_Rest;

	AddInfo(FString::Printf(
		TEXT("FIXTURE A DERIVED: body weight %.6g uu, centroid X %.4g, fulcrum X %.4g; overturning "
			 "%.6g vs MOST plastic restoring %.6g uu.cm => ratio %.4g (>1 falls)"),
		A_BodyWeightUu(), A_BodyCentreXCm, A_FulcrumXCm, A_Over, A_Rest, A_Ratio));

	TestTrue(
		*FString::Printf(TEXT("FIXTURE A: past tipping by a clear margin, overturning/restoring %.4g > 2"), A_Ratio),
		A_Ratio > 2.0);

	{
		const FOracleProblem P = BuildFixtureA();
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("FIXTURE A: answered %d, lambda* %.6g, outcome %d (1=Falls), mechanism present %d, "
				 "certified %d, blocks %d, joints %d"),
			R.bAnswered ? 1 : 0, R.Lambda, static_cast<int32>(OutcomeOf(R)),
			R.Mechanism.bPresent ? 1 : 0, R.Mechanism.bIsCertified ? 1 : 0,
			R.Mechanism.Blocks.Num(), R.Mechanism.JointOpensOrSlides.Num()));

		TestTrue(TEXT("FIXTURE A: the oracle answers"), R.bAnswered);
		TestEqual(TEXT("FIXTURE A: infeasible under dead gravity (Falls)"),
			static_cast<int32>(OutcomeOf(R)), static_cast<int32>(EOracleOutcome::Falls));

		TestTrue(TEXT("FIXTURE A [RED]: an infeasible problem must carry a mechanism (bPresent)"),
			R.Mechanism.bPresent);

		TestTrue(TEXT("FIXTURE A [RED]: the BODY moves"), BlockMoves(R.Mechanism, A_Body));
		TestFalse(TEXT("FIXTURE A: the grounded SEAT does not move"), BlockMoves(R.Mechanism, A_Seat));
		TestTrue(TEXT("FIXTURE A [RED]: the single bed JOINT opens"), JointOpens(R.Mechanism, 0));

		const double BodyUz = R.Mechanism.Blocks.IsValidIndex(A_Body)
			? R.Mechanism.Blocks[A_Body].VirtualUz : 0.0;
		TestTrue(
			*FString::Printf(TEXT("FIXTURE A [RED]: the body's centroid descends, VirtualUz %.6g < 0"), BodyUz),
			BodyUz < 0.0);

		TSet<int32> BodyOnly;
		BodyOnly.Add(A_Body);
		const double Outside = MaxTripleMagnitudeOutside(R.Mechanism, BodyOnly);
		double BodyMag = 0.0;
		if (R.Mechanism.Blocks.IsValidIndex(A_Body))
		{
			const FOracleMechanismBlock& T = R.Mechanism.Blocks[A_Body];
			BodyMag = FMath::Abs(T.VirtualUx) + FMath::Abs(T.VirtualUz) + FMath::Abs(T.VirtualOmega);
		}
		TestTrue(
			*FString::Printf(
				TEXT("FIXTURE A [RED]: the grounded seat's triple (%.3e) is negligible beside the body's (%.3e)"),
				Outside, BodyMag),
			BodyMag > 0.0 && Outside <= 1.0e-6 * BodyMag);

		const double Work = GravityVirtualWork(P, R.Mechanism);
		TestTrue(
			*FString::Printf(TEXT("FIXTURE A [RED]: gravity does positive work on the mechanism, %.6g > 0"), Work),
			Work > 0.0);
	}

	// Fixture C: feasible, so an empty mechanism.
	{
		const FOracleProblem P = BuildFixtureC();
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("FIXTURE C: answered %d, lambda* %.6g, outcome %d (2=Stands), mechanism present %d, moving %d"),
			R.bAnswered ? 1 : 0, R.Lambda, static_cast<int32>(OutcomeOf(R)),
			R.Mechanism.bPresent ? 1 : 0, MovingBlocks(R.Mechanism).Num()));

		TestTrue(TEXT("FIXTURE C: the oracle answers"), R.bAnswered);
		TestEqual(TEXT("FIXTURE C: a centred brick stands (feasible)"),
			static_cast<int32>(OutcomeOf(R)), static_cast<int32>(EOracleOutcome::Stands));

		TestFalse(TEXT("FIXTURE C: a feasible problem carries NO mechanism (bPresent false)"),
			R.Mechanism.bPresent);
		TestEqual(TEXT("FIXTURE C: a feasible problem moves no block"),
			MovingBlocks(R.Mechanism).Num(), 0);
	}

	// Fixture B: two load paths, bridged.
	{
		FTwoPathBody Fx;
		BuildFixtureB(Fx);

		if (Fx.AnchorJoint == INDEX_NONE || Fx.PivotJoint == INDEX_NONE)
		{
			AddError(TEXT("FIXTURE B: the producer must emit both body-seat bed joints"));
			return false;
		}

		FOracleProblem P;
		FString Why;
		const bool bBridged = BuildRigidBlockProblem(Fx.Structure, P, Why);
		TestTrue(*FString::Printf(TEXT("FIXTURE B: the bridge accepts the structure (%s)"), *Why), bBridged);

		if (bBridged)
		{
			P.bGravityIsLive = false; /* feasibility formulation for the mechanism */

			const FOracleResult R = SolveRigidBlock(P);

			AddInfo(FString::Printf(
				TEXT("FIXTURE B: answered %d, lambda* %.6g, outcome %d (1=Falls), present %d, blocks %d, "
					 "PieceOfBlock %d, ConnectionOfJoint %d"),
				R.bAnswered ? 1 : 0, R.Lambda, static_cast<int32>(OutcomeOf(R)),
				R.Mechanism.bPresent ? 1 : 0, R.Mechanism.Blocks.Num(),
				P.PieceOfBlock.Num(), P.ConnectionOfJoint.Num()));

			TestTrue(TEXT("FIXTURE B: the oracle answers"), R.bAnswered);
			TestEqual(TEXT("FIXTURE B: infeasible under dead gravity (Falls)"),
				static_cast<int32>(OutcomeOf(R)), static_cast<int32>(EOracleOutcome::Falls));

			// Find the body by centroid, independent of block order and provenance.
			int32 BodyBlock = INDEX_NONE;
			for (int32 B = 0; B < P.Blocks.Num(); ++B)
			{
				if (!P.Blocks[B].bGrounded
					&& FMath::Abs(P.Blocks[B].CentroidXCm - B_BodyCentreXCm) < 1.0)
				{
					BodyBlock = B;
				}
			}
			TestTrue(TEXT("FIXTURE B: the overhanging body block is found by centroid"),
				BodyBlock != INDEX_NONE);

			TestTrue(TEXT("FIXTURE B [RED]: an infeasible problem must carry a mechanism"),
				R.Mechanism.bPresent);
			if (BodyBlock != INDEX_NONE)
			{
				TestTrue(TEXT("FIXTURE B [RED]: the overhanging body moves"),
					BlockMoves(R.Mechanism, BodyBlock));
			}
			for (int32 B = 0; B < P.Blocks.Num(); ++B)
			{
				if (P.Blocks[B].bGrounded)
				{
					TestFalse(
						*FString::Printf(TEXT("FIXTURE B: grounded seat block %d does not move"), B),
						BlockMoves(R.Mechanism, B));
				}
			}

			const double Work = GravityVirtualWork(P, R.Mechanism);
			TestTrue(
				*FString::Printf(TEXT("FIXTURE B [RED]: gravity does positive work on the mechanism, %.6g > 0"), Work),
				Work > 0.0);

			// Bridge provenance (§12 D7): the mechanism must be able to name real pieces and joints.
			TestEqual(TEXT("FIXTURE B [RED]: the bridge fills PieceOfBlock, one entry per block"),
				P.PieceOfBlock.Num(), P.Blocks.Num());
			TestEqual(TEXT("FIXTURE B [RED]: the bridge fills ConnectionOfJoint, one entry per joint"),
				P.ConnectionOfJoint.Num(), P.Joints.Num());

			if (P.PieceOfBlock.Num() == P.Blocks.Num() && BodyBlock != INDEX_NONE)
			{
				TestEqual(TEXT("FIXTURE B [RED]: PieceOfBlock names the FStructure body piece"),
					P.PieceOfBlock[BodyBlock], Fx.Body);
			}
		}
	}

	return true;
}

/*
 * Permutation determinism, the design gate (D7). The phase-1 dual is non-unique, so
 * canonicalization (normalized certificate, per-block threshold tau, lexicographic tie-break) must
 * give one named set regardless of order; tau and the tie-break are chosen against this fuzz. For
 * each fixture over seeded permutations: the base set is non-empty (so empty sets cannot match
 * vacuously), and the moving and opening sets mapped back are identical. Bite-prover: a
 * non-canonical selection tracks column order and diverges.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMechanismPermutationDeterminismTest,
	"DestructionGame.Oracle.RigidBlock.Mechanism.IsPermutationDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMechanismPermutationDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleMechanismExtractionSupport;

	auto BuildBaseB = []() -> FOracleProblem
	{
		FTwoPathBody Fx;
		BuildFixtureB(Fx);
		FOracleProblem P;
		FString Why;
		if (!BuildRigidBlockProblem(Fx.Structure, P, Why))
		{
			return FOracleProblem();
		}
		P.bGravityIsLive = false;
		return P;
	};

	struct FNamed
	{
		FOracleProblem Base;
		FString Label;
	};

	TArray<FNamed> Fixtures;
	Fixtures.Add({ BuildFixtureA(), TEXT("A(single-overhang)") });
	Fixtures.Add({ BuildBaseB(), TEXT("B(two-load-path)") });
	Fixtures.Add({ BuildFixtureD(), TEXT("D(degenerate-mixed-stack)") });

	const int32 BaseSeed = 0x03A0F00D;
	const int32 NumPermutations = 6;

	for (const FNamed& Fixture : Fixtures)
	{
		const FOracleProblem& Base = Fixture.Base;
		if (Base.Blocks.Num() == 0)
		{
			AddError(*FString::Printf(TEXT("%s: base problem failed to build"), *Fixture.Label));
			continue;
		}

		const FOracleResult BaseR = SolveRigidBlock(Base);

		TestEqual(*FString::Printf(TEXT("%s: base is infeasible (Falls)"), *Fixture.Label),
			static_cast<int32>(OutcomeOf(BaseR)), static_cast<int32>(EOracleOutcome::Falls));

		const TSet<int32> BaseMoving = MovingBlocks(BaseR.Mechanism);
		const TSet<int32> BaseOpening = OpeningJoints(BaseR.Mechanism);

		FString MovingList;
		for (int32 B : BaseMoving.Array())
		{
			MovingList += FString::Printf(TEXT("%d "), B);
		}

		AddInfo(*FString::Printf(
			TEXT("%s: base names %d moving block(s) {%s}, %d opening joint(s), present %d; "
				 "DEGENERACY: %d Bland entries, %d pivots over %d blocks/%d joints"),
			*Fixture.Label, BaseMoving.Num(), *MovingList, BaseOpening.Num(),
			BaseR.Mechanism.bPresent ? 1 : 0,
			BaseR.BlandDegenerateEntries, BaseR.SimplexIterations,
			Base.Blocks.Num(), Base.Joints.Num()));

		// Two empty sets compare equal, so the base must name something.
		TestTrue(
			*FString::Printf(TEXT("%s [RED]: the base mechanism names a NON-EMPTY moving set"), *Fixture.Label),
			BaseMoving.Num() >= 1);

		// Raw-dual drift > 0 shows the dual is non-unique, so a stable set is canonicalization's work.
		double WorstRawDualDrift = 0.0;

		for (int32 Perm = 0; Perm < NumPermutations; ++Perm)
		{
			const int32 Seed = BaseSeed + Perm;
			FRandomStream Rng(Seed);

			const TArray<int32> BlockPerm = SeededPermutation(Rng, Base.Blocks.Num());
			const TArray<int32> JointPerm = Base.Joints.Num() > 0
				? SeededPermutation(Rng, Base.Joints.Num())
				: TArray<int32>();

			const FOracleProblem PermProblem = Permute(Base, BlockPerm, JointPerm);
			const FOracleResult PermR = SolveRigidBlock(PermProblem);

			for (int32 Old = 0; Old < Base.Blocks.Num(); ++Old)
			{
				const int32 New = BlockPerm[Old];
				const double BaseUz = BaseR.Mechanism.Blocks.IsValidIndex(Old)
					? BaseR.Mechanism.Blocks[Old].VirtualUz : 0.0;
				const double PermUz = PermR.Mechanism.Blocks.IsValidIndex(New)
					? PermR.Mechanism.Blocks[New].VirtualUz : 0.0;
				WorstRawDualDrift = FMath::Max(WorstRawDualDrift, FMath::Abs(BaseUz - PermUz));
			}

			TestEqual(
				*FString::Printf(TEXT("%s seed=%d: the verdict is permutation-invariant (Falls)"), *Fixture.Label, Seed),
				static_cast<int32>(OutcomeOf(PermR)), static_cast<int32>(EOracleOutcome::Falls));

			// Map the permuted sets back to base indices.
			TSet<int32> PermMovingInBase;
			for (int32 Old = 0; Old < Base.Blocks.Num(); ++Old)
			{
				if (BlockMoves(PermR.Mechanism, BlockPerm[Old]))
				{
					PermMovingInBase.Add(Old);
				}
			}
			TSet<int32> PermOpeningInBase;
			for (int32 Old = 0; Old < Base.Joints.Num(); ++Old)
			{
				if (JointOpens(PermR.Mechanism, JointPerm[Old]))
				{
					PermOpeningInBase.Add(Old);
				}
			}

			const bool bMovingSame =
				PermMovingInBase.Num() == BaseMoving.Num() && PermMovingInBase.Includes(BaseMoving);
			const bool bOpeningSame =
				PermOpeningInBase.Num() == BaseOpening.Num() && PermOpeningInBase.Includes(BaseOpening);

			TestTrue(
				*FString::Printf(
					TEXT("%s seed=%d [GATE]: the canonical MOVING set is identical under permutation "
						 "(base %d, permuted %d)"),
					*Fixture.Label, Seed, BaseMoving.Num(), PermMovingInBase.Num()),
				bMovingSame);

			TestTrue(
				*FString::Printf(
					TEXT("%s seed=%d [GATE]: the canonical OPENING-joint set is identical under permutation "
						 "(base %d, permuted %d)"),
					*Fixture.Label, Seed, BaseOpening.Num(), PermOpeningInBase.Num()),
				bOpeningSame);
		}

		AddInfo(*FString::Printf(
			TEXT("%s: worst raw-dual VirtualUz drift across permutations = %.3e "
				 "(> 0 means the dual is non-unique and canonicalization is load-bearing)"),
			*Fixture.Label, WorstRawDualDrift));
	}

	return true;
}

/*
 * Farkas fail-closed. A: the mechanism is present, certified, and does positive gravity work. C: no
 * mechanism is reported. An unverifiable certificate must refuse with VerificationFailure (§3.6);
 * that needs a solver mutation, so here only the weaker direction is checked. Bite-provers: skipping
 * the Farkas check reddens A; emitting a mechanism on the feasible arm reddens C.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMechanismFarkasFailClosedTest,
	"DestructionGame.Oracle.RigidBlock.Mechanism.FarkasCertificateFailsClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMechanismFarkasFailClosedTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleMechanismExtractionSupport;

	// Infeasible: present, certified, and a real collapse.
	{
		const FOracleProblem P = BuildFixtureA();
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("FARKAS A: answered %d, outcome %d (1=Falls), present %d, certified %d, refusal %d, work %.6g"),
			R.bAnswered ? 1 : 0, static_cast<int32>(OutcomeOf(R)),
			R.Mechanism.bPresent ? 1 : 0, R.Mechanism.bIsCertified ? 1 : 0,
			static_cast<int32>(R.Refusal), GravityVirtualWork(P, R.Mechanism)));

		TestTrue(TEXT("FARKAS A: the oracle answers the infeasible problem"), R.bAnswered);
		TestEqual(TEXT("FARKAS A: it is infeasible (Falls)"),
			static_cast<int32>(OutcomeOf(R)), static_cast<int32>(EOracleOutcome::Falls));

		TestTrue(TEXT("FARKAS A [RED]: the mechanism is present"), R.Mechanism.bPresent);
		TestTrue(TEXT("FARKAS A [RED]: the mechanism is Farkas-CERTIFIED (bIsCertified)"),
			R.Mechanism.bIsCertified);

		const double Work = GravityVirtualWork(P, R.Mechanism);
		TestTrue(
			*FString::Printf(TEXT("FARKAS A [RED]: yb > 0 — gravity does positive work on the mechanism (%.6g)"), Work),
			Work > 0.0);

		TestNotEqual(TEXT("FARKAS A: a certified fixture is NOT a VerificationFailure"),
			static_cast<int32>(R.Refusal), static_cast<int32>(EOracleRefusal::VerificationFailure));
	}

	// Feasible: never a mechanism.
	{
		const FOracleProblem P = BuildFixtureC();
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("FARKAS C: answered %d, outcome %d (2=Stands), present %d, moving %d, certified %d"),
			R.bAnswered ? 1 : 0, static_cast<int32>(OutcomeOf(R)),
			R.Mechanism.bPresent ? 1 : 0, MovingBlocks(R.Mechanism).Num(), R.Mechanism.bIsCertified ? 1 : 0));

		TestTrue(TEXT("FARKAS C: the oracle answers the feasible problem"), R.bAnswered);
		TestEqual(TEXT("FARKAS C: it stands (feasible)"),
			static_cast<int32>(OutcomeOf(R)), static_cast<int32>(EOracleOutcome::Stands));

		TestFalse(TEXT("FARKAS C: a feasible problem reports NO mechanism (bPresent false)"),
			R.Mechanism.bPresent);
		TestEqual(TEXT("FARKAS C: a feasible problem moves no block"),
			MovingBlocks(R.Mechanism).Num(), 0);
		TestFalse(TEXT("FARKAS C: a feasible problem's mechanism is not certified"),
			R.Mechanism.bIsCertified);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
