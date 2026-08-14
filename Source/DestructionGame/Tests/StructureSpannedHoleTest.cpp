// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named for what it holds. An anonymous namespace is private
 * to a TRANSLATION UNIT rather than to a file, and a unity build merges many files into one — at
 * which point two anonymous namespaces in the blob are the SAME namespace and identically-named
 * helpers in files that never refer to each other are a hard compile error.
 */
namespace StructureSpannedHoleTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace StructureArchingTestSupport;

	/**
	 * THE THREE BRICKS THE PLAYER DELETES: full bricks 1, 2 and 3 of course 1, x = 33.75 to 78.75.
	 *
	 * Course 1 runs half bat, then full bricks at 11.25, 33.75, 56.25, 78.75, 101.25, 123.75, then
	 * half bat. Taking the middle three leaves the void spanning x = 22.5 to 90 — three coordinating
	 * cells, 67.5 cm — with a full column of wall standing either side of it, so nothing below is a
	 * free-end effect.
	 */
	constexpr int32 CutCourse = 1;
	constexpr int32 FirstCutBrickIndex = 1;
	constexpr int32 CutCellCount = 3;

	/** The course whose bricks lose their seats: the one immediately above the cut. */
	constexpr int32 SpannedCourse = CutCourse + 1;

	/**
	 * THE FOUR BRICKS THE CUT LEAVES WITHOUT A COMPLETE SEAT, and the shape is the whole slice.
	 *
	 *      course 2      [ 22.5 ][  45  ][ 67.5 ][  90  ]      the GROUP
	 *      course 1   [11.25][ ---- ][ ---- ][ ---- ][101.25]  the cut
	 *
	 * The two on the OUTSIDE keep exactly one 10.25 x 10.25 patch each — those are the SPRINGINGS,
	 * and they are the same half seat slice 1 already arches. The two in the MIDDLE keep NONE: both
	 * bricks that carried them are gone, so under the two-tier rule they fall back to their head
	 * joints and hang sideways off their neighbours.
	 */
	constexpr double LeftSpringingBrickXCm = ArchWallEvenBrickXCm(1);
	constexpr double RightSpringingBrickXCm = ArchWallEvenBrickXCm(4);

	constexpr double LeftHangingBrickXCm = ArchWallEvenBrickXCm(2);
	constexpr double RightHangingBrickXCm = ArchWallEvenBrickXCm(3);

	/** The one seat each springing keeps: the course-1 full brick on its OUTBOARD side. */
	constexpr double LeftSpringingSeatXCm = ArchWallOddBrickXCm(0);
	constexpr double RightSpringingSeatXCm = ArchWallOddBrickXCm(4);

	/**
	 * EVERY JOINT THE THREE DELETED BRICKS TAKE WITH THEM, COUNTED BY HAND OFF THE BOND.
	 *
	 * Four head joints along course 1 (the two inside the cut, plus one to the survivor at each
	 * end); six bed joints down to course 0 and six up to course 2, since each deleted brick spans
	 * two bricks below and carries two above. 4 + 6 + 6 = 16. The one-brick cut's equivalent count
	 * is 6, and it is the same walk: a brick shares its two inner head joints with its neighbours in
	 * the cut, so widening the hole by one cell adds five joints and not six.
	 *
	 * Pinned so that a cascade which happened to break exactly zero joints while the deletion
	 * quietly took a seventeenth could not read as a wall that stood.
	 */
	constexpr int32 JointsLostWithTheCut = 16;

	/** How many intact bed joints this piece is resting on RIGHT NOW. */
	inline int32 IntactSeatsUnder(const FStructure& Structure, int32 Piece)
	{
		int32 Seats = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			/*
			 * A JOINT THAT HAS GIVEN IS NOT A SEAT, and forgetting to ask is a trap this fixture
			 * inherits from slice 1's. GetJointRole is pure geometry and keeps answering for a
			 * joint that has left the structure — deliberately, because what a given joint USED to
			 * be is exactly what a debugger wants — so the joints that went with the deleted bricks
			 * still report BedBeneath and a naive count says the hangers are fully seated.
			 */
			if (Structure.GetJointRole(Joint, Piece) == EJointRole::BedBeneath
				&& !Structure.GetConnection(Joint).HasGiven())
			{
				++Seats;
			}
		}

		return Seats;
	}

	/** The one intact bed joint under a piece, or INDEX_NONE if it does not have exactly one. */
	inline int32 TheOneSeatUnder(const FStructure& Structure, int32 Piece)
	{
		int32 Seat = INDEX_NONE;
		int32 Seats = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			if (Structure.GetJointRole(Joint, Piece) == EJointRole::BedBeneath
				&& !Structure.GetConnection(Joint).HasGiven())
			{
				Seat = Joint;
				++Seats;
			}
		}

		return Seats == 1 ? Seat : INDEX_NONE;
	}

	/** Everything an intact piece is carrying, read off the joints it rests on. */
	inline double TotalCarriedUu(const FStructure& Structure, int32 Piece)
	{
		double TotalUu = 0.0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			if (Structure.GetJointRole(Joint, Piece) == EJointRole::BedBeneath
				&& !Structure.GetConnection(Joint).HasGiven())
			{
				TotalUu += FMath::Abs(Structure.GetConnectionForce(Joint).Z);
			}
		}

		return TotalUu;
	}

	/** What ARCHING_DESIGN.md predicts for the springing of a three-cell hole. */
	constexpr double DesignSpringingUtilisation = 0.036;
}

/**
 * A HOLE WIDER THAN ONE BRICK IS SPANNED: the bricks left with NO seat at all stop hanging from
 * their head joints and are re-seated onto the group's abutment joints, so nothing comes down.
 *
 * WHY SLICE 1 DOES NOT ALREADY DO THIS, AND THE REASON IS THE POINT OF THE SLICE. Delete three
 * bricks instead of one and the course above keeps two half-seated bricks at the EDGES and two
 * with no seat whatever in the MIDDLE. The middle pair fall back to their head joints, so the
 * edge brick's neighbour is now hanging FROM it — and ARCHING_DESIGN's trap 3 says an abutment
 * that rests on the piece it is abutting is not an abutment, it is two bricks propping each other
 * over open air. Slice 1's gate therefore REFUSES the arch, correctly, and the springing goes back
 * to cantilevering at 1.63 of capacity. Worse, the two hangers are each other's support, which is
 * a two-node cycle: SolveLoads strands them both, and the brick above resting only on the pair
 * loses its path to the ground too. Slice 2 supersedes that refusal by GROUPING — contiguous
 * incomplete-seat pieces form a group through their head joints, and the group is abutted when it
 * has a live seat on BOTH sides of its centre — which re-seats the hangers onto joints OUTSIDE the
 * group and is acyclic by construction.
 *
 * THE RED CLAIM IS THE ROUTING, NOT A NUMBER. Today the two middle bricks read Stranded and a
 * wedge above them reads Falling; the cantilevering springings then break and the cascade takes
 * the wall. So the outcome assertion is a count of joints that FAILED UNDER LOAD, and the
 * mechanism assertion is that no piece is left unrouted. Neither is a displacement: two pieces can
 * sever and stay resting exactly where they were, so how far anything moved would say nothing.
 *
 * WHAT THE SPRINGING MUST READ, AND IT IS DERIVED HERE RATHER THAN COPIED. Every load figure comes
 * out of the INTACT wall's own solve: the four group bricks each carry a column, and after the cut
 * every one of those columns has to leave through the two surviving seats, because there is nothing
 * else left for it to go through. That is conservation and not a model — it holds whatever rule
 * slice 2 uses to divide the group's load — so the sum of the two springing forces is pinned to
 * 1e-9 and each one to 2%, which is slack for the wall's mild left-right asymmetry and for nothing
 * else.
 *
 * AND IT DISAGREES WITH ARCHING_DESIGN.md's OWN PREDICTION, WHICH IS SAID OUT LOUD RATHER THAN
 * TUNED AWAY. The design predicts a springing utilisation of about 0.036. Four columns through two
 * seats gives 0.0284, and 0.036 is what FIVE columns through two seats gives — i.e. the design's
 * figure is the arithmetic for a hole one cell WIDER than the one it describes, because an N-cell
 * cut leaves N - 1 unseated bricks and 2 half-seated ones, not N and 2. The number asserted below
 * is the derived one. If the implementation lands on 0.036 this test fails, and that is deliberate:
 * whichever of the two is wrong should be found here.
 *
 * THE UTILISATION IDENTITY IS WRITTEN TO SURVIVE SLICE 3. The tight claims are that the joint's
 * PUBLISHED moment sits exactly on the kern edge (sigma_b == |sigma_n|), that peak tension is
 * exactly zero, and that GetConnectionUtilisation agrees with ordinary beam theory on the force and
 * moment the solver reports. All three still hold once slice 3 puts a horizontal thrust into the
 * springing force; only "compression is the governing axis" would not, so that is printed rather
 * than asserted.
 *
 * TWO GATE ROWS THAT NO SLICE 1 TEST COULD CARRY. ARCHING_DESIGN records that gates 1 and 3 are
 * inert under any slice 1 fixture, and slice 2 is where they stop being:
 *
 *   - GATE 1, COMPLETE GEOMETRY, is what keeps both fuzz generators alive. They emit structures
 *     with no positions at all — 12,000 and 8,000 cases, and the only property tests over routing
 *     this project has — so a group rule that fired without geometry would put every one of them
 *     against an oracle that has never heard of an arch. The row below builds the exact topology of
 *     a spanned hole with nobody's position known and asserts it routes as it does today, to the
 *     last bit: the mutually-propping pair stays Stranded and their head joints carry exactly zero.
 *   - GATE 3, OUTSIDE THE KERN, catches the version of the cap written without the min. That row
 *     could not bite in slice 1's file because its oracle was built from the solver's own published
 *     moment, and an unconditional scale changes that moment — so the oracle moved with the defect.
 *     Here the expected moment is rebuilt from the fixture's own 0.5 cm lever arm instead.
 *
 * NEEDS A TICKING WORLD: NO. FStructure is plain arithmetic over a graph and Layout is plain
 * arithmetic over boxes; nothing here needs an actor, a tick or a renderer.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureSpannedHoleTest,
	"DestructionGame.Core.Structure.AHoleWiderThanOneBrickIsSpanned",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureSpannedHoleTest::RunTest(const FString& Parameters)
{
	using namespace StructureArchingTestSupport;
	using namespace StructureSpannedHoleTestSupport;
	using namespace StaircaseWallTestSupport;

	/*
	 * THE EXPECTED NUMBERS ARE RATIOS OF PUBLISHED STRENGTHS, so they mean what they say only
	 * while the profile still carries the figures they were derived against. Asserted rather
	 * than imported: a test that read the profile would agree with a wrong profile.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean f_x1 = 0.7 MPa (re-anchor 2026-08-13), the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.7);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	/*
	 * ===================================================================================
	 * PART 1 — THE INTACT WALL, AND THE FOUR COLUMNS IT HANDS THE HOLE.
	 * ===================================================================================
	 *
	 * Everything the cut wall must carry is measured HERE, before anything is deleted. The bond
	 * above the spanned course is untouched by the cut — every brick up there still rests on its
	 * own two seats and still splits by area — so each group brick's column is the same number in
	 * both walls, and the only question the cut asks is which joints it leaves by.
	 */
	FBrickLayout Intact;

	if (!RunningBond(ArchWallSpec(), Intact) || Intact.Boxes.Num() != ArchWallPieceCount)
	{
		AddError(FString::Printf(
			TEXT("FIXTURE: a flush 7 x 30 wall should lay as %d pieces, got %d"),
			ArchWallPieceCount, Intact.Boxes.Num()));

		return true;
	}

	TestTrue(
		TEXT("FIXTURE: the laid wall must know where every piece and every joint is, or every "
			 "moment below is silently zero and this measures nothing"),
		Intact.Structure.HasCompleteGeometry());

	Intact.Structure.SolveLoads();

	const double GroupBrickXCm[4] =
	{
		LeftSpringingBrickXCm, LeftHangingBrickXCm, RightHangingBrickXCm, RightSpringingBrickXCm
	};

	double ExpectedSpringingSumUu = 0.0;
	bool bColumnsMeasured = true;

	for (const double BrickXCm : GroupBrickXCm)
	{
		const int32 Brick = StaircasePieceAt(
			Intact.Boxes, BrickXCm, ArchWallCourseZCm(SpannedCourse));

		if (Brick == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: the intact wall has no brick at x %g in course %d"),
				BrickXCm, SpannedCourse));

			bColumnsMeasured = false;
			continue;
		}

		/*
		 * TWO SEATS EACH, WHICH IS WHAT MAKES THIS THE BASELINE. An intact running-bond brick
		 * rests on a pair of patches whose area-weighted centroid is its own centre of mass, so
		 * e = 0 exactly and no joint in this wall carries a moment at all.
		 */
		const int32 Seats = IntactSeatsUnder(Intact.Structure, Brick);

		TestEqual(
			FString::Printf(
				TEXT("FIXTURE: the intact brick at x %g must rest on two seats, it rests on %d"),
				BrickXCm, Seats),
			Seats, 2);

		const double ColumnUu = TotalCarriedUu(Intact.Structure, Brick);

		AddInfo(FString::Printf(
			TEXT("INTACT: the column standing on the brick at x %g is %s uu (%.4f brick weights)"),
			BrickXCm, *Bits(ColumnUu), ColumnUu / BrickWeightUu));

		ExpectedSpringingSumUu += ColumnUu;
	}

	if (!bColumnsMeasured)
	{
		return true;
	}

	/*
	 * WHAT EACH SPRINGING MUST THEN READ, DERIVED AND NOT COPIED. Half the group's load on one
	 * 105.0625 cm2 patch, held at the kern edge so peak compression is exactly twice the mean:
	 *
	 *     2 * (F / (105.0625 * 10000)) / 10 MPa
	 *
	 * The halving is the fixture's own symmetry rather than a modelling claim — the group is four
	 * near-identical columns with a surviving seat at each end — and the 2% below is slack for the
	 * wall's mild left-right asymmetry and for nothing else. The SUM is exact and is asserted as
	 * such; only the division between the two ends is approximate.
	 */
	const double ExpectedSpringingForceUu = ExpectedSpringingSumUu / 2.0;

	const double ExpectedSpringingUtilisation =
		2.0 * (ExpectedSpringingForceUu / (HalfSeatAreaSqCm * ForceUnitsPerMPaPerSqCm))
			/ GeneralPurposeMortar.CompressiveStrengthMPa;

	constexpr double SpringingTolerance = 0.02;

	AddInfo(FString::Printf(
		TEXT("DERIVED: the group's four columns come to %s uu, so each springing carries %s uu ")
		TEXT("(%.4f brick weights) and reads %s. ARCHING_DESIGN.md predicts %g, which is the ")
		TEXT("arithmetic for FIVE columns through two seats — an N-cell cut leaves N-1 unseated ")
		TEXT("bricks and 2 half-seated ones, not N and 2. THE DERIVED FIGURE IS WHAT IS ASSERTED."),
		*Bits(ExpectedSpringingSumUu), *Bits(ExpectedSpringingForceUu),
		ExpectedSpringingForceUu / BrickWeightUu, *Bits(ExpectedSpringingUtilisation),
		DesignSpringingUtilisation));

	/*
	 * ===================================================================================
	 * PART 2 — THE THREE-CELL CUT.
	 * ===================================================================================
	 */

	FBrickLayout Cut;

	if (!RunningBond(ArchWallSpec(), Cut) || Cut.Boxes.Num() != ArchWallPieceCount)
	{
		AddError(TEXT("FIXTURE: the second copy of the wall did not lay"));
		return true;
	}

	TArray<int32> DeletedPieces;

	for (int32 Cell = 0; Cell < CutCellCount; ++Cell)
	{
		const double BrickXCm = ArchWallOddBrickXCm(FirstCutBrickIndex + Cell);
		const int32 Piece = StaircasePieceAt(Cut.Boxes, BrickXCm, ArchWallCourseZCm(CutCourse));

		if (Piece == INDEX_NONE || !Cut.Structure.RemovePiece(Piece))
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: there should be a brick at x %g in course %d to delete"),
				BrickXCm, CutCourse));

			return true;
		}

		DeletedPieces.Add(Piece);
	}

	Cut.Structure.SolveLoads();

	// --- the shape the cut leaves, arbitrated against the producer ------------------------

	const int32 LeftSpringingBrick = StaircasePieceAt(
		Cut.Boxes, LeftSpringingBrickXCm, ArchWallCourseZCm(SpannedCourse));
	const int32 RightSpringingBrick = StaircasePieceAt(
		Cut.Boxes, RightSpringingBrickXCm, ArchWallCourseZCm(SpannedCourse));
	const int32 LeftHangingBrick = StaircasePieceAt(
		Cut.Boxes, LeftHangingBrickXCm, ArchWallCourseZCm(SpannedCourse));
	const int32 RightHangingBrick = StaircasePieceAt(
		Cut.Boxes, RightHangingBrickXCm, ArchWallCourseZCm(SpannedCourse));

	if (LeftSpringingBrick == INDEX_NONE || RightSpringingBrick == INDEX_NONE
		|| LeftHangingBrick == INDEX_NONE || RightHangingBrick == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the four bricks over the hole were not all found"));
		return true;
	}

	/*
	 * THE MIDDLE PAIR HAVE NO SEAT AT ALL, and that is what makes this hole different in kind
	 * from a one-brick one rather than merely wider. A piece with one seat is a cantilever the
	 * cap can relieve; a piece with none has fallen back to the head tier and is hanging.
	 */
	for (const int32 Hanger : { LeftHangingBrick, RightHangingBrick })
	{
		const int32 Seats = IntactSeatsUnder(Cut.Structure, Hanger);

		TestEqual(
			FString::Printf(
				TEXT("FIXTURE: the brick at x %g must be left with NO seat at all, it has %d"),
				Cut.Boxes[Hanger].CentreCm.X, Seats),
			Seats, 0);
	}

	/*
	 * THE TWO HANGERS ARE CONTIGUOUS WITH THE SPRINGINGS THROUGH INTACT HEAD JOINTS, which is
	 * what a GROUP is made of. Checked rather than assumed, so a bond that quietly stopped
	 * emitting a head joint fails as a fixture instead of as physics.
	 */
	const int32 GroupChain[4] =
	{
		LeftSpringingBrick, LeftHangingBrick, RightHangingBrick, RightSpringingBrick
	};

	for (int32 Link = 0; Link + 1 < 4; ++Link)
	{
		const int32 Head = JointBetweenPieces(
			Cut.Structure, GroupChain[Link], GroupChain[Link + 1]);

		TestTrue(
			FString::Printf(
				TEXT("FIXTURE: pieces %d and %d must share an INTACT head joint or they are not ")
				TEXT("one group"),
				GroupChain[Link], GroupChain[Link + 1]),
			Head != INDEX_NONE
				&& Cut.Structure.GetJointRole(Head, GroupChain[Link]) == EJointRole::Head
				&& !Cut.Structure.GetConnection(Head).HasGiven());
	}

	/*
	 * AND TRAP 3 IS LIVE, WHICH IS EXACTLY WHY SLICE 1 CANNOT ANSWER THIS FIXTURE. The springing's
	 * neighbour on the eccentric side is a hanger, and a hanger's only supports are its head
	 * joints — so the springing appears among them. Slice 1's abutment test refuses the arch on
	 * precisely that fact, correctly, and slice 2 is what supersedes it by grouping. Asserted here
	 * so that the reason this test is red is recorded in the test rather than only in the design.
	 */
	{
		int32 SpringingsTheHangerRestsOn = 0;

		for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
		{
			const FConnection& Connection = Cut.Structure.GetConnection(Joint);

			if (Connection.HasGiven())
			{
				continue;
			}

			const bool bTouchesHanger =
				Connection.PieceA == LeftHangingBrick || Connection.PieceB == LeftHangingBrick;
			const bool bTouchesSpringing =
				Connection.PieceA == LeftSpringingBrick || Connection.PieceB == LeftSpringingBrick;

			if (bTouchesHanger && bTouchesSpringing)
			{
				++SpringingsTheHangerRestsOn;
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("FIXTURE: the hanger and the springing must share exactly one intact joint — ")
				TEXT("the head joint slice 1 refuses to arch against; they share %d"),
				SpringingsTheHangerRestsOn),
			SpringingsTheHangerRestsOn, 1);
	}

	/*
	 * ===================================================================================
	 * PART 3 — THE ROUTING CLAIM: NOTHING IS LEFT UNROUTED.
	 * ===================================================================================
	 *
	 * TODAY THIS IS THE FIRST THING THAT FAILS. The two hangers are each other's support, which
	 * SolveLoads reports as a knot and strands; the brick in the course above resting only on the
	 * pair then loses its own path to the ground and reads Falling, and the wedge climbs. A wall
	 * that spans its hole has every piece it still contains either on the earth or reaching it.
	 *
	 * REMOVED PIECES ARE SKIPPED, AND FORGETTING TO WOULD MAKE THIS UNSATISFIABLE. GetPieceSupport
	 * answers Falling for a piece that has been deleted — deliberately, since nothing is holding up
	 * something that is not there — so the three bricks the player took would always be counted.
	 */
	{
		int32 Stranded = 0;
		int32 Falling = 0;
		int32 FirstUnrouted = INDEX_NONE;

		for (int32 Piece = 0; Piece < Cut.Structure.NumPieces(); ++Piece)
		{
			if (Cut.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Cut.Structure.GetPieceSupport(Piece);

			if (Support == EPieceSupport::Stranded)
			{
				++Stranded;
			}
			else if (Support == EPieceSupport::Falling)
			{
				++Falling;
			}
			else
			{
				continue;
			}

			if (FirstUnrouted == INDEX_NONE)
			{
				FirstUnrouted = Piece;
			}
		}

		AddInfo(FString::Printf(
			TEXT("after a three-cell cut %d of %d pieces are Stranded and %d are Falling; the ")
			TEXT("first is piece %d"),
			Stranded, Cut.Structure.NumPieces() - DeletedPieces.Num(), Falling, FirstUnrouted));

		TestEqual(
			FString::Printf(
				TEXT("a spanned hole leaves NO piece stranded in a knot; %d are"), Stranded),
			Stranded, 0);

		TestEqual(
			FString::Printf(
				TEXT("and none of the wall it did not delete falling; %d are"), Falling),
			Falling, 0);

		TestTrue(
			FString::Printf(
				TEXT("the two bricks with no seat must be routed to the ground through the group, ")
				TEXT("not left hanging — they read %d and %d (Supported is %d)"),
				static_cast<int32>(Cut.Structure.GetPieceSupport(LeftHangingBrick)),
				static_cast<int32>(Cut.Structure.GetPieceSupport(RightHangingBrick)),
				static_cast<int32>(EPieceSupport::Supported)),
			Cut.Structure.GetPieceSupport(LeftHangingBrick) == EPieceSupport::Supported
				&& Cut.Structure.GetPieceSupport(RightHangingBrick) == EPieceSupport::Supported);
	}

	/*
	 * ===================================================================================
	 * PART 4 — THE TWO SPRINGINGS, WHICH IS WHERE THE GROUP'S LOAD LEAVES.
	 * ===================================================================================
	 */

	struct FSpringingCase
	{
		const TCHAR* Description;
		int32 Brick;
		double SeatXCm;

		/** Which way it overhangs: +1 is toward increasing X, i.e. into the hole. */
		double EccentricSign;
	};

	const TArray<FSpringingCase> Springings = {
		{ TEXT("the LEFT springing"), LeftSpringingBrick, LeftSpringingSeatXCm, +1.0 },
		{ TEXT("the RIGHT springing"), RightSpringingBrick, RightSpringingSeatXCm, -1.0 },
	};

	double MeasuredSpringingSumUu = 0.0;

	for (const FSpringingCase& Case : Springings)
	{
		const int32 Seat = StaircasePieceAt(
			Cut.Boxes, Case.SeatXCm, ArchWallCourseZCm(CutCourse));

		const int32 Springing = TheOneSeatUnder(Cut.Structure, Case.Brick);

		if (Seat == INDEX_NONE || Springing == INDEX_NONE
			|| Springing != JointBetweenPieces(Cut.Structure, Case.Brick, Seat))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: it must rest on EXACTLY ONE bed joint, and on the brick at x %g"),
				Case.Description, Case.SeatXCm));

			continue;
		}

		const FConnection& Bed = Cut.Structure.GetConnection(Springing);

		/*
		 * THE SPRINGING IS THE SAME HALF SEAT SLICE 1 ALREADY ARCHES — 10.25 x 10.25, loaded
		 * 5.625 cm off its own centroid, against a kern that reaches 1.7083 cm. Arbitrated against
		 * the producer rather than assumed, so the number and the reason for it fail together.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the surviving seat should be %g cm2 with half-extents (%g, %g); ")
				TEXT("MakeInterface emitted %g cm2 with (%g, %g)"),
				Case.Description, HalfSeatAreaSqCm, HalfSeatHalfExtentCm, HalfSeatHalfExtentCm,
				Bed.InterfaceAreaSqCm,
				Bed.InterfaceHalfExtentCm.X, Bed.InterfaceHalfExtentCm.Y),
			FMath::IsNearlyEqual(Bed.InterfaceAreaSqCm, HalfSeatAreaSqCm, 1.0e-9)
				&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.X, HalfSeatHalfExtentCm, 1.0e-9)
				&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.Y, HalfSeatHalfExtentCm, 1.0e-9));

		const double EccentricityCm =
			Cut.Boxes[Case.Brick].CentreCm.X - Bed.InterfaceCentreCm.X;

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: it should overhang its seat by %g cm INTO the hole, it ")
				TEXT("overhangs %g"),
				Case.Description, HalfSeatEccentricityCm, EccentricityCm),
			FMath::IsNearlyEqual(
				EccentricityCm, Case.EccentricSign * HalfSeatEccentricityCm, 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: %g cm of eccentricity must be OUTSIDE the kern's %g cm, or no ")
				TEXT("part of the face is opening and there is nothing to relieve"),
				Case.Description, FMath::Abs(EccentricityCm),
				KernFromHalfExtentCm(HalfSeatHalfExtentCm)),
			FMath::Abs(EccentricityCm) > KernFromHalfExtentCm(HalfSeatHalfExtentCm));

		// --- what it reads ------------------------------------------------------------------

		const FVector ForceUu = Cut.Structure.GetConnectionForce(Springing);
		const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(Springing);

		const double Utilisation = Cut.Structure.GetConnectionUtilisation(Springing);

		const FBedJointReading Published = ReadBedJoint(
			ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
			GeneralPurposeMortar);

		const double Arched = ArchedUtilisation(Published, GeneralPurposeMortar);

		MeasuredSpringingSumUu += FMath::Abs(ForceUu.Z);

		AddInfo(FString::Printf(
			TEXT("%s: joint %d carries %s uu (%.4f brick weights) and publishes %s uu.cm; reads ")
			TEXT("%s (tension %s, compression %s, shear %s); an arch reads %s, the derivation ")
			TEXT("above says %s"),
			Case.Description, Springing, *Bits(FMath::Abs(ForceUu.Z)),
			FMath::Abs(ForceUu.Z) / BrickWeightUu, *Bits(MomentUuCm.Size()), *Bits(Utilisation),
			*Bits(Published.TensionUtilisation), *Bits(Published.CompressionUtilisation),
			*Bits(Published.ShearUtilisation), *Bits(Arched),
			*Bits(ExpectedSpringingUtilisation)));

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the seat must be in COMPRESSION, sigma_n is %s MPa"),
				Case.Description, *Bits(Published.NormalStressMPa)),
			Published.NormalStressMPa < 0.0);

		/*
		 * THE THRUST LINE SITS ON THE KERN EDGE, and that is what an arch IS. Written on the
		 * PUBLISHED moment because ARCHING_DESIGN requires the capped value to be both what
		 * travels and what the joint reads — a solver that relieved the stress it evaluates while
		 * handing an unrelieved moment to the course below would be the second, private quantity
		 * Structure.cpp's own comment exists to forbid, and this is the row that sees it.
		 *
		 * SURVIVES SLICE 3: a horizontal thrust is shear on a bed joint and changes neither the
		 * normal stress nor the moment.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: the moment it publishes must sit on the kern edge — sigma_b %s against ")
				TEXT("|sigma_n| %s"),
				Case.Description, *Bits(Published.BendingStressMPa),
				*Bits(FMath::Abs(Published.NormalStressMPa))),
			FMath::Abs(Published.BendingStressMPa - FMath::Abs(Published.NormalStressMPa))
				<= 1.0e-12 * FMath::Abs(Published.NormalStressMPa));

		TestTrue(
			FString::Printf(
				TEXT("%s: at the cap the opened edge carries exactly nothing; tension reads %s"),
				Case.Description, *Bits(Published.TensionUtilisation)),
			Published.TensionUtilisation == 0.0);

		/*
		 * AND THE READING AGREES WITH BEAM THEORY ON THE FORCE AND MOMENT THE SOLVER REPORTS,
		 * which is what pins the AXIS as well as the number. ComputeUtilisation returns the worst
		 * of three, so a fixture aimed at compression would silently measure shear the day shear
		 * exceeded it — as it will at slice 3. Comparing against the oracle's own worst is the
		 * form of that claim which survives.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: it must read what beam theory says, %s, and it reads %s"),
				Case.Description, *Bits(Published.Worst), *Bits(Utilisation)),
			FMath::Abs(Utilisation - Published.Worst)
				<= 1.0e-12 * FMath::Max(Published.Worst, 1.0e-12));

		/*
		 * NOT THE MOMENT SIMPLY ZEROED, which is out by exactly a factor of two on the compression
		 * axis — |sigma_n| against 2|sigma_n| — and would otherwise look like a plausible small
		 * number sitting next to a plausible small number.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: the compression axis must read 2|sigma_n|/f_c = %s, not the %s a deleted ")
				TEXT("moment gives; it reads %s"),
				Case.Description, *Bits(Arched),
				*Bits(FMath::Abs(Published.NormalStressMPa)
					/ GeneralPurposeMortar.CompressiveStrengthMPa),
				*Bits(Published.CompressionUtilisation)),
			FMath::Abs(Published.CompressionUtilisation - Arched) <= 1.0e-12 * Arched);

		/* And against the load the intact wall says has to come through here. */
		TestTrue(
			FString::Printf(
				TEXT("%s: the group's four columns through two seats put %s on this joint, it ")
				TEXT("reads %s"),
				Case.Description, *Bits(ExpectedSpringingUtilisation),
				*Bits(Published.CompressionUtilisation)),
			FMath::Abs(Published.CompressionUtilisation - ExpectedSpringingUtilisation)
				<= SpringingTolerance * ExpectedSpringingUtilisation);
	}

	/*
	 * CONSERVATION, AND IT IS THE STRONGEST CLAIM IN THIS FILE because it does not depend on the
	 * rule slice 2 picks for dividing the group's load. Everything standing on the four bricks
	 * over the hole has exactly two joints left to reach the ground through, so the two springing
	 * forces must add up to the four columns the INTACT wall measured — to the last bits, since
	 * nothing above the spanned course changed and the accumulation up there is the same
	 * arithmetic in the same order.
	 */
	AddInfo(FString::Printf(
		TEXT("the two springings carry %s uu between them; the intact wall's four columns come to ")
		TEXT("%s uu"),
		*Bits(MeasuredSpringingSumUu), *Bits(ExpectedSpringingSumUu)));

	TestTrue(
		FString::Printf(
			TEXT("everything over the hole must leave through the two springings: %s uu against ")
			TEXT("the intact wall's %s uu"),
			*Bits(MeasuredSpringingSumUu), *Bits(ExpectedSpringingSumUu)),
		FMath::Abs(MeasuredSpringingSumUu - ExpectedSpringingSumUu)
			<= 1.0e-9 * ExpectedSpringingSumUu);

	/*
	 * ===================================================================================
	 * PART 5 — AND THE WALL STANDS.
	 * ===================================================================================
	 *
	 * COUNTED BY BREAK PASS AND NOT BY HasGiven. GetBreakPass's contract spells the encoding out:
	 * a joint that went WITH A REMOVED PIECE has HasGiven true and a pass of INDEX_NONE, because it
	 * never snapped. The player's own click always takes joints with it, so a HasGiven count could
	 * never reach zero and would make this unsatisfiable however correct the physics got. What "the
	 * wall stood" means is that nothing FAILED UNDER LOAD.
	 */
	const int32 BreakingPasses = Cut.Structure.SolveAndBreak();

	int32 JointsBrokenByLoad = 0;
	int32 JointsGoneWithTheCut = 0;

	for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
	{
		if (!Cut.Structure.GetConnection(Joint).HasGiven())
		{
			continue;
		}

		if (Cut.Structure.GetBreakPass(Joint) != INDEX_NONE)
		{
			++JointsBrokenByLoad;
		}
		else
		{
			++JointsGoneWithTheCut;
		}
	}

	AddInfo(FString::Printf(
		TEXT("after a three-cell cut the cascade ran %d passes; %d of %d joints failed under load ")
		TEXT("and %d went with the three deleted bricks"),
		BreakingPasses, JointsBrokenByLoad, Cut.Structure.NumConnections(), JointsGoneWithTheCut));

	TestEqual(
		FString::Printf(
			TEXT("a three-cell interior cut must break NOTHING; the cascade ran %d passes"),
			BreakingPasses),
		BreakingPasses, 0);

	TestEqual(
		FString::Printf(
			TEXT("and must leave every joint it did not delete intact; %d failed under load"),
			JointsBrokenByLoad),
		JointsBrokenByLoad, 0);

	TestEqual(
		FString::Printf(
			TEXT("only the three deleted bricks' own %d joints may leave the graph; %d did"),
			JointsLostWithTheCut, JointsGoneWithTheCut),
		JointsGoneWithTheCut, JointsLostWithTheCut);

	/*
	 * ===================================================================================
	 * PART 6 — GATE 1: A GEOMETRY-FREE STRUCTURE ROUTES BIT-IDENTICALLY.
	 * ===================================================================================
	 *
	 *      [ A ][ B ][ C ][ D ]      four pieces in a row, joined head to head
	 *      [pad]          [pad]      A and D each have a bed joint to the earth
	 *
	 * This is the exact topology of a spanned hole — two ends with seats, two middles with none,
	 * every one of them contiguous through head joints — and NOBODY'S POSITION IS KNOWN. B and C
	 * therefore fall back to the head tier and become each other's support, which is a two-node
	 * cycle: SolveLoads strands both and their joints carry exactly nothing.
	 *
	 * THAT ANSWER MUST NOT CHANGE, and it is the whole reason gate 1 exists. Both fuzz generators
	 * emit structures with no geometry — 12,000 and 8,000 cases, and the only property tests over
	 * routing this project has — so a group rule that fired here would set every one of them
	 * against an oracle that knows nothing about arches, and they would go dark quietly. The
	 * assertions are exact rather than approximate because "unreached" and "reached and inert" are
	 * different claims and only equality tells them apart.
	 */
	{
		FStructure Structure;

		const int32 LeftPad = Structure.AddPiece(BrickMassKg, true);
		const int32 A = Structure.AddPiece(BrickMassKg, false);
		const int32 B = Structure.AddPiece(BrickMassKg, false);
		const int32 C = Structure.AddPiece(BrickMassKg, false);
		const int32 D = Structure.AddPiece(BrickMassKg, false);
		const int32 RightPad = Structure.AddPiece(BrickMassKg, true);

		const auto AddBed = [&Structure](int32 Below, int32 Above) -> int32
		{
			FConnection Bed;
			Bed.PieceA = Below;
			Bed.PieceB = Above;
			Bed.InterfaceNormal = FVector::ZAxisVector;
			Bed.InterfaceAreaSqCm = HalfSeatAreaSqCm;
			Bed.Strength = GeneralPurposeMortar;
			return Structure.AddConnection(Bed);
		};

		const auto AddHead = [&Structure](int32 First, int32 Second) -> int32
		{
			FConnection Head;
			Head.PieceA = First;
			Head.PieceB = Second;
			Head.InterfaceNormal = FVector::XAxisVector;
			Head.InterfaceAreaSqCm = BrickWidthCm * BrickHeightCm;
			Head.Strength = GeneralPurposeMortar;
			return Structure.AddConnection(Head);
		};

		const int32 LeftSeat = AddBed(LeftPad, A);
		const int32 RightSeat = AddBed(RightPad, D);

		const int32 HeadAB = AddHead(A, B);
		const int32 HeadBC = AddHead(B, C);
		const int32 HeadCD = AddHead(C, D);

		TestTrue(
			TEXT("GATE 1: FIXTURE: nobody's position is known, so HasCompleteGeometry must be false"),
			!Structure.HasCompleteGeometry());

		Structure.SolveLoads();

		AddInfo(FString::Printf(
			TEXT("GATE 1: A %d, B %d, C %d, D %d; the seats carry %s and %s uu, the head joints ")
			TEXT("%s, %s and %s uu"),
			static_cast<int32>(Structure.GetPieceSupport(A)),
			static_cast<int32>(Structure.GetPieceSupport(B)),
			static_cast<int32>(Structure.GetPieceSupport(C)),
			static_cast<int32>(Structure.GetPieceSupport(D)),
			*Bits(Structure.GetConnectionForce(LeftSeat).Size()),
			*Bits(Structure.GetConnectionForce(RightSeat).Size()),
			*Bits(Structure.GetConnectionForce(HeadAB).Size()),
			*Bits(Structure.GetConnectionForce(HeadBC).Size()),
			*Bits(Structure.GetConnectionForce(HeadCD).Size())));

		TestTrue(
			TEXT("GATE 1: the two seated pieces must still be Supported"),
			Structure.GetPieceSupport(A) == EPieceSupport::Supported
				&& Structure.GetPieceSupport(D) == EPieceSupport::Supported);

		TestTrue(
			FString::Printf(
				TEXT("GATE 1: with no geometry there is no group, so the mutually-propping pair ")
				TEXT("must STILL be Stranded — they read %d and %d (Stranded is %d)"),
				static_cast<int32>(Structure.GetPieceSupport(B)),
				static_cast<int32>(Structure.GetPieceSupport(C)),
				static_cast<int32>(EPieceSupport::Stranded)),
			Structure.GetPieceSupport(B) == EPieceSupport::Stranded
				&& Structure.GetPieceSupport(C) == EPieceSupport::Stranded);

		/*
		 * AND THE LOADS ARE THE SAME BITS. A seat carries its own piece's weight and nothing else,
		 * because the pair above it conducts nothing; a head joint carries exactly zero. Written
		 * with == rather than a tolerance: a re-seat that moved any load at all would have to move
		 * it through one of these five joints.
		 */
		TestTrue(
			FString::Printf(
				TEXT("GATE 1: each seat must carry exactly one brick weight, %s uu; they carry %s ")
				TEXT("and %s"),
				*Bits(BrickWeightUu),
				*Bits(FMath::Abs(Structure.GetConnectionForce(LeftSeat).Z)),
				*Bits(FMath::Abs(Structure.GetConnectionForce(RightSeat).Z))),
			FMath::Abs(Structure.GetConnectionForce(LeftSeat).Z) == BrickWeightUu
				&& FMath::Abs(Structure.GetConnectionForce(RightSeat).Z) == BrickWeightUu);

		for (const int32 Head : { HeadAB, HeadBC, HeadCD })
		{
			TestTrue(
				FString::Printf(
					TEXT("GATE 1: head joint %d must carry exactly nothing, it carries %s uu"),
					Head, *Bits(Structure.GetConnectionForce(Head).Size())),
				Structure.GetConnectionForce(Head) == FVector::ZeroVector);

			TestTrue(
				FString::Printf(
					TEXT("GATE 1: head joint %d must carry no moment either, it carries %s uu.cm"),
					Head, *Bits(Structure.GetConnectionMoment(Head).Size())),
				Structure.GetConnectionMoment(Head) == FVector::ZeroVector);
		}

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			const double Utilisation = Structure.GetConnectionUtilisation(Joint);

			TestTrue(
				FString::Printf(
					TEXT("GATE 1: joint %d must read a finite utilisation, it reads %s"),
					Joint, *Bits(Utilisation)),
				FMath::IsFinite(Utilisation));
		}
	}

	/*
	 * ===================================================================================
	 * PART 7 — GATE 3: INSIDE THE KERN, AGAINST AN ORACLE THE CAP CANNOT MOVE.
	 * ===================================================================================
	 *
	 * A brick sitting 1 cm off a pad it overlaps almost completely loads a 20.5 cm deep patch
	 * 0.5 cm off centre, against a kern that reaches 3.4167 cm. No part of the face is opening, so
	 * there is nothing for an arch to relieve — and min(1, |sigma_n|/sigma_b) is 1 here, which
	 * means a correct implementation is inert by construction.
	 *
	 * WHAT THIS ROW CATCHES IS THE VERSION WRITTEN WITHOUT THE MIN, and it can only catch it
	 * because the expected moment is rebuilt from the fixture's own 0.5 cm lever arm. An oracle
	 * fed GetConnectionMoment moves WITH the defect — the scale changes the published moment, the
	 * oracle reads the changed moment, and the two agree on a wrong answer. That is exactly why
	 * ARCHING_DESIGN records gate 3 as unpunishable by any slice 1 test.
	 *
	 * The unconditional scale is |sigma_n|/sigma_b = 6.83 here, which would put sigma_b onto
	 * |sigma_n| and read 2|sigma_n|/f_c — 2.5387e-4 against the correct 1.4551e-4. Both are tiny,
	 * both are on the compression axis, and only an independently derived expectation separates
	 * them.
	 */
	{
		FStructure Structure;

		constexpr double PadZCm = BrickHeightCm / 2.0;
		constexpr double UpperZCm = PadZCm + CoursePitchCm;
		constexpr double OffsetCm = 1.0;

		const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, PadZCm);
		const FPieceBox LoadedBox = BrickBoxAt(OffsetCm, 0.0, UpperZCm);
		const FPieceBox NeighbourBox = BrickBoxAt(OffsetCm + BrickPitchCm, 0.0, UpperZCm);
		const FPieceBox FarPadBox = BrickBoxAt(OffsetCm + BrickPitchCm, 0.0, PadZCm);

		const int32 Pad = Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm);
		const int32 Loaded = Structure.AddPiece(BrickMassKg, false, LoadedBox.CentreCm);
		const int32 FarPad = Structure.AddPiece(BrickMassKg, true, FarPadBox.CentreCm);
		const int32 Neighbour = Structure.AddPiece(BrickMassKg, false, NeighbourBox.CentreCm);

		FConnection Bed;
		MakeInterface(Pad, PadBox, Loaded, LoadedBox, MortarJointCm, GeneralPurposeMortar, Bed);
		const int32 Subject = Structure.AddConnection(Bed);

		FConnection FarBed;
		MakeInterface(
			FarPad, FarPadBox, Neighbour, NeighbourBox,
			MortarJointCm, GeneralPurposeMortar, FarBed);
		Structure.AddConnection(FarBed);

		FConnection Head;
		MakeInterface(
			Loaded, LoadedBox, Neighbour, NeighbourBox,
			MortarJointCm, GeneralPurposeMortar, Head);
		Structure.AddConnection(Head);

		if (Subject == INDEX_NONE)
		{
			AddError(TEXT("GATE 3: FIXTURE: the bed joint was refused"));
			return true;
		}

		Structure.SolveLoads();

		const FConnection& Joint = Structure.GetConnection(Subject);
		const FVector ForceUu = Structure.GetConnectionForce(Subject);

		const double EccentricityCm = LoadedBox.CentreCm.X - Joint.InterfaceCentreCm.X;

		TestTrue(
			FString::Printf(
				TEXT("GATE 3: FIXTURE: %g cm of eccentricity must be INSIDE the kern's %g cm, or ")
				TEXT("this row is measuring the arch instead of refusing it"),
				EccentricityCm, KernFromHalfExtentCm(Joint.InterfaceHalfExtentCm.X)),
			FMath::Abs(EccentricityCm) < KernFromHalfExtentCm(Joint.InterfaceHalfExtentCm.X));

		/*
		 * THE MOMENT THE JOINT MUST BE ANSWERING, BUILT HERE. One brick weight — nothing rests on
		 * this piece — acting through the eccentricity above. The load is off centre along X, so
		 * the moment is about Y; unlike the wall's square half seat this patch is 20.5 by 10.25,
		 * so its two in-plane moduli differ by a factor of four and putting it on the wrong axis
		 * would be visible rather than harmless.
		 */
		const FVector ExpectedMomentUuCm(
			0.0, FMath::Abs(ForceUu.Z) * FMath::Abs(EccentricityCm), 0.0);

		const FBedJointReading Expected = ReadBedJoint(
			ForceUu, ExpectedMomentUuCm, Joint.InterfaceHalfExtentCm, Joint.InterfaceAreaSqCm,
			GeneralPurposeMortar);

		const double Utilisation = Structure.GetConnectionUtilisation(Subject);
		const double IfScaledWithoutTheMin = ArchedUtilisation(Expected, GeneralPurposeMortar);

		AddInfo(FString::Printf(
			TEXT("GATE 3: the joint carries %s uu at %g cm, so sigma_n %s and sigma_b %s MPa — a ")
			TEXT("ratio of %s. It must read %s; scaling without the min reads %s. It reads %s"),
			*Bits(FMath::Abs(ForceUu.Z)), EccentricityCm,
			*Bits(Expected.NormalStressMPa), *Bits(Expected.BendingStressMPa),
			*Bits(FMath::Abs(Expected.NormalStressMPa) / Expected.BendingStressMPa),
			*Bits(Expected.Worst), *Bits(IfScaledWithoutTheMin), *Bits(Utilisation)));

		TestTrue(
			FString::Printf(
				TEXT("GATE 3: FIXTURE: inside the kern |sigma_n| exceeds sigma_b, so an ")
				TEXT("unconditional scale would MULTIPLY the bending stress — %s against %s"),
				*Bits(FMath::Abs(Expected.NormalStressMPa)), *Bits(Expected.BendingStressMPa)),
			FMath::Abs(Expected.NormalStressMPa) > Expected.BendingStressMPa);

		TestTrue(
			FString::Printf(
				TEXT("GATE 3: the arch must NOT fire — the fixture's own %g cm arm says %s, the ")
				TEXT("joint reads %s"),
				EccentricityCm, *Bits(Expected.Worst), *Bits(Utilisation)),
			FMath::Abs(Utilisation - Expected.Worst)
				<= 1.0e-12 * FMath::Max(Expected.Worst, 1.0e-12));

		TestTrue(
			FString::Printf(
				TEXT("GATE 3: and specifically it must not read the %s an unconditional scale ")
				TEXT("gives; it reads %s"),
				*Bits(IfScaledWithoutTheMin), *Bits(Utilisation)),
			FMath::Abs(Utilisation - IfScaledWithoutTheMin)
				> 1.0e-6 * IfScaledWithoutTheMin);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
