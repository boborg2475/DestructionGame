// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/DemoBuilding.h"
#include "Core/BuildMode/Placement.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Integration test for the scripted demo building — BUILD_MODE_PLAN.md slice 2,
 * behavior 2c. The substance behind the /goal's "producing a live destructible
 * FStructure ... driven by unit tests and a small render."
 *
 * The behavior under test in one sentence: BuildMode::BuildDemoBuilding assembles a
 * small running-bond brick wall with a timber wall-plate ENTIRELY through
 * BuildMode::PlacePiece, and the FStructure it produces STANDS under its own weight.
 *
 * ASSERT ON THE MECHANISM AND THE OUTCOME, NEVER DISPLACEMENT. Two families of
 * assertion:
 *   - MECHANISM: the per-step snap Kind and joints-formed sequence, the piece and
 *     connection counts, and the MIX of connection profiles (strong bed mortar, weak
 *     perpend head, dry-stone timber bearing) — each binary and exact.
 *   - OUTCOME: after SolveLoads, EVERY piece is supported. A grounded bottom course
 *     with a bonded course and a bearing plate above holds itself up; a bug in the
 *     builder's geometry (a plate that does not actually bear, a course that does not
 *     bed) would leave a piece Falling. This is the mechanism reading of "it stands",
 *     not a position check.
 *
 * THE DEMO GEOMETRY THIS TEST PINS (dev builds BuildDemoBuilding to exactly this;
 * poses are the requested CENTRES handed to PlacePiece, HalfBrick = (10.75,5.125,3.25),
 * running-bond coordinating grid 22.5 x 11.25 x 7.5 per Core/Layout.h):
 *
 *   idx  requested centre     grounded  material   half-extent            -> Kind             joints
 *   0    (0.00, 0, 0.00)      yes       ClayBrick  (10.75,5.125,3.25)     -> Free             0
 *   1    (22.50,0, 0.00)      yes       ClayBrick  (10.75,5.125,3.25)     -> BrickSameCourse  1  head->0
 *   2    (45.00,0, 0.00)      yes       ClayBrick  (10.75,5.125,3.25)     -> BrickSameCourse  1  head->1
 *   3    (67.50,0, 0.00)      yes       ClayBrick  (10.75,5.125,3.25)     -> BrickSameCourse  1  head->2
 *   4    (11.25,0, 7.50)      no        ClayBrick  (10.75,5.125,3.25)     -> BrickNextCourse  2  beds->0,1
 *   5    (33.75,0, 7.50)      no        ClayBrick  (10.75,5.125,3.25)     -> BrickNextCourse  3  beds->1,2 + head->4
 *   6    (56.25,0, 7.50)      no        ClayBrick  (10.75,5.125,3.25)     -> BrickNextCourse  3  beds->2,3 + head->5
 *   7    (33.75,0,16.75)      no        Timber     (33.75,5.125,5.00)     -> TimberCentered   3  bearings->4,5,6
 *
 * Piece count 8; connection count 14 = 6 beds (GeneralPurposeMortar) + 5 heads
 * (GeneralPurposeMortarPerpend) + 3 timber bearings (DryStone). The plate at index 7
 * is a 67.5 x 10.25 x 10 cm Timber beam (half-extent 33.75 x 5.125 x 5.0) whose
 * underside sits one joint above the
 * course-1 brick tops and spans all three course-1 bricks, so its centred snap forms
 * a dry-stone bearing to each. All joints are X/Z-normal, so the build is planar and
 * needs no SetThreeDimensional.
 *
 * WORLD-FREE. FStructure::SolveLoads is a world-free structural solve (gravity is
 * applied inside it), not a Chaos tick, so this needs no ticking world.
 */

/*
 * NAMED NAMESPACE, distinct from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT, not a file, and a unity build merges
 * files. See PlacementTest.cpp / SnapSolverTest.cpp for the rule.
 */
namespace DemoBuildingTestSupport
{
	/*
	 * Full-field profile identity. FConnectionStrength has no operator==, so a joint
	 * profile is recognised by matching the five load-bearing fields against a named
	 * library constant. This is what separates the strong bed mortar from its weak
	 * perpend sibling and from the dry-stone bearing — they differ on the two bond
	 * axes, on compression, on friction and on the shear ceiling.
	 */
	bool ProfileMatches(const FConnectionStrength& Got, const FConnectionStrength& Want)
	{
		return Got.CompressiveStrengthMPa == Want.CompressiveStrengthMPa
			&& Got.ShearCohesionMPa == Want.ShearCohesionMPa
			&& Got.TensileStrengthMPa == Want.TensileStrengthMPa
			&& Got.FrictionCoefficient == Want.FrictionCoefficient
			&& Got.MaxShearStrengthMPa == Want.MaxShearStrengthMPa;
	}

	struct FExpectedStep
	{
		BuildMode::ESnapKind Kind;
		int32 JointsFormed;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildDemoBuildingStandsTest,
	"DestructionGame.Core.BuildMode.BuildDemoBuildingStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildDemoBuildingStandsTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace DemoBuildingTestSupport;

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	FBrickLayout Layout; // empty: the builder grows it from nothing via PlacePiece.

	const TArray<FPlacementResult> Steps = BuildDemoBuilding(Layout, Settings);

	/*
	 * 1. PER-STEP SNAP SEQUENCE. Each placement's Kind and joints-formed pin that the
	 * scripted build snapped the way it intended: a grounded Free seed, a same-course
	 * run to finish course 0, staggered next-course bricks (the interior one also
	 * heading its course-mate), then a timber plate centred and bearing on the course.
	 */
	const FExpectedStep Expected[] = {
		{ ESnapKind::Free,            0 },
		{ ESnapKind::BrickSameCourse, 1 },
		{ ESnapKind::BrickSameCourse, 1 },
		{ ESnapKind::BrickSameCourse, 1 },
		{ ESnapKind::BrickNextCourse, 2 },
		{ ESnapKind::BrickNextCourse, 3 },
		{ ESnapKind::BrickNextCourse, 3 },
		{ ESnapKind::TimberCentered,  3 },
	};
	const int32 ExpectedPieces = UE_ARRAY_COUNT(Expected);

	TestEqual(TEXT("builder returns one result per placed piece"),
		Steps.Num(), ExpectedPieces);

	if (Steps.Num() == ExpectedPieces)
	{
		for (int32 i = 0; i < ExpectedPieces; ++i)
		{
			const FString Prefix = FString::Printf(TEXT("step %d "), i);
			TestEqual(*(Prefix + TEXT("snap Kind")),
				static_cast<int32>(Steps[i].Kind),
				static_cast<int32>(Expected[i].Kind));
			TestEqual(*(Prefix + TEXT("joints formed")),
				Steps[i].JointsFormed, Expected[i].JointsFormed);
			TestEqual(*(Prefix + TEXT("piece handle in placement order")),
				Steps[i].PieceHandle, i);
		}
	}

	/*
	 * 2. STRUCTURE SHAPE. Eight pieces, fourteen connections, with the boxes kept
	 * strictly parallel to the piece array (PlacePiece's invariant).
	 */
	TestEqual(TEXT("structure has eight pieces"), Layout.Structure.NumPieces(), 8);
	TestEqual(TEXT("structure has fourteen connections"), Layout.Structure.NumConnections(), 14);
	TestEqual(TEXT("boxes parallel to pieces"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	/*
	 * 3. PROFILE MIX, EACH PINNED TO ITS GEOMETRY. Every connection is one of exactly
	 * three inferred profiles, and the counts are the expected mix: 6 strong bed
	 * mortars, 5 weak perpend heads, 3 dry-stone timber bearings. Counting by full-field
	 * identity proves the joint inference chose the right profile for each contact
	 * direction and material pair, not merely that fourteen joints exist.
	 *
	 * AND each connection's profile must AGREE WITH ITS INTERFACE NORMAL, so a
	 * bed-given-a-perpend and a head-given-mortar cannot cancel out in the totals: a
	 * bed mortar and a dry-stone bearing are horizontal faces (normal on Z), a perpend
	 * head is an end face (normal on X). JointForContact keys the bed-vs-perpend choice
	 * off exactly this axis, so pinning the axis beside the profile pins the inference.
	 */
	const double AxisTol = static_cast<double>(KINDA_SMALL_NUMBER);
	int32 BedMortarCount = 0;
	int32 PerpendCount = 0;
	int32 DryStoneCount = 0;
	int32 UnrecognisedCount = 0;
	for (int32 c = 0; c < Layout.Structure.NumConnections(); ++c)
	{
		const FConnection& Conn = Layout.Structure.GetConnection(c);
		const FConnectionStrength& S = Conn.Strength;
		const FVector N = Conn.InterfaceNormal;
		const FString CPrefix = FString::Printf(TEXT("connection %d "), c);
		if (ProfileMatches(S, GeneralPurposeMortar))
		{
			++BedMortarCount;
			TestEqual(*(CPrefix + TEXT("bed mortar has a Z-axis normal")),
				FMath::Abs(N.Z), 1.0, AxisTol);
		}
		else if (ProfileMatches(S, GeneralPurposeMortarPerpend))
		{
			++PerpendCount;
			TestEqual(*(CPrefix + TEXT("perpend head has an X-axis normal")),
				FMath::Abs(N.X), 1.0, AxisTol);
		}
		else if (ProfileMatches(S, DryStone))
		{
			++DryStoneCount;
			TestEqual(*(CPrefix + TEXT("dry-stone bearing has a Z-axis normal")),
				FMath::Abs(N.Z), 1.0, AxisTol);
		}
		else
		{
			++UnrecognisedCount;
		}
	}

	TestEqual(TEXT("6 bed joints are GeneralPurposeMortar"), BedMortarCount, 6);
	TestEqual(TEXT("5 head joints are GeneralPurposeMortarPerpend"), PerpendCount, 5);
	TestEqual(TEXT("3 timber bearings are DryStone"), DryStoneCount, 3);
	TestEqual(TEXT("no connection has an unexpected profile"), UnrecognisedCount, 0);

	/*
	 * 4. IT STANDS, AND THE LOAD PATH IS REAL — NOT EVERYTHING GROUNDED. Solve the built
	 * structure and require the EXACT support KIND per piece, via GetPieceSupport rather
	 * than the composite IsPieceSupported. IsPieceSupported is true for a grounded piece
	 * AND for a joint-supported one, so "all supported" would pass even if every piece
	 * were pinned to the earth (a builder that grounded all eight rows would slip
	 * through). GetPieceSupport separates them:
	 *
	 *   - pieces 0..3 (the bottom course) are GROUNDED: laid bGrounded=true, resting on
	 *     the earth, so the earth terminates their load.
	 *   - pieces 4..7 (the two staggered course-1 bricks + the timber plate) are
	 *     SUPPORTED: laid bGrounded=false, they reach the earth ONLY through the joints
	 *     the builder formed. A bed that did not bed or a plate that did not bear would
	 *     read Falling here (or the plate would have to be spuriously grounded to pass),
	 *     which is the mechanism reading of collapse, not a displacement check.
	 */
	Layout.Structure.SolveLoads();

	for (int32 p = 0; p < Layout.Structure.NumPieces(); ++p)
	{
		const EPieceSupport Want = (p <= 3) ? EPieceSupport::Grounded : EPieceSupport::Supported;
		const FString Prefix = FString::Printf(
			TEXT("piece %d support kind after SolveLoads (%s expected)"),
			p, (p <= 3) ? TEXT("Grounded") : TEXT("Supported"));
		TestEqual(*Prefix,
			static_cast<int32>(Layout.Structure.GetPieceSupport(p)),
			static_cast<int32>(Want));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
