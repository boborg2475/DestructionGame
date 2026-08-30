// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E3 — THE 3D BRIDGE, RED (THREED_DESIGN.md "E1 slice sequence" E3 bullet).
 *
 * The oracle can already solve genuinely-3D problems (E1a-E2c: six equilibrium rows, four-corner
 * contacts, the k=8 inscribed friction pyramid, 3D applied forces, a permutation-deterministic 3D
 * mechanism), reachable by posing a hand-built FOracleProblem with Dim = Dim3D. What CANNOT reach
 * it is a production FStructure: RigidBlockBridge is still 2D-only. Two pieces of it are what E3
 * replaces, both quoted from RigidBlockBridge.cpp:
 *
 *   THE Y-NORMAL REFUSAL (~:126-133):
 *       if (FMath::Abs(Normal.Y) > 1.0e-9) { OutWhyNot = "... an out-of-plane (Y) normal, which a
 *       2D X-Z oracle must refuse rather than project"; OutProblem = FOracleProblem(); return false; }
 *
 *   THE HALF-EXTENT SHORTCUT (~:148-150):
 *       Out.HalfLengthCm = FMath::Abs(Normal.Z) >= FMath::Abs(Normal.X)
 *           ? Joint.InterfaceHalfExtentCm.X : Joint.InterfaceHalfExtentCm.Z;   // picks ONE in-plane extent
 *
 * The bridge also drops the block's plan-Y (it sets CentroidXCm/CentroidZCm but never CentroidYCm)
 * and never sets OutProblem.Dim (so every bridged problem is Dim2D).
 *
 * THE GEOMETRY IS ALREADY THERE. FConnection carries a full 3D InterfaceNormal, a 3D
 * InterfaceCentreCm and per-axis InterfaceHalfExtentCm, and FStructurePiece carries a 3D
 * CentreOfMassCm — so E3 is "pose the 3D geometry FStructure already holds", not "add 3D fields to
 * FStructure first". The ONE thing FStructure lacked is the SIGNAL that a structure is meant to be
 * posed in 3D; this test adds it as SetThreeDimensional (a bare 2D-default flag stub, see its
 * contract) and the bridge is meant to key its Dim3D pose and its Y-normal lift on that flag while a
 * 2D structure's Y-normal stays loudly refused.
 *
 * THE FIXTURE — A BLOCK BONDED TO A WALL ACROSS A VERTICAL, OUT-OF-PLANE (Y-FACING) JOINT.
 *
 *                          Z
 *                          ^        Wall (grounded)      Block (free, weight W = -Z)
 *                          |      +-----------+ | +-----------+
 *                          |      |           | | |           |
 *                          |      |   WALL    |=|=|  BLOCK    |   the "=" is the bed... no: the joint
 *                          |      |           | | |           |   is the VERTICAL face between them,
 *                          |      +-----------+ | +-----------+   whose normal points along +Y.
 *                          +----------------------------------> Y
 *
 *   Wall A: grounded, box centre (0, 0, 15), half-extents (10, 5, 15)  -> spans Y[-5, 5], Z[0, 30].
 *   Block B: free,     box centre (0, 11, 15), half-extents (10, 5, 15) -> spans Y[6, 16], Z[0, 30].
 *   They are separated on EXACTLY the Y axis by a 1 cm joint and overlap fully on X (20 cm) and Z
 *   (30 cm), so MakeInterface builds ONE joint whose normal is +Y (the axis of separation, A->B),
 *   whose face is 20 x 30 = 600 cm2, whose in-plane half-extents are (X 10, Z 15) — DELIBERATELY
 *   UNEQUAL so a shortcut that keeps only one is caught — and whose centre is (0, 5.5, 15).
 *
 * This is genuinely out of the X-Z plane: the joint normal is +Y, so gravity (-Z) is PERPENDICULAR
 * to it and can only be carried in SHEAR. The 2D X-Z oracle cannot express such a joint at all,
 * which is exactly why the bridge refuses it today.
 *
 * WHAT IS ASSERTED.
 *
 *   POSED GEOMETRY (the bridge must accept the 3D structure and carry the real 3D joint):
 *     - the bridge ACCEPTS it (no refusal) and sets Problem.Dim = Dim3D;
 *     - the bridged joint has |NormalY| ~ 1 with NormalX ~ NormalZ ~ 0 (the out-of-plane normal
 *       LIFTED, not refused, not projected onto X-Z);
 *     - its CentreYCm ~ 5.5 and AreaSqCm ~ 600 are carried;
 *     - its two in-plane half-extents {HalfUCm, HalfVCm} are the real pair {10, 15} — the
 *       |Nz|-vs-|Nx| shortcut is GONE (it could only ever set one length);
 *     - the free block's CentroidYCm ~ 11 is carried (the dropped plan-Y);
 *     - the joint's strength (the cohesive bond) is carried through EffectiveJointStrength.
 *
 *   SOLVE OUTCOME (end-to-end, the bridged problem fed straight to SolveRigidBlock):
 *     - STANDS arm: a HUGE bond -> the shear joint carries the weight -> OutcomeOf == Stands.
 *     - FALLS arm: a ZERO bond (a frictionless, cohesionless, bondless Y-contact) -> the joint can
 *       carry NO shear (the k=8 pyramid pins shear to 0) and no peel -> no admissible equilibrium ->
 *       OutcomeOf == Falls, and on the feasibility pose the collapse MECHANISM moves the FREE block
 *       (descending, VirtualUz < 0) and not the grounded wall. Asserting the MECHANISM, never
 *       displacement (DESIGN.md §4: two pieces can sever and rest exactly in place).
 *
 * WHY THE SOLVE IS PYRAMID-INDEPENDENT. The exact shear capacity of the inscribed octagon depends on
 * whether -Z lands on a facet (cos(pi/8)*R) or a vertex (R) of the pyramid, which is the "wrong axis
 * governs" trap. This test never relies on that: the two arms BRACKET the verdict with a bond that
 * is either enormous (stands however the octagon is oriented) or exactly zero (falls however it is
 * oriented). No facet/vertex arithmetic is load-bearing.
 *
 * WHY THIS IS RED, AND FOR THE RIGHT REASON. BuildRigidBlockProblem hits the |Normal.Y| > 1e-9 guard
 * on the one joint, empties the problem and returns false with the "out-of-plane (Y) normal" reason.
 * So bBridged is false, the problem is empty, Dim is its Dim2D default, and every posed-geometry and
 * solve assertion below fails on the missing problem. That is the bridge REFUSING the out-of-plane
 * normal — not a compile error (the stub flag and the 3D oracle fields all exist), not a broken
 * fixture (the structure has complete geometry and one honest Y-facing joint).
 *
 * BITE (for dev, at green): the {HalfUCm, HalfVCm} == {10, 15} assertion cannot be satisfied by the
 * old single-length shortcut; the CentroidYCm ~ 11 assertion cannot be satisfied while the block's
 * plan-Y is dropped; and the FALLS mechanism moving the free block cannot be produced by a 2D
 * projection of a Y-normal joint (there is no such projection).
 *
 * UNITS derived here (mass * 980 already carries 1 N = 100 uu; 1 MPa over 1 cm2 = 10000 uu), NOT
 * imported, so a wrong production constant disagrees rather than agrees. The solve arms are binary
 * Stands/Falls and pure kinematics, so no strength/force comparison crosses the unit boundary; the
 * bonds only have to be "enormous" or "exactly zero".
 *
 * NEEDS A TICKING WORLD: NO. A hand-built FStructure bridged and fed to SolveRigidBlock — no Chaos,
 * no world tick, core-only, the same footing as the other 3D oracle tests and CrossMaterialBearing.
 *
 * NAMED NAMESPACE, not anonymous: the unity build merges many files into one translation unit.
 */
namespace ThreeDBridgeSupport
{
	using namespace DestructionLayout;
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * UNITS, derived here so a wrong production constant fails rather than agrees.
	 * ================================================================================ */

	/** MassKg * 980 is a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/* ================================================================================
	 * THE BONDED-TO-A-WALL BLOCK, hand-built so the Y-facing geometry stays checkable.
	 * ================================================================================ */

	constexpr double MassKg = 10.0;              /* W = 9800 uu */
	constexpr double JointThicknessCm = 1.0;     /* the 1 cm Y gap the joint is formed across */

	/* The joint face, worked out from the boxes below (see the header diagram). */
	constexpr double HalfXCm = 10.0;             /* X overlap 20 -> half 10 */
	constexpr double HalfZCm = 15.0;             /* Z overlap 30 -> half 15 (deliberately != HalfXCm) */
	constexpr double AreaSqCm = 4.0 * HalfXCm * HalfZCm;   /* 20 x 30 = 600 */
	constexpr double JointCentreYCm = 5.5;       /* midpoint of the 1 cm gap between Y=5 and Y=6 */
	constexpr double JointCentreXCm = 0.0;
	constexpr double JointCentreZCm = 15.0;
	constexpr double BlockComYCm = 11.0;         /* free block box centre Y — the dropped plan-Y */

	struct FBonded
	{
		FStructure Structure;
		int32 Wall = INDEX_NONE;   /* grounded */
		int32 Block = INDEX_NONE;  /* free, bonded to the wall across the Y-facing joint */
		int32 Joint = INDEX_NONE;  /* Wall -> Block, normal +Y */
	};

	FPieceBox Box(double CentreYCm)
	{
		FPieceBox B;
		B.ExtentCm = FVector(HalfXCm, 5.0, HalfZCm);
		B.CentreCm = FVector(0.0, CentreYCm, JointCentreZCm);
		return B;
	}

	/**
	 * A bond that STANDS: enormous on every axis, so the Y-facing shear joint carries the weight
	 * however the inscribed friction octagon happens to be oriented.
	 */
	FConnectionStrength StandingBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1.0e9;
		S.TensileStrengthMPa = 1.0e9;
		S.ShearCohesionMPa = 1.0e9;
		S.FrictionCoefficient = 1.0e9;
		return S;
	}

	/**
	 * A bond that FALLS: a frictionless, cohesionless, bondless contact. With c = mu = f_t = 0 the
	 * k=8 pyramid pins the joint's shear to exactly zero and there is no tension to resist a peel, so
	 * a Y-facing joint can carry NONE of the block's -Z weight — no admissible equilibrium exists.
	 */
	FConnectionStrength FallingBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1.0e9;   /* compression is irrelevant: the load is pure shear */
		S.TensileStrengthMPa = 0.0;
		S.ShearCohesionMPa = 0.0;
		S.FrictionCoefficient = 0.0;
		return S;
	}

	/** Lay the grounded wall and the free block bonded to its +Y face with the given bond. */
	void Build(FBonded& Out, const FConnectionStrength& Bond)
	{
		const FPieceBox WallBox = Box(0.0);
		const FPieceBox BlockBox = Box(BlockComYCm);

		/* Wall mass is irrelevant (grounded -> its weight goes to earth, not through the joint). */
		Out.Wall = Out.Structure.AddPiece(50.0, /*bIsGrounded*/ true, WallBox.CentreCm);
		Out.Block = Out.Structure.AddPiece(MassKg, /*bIsGrounded*/ false, BlockBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Wall, WallBox, Out.Block, BlockBox, JointThicknessCm, Bond, Joint))
		{
			Out.Joint = Out.Structure.AddConnection(Joint);
		}

		/* THE SIGNAL: this structure is meant to be posed in 3D (see SetThreeDimensional). */
		Out.Structure.SetThreeDimensional(true);
	}

	double WeightUu() { return MassKg * GravityCmPerSecondSquared; }

	/** The oracle block index that came from a given FStructure piece, or INDEX_NONE. */
	int32 OracleBlockOfPiece(const FOracleProblem& P, int32 Piece)
	{
		for (int32 B = 0; B < P.PieceOfBlock.Num(); ++B)
		{
			if (P.PieceOfBlock[B] == Piece)
			{
				return B;
			}
		}
		return INDEX_NONE;
	}

	/** The oracle joint index that came from a given FStructure connection, or INDEX_NONE. */
	int32 OracleJointOfConnection(const FOracleProblem& P, int32 Connection)
	{
		for (int32 J = 0; J < P.ConnectionOfJoint.Num(); ++J)
		{
			if (P.ConnectionOfJoint[J] == Connection)
			{
				return J;
			}
		}
		return INDEX_NONE;
	}

	bool Near(double A, double B, double Tol) { return FMath::Abs(A - B) <= Tol; }
}

/* ================================================================================================
 * THE 3D BRIDGE POSES A Y-FACING JOINT — E3, RED.
 *
 * RED BECAUSE BuildRigidBlockProblem refuses the joint's out-of-plane (Y) normal (the |Normal.Y| >
 * 1e-9 guard), empties the problem and returns false — so nothing below can read a Dim3D problem, a
 * lifted Y-normal, both real half-extents, the block's plan-Y or a 3D verdict.
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FThreeDBridgeTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.BridgePosesAnOutOfPlaneJoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FThreeDBridgeTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace ThreeDBridgeSupport;

	/* ------------------------------------------------------------------ *
	 * THE CONVERSION, DERIVED HERE. 1 N = 100 uu, 1 cm2 = 100 mm2, so
	 * 1 MPa (= 1 N/mm2) over 1 cm2 is 100 * 100 = 10000 uu. Independent of
	 * ForceUnitsPerMPaSqCm on purpose (nothing below actually needs it —
	 * the solve is binary — but the derivation is kept for the record).
	 * ------------------------------------------------------------------ */
	constexpr double UuPerMPaSqCm = 100.0 * 100.0;
	(void)UuPerMPaSqCm;

	/* ================================================================================
	 * POSE 1 — GEOMETRY: the bridge accepts the 3D structure and carries the real joint.
	 * ================================================================================ */
	{
		FBonded Fx;
		Build(Fx, StandingBond());

		/* ---- FIXTURE PRECONDITIONS — the structure is the one the header claims. ---- */
		if (Fx.Joint == INDEX_NONE)
		{
			AddError(TEXT("FIXTURE: MakeInterface must emit the Y-facing joint"));
			return true;
		}
		TestEqual(TEXT("FIXTURE: two pieces — the grounded wall and the free block"),
			Fx.Structure.NumPieces(), 2);
		TestEqual(TEXT("FIXTURE: one joint — the out-of-plane bearing"),
			Fx.Structure.NumConnections(), 1);
		TestTrue(TEXT("FIXTURE: complete geometry, so honest lever arms exist"),
			Fx.Structure.HasCompleteGeometry());
		TestTrue(TEXT("FIXTURE: the structure is flagged 3D"),
			Fx.Structure.IsThreeDimensional());

		const FConnection& Conn = Fx.Structure.GetConnection(Fx.Joint);
		TestTrue(
			*FString::Printf(TEXT("FIXTURE: the joint normal is out of the X-Z plane, |Y| ~ 1 (got %g, %g, %g)"),
				Conn.InterfaceNormal.X, Conn.InterfaceNormal.Y, Conn.InterfaceNormal.Z),
			FMath::IsNearlyEqual(FMath::Abs(Conn.InterfaceNormal.Y), 1.0, 1.0e-9));
		TestTrue(
			*FString::Printf(TEXT("FIXTURE: the joint face is 600 cm2, got %g"), Conn.InterfaceAreaSqCm),
			FMath::IsNearlyEqual(Conn.InterfaceAreaSqCm, AreaSqCm, 1.0e-6));

		/* ---- THE BRIDGE. ---- */
		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Fx.Structure, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("BRIDGE: accepted %d (why-not \"%s\"); Dim %d (2D=%d, 3D=%d); blocks %d, joints %d"),
			bBridged ? 1 : 0, *WhyNot,
			static_cast<int32>(Problem.Dim),
			static_cast<int32>(EOracleDim::Dim2D), static_cast<int32>(EOracleDim::Dim3D),
			Problem.Blocks.Num(), Problem.Joints.Num()));

		TestTrue(
			*FString::Printf(TEXT("[RED]: the bridge must ACCEPT a 3D-flagged structure with a Y-normal joint "
				"(it refuses today: \"%s\")"), *WhyNot),
			bBridged);

		TestEqual(TEXT("[RED]: the bridged problem is posed in 3D (Dim3D)"),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim3D));

		const int32 J = OracleJointOfConnection(Problem, Fx.Joint);
		const int32 FreeBlock = OracleBlockOfPiece(Problem, Fx.Block);

		TestTrue(TEXT("[RED]: the bridged problem carries the Y-facing joint"), J != INDEX_NONE);
		TestTrue(TEXT("[RED]: the bridged problem carries the free block"), FreeBlock != INDEX_NONE);

		if (J != INDEX_NONE)
		{
			const FOracleJoint& OJ = Problem.Joints[J];

			AddInfo(FString::Printf(
				TEXT("JOINT: normal (%.6g, %.6g, %.6g), centreY %.6g, halfU %.6g, halfV %.6g, area %.6g, "
					"cohesion %.6g MPa"),
				OJ.NormalX, OJ.NormalY, OJ.NormalZ, OJ.CentreYCm, OJ.HalfUCm, OJ.HalfVCm, OJ.AreaSqCm,
				OJ.Strength.ShearCohesionMPa));

			/* The out-of-plane normal is LIFTED (posed), not projected onto X-Z. */
			TestTrue(
				*FString::Printf(TEXT("[RED]: NormalY is posed, |%.6g| ~ 1"), OJ.NormalY),
				FMath::IsNearlyEqual(FMath::Abs(OJ.NormalY), 1.0, 1.0e-9));
			TestTrue(
				*FString::Printf(TEXT("[RED]: NormalX ~ 0 (got %.6g)"), OJ.NormalX),
				FMath::IsNearlyZero(OJ.NormalX, 1.0e-9));
			TestTrue(
				*FString::Printf(TEXT("[RED]: NormalZ ~ 0 (got %.6g)"), OJ.NormalZ),
				FMath::IsNearlyZero(OJ.NormalZ, 1.0e-9));

			TestTrue(
				*FString::Printf(TEXT("[RED]: CentreYCm carried, ~ %.6g (got %.6g)"), JointCentreYCm, OJ.CentreYCm),
				Near(OJ.CentreYCm, JointCentreYCm, 1.0e-6));
			TestTrue(
				*FString::Printf(TEXT("[RED]: AreaSqCm carried, ~ %.6g (got %.6g)"), AreaSqCm, OJ.AreaSqCm),
				Near(OJ.AreaSqCm, AreaSqCm, 1.0e-6));

			/*
			 * BOTH real in-plane half-extents, as a SET {10, 15}, so the old |Nz|-vs-|Nx| shortcut —
			 * which could only ever set ONE length — cannot pass. Which of the two the deterministic
			 * axis derivation calls U vs V is the oracle's own (DeriveInPlaneAxes) business and is not
			 * asserted here; the bridge's job is to hand over BOTH real extents.
			 */
			const double HalfMin = FMath::Min(OJ.HalfUCm, OJ.HalfVCm);
			const double HalfMax = FMath::Max(OJ.HalfUCm, OJ.HalfVCm);
			TestTrue(
				*FString::Printf(TEXT("[RED]: the two in-plane half-extents are {%.6g, %.6g} = {10, 15} "
					"(the shortcut kept only one)"), HalfMin, HalfMax),
				Near(HalfMin, FMath::Min(HalfXCm, HalfZCm), 1.0e-6)
					&& Near(HalfMax, FMath::Max(HalfXCm, HalfZCm), 1.0e-6));

			/* The bond is carried through EffectiveJointStrength (single-material here => bare). */
			TestTrue(
				*FString::Printf(TEXT("[RED]: the joint's cohesive bond is carried (%.6g MPa)"),
					OJ.Strength.ShearCohesionMPa),
				OJ.Strength.ShearCohesionMPa > 1.0e6);
		}

		if (FreeBlock != INDEX_NONE)
		{
			const double GotComY = Problem.Blocks[FreeBlock].CentroidYCm;
			TestTrue(
				*FString::Printf(TEXT("[RED]: the free block's plan-Y is carried, CentroidYCm ~ %.6g (got %.6g)"),
					BlockComYCm, GotComY),
				Near(GotComY, BlockComYCm, 1.0e-6));
		}
	}

	/* ================================================================================
	 * POSE 2 — STANDS: a huge bond -> the Y-facing shear joint holds -> Stands.
	 * ================================================================================ */
	{
		FBonded Fx;
		Build(Fx, StandingBond());

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Fx.Structure, Problem, WhyNot);

		FOracleResult R;
		if (bBridged)
		{
			R = SolveRigidBlock(Problem);
		}

		AddInfo(FString::Printf(
			TEXT("STANDS: bridged %d, answered %d, lambda* %.6g, outcome %d (Stands=%d)"),
			bBridged ? 1 : 0, R.bAnswered ? 1 : 0, R.Lambda,
			static_cast<int32>(OutcomeOf(R)), static_cast<int32>(EOracleOutcome::Stands)));

		TestTrue(TEXT("STANDS [RED]: the bridge accepts the 3D structure"), bBridged);
		TestTrue(TEXT("STANDS [RED]: the bonded block STANDS (its shear joint carries the weight)"),
			OutcomeOf(R) == EOracleOutcome::Stands);
	}

	/* ================================================================================
	 * POSE 3 — FALLS: a zero bond -> the Y-facing joint carries nothing -> Falls, and the
	 * collapse mechanism moves the FREE block (descending), not the grounded wall.
	 * ================================================================================ */
	{
		FBonded Fx;
		Build(Fx, FallingBond());

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Fx.Structure, Problem, WhyNot);

		/* ---- The Stands/Falls VERDICT, gravity live (lambda* = 0 => it falls). ---- */
		FOracleResult Verdict;
		if (bBridged)
		{
			Verdict = SolveRigidBlock(Problem);
		}

		AddInfo(FString::Printf(
			TEXT("FALLS: bridged %d, answered %d, lambda* %.6g, outcome %d (Falls=%d)"),
			bBridged ? 1 : 0, Verdict.bAnswered ? 1 : 0, Verdict.Lambda,
			static_cast<int32>(OutcomeOf(Verdict)), static_cast<int32>(EOracleOutcome::Falls)));

		TestTrue(TEXT("FALLS [RED]: the bridge accepts the 3D structure"), bBridged);
		TestTrue(TEXT("FALLS [RED]: the bondless block FALLS (a Y-facing joint carries no -Z weight)"),
			OutcomeOf(Verdict) == EOracleOutcome::Falls);

		/*
		 * ---- The MECHANISM (never displacement): the free block moves and descends. ----
		 *
		 * The mechanism lives on the INFEASIBLE arm of the FEASIBILITY formulation (gravity DEAD), so
		 * pose the same bridged problem with bGravityIsLive = false and read the Farkas dual. The free
		 * block, unheld, descends (VirtualUz < 0 by the fixed global sign convention); the grounded
		 * wall writes no rows and does not move.
		 */
		if (bBridged)
		{
			FOracleProblem Feas = Problem;
			Feas.bGravityIsLive = false;
			const FOracleResult M = SolveRigidBlock(Feas);

			const int32 FreeBlock = OracleBlockOfPiece(Problem, Fx.Block);
			const int32 WallBlock = OracleBlockOfPiece(Problem, Fx.Wall);
			const bool bFreeValid = M.Mechanism.Blocks.IsValidIndex(FreeBlock);
			const bool bWallValid = M.Mechanism.Blocks.IsValidIndex(WallBlock);

			const FOracleMechanismBlock FreeT =
				bFreeValid ? M.Mechanism.Blocks[FreeBlock] : FOracleMechanismBlock();

			AddInfo(FString::Printf(
				TEXT("FALLS MECHANISM: answered %d, present %d, certified %d; free moves %d Uz %.6g; wall moves %d"),
				M.bAnswered ? 1 : 0, M.Mechanism.bPresent ? 1 : 0, M.Mechanism.bIsCertified ? 1 : 0,
				(bFreeValid && M.Mechanism.Blocks[FreeBlock].bMoves) ? 1 : 0, FreeT.VirtualUz,
				(bWallValid && M.Mechanism.Blocks[WallBlock].bMoves) ? 1 : 0));

			TestTrue(TEXT("FALLS MECHANISM [RED]: a mechanism is present and certified"),
				M.Mechanism.bPresent && M.Mechanism.bIsCertified);
			TestTrue(TEXT("FALLS MECHANISM [RED]: the free block moves"),
				bFreeValid && M.Mechanism.Blocks[FreeBlock].bMoves);
			TestTrue(
				*FString::Printf(TEXT("FALLS MECHANISM [RED]: the free block descends, VirtualUz %.6g < 0"),
					FreeT.VirtualUz),
				bFreeValid && FreeT.VirtualUz < 0.0);
			TestFalse(TEXT("FALLS MECHANISM: the grounded wall does not move"),
				bWallValid && M.Mechanism.Blocks[WallBlock].bMoves);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
