// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Review item 3: a board on two co-linear compression bearings with its centre of mass past the
 * bearing line has no admissible equilibrium and must overturn. The router's SolveLoads zeroed the
 * moment for any piece with two or more load paths, so it read Supported. This is the small
 * fixture behind the realistic shed's ridge staying up.
 *
 * The LP already handles this below the 200-block cap, but the shed (442 blocks) is decided by the
 * router, so both arms set the block cap to 0 to force the router.
 *
 * Shared geometry: a Timber board on two grounded posts in the Y band [0, 10], centre of mass at
 * Y = 12.5. Every compression reaction is upward at Y <= 10, so none balances it.
 *   Arm A: DryStone bearings only (no tension). The board must fall; the posts stay. Like the ridge.
 *   Arm B: plus a back anchor tied by a Screw (0.54 MPa withdrawal). The centre of mass is still
 *     outside all contacts, but the tie holds the back down, so it must stand. Like the porch.
 *     Guards against a fix without a tension clause.
 *
 * Asserts support state, never displacement (DESIGN.md §4); nothing may be Stranded. World-free.
 * Named namespace for unity builds.
 */
namespace MultiSupportOverturningTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** EN 338 C24 mean density. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	constexpr double BedJointThicknessCm = 1.0;

	// The two grounded posts, co-linear on Y [0, 10].

	constexpr double PostTopZCm = 20.0;
	constexpr double PostBackYCm = 0.0;
	constexpr double PostFrontYCm = 10.0;   // front bearing edge, the tipping fulcrum

	constexpr double PostLLeftXCm = -10.0;
	constexpr double PostLRightXCm = 0.0;
	constexpr double PostRLeftXCm = 40.0;
	constexpr double PostRRightXCm = 50.0;

	// The board, its centre of mass forward of the post line.

	constexpr double BoardLeftXCm = -10.0;
	constexpr double BoardRightXCm = 50.0;
	constexpr double BoardBackYCm = -15.0;
	constexpr double BoardFrontYCm = 40.0;
	constexpr double BoardBottomZCm = PostTopZCm + BedJointThicknessCm;   // 21
	constexpr double BoardThicknessZCm = 5.0;
	constexpr double BoardTopZCm = BoardBottomZCm + BoardThicknessZCm;    // 26

	constexpr double BoardCentreXCm = (BoardLeftXCm + BoardRightXCm) / 2.0;   // 20
	constexpr double BoardCentreYCm = (BoardBackYCm + BoardFrontYCm) / 2.0;   // 12.5, past the line at Y=10
	constexpr double BoardCentreZCm = (BoardBottomZCm + BoardTopZCm) / 2.0;   // 23.5

	// Arm B's back anchor, tied to the board by a Screw.

	constexpr double AnchorLeftXCm = -10.0;
	constexpr double AnchorRightXCm = 50.0;
	constexpr double AnchorBackYCm = -15.0;
	constexpr double AnchorFrontYCm = -5.0;   // behind the post line, so it acts in tension

	/** MassKg * 980 is already a weight in uu; do not apply 1 N = 100 uu again. */
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

	/** Board on two DryStone post bearings, plus a Screw-tied back anchor if bWithTensionTie. Block cap 0 forces the router. */
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

/** Arm A: compression-only bearings, centre of mass past the line, no tension; the board must overturn. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMultiSupportPastTippingOverturnsTest,
	"DestructionGame.Core.Structure.MultiSupportBodyPastTippingOverturnsWithoutTension",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMultiSupportPastTippingOverturnsTest::RunTest(const FString& Parameters)
{
	using namespace MultiSupportOverturningTestSupport;
	using namespace DestructionProfiles;

	// Independent oracle from geometry: the centre of mass is forward of every upward reaction.
	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the board CoM (Y=%.3f) is forward of the front bearing edge (Y=%.3f) — past tipping"),
			BoardCentreYCm, PostFrontYCm),
		BoardCentreYCm > PostFrontYCm);

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

	TestEqual(TEXT("ARM A: nothing may be Stranded — the fall is about equilibrium, not the router declining"),
		Stranded, 0);

	TestFalse(
		*FString::Printf(TEXT("RED: the board past its bearing line must overturn (support = %d), not read Supported"),
			static_cast<int32>(BoardSupport)),
		IsStanding(BoardSupport));

	// Only the board falls.
	TestTrue(TEXT("ARM A: the left post keeps the earth"),
		IsStanding(Fixture.Structure.GetPieceSupport(Fixture.PostL)));
	TestTrue(TEXT("ARM A: the right post keeps the earth"),
		IsStanding(Fixture.Structure.GetPieceSupport(Fixture.PostR)));

	return true;
}

/**
 * Arm B: the same board, but a Screw tie at the back holds it down, so it must stand even with its
 * centre of mass outside every support. Guards against an overturn rule without a tension clause.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMultiSupportPastTippingStandsWhenTiedTest,
	"DestructionGame.Core.Structure.MultiSupportBodyPastTippingStandsWhenTensionTied",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMultiSupportPastTippingStandsWhenTiedTest::RunTest(const FString& Parameters)
{
	using namespace MultiSupportOverturningTestSupport;
	using namespace DestructionProfiles;

	// The centre of mass is still forward of every support, anchor included.
	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the board CoM (Y=%.3f) is forward of the frontmost support (Y=%.3f) — outside the union"),
			BoardCentreYCm, PostFrontYCm),
		BoardCentreYCm > PostFrontYCm && AnchorFrontYCm < PostFrontYCm);

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

	// A tension-capable support in the load path keeps the board up.
	TestTrue(
		*FString::Printf(TEXT("ANTI-REGRESSION: the tension-tied board must keep the earth (support = %d)"),
			static_cast<int32>(BoardSupport)),
		IsStanding(BoardSupport));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
