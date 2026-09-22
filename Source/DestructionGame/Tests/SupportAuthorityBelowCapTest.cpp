// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * GetPieceSupport is LP-authoritative below the block cap (Slice 3b; PROMOTION_DESIGN.md §12 D7,
 * §3.7), a miniature of catalogue rows 10 and 19.
 *
 * The router floods load down a support graph; a piece whose load loops back has no path. A
 * possibly real loop is Stranded (DESIGN.md §5.1), but a refused one-sided arch is Falling under
 * ruling (b) (DESIGN §8, case-21 scope limit). The LP asks whether any admissible equilibrium
 * exists, so it stands structures the router cannot route (rows 10/19, λ* 111.5 / 48.0).
 *
 * Below the cap an LP-carried piece reads Supported/Grounded. Above it (the fail-closed boundary,
 * §12 D2⁗) the router's answer governs. The test runs the same structure at two injected caps
 * (SetEquilibriumGateBlockCap).
 *
 * Fixture: an opening at a free end, abutment on one side only (row 10's shape).
 *
 *        L      R          A (abutment / "wall body")
 *      +----+ +----+   +----------+
 *      | L  | | R  |###|    A     |     ### = 1 cm head joints
 *      +----+ +----+   +----------+     (vertical faces, normal along X)
 *        void   void   +----------+
 *                      |    B0    |     B0 grounded; A bears on it (bed joint)
 *                      +==========+
 *                          earth
 *
 * B0 is grounded and A is seated on it by a bed joint. R and L hang over a void, joined only by
 * head joints, so both are seatless.
 *
 * The router falls R and L: head-joint fallback gives the cycle R -> L -> R, and
 * ReseatSpannedGroups needs seated abutments on both sides. The LP carries them: mortar cohesion
 * over the 66.6 cm² head face is ~483x each brick's weight (checked via the oracle as a
 * precondition).
 *
 * Asserts support state only (DESIGN.md §4); no joint breaks in either arm. The same brick reads
 * Falling above the cap and Supported below it. No world. Named namespace: unity builds.
 */
namespace SupportAuthorityBelowCapSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Lengths in cm (1 uu = 1 cm).

	/** Fired clay, 1.9 g/cm3. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Single-wythe depth on Y, shared by every piece. */
	constexpr double WytheWidthCm = 10.25;

	/** Mortar joint thickness. */
	constexpr double JointThicknessCm = 1.0;

	/** MassKg * 980 is a weight in uu (includes 1 N = 100 uu). */
	constexpr double GravityCmPerSecondSquared = 980.0;

	struct FFreeEndOpening
	{
		FStructure Structure;

		int32 Base = INDEX_NONE;      // B0, grounded
		int32 Abutment = INDEX_NONE;  // A, seated on B0
		int32 Inner = INDEX_NONE;     // R, seatless, head-joined to A
		int32 Outer = INDEX_NONE;     // L, seatless, head-joined to R (the free end)

		int32 BedJoint = INDEX_NONE;      // A - B0
		int32 InnerHeadJoint = INDEX_NONE; // R - A
		int32 OuterHeadJoint = INDEX_NONE; // L - R
	};

	FPieceBox MakeBox(double CentreX, double SizeX, double CentreZ, double SizeZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(SizeX, WytheWidthCm, SizeZ) * 0.5;
		Box.CentreCm = FVector(CentreX, 0.0, CentreZ);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/** Lay four pieces and three joints: one bed (A on B0), two head (R↔A, L↔R). */
	void Build(FFreeEndOpening& Out)
	{
		// B0: grounded, only under A.
		const FPieceBox BaseBox = MakeBox(/*X*/ 100.0, /*SizeX*/ 24.0, /*Z*/ 10.0, /*SizeZ*/ 20.0);

		// A: the abutment, seated one joint above B0.
		const FPieceBox AbutBox = MakeBox(/*X*/ 100.0, /*SizeX*/ 20.0, /*Z*/ 31.0, /*SizeZ*/ 20.0);

		// R: seatless, one head joint left of A.
		const FPieceBox InnerBox = MakeBox(/*X*/ 84.0, /*SizeX*/ 10.0, /*Z*/ 34.0, /*SizeZ*/ 6.5);

		// L: the free end, one head joint left of R.
		const FPieceBox OuterBox = MakeBox(/*X*/ 73.0, /*SizeX*/ 10.0, /*Z*/ 34.0, /*SizeZ*/ 6.5);

		Out.Base = Out.Structure.AddPiece(BoxMassKg(BaseBox), /*bIsGrounded*/ true, BaseBox.CentreCm);
		Out.Abutment = Out.Structure.AddPiece(BoxMassKg(AbutBox), /*bIsGrounded*/ false, AbutBox.CentreCm);
		Out.Inner = Out.Structure.AddPiece(BoxMassKg(InnerBox), /*bIsGrounded*/ false, InnerBox.CentreCm);
		Out.Outer = Out.Structure.AddPiece(BoxMassKg(OuterBox), /*bIsGrounded*/ false, OuterBox.CentreCm);

		FConnection Joint;

		if (MakeInterface(Out.Base, BaseBox, Out.Abutment, AbutBox, JointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}

		if (MakeInterface(Out.Inner, InnerBox, Out.Abutment, AbutBox, JointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.InnerHeadJoint = Out.Structure.AddConnection(Joint);
		}

		if (MakeInterface(Out.Outer, OuterBox, Out.Inner, InnerBox, JointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.OuterHeadJoint = Out.Structure.AddConnection(Joint);
		}
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 Stranded = 0;
		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (!S.IsPieceRemoved(Piece) && S.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}
		return Stranded;
	}

	int32 FallingCount(const FStructure& S)
	{
		int32 Falling = 0;
		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (!S.IsPieceRemoved(Piece) && S.GetPieceSupport(Piece) == EPieceSupport::Falling)
			{
				++Falling;
			}
		}
		return Falling;
	}

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	/** Whether any joint has a break stamp. */
	bool AnyJointBrokeUnderLoad(const FStructure& S)
	{
		for (int32 Joint = 0; Joint < S.NumConnections(); ++Joint)
		{
			if (S.GetBreakPass(Joint) != INDEX_NONE)
			{
				return true;
			}
		}
		return false;
	}
}

/** GetPieceSupport reads the LP verdict below the cap and the router's above it. See the file header. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSupportAuthorityBelowCapTest,
	"DestructionGame.Acceptance.SupportAuthority.GetPieceSupportIsLPAuthoritativeBelowTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSupportAuthorityBelowCapTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace SupportAuthorityBelowCapSupport;

	// The LP carries seatless bricks in shear only if the mortar has cohesion.

	TestEqual(TEXT("FIXTURE: the joints are the mean-basis 0.90 MPa cohesion mortar"),
		GeneralPurposeMortar.ShearCohesionMPa, 0.9);

	TestEqual(TEXT("FIXTURE: the joints are the mean-basis 0.70 MPa flexural bond mortar"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.7);

	// Check the topology: four pieces, three joints, R and L held only by head joints.

	FFreeEndOpening Probe;
	Build(Probe);

	if (Probe.BedJoint == INDEX_NONE || Probe.InnerHeadJoint == INDEX_NONE
		|| Probe.OuterHeadJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit the bed joint and both head joints"));
		return false;
	}

	TestEqual(TEXT("FIXTURE: four pieces — grounded base, abutment, and two seatless bricks"),
		Probe.Structure.NumPieces(), 4);

	TestEqual(TEXT("FIXTURE: three joints — one bed (A on B0) and two head (R-A, L-R)"),
		Probe.Structure.NumConnections(), 3);

	TestTrue(TEXT("FIXTURE: every piece and joint must know where it is, or there is no support graph"),
		Probe.Structure.HasCompleteGeometry());

	TestTrue(TEXT("FIXTURE: A bears on B0 through a BED joint, so A is genuinely seated (no head fallback)"),
		Probe.Structure.GetJointRole(Probe.BedJoint, Probe.Abutment) == EJointRole::BedBeneath);

	TestTrue(TEXT("FIXTURE: R is held to the abutment by a HEAD joint (a vertical face, carried in shear)"),
		Probe.Structure.GetJointRole(Probe.InnerHeadJoint, Probe.Inner) == EJointRole::Head);

	TestTrue(TEXT("FIXTURE: L (the free end) is held to R by a HEAD joint — the two-node cycle's other edge"),
		Probe.Structure.GetJointRole(Probe.OuterHeadJoint, Probe.Outer) == EJointRole::Head);

	// Cross-check: the LP must stand the fixture at self-weight (λ* ≥ 1).

	RigidBlockOracle::FOracleProblem Problem;
	FString BridgeWhy;

	const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Probe.Structure, Problem, BridgeWhy);

	TestTrue(
		*FString::Printf(TEXT("CROSS-CHECK: the oracle bridge must accept this 2D structure (%s)"), *BridgeWhy),
		bBridged);

	if (bBridged)
	{
		Problem.bGravityIsLive = false;

		const RigidBlockOracle::FOracleResult Oracle = RigidBlockOracle::SolveRigidBlock(Problem);

		AddInfo(FString::Printf(
			TEXT("CROSS-CHECK: oracle answered %d, lambda* %.10g, %d pivots — Stands means lambda* >= 1"),
			Oracle.bAnswered ? 1 : 0, Oracle.Lambda, Oracle.SimplexIterations));

		TestTrue(TEXT("CROSS-CHECK: the oracle must ANSWER this fixture (a refusal cannot license the flip)"),
			Oracle.bAnswered);

		TestEqual(
			TEXT("CROSS-CHECK: the LP must find an admissible equilibrium at self-weight (Stands) — this "
				 "is the equilibrium the router cannot route to and the LP authority must report"),
			static_cast<int32>(RigidBlockOracle::OutcomeOf(Oracle)),
			static_cast<int32>(RigidBlockOracle::EOracleOutcome::Stands));

		TestTrue(
			*FString::Printf(TEXT("CROSS-CHECK: lambda* %.10g must sit clearly above 1"), Oracle.Lambda),
			Oracle.bAnswered && Oracle.Lambda > 1.0);
	}

	// Settle twice on fresh builds (SolveAndBreak stamps), changing only the cap.

	struct FRun
	{
		int32 Passes = 0;
		int32 Stranded = 0;
		int32 Falling = 0;
		bool bAnyBroke = false;
		EPieceSupport Inner = EPieceSupport::Falling;
		EPieceSupport Outer = EPieceSupport::Falling;
		EPieceSupport Abutment = EPieceSupport::Falling;
		EPieceSupport Base = EPieceSupport::Falling;
	};

	auto RunAtCap = [](int32 Cap) -> FRun
	{
		FFreeEndOpening Fx;
		Build(Fx);
		Fx.Structure.SetEquilibriumGateBlockCap(Cap);

		FRun R;
		R.Passes = Fx.Structure.SolveAndBreak();
		R.Stranded = StrandedCount(Fx.Structure);
		R.Falling = FallingCount(Fx.Structure);
		R.bAnyBroke = AnyJointBrokeUnderLoad(Fx.Structure);
		R.Inner = Fx.Structure.GetPieceSupport(Fx.Inner);
		R.Outer = Fx.Structure.GetPieceSupport(Fx.Outer);
		R.Abutment = Fx.Structure.GetPieceSupport(Fx.Abutment);
		R.Base = Fx.Structure.GetPieceSupport(Fx.Base);
		return R;
	};

	// Within the cap the LP is the authority.
	constexpr int32 AuthoritativeCap = 8;
	const FRun Auth = RunAtCap(AuthoritativeCap);

	// Over the cap the gate declines and the router falls the one-sided arch.
	constexpr int32 DeclineCap = 2;
	const FRun Decline = RunAtCap(DeclineCap);

	AddInfo(FString::Printf(
		TEXT("CAP=%d (>=4, LP-authoritative): passes %d, stranded %d, anyBroke %d, R %d, L %d, A %d, B0 %d. "
			 "CAP=%d (<4, declines to router): passes %d, stranded %d, anyBroke %d, R %d, L %d, A %d, B0 %d. "
			 "(support 1=Grounded,2=Supported,3=Stranded,0=Falling)"),
		AuthoritativeCap, Auth.Passes, Auth.Stranded, Auth.bAnyBroke ? 1 : 0,
		static_cast<int32>(Auth.Inner), static_cast<int32>(Auth.Outer),
		static_cast<int32>(Auth.Abutment), static_cast<int32>(Auth.Base),
		DeclineCap, Decline.Passes, Decline.Stranded, Decline.bAnyBroke ? 1 : 0,
		static_cast<int32>(Decline.Inner), static_cast<int32>(Decline.Outer),
		static_cast<int32>(Decline.Abutment), static_cast<int32>(Decline.Base)));

	// No joint breaks in either arm: the flip is a reclassification, not a break.

	TestEqual(TEXT("BOTH ARMS: the LP stands the structure, so no joint may break under load — above cap"),
		Decline.bAnyBroke, false);
	TestEqual(TEXT("BOTH ARMS: the LP stands the structure, so no joint may break under load — below cap"),
		Auth.bAnyBroke, false);

	TestTrue(TEXT("BOTH ARMS: the grounded base keeps the earth in both arms"),
		Auth.Base == EPieceSupport::Grounded && Decline.Base == EPieceSupport::Grounded);

	TestTrue(TEXT("BOTH ARMS: the seated abutment A is held by B0 in both arms (never stranded)"),
		Auth.Abutment == EPieceSupport::Supported && Decline.Abutment == EPieceSupport::Supported);

	// Above the cap, ruling (b) (DESIGN §8) falls R and L rather than stranding them.

	TestEqual(
		TEXT("ABOVE CAP: the brittle router FALLS exactly the two seatless bricks of the refused one-sided "
			 "arch (R and L); it has no honest path to hold them"),
		Decline.Falling, 2);

	TestEqual(
		TEXT("ABOVE CAP: ruling (b) leaves NOTHING stranded — a refused one-sided arch falls, it does not "
			 "strand (stranding was the artefact removed)"),
		Decline.Stranded, 0);

	TestEqual(
		*FString::Printf(TEXT("ABOVE CAP: R reads Falling (brittle router, ruling b), support %d"), static_cast<int32>(Decline.Inner)),
		static_cast<int32>(Decline.Inner), static_cast<int32>(EPieceSupport::Falling));

	TestEqual(
		*FString::Printf(TEXT("ABOVE CAP: L reads Falling (brittle router, ruling b), support %d"), static_cast<int32>(Decline.Outer)),
		static_cast<int32>(Decline.Outer), static_cast<int32>(EPieceSupport::Falling));

	// Below the cap the LP-carried bricks read Supported/Grounded, not Stranded (§3.7).

	TestEqual(
		TEXT("BELOW CAP, THE RED: with the LP authoritative, nothing may read Stranded — the LP carries "
			 "the whole structure, so the router's Stranded verdict is overridden"),
		Auth.Stranded, 0);

	TestTrue(
		*FString::Printf(
			TEXT("BELOW CAP, THE RED: R must read Supported/Grounded (LP-carried), not Stranded; it reads "
				 "%d. Today GetPieceSupport hands out the router's Stranded below the cap"),
			static_cast<int32>(Auth.Inner)),
		IsStanding(Auth.Inner));

	TestTrue(
		*FString::Printf(
			TEXT("BELOW CAP, THE RED: L (the free end) must read Supported/Grounded (LP-carried), not "
				 "Stranded; it reads %d"),
			static_cast<int32>(Auth.Outer)),
		IsStanding(Auth.Outer));

	// The cap alone flips the same piece between Falling and Supported/Grounded.

	TestTrue(
		TEXT("THE SEAM: the block cap alone flips R's support — Falling above the cap (brittle router "
			 "refuses the one-sided arch), Supported/Grounded below it (LP) — on one and the same structure"),
		Decline.Inner == EPieceSupport::Falling && IsStanding(Auth.Inner));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
