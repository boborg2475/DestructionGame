// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * GetPieceSupport IS LP-AUTHORITATIVE BELOW THE BLOCK CAP — the one genuinely NEW semantics that
 * Slice 3b introduces (PROMOTION_DESIGN.md §12 D7's 3b section, §3.7). This is the RED that pins it
 * so a wrong wiring cannot quietly pass, and it is the miniature of catalogue rows 10 and 19.
 *
 * THE SEAM UNDER TEST, STATED AS ARITHMETIC. Support is answered two different ways by the two
 * mechanisms this step reconciles:
 *   - The ROUTER (SolveLoads' downward flood) can only route a piece's load to the ground DOWN a
 *     support graph. A piece caught in a knot — its own load returning to it round a cycle it has
 *     no rule to divide — is conservatively `Stranded` (DESIGN.md §5.1). Stranding is a statement
 *     about the SOLVER, not about the masonry: the piece may be perfectly well held up by a force
 *     system the downward flood cannot see (an arch, lateral thrust, an upward reaction).
 *   - The LP (rigid-block limit analysis) asks whether ANY admissible force system exists in
 *     equilibrium with self-weight. It has no routing to fail and no accumulation order to be
 *     defeated by a loop, so it stands a structure the router strands whenever a real equilibrium
 *     exists — which is exactly why rows 10 and 19 (λ* 111.5 / 48.0) STAND while the router drops
 *     the pieces it could not route.
 *
 * Slice 3b makes the LP the authority BELOW the block cap: after a settling solve, a piece the LP
 * carries reads `Supported`/`Grounded`, NOT `Stranded`, because the LP stands the whole structure.
 * That is the ruling D7 names ("a piece in the LP force system reads Supported") and it is what
 * greens rows 10/19 — the wall harness's FallenPieces/StrandedCount read GetPieceSupport, so a
 * router-`Stranded` piece the LP carries is counted as fallen today and must stop being.
 *
 * ABOVE THE CAP NOTHING CHANGES. The cap is the fail-closed boundary that keeps synchronous LP
 * authority off the flagship scenarios (PROMOTION_DESIGN.md §12 D2⁗); above it GetPieceSupport must
 * fall through to the router's `Stranded` exactly as production does today. This test drives BOTH
 * sides of that boundary on ONE AND THE SAME structure, changing only the injectable cap
 * (SetEquilibriumGateBlockCap) — so it pins that the LP verdict overrides the router's support
 * enumeration below the cap and ONLY below it.
 *
 * THE FIXTURE — THE ROW-10 SHAPE IN MINIATURE: AN OPENING AT A FREE END, NO ABUTMENT.
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
 * FOUR PIECES. B0 is grounded. A bears on B0 through a BED joint, so A is genuinely SEATED (it does
 * NOT fall back to head joints — no support-graph pollution from a grounded neighbour naming a
 * cantilever as its own support). R and L hang out to the LEFT over a void: their only joints are
 * HEAD joints (R↔A and L↔R), and NOTHING sits beneath them, so both are SEATLESS.
 *
 * WHY THE ROUTER STRANDS R AND L (and this is genuine, not contrived). A seatless piece falls back
 * to its head joints as supports, sign-blind. So R's supports are {A, L} and L's support is {R}:
 * L leans on R and R leans (partly) on L, a two-node cycle. LoadReturnsToPiece walks R → L → R and
 * R → A → B0(ground); the return to R via L makes R its own support, so R is `Stranded`, and L the
 * same. The re-seat that would rescue a spanned run (ReseatSpannedGroups) does NOT fire here: it
 * only routes a group with a SEATED abutment on BOTH opposite sides, and this opening has an
 * abutment (A) on the RIGHT only — the free end on the left has none. That is precisely case 10's
 * shape, "opening at a free end, no abutment", and precisely why the router leaves 3 of its pieces
 * Stranded there.
 *
 * WHY THE LP CARRIES THEM. The head joints are vertical faces; the seatless bricks' weight is
 * carried across them in SHEAR (mortar cohesion 0.9 MPa over a 66.6 cm² face is ~483× each brick's
 * ~1240 uu weight), and the small cantilever moment is taken by the joints' tension/compression
 * couple far inside the flexural bond. So an admissible equilibrium at self-weight plainly exists
 * and the LP stands the whole structure with a large margin — asserted here through the oracle as a
 * PRECONDITION (router-strands / LP-carries is the whole point, so a fixture the LP did not stand
 * would be wrong), exactly as the two-load-path red asserts its oracle Falls.
 *
 * ASSERTIONS, per DESIGN.md §4. OUTCOME/support-state only; displacement is never read. The
 * structure STANDS in both arms (nothing released, no joint broke under load) — the flip is a
 * re-classification of one piece's support state by the authority answering it, not a break. The
 * mechanism asserted is the discrete support ENUMERATOR (Stranded vs Supported/Grounded), which is
 * binary and immune to jitter, and it is exactly the reading GetPieceSupport hands the wall harness.
 *
 * NOTHING IS IMPORTED FROM THE CODE UNDER TEST EXCEPT THE PRODUCER (MakeInterface) and the mortar
 * profile the joints are laid in; masses are derived here from density and geometry. The verdict
 * rests on the LP's own answer (which derives its own MPa conversion) and on the router's own
 * support enumeration — not on any strength constant this file re-derives.
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on the ordinary way (weight is MassKg × 980 inside
 * FStructure), everything is connected, and every assertion is on solver state, the oracle, or the
 * settled support enumeration. Same footing as the two-load-path and dry-stack acceptance tests.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace SupportAuthorityBelowCapSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * GEOMETRY. Every length is centimetres, at Unreal's default 1 uu = 1 cm.
	 * ================================================================================ */

	/** Fired clay, 1.9 g/cm3 — the same figure every wall fixture uses. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** The single wythe: every piece is this deep on Y, so every joint's Y overlap is full. */
	constexpr double WytheWidthCm = 10.25;

	/** A 1 cm mortar bed/head joint — the separation every interface is formed across. */
	constexpr double JointThicknessCm = 1.0;

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
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

	/**
	 * Lay the four pieces and the three joints — one bed (A on B0) and two head (R↔A, L↔R). The two
	 * seatless bricks R and L get NO joint beneath them, by construction, so the tier can only class
	 * them seatless and the router can only route them through their head joints.
	 */
	void Build(FFreeEndOpening& Out)
	{
		/* B0: the grounded base A bears on. Kept narrow so it sits only under A. */
		const FPieceBox BaseBox = MakeBox(/*X*/ 100.0, /*SizeX*/ 24.0, /*Z*/ 10.0, /*SizeZ*/ 20.0);

		/* A: the abutment, one bed above B0's top — genuinely seated, so no head fallback. */
		const FPieceBox AbutBox = MakeBox(/*X*/ 100.0, /*SizeX*/ 20.0, /*Z*/ 31.0, /*SizeZ*/ 20.0);

		/* R: seatless, one head joint left of A, at mid-height of A's face; nothing beneath it. */
		const FPieceBox InnerBox = MakeBox(/*X*/ 84.0, /*SizeX*/ 10.0, /*Z*/ 34.0, /*SizeZ*/ 6.5);

		/* L: the free end, one head joint left of R; nothing beneath it, nothing to its left. */
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

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	/** Whether ANY joint in the structure gave under load (a break stamp), as opposed to none. */
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

/**
 * GetPieceSupport reads the LP verdict below the cap and the router's verdict above it.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSupportAuthorityBelowCapTest,
	"DestructionGame.Acceptance.SupportAuthority.GetPieceSupportIsLPAuthoritativeBelowTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSupportAuthorityBelowCapTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace SupportAuthorityBelowCapSupport;

	/* ------------------------------------------------------------------ *
	 * PRECONDITION ON THE STRENGTH BASIS — the LP only carries seatless
	 * bricks in shear if the joints have cohesion, so a dry profile would
	 * not stand this. Pin the mortar bond the fixture relies on.
	 * ------------------------------------------------------------------ */

	TestEqual(TEXT("FIXTURE: the joints are the mean-basis 0.90 MPa cohesion mortar"),
		GeneralPurposeMortar.ShearCohesionMPa, 0.9);

	TestEqual(TEXT("FIXTURE: the joints are the mean-basis 0.70 MPa flexural bond mortar"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.7);

	/* ------------------------------------------------------------------ *
	 * BUILD, AND CHECK THE TOPOLOGY IS THE ONE CLAIMED: four pieces, three
	 * joints, complete geometry, R and L held only by head joints.
	 * ------------------------------------------------------------------ */

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

	/* ------------------------------------------------------------------ *
	 * THE CROSS-CHECK: the LP finds an admissible equilibrium at self-weight
	 * (Stands, λ* ≥ 1). Router-strands / LP-carries is the whole property, so
	 * a fixture the LP did NOT stand would be wrong — asserted, not reported.
	 * ------------------------------------------------------------------ */

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

	/* ------------------------------------------------------------------ *
	 * RUN THE SETTLING SOLVE TWICE, CHANGING ONLY THE CAP. Fresh build each
	 * time — SolveAndBreak stamps — so the ONLY difference is the block cap.
	 * ------------------------------------------------------------------ */

	struct FRun
	{
		int32 Passes = 0;
		int32 Stranded = 0;
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
		R.bAnyBroke = AnyJointBrokeUnderLoad(Fx.Structure);
		R.Inner = Fx.Structure.GetPieceSupport(Fx.Inner);
		R.Outer = Fx.Structure.GetPieceSupport(Fx.Outer);
		R.Abutment = Fx.Structure.GetPieceSupport(Fx.Abutment);
		R.Base = Fx.Structure.GetPieceSupport(Fx.Base);
		return R;
	};

	/* AT OR BELOW THE CAP the LP is the authority: a piece it carries reads Supported/Grounded. */
	constexpr int32 AuthoritativeCap = 8;
	const FRun Auth = RunAtCap(AuthoritativeCap);

	/* ABOVE THE CAP the gate declines: GetPieceSupport falls through to the router's Stranded. */
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

	/* ------------------------------------------------------------------ *
	 * THE STRUCTURE STANDS IN BOTH ARMS. The flip is a re-classification of a
	 * support state, never a break: nothing is felled, no joint gives.
	 * ------------------------------------------------------------------ */

	TestEqual(TEXT("BOTH ARMS: the LP stands the structure, so no joint may break under load — above cap"),
		Decline.bAnyBroke, false);
	TestEqual(TEXT("BOTH ARMS: the LP stands the structure, so no joint may break under load — below cap"),
		Auth.bAnyBroke, false);

	TestTrue(TEXT("BOTH ARMS: the grounded base keeps the earth in both arms"),
		Auth.Base == EPieceSupport::Grounded && Decline.Base == EPieceSupport::Grounded);

	TestTrue(TEXT("BOTH ARMS: the seated abutment A is held by B0 in both arms (never stranded)"),
		Auth.Abutment == EPieceSupport::Supported && Decline.Abutment == EPieceSupport::Supported);

	/* ------------------------------------------------------------------ *
	 * THE ROUTER BASELINE (above the cap) — passes today, and must KEEP passing
	 * after 3b: above the cap GetPieceSupport is the router, which strands the
	 * two seatless bricks it cannot route. This is the "router strands it" half
	 * of the property, and the guard that the LP authority is bounded by the cap.
	 * ------------------------------------------------------------------ */

	TestEqual(
		TEXT("ABOVE CAP: the router strands exactly the two seatless bricks it cannot route (R and L)"),
		Decline.Stranded, 2);

	TestEqual(
		*FString::Printf(TEXT("ABOVE CAP: R reads Stranded (router), support %d"), static_cast<int32>(Decline.Inner)),
		static_cast<int32>(Decline.Inner), static_cast<int32>(EPieceSupport::Stranded));

	TestEqual(
		*FString::Printf(TEXT("ABOVE CAP: L reads Stranded (router), support %d"), static_cast<int32>(Decline.Outer)),
		static_cast<int32>(Decline.Outer), static_cast<int32>(EPieceSupport::Stranded));

	/* ------------------------------------------------------------------ *
	 * THE RED — below the cap the LP is authoritative, so the two bricks it
	 * carries must read Supported/Grounded, NOT Stranded. Today GetPieceSupport
	 * still hands out the router's Stranded below the cap, so this arm fails —
	 * which is the behaviour Slice 3b adds (§3.7's release-rule change, the
	 * ruling that "a piece in the LP force system reads Supported").
	 * ------------------------------------------------------------------ */

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

	/* ------------------------------------------------------------------ *
	 * THE SEAM ITSELF: the block cap ALONE decides which authority answers.
	 * The SAME piece reads Stranded above the cap and Supported/Grounded below
	 * it — the LP verdict overrides the router's support enumeration below the
	 * cap and ONLY below it.
	 * ------------------------------------------------------------------ */

	TestTrue(
		TEXT("THE SEAM: the block cap alone flips R's support — Stranded above the cap (router), "
			 "Supported/Grounded below it (LP) — on one and the same structure"),
		Decline.Inner == EPieceSupport::Stranded && IsStanding(Auth.Inner));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
