// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/Placement.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Integration test for the corner-return slice — CR-2a, DESIGN §8's 2026-09-15 corner
 * ruling, built the way a player builds.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE: an L-shaped wall laid ENTIRELY through
 * BuildMode::PlacePiece — a run along X, a rotated brick returning at its end, a run
 * along Y, and a staggered course LAPPED OVER THE CORNER on top of all three — snaps to
 * the intended pose at every step and STANDS under its own weight.
 *
 * WHY AN L AND NOT A STRAIGHT WALL. Every piece of CR-2a is needed before this building
 * can exist, and none of them alone builds it: the corner return places the first brick of
 * the second leg, the long-axis-aware running bond lets that leg grow along Y, and the
 * boxed joint inference is what makes the quoin a bonded corner rather than a perpend.
 * A straight wall (DemoBuildingTest) exercises none of the three.
 *
 * ASSERT ON THE MECHANISM AND THE OUTCOME, NEVER DISPLACEMENT — the same two families
 * DemoBuildingTest uses, for the same reason (DESIGN §4): two pieces can sever and stay
 * resting exactly in place, so a position check measures nothing.
 *   - MECHANISM: the per-step snap Kind and joints-formed sequence, the piece and
 *     connection counts, the profile mix, and the individual joints' profile / area /
 *     normal. Each binary and exact. THE QUOIN AND THE CORNER LAP LIVE HERE, not in the
 *     outcome — see the next paragraph.
 *   - OUTCOME: after SolveLoads, the EXACT support kind per piece — the six laid on the
 *     earth Grounded, the five above them Supported, and nothing Falling or Stranded. What
 *     this pins is that every course-1 brick reaches the earth THROUGH BEDS the builder
 *     formed: a bed that never formed leaves its brick with no load path.
 *
 * WHAT THE STANDS-CLAIM DOES *NOT* PIN, corrected after the CR-2a review called the
 * earlier wording false. The QUOIN (2-3) is a VERTICAL-normal joint, and SolveLoads routes
 * weight DOWN BEDS — so the quoin carries no vertical load and deleting it would change no
 * support kind in section SIX. Brick 3 rests on the earth either way. The quoin bonds the
 * two legs LATERALLY, which is what keeps the corner together under a horizontal push, and
 * it is pinned by the MECHANISM sections (THREE and FIVE) alone. The joint that genuinely
 * makes the return load-bearing FROM ABOVE is the course-1 corner brick's BED ONTO IT
 * (9-3), section SEVEN — that one is a horizontal face under a vertical load path, so its
 * utilisation after a solve is a positive number rather than a claim about a label.
 *
 * THE BUILD, STEP BY STEP (requested CENTRES handed to PlacePiece; HalfBrick =
 * (10.75,5.125,3.25) laid along X, HalfBrickRotated = (5.125,10.75,3.25) laid along Y;
 * the 22.5 x 11.25 x 7.5 coordinating grid of Core/Layout.h, and course 0 RESTING ON THE
 * GROUND at centre Z = 3.25 per the 2026-09-15 convention):
 *
 *   idx  requested centre        extent    grounded  -> Kind               joints
 *   0    ( 0.000,  0.000, 3.25)  X-long    yes       -> Free               0
 *   1    (22.500,  0.000, 3.25)  X-long    yes       -> BrickSameCourse    1  head ->0
 *   2    (45.000,  0.000, 3.25)  X-long    yes       -> BrickSameCourse    1  head ->1
 *   3    (61.875,  5.000, 3.25)  Y-long    yes       -> BrickCornerReturn  1  quoin->2
 *   4    (61.875, 28.000, 3.25)  Y-long    yes       -> BrickSameCourse    1  head ->3
 *   5    (61.875, 50.500, 3.25)  Y-long    yes       -> BrickSameCourse    1  head ->4
 *   6    (11.250,  0.000,10.75)  X-long    no        -> BrickNextCourse    2  beds ->0,1
 *   7    (33.750,  0.000,10.75)  X-long    no        -> BrickNextCourse    3  beds ->1,2 + head->6
 *   8    (61.875, 16.875,10.75)  Y-long    no        -> BrickNextCourse    2  beds ->3,4
 *   9    (56.250,  0.000,10.75)  X-long    no        -> BrickNextCourse    4  beds ->2,3
 *                                                                            + head->7
 *                                                                            + quoin->8
 *  10    (61.875, 39.375,10.75)  Y-long    no        -> BrickNextCourse    3  beds ->4,5
 *                                                                            + head->8
 *
 * WHERE THE CURSORS COME FROM, since ranking is raw distance and the numbers have to be
 * worked against the solver rather than guessed:
 *   - step 3 is asked for at Y = 5.0, which is 0.625 cm from the corner return that
 *     finishes flush with brick 2's -Y face (61.875, 5.625) and 10.625 cm from the other
 *     flush choice at that end, so the intended return wins outright. The same-end returns
 *     off brick 1 land at (39.375, +/-5.625), INSIDE brick 2, and are dropped as occupied;
 *     the -X-end returns off brick 2 are 33.75 cm away, outside the 30 cm radius.
 *   - steps 4 and 5 are asked for 0.125 cm short of the Y pitch, so the same-course pose
 *     beats the next-course pose 13.4 cm away. The corner returns off brick 2 that are in
 *     range from there land exactly on brick 3 and are dropped as occupied.
 *   - steps 6 to 10 are asked for AT their running-bond poses (offset 0), where several
 *     neighbours' poses coincide and MERGE into one candidate carrying every one of their
 *     joints. Nothing else can outrank an offset of zero. Their same-course poses that
 *     land on cells course 0 already fills are dropped as occupied.
 *
 * STEP 9 IS THE CORNER BRICK — THE REVIEW'S FINDING B2, and the reason this test grew. A
 * bricklayer laps the course-1 stretcher OVER the return so the two legs are bonded on
 * every course, not just at one vertical face. Laid at (56.25, 0, 10.75) it spans
 * X [45.5, 67] x Y [-5.125, 5.125] with its underside at Z = 7.5 — one 1 cm bed joint
 * above BOTH brick 2 (top Z = 6.5, footprint X [34.25, 55.75]) and brick 3, the rotated
 * return (top Z = 6.5, footprint X [56.75, 67] x Y [-5.125, 16.375]). Each lap is a full
 * brick width square: 10.25 x 10.25 = 105.0625 cm2. It also abuts brick 7 end to end
 * (a head joint) and meets the Y-leg's course-1 brick 8 across a horizontal face
 * 10.25 x 6.5 = 66.625 cm2 which the L-footprint rule reads as a bonded corner.
 *
 * NINETEEN CONNECTIONS: 10 beds + 2 quoins (all GeneralPurposeMortar) + 7 heads (the weak
 * GeneralPurposeMortarPerpend). A quoin is a mortar joint with a HORIZONTAL normal, which
 * is exactly what the 2026-09-15 ruling adds and what the pre-ruling inference would have
 * called a perpend.
 *
 * WORLD-FREE, AND NEEDS NO TICKING WORLD. FStructure::SolveLoads is a world-free
 * structural solve (gravity is applied inside it), not a Chaos tick.
 *
 * RED TODAY (the CR-2a review's blocking finding B2): the solver's bed/head branch is
 * gated on the two bricks running the SAME way, so step 9's candidate never beds onto the
 * crossed return. It forms three joints instead of four, the wall has eighteen connections
 * instead of nineteen, one bonded mortar joint is missing, and the lap section SEVEN finds
 * no connection between the corner brick and the return at all.
 */

/*
 * NAMED NAMESPACE, distinct from every other one in this module — an anonymous namespace
 * is private to a TRANSLATION UNIT, not a file, and a unity build merges files. See
 * DemoBuildingTest.cpp / SnapSolverTest.cpp for the rule.
 */
namespace CornerWallTestSupport
{
	const FVector HalfBrick(10.75, 5.125, 3.25);
	const FVector HalfBrickRotated(5.125, 10.75, 3.25);

	/*
	 * Full-field profile identity. FConnectionStrength has no operator==, and the profiles
	 * this build can form are distinguishable only across all five fields — mortar and its
	 * perpend sibling differ ONLY on cohesion (0.9 vs 0.2) and tension (0.7 vs 0.1).
	 */
	bool ProfileMatches(const FConnectionStrength& Got, const FConnectionStrength& Want)
	{
		return Got.CompressiveStrengthMPa == Want.CompressiveStrengthMPa
			&& Got.ShearCohesionMPa == Want.ShearCohesionMPa
			&& Got.TensileStrengthMPa == Want.TensileStrengthMPa
			&& Got.FrictionCoefficient == Want.FrictionCoefficient
			&& Got.MaxShearStrengthMPa == Want.MaxShearStrengthMPa;
	}

	/** One laid piece: what was asked for, and what the snap solver is expected to answer. */
	struct FStep
	{
		FVector RequestedCentreCm;
		FVector ExtentCm;
		bool bGrounded;
		BuildMode::ESnapKind ExpectedKind;
		int32 ExpectedJoints;
		FVector ExpectedCentreCm;
	};

	/** The INDEX of the one connection joining these two handles, in either order, or INDEX_NONE. */
	int32 FindConnectionIndex(const FStructure& Structure, int32 A, int32 B)
	{
		for (int32 c = 0; c < Structure.NumConnections(); ++c)
		{
			const FConnection& Conn = Structure.GetConnection(c);
			if ((Conn.PieceA == A && Conn.PieceB == B) || (Conn.PieceA == B && Conn.PieceB == A))
			{
				return c;
			}
		}
		return INDEX_NONE;
	}

	/** The one connection joining these two handles, in either order, or nullptr. */
	const FConnection* FindConnection(const FStructure& Structure, int32 A, int32 B)
	{
		const int32 Index = FindConnectionIndex(Structure, A, B);
		return Index == INDEX_NONE ? nullptr : &Structure.GetConnection(Index);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerWallStandsTest,
	"DestructionGame.Core.BuildMode.CornerWallStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCornerWallStandsTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace CornerWallTestSupport;

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.
	const double Tol = KINDA_SMALL_NUMBER;

	const FStep Steps[] = {
		// Course 0, the X leg.
		{ FVector(0.0, 0.0, 3.25), HalfBrick, true,
			ESnapKind::Free, 0, FVector(0.0, 0.0, 3.25) },
		{ FVector(22.5, 0.0, 3.25), HalfBrick, true,
			ESnapKind::BrickSameCourse, 1, FVector(22.5, 0.0, 3.25) },
		{ FVector(45.0, 0.0, 3.25), HalfBrick, true,
			ESnapKind::BrickSameCourse, 1, FVector(45.0, 0.0, 3.25) },

		// The corner: a rotated brick returning off the X leg's +X end.
		{ FVector(61.875, 5.0, 3.25), HalfBrickRotated, true,
			ESnapKind::BrickCornerReturn, 1, FVector(61.875, 5.625, 3.25) },

		// Course 0, the Y leg — the same running bond, stepping along Y.
		{ FVector(61.875, 28.0, 3.25), HalfBrickRotated, true,
			ESnapKind::BrickSameCourse, 1, FVector(61.875, 28.125, 3.25) },
		{ FVector(61.875, 50.5, 3.25), HalfBrickRotated, true,
			ESnapKind::BrickSameCourse, 1, FVector(61.875, 50.625, 3.25) },

		// Course 1, staggered over both legs.
		{ FVector(11.25, 0.0, 10.75), HalfBrick, false,
			ESnapKind::BrickNextCourse, 2, FVector(11.25, 0.0, 10.75) },
		{ FVector(33.75, 0.0, 10.75), HalfBrick, false,
			ESnapKind::BrickNextCourse, 3, FVector(33.75, 0.0, 10.75) },
		{ FVector(61.875, 16.875, 10.75), HalfBrickRotated, false,
			ESnapKind::BrickNextCourse, 2, FVector(61.875, 16.875, 10.75) },

		/*
		 * THE CORNER BRICK — the course-1 stretcher a bricklayer laps over the return, so
		 * the two legs are bonded on this course as well as at the quoin below. Four joints:
		 * beds onto brick 2 (the X leg's end) AND brick 3 (the crossed return), a head joint
		 * end to end with brick 7, and a horizontal corner face to the Y leg's brick 8.
		 */
		{ FVector(56.25, 0.0, 10.75), HalfBrick, false,
			ESnapKind::BrickNextCourse, 4, FVector(56.25, 0.0, 10.75) },

		// The Y leg's second course-1 brick, bedding on bricks 4 and 5 and abutting 8.
		{ FVector(61.875, 39.375, 10.75), HalfBrickRotated, false,
			ESnapKind::BrickNextCourse, 3, FVector(61.875, 39.375, 10.75) },
	};
	const int32 ExpectedPieces = UE_ARRAY_COUNT(Steps);

	FBrickLayout Layout; // empty: the wall is grown from nothing, one PlacePiece at a time.

	/*
	 * 1. PER-STEP SNAP SEQUENCE. Each placement's Kind, its joints-formed count and the
	 * pose it actually adopted. Adding the adopted CENTRE is what stops a step passing on
	 * the right label at the wrong place — the corner return in particular has four poses
	 * and only one of them turns this corner.
	 */
	for (int32 i = 0; i < ExpectedPieces; ++i)
	{
		const FStep& Step = Steps[i];
		const FPlacementResult Result = PlacePiece(
			Layout, Step.RequestedCentreCm, Step.ExtentCm, ClayBrick, Step.bGrounded, Settings);

		const FString Prefix = FString::Printf(TEXT("step %d "), i);

		TestEqual(*(Prefix + TEXT("piece handle in placement order")), Result.PieceHandle, i);
		TestEqual(*(Prefix + TEXT("snap Kind")),
			static_cast<int32>(Result.Kind), static_cast<int32>(Step.ExpectedKind));
		TestEqual(*(Prefix + TEXT("joints formed")), Result.JointsFormed, Step.ExpectedJoints);

		if (Layout.Boxes.Num() == i + 1)
		{
			TestTrue(
				*FString::Printf(
					TEXT("%sadopted centre is (%g,%g,%g), got (%g,%g,%g)"),
					*Prefix,
					Step.ExpectedCentreCm.X, Step.ExpectedCentreCm.Y, Step.ExpectedCentreCm.Z,
					Layout.Boxes[i].CentreCm.X, Layout.Boxes[i].CentreCm.Y,
					Layout.Boxes[i].CentreCm.Z),
				Layout.Boxes[i].CentreCm.Equals(Step.ExpectedCentreCm, Tol));
		}
	}

	/*
	 * 2. STRUCTURE SHAPE. Eleven pieces and nineteen connections, boxes kept strictly
	 * parallel to the piece array (PlacePiece's invariant).
	 */
	TestEqual(TEXT("the L-wall has eleven pieces"), Layout.Structure.NumPieces(), ExpectedPieces);
	TestEqual(TEXT("the L-wall has nineteen connections"), Layout.Structure.NumConnections(), 19);
	TestEqual(TEXT("boxes parallel to pieces"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	/*
	 * 3. THE QUOIN ITSELF. The joint between the X leg's end brick (2) and the return (3)
	 * is CR-2a's own behaviour: full mortar over the brick's END face — 10.25 cm of width
	 * by 6.5 cm of course = 66.625 cm2 — across a HORIZONTAL normal. The normal is
	 * asserted because it is what makes the profile assertion mean something: this is
	 * precisely the contact the 2026-09-03 inference called a weak perpend.
	 *
	 * IT IS PINNED HERE AND NOWHERE ELSE. Both bricks rest on the earth and the joint is
	 * vertical, so it carries no vertical load and section SIX cannot see it at all — the
	 * bond it provides is LATERAL. This mechanism assertion is the whole of the evidence
	 * for it, which is why the profile is compared across all five fields.
	 */
	const FConnection* Quoin = FindConnection(Layout.Structure, 2, 3);
	TestNotNull(TEXT("a connection joins the X leg's end brick to the return"), Quoin);
	if (Quoin != nullptr)
	{
		TestTrue(TEXT("the quoin joint is full GeneralPurposeMortar"),
			ProfileMatches(Quoin->Strength, GeneralPurposeMortar));
		TestEqual(TEXT("the quoin's interface area is the brick end face 66.625 cm2"),
			Quoin->InterfaceAreaSqCm, 66.625, 1.0e-6);
		TestEqual(TEXT("the quoin's normal is horizontal, on the X axis (|X| == 1)"),
			FMath::Abs(Quoin->InterfaceNormal.X), 1.0, Tol);
	}

	/*
	 * 4. THE HEAD JOINTS STAY WEAK. The X leg's two head joints are the control for the
	 * quoin: same materials, same horizontal contact, and the ONLY difference is that the
	 * two bricks run the same way. If they came back as mortar too, the corner's mortar
	 * would prove nothing about orientation.
	 */
	const FConnection* HeadZeroOne = FindConnection(Layout.Structure, 0, 1);
	const FConnection* HeadOneTwo = FindConnection(Layout.Structure, 1, 2);
	TestNotNull(TEXT("a connection joins X-leg bricks 0 and 1"), HeadZeroOne);
	TestNotNull(TEXT("a connection joins X-leg bricks 1 and 2"), HeadOneTwo);
	if (HeadZeroOne != nullptr)
	{
		TestTrue(TEXT("the head joint 0-1 is the weak GeneralPurposeMortarPerpend"),
			ProfileMatches(HeadZeroOne->Strength, GeneralPurposeMortarPerpend));
	}
	if (HeadOneTwo != nullptr)
	{
		TestTrue(TEXT("the head joint 1-2 is the weak GeneralPurposeMortarPerpend"),
			ProfileMatches(HeadOneTwo->Strength, GeneralPurposeMortarPerpend));
	}

	/*
	 * 5. THE PROFILE MIX, COUNTED. Twelve bonded joints (ten beds + two quoins) and seven
	 * perpends, with nothing unrecognised — so a wrong profile somewhere cannot cancel
	 * against a right one elsewhere in the totals.
	 *
	 * THE SECOND QUOIN IS 9-8, the course-1 corner brick's horizontal face against the Y
	 * leg's course-1 brick. It is a bonded corner by the L-footprint rule of the
	 * 2026-09-15 ruling, NOT by a separate decision: the corner brick's X span [45.5, 67]
	 * properly crosses exactly one of brick 8's two width-face planes (X = 56.75, not
	 * X = 67, which it only reaches), and brick 8's Y span crosses neither of the corner
	 * brick's. One crossing, so Corner. Recorded here because a reader counting "six beds
	 * plus one quoin" in the old wall will otherwise read the total as an error.
	 */
	int32 MortarCount = 0;
	int32 PerpendCount = 0;
	int32 UnrecognisedCount = 0;
	for (int32 c = 0; c < Layout.Structure.NumConnections(); ++c)
	{
		const FConnectionStrength& S = Layout.Structure.GetConnection(c).Strength;
		if (ProfileMatches(S, GeneralPurposeMortar))
		{
			++MortarCount;
		}
		else if (ProfileMatches(S, GeneralPurposeMortarPerpend))
		{
			++PerpendCount;
		}
		else
		{
			++UnrecognisedCount;
		}
	}

	TestEqual(TEXT("12 bonded joints are GeneralPurposeMortar (10 beds + 2 quoins)"),
		MortarCount, 12);
	TestEqual(TEXT("7 head joints are GeneralPurposeMortarPerpend"), PerpendCount, 7);
	TestEqual(TEXT("no connection has an unexpected profile"), UnrecognisedCount, 0);

	/*
	 * 6. IT STANDS, AND NOT BY BEING PINNED TO THE EARTH. The exact support kind per piece,
	 * via GetPieceSupport rather than the composite IsPieceSupported — which is true for a
	 * grounded piece AND a joint-supported one, so "all supported" would pass even if every
	 * piece were grounded. The six laid on the ground read Grounded; the five course-1
	 * bricks were laid bGrounded = false and reach the earth ONLY through the joints the
	 * builder formed, so anything wrong with those beds reads Falling (genuinely no load
	 * path) or Stranded (the solver could not route it) instead of Supported.
	 *
	 * THIS SECTION SAYS NOTHING ABOUT THE QUOIN, deliberately (see the header): 2-3 is a
	 * VERTICAL-normal joint between two pieces that both rest on the earth, so no support
	 * kind here depends on it. What it does pin is that each course-1 brick found its beds.
	 *
	 * SetThreeDimensional FIRST, because this build forms Y-NORMAL joints (the Y leg's head
	 * joints) and Placement.h's contract puts that call on the caller: whether a build is
	 * planar or 3D is a structure-level intent, not a per-placement one.
	 *
	 * BUT SolveLoads ITSELF DOES NOT READ THE FLAG, and the line above is not what makes this
	 * section pass. The router is dimension-agnostic — it routes load down bed joints and reads
	 * each normal directly — so it answers a Y-normal joint identically either way. The ONE
	 * reader is the LP bridge (RigidBlockOracle::BuildRigidBlockProblem), which refuses an
	 * out-of-plane normal unless the structure states it is 3D; so the call matters to
	 * SolveAndBreak's below-cap gate, not here. It is kept because the intent is genuine and
	 * because any later section that settles this layout would need it.
	 */
	Layout.Structure.SetThreeDimensional(true);
	Layout.Structure.SolveLoads();

	for (int32 p = 0; p < Layout.Structure.NumPieces(); ++p)
	{
		const EPieceSupport Want = (p <= 5) ? EPieceSupport::Grounded : EPieceSupport::Supported;
		TestEqual(
			*FString::Printf(
				TEXT("piece %d support kind after SolveLoads (%s expected)"),
				p, (p <= 5) ? TEXT("Grounded") : TEXT("Supported")),
			static_cast<int32>(Layout.Structure.GetPieceSupport(p)),
			static_cast<int32>(Want));
	}

	/*
	 * 7. THE CORNER LAP — the CR-2a review's finding B2, and the joint that makes the
	 * return LOAD-BEARING FROM ABOVE rather than merely bonded sideways. The course-1
	 * corner brick (9) beds onto the rotated return (3) over a full brick width square: the
	 * two footprints overlap [56.75, 67] in X and [-5.125, 5.125] in Y, so
	 * 10.25 x 10.25 = 105.0625 cm2, across a VERTICAL normal.
	 *
	 * THE NORMAL IS WHAT MAKES THE AREA MEAN SOMETHING. A joint of exactly this size could
	 * also be two side faces meeting; |Z| == 1 is what says the brick RESTS on the return
	 * rather than merely touching it, and a vertical normal is what SolveLoads routes
	 * weight down. Its profile is the bed's full mortar: the boxed inference classifies a
	 * vertical contact as a Bed before orientation is ever consulted, so a bed onto a
	 * CROSSED neighbour is a bed like any other and not a quoin.
	 *
	 * AND THE UTILISATION IS THE PROOF IT CARRIES, not merely exists. After the solve the
	 * corner brick's weight is split between its two beds by interface area, so the share
	 * crossing 9-3 is strictly positive — which is exactly the claim "the return now takes
	 * load from above" and exactly what the quoin below it cannot say. Asserted as > 0
	 * rather than at a value: the magnitude is a tiny fraction of masonry's compressive
	 * capacity and pinning it would be pinning the router's arithmetic, not this bond.
	 */
	const int32 CornerLapIndex = FindConnectionIndex(Layout.Structure, 9, 3);
	TestTrue(
		TEXT("the course-1 corner brick beds onto the return (a connection 9-3 exists)"),
		CornerLapIndex != INDEX_NONE);
	if (CornerLapIndex != INDEX_NONE)
	{
		const FConnection& Lap = Layout.Structure.GetConnection(CornerLapIndex);
		TestTrue(TEXT("the corner lap 9-3 is full GeneralPurposeMortar"),
			ProfileMatches(Lap.Strength, GeneralPurposeMortar));
		TestEqual(TEXT("the corner lap's interface area is the 10.25 x 10.25 lap, 105.0625 cm2"),
			Lap.InterfaceAreaSqCm, 105.0625, 1.0e-6);
		TestEqual(TEXT("the corner lap's normal is VERTICAL (|Z| == 1)"),
			FMath::Abs(Lap.InterfaceNormal.Z), 1.0, Tol);

		const double LapUtilisation = Layout.Structure.GetConnectionUtilisation(CornerLapIndex);
		TestTrue(
			*FString::Printf(
				TEXT("the corner lap carries load after SolveLoads (utilisation %g > 0)"),
				LapUtilisation),
			LapUtilisation > 0.0);
	}

	/*
	 * And the OTHER bed of the same brick, onto the X leg's end brick (2) — the control.
	 * Without it the "beds onto the return" assertion above could pass on a corner brick
	 * that had swapped one leg for the other rather than lapping both.
	 */
	const FConnection* LegLap = FindConnection(Layout.Structure, 9, 2);
	TestNotNull(TEXT("the course-1 corner brick also beds onto the X leg's end brick (9-2)"), LegLap);
	if (LegLap != nullptr)
	{
		TestTrue(TEXT("the leg lap 9-2 is full GeneralPurposeMortar"),
			ProfileMatches(LegLap->Strength, GeneralPurposeMortar));
		TestEqual(TEXT("the leg lap's interface area is the 10.25 x 10.25 lap, 105.0625 cm2"),
			LegLap->InterfaceAreaSqCm, 105.0625, 1.0e-6);
		TestEqual(TEXT("the leg lap's normal is VERTICAL (|Z| == 1)"),
			FMath::Abs(LegLap->InterfaceNormal.Z), 1.0, Tol);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
