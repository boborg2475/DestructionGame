// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E3: the 3D bridge (THREED_DESIGN.md, E1 slice sequence). Written red against a 2D-only
 * RigidBlockBridge that refused any Y normal, kept only one in-plane half-extent, dropped the
 * block's CentroidYCm and never set Dim3D. FStructure already holds the 3D geometry; the only
 * addition is SetThreeDimensional, the flag the bridge keys its 3D pose on. A 2D structure's
 * Y normal must still be refused.
 *
 * Fixture: a free block bonded to a grounded wall across a vertical joint whose normal is +Y.
 *
 *                          Z
 *                          ^        Wall (grounded)      Block (free, weight W = -Z)
 *                          |      +-----------+ | +-----------+
 *                          |      |           | | |           |
 *                          |      |   WALL    |=|=|  BLOCK    |   the joint is the vertical
 *                          |      |           | | |           |   face between them, normal +Y
 *                          |      +-----------+ | +-----------+
 *                          +----------------------------------> Y
 *
 *   Wall: grounded, centre (0, 0, 15), half-extents (10, 5, 15).
 *   Block: free, centre (0, 11, 15), same half-extents. 1 cm gap on Y.
 *   Joint: normal +Y, 600 cm2, half-extents X 10 and Z 15 (unequal on purpose), centre (0, 5.5, 15).
 *
 * Gravity is perpendicular to the normal, so the joint carries it only in shear.
 *
 * Asserted:
 * - Geometry: bridge accepts, Dim3D, |NormalY| ~ 1, CentreYCm 5.5, area 600, half-extents {10, 15}
 *   as a set, block CentroidYCm 11, bond carried.
 * - Stands: an enormous bond stands.
 * - Falls: a zero bond (no friction, cohesion or tension) falls, and the mechanism moves the free
 *   block downward, not the wall. Mechanism, never displacement (DESIGN.md §4).
 * The two bonds bracket the verdict, so the k=8 friction pyramid's facet/vertex orientation never
 * matters.
 *
 * Units are derived here, not imported, though the binary verdicts never cross the unit boundary.
 * No world needed. Named namespace for unity builds.
 */
namespace ThreeDBridgeSupport
{
	using namespace DestructionLayout;
	using namespace RigidBlockOracle;

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double MassKg = 10.0;              // W = 9800 uu
	constexpr double JointThicknessCm = 1.0;     // the Y gap the joint spans

	// The joint face, from the boxes below.
	constexpr double HalfXCm = 10.0;
	constexpr double HalfZCm = 15.0;             // deliberately != HalfXCm
	constexpr double AreaSqCm = 4.0 * HalfXCm * HalfZCm;   // 20 x 30 = 600
	constexpr double JointCentreYCm = 5.5;       // midpoint of the gap between Y=5 and Y=6
	constexpr double JointCentreXCm = 0.0;
	constexpr double JointCentreZCm = 15.0;
	constexpr double BlockComYCm = 11.0;         // free block centre Y

	struct FBonded
	{
		FStructure Structure;
		int32 Wall = INDEX_NONE;   // grounded
		int32 Block = INDEX_NONE;  // free
		int32 Joint = INDEX_NONE;  // Wall -> Block, normal +Y
	};

	FPieceBox Box(double CentreYCm)
	{
		FPieceBox B;
		B.ExtentCm = FVector(HalfXCm, 5.0, HalfZCm);
		B.CentreCm = FVector(0.0, CentreYCm, JointCentreZCm);
		return B;
	}

	/** Enormous on every axis, so the joint holds whatever the friction pyramid's orientation. */
	FConnectionStrength StandingBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1.0e9;
		S.TensileStrengthMPa = 1.0e9;
		S.ShearCohesionMPa = 1.0e9;
		S.FrictionCoefficient = 1.0e9;
		return S;
	}

	/** c = mu = f_t = 0: the joint carries no shear or tension, so the block has no equilibrium. */
	FConnectionStrength FallingBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1.0e9;   // irrelevant: the load is pure shear
		S.TensileStrengthMPa = 0.0;
		S.ShearCohesionMPa = 0.0;
		S.FrictionCoefficient = 0.0;
		return S;
	}

	/** Lay the wall and the block bonded to its +Y face. */
	void Build(FBonded& Out, const FConnectionStrength& Bond)
	{
		const FPieceBox WallBox = Box(0.0);
		const FPieceBox BlockBox = Box(BlockComYCm);

		// Wall mass is irrelevant: it is grounded.
		Out.Wall = Out.Structure.AddPiece(50.0, /*bIsGrounded*/ true, WallBox.CentreCm);
		Out.Block = Out.Structure.AddPiece(MassKg, /*bIsGrounded*/ false, BlockBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Wall, WallBox, Out.Block, BlockBox, JointThicknessCm, Bond, Joint))
		{
			Out.Joint = Out.Structure.AddConnection(Joint);
		}

		// Pose this structure in 3D.
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

/** The 3D bridge poses a Y-facing joint (E3). See the file header. No world needed. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FThreeDBridgeTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.BridgePosesAnOutOfPlaneJoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FThreeDBridgeTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace ThreeDBridgeSupport;

	/*
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. Not imported from
	 * ForceUnitsPerMPaSqCm. Unused (the solve is binary), kept for the record.
	 */
	constexpr double UuPerMPaSqCm = 100.0 * 100.0;
	(void)UuPerMPaSqCm;

	// Pose 1, geometry: the bridge accepts the 3D structure and carries the real joint.
	{
		FBonded Fx;
		Build(Fx, StandingBond());

		// Fixture preconditions.
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

			// The Y normal is posed, not projected onto X-Z.
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

			// Both half-extents as a set; which is U vs V is DeriveInPlaneAxes' business.
			const double HalfMin = FMath::Min(OJ.HalfUCm, OJ.HalfVCm);
			const double HalfMax = FMath::Max(OJ.HalfUCm, OJ.HalfVCm);
			TestTrue(
				*FString::Printf(TEXT("[RED]: the two in-plane half-extents are {%.6g, %.6g} = {10, 15} "
					"(the shortcut kept only one)"), HalfMin, HalfMax),
				Near(HalfMin, FMath::Min(HalfXCm, HalfZCm), 1.0e-6)
					&& Near(HalfMax, FMath::Max(HalfXCm, HalfZCm), 1.0e-6));

			// Via EffectiveJointStrength; single-material here, so the bare bond.
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

	// Pose 2: an enormous bond stands.
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

	// Pose 3: a zero bond falls, and the mechanism moves the free block down, not the wall.
	{
		FBonded Fx;
		Build(Fx, FallingBond());

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Fx.Structure, Problem, WhyNot);

		// Verdict with gravity live (lambda* = 0 means it falls).
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
		 * The mechanism comes from the feasibility formulation (bGravityIsLive = false) via the
		 * Farkas dual. The free block descends (VirtualUz < 0); the grounded wall does not move.
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
