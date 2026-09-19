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
 * Oracle-side collapse-mechanism extraction tests (PROMOTION_DESIGN.md §12 D7), driving the
 * extraction before any wall depends on it. They pin three things:
 *
 *   1. Mechanism correctness against an independent kinematic oracle. Two small infeasible
 *      fixtures whose collapse mechanism is hand-derivable — a brick overhanging its tipping
 *      point on one bearing, and a body on two load paths left of its centroid — asserted
 *      against hand rigid-body kinematics rather than the LP dual, plus a feasible fixture
 *      whose mechanism must be empty.
 *   2. Permuted-column determinism, the gate: the canonical named set must be identical when the
 *      problem's block/joint order is permuted by a seeded permutation, pinning the non-unique
 *      degenerate dual to a stable break set. A failure here is a design-gate failure: the whole
 *      slice stops and the design changes shape (D7).
 *   3. Farkas fail-closed: a reported mechanism must be certified (bIsCertified) and be a
 *      genuine downward collapse (positive virtual work of gravity, the yb > 0 half of Farkas,
 *      checked independently here). A feasible problem must never report one.
 *
 * Why the feasibility formulation: the mechanism lives on the infeasible arm of the feasibility
 * LP (PROMOTION_DESIGN §3.2/§3.3) — gravity dead (bGravityIsLive = false), no live forces, so
 * phase 1 genuinely runs and, when no admissible force system exists, its dual is the Farkas
 * certificate = the kinematic mechanism. A standing structure then reads Lambda = LambdaCap and
 * a falling one Lambda = 0 (RigidBlockOracle.cpp's infeasible arm, ~line 1596).
 *
 * Independence is the whole value: the kinematic oracle is rigid-body statics worked by hand —
 * which block rotates about which edge, which contact lifts — derived a different way from the
 * LP's phase-1 dual; an oracle that mirrored the dual would be worthless. Units are derived in
 * this file (1 MPa over 1 cm2 = 10000 uu), never imported, so a wrong production constant
 * disagrees rather than agrees.
 *
 * Red for the right reason: FOracleMechanism is empty by default (bPresent = false, no triples,
 * uncertified), so every moves/opens/present/certified/positive-work assertion fails on missing
 * behaviour; the seat-stays-still and feasible-is-empty assertions are green on arrival (an
 * empty mechanism moves nothing).
 *
 * No ticking world: every fixture is a pure FOracleProblem (or a bridged FStructure) fed to
 * SolveRigidBlock — no Chaos, no world tick.
 *
 * Named namespace, not anonymous: a unity build merges many files into one translation unit.
 */
namespace OracleMechanismExtractionSupport
{
	using namespace RigidBlockOracle;
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* Units, derived here so a wrong production constant fails rather than agrees. */

	/** MassKg * 980 is a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. NOT imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	constexpr double ClayDensityGramsPerCubicCm = 1.9;
	constexpr double WytheWidthCm = 10.25;

	/* A tiny hand-built FOracleProblem vocabulary: blocks and one bed joint, all in the X-Z
	 * plane with an upward (0,1) normal, so nothing goes near the bridge and the kinematics
	 * stay hand-checkable. */

	/** A grounded seat block: it is the earth, writes no equilibrium rows, must read a zero triple. */
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

	/* FIXTURE A — a single brick overhanging past its tipping point on one bearing.
	 *
	 * A long bar rests on one narrow grounded seat near its left end; its centre of mass hangs
	 * far to the right of the seat. Under its own dead weight it rotates clockwise about the
	 * seat's right contact edge (the fulcrum); the seat's left contact lifts.
	 *
	 *   body centroid (X = 97.5) --------------------------------+
	 *   +--------------------------------------------------------+   one free bar
	 *   | [seat]                                                 |   overhang -->
	 *   +--##----------------------------------------------------+
	 *      earth      seat spans X in [-5, +5], fulcrum at X = +5
	 *
	 * Hand-derived mechanism (rigid-body kinematics, not the LP dual): the body is the only
	 * moving block — it rotates about the fulcrum (VirtualOmega != 0) and its centroid (right
	 * of the fulcrum) descends (VirtualUz < 0); the seat is grounded and uninvolved (triple ~0,
	 * bMoves = false); the single bed joint opens (its left contact carries tension at the
	 * plastic limit and lifts, so JointOpensOrSlides[0] = true); gravity does positive work on
	 * the mechanism, the yb > 0 half of the Farkas certificate, checked here from the triples.
	 *
	 * Past tipping by a clear margin: overturning about the fulcrum is W*(X_com - X_fulcrum),
	 * and the most charitable plastic bond — the left contact pulling down at f_t over its
	 * tributary half-area, at its lever from the fulcrum — is worked in RunTest and outrun by
	 * ~4x, so no admissible force system exists. */

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

	/** Most charitable plastic restoring: the left contact at f_t over its half-area, at its lever. */
	double A_MaxPlasticRestoringUuCm(double BondMPa)
	{
		const double PerContactUu = BondMPa * ForceUnitsPerMPaSqCmHere * (A_JointAreaSqCm / 2.0);
		const double LeftLeverCm = A_FulcrumXCm - (A_SeatCentreXCm - A_SeatHalfXCm); /* +5 - (-5) = 10 */
		return PerContactUu * LeftLeverCm;
	}

	/** Block indices in fixture A: 0 = seat (grounded), 1 = body (free). One joint, index 0. */
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

	/* FIXTURE C — a stable brick centred on its seat (feasible). The body's centre of mass sits
	 * directly over the middle of a wide seat, well inside the kern, so an admissible force
	 * system exists (pure compression). Under the feasibility formulation it reads
	 * Lambda = LambdaCap and its mechanism must be empty — the "feasible never reports a
	 * mechanism" control for Farkas. */

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

	/* FIXTURE B — a body on two load paths, past tipping, built as an FStructure and bridged so
	 * the bridge path and its provenance maps get driven. Two grounded seats, both left of the
	 * body's centroid, so the body races out past both bearings and rotates about the rightmost
	 * bearing edge — the topology TwoLoadPathOverturning uses, constants re-derived here.
	 *
	 * Hand-derived mechanism: the body moves (rotation about the pivot seat's right edge,
	 * centroid far to the right descends -> VirtualUz < 0); both grounded seats stay still
	 * (zero triples); both bed joints open on their lifting side. */

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

	/* FIXTURE D — a genuinely degenerate multi-block infeasible fixture, added so the
	 * permutation-determinism gate is not vacuous. Fixtures A and B each have exactly one free
	 * block, so their moving set is trivially that block whatever the dual does — they exercise
	 * extraction but not the canonical selection of a non-trivial subset from a degenerate dual
	 * (PROMOTION_DESIGN §12 D7's flagged risk). D poses two disconnected sub-structures:
	 *
	 *   - a dry leaning stack, offset hard past its tipping point, on its own grounded base — no
	 *     tensile bond, so it rocks and falls; all its free blocks rotate together and are the
	 *     moving set. NOTE (corrected 2026-08-27): this fixture measures 0 Bland entries — its
	 *     degeneracy is dual-multiplier non-uniqueness (what made the joint set wobble 8->16
	 *     before the kinematic derivation), not the 170-358-Bland opening-ladder regime; it does
	 *     not exercise the Bland fallback. The Bland-degenerate multi-mode permutation fixture
	 *     D7 needs before 3b is still owed (CURRENT_STATE).
	 *   - a robustly stable dry stack, centred well inside its own wide grounded seat far away
	 *     in X — it stands, so its free blocks have ~zero virtual motion and must stay out of
	 *     the moving set under every column permutation.
	 *
	 * So the canonical named set is a strict, non-trivial subset of the free blocks, selected
	 * from a degenerate dual: exactly the case the determinism gate must bite on. Dry stone
	 * (f_t = 0) keeps the plastic no-tension form, so the tipping is hand-obvious and the
	 * infeasibility does not depend on a bond constant. */

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

		/* ---- The FALLING dry leaning stack: base grounded, then free courses offset +X. ---- */
		const int32 NumLeaning = 24;
		const double OffsetXCm = 4.0;                 /* per course; 20-wide bricks -> steady lean */
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

		/* ---- The STANDING dry stack: centred well inside a wide grounded seat, far in X. ---- */
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

	/* Mechanism inspection — the named set, and the independent Farkas-lite work check. Guards
	 * against the empty stub so a missing mechanism fails an assertion rather than reads out of
	 * bounds. */

	/** The set of oracle-block indices the canonical mechanism says MOVE. */
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

	/** The set of oracle-joint indices the canonical mechanism says OPEN or SLIDE. */
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

	/** True iff block B is present in the mechanism array and flagged moving. */
	bool BlockMoves(const FOracleMechanism& M, int32 B)
	{
		return M.Blocks.IsValidIndex(B) && M.Blocks[B].bMoves;
	}

	/** True iff joint J is present in the mechanism array and flagged open/slide. */
	bool JointOpens(const FOracleMechanism& M, int32 J)
	{
		return M.JointOpensOrSlides.IsValidIndex(J) && M.JointOpensOrSlides[J];
	}

	/**
	 * The virtual work gravity does on the reported mechanism: sum over blocks of
	 * (-W_i) . (VirtualUz_i), which for a genuine downward collapse is POSITIVE (weighted
	 * centroids descend, VirtualUz < 0). This is the yb > 0 half of the Farkas certificate,
	 * recomputed here from the triples and the block masses alone — independent of the LP.
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

	/** The largest triple magnitude among blocks NOT in the given set — the "everything else" motion. */
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

	/* Permutation — reorder an FOracleProblem's blocks and joints by seeded permutations,
	 * remapping every block reference, so the same physics is posed in a different column and
	 * row order. NewIndex = Perm[OldIndex]. */

	TArray<int32> SeededPermutation(FRandomStream& Rng, int32 N)
	{
		TArray<int32> Perm;
		Perm.SetNumUninitialized(N);
		for (int32 I = 0; I < N; ++I)
		{
			Perm[I] = I;
		}
		/* Fisher-Yates on the identity: Perm[i] becomes NewIndex-of-old-i after inversion below. */
		for (int32 I = N - 1; I > 0; --I)
		{
			const int32 J = Rng.RandRange(0, I);
			Swap(Perm[I], Perm[J]);
		}
		/* Perm currently maps NewIndex -> OldIndex; invert to OldIndex -> NewIndex. */
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

/* TEST 1 — mechanism correctness against an independent kinematic oracle.
 *
 * Asserts (against hand-derived rigid-body kinematics, not the LP dual): Fixture A (single
 * overhang) — the body moves, the seat does not, the single joint opens, the body's centroid
 * descends, and gravity does positive work; Fixture C (stable, centred) — the mechanism is
 * empty (bPresent = false, nothing moves); Fixture B (two load paths, bridged) — the body
 * moves, both seats stay still, and the bridge fills its provenance maps (PieceOfBlock /
 * ConnectionOfJoint) so the mechanism can name real pieces. The body block is located by
 * centroid match, independent of block order and provenance, so a wrong provenance map cannot
 * mask a wrong mechanism.
 *
 * Red because FOracleMechanism is empty by default, so moves/opens/present/positive-work all
 * fail on missing behaviour; the seat-still and feasible-empty arms are green on arrival.
 * Mutation that proves it bites once extraction lands: force the mechanism empty (skip the
 * BTRAN / clear the arrays at the infeasible arm) and every such assertion goes red again.
 *
 * Needs a ticking world: no. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMechanismCorrectnessTest,
	"DestructionGame.Oracle.RigidBlock.Mechanism.NamesTheHandDerivedCollapse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMechanismCorrectnessTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace DestructionProfiles;
	using namespace OracleMechanismExtractionSupport;

	/* ---------------- FIXTURE A: single overhang ---------------- */

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

		/* The feasibility formulation must answer, and answer INFEASIBLE (Falls, lambda* == 0). */
		TestTrue(TEXT("FIXTURE A: the oracle answers"), R.bAnswered);
		TestEqual(TEXT("FIXTURE A: infeasible under dead gravity (Falls)"),
			static_cast<int32>(OutcomeOf(R)), static_cast<int32>(EOracleOutcome::Falls));

		/* THE RED: a mechanism must be present and name the hand-derived collapse. */
		TestTrue(TEXT("FIXTURE A [RED]: an infeasible problem must carry a mechanism (bPresent)"),
			R.Mechanism.bPresent);

		TestTrue(TEXT("FIXTURE A [RED]: the BODY moves"), BlockMoves(R.Mechanism, A_Body));
		TestFalse(TEXT("FIXTURE A: the grounded SEAT does not move"), BlockMoves(R.Mechanism, A_Seat));
		TestTrue(TEXT("FIXTURE A [RED]: the single bed JOINT opens"), JointOpens(R.Mechanism, 0));

		/* The body's centroid must DESCEND (VirtualUz < 0) — the rotation-about-fulcrum sign. */
		const double BodyUz = R.Mechanism.Blocks.IsValidIndex(A_Body)
			? R.Mechanism.Blocks[A_Body].VirtualUz : 0.0;
		TestTrue(
			*FString::Printf(TEXT("FIXTURE A [RED]: the body's centroid descends, VirtualUz %.6g < 0"), BodyUz),
			BodyUz < 0.0);

		/* The seat's triple must be ~zero relative to the moving body's motion. */
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

		/* Farkas yb > 0: gravity does positive work on the collapse. */
		const double Work = GravityVirtualWork(P, R.Mechanism);
		TestTrue(
			*FString::Printf(TEXT("FIXTURE A [RED]: gravity does positive work on the mechanism, %.6g > 0"), Work),
			Work > 0.0);
	}

	/* ---------------- FIXTURE C: stable, feasible -> empty mechanism ---------------- */
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

		/* Green on arrival (empty stub) — the "feasible never reports a mechanism" control. */
		TestFalse(TEXT("FIXTURE C: a feasible problem carries NO mechanism (bPresent false)"),
			R.Mechanism.bPresent);
		TestEqual(TEXT("FIXTURE C: a feasible problem moves no block"),
			MovingBlocks(R.Mechanism).Num(), 0);
	}

	/* ---------------- FIXTURE B: two load paths, bridged ---------------- */
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

			/* Locate the body block by CENTROID MATCH — independent of block order and provenance. */
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

			/* THE RED: the body moves, and both grounded seats do not. */
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

			/* Positive collapse work (independent Farkas yb > 0). */
			const double Work = GravityVirtualWork(P, R.Mechanism);
			TestTrue(
				*FString::Printf(TEXT("FIXTURE B [RED]: gravity does positive work on the mechanism, %.6g > 0"), Work),
				Work > 0.0);

			/* Bridge provenance seam (PROMOTION_DESIGN §12 D7): the bridge must fill PieceOfBlock and
			 * ConnectionOfJoint so the mechanism can name real pieces/joints. Red now — the stub
			 * leaves them empty — separately from the mechanism reds above (which map by centroid). */
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

/* TEST 2 — permuted-column determinism, the gate on the whole slice.
 *
 * A failure here is a design-gate failure, not a tuning nit. The phase-1 optimum is massively
 * degenerate (170-358 Bland entries on a single opening-ladder rung), so the raw dual is
 * non-unique. Canonicalization (normalize the certificate, per-block relative-magnitude
 * threshold tau, minimal-support lexicographic tie-break) must pin it to one named set that does
 * not depend on column/block arrival order — if it cannot be made permutation-stable, Slice 3
 * stops and the design changes shape (D7). tau and the tie-break must be chosen against this
 * permutation fuzz, never by eye.
 *
 * Asserts, for fixtures A and B over several seeded block+joint permutations: the base
 * (unpermuted) mechanism names a non-empty moving set (guards against two compared-empty sets
 * passing vacuously); the canonical moving set and opening-joint set, translated back through
 * the known permutation inverse, are identical across every permutation.
 *
 * Red because the empty stub names an empty set, so the non-empty assertion fails on missing
 * behaviour (the set-equality assertion is vacuously green while empty; it becomes load-bearing
 * once extraction populates a set). Mutation that proves it bites: replace the canonical
 * selection with a fixed non-canonical one (e.g. "move the lowest-index non-grounded block", or
 * the raw dual without the lexicographic tie-break) — the named set then tracks column order and
 * diverges from the base under permutation.
 *
 * Needs a ticking world: no. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMechanismPermutationDeterminismTest,
	"DestructionGame.Oracle.RigidBlock.Mechanism.IsPermutationDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMechanismPermutationDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleMechanismExtractionSupport;

	/* Build the two base problems, both purely at the oracle level for full control. */
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

		/* The base must be the infeasible fixture the whole test is about. */
		TestEqual(*FString::Printf(TEXT("%s: base is infeasible (Falls)"), *Fixture.Label),
			static_cast<int32>(OutcomeOf(BaseR)), static_cast<int32>(EOracleOutcome::Falls));

		const TSet<int32> BaseMoving = MovingBlocks(BaseR.Mechanism);
		const TSet<int32> BaseOpening = OpeningJoints(BaseR.Mechanism);

		/* DEGENERACY, MEASURED: how hard the phase-1 optimum makes canonicalization work. */
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

		/* GUARD AGAINST VACUITY: two empty sets compare equal. The base MUST name something. */
		TestTrue(
			*FString::Printf(TEXT("%s [RED]: the base mechanism names a NON-EMPTY moving set"), *Fixture.Label),
			BaseMoving.Num() >= 1);

		/* Track whether the raw dual actually moves under permutation: if the permuted solve's
		 * per-block VirtualUz (mapped back to base indices) ever differs from the base's beyond
		 * rounding, the dual is genuinely non-unique and the canonical set holding steady is
		 * canonicalization doing real work, not a trivially unique dual relabelled. */
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

			/* The verdict itself must be permutation-invariant (already true today). */
			TestEqual(
				*FString::Printf(TEXT("%s seed=%d: the verdict is permutation-invariant (Falls)"), *Fixture.Label, Seed),
				static_cast<int32>(OutcomeOf(PermR)), static_cast<int32>(EOracleOutcome::Falls));

			/* Translate the permuted named sets back to the base (original) index space. */
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

/* TEST 3 — Farkas fail-closed. The extraction must certify its own certificate.
 *
 * Asserts: Fixture A (infeasible) — the reported mechanism is certified (bIsCertified) and is a
 * genuine downward collapse (gravity does positive work, the yb > 0 half of Farkas, recomputed
 * here); a present-but-uncertified mechanism is forbidden. Fixture C (feasible) — no mechanism
 * is ever reported (bPresent false, nothing moves, not certified).
 *
 * Specified, not driven here (needs a solver mutation to reach): an unverifiable certificate
 * must make the solve refuse with EOracleRefusal::VerificationFailure rather than hand out a
 * mechanism — the two-sided verification gate of §3.6, wired by dev-expert at the infeasible
 * arm and exercised by the R7 gate and a Farkas-corruption mutation. Asserted here only in the
 * weaker direction: a certified fixture must not be sitting on VerificationFailure.
 *
 * Red because the empty stub never certifies and does no work, so fixture A's certified /
 * positive-work assertions fail on missing behaviour; fixture C's empty-mechanism arm is green
 * on arrival (nothing to certify). Mutation that proves it bites: (a) skip the Farkas check so
 * bIsCertified stays false while present -> fixture A's certified assertion goes red; (b) emit a
 * mechanism on the feasible arm too -> fixture C goes red.
 *
 * Needs a ticking world: no. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMechanismFarkasFailClosedTest,
	"DestructionGame.Oracle.RigidBlock.Mechanism.FarkasCertificateFailsClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMechanismFarkasFailClosedTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleMechanismExtractionSupport;

	/* ---------------- Infeasible: must be present AND certified AND a real collapse. ---------------- */
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

		/* yb > 0: the collapse releases gravitational energy — checked independently from the triples. */
		const double Work = GravityVirtualWork(P, R.Mechanism);
		TestTrue(
			*FString::Printf(TEXT("FARKAS A [RED]: yb > 0 — gravity does positive work on the mechanism (%.6g)"), Work),
			Work > 0.0);

		/* A certified fixture must not be refused for verification failure. */
		TestNotEqual(TEXT("FARKAS A: a certified fixture is NOT a VerificationFailure"),
			static_cast<int32>(R.Refusal), static_cast<int32>(EOracleRefusal::VerificationFailure));
	}

	/* ---------------- Feasible: NEVER a non-empty mechanism. ---------------- */
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

		/* Green on arrival — the fail-closed direction that must never regress. */
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
