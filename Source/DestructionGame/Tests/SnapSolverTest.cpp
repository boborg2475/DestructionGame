// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * Unit tests for the snap-candidate solver (BUILD_MODE_PLAN.md behavior 2). World-free;
 * assertions are on candidate poses, joints and order, never load or displacement.
 * Grid (Core/Layout.h): a 21.5 x 10.25 x 6.5 brick on 1 cm joints gives a 22.5 pitch,
 * an 11.25 half-stagger and a 7.5 course.
 */

// Named, not anonymous: unity builds merge translation units (see JointInferenceTest.cpp).
namespace SnapSolverTestSupport
{
	const FVector HalfBrick(10.75, 5.125, 3.25);

	// The same brick turned 90 degrees (runs along Y). The brick-sized gate must accept both.
	const FVector HalfBrickRotated(5.125, 10.75, 3.25);

	/** Swaps X and Y: reflects through x = y. */
	FVector SwapXY(const FVector& V)
	{
		return FVector(V.Y, V.X, V.Z);
	}

	/*
	 * Full-field profile match (FConnectionStrength has no operator==). One field alone
	 * would let a sibling profile through: mortar, perpend and DryStone differ on several.
	 */
	void CheckProfileIdentity(
		FAutomationTestBase& Test,
		const FString& Prefix,
		const FConnectionStrength& Got,
		const FConnectionStrength& Want)
	{
		Test.TestEqual(Prefix + TEXT("CompressiveStrengthMPa"),
			Got.CompressiveStrengthMPa, Want.CompressiveStrengthMPa);
		Test.TestEqual(Prefix + TEXT("ShearCohesionMPa"),
			Got.ShearCohesionMPa, Want.ShearCohesionMPa);
		Test.TestEqual(Prefix + TEXT("TensileStrengthMPa"),
			Got.TensileStrengthMPa, Want.TensileStrengthMPa);
		Test.TestEqual(Prefix + TEXT("FrictionCoefficient"),
			Got.FrictionCoefficient, Want.FrictionCoefficient);
		Test.TestEqual(Prefix + TEXT("MaxShearStrengthMPa"),
			Got.MaxShearStrengthMPa, Want.MaxShearStrengthMPa);
	}
}

/**
 * 2a, single neighbour: a brick above and +X of another is offered a BrickNextCourse
 * snap at (11.25, 0, 7.5) with one GeneralPurposeMortar bed joint, ranked ahead of Free.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverBrickNextCourseTest,
	"DestructionGame.Core.BuildMode.SnapSolverBrickNextCourse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverBrickNextCourseTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	const FVector Requested(11.0, 0.0, 7.5);
	const FPieceBox Placed{ Requested, HalfBrick };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	// Exact coordinate, so a tight tolerance.
	const FVector ExpectedNextCourseCentre(11.25, 0.0, 7.5);
	const double Tol = KINDA_SMALL_NUMBER;

	int32 NextCourseIndex = INDEX_NONE;
	int32 FreeIndex = INDEX_NONE;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (NextCourseIndex == INDEX_NONE
			&& C.Kind == ESnapKind::BrickNextCourse
			&& C.CentreCm.Equals(ExpectedNextCourseCentre, Tol))
		{
			NextCourseIndex = i;
		}
		if (FreeIndex == INDEX_NONE && C.Kind == ESnapKind::Free)
		{
			FreeIndex = i;
		}
	}

	const bool bFoundNextCourse = NextCourseIndex != INDEX_NONE;
	TestTrue(
		TEXT("a BrickNextCourse candidate exists at origin + (11.25, 0, 7.5)"),
		bFoundNextCourse);

	const bool bFoundFree = FreeIndex != INDEX_NONE;
	TestTrue(TEXT("a Free fallback candidate exists"), bFoundFree);

	if (bFoundNextCourse)
	{
		const FSnapCandidate& Snap = Candidates[NextCourseIndex];

		// One bed joint to brick 0, derived via JointForContact with a vertical normal.
		TestEqual(TEXT("next-course candidate forms exactly one joint"),
			Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(TEXT("joint is to the existing brick (OtherPieceIndex 0)"),
				Snap.Joints[0].OtherPieceIndex, 0);
			CheckProfileIdentity(
				*this,
				TEXT("bed joint profile == GeneralPurposeMortar: "),
				Snap.Joints[0].Profile,
				GeneralPurposeMortar);
		}

		// Distance from (11,0,7.5) to (11.25,0,7.5).
		TestEqual(TEXT("next-course OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, 0.25, 1.0e-6);
	}

	if (bFoundFree)
	{
		const FSnapCandidate& Free = Candidates[FreeIndex];
		TestTrue(TEXT("Free candidate sits at the requested pose"),
			Free.CentreCm.Equals(Requested, Tol));
		TestEqual(TEXT("Free candidate forms no joints"), Free.Joints.Num(), 0);
	}

	// Free has offset 0, so a plain offset sort would rank it first. Snaps must come before it.
	if (bFoundNextCourse && bFoundFree)
	{
		TestTrue(
			TEXT("BrickNextCourse is ranked ahead of Free (appears earlier)"),
			NextCourseIndex < FreeIndex);
	}

	return true;
}

/**
 * 2a, straddle: a brick at (11.25, 0, 7.5) over bricks at X = 0 and 22.5 is ONE candidate
 * with two bed joints, not one candidate per neighbour.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverTwoNeighbourStraddleTest,
	"DestructionGame.Core.BuildMode.SnapSolverTwoNeighbourStraddle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverTwoNeighbourStraddleTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox BrickLeft{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const FPieceBox BrickRight{ FVector(22.5, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { BrickLeft, BrickRight };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick };

	const FVector Requested(11.25, 0.0, 7.5);
	const FPieceBox Placed{ Requested, HalfBrick };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedCentre(11.25, 0.0, 7.5);
	const double Tol = KINDA_SMALL_NUMBER;

	TArray<int32> StraddleCandidateIdx;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (C.Kind == ESnapKind::BrickNextCourse && C.CentreCm.Equals(ExpectedCentre, Tol))
		{
			StraddleCandidateIdx.Add(i);
		}
	}

	TestEqual(
		TEXT("exactly one BrickNextCourse candidate at the straddle pose (11.25,0,7.5)"),
		StraddleCandidateIdx.Num(), 1);

	if (StraddleCandidateIdx.Num() == 1)
	{
		const FSnapCandidate& Snap = Candidates[StraddleCandidateIdx[0]];

		TestEqual(TEXT("straddle candidate forms two joints"), Snap.Joints.Num(), 2);

		if (Snap.Joints.Num() == 2)
		{
			// Order-independent {0, 1}: both neighbours, neither twice.
			const int32 IdxA = Snap.Joints[0].OtherPieceIndex;
			const int32 IdxB = Snap.Joints[1].OtherPieceIndex;
			const bool bBothNeighbours =
				(IdxA == 0 && IdxB == 1) || (IdxA == 1 && IdxB == 0);
			TestTrue(
				TEXT("straddle candidate beds onto both bricks (OtherPieceIndex set == {0,1})"),
				bBothNeighbours);

			CheckProfileIdentity(
				*this,
				FString::Printf(TEXT("straddle joint 0 (to piece %d) == GeneralPurposeMortar: "), IdxA),
				Snap.Joints[0].Profile,
				GeneralPurposeMortar);
			CheckProfileIdentity(
				*this,
				FString::Printf(TEXT("straddle joint 1 (to piece %d) == GeneralPurposeMortar: "), IdxB),
				Snap.Joints[1].Profile,
				GeneralPurposeMortar);
		}
	}

	return true;
}

/**
 * 2a, ranking: snaps are ordered by offset from the requested pose, not input order, and
 * Free is last. Requested at the right brick's +X pose (offset 0); the left brick's is 22.5 away.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverOffsetRankingTest,
	"DestructionGame.Core.BuildMode.SnapSolverOffsetRanking",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverOffsetRankingTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox BrickLeft{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const FPieceBox BrickRight{ FVector(22.5, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { BrickLeft, BrickRight };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick };

	const FVector Requested(33.75, 0.0, 7.5);
	const FPieceBox Placed{ Requested, HalfBrick };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const FVector NearPose(33.75, 0.0, 7.5); // +X stagger of the RIGHT brick, offset 0.
	const FVector FarPose(11.25, 0.0, 7.5);  // +X stagger of the LEFT brick, offset 22.5.
	const double Tol = KINDA_SMALL_NUMBER;

	int32 NearIndex = INDEX_NONE;
	int32 FarIndex = INDEX_NONE;
	int32 FreeIndex = INDEX_NONE;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (C.Kind == ESnapKind::BrickNextCourse && C.CentreCm.Equals(NearPose, Tol))
		{
			NearIndex = i;
		}
		if (C.Kind == ESnapKind::BrickNextCourse && C.CentreCm.Equals(FarPose, Tol))
		{
			FarIndex = i;
		}
		if (FreeIndex == INDEX_NONE && C.Kind == ESnapKind::Free)
		{
			FreeIndex = i;
		}
	}

	const bool bFoundNear = NearIndex != INDEX_NONE;
	const bool bFoundFar = FarIndex != INDEX_NONE;
	const bool bFoundFree = FreeIndex != INDEX_NONE;

	TestTrue(TEXT("the near (right-brick) next-course candidate exists at (33.75,0,7.5)"), bFoundNear);
	TestTrue(TEXT("the far (left-brick) next-course candidate exists at (11.25,0,7.5)"), bFoundFar);
	TestTrue(TEXT("a Free fallback candidate exists"), bFoundFree);

	if (bFoundNear)
	{
		TestEqual(TEXT("near candidate OffsetFromRequestedCm == 0"),
			Candidates[NearIndex].OffsetFromRequestedCm, 0.0, 1.0e-6);
	}
	if (bFoundFar)
	{
		TestEqual(TEXT("far candidate OffsetFromRequestedCm == 22.5"),
			Candidates[FarIndex].OffsetFromRequestedCm, 22.5, 1.0e-6);
	}

	if (bFoundNear && bFoundFar)
	{
		TestTrue(
			TEXT("the nearer snap (offset 0) is ranked ahead of the farther snap (offset 22.5)"),
			NearIndex < FarIndex);
	}

	if (bFoundFree)
	{
		TestEqual(TEXT("Free is the last candidate"),
			FreeIndex, Candidates.Num() - 1);
	}

	return true;
}

/**
 * 2a, timber on brick (regression pin): a brick-sized Timber block still snaps to the next
 * course, but its joint is a passive DryStone bearing. Proves the materials reach
 * JointForContact; a hardcoded mortar fails here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverTimberOnBrickTest,
	"DestructionGame.Core.BuildMode.SnapSolverTimberOnBrick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverTimberOnBrickTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	const FVector Requested(11.0, 0.0, 7.5);
	const FPieceBox Placed{ Requested, HalfBrick };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedCentre(11.25, 0.0, 7.5);
	const double Tol = KINDA_SMALL_NUMBER;

	int32 NextCourseIndex = INDEX_NONE;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (C.Kind == ESnapKind::BrickNextCourse && C.CentreCm.Equals(ExpectedCentre, Tol))
		{
			NextCourseIndex = i;
			break;
		}
	}

	const bool bFound = NextCourseIndex != INDEX_NONE;
	TestTrue(
		TEXT("a BrickNextCourse candidate exists for the brick-sized timber block"),
		bFound);

	if (bFound)
	{
		const FSnapCandidate& Snap = Candidates[NextCourseIndex];
		TestEqual(TEXT("timber candidate forms exactly one joint"), Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(TEXT("joint is to the existing brick (OtherPieceIndex 0)"),
				Snap.Joints[0].OtherPieceIndex, 0);
			CheckProfileIdentity(
				*this,
				TEXT("timber-on-brick joint profile == DryStone: "),
				Snap.Joints[0].Profile,
				DryStone);
		}
	}

	return true;
}

/**
 * 2b, same course: a brick beside another on the same course is offered a BrickSameCourse
 * snap at (22.5, 0, 0) (21.5 + 1 cm joint) with one head joint, ranked ahead of Free.
 * The head joint's normal is horizontal, so its profile is the weak perpend, not bed mortar.
 * Also guards that no BrickNextCourse candidate lands at that pose.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverBrickSameCourseTest,
	"DestructionGame.Core.BuildMode.SnapSolverBrickSameCourse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverBrickSameCourseTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	const FVector Requested(21.0, 0.0, 0.0);
	const FPieceBox Placed{ Requested, HalfBrick };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedSameCourseCentre(22.5, 0.0, 0.0);
	const double Tol = KINDA_SMALL_NUMBER;

	int32 SameCourseIndex = INDEX_NONE;
	int32 FreeIndex = INDEX_NONE;
	bool bNextCourseAtSameCoursePose = false;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (SameCourseIndex == INDEX_NONE
			&& C.Kind == ESnapKind::BrickSameCourse
			&& C.CentreCm.Equals(ExpectedSameCourseCentre, Tol))
		{
			SameCourseIndex = i;
		}
		if (FreeIndex == INDEX_NONE && C.Kind == ESnapKind::Free)
		{
			FreeIndex = i;
		}
		if (C.Kind == ESnapKind::BrickNextCourse
			&& C.CentreCm.Equals(ExpectedSameCourseCentre, Tol))
		{
			bNextCourseAtSameCoursePose = true;
		}
	}

	const bool bFoundSameCourse = SameCourseIndex != INDEX_NONE;
	TestTrue(
		TEXT("a BrickSameCourse candidate exists at origin + (22.5, 0, 0)"),
		bFoundSameCourse);

	const bool bFoundFree = FreeIndex != INDEX_NONE;
	TestTrue(TEXT("a Free fallback candidate exists"), bFoundFree);

	if (bFoundSameCourse)
	{
		const FSnapCandidate& Snap = Candidates[SameCourseIndex];

		TestEqual(TEXT("same-course candidate forms exactly one joint"),
			Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(TEXT("joint is to the existing brick (OtherPieceIndex 0)"),
				Snap.Joints[0].OtherPieceIndex, 0);
			CheckProfileIdentity(
				*this,
				TEXT("head joint profile == GeneralPurposeMortarPerpend: "),
				Snap.Joints[0].Profile,
				GeneralPurposeMortarPerpend);
		}

		TestEqual(TEXT("same-course OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, 1.5, 1.0e-6);
	}

	if (bFoundFree)
	{
		const FSnapCandidate& Free = Candidates[FreeIndex];
		TestTrue(TEXT("Free candidate sits at the requested pose"),
			Free.CentreCm.Equals(Requested, Tol));
		TestEqual(TEXT("Free candidate forms no joints"), Free.Joints.Num(), 0);
	}

	if (bFoundFree)
	{
		TestEqual(TEXT("Free is the last candidate"),
			FreeIndex, Candidates.Num() - 1);
	}

	if (bFoundSameCourse && bFoundFree)
	{
		TestTrue(
			TEXT("BrickSameCourse is ranked ahead of Free (appears earlier)"),
			SameCourseIndex < FreeIndex);
	}

	TestFalse(
		TEXT("no BrickNextCourse candidate lands at the same-course pose (22.5,0,0)"),
		bNextCourseAtSameCoursePose);

	return true;
}

/**
 * 2b, bed + head merge (regression pin): a brick at (33.75, 0, 7.5) beds onto brick@22.5
 * below (mortar) and abuts brick@11.25 on its course (perpend). The pose-keyed merge must
 * give ONE candidate with both joints. Kind is not asserted: the merged label is
 * order-dependent (logged separately).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverBedAndHeadMergeTest,
	"DestructionGame.Core.BuildMode.SnapSolverBedAndHeadMerge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverBedAndHeadMergeTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Course0Left{ FVector(0.0, 0.0, 0.0), HalfBrick };   // idx 0
	const FPieceBox Course0Right{ FVector(22.5, 0.0, 0.0), HalfBrick }; // idx 1
	const FPieceBox Course1Mid{ FVector(11.25, 0.0, 7.5), HalfBrick };  // idx 2
	const TArray<FPieceBox> NearbyBoxes = { Course0Left, Course0Right, Course1Mid };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick, ClayBrick };

	const FVector Requested(33.75, 0.0, 7.5);
	const FPieceBox Placed{ Requested, HalfBrick };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedCentre(33.75, 0.0, 7.5);
	const double Tol = KINDA_SMALL_NUMBER;

	const int32 BedNeighbour = 1;  // course-0 brick@22.5, one course below -> bed.
	const int32 HeadNeighbour = 2; // course-1 brick@11.25, same course -> head.

	TArray<int32> AtPose;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (C.Kind != ESnapKind::Free && C.CentreCm.Equals(ExpectedCentre, Tol))
		{
			AtPose.Add(i);
		}
	}

	TestEqual(
		TEXT("exactly one candidate at the merged running-bond pose (33.75,0,7.5)"),
		AtPose.Num(), 1);

	if (AtPose.Num() == 1)
	{
		const FSnapCandidate& Snap = Candidates[AtPose[0]];

		TestEqual(TEXT("merged candidate forms two joints (bed + head)"),
			Snap.Joints.Num(), 2);

		if (Snap.Joints.Num() == 2)
		{
			const FFormedJoint* BedJoint = Snap.Joints.FindByPredicate(
				[BedNeighbour](const FFormedJoint& J)
				{ return J.OtherPieceIndex == BedNeighbour; });
			const FFormedJoint* HeadJoint = Snap.Joints.FindByPredicate(
				[HeadNeighbour](const FFormedJoint& J)
				{ return J.OtherPieceIndex == HeadNeighbour; });

			TestNotNull(TEXT("a joint to the course-0 brick@22.5 (bed neighbour) exists"),
				BedJoint);
			TestNotNull(TEXT("a joint to the course-1 brick@11.25 (head neighbour) exists"),
				HeadJoint);

			if (BedJoint != nullptr)
			{
				CheckProfileIdentity(
					*this,
					TEXT("bed joint (to brick@22.5) == GeneralPurposeMortar: "),
					BedJoint->Profile,
					GeneralPurposeMortar);
			}
			if (HeadJoint != nullptr)
			{
				CheckProfileIdentity(
					*this,
					TEXT("head joint (to brick@11.25) == GeneralPurposeMortarPerpend: "),
					HeadJoint->Profile,
					GeneralPurposeMortarPerpend);
			}
		}
	}

	return true;
}

/**
 * 2d, timber centred on a brick: a 60 x 10.25 x 3.0 lintel requested off-centre and low
 * snaps to (0, 0, 5.75) (brick top 3.25 + joint 1.0 + half-height 1.5) with one passive
 * DryStone bearing. The lintel is not brick-sized, so no brick kinds may fire; Free is last.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverTimberCenteredOnBrickTest,
	"DestructionGame.Core.BuildMode.SnapSolverTimberCenteredOnBrick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverTimberCenteredOnBrickTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	// Off-centre in X and low in Z so both the centring and the one-joint rise show.
	const FVector PlacedHalfExtent(30.0, 5.125, 1.5);
	const FVector Requested(2.0, 0.0, 5.0);
	const FPieceBox Placed{ Requested, PlacedHalfExtent };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedCentre(0.0, 0.0, 5.75);
	const double Tol = KINDA_SMALL_NUMBER;

	int32 CenteredIndex = INDEX_NONE;
	int32 FreeIndex = INDEX_NONE;
	bool bAnyBrickKind = false;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (CenteredIndex == INDEX_NONE
			&& C.Kind == ESnapKind::TimberCentered
			&& C.CentreCm.Equals(ExpectedCentre, Tol))
		{
			CenteredIndex = i;
		}
		if (FreeIndex == INDEX_NONE && C.Kind == ESnapKind::Free)
		{
			FreeIndex = i;
		}
		if (C.Kind == ESnapKind::BrickNextCourse || C.Kind == ESnapKind::BrickSameCourse)
		{
			bAnyBrickKind = true;
		}
	}

	const bool bFoundCentered = CenteredIndex != INDEX_NONE;
	TestTrue(
		TEXT("a TimberCentered candidate exists at the brick-centred pose (0,0,5.75)"),
		bFoundCentered);

	const bool bFoundFree = FreeIndex != INDEX_NONE;
	TestTrue(TEXT("a Free fallback candidate exists"), bFoundFree);

	if (bFoundCentered)
	{
		const FSnapCandidate& Snap = Candidates[CenteredIndex];

		TestEqual(TEXT("timber centered candidate forms exactly one joint"),
			Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(TEXT("joint is to the existing brick (OtherPieceIndex 0)"),
				Snap.Joints[0].OtherPieceIndex, 0);
			CheckProfileIdentity(
				*this,
				TEXT("timber-on-brick bearing joint profile == DryStone: "),
				Snap.Joints[0].Profile,
				DryStone);
		}

		// (2,0,5) to (0,0,5.75): sqrt(4 + 0.5625).
		TestEqual(TEXT("timber centered OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, FMath::Sqrt(4.5625), 1.0e-6);
	}

	if (bFoundFree)
	{
		const FSnapCandidate& Free = Candidates[FreeIndex];
		TestTrue(TEXT("Free candidate sits at the requested pose"),
			Free.CentreCm.Equals(Requested, Tol));
		TestEqual(TEXT("Free candidate forms no joints"), Free.Joints.Num(), 0);

		TestEqual(TEXT("Free is the last candidate"),
			FreeIndex, Candidates.Num() - 1);
	}

	if (bFoundCentered && bFoundFree)
	{
		TestTrue(
			TEXT("TimberCentered is ranked ahead of Free (appears earlier)"),
			CenteredIndex < FreeIndex);
	}

	TestFalse(
		TEXT("no BrickNextCourse/BrickSameCourse candidate for a non-brick-sized timber lintel"),
		bAnyBrickKind);

	return true;
}


/**
 * 2d, a lintel bears on every brick it spans: a 60 cm lintel centred on brick@22.5 spans
 * [-7.5, 52.5] over bricks at X = 0, 22.5, 45, so the candidate at (22.5, 0, 5.75) carries
 * three DryStone bearings. Candidates centred on the other bricks are allowed, not asserted.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverTimberLintelBearsAllTest,
	"DestructionGame.Core.BuildMode.SnapSolverTimberLintelBearsAll",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverTimberLintelBearsAllTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Brick0{ FVector(0.0, 0.0, 0.0), HalfBrick };   // idx 0
	const FPieceBox Brick1{ FVector(22.5, 0.0, 0.0), HalfBrick };  // idx 1
	const FPieceBox Brick2{ FVector(45.0, 0.0, 0.0), HalfBrick };  // idx 2
	const TArray<FPieceBox> NearbyBoxes = { Brick0, Brick1, Brick2 };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick, ClayBrick };

	const FVector PlacedHalfExtent(30.0, 5.125, 1.5);
	const FVector Requested(24.0, 0.0, 5.0);
	const FPieceBox Placed{ Requested, PlacedHalfExtent };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedCentre(22.5, 0.0, 5.75);
	const double Tol = KINDA_SMALL_NUMBER;

	TArray<int32> AtPose;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (C.Kind == ESnapKind::TimberCentered && C.CentreCm.Equals(ExpectedCentre, Tol))
		{
			AtPose.Add(i);
		}
	}

	TestEqual(
		TEXT("exactly one TimberCentered candidate at the brick@22.5 pose (22.5,0,5.75)"),
		AtPose.Num(), 1);

	if (AtPose.Num() == 1)
	{
		const FSnapCandidate& Snap = Candidates[AtPose[0]];

		TestEqual(TEXT("lintel candidate forms three bearing joints (one per spanned brick)"),
			Snap.Joints.Num(), 3);

		if (Snap.Joints.Num() == 3)
		{
			// Order-independent {0, 1, 2}: each brick once.
			for (int32 Expected = 0; Expected <= 2; ++Expected)
			{
				const FFormedJoint* J = Snap.Joints.FindByPredicate(
					[Expected](const FFormedJoint& Joint)
					{ return Joint.OtherPieceIndex == Expected; });
				TestNotNull(
					*FString::Printf(TEXT("a bearing joint to brick idx %d exists"), Expected),
					J);
				if (J != nullptr)
				{
					CheckProfileIdentity(
						*this,
						FString::Printf(TEXT("lintel bearing to brick idx %d == DryStone: "), Expected),
						J->Profile,
						DryStone);
				}
			}
		}
	}

	return true;
}

/**
 * 2e, timber edge-flush: a 10 cm plank (narrower than the brick) requested near the
 * brick's +X edge snaps with its +X face flush to the brick's, at (10.75 - 5, 0, 5.75) =
 * (5.75, 0, 5.75), with one DryStone bearing. It is nearer the cursor than the centred
 * pose (2.37 vs 8.04 cm), so it must rank ahead of any TimberCentered candidate. Free is last.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverTimberEdgeFlushTest,
	"DestructionGame.Core.BuildMode.SnapSolverTimberEdgeFlush",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverTimberEdgeFlushTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	// Narrower than the brick, so edge-flush is a distinct pose from centred.
	const FVector PlacedHalfExtent(5.0, 5.125, 1.5);
	const FVector Requested(8.0, 0.0, 5.0);
	const FPieceBox Placed{ Requested, PlacedHalfExtent };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedCentre(5.75, 0.0, 5.75);
	const double Tol = KINDA_SMALL_NUMBER;

	int32 EdgeFlushIndex = INDEX_NONE;
	int32 CenteredIndex = INDEX_NONE;
	int32 FreeIndex = INDEX_NONE;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (EdgeFlushIndex == INDEX_NONE
			&& C.Kind == ESnapKind::TimberEdgeFlush
			&& C.CentreCm.Equals(ExpectedCentre, Tol))
		{
			EdgeFlushIndex = i;
		}
		if (CenteredIndex == INDEX_NONE && C.Kind == ESnapKind::TimberCentered)
		{
			CenteredIndex = i;
		}
		if (FreeIndex == INDEX_NONE && C.Kind == ESnapKind::Free)
		{
			FreeIndex = i;
		}
	}

	const bool bFoundEdgeFlush = EdgeFlushIndex != INDEX_NONE;
	TestTrue(
		TEXT("a TimberEdgeFlush candidate exists at the +X-flush pose (5.75,0,5.75)"),
		bFoundEdgeFlush);

	const bool bFoundFree = FreeIndex != INDEX_NONE;
	TestTrue(TEXT("a Free fallback candidate exists"), bFoundFree);

	if (bFoundEdgeFlush)
	{
		const FSnapCandidate& Snap = Candidates[EdgeFlushIndex];

		const double PlankPlusXFace = Snap.CentreCm.X + PlacedHalfExtent.X;
		const double BrickPlusXFace = Existing.CentreCm.X + HalfBrick.X;
		TestEqual(
			TEXT("plank +X face is flush with the brick +X face (both 10.75)"),
			PlankPlusXFace, BrickPlusXFace, 1.0e-6);

		// The plank spans [0.75, 10.75], so it rests only on brick 0.
		TestEqual(TEXT("edge-flush candidate forms exactly one joint"),
			Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(TEXT("joint is to the existing brick (OtherPieceIndex 0)"),
				Snap.Joints[0].OtherPieceIndex, 0);
			CheckProfileIdentity(
				*this,
				TEXT("edge-flush bearing joint profile == DryStone: "),
				Snap.Joints[0].Profile,
				DryStone);
		}

		// (8,0,5) to (5.75,0,5.75): sqrt(2.25^2 + 0.75^2).
		TestEqual(TEXT("edge-flush OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, FMath::Sqrt(5.625), 1.0e-6);
	}

	if (bFoundFree)
	{
		const FSnapCandidate& Free = Candidates[FreeIndex];
		TestTrue(TEXT("Free candidate sits at the requested pose"),
			Free.CentreCm.Equals(Requested, Tol));
		TestEqual(TEXT("Free candidate forms no joints"), Free.Joints.Num(), 0);

		TestEqual(TEXT("Free is the last candidate"),
			FreeIndex, Candidates.Num() - 1);
	}

	if (bFoundEdgeFlush && bFoundFree)
	{
		TestTrue(
			TEXT("TimberEdgeFlush is ranked ahead of Free (appears earlier)"),
			EdgeFlushIndex < FreeIndex);
	}

	// A TimberCentered candidate is allowed, but must rank behind the nearer edge-flush one.
	if (bFoundEdgeFlush && CenteredIndex != INDEX_NONE)
	{
		TestTrue(
			TEXT("TimberEdgeFlush is ranked ahead of TimberCentered when the cursor is near the edge"),
			EdgeFlushIndex < CenteredIndex);
	}

	return true;
}

/**
 * 2e, no duplicate bearings: a plank with the brick's X/Y footprint has coincident
 * centred and edge-flush poses, and the pose-keyed merge used to append the same bearing
 * twice, doubling that brick's bearing capacity. The candidate at (0, 0, 5.75) must carry
 * exactly one DryStone bearing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverTimberBrickWidthNoDuplicateJointsTest,
	"DestructionGame.Core.BuildMode.SnapSolverTimberBrickWidthNoDuplicateJoints",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverTimberBrickWidthNoDuplicateJointsTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	// Brick footprint, but 3 cm thick, so not brick-sized.
	const FVector PlacedHalfExtent(10.75, 5.125, 1.5);
	const FVector Requested(1.0, 0.0, 5.0);
	const FPieceBox Placed{ Requested, PlacedHalfExtent };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedCentre(0.0, 0.0, 5.75);
	const double Tol = KINDA_SMALL_NUMBER;

	TArray<int32> AtPose;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if ((C.Kind == ESnapKind::TimberCentered || C.Kind == ESnapKind::TimberEdgeFlush)
			&& C.CentreCm.Equals(ExpectedCentre, Tol))
		{
			AtPose.Add(i);
		}
	}

	TestEqual(
		TEXT("exactly one timber-kind candidate at the coincident centred/edge-flush pose (0,0,5.75)"),
		AtPose.Num(), 1);

	if (AtPose.Num() == 1)
	{
		const FSnapCandidate& Snap = Candidates[AtPose[0]];

		TestEqual(TEXT("candidate forms exactly one bearing joint (no duplicate)"),
			Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(TEXT("the single bearing is to the existing brick (OtherPieceIndex 0)"),
				Snap.Joints[0].OtherPieceIndex, 0);
			CheckProfileIdentity(
				*this,
				TEXT("bearing joint profile == DryStone: "),
				Snap.Joints[0].Profile,
				DryStone);
		}
	}

	return true;
}

/**
 * 2e, wall plate (regression pin): a 67.5 cm plate centred on brick@22.5 keeps three
 * distinct DryStone bearings, one per brick. Guards against a dedupe that drops real bearings.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverTimberWallPlateBearsDistinctTest,
	"DestructionGame.Core.BuildMode.SnapSolverTimberWallPlateBearsDistinct",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverTimberWallPlateBearsDistinctTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Brick0{ FVector(0.0, 0.0, 0.0), HalfBrick };   // idx 0
	const FPieceBox Brick1{ FVector(22.5, 0.0, 0.0), HalfBrick };  // idx 1
	const FPieceBox Brick2{ FVector(45.0, 0.0, 0.0), HalfBrick };  // idx 2
	const TArray<FPieceBox> NearbyBoxes = { Brick0, Brick1, Brick2 };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick, ClayBrick };

	const FVector PlacedHalfExtent(33.75, 5.125, 1.5);
	const FVector Requested(24.0, 0.0, 5.0);
	const FPieceBox Placed{ Requested, PlacedHalfExtent };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedCentre(22.5, 0.0, 5.75);
	const double Tol = KINDA_SMALL_NUMBER;

	TArray<int32> AtPose;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if ((C.Kind == ESnapKind::TimberCentered || C.Kind == ESnapKind::TimberEdgeFlush)
			&& C.CentreCm.Equals(ExpectedCentre, Tol))
		{
			AtPose.Add(i);
		}
	}

	TestEqual(
		TEXT("exactly one timber-kind candidate at the plate's centred pose (22.5,0,5.75)"),
		AtPose.Num(), 1);

	if (AtPose.Num() == 1)
	{
		const FSnapCandidate& Snap = Candidates[AtPose[0]];

		TestEqual(TEXT("plate candidate forms three bearing joints (one per spanned brick)"),
			Snap.Joints.Num(), 3);

		if (Snap.Joints.Num() == 3)
		{
			// Three joints over three distinct indices means no duplicate.
			for (int32 Expected = 0; Expected <= 2; ++Expected)
			{
				const FFormedJoint* J = Snap.Joints.FindByPredicate(
					[Expected](const FFormedJoint& Joint)
					{ return Joint.OtherPieceIndex == Expected; });
				TestNotNull(
					*FString::Printf(TEXT("a distinct bearing joint to brick idx %d exists"), Expected),
					J);
				if (J != nullptr)
				{
					CheckProfileIdentity(
						*this,
						FString::Printf(TEXT("plate bearing to brick idx %d == DryStone: "), Expected),
						J->Profile,
						DryStone);
				}
			}
		}
	}

	return true;
}

/**
 * 2b, occupancy filter: a snap whose box overlaps a placed piece on all three axes
 * (|dCentre| < sum of half-extents on each) is dropped, however near. Touching on an axis
 * (a joint-separated snap) is not overlap; see FSnapSolverContactPoseSurvives.
 *
 * Brick 1 sits at (11.25, 0, 7.5) on brick 0. Requesting (11.25, 0, 15) makes brick 0's
 * next-course pose, exactly brick 1's cell, the nearest snap; it must be dropped while
 * (22.5, 0, 15) on brick 1 survives.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverOccupiedCellDroppedTest,
	"DestructionGame.Core.BuildMode.SnapSolverOccupiedCellDropped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverOccupiedCellDroppedTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Brick0{ FVector(0.0, 0.0, 0.0), HalfBrick };      // idx 0
	const FPieceBox Brick1{ FVector(11.25, 0.0, 7.5), HalfBrick };    // idx 1 (occupied cell)
	const TArray<FPieceBox> NearbyBoxes = { Brick0, Brick1 };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick };

	// Clear of both bricks: dZ to brick 1 is 7.5 > sumZ 6.5.
	const FVector Requested(11.25, 0.0, 15.0);
	const FPieceBox Placed{ Requested, HalfBrick };
	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const FVector OccupiedCell(11.25, 0.0, 7.5);
	const FVector ClearSurvivor(22.5, 0.0, 15.0); // brick 1's +X next-course, gapped in Z.
	const double Tol = KINDA_SMALL_NUMBER;

	/*
	 * The filter applies to snaps only; Free is honoured verbatim and survives here only
	 * because the requested pose is clear.
	 */
	bool bOccupiedReturned = false;
	for (const FSnapCandidate& C : Candidates)
	{
		if (C.CentreCm.Equals(OccupiedCell, Tol))
		{
			bOccupiedReturned = true;
		}
	}
	TestFalse(
		TEXT("the occupied-cell candidate at (11.25,0,7.5) is dropped (its box interpenetrates brick 1)"),
		bOccupiedReturned);

	// The filter must not empty the list: brick 1's own next-course pose is clear.
	int32 SurvivorIndex = INDEX_NONE;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		if (Candidates[i].Kind == ESnapKind::BrickNextCourse
			&& Candidates[i].CentreCm.Equals(ClearSurvivor, Tol))
		{
			SurvivorIndex = i;
			break;
		}
	}
	const bool bFoundSurvivor = SurvivorIndex != INDEX_NONE;
	TestTrue(
		TEXT("a clear next-course survivor at (22.5,0,15) is still offered"),
		bFoundSurvivor);
	if (bFoundSurvivor)
	{
		const FSnapCandidate& Snap = Candidates[SurvivorIndex];
		TestEqual(TEXT("survivor forms exactly one bed joint"), Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(TEXT("survivor beds onto brick 1 (OtherPieceIndex 1)"),
				Snap.Joints[0].OtherPieceIndex, 1);
			CheckProfileIdentity(
				*this,
				TEXT("survivor bed joint profile == GeneralPurposeMortar: "),
				Snap.Joints[0].Profile,
				GeneralPurposeMortar);
		}
	}

	// No returned box overlaps any nearby piece, checked independently of production helpers.
	for (const FSnapCandidate& C : Candidates)
	{
		for (int32 j = 0; j < NearbyBoxes.Num(); ++j)
		{
			const FPieceBox& Other = NearbyBoxes[j];
			const bool bOverlapX =
				FMath::Abs(C.CentreCm.X - Other.CentreCm.X) < Placed.ExtentCm.X + Other.ExtentCm.X - Tol;
			const bool bOverlapY =
				FMath::Abs(C.CentreCm.Y - Other.CentreCm.Y) < Placed.ExtentCm.Y + Other.ExtentCm.Y - Tol;
			const bool bOverlapZ =
				FMath::Abs(C.CentreCm.Z - Other.CentreCm.Z) < Placed.ExtentCm.Z + Other.ExtentCm.Z - Tol;
			TestFalse(
				*FString::Printf(
					TEXT("candidate at (%g,%g,%g) does not interpenetrate nearby piece %d"),
					C.CentreCm.X, C.CentreCm.Y, C.CentreCm.Z, j),
				bOverlapX && bOverlapY && bOverlapZ);
		}
	}

	return true;
}

/**
 * 2b, contact survives (regression pin): the next-course pose (11.25, 0, 7.5) overlaps
 * brick 0 in X and Y but is gapped in Z (7.5 > 6.5), so the occupancy filter must keep it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverContactPoseSurvivesTest,
	"DestructionGame.Core.BuildMode.SnapSolverContactPoseSurvives",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverContactPoseSurvivesTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	const FVector Requested(11.0, 0.0, 7.5);
	const FPieceBox Placed{ Requested, HalfBrick };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ContactPose(11.25, 0.0, 7.5);
	const double Tol = KINDA_SMALL_NUMBER;

	int32 ContactIndex = INDEX_NONE;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		if (Candidates[i].Kind == ESnapKind::BrickNextCourse
			&& Candidates[i].CentreCm.Equals(ContactPose, Tol))
		{
			ContactIndex = i;
			break;
		}
	}

	const bool bFound = ContactIndex != INDEX_NONE;
	TestTrue(
		TEXT("the joint-separated next-course contact at (11.25,0,7.5) survives the occupancy filter"),
		bFound);

	if (bFound)
	{
		const FSnapCandidate& Snap = Candidates[ContactIndex];
		TestEqual(TEXT("contact candidate forms exactly one bed joint"), Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(TEXT("contact candidate beds onto brick 0 (OtherPieceIndex 0)"),
				Snap.Joints[0].OtherPieceIndex, 0);
			CheckProfileIdentity(
				*this,
				TEXT("contact bed joint profile == GeneralPurposeMortar: "),
				Snap.Joints[0].Profile,
				GeneralPurposeMortar);
		}
	}

	return true;
}

/**
 * CR-2a, corner return: a brick laid perpendicular to a same-course neighbour is offered
 * four BrickCornerReturn poses (either end x flush with either width face), each with one
 * joint of full GeneralPurposeMortar, never the perpend.
 *
 * Poses: X = 10.75 + 1 + 5.125 = +/-16.875; flush with a width face gives
 * Y = -5.125 + 10.75 = +/-5.625. Flushness makes an L (a quoin, DESIGN §8 2026-09-15)
 * rather than a header through the wall. The 3-argument JointForContact returns the
 * perpend for this horizontal normal; only the boxed overload returns mortar, so a
 * perpend here means the call site was not migrated. Each pose is also fed through
 * MakeInterface and must give the 66.625 cm2 end face with a horizontal normal.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverBrickCornerReturnTest,
	"DestructionGame.Core.BuildMode.SnapSolverBrickCornerReturn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverBrickCornerReturnTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Existing{ FVector(0.0, 0.0, 3.25), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.
	const double Tol = KINDA_SMALL_NUMBER;

	/*
	 * Section one: the cursor on the neighbour's centre is 17.79 cm from all four poses, so
	 * all are in range and their order is a tie (not asserted). Section two pins ranking.
	 */
	const FVector Requested(0.0, 0.0, 3.25);
	const FPieceBox Placed{ Requested, HalfBrickRotated };

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const TArray<FVector> ExpectedPoses = {
		FVector(16.875, 5.625, 3.25),
		FVector(16.875, -5.625, 3.25),
		FVector(-16.875, 5.625, 3.25),
		FVector(-16.875, -5.625, 3.25),
	};

	const double ExpectedOffset = FMath::Sqrt(16.875 * 16.875 + 5.625 * 5.625);

	int32 FreeIndex = INDEX_NONE;
	bool bAnyBedOrHeadKind = false;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		if (FreeIndex == INDEX_NONE && Candidates[i].Kind == ESnapKind::Free)
		{
			FreeIndex = i;
		}
		if (Candidates[i].Kind == ESnapKind::BrickNextCourse
			|| Candidates[i].Kind == ESnapKind::BrickSameCourse)
		{
			bAnyBedOrHeadKind = true;
		}
	}

	for (const FVector& Pose : ExpectedPoses)
	{
		const FString Prefix = FString::Printf(
			TEXT("corner return at (%g,%g,%g): "), Pose.X, Pose.Y, Pose.Z);

		TArray<int32> AtPose;
		for (int32 i = 0; i < Candidates.Num(); ++i)
		{
			if (Candidates[i].Kind == ESnapKind::BrickCornerReturn
				&& Candidates[i].CentreCm.Equals(Pose, Tol))
			{
				AtPose.Add(i);
			}
		}

		TestEqual(*(Prefix + TEXT("exactly one BrickCornerReturn candidate exists")),
			AtPose.Num(), 1);
		if (AtPose.Num() != 1)
		{
			continue;
		}

		const FSnapCandidate& Snap = Candidates[AtPose[0]];

		TestEqual(*(Prefix + TEXT("forms exactly one joint")), Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(*(Prefix + TEXT("the joint is to the existing brick (OtherPieceIndex 0)")),
				Snap.Joints[0].OtherPieceIndex, 0);
			CheckProfileIdentity(
				*this,
				Prefix + TEXT("quoin joint profile == GeneralPurposeMortar: "),
				Snap.Joints[0].Profile,
				GeneralPurposeMortar);
		}

		TestEqual(*(Prefix + TEXT("OffsetFromRequestedCm is the distance to the snap")),
			Snap.OffsetFromRequestedCm, ExpectedOffset, 1.0e-6);

		TestTrue(*(Prefix + TEXT("ranked ahead of the Free fallback")),
			FreeIndex != INDEX_NONE && AtPose[0] < FreeIndex);

		// Composed as Placement.cpp does: the end face, 10.25 x 6.5 = 66.625 cm2, horizontal normal.
		FConnection Connection;
		const FPieceBox AtSnap{ Snap.CentreCm, HalfBrickRotated };
		const bool bFormed = MakeInterface(
			1, AtSnap, 0, Existing, Settings.JointThicknessCm,
			Snap.Joints.Num() == 1 ? Snap.Joints[0].Profile : FConnectionStrength(), Connection);

		TestTrue(*(Prefix + TEXT("the pose forms a face at all (one axis separated by the joint)")),
			bFormed);
		if (bFormed)
		{
			TestEqual(*(Prefix + TEXT("interface area is the brick end face 66.625 cm2")),
				Connection.InterfaceAreaSqCm, 66.625, 1.0e-6);
			TestEqual(*(Prefix + TEXT("the interface normal is on the X axis (|X| == 1)")),
				FMath::Abs(Connection.InterfaceNormal.GetSafeNormal().X), 1.0, Tol);
		}
	}

	// Running bond needs pieces laid the same way, so a crossed pair offers only the corner.
	TestFalse(
		TEXT("no BrickNextCourse/BrickSameCourse candidate for a rotated brick beside an X-long one"),
		bAnyBedOrHeadKind);

	const bool bFoundFree = FreeIndex != INDEX_NONE;
	TestTrue(TEXT("a Free fallback candidate exists"), bFoundFree);
	if (bFoundFree)
	{
		TestTrue(TEXT("Free candidate sits at the requested pose"),
			Candidates[FreeIndex].CentreCm.Equals(Requested, Tol));
		TestEqual(TEXT("Free candidate forms no joints"), Candidates[FreeIndex].Joints.Num(), 0);
		TestEqual(TEXT("Free is the last candidate"), FreeIndex, Candidates.Num() - 1);
	}

	/*
	 * Section two: ranking is by distance. A cursor at (16, 5, 3.25) is 1.08 cm from one
	 * return and 11.1 cm from the next, so that return must come first.
	 */
	const FVector NearRequested(16.0, 5.0, 3.25);
	const FPieceBox NearPlaced{ NearRequested, HalfBrickRotated };

	const TArray<FSnapCandidate> NearCandidates = SolveSnapCandidates(
		NearPlaced, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	TestTrue(TEXT("the cursor near one corner return gets at least one candidate plus Free"),
		NearCandidates.Num() >= 2);
	if (NearCandidates.Num() >= 1)
	{
		TestEqual(TEXT("the nearest corner return ranks FIRST (Kind)"),
			static_cast<int32>(NearCandidates[0].Kind),
			static_cast<int32>(ESnapKind::BrickCornerReturn));
		TestTrue(
			TEXT("the nearest corner return ranks FIRST (pose (16.875, 5.625, 3.25))"),
			NearCandidates[0].CentreCm.Equals(FVector(16.875, 5.625, 3.25), Tol));
	}

	return true;
}

/**
 * CR-2a, Y-run grid: running bond steps along the neighbour's long axis, so a Y-long pair
 * gets next-course (0, 11.25, 7.5) and same-course (0, 22.5, 0) offsets, never the X-stepped
 * poses. Joints are the ordinary bed mortar and head perpend; pins that widening the
 * brick-sized gate did not turn every Y-long pairing into a quoin.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverYLongRunningBondStepsAlongYTest,
	"DestructionGame.Core.BuildMode.SnapSolverYLongRunningBondStepsAlongY",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverYLongRunningBondStepsAlongYTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox Existing{ FVector(0.0, 0.0, 3.25), HalfBrickRotated };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	// Both the next-course (offset 0.25) and same-course (13.73) poses are in range.
	const FVector Requested(0.0, 11.0, 10.75);
	const FPieceBox Placed{ Requested, HalfBrickRotated };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedNextCourse(0.0, 11.25, 10.75);
	const FVector ExpectedSameCourse(0.0, 22.5, 3.25);
	const double Tol = KINDA_SMALL_NUMBER;

	// X-stepped poses would bed a brick half over open air across the wall; they must be absent.
	const FVector XSteppedNextCourse(11.25, 0.0, 10.75);
	const FVector XSteppedSameCourse(22.5, 0.0, 3.25);

	int32 NextCourseIndex = INDEX_NONE;
	int32 SameCourseIndex = INDEX_NONE;
	int32 FreeIndex = INDEX_NONE;
	bool bAnyXSteppedPose = false;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (NextCourseIndex == INDEX_NONE
			&& C.Kind == ESnapKind::BrickNextCourse
			&& C.CentreCm.Equals(ExpectedNextCourse, Tol))
		{
			NextCourseIndex = i;
		}
		if (SameCourseIndex == INDEX_NONE
			&& C.Kind == ESnapKind::BrickSameCourse
			&& C.CentreCm.Equals(ExpectedSameCourse, Tol))
		{
			SameCourseIndex = i;
		}
		if (FreeIndex == INDEX_NONE && C.Kind == ESnapKind::Free)
		{
			FreeIndex = i;
		}
		if (C.CentreCm.Equals(XSteppedNextCourse, Tol)
			|| C.CentreCm.Equals(XSteppedSameCourse, Tol))
		{
			bAnyXSteppedPose = true;
		}
	}

	const bool bFoundNextCourse = NextCourseIndex != INDEX_NONE;
	TestTrue(
		TEXT("a BrickNextCourse candidate exists at the Y stagger (0, 11.25, 10.75)"),
		bFoundNextCourse);
	if (bFoundNextCourse)
	{
		const FSnapCandidate& Snap = Candidates[NextCourseIndex];
		TestEqual(TEXT("Y-run next-course candidate forms exactly one joint"),
			Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(TEXT("Y-run bed joint is to the existing brick (OtherPieceIndex 0)"),
				Snap.Joints[0].OtherPieceIndex, 0);
			CheckProfileIdentity(
				*this,
				TEXT("Y-run bed joint profile == GeneralPurposeMortar: "),
				Snap.Joints[0].Profile,
				GeneralPurposeMortar);
		}
		TestEqual(TEXT("Y-run next-course OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, 0.25, 1.0e-6);
	}

	const bool bFoundSameCourse = SameCourseIndex != INDEX_NONE;
	TestTrue(
		TEXT("a BrickSameCourse candidate exists at the Y pitch (0, 22.5, 3.25)"),
		bFoundSameCourse);
	if (bFoundSameCourse)
	{
		const FSnapCandidate& Snap = Candidates[SameCourseIndex];
		TestEqual(TEXT("Y-run same-course candidate forms exactly one joint"),
			Snap.Joints.Num(), 1);
		if (Snap.Joints.Num() == 1)
		{
			TestEqual(TEXT("Y-run head joint is to the existing brick (OtherPieceIndex 0)"),
				Snap.Joints[0].OtherPieceIndex, 0);
			// Pieces running the same way make a head joint (perpend), not a corner (mortar).
			CheckProfileIdentity(
				*this,
				TEXT("Y-run head joint profile == GeneralPurposeMortarPerpend: "),
				Snap.Joints[0].Profile,
				GeneralPurposeMortarPerpend);
		}
		TestEqual(TEXT("Y-run same-course OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, FMath::Sqrt(11.5 * 11.5 + 7.5 * 7.5), 1.0e-6);
	}

	TestFalse(
		TEXT("no candidate at the X-stepped poses (11.25,0,10.75) / (22.5,0,3.25)"),
		bAnyXSteppedPose);

	TestTrue(TEXT("a Free fallback candidate exists"), FreeIndex != INDEX_NONE);
	if (FreeIndex != INDEX_NONE)
	{
		TestEqual(TEXT("Free is the last candidate"), FreeIndex, Candidates.Num() - 1);
	}

	return true;
}

/**
 * CR-2a, mirror property: a Y-run wall grows exactly as an X-run wall does, reflected
 * through x = y. The oracle is the X-frame solve itself, compared index for index (count,
 * order, kind, pose, offset, joints), so a stray pose or re-ordering cannot hide the way it
 * could from a row table. Settings stay unmirrored: brick length/width are brick
 * properties, not world axes. Each cursor avoids ties so the order is strict.
 * Rows: collinear (bed + head) and crossed (corner return). Add arrangements as rows here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverYWallMirrorsTheXWallTest,
	"DestructionGame.Core.BuildMode.SnapSolverYWallMirrorsTheXWall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverYWallMirrorsTheXWallTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FSnapSettings Settings;
	const double Tol = KINDA_SMALL_NUMBER;

	/*
	 * Stated in the X frame; the Y frame is derived by reflection. ExpectedXCandidates guards
	 * against vacuity: two Free-only solves would mirror trivially.
	 */
	struct FMirrorRow
	{
		const TCHAR* Label;
		FVector NeighbourCentreCm;
		FVector NeighbourExtentCm;
		FVector PlacedExtentCm;
		FVector RequestedCentreCm;
		int32 ExpectedXCandidates;
	};

	const FMirrorRow Rows[] = {
		// Collinear: next-course and same-course snaps plus Free.
		{ TEXT("collinear stretchers"),
			FVector(0.0, 0.0, 3.25), HalfBrick, HalfBrick,
			FVector(11.0, 0.0, 10.75), 3 },

		/*
		 * Crossed (corner return): the cursor is 1.08 and 10.66 cm from the two +X-end
		 * returns; the -X-end ones are outside the 30 cm radius. Two snaps plus Free.
		 */
		{ TEXT("crossed pair, a corner return"),
			FVector(0.0, 0.0, 3.25), HalfBrick, HalfBrickRotated,
			FVector(16.0, 5.0, 3.25), 3 },
	};

	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	for (const FMirrorRow& Row : Rows)
	{
		const FString RowPrefix = FString::Printf(TEXT("[%s] "), Row.Label);

		const FPieceBox XNeighbour{ Row.NeighbourCentreCm, Row.NeighbourExtentCm };
		const TArray<FPieceBox> XBoxes = { XNeighbour };
		const FPieceBox XPlaced{ Row.RequestedCentreCm, Row.PlacedExtentCm };

		const FPieceBox YNeighbour{ SwapXY(XNeighbour.CentreCm), SwapXY(XNeighbour.ExtentCm) };
		const TArray<FPieceBox> YBoxes = { YNeighbour };
		const FPieceBox YPlaced{ SwapXY(XPlaced.CentreCm), SwapXY(XPlaced.ExtentCm) };

		const TArray<FSnapCandidate> XCandidates = SolveSnapCandidates(
			XPlaced, ClayBrick, XBoxes, NearbyMaterials, Settings);
		const TArray<FSnapCandidate> YCandidates = SolveSnapCandidates(
			YPlaced, ClayBrick, YBoxes, NearbyMaterials, Settings);

		TestEqual(*(RowPrefix + TEXT("the X frame offers the expected number of candidates")),
			XCandidates.Num(), Row.ExpectedXCandidates);

		TestEqual(*(RowPrefix + TEXT("the Y frame offers the same number of candidates as the X frame")),
			YCandidates.Num(), XCandidates.Num());

		const int32 Common = FMath::Min(XCandidates.Num(), YCandidates.Num());
		for (int32 i = 0; i < Common; ++i)
		{
			const FSnapCandidate& X = XCandidates[i];
			const FSnapCandidate& Y = YCandidates[i];
			const FString Prefix = RowPrefix + FString::Printf(TEXT("candidate %d: "), i);

			TestEqual(*(Prefix + TEXT("same Kind in both frames")),
				static_cast<int32>(Y.Kind), static_cast<int32>(X.Kind));

			const FVector MirroredCentre = SwapXY(X.CentreCm);
			TestTrue(
				*FString::Printf(
					TEXT("%sthe Y-frame pose (%g,%g,%g) is the X-frame pose (%g,%g,%g) reflected through x = y"),
					*Prefix, Y.CentreCm.X, Y.CentreCm.Y, Y.CentreCm.Z,
					X.CentreCm.X, X.CentreCm.Y, X.CentreCm.Z),
				Y.CentreCm.Equals(MirroredCentre, Tol));

			// A reflection is an isometry, so the ranking key is unchanged.
			TestEqual(*(Prefix + TEXT("same OffsetFromRequestedCm in both frames")),
				Y.OffsetFromRequestedCm, X.OffsetFromRequestedCm, 1.0e-9);

			TestEqual(*(Prefix + TEXT("same number of formed joints in both frames")),
				Y.Joints.Num(), X.Joints.Num());

			const int32 CommonJoints = FMath::Min(X.Joints.Num(), Y.Joints.Num());
			for (int32 j = 0; j < CommonJoints; ++j)
			{
				TestEqual(
					*FString::Printf(TEXT("%sjoint %d names the same neighbour"), *Prefix, j),
					Y.Joints[j].OtherPieceIndex, X.Joints[j].OtherPieceIndex);
				CheckProfileIdentity(
					*this,
					FString::Printf(TEXT("%sjoint %d has the same profile in both frames: "), *Prefix, j),
					Y.Joints[j].Profile,
					X.Joints[j].Profile);
			}
		}
	}

	return true;
}

/**
 * CR-2a finding B2, crossed bed: a NextCourse candidate forms a bed joint to every
 * brick-sized neighbour it rests on, whichever way that neighbour runs. The course-1
 * stretcher over an L's corner laps half onto the crossed return; without that bed the
 * quoin hangs on its vertical joint, which routes no vertical load.
 *
 * Leg end (X-long) at (45, 0, 3.25) and return (Y-long) at (61.875, 5.625, 3.25); the
 * placed brick at (56.25, 0, 10.75) sits one bed joint above both, with a 10.25 x 10.25 =
 * 105.0625 cm2 lap on each. Kind is only asserted not-Free: the merged label is
 * order-dependent (CURRENT_STATE snap-solver item vii).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSnapSolverNextCourseBedsOnACrossedNeighbourTest,
	"DestructionGame.Core.BuildMode.SnapSolverNextCourseBedsOnACrossedNeighbour",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSnapSolverNextCourseBedsOnACrossedNeighbourTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace SnapSolverTestSupport;

	const FPieceBox LegEnd{ FVector(45.0, 0.0, 3.25), HalfBrick };
	const FPieceBox Return{ FVector(61.875, 5.625, 3.25), HalfBrickRotated };
	const TArray<FPieceBox> NearbyBoxes = { LegEnd, Return };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick };

	const FVector Requested(56.25, 0.0, 10.75);
	const FPieceBox Placed{ Requested, HalfBrick };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.
	const double Tol = KINDA_SMALL_NUMBER;

	const double BrickWidthCm = Settings.BrickSizeCm.Y; // 10.25
	const double ExpectedBedAreaSqCm = BrickWidthCm * BrickWidthCm; // 105.0625

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	TestTrue(TEXT("the solver offers at least one snap plus the Free fallback"),
		Candidates.Num() >= 2);
	if (Candidates.Num() < 1)
	{
		return false;
	}

	const FSnapCandidate& Best = Candidates[0];

	// The cursor sits exactly on the course-up pose, so it has offset 0 and ranks first.
	TestTrue(
		*FString::Printf(
			TEXT("the first candidate sits at (56.25, 0, 10.75), got (%g,%g,%g)"),
			Best.CentreCm.X, Best.CentreCm.Y, Best.CentreCm.Z),
		Best.CentreCm.Equals(Requested, Tol));
	TestEqual(TEXT("the first candidate is one course (7.5 cm) above the neighbours"),
		Best.CentreCm.Z - LegEnd.CentreCm.Z, 7.5, 1.0e-9);
	TestEqual(TEXT("the first candidate's OffsetFromRequestedCm is zero"),
		Best.OffsetFromRequestedCm, 0.0, 1.0e-9);
	TestNotEqual(TEXT("the first candidate is a snap, not the Free fallback"),
		static_cast<int32>(Best.Kind), static_cast<int32>(ESnapKind::Free));

	// Joints are looked up by neighbour index; append order is an implementation detail.
	TestEqual(TEXT("the course-up candidate forms TWO bed joints"), Best.Joints.Num(), 2);

	for (int32 j = 0; j < NearbyBoxes.Num(); ++j)
	{
		const FString Prefix = FString::Printf(
			TEXT("bed onto neighbour %d (%s): "), j, (j == 0) ? TEXT("collinear") : TEXT("crossed"));

		const FFormedJoint* Joint = Best.Joints.FindByPredicate(
			[j](const FFormedJoint& Have) { return Have.OtherPieceIndex == j; });

		TestNotNull(*(Prefix + TEXT("a joint to it exists")), Joint);
		if (Joint == nullptr)
		{
			continue;
		}

		// A vertical normal classifies as Bed before orientation is consulted: full mortar.
		CheckProfileIdentity(
			*this,
			Prefix + TEXT("profile == GeneralPurposeMortar: "),
			Joint->Profile,
			GeneralPurposeMortar);

		// Composed as Placement.cpp does: a 105.0625 cm2 lap across a vertical normal.
		FConnection Connection;
		const FPieceBox AtSnap{ Best.CentreCm, HalfBrick };
		const bool bFormed = MakeInterface(
			2, AtSnap, j, NearbyBoxes[j], Settings.JointThicknessCm, Joint->Profile, Connection);

		TestTrue(*(Prefix + TEXT("the pose forms a face at all")), bFormed);
		if (bFormed)
		{
			TestEqual(*(Prefix + TEXT("interface area is the 10.25 x 10.25 lap, 105.0625 cm2")),
				Connection.InterfaceAreaSqCm, ExpectedBedAreaSqCm, 1.0e-6);
			TestEqual(*(Prefix + TEXT("the interface normal is vertical (|Z| == 1)")),
				FMath::Abs(Connection.InterfaceNormal.GetSafeNormal().Z), 1.0, Tol);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
