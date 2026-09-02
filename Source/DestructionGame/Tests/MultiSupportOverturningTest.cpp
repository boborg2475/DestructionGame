// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE ROUTER LEAVES A MULTI-SUPPORT BODY STANDING WHEN ITS CENTRE OF MASS IS OUTSIDE ITS BEARINGS —
 * the focused, world-free mechanism driver for review item 3. This is the SMALL fixture behind the
 * realistic shed's "the ridge stays up" artefact: a board on TWO co-linear compression bearings whose
 * centre of mass projects PAST the bearing line has no admissible equilibrium and must overturn, yet
 * `FStructure::SolveLoads` ZEROES the overturning moment for any piece with two or more load paths
 * (`bLoadPathIsDeterminate = LoadPaths[Current].Num() == 1 && ...`, Core/Structure.cpp), so every
 * bearing reads a comfortable split compression, nothing breaks, and the board reads Supported.
 *
 * =====================================================================================
 * WHY THE ROUTER, NOT THE LP (the authority this fixture forces)
 * =====================================================================================
 *
 * The equilibrium LP already answers the overturning question correctly BELOW the 200-block cap — it
 * fells the two-load-path body of `Acceptance.Overturning.ABodyOnTwoLoadPathsPastTippingMustFall`. But
 * the realistic shed is 442 blocks, far ABOVE the cap, so its break authority is the ROUTER's per-joint
 * capacity sweep, which is where the ridge artefact lives. Both fixtures here force the router by setting
 * the equilibrium gate's block cap to zero (`SetEquilibriumGateBlockCap(0)`) — the same injectable seam
 * the scope-by-size tests use — so a three- or four-piece fixture is decided by the router, exactly as the
 * shed is. This is a ROUTER (`SolveLoads`) fix; the LP path is not touched.
 *
 * =====================================================================================
 * THE TWO ARMS, AND THE TENSION CLAUSE THAT SEPARATES THEM
 * =====================================================================================
 *
 * Both arms share ONE geometry: a Timber board on two grounded Timber posts, the posts co-linear on a
 * single Y band [0, 10] and the board's centre of mass at Y = 12.5 — PAST the bearing line, so gravity
 * rotates it forward about the front edge of the posts. Because every reaction a compression-only bearing
 * can offer points UP and sits at Y <= 10, no upward-only reaction set has its centroid at Y = 12.5:
 * there is no admissible equilibrium and the board overturns. That is the INDEPENDENT oracle, asserted
 * from the geometry below, not read from production.
 *
 * ARM A (the RED) — the two posts only, both bearings compression-only DryStone (Tensile = 0). Two load
 * paths, centre of mass outside their union, and NO support that can carry tension: the body must lose the
 * earth. Production stands it today (the moment is zeroed for N >= 2), so this is red until the CoM-in-union
 * gate lands.
 *
 * ARM B (the ANTI-REGRESSION) — the same two posts PLUS a third support at the BACK: a grounded anchor tied
 * to the board's back underside by a tension-capable Screw (Tensile = 0.54 MPa), which holds the lifting
 * back down in withdrawal exactly as the porch overhang's cleat does. The centre of mass is STILL outside
 * the union of all three contacts (Y = 12.5 is forward of the frontmost bearing edge, Y = 10), so a naive
 * per-axis "CoM outside the hull -> overturn" rule with no tension clause would FELL this board — a
 * regression, because it genuinely stands. The board must keep the earth: the fix must spare a body that
 * has a tension-capable support in its load path.
 *
 * This is the same distinction the shed draws: the RIDGE bears on compression-only dry gable seats and
 * must overturn once its back gable is gone (ARM A's shape); the PORCH OVERHANG has a Screw tie and must
 * stand (ARM B's shape).
 *
 * =====================================================================================
 * ASSERTIONS — MECHANISM / SUPPORT STATE, NEVER DISPLACEMENT (DESIGN.md §4)
 * =====================================================================================
 *
 * A body can sever every bearing and rest exactly in place, so distance travelled proves nothing. Both
 * arms read `GetPieceSupport`: ARM A asserts the board loses the earth (Falling / Stranded) while the two
 * posts keep it; ARM B asserts the board keeps the earth. Nothing may be Stranded, so a routing limitation
 * cannot wear the overturn's clothes.
 *
 * NOTHING IS IMPORTED FROM THE CODE UNDER TEST EXCEPT THE PRODUCER (`MakeInterface`) and the two connection
 * profiles the bearings use (DryStone, Screw), so the tension-capable distinction is read off the SAME data
 * the fix will read. The unit conversion, the statics and the tipping check are derived here.
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on (weight is mass x 980), everything is connected, and every
 * assertion is on solver support state. Same footing as the two-load-path and leaning-stack fixtures.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace MultiSupportOverturningTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * THE GEOMETRY. Every length is centimetres, at Unreal's default 1 uu = 1 cm.
	 * The board's centre of mass sits FORWARD of the single post bearing line.
	 * ================================================================================ */

	/** EN 338 C24 softwood at its mean density — the same 0.42 the Timber roof boards use. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	/** A 1 cm dry bearing under the board: the separation each bed joint is formed across. */
	constexpr double BedJointThicknessCm = 1.0;

	/* --- The two grounded posts: a single bearing line, co-linear on the Y band [0, 10]. --- */

	constexpr double PostTopZCm = 20.0;
	constexpr double PostBackYCm = 0.0;
	constexpr double PostFrontYCm = 10.0;   // the front bearing edge — the fulcrum the board tips about

	constexpr double PostLLeftXCm = -10.0;
	constexpr double PostLRightXCm = 0.0;
	constexpr double PostRLeftXCm = 40.0;
	constexpr double PostRRightXCm = 50.0;

	/* --- The overhanging board: one rigid Timber piece, its CoM forward of the post line. --- */

	constexpr double BoardLeftXCm = -10.0;
	constexpr double BoardRightXCm = 50.0;
	constexpr double BoardBackYCm = -15.0;
	constexpr double BoardFrontYCm = 40.0;
	constexpr double BoardBottomZCm = PostTopZCm + BedJointThicknessCm;   // 21
	constexpr double BoardThicknessZCm = 5.0;
	constexpr double BoardTopZCm = BoardBottomZCm + BoardThicknessZCm;    // 26

	constexpr double BoardCentreXCm = (BoardLeftXCm + BoardRightXCm) / 2.0;   // 20
	constexpr double BoardCentreYCm = (BoardBackYCm + BoardFrontYCm) / 2.0;   // 12.5 — PAST the line at Y=10
	constexpr double BoardCentreZCm = (BoardBottomZCm + BoardTopZCm) / 2.0;   // 23.5

	/* --- The back tension anchor (ARM B only): a grounded seat tied to the board's back by a Screw. --- */

	constexpr double AnchorLeftXCm = -10.0;
	constexpr double AnchorRightXCm = 50.0;
	constexpr double AnchorBackYCm = -15.0;
	constexpr double AnchorFrontYCm = -5.0;   // wholly BEHIND the post line, so it lifts and pulls in tension

	/* ================================================================================
	 * UNITS. MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it.
	 * ================================================================================ */

	constexpr double GravityCmPerSecondSquared = 980.0;

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

	struct FBoardFixture
	{
		FStructure Structure;
		int32 PostL = INDEX_NONE;
		int32 PostR = INDEX_NONE;
		int32 Board = INDEX_NONE;
		int32 Anchor = INDEX_NONE;   // ARM B only; INDEX_NONE in ARM A
	};

	/**
	 * Lay the board on its two posts, and — when bWithTensionTie — a back anchor tied by a Screw. The two
	 * post bearings are always compression-only DryStone; the tie, when present, is a tension-capable Screw.
	 * The cap is set to zero so the ROUTER, not the LP, is the break authority (the shed's authority at 442).
	 */
	void Build(FBoardFixture& Out, bool bWithTensionTie)
	{
		Out.Structure.SetEquilibriumGateBlockCap(0);

		const FPieceBox PostLBox = MakeBox(PostLLeftXCm, PostLRightXCm, PostBackYCm, PostFrontYCm, 0.0, PostTopZCm);
		const FPieceBox PostRBox = MakeBox(PostRLeftXCm, PostRRightXCm, PostBackYCm, PostFrontYCm, 0.0, PostTopZCm);
		const FPieceBox BoardBox = MakeBox(
			BoardLeftXCm, BoardRightXCm, BoardBackYCm, BoardFrontYCm, BoardBottomZCm, BoardTopZCm);

		Out.PostL = Out.Structure.AddPiece(BoxMassKg(PostLBox), /*bIsGrounded*/ true, PostLBox.CentreCm);
		Out.PostR = Out.Structure.AddPiece(BoxMassKg(PostRBox), /*bIsGrounded*/ true, PostRBox.CentreCm);
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

		if (bWithTensionTie)
		{
			const FPieceBox AnchorBox = MakeBox(
				AnchorLeftXCm, AnchorRightXCm, AnchorBackYCm, AnchorFrontYCm, 0.0, PostTopZCm);
			Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
			if (MakeInterface(Out.Anchor, AnchorBox, Out.Board, BoardBox, BedJointThicknessCm, Screw, Joint))
			{
				Out.Structure.AddConnection(Joint);
			}
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
}

/**
 * ARM A — THE RED. Two co-linear compression-only bearings, the board's centre of mass past the line,
 * and no tension anywhere: the board must overturn. Production zeroes the moment for its two load paths
 * and stands it — the artefact this drives out.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMultiSupportPastTippingOverturnsTest,
	"DestructionGame.Core.Structure.MultiSupportBodyPastTippingOverturnsWithoutTension",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMultiSupportPastTippingOverturnsTest::RunTest(const FString& Parameters)
{
	using namespace MultiSupportOverturningTestSupport;
	using namespace DestructionProfiles;

	/* THE FIXTURE IS GENUINELY PAST TIPPING — the independent oracle. Every compression-only reaction is
	 * upward and sits at Y <= PostFrontYCm; the centre of mass is forward of that edge, so no upward-only
	 * reaction set balances the weight and there is no admissible equilibrium. Read from the geometry. */
	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the board CoM (Y=%.3f) is forward of the front bearing edge (Y=%.3f) — past tipping"),
			BoardCentreYCm, PostFrontYCm),
		BoardCentreYCm > PostFrontYCm);

	/* THE BEARINGS CANNOT CARRY TENSION — DryStone, Tensile == 0 — so nothing holds the lifting side down. */
	TestEqual(TEXT("FIXTURE: the two bearings are compression-only DryStone (Tensile == 0)"),
		DryStone.TensileStrengthMPa, 0.0);

	FBoardFixture Fixture;
	Build(Fixture, /*bWithTensionTie*/ false);

	TestEqual(TEXT("FIXTURE: three pieces — two posts and the board"), Fixture.Structure.NumPieces(), 3);
	TestEqual(TEXT("FIXTURE: two load paths beneath the board — the guard's N >= 2 blind spot"),
		Fixture.Structure.NumConnections(), 2);

	const int32 Passes = Fixture.Structure.SolveAndBreak();

	const EPieceSupport BoardSupport = Fixture.Structure.GetPieceSupport(Fixture.Board);
	const int32 Stranded = StrandedCount(Fixture.Structure);

	AddInfo(FString::Printf(
		TEXT("ARM A: %d breaking pass(es); board support = %d (0 Falling, 1 Grounded, 2 Supported, 3 Stranded); "
			"%d stranded."),
		Passes, static_cast<int32>(BoardSupport), Stranded));

	/* NOTHING MAY BE STRANDED — a routing knot must not wear the overturn's clothes (DESIGN.md §4). */
	TestEqual(TEXT("ARM A: nothing may be Stranded — the fall is about equilibrium, not the router declining"),
		Stranded, 0);

	/* THE RED: the board has no admissible equilibrium on two compression bearings and must LOSE THE EARTH. */
	TestFalse(
		*FString::Printf(TEXT("RED: the board past its bearing line must overturn (support = %d), not read Supported"),
			static_cast<int32>(BoardSupport)),
		IsStanding(BoardSupport));

	/* The two posts are the earth and keep it — only the board falls. */
	TestTrue(TEXT("ARM A: the left post keeps the earth"),
		IsStanding(Fixture.Structure.GetPieceSupport(Fixture.PostL)));
	TestTrue(TEXT("ARM A: the right post keeps the earth"),
		IsStanding(Fixture.Structure.GetPieceSupport(Fixture.PostR)));

	return true;
}

/**
 * ARM B — THE ANTI-REGRESSION. The same board, the same two posts, the same centre of mass OUTSIDE the
 * support union — but a tension-capable Screw tie at the back holds the lifting side down. The board must
 * STAND. A per-axis fix with no tension clause would fell it; the tension clause is what spares it, and it
 * is what spares the porch overhang.
 *
 * This arm is green today (production zeroes the moment) and must STAY green after the fix — its value is
 * pinning the trap, not driving code. It bites only against a fix that ignores the tension clause.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMultiSupportPastTippingStandsWhenTiedTest,
	"DestructionGame.Core.Structure.MultiSupportBodyPastTippingStandsWhenTensionTied",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMultiSupportPastTippingStandsWhenTiedTest::RunTest(const FString& Parameters)
{
	using namespace MultiSupportOverturningTestSupport;
	using namespace DestructionProfiles;

	/* The centre of mass is STILL outside the union of all three contacts — the frontmost bearing edge is
	 * the posts' Y = PostFrontYCm and the anchor sits wholly behind it, so the board's CoM at Y = 12.5 is
	 * forward of every support. A union-only gate would overturn it; only the tension clause spares it. */
	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the board CoM (Y=%.3f) is forward of the frontmost support (Y=%.3f) — outside the union"),
			BoardCentreYCm, PostFrontYCm),
		BoardCentreYCm > PostFrontYCm && AnchorFrontYCm < PostFrontYCm);

	/* THE TIE CAN CARRY TENSION — Screw, Tensile > 0 — the one fact the tension clause turns on. */
	TestTrue(TEXT("FIXTURE: the back tie is a tension-capable Screw (Tensile > 0)"),
		Screw.TensileStrengthMPa > 0.0);

	FBoardFixture Fixture;
	Build(Fixture, /*bWithTensionTie*/ true);

	TestEqual(TEXT("FIXTURE: four pieces — two posts, the board, and the back anchor"),
		Fixture.Structure.NumPieces(), 4);
	TestEqual(TEXT("FIXTURE: three load paths beneath the board — two compression posts and one tension tie"),
		Fixture.Structure.NumConnections(), 3);

	const int32 Passes = Fixture.Structure.SolveAndBreak();

	const EPieceSupport BoardSupport = Fixture.Structure.GetPieceSupport(Fixture.Board);
	const int32 Stranded = StrandedCount(Fixture.Structure);

	AddInfo(FString::Printf(
		TEXT("ARM B: %d breaking pass(es); board support = %d; %d stranded."),
		Passes, static_cast<int32>(BoardSupport), Stranded));

	TestEqual(TEXT("ARM B: nothing may be Stranded"), Stranded, 0);

	/* THE ANTI-REGRESSION: a body with a tension-capable support in its load path must KEEP the earth,
	 * however far its centre of mass reaches past the compression bearings. */
	TestTrue(
		*FString::Printf(TEXT("ANTI-REGRESSION: the tension-tied board must keep the earth (support = %d)"),
			static_cast<int32>(BoardSupport)),
		IsStanding(BoardSupport));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
