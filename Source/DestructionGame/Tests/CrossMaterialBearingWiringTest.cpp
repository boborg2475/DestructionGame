// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SHED_PATH.md slice B3: a joint between two different materials has a compression capacity of
 * min(connection, matA, matB) (EffectiveBondedStrength), on both the router
 * (GetConnectionUtilisation) and the oracle bridge's LP strength row.
 *
 *        +----------+     Post: Timber (f_c,0 mean = 29 MPa), ungrounded.
 *        |   POST   |
 *        +==========+  <- bed joint, Unbreakable connection (1e12 MPa), so the material governs
 *        +----------+
 *        | FOOTING  |     Footing: ClayBrick (f_c = 20 MPa), grounded.
 *        +==========+
 *            earth
 *
 * The Unbreakable connection makes the bare-connection answer ~1e-11, far from the wired 0.5. The
 * expected crush is 20 (brick), not 29, so the wiring must read both faces. A mirrored case (brick
 * post on timber footing) puts the weaker material on the other face, so any single-face
 * implementation fails one of the two rows (it would read 29, util 0.345).
 *
 * The load is centred pure compression: the post mass is chosen for exactly 10 MPa bearing, half
 * the crush, so the joint reads 0.5 and nothing breaks. Units are derived here (10000 uu per MPa
 * per cm2), not imported from ForceUnitsPerMPaSqCm (DESIGN.md §3). No ticking world. Named
 * namespace because unity builds merge anonymous ones.
 */
namespace CrossMaterialBearingWiringSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Piece depth on Y; with the 10 cm face the bed is 98 cm2. */
	constexpr double WytheWidthCm = 9.8;

	constexpr double FaceLengthCm = 10.0;

	constexpr double JointThicknessCm = 1.0;

	/**
	 * Post mass, kg: a test scalar, not a realistic weight. 10000 x 980 = 9.8e6 uu over 98 cm2 is
	 * exactly 10 MPa.
	 */
	constexpr double PostMassKg = 10000.0;

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	struct FBearing
	{
		FStructure Structure;

		int32 Footing = INDEX_NONE;  // grounded footing (material chosen per case)
		int32 Post = INDEX_NONE;     // bears on the footing (material chosen per case)
		int32 BedJoint = INDEX_NONE; // Post - Footing, Unbreakable connection
	};

	FPieceBox MakeBox(double CentreZ, double SizeZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(FaceLengthCm, WytheWidthCm, SizeZ) * 0.5;
		Box.CentreCm = FVector(0.0, 0.0, CentreZ);
		return Box;
	}

	/**
	 * Lay a grounded footing and a centred post joined by an Unbreakable bed joint, each tagged with
	 * the given material.
	 */
	void Build(
		FBearing& Out,
		const FMaterialProfile& FootingMaterial,
		const FMaterialProfile& PostMaterial)
	{
		// Footing top at Z = 20; post bottom at Z = 21; the 1 cm gap is the joint.
		const FPieceBox FootBox = MakeBox(/*Z*/ 10.0, /*SizeZ*/ 20.0);
		const FPieceBox PostBox = MakeBox(/*Z*/ 31.0, /*SizeZ*/ 20.0);

		// Footing mass is irrelevant: it is grounded.
		Out.Footing = Out.Structure.AddPiece(50.0, /*bIsGrounded*/ true, FootBox.CentreCm);
		Out.Post = Out.Structure.AddPiece(PostMassKg, /*bIsGrounded*/ false, PostBox.CentreCm);

		Out.Structure.SetPieceMaterial(Out.Footing, &FootingMaterial);
		Out.Structure.SetPieceMaterial(Out.Post, &PostMaterial);

		FConnection Joint;
		if (MakeInterface(Out.Footing, FootBox, Out.Post, PostBox, JointThicknessCm, Unbreakable, Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}
	}
}

/** The joint's compression capacity is the weakest-link material crush on both strength paths. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrossMaterialBearingWiringTest,
	"DestructionGame.Acceptance.CrossMaterialBearing.JointCompressionCapacityIsTheMaterialCrush",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrossMaterialBearingWiringTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace CrossMaterialBearingWiringSupport;

	// 1 MPa over 1 cm2 is 100 * 100 uu. Deliberately independent of ForceUnitsPerMPaSqCm.
	constexpr double UuPerMPaSqCm = 100.0 * 100.0;

	// Preconditions: the profiles carry the strengths the numbers were derived from.
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the Unbreakable connection's compressive (%g MPa) must dwarf "
			"both materials, so the MATERIAL crush governs"), Unbreakable.CompressiveStrengthMPa),
		Unbreakable.CompressiveStrengthMPa > 1.0e9);

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: timber compressive must be 29 MPa (C24 mean f_c,0), profile carries %g"),
			Timber.Strength.CompressiveStrengthMPa),
		Timber.Strength.CompressiveStrengthMPa == 29.0);

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: clay brick compressive must be 20 MPa, profile carries %g"),
			ClayBrick.Strength.CompressiveStrengthMPa),
		ClayBrick.Strength.CompressiveStrengthMPa == 20.0);

	// Weakest-link crush: the brick's 20, not timber's 29 or the connection's 1e12.
	const double MaterialCrushMPa = FMath::Min3(
		Unbreakable.CompressiveStrengthMPa,
		Timber.Strength.CompressiveStrengthMPa,
		ClayBrick.Strength.CompressiveStrengthMPa);                                   // 20

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the weakest-link crush must be the brick's 20 MPa, got %g"),
			MaterialCrushMPa),
		MaterialCrushMPa == 20.0);

	// One case, run twice with the weaker material on each face in turn.
	auto RunBearingCase =
		[&](const FMaterialProfile& FootingMaterial,
			const FMaterialProfile& PostMaterial,
			const TCHAR* Label) -> void
	{
		FBearing Fx;
		Build(Fx, FootingMaterial, PostMaterial);

		if (Fx.BedJoint == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("[%s] FIXTURE: the producer must emit the bed joint"), Label));
			return;
		}

		TestEqual(*FString::Printf(TEXT("[%s] FIXTURE: two pieces — the grounded footing and the post"), Label),
			Fx.Structure.NumPieces(), 2);
		TestEqual(*FString::Printf(TEXT("[%s] FIXTURE: one joint — the cross-material bed bearing"), Label),
			Fx.Structure.NumConnections(), 1);
		TestTrue(*FString::Printf(TEXT("[%s] FIXTURE: every piece and joint must know where it is, or there "
			"are no honest lever arms"), Label),
			Fx.Structure.HasCompleteGeometry());
		TestTrue(*FString::Printf(TEXT("[%s] FIXTURE: the post bears on the footing through a BED joint "
			"(a compression bearing)"), Label),
			Fx.Structure.GetJointRole(Fx.BedJoint, Fx.Post) == EJointRole::BedBeneath);

		// Recomputed from the faces actually laid; 20 either way.
		const double CaseCrushMPa = FMath::Min3(
			Unbreakable.CompressiveStrengthMPa,
			FootingMaterial.Strength.CompressiveStrengthMPa,
			PostMaterial.Strength.CompressiveStrengthMPa);

		TestTrue(*FString::Printf(TEXT("[%s] FIXTURE: the weakest-link crush must be the brick's 20 MPa "
			"(the weaker material, whichever face it is on), got %g"), Label, CaseCrushMPa),
			CaseCrushMPa == 20.0);

		Fx.Structure.SolveLoads();

		const FVector BedForce = Fx.Structure.GetConnectionForce(Fx.BedJoint);
		const double PostWeightUu = PostMassKg * GravityCmPerSecondSquared;               // 9.8e6

		TestTrue(
			*FString::Printf(TEXT("[%s] FIXTURE: the bed joint carries the post's whole weight |%g| == %g uu"),
				Label, BedForce.Size(), PostWeightUu),
			FMath::IsNearlyEqual(BedForce.Size(), PostWeightUu, 1.0));
		TestTrue(
			*FString::Printf(TEXT("[%s] FIXTURE: the bearing force must be purely vertical (X %g, Y %g both "
				"~0), so the load is pure compression"), Label, BedForce.X, BedForce.Y),
			FMath::IsNearlyZero(BedForce.X, 1.0e-6) && FMath::IsNearlyZero(BedForce.Y, 1.0e-6));

		// 9.8e6 uu / 98 cm2 / 10000 = 10 MPa.
		const double BedAreaSqCm = FaceLengthCm * WytheWidthCm;                           // 98
		const double BearingStressMPa = PostWeightUu / BedAreaSqCm / UuPerMPaSqCm;        // 10

		TestTrue(
			*FString::Printf(TEXT("[%s] FIXTURE: the bearing stress must be exactly 10 MPa, worked out to %g"),
				Label, BearingStressMPa),
			FMath::IsNearlyEqual(BearingStressMPa, 10.0, 1.0e-9));

		// Wired: 10 / 20 = 0.5. Bare connection: ~1e-11. Single-face bug: 10 / 29 ~ 0.345.
		const double ExpectedWiredUtilisation = BearingStressMPa / CaseCrushMPa;          // 0.5
		const double BareUtilisation = BearingStressMPa / Unbreakable.CompressiveStrengthMPa; // ~1e-11

		// Seam 1: the router.
		const double RouterUtilisation = Fx.Structure.GetConnectionUtilisation(Fx.BedJoint);

		AddInfo(FString::Printf(
			TEXT("[%s] ROUTER: GetConnectionUtilisation = %.12g. Wired (material 20 MPa) expects %.12g; "
				"bare connection (1e12 MPa) gives %.3g"),
			Label, RouterUtilisation, ExpectedWiredUtilisation, BareUtilisation));

		TestTrue(
			*FString::Printf(TEXT("[%s] SEAM 1 (ROUTER): the joint's compression utilisation must reflect the "
				"weakest-link crush (0.5), got %.12g. A bare connection reads ~%.3g; a single-face bug that "
				"ignored the brick face would read ~0.345"),
				Label, RouterUtilisation, BareUtilisation),
			FMath::IsNearlyEqual(RouterUtilisation, ExpectedWiredUtilisation, 1.0e-9));

		// Seam 2: the oracle bridge's LP strength row.
		RigidBlockOracle::FOracleProblem Problem;
		FString BridgeWhy;
		const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Fx.Structure, Problem, BridgeWhy);

		TestTrue(
			*FString::Printf(TEXT("[%s] CROSS-CHECK: the oracle bridge must accept this 2D structure (%s)"),
				Label, *BridgeWhy),
			bBridged);

		if (bBridged)
		{
			int32 OracleJoint = INDEX_NONE;
			for (int32 J = 0; J < Problem.ConnectionOfJoint.Num(); ++J)
			{
				if (Problem.ConnectionOfJoint[J] == Fx.BedJoint)
				{
					OracleJoint = J;
					break;
				}
			}

			TestTrue(*FString::Printf(TEXT("[%s] CROSS-CHECK: the bridged problem must carry the bed joint"), Label),
				OracleJoint != INDEX_NONE);

			if (OracleJoint != INDEX_NONE)
			{
				const double LpCompressiveMPa =
					Problem.Joints[OracleJoint].Strength.CompressiveStrengthMPa;

				AddInfo(FString::Printf(
					TEXT("[%s] BRIDGE: LP row compressive strength = %.6g MPa. Wired expects %.6g (material "
						"crush); bare connection copies %.3g"),
					Label, LpCompressiveMPa, CaseCrushMPa, Unbreakable.CompressiveStrengthMPa));

				TestTrue(
					*FString::Printf(TEXT("[%s] SEAM 2 (BRIDGE): the LP strength row must carry the weakest-link "
						"crush (%g MPa), got %.6g. A single-face bug that ignored the brick face would copy the "
						"stronger face (%.3g)"),
						Label, CaseCrushMPa, LpCompressiveMPa,
						FMath::Max(FootingMaterial.Strength.CompressiveStrengthMPa,
							PostMaterial.Strength.CompressiveStrengthMPa)),
					FMath::IsNearlyEqual(LpCompressiveMPa, CaseCrushMPa, 1.0e-6));
			}
		}
	};

	// Case A: weaker material on the lower face; catches an upper-face-only bug.
	RunBearingCase(/*Footing*/ ClayBrick, /*Post*/ Timber, TEXT("weaker=lower (timber-on-brick)"));

	// Case B: weaker material on the upper face; catches a lower-face-only bug.
	RunBearingCase(/*Footing*/ Timber, /*Post*/ ClayBrick, TEXT("weaker=upper (brick-on-timber)"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
