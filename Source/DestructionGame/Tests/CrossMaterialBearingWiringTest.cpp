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
 * SHED_PATH.md Phase B / slice B3 — WIRE the connection x material weakest-link pairing
 * into the joint strength path (the cross-material bearing the shed's wooden roof-on-brick
 * head and post-on-footing need).
 *
 * BEHAVIOUR UNDER TEST, in one sentence: for a joint between two DIFFERENT materials, the
 * effective COMPRESSION capacity that both production strength paths consume — the router
 * (FStructure::GetConnectionUtilisation) and the oracle bridge (RigidBlockBridge) — is the
 * weakest-link MATERIAL crush min(connection, matA, matB), NOT the bare connection's own
 * compressive strength.
 *
 * WHY THIS IS THE RED FOR B3. B2 landed DestructionForce::EffectiveBondedStrength as a
 * STANDALONE function (BondFactorWeakestLinkTest calls it directly), but NOTHING in
 * production consults it: the router reads FConnection::Strength through UtilisationUnder,
 * and the bridge copies FConnection::Strength verbatim into the LP row
 * (RigidBlockBridge.cpp `Out.Strength = Joint.Strength`). This test builds a real
 * FStructure whose bed joint bears wood on brick and pins that, once the wiring lands, the
 * joint's compression capacity reflects the material crush — so both readings below fail
 * TODAY because the wiring is absent (the joint reads the bare connection), which is the
 * missing behaviour and not a broken fixture.
 *
 * THE FIXTURE — A TIMBER POST BEARING ON A GROUNDED CLAY-BRICK FOOTING.
 *
 *        +----------+     Post: Timber (f_c,0 = 21 MPa), ungrounded.
 *        |   POST   |     Its whole weight bears straight down onto the joint below.
 *        +==========+  <- BED JOINT, Unbreakable connection (compressive 1e12 MPa):
 *        +----------+     the CONNECTION cannot govern, so the MATERIAL crush must.
 *        | FOOTING  |     Footing: ClayBrick (f_c = 20 MPa), GROUNDED.
 *        +==========+
 *            earth
 *
 * WHY THE CONNECTION IS `Unbreakable`. Its compressive strength (1e12 MPa) sits eleven
 * orders of magnitude above either material, so the bare-connection path reads the joint as
 * essentially incapable of crushing (utilisation ~ 1e-11) — while the wired weakest-link
 * path reads the BRICK's 20 MPa. That gulf is what makes the assertion a clean discriminator
 * rather than a numeric coincidence: the two answers cannot be confused.
 *
 * WHY THE EXPECTED CAP IS 20, NOT 21. The crush is the min over BOTH faces and the
 * connection: min(1e12, timber 21, brick 20) = 20, the weaker of the two MATERIALS. Pinning
 * exactly 20 (the brick) rather than 21 (the timber) is deliberate — it proves the wiring
 * consults BOTH pieces' materials, not just one, so an implementation that paired the joint
 * with only its upper (or only its lower) face would still fail this row.
 *
 * THE LOAD IS A KNOWN, CENTRED, PURE COMPRESSION so that the compression axis unambiguously
 * governs ComputeUtilisation's worst-axis answer. The post's centre of mass sits directly
 * over the joint centre (zero eccentricity -> zero moment), the joint normal is vertical, and
 * the only force is gravity on the post — so shear and tension are exactly zero and the
 * utilisation IS the compression ratio. The post mass is a chosen test scalar (not a realistic
 * timber weight) picked to deliver exactly 10 MPa of bearing stress: 10 MPa is HALF the brick
 * crush, so the wired joint reads 0.5 and STANDS — the assertion is a pure strength READOUT,
 * nothing breaks, and no displacement is ever read (DESIGN.md §4).
 *
 * THE TWO SEAMS, BOTH DRIVEN RED:
 *   1. THE ROUTER — GetConnectionUtilisation(bed) must read stress / material-crush = 0.5.
 *      Today it reads stress / 1e12 ~ 0, because UtilisationUnder consumes the bare
 *      FConnection::Strength.
 *   2. THE BRIDGE — the LP strength row the oracle solves against, Problem.Joints[j].Strength,
 *      must carry the material crush (20 MPa compressive). Today the bridge copies the bare
 *      connection (1e12).
 *
 * Wiring means BOTH consult EffectiveBondedStrength(connection, matA, matB); a fix to only
 * one seam leaves the other red, which is the point.
 *
 * UNITS. The MPa <-> uu conversion (1 N = 100 uu, 1 cm2 = 100 mm2 -> 10000 uu per MPa per
 * cm2) is spelled out from first principles below rather than imported from
 * ForceUnitsPerMPaSqCm, so this test fails if that constant is wrong instead of silently
 * agreeing with it (DESIGN.md §3).
 *
 * NEEDS A TICKING WORLD: NO. Gravity is the ordinary FStructure kind (weight is MassKg x 980
 * inside the solver); every assertion is on solver state or on the bridged LP problem. Same
 * footing as SupportAuthorityBelowCap and the two-load-path acceptance tests.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace CrossMaterialBearingWiringSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Every piece is this deep on Y, so with a 10 cm face length the bed area is 98 cm2. */
	constexpr double WytheWidthCm = 9.8;

	/** The bearing face is 10 cm long on X; with the 9.8 cm wythe that is 98 cm2. */
	constexpr double FaceLengthCm = 10.0;

	/** A 1 cm mortarless contact — the separation the bed joint is formed across. */
	constexpr double JointThicknessCm = 1.0;

	/**
	 * The post mass, kilograms — a chosen test SCALAR, not a realistic timber weight. It is
	 * picked so the bearing stress is exactly 10 MPa: weight = 10000 x 980 = 9.8e6 uu, over
	 * 98 cm2 that is 100000 uu/cm2 = 10 MPa. The fixture isolates the strength path; what the
	 * post would really weigh is irrelevant to which capacity the joint reads.
	 */
	constexpr double PostMassKg = 10000.0;

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	struct FBearing
	{
		FStructure Structure;

		int32 Footing = INDEX_NONE;  // ClayBrick, grounded
		int32 Post = INDEX_NONE;     // Timber, bears on the footing
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
	 * Lay the grounded brick footing and the timber post that bears on it through one bed
	 * joint laid in the Unbreakable connection, and TAG each piece with its material so the
	 * wiring has two faces to pair. The post's centre sits over the joint centre (X = 0) so
	 * the bearing is centred and the joint carries pure compression.
	 */
	void Build(FBearing& Out)
	{
		/* Footing top at Z = 20; post bottom at Z = 21; the 1 cm gap is the joint. */
		const FPieceBox FootBox = MakeBox(/*Z*/ 10.0, /*SizeZ*/ 20.0);
		const FPieceBox PostBox = MakeBox(/*Z*/ 31.0, /*SizeZ*/ 20.0);

		/* Footing mass is irrelevant (grounded -> its weight goes to earth, not through the bed). */
		Out.Footing = Out.Structure.AddPiece(50.0, /*bIsGrounded*/ true, FootBox.CentreCm);
		Out.Post = Out.Structure.AddPiece(PostMassKg, /*bIsGrounded*/ false, PostBox.CentreCm);

		Out.Structure.SetPieceMaterial(Out.Footing, &ClayBrick);
		Out.Structure.SetPieceMaterial(Out.Post, &Timber);

		FConnection Joint;
		if (MakeInterface(Out.Footing, FootBox, Out.Post, PostBox, JointThicknessCm, Unbreakable, Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}
	}
}

/**
 * The joint's compression capacity is the weakest-link material crush, on both the router and
 * the oracle-bridge strength paths.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCrossMaterialBearingWiringTest,
	"DestructionGame.Acceptance.CrossMaterialBearing.JointCompressionCapacityIsTheMaterialCrush",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrossMaterialBearingWiringTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace CrossMaterialBearingWiringSupport;

	/* ------------------------------------------------------------------ *
	 * THE CONVERSION, DERIVED HERE. 1 N = 100 uu, 1 cm2 = 100 mm2, so
	 * 1 MPa (= 1 N/mm2) over 1 cm2 is 100 * 100 = 10000 uu. Independent of
	 * ForceUnitsPerMPaSqCm on purpose.
	 * ------------------------------------------------------------------ */
	constexpr double UuPerMPaSqCm = 100.0 * 100.0;

	/* ------------------------------------------------------------------ *
	 * FIXTURE PRECONDITIONS — the hand-derived numbers only mean what they
	 * say while the profiles carry the strengths they were derived against,
	 * and while the connection genuinely cannot govern.
	 * ------------------------------------------------------------------ */

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the Unbreakable connection's compressive (%g MPa) must dwarf "
			"both materials, so the MATERIAL crush governs"), Unbreakable.CompressiveStrengthMPa),
		Unbreakable.CompressiveStrengthMPa > 1.0e9);

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: timber compressive must be 21 MPa, profile carries %g"),
			Timber.Strength.CompressiveStrengthMPa),
		Timber.Strength.CompressiveStrengthMPa == 21.0);

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: clay brick compressive must be 20 MPa, profile carries %g"),
			ClayBrick.Strength.CompressiveStrengthMPa),
		ClayBrick.Strength.CompressiveStrengthMPa == 20.0);

	/*
	 * The weakest link on the compression (bearing) axis: min over the connection and both
	 * faces. The brick (20) is the weaker MATERIAL, so it governs — NOT the timber (21), and
	 * emphatically not the connection (1e12). Pinning 20 rather than 21 is what forces the
	 * wiring to consult BOTH faces.
	 */
	const double MaterialCrushMPa = FMath::Min3(
		Unbreakable.CompressiveStrengthMPa,
		Timber.Strength.CompressiveStrengthMPa,
		ClayBrick.Strength.CompressiveStrengthMPa);                                   // 20

	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the weakest-link crush must be the brick's 20 MPa, got %g"),
			MaterialCrushMPa),
		MaterialCrushMPa == 20.0);

	/* ------------------------------------------------------------------ *
	 * BUILD, AND CHECK THE TOPOLOGY IS THE ONE CLAIMED.
	 * ------------------------------------------------------------------ */

	FBearing Fx;
	Build(Fx);

	if (Fx.BedJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit the bed joint"));
		return false;
	}

	TestEqual(TEXT("FIXTURE: two pieces — the grounded brick footing and the timber post"),
		Fx.Structure.NumPieces(), 2);
	TestEqual(TEXT("FIXTURE: one joint — the wood-on-brick bed bearing"),
		Fx.Structure.NumConnections(), 1);
	TestTrue(TEXT("FIXTURE: every piece and joint must know where it is, or there are no honest lever arms"),
		Fx.Structure.HasCompleteGeometry());
	TestTrue(TEXT("FIXTURE: the post bears on the footing through a BED joint (a compression bearing)"),
		Fx.Structure.GetJointRole(Fx.BedJoint, Fx.Post) == EJointRole::BedBeneath);

	/* ------------------------------------------------------------------ *
	 * THE KNOWN, CENTRED, PURE-COMPRESSION LOAD. Solve, then confirm the bed
	 * joint carries the post's whole weight vertically, with no horizontal
	 * component — so the compression axis is what ComputeUtilisation reads.
	 * ------------------------------------------------------------------ */

	Fx.Structure.SolveLoads();

	const FVector BedForce = Fx.Structure.GetConnectionForce(Fx.BedJoint);
	const double PostWeightUu = PostMassKg * GravityCmPerSecondSquared;               // 9.8e6

	TestTrue(
		FString::Printf(TEXT("FIXTURE: the bed joint carries the post's whole weight |%g| == %g uu"),
			BedForce.Size(), PostWeightUu),
		FMath::IsNearlyEqual(BedForce.Size(), PostWeightUu, 1.0));
	TestTrue(
		FString::Printf(TEXT("FIXTURE: the bearing force must be purely vertical (X %g, Y %g both ~0), "
			"so the load is pure compression"), BedForce.X, BedForce.Y),
		FMath::IsNearlyZero(BedForce.X, 1.0e-6) && FMath::IsNearlyZero(BedForce.Y, 1.0e-6));

	/* The bearing stress, worked through independently: 9.8e6 uu / 98 cm2 / 10000 = 10 MPa. */
	const double BedAreaSqCm = FaceLengthCm * WytheWidthCm;                           // 98
	const double BearingStressMPa = PostWeightUu / BedAreaSqCm / UuPerMPaSqCm;        // 10

	TestTrue(
		FString::Printf(TEXT("FIXTURE: the bearing stress must be exactly 10 MPa, worked out to %g"),
			BearingStressMPa),
		FMath::IsNearlyEqual(BearingStressMPa, 10.0, 1.0e-9));

	/*
	 * 10 MPa is HALF the brick's 20 MPa crush, so the WIRED joint reads 0.5 and stands — the
	 * assertion is a pure readout, nothing breaks. The BARE connection reads 10 / 1e12 ~ 1e-11.
	 */
	const double ExpectedWiredUtilisation = BearingStressMPa / MaterialCrushMPa;      // 0.5
	const double BareUtilisation = BearingStressMPa / Unbreakable.CompressiveStrengthMPa; // ~1e-11

	/* ------------------------------------------------------------------ *
	 * SEAM 1, THE ROUTER — GetConnectionUtilisation must read the material
	 * crush. Today it consumes the bare FConnection::Strength (1e12) and
	 * reads ~0; the wiring makes it consult EffectiveBondedStrength(bed's
	 * connection, timber, brick) so it reads 0.5.
	 * ------------------------------------------------------------------ */

	const double RouterUtilisation = Fx.Structure.GetConnectionUtilisation(Fx.BedJoint);

	AddInfo(FString::Printf(
		TEXT("ROUTER: GetConnectionUtilisation = %.12g. Wired (material 20 MPa) expects %.12g; "
			"bare connection (1e12 MPa) gives %.3g"),
		RouterUtilisation, ExpectedWiredUtilisation, BareUtilisation));

	TestTrue(
		FString::Printf(TEXT("SEAM 1 (ROUTER), THE RED: the joint's compression utilisation must reflect "
			"the material crush (0.5), got %.12g. Today GetConnectionUtilisation reads the bare "
			"connection (~%.3g) because the wiring is absent"),
			RouterUtilisation, BareUtilisation),
		FMath::IsNearlyEqual(RouterUtilisation, ExpectedWiredUtilisation, 1.0e-9));

	/* ------------------------------------------------------------------ *
	 * SEAM 2, THE ORACLE BRIDGE — the LP strength row the oracle solves
	 * against must carry the material crush (20 MPa compressive). Today the
	 * bridge copies the bare connection (1e12) verbatim.
	 * ------------------------------------------------------------------ */

	RigidBlockOracle::FOracleProblem Problem;
	FString BridgeWhy;
	const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Fx.Structure, Problem, BridgeWhy);

	TestTrue(
		*FString::Printf(TEXT("CROSS-CHECK: the oracle bridge must accept this 2D structure (%s)"), *BridgeWhy),
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

		TestTrue(TEXT("CROSS-CHECK: the bridged problem must carry the bed joint"),
			OracleJoint != INDEX_NONE);

		if (OracleJoint != INDEX_NONE)
		{
			const double LpCompressiveMPa =
				Problem.Joints[OracleJoint].Strength.CompressiveStrengthMPa;

			AddInfo(FString::Printf(
				TEXT("BRIDGE: LP row compressive strength = %.6g MPa. Wired expects %.6g (material crush); "
					"bare connection copies %.3g"),
				LpCompressiveMPa, MaterialCrushMPa, Unbreakable.CompressiveStrengthMPa));

			TestTrue(
				FString::Printf(TEXT("SEAM 2 (BRIDGE), THE RED: the LP strength row must carry the material "
					"crush (%g MPa), got %.6g. Today the bridge copies the bare connection (%.3g) so the "
					"LP would never crush this bearing"),
					MaterialCrushMPa, LpCompressiveMPa, Unbreakable.CompressiveStrengthMPa),
				FMath::IsNearlyEqual(LpCompressiveMPa, MaterialCrushMPa, 1.0e-6));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
