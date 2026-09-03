// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 4a OF THE REGIONAL COLLAPSE PROVER (REGIONAL_PROVER_PLAN.md §§3-4, slice 4; review item 12) —
 * WIRING THE PROVER INTO THE REAL SolveAndBreak CASCADE, RED. Slices 1-3 built and pinned the prover
 * machinery in ISOLATION (SolveAndBreak_WithRegionalProver, an explicit test entry that never touches
 * the real cascade). This slice drives that machinery through the PRODUCTION break path: a disturbance
 * (RemovePiece) above the equilibrium-gate cap, then a plain SolveAndBreak() that — once wired — must
 * seed the regional prover from the disturbance neighbourhood and UPGRADE a router "stands" to a proven
 * "falls" (physics-model call 1: override toward Falling, live in the cascade).
 *
 * =====================================================================================
 * THE GENUINE FULL-CASCADE OVER-HOLD THIS FIXTURE ENCODES (found empirically, 2026-09-03)
 * =====================================================================================
 *
 * The prover's cascade value is to fell what the FULL router leaves standing — not merely what the
 * bare SolveLoads baseline leaves standing (the router's guards, item-2/3/5 and the capacity sweep,
 * already fell the simple over-holds: the leaning stack, the two-load-path body, the overturned ridge).
 * So this fixture is a GLOBAL mechanism the per-joint / per-body router structurally cannot see:
 *
 *   - Every joint is UNDER its own per-joint capacity, so BreakByCapacitySweep severs nothing.
 *   - No single body's centre of mass is felled by the item-3 overturning gate, because the board has
 *     a TENSION-CAPABLE support in its load path — the item-3 tension clause spares any body with a
 *     load-path connection whose TensileStrengthMPa > 0, REGARDLESS of that tie's actual capacity
 *     (DESIGN.md §8 2026-09-02 overturning ruling: "the tension clause spares on f_t > 0 capability,
 *     not magnitude/position — a token Nail spares a body the real load might overturn").
 *   - Yet the GLOBAL assembly has no admissible equilibrium: the board cantilevers 137 cm past its one
 *     remaining compression post, and the only thing holding its lifting back down is a single Nail
 *     whose 0.071 MPa withdrawal capacity the required uplift far exceeds.
 *
 * The router therefore OVER-HOLDS: for a piece with N >= 2 load paths SolveLoads ZEROES the overturning
 * moment and area-splits the weight as pure compression, so every joint reads a comfortable share and
 * the board reads Supported. The whole-structure ground-only LP — which reasons about the whole
 * structure's admissible force system, crediting the Nail only up to 0.071 MPa — finds no equilibrium
 * and fells the board. That is the disagreement the regional prover exists to close above the cap.
 *
 * EMPIRICAL EVIDENCE (measured on this exact geometry, cap = 0 forcing the router):
 *   - AS-BUILT (both posts):     whole-structure LP Stands;  router Stands; board Supported.
 *   - REMOVE the +X post:        whole-structure LP FALLS, naming exactly the board (1 moving block).
 *   - REMOVE + isolated prover:  SolveAndBreak_WithRegionalProver(seed={board}, cap=512) releases the
 *                                board (Falling), 0 stranded — the slice-1/2 machinery already fells it.
 *   - REMOVE + REAL SolveAndBreak: 0 breaking passes, board still SUPPORTED, 0 stranded — the cascade
 *                                OVER-HOLDS because it does not yet invoke the prover. THIS is the red.
 *
 * =====================================================================================
 * WHY THIS IS RED, AND FOR THE RIGHT REASON
 * =====================================================================================
 *
 * The real SolveAndBreak pass loop today runs only BreakByEquilibrium (declines above the cap) then
 * BreakByCapacitySweep (severs nothing here). It never calls ProveRegionalCollapse, so the board keeps
 * its router-Supported reading. The red assertion — "the board the whole-structure LP fells must read
 * Falling after SolveAndBreak" — fails because the cascade leaves it Supported, NOT because the fixture
 * or the LP truth is malformed (both are asserted green above it). dev-expert (Slice 4a) makes it green
 * by unioning ProveRegionalCollapse(seedForThisPass, RegionBlockCap) into the gate's decline arm, seeded
 * on pass 1 by the neighbours of the removed piece — here the board.
 *
 * =====================================================================================
 * ASSERTIONS — MECHANISM, NEVER DISPLACEMENT (DESIGN.md §4)
 * =====================================================================================
 *
 * A severed cantilever can rest exactly in place, so distance travelled proves nothing. Every assertion
 * is on solver support state: the board reads EPieceSupport::Falling; the felled set is a SUBSET of the
 * whole-structure LP's moving set (nothing outside the truth falls); the two posts and the anchor keep
 * the earth; and NOTHING reads Stranded — a routing decline must never wear the collapse's clothes.
 *
 * The whole-structure LP truth is derived through a DIFFERENT code path (BuildRigidBlockProblem +
 * SolveRigidBlock) than the regional prover the cascade will invoke, so this has real teeth rather than
 * mirroring the thing under test.
 *
 * =====================================================================================
 * PRODUCTION SURFACE THIS SLICE SPECIFIES (what dev-expert builds to)
 * =====================================================================================
 *
 *   - void FStructure::SetRegionBlockCap(int32): settable member (shipped default 48, the modest
 *     latency-unblock value; 200 reachable via the setter), mirroring SetEquilibriumGateBlockCap.
 *     This test sets it to 512 explicitly, so the default does not affect it.
 *   - int32 FStructure::ProveRegionalCollapse(const TArray<int32>& Seed, int32 RegionBlockCap):
 *     factor the flood + grounded-boundary pose + SolveRigidBlock + Falling-only stitch out of
 *     SolveAndBreak_WithRegionalProver (WITHOUT the SolveLoads baseline — the cascade already solved),
 *     returning the released count. SolveAndBreak_WithRegionalProver becomes
 *     `SolveLoads(); return ProveRegionalCollapse(Seed, Cap);` so the slice-1/2/3 tests pass unchanged.
 *   - In SolveAndBreak's pass loop, when the equilibrium gate DECLINES (above cap), after
 *     BreakByCapacitySweep(Pass) union in ProveRegionalCollapse(seedForThisPass, RegionBlockCap):
 *     seed = neighbours of the removed/tombstoned piece(s) on pass 1, previous-pass severed-joint
 *     endpoints thereafter. Gated on HasCompleteGeometry() exactly as BreakByEquilibrium, so it is a
 *     provable no-op for the geometry-free fuzzes.
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on (weight is mass x 980), everything is connected, and every
 * assertion is an FStructure state query or a pure FOracleProblem solve. No Chaos, no world tick.
 *
 * UNITS ARE DERIVED HERE, never imported, so a wrong production constant disagrees rather than agrees.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace RegionalProverCascadeSeamSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * THE FIXTURE. Centimetres at Unreal's default 1 uu = 1 cm; nothing imported except the
	 * producer (MakeInterface) and the two profiles the joints use (DryStone, Nail).
	 *
	 * A Timber board on two grounded posts plus a token Nail tie at its back:
	 *   - PostL  X[0,10]      grounded, top Z = 20 — the near compression fulcrum.
	 *   - PostR  X[140,150]   grounded, top Z = 20 — carries the far end while it is present.
	 *   - Anchor X[-6,-1]     grounded, top Z = 20 — the seat the Nail ties the board's back to.
	 *   - Board  X[-6,300]    ungrounded, Z[21,26], centre of mass at X = 147.
	 * ================================================================================ */

	/** EN 338 C24 softwood at its mean density — the same 0.42 the Timber roof boards use. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	/** A 1 cm dry bearing under each seat: the separation each bed joint is formed across. */
	constexpr double BedJointThicknessCm = 1.0;

	/** All pieces share one depth in Y — a genuinely planar (X-Z) fixture the 2D LP sees whole. */
	constexpr double DepthYCm = 20.0;

	constexpr double PostTopZCm = 20.0;
	constexpr double PostLLeftXCm = 0.0;
	constexpr double PostLRightXCm = 10.0;    // the front bearing edge the board cantilevers past
	constexpr double PostRLeftXCm = 140.0;
	constexpr double PostRRightXCm = 150.0;

	constexpr double AnchorLeftXCm = -6.0;
	constexpr double AnchorRightXCm = -1.0;

	constexpr double BoardLeftXCm = -6.0;
	constexpr double BoardRightXCm = 300.0;
	constexpr double BoardBottomZCm = PostTopZCm + BedJointThicknessCm;   // 21
	constexpr double BoardThicknessZCm = 5.0;
	constexpr double BoardTopZCm = BoardBottomZCm + BoardThicknessZCm;    // 26

	constexpr double BoardCentreXCm = (BoardLeftXCm + BoardRightXCm) / 2.0;   // 147 — 137 cm past PostL's edge

	FPieceBox MakeBox(double LoX, double HiX, double LoY, double HiY, double LoZ, double HiZ)
	{
		FPieceBox Box;
		Box.CentreCm = FVector((LoX + HiX) / 2.0, (LoY + HiY) / 2.0, (LoZ + HiZ) / 2.0);
		Box.ExtentCm = FVector((HiX - LoX) / 2.0, (HiY - LoY) / 2.0, (HiZ - LoZ) / 2.0);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return TimberDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	struct FFixture
	{
		FStructure Structure;
		int32 PostL = INDEX_NONE;
		int32 PostR = INDEX_NONE;
		int32 Anchor = INDEX_NONE;
		int32 Board = INDEX_NONE;
	};

	/**
	 * Lay the two posts, the anchor and the board. GateBlockCap = 0 forces the ROUTER to be the break
	 * authority (NumPieces() > 0 always declines the equilibrium gate), exactly as the 442-block shed
	 * is above its 200-block cap — so this small fixture exercises the same above-cap decline arm the
	 * regional prover is wired into.
	 */
	void Build(FFixture& Out)
	{
		Out.Structure.SetEquilibriumGateBlockCap(0);

		const FPieceBox PostLBox = MakeBox(PostLLeftXCm, PostLRightXCm, -DepthYCm / 2, DepthYCm / 2, 0.0, PostTopZCm);
		const FPieceBox PostRBox = MakeBox(PostRLeftXCm, PostRRightXCm, -DepthYCm / 2, DepthYCm / 2, 0.0, PostTopZCm);
		const FPieceBox AnchorBox = MakeBox(AnchorLeftXCm, AnchorRightXCm, -DepthYCm / 2, DepthYCm / 2, 0.0, PostTopZCm);
		const FPieceBox BoardBox = MakeBox(
			BoardLeftXCm, BoardRightXCm, -DepthYCm / 2, DepthYCm / 2, BoardBottomZCm, BoardTopZCm);

		Out.PostL = Out.Structure.AddPiece(BoxMassKg(PostLBox), /*bIsGrounded*/ true, PostLBox.CentreCm);
		Out.PostR = Out.Structure.AddPiece(BoxMassKg(PostRBox), /*bIsGrounded*/ true, PostRBox.CentreCm);
		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Board = Out.Structure.AddPiece(BoxMassKg(BoardBox), /*bIsGrounded*/ false, BoardBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.PostL, PostLBox, Out.Board, BoardBox, BedJointThicknessCm, DryStone, Joint))
		{
			Out.Structure.AddConnection(Joint);
		}
		if (MakeInterface(Out.PostR, PostRBox, Out.Board, BoardBox, BedJointThicknessCm, DryStone, Joint))
		{
			Out.Structure.AddConnection(Joint);
		}
		if (MakeInterface(Out.Anchor, AnchorBox, Out.Board, BoardBox, BedJointThicknessCm, Nail, Joint))
		{
			Out.Structure.AddConnection(Joint);
		}
	}

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (!S.IsPieceRemoved(P) && S.GetPieceSupport(P) == EPieceSupport::Stranded)
			{
				++N;
			}
		}
		return N;
	}

	/** The whole-structure ground-only LP's moving set for the CURRENT (post-removal) graph. */
	TSet<int32> WholeStructureMovingSet(const FStructure& S, bool& bOutFalls, bool& bOutCertified)
	{
		using namespace RigidBlockOracle;
		bOutFalls = false;
		bOutCertified = false;

		FOracleProblem Problem;
		FString WhyNot;
		if (!BuildRigidBlockProblem(S, Problem, WhyNot))
		{
			return {};
		}

		// The below-cap authority's pose: feasibility at self-weight, with first-crack rows.
		Problem.bGravityIsLive = false;
		Problem.bFirstCrackRows = true;

		const FOracleResult Result = SolveRigidBlock(Problem);
		bOutFalls = OutcomeOf(Result) == EOracleOutcome::Falls;
		bOutCertified = Result.Mechanism.bPresent && Result.Mechanism.bIsCertified;

		TSet<int32> Moving;
		for (int32 Block = 0; Block < Result.Mechanism.Blocks.Num(); ++Block)
		{
			if (Result.Mechanism.Blocks[Block].bMoves && Problem.PieceOfBlock.IsValidIndex(Block))
			{
				Moving.Add(Problem.PieceOfBlock[Block]);
			}
		}
		return Moving;
	}
}

/**
 * Removing the far post above the cap leaves the board a deep cantilever the router over-holds (the
 * item-3 tension clause spares it on the token Nail's f_t > 0, and the N >= 2 moment-zero splits its
 * weight as comfortable compression) but the whole-structure LP fells — and once the regional prover
 * is wired into the cascade, a plain SolveAndBreak must fell it too, releasing exactly the LP's moving
 * set, 0 stranded.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverCascadeFellsAnAboveCapOverHoldTest,
	"DestructionGame.Core.Structure.RegionalProver.CascadeFellsAnAboveCapOverHoldTheRouterStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverCascadeFellsAnAboveCapOverHoldTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverCascadeSeamSupport;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * FIXTURE PRECONDITIONS — the geometry is genuinely past tipping, the tie is a token, and the
	 * structure is above the equilibrium-gate cap (router is the authority).
	 * ================================================================================ */
	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the board CoM (X=%.1f) is past PostL's front edge (X=%.1f) — a deep cantilever"),
			BoardCentreXCm, PostLRightXCm),
		BoardCentreXCm > PostLRightXCm);

	/* The Nail is a genuine tension-capable tie (so the item-3 tension clause spares the board) but a
	 * WEAK one — 0.071 MPa withdrawal, ~1/8 the Screw — so the LP's true-capacity check fells it. Both
	 * facts read off the profile data the fix reads, never from the code under test. */
	TestTrue(TEXT("FIXTURE: the back tie is tension-capable (Nail f_t > 0) — the router's tension clause spares it"),
		Nail.TensileStrengthMPa > 0.0);
	TestTrue(TEXT("FIXTURE: the tie is a TOKEN — Nail f_t is far weaker than a Screw's 0.54 MPa"),
		Nail.TensileStrengthMPa < 0.1 && Nail.TensileStrengthMPa < Screw.TensileStrengthMPa);
	TestEqual(TEXT("FIXTURE: the two posts are compression-only DryStone (f_t == 0)"),
		DryStone.TensileStrengthMPa, 0.0);

	FFixture Fixture;
	Build(Fixture);

	TestEqual(TEXT("FIXTURE: four pieces — two posts, an anchor, and the board"),
		Fixture.Structure.NumPieces(), 4);
	TestEqual(TEXT("FIXTURE: three joints beneath the board — two compression posts and one Nail tie"),
		Fixture.Structure.NumConnections(), 3);
	TestTrue(TEXT("FIXTURE: complete geometry (the regional prover is gated on it)"),
		Fixture.Structure.HasCompleteGeometry());
	TestTrue(TEXT("FIXTURE: the structure is above the equilibrium-gate cap, so the router is the authority"),
		Fixture.Structure.NumPieces() > 0 /* GateBlockCap set to 0 in Build */);

	/* ================================================================================
	 * PRECONDITION (A): AS BUILT, THE WHOLE-STRUCTURE LP STANDS IT. This proves the fall is caused by
	 * the removal, not by a fixture the LP condemns from the start. Const read — no mutation.
	 * ================================================================================ */
	{
		bool bFalls = false, bCertified = false;
		WholeStructureMovingSet(Fixture.Structure, bFalls, bCertified);
		TestFalse(TEXT("PRECONDITION: as built (both posts) the whole-structure LP does NOT fell anything"),
			bFalls);
	}

	/* ================================================================================
	 * THE DISTURBANCE: remove the far post above the cap. The board is now a cantilever over PostL,
	 * held at the back only by the token Nail.
	 * ================================================================================ */
	TestTrue(TEXT("DISTURBANCE: the far post is removed"),
		Fixture.Structure.RemovePiece(Fixture.PostR));

	/* ================================================================================
	 * (B) THE INDEPENDENT TRUTH SET — the whole-structure ground-only LP on the POST-REMOVAL graph,
	 * a DIFFERENT code path than the regional prover the cascade invokes. Const read before SolveAndBreak.
	 * ================================================================================ */
	bool bTruthFalls = false, bTruthCertified = false;
	const TSet<int32> TruthMoving =
		WholeStructureMovingSet(Fixture.Structure, bTruthFalls, bTruthCertified);

	TestTrue(TEXT("TRUTH: the whole-structure LP proves the cantilever FALLS once the far post is gone"),
		bTruthFalls);
	TestTrue(TEXT("TRUTH: the fall carries a present, Farkas-certified collapse mechanism"),
		bTruthCertified);
	TestTrue(
		*FString::Printf(TEXT("TRUTH: the LP names the board among the moving set (moving pieces %d)"),
			TruthMoving.Num()),
		TruthMoving.Contains(Fixture.Board));
	TestTrue(
		*FString::Printf(TEXT("TRUTH: only the ungrounded board moves — grounded posts/anchor stay (moving %d)"),
			TruthMoving.Num()),
		TruthMoving.Num() >= 1);

	/* ================================================================================
	 * (C) THE NEW BEHAVIOUR (RED UNTIL THE CASCADE IS WIRED). Set a region cap that covers the whole
	 * structure, then run the PLAIN production cascade. Once the prover is unioned into the gate's
	 * decline arm (seeded from the removed piece's neighbours — the board), SolveAndBreak must fell
	 * exactly the LP's moving set. Assert on MECHANISM: support state and stranded count, never movement.
	 * ================================================================================ */
	Fixture.Structure.SetRegionBlockCap(512);   // >= the live block count: the region floods everything

	const int32 Passes = Fixture.Structure.SolveAndBreak();

	const EPieceSupport BoardSupport = Fixture.Structure.GetPieceSupport(Fixture.Board);
	const int32 Stranded = StrandedCount(Fixture.Structure);

	AddInfo(FString::Printf(
		TEXT("CASCADE: %d breaking pass(es); board support = %d (0 Falling, 1 Grounded, 2 Supported, 3 Stranded); "
			"%d stranded; LP truth-moving = %d."),
		Passes, static_cast<int32>(BoardSupport), Stranded, TruthMoving.Num()));

	/* NOTHING MAY BE STRANDED — a routing decline must not wear the collapse's clothes (DESIGN.md §4). */
	TestEqual(TEXT("CASCADE: nothing is Stranded — the fall is about equilibrium, not the router declining"),
		Stranded, 0);

	/* THE RED: the cascade, seeded from the disturbance, must fell every piece the whole-structure LP
	 * names — here the board. It currently over-holds it (SolveAndBreak never invokes the prover), so
	 * the board reads Supported and this fails for the right reason. */
	for (const int32 Piece : TruthMoving)
	{
		TestEqual(
			*FString::Printf(
				TEXT("RED: truth-set piece %d (the whole-structure LP fells it) must read Falling after the ")
				TEXT("wired cascade — the router over-holds it today"),
				Piece),
			Fixture.Structure.GetPieceSupport(Piece), EPieceSupport::Falling);
	}

	/* The felled set is a SUBSET of the LP truth: the two remaining supports keep the earth, nothing
	 * outside the mechanism is dragged down. */
	TestTrue(TEXT("SUBSET: PostL keeps the earth"),
		IsStanding(Fixture.Structure.GetPieceSupport(Fixture.PostL)));
	TestTrue(TEXT("SUBSET: the back anchor keeps the earth"),
		IsStanding(Fixture.Structure.GetPieceSupport(Fixture.Anchor)));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
