// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Unit tests for the snap-candidate solver — BUILD_MODE_PLAN.md behavior 2a, the
 * FIRST snap kind: brick-on-brick RUNNING-BOND NEXT COURSE.
 *
 * Pure and world-free by design: no gravity, no solver, no ticking. The mechanism
 * under test is deterministic geometry + material-pairing, so the assertions are
 * on the returned CANDIDATE POSES, their FORMED JOINTS and their ORDER — never on
 * any load, displacement or solve. (Displacement would be meaningless here anyway:
 * nothing moves.)
 *
 * THE RUNNING-BOND NUMBERS ARE READ, NOT RE-DERIVED. Core/Layout.h documents that
 * a 21.5 x 10.25 x 6.5 brick on 1 cm joints gives the 22.5 x 11.25 x 7.5
 * coordinating grid; the half-brick stagger is therefore +11.25 in X and one
 * course is +7.5 in Z.
 */

/*
 * NAMED NAMESPACE, named distinctly from every other one in this module — an
 * anonymous namespace is private to a TRANSLATION UNIT, not a file, and a unity
 * build merges files. See JointInferenceTest.cpp / ConnectionLoadTest.cpp for the
 * incident that established this rule.
 */
namespace SnapSolverTestSupport
{
	const FVector HalfBrick(10.75, 5.125, 3.25);

	/*
	 * Full-field profile identity. FConnectionStrength has no operator==, so a joint
	 * profile is pinned by matching all five fields against the named library
	 * constant — the same discipline JointInferenceTest uses. This is what proves the
	 * auto-formed joint is the intended profile (strong bed mortar vs weak perpend vs
	 * dry-stone bearing), which differ on the two bond axes, on compression, on
	 * friction and on the shear ceiling. Asserting one field alone would let a sibling
	 * profile through.
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
 * BEHAVIOR 2a, single neighbour. A ClayBrick placed near the top of and offset
 * toward +X of ONE existing ClayBrick offers a BrickNextCourse candidate at the
 * running-bond half-stagger pose (origin + (11.25, 0, 7.5)) that auto-forms exactly
 * one GeneralPurposeMortar bed joint to it, plus a Free fallback at the requested
 * pose, ranked ahead of Free.
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

	// One existing full brick at the origin.
	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	// The piece being placed: same size, requested ABOVE and biased toward +X.
	const FVector Requested(11.0, 0.0, 7.5);
	const FPieceBox Placed{ Requested, HalfBrick };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	/*
	 * The running-bond half-stagger on the +X side the requested pose leans toward,
	 * one course up: origin + (11.25, 0, 7.5). Tight tolerance — this is an exact
	 * coordinate, not a fitted value.
	 */
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

	// 1. The running-bond next-course candidate exists at the exact +X stagger pose.
	const bool bFoundNextCourse = NextCourseIndex != INDEX_NONE;
	TestTrue(
		TEXT("a BrickNextCourse candidate exists at origin + (11.25, 0, 7.5)"),
		bFoundNextCourse);

	// 3. A Free fallback candidate exists at the requested pose.
	const bool bFoundFree = FreeIndex != INDEX_NONE;
	TestTrue(TEXT("a Free fallback candidate exists"), bFoundFree);

	if (bFoundNextCourse)
	{
		const FSnapCandidate& Snap = Candidates[NextCourseIndex];

		/*
		 * 2. Exactly ONE joint, to the existing brick (index 0 into NearbyBoxes),
		 * whose profile is the STRONG bed mortar. The solver must DERIVE this via
		 * JointForContact with a +/-Z (bed) normal, not hardcode it.
		 */
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

		/*
		 * The offset field carries the ranking key's meaning: the Euclidean distance
		 * from the requested pose (11,0,7.5) to the snapped pose (11.25,0,7.5) = 0.25.
		 */
		TestEqual(TEXT("next-course OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, 0.25, 1.0e-6);
	}

	if (bFoundFree)
	{
		// 3 (cont). The Free candidate sits at the requested pose and forms no joint.
		const FSnapCandidate& Free = Candidates[FreeIndex];
		TestTrue(TEXT("Free candidate sits at the requested pose"),
			Free.CentreCm.Equals(Requested, Tol));
		TestEqual(TEXT("Free candidate forms no joints"), Free.Joints.Num(), 0);
	}

	/*
	 * 4. Ranking sanity: an in-range snap BEATS free. Free always sits AT the
	 * requested pose (offset 0), so a naive offset-ascending sort would rank it
	 * first — the solver must instead order the snap ahead of the fallback. Pinned
	 * by array position: the next-course candidate appears BEFORE the Free one.
	 */
	if (bFoundNextCourse && bFoundFree)
	{
		TestTrue(
			TEXT("BrickNextCourse is ranked ahead of Free (appears earlier)"),
			NextCourseIndex < FreeIndex);
	}

	return true;
}

/**
 * BEHAVIOR 2a, TWO-NEIGHBOUR STRADDLE. A running-bond brick laid at the ordinary
 * pose straddles the TWO bricks below it and beds onto BOTH — one bed joint each.
 * The solver must therefore emit ONE candidate holding TWO joints, not one
 * candidate per neighbour each holding half.
 *
 * Fixture: two full bricks side by side at X = 0 and X = 22.5 (adjacent on the
 * coordinating grid); a brick placed at the running-bond pose (11.25, 0, 7.5) sits
 * squarely on the joint between them, half over each.
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

	// Count every BrickNextCourse candidate sitting at the straddle pose.
	TArray<int32> StraddleCandidateIdx;
	for (int32 i = 0; i < Candidates.Num(); ++i)
	{
		const FSnapCandidate& C = Candidates[i];
		if (C.Kind == ESnapKind::BrickNextCourse && C.CentreCm.Equals(ExpectedCentre, Tol))
		{
			StraddleCandidateIdx.Add(i);
		}
	}

	// EXACTLY ONE candidate at the straddle pose — not one per neighbour.
	TestEqual(
		TEXT("exactly one BrickNextCourse candidate at the straddle pose (11.25,0,7.5)"),
		StraddleCandidateIdx.Num(), 1);

	if (StraddleCandidateIdx.Num() == 1)
	{
		const FSnapCandidate& Snap = Candidates[StraddleCandidateIdx[0]];

		// It beds onto BOTH bricks: two joints.
		TestEqual(TEXT("straddle candidate forms two joints"), Snap.Joints.Num(), 2);

		if (Snap.Joints.Num() == 2)
		{
			/*
			 * Both neighbours present, order-independent: the OtherPieceIndex set is
			 * {0, 1}. Also proves the two are distinct (no neighbour bedded twice).
			 */
			const int32 IdxA = Snap.Joints[0].OtherPieceIndex;
			const int32 IdxB = Snap.Joints[1].OtherPieceIndex;
			const bool bBothNeighbours =
				(IdxA == 0 && IdxB == 1) || (IdxA == 1 && IdxB == 0);
			TestTrue(
				TEXT("straddle candidate beds onto both bricks (OtherPieceIndex set == {0,1})"),
				bBothNeighbours);

			// Both are STRONG bed mortar.
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
 * BEHAVIOR 2a, OFFSET RANKING among snaps. When several running-bond poses are in
 * range, the candidates are ordered by proximity to the requested pose (smallest
 * OffsetFromRequestedCm first), NOT by input order — and the Free fallback stays
 * LAST.
 *
 * Fixture: two bricks at X = 0 and X = 22.5; the piece requested at (33.75, 0, 7.5)
 * — exactly the +X running-bond pose of the RIGHT brick (offset 0), and 22.5 cm
 * from the +X pose of the LEFT brick (11.25). The nearer (right) candidate must
 * come first even though its neighbour is second in the input.
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

	// The offset field carries the sort key's meaning.
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

	// Nearer snap ranked ahead of the farther one (proximity, not input order).
	if (bFoundNear && bFoundFar)
	{
		TestTrue(
			TEXT("the nearer snap (offset 0) is ranked ahead of the farther snap (offset 22.5)"),
			NearIndex < FarIndex);
	}

	// Free stays LAST — behind every snap.
	if (bFoundFree)
	{
		TestEqual(TEXT("Free is the last candidate"),
			FreeIndex, Candidates.Num() - 1);
	}

	return true;
}

/**
 * BEHAVIOR 2a, TIMBER-ON-BRICK material row (regression pin). A brick-SIZED Timber
 * block placed at the running-bond pose above a brick still snaps to the next
 * course (geometry is size-driven), but the auto-formed joint is the PASSIVE
 * DryStone bearing, not mortar — because the profile comes from JointForContact
 * with the two MATERIALS, not from a hardcoded masonry constant.
 *
 * This proves the materials actually reach the inference: a mutation replacing the
 * JointForContact call with a hardcoded GeneralPurposeMortar fails here (full-field
 * DryStone != full-field mortar on compression, both bond axes, friction and the
 * shear ceiling).
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

	// Brick-sized TIMBER block placed above and biased +X.
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
			/*
			 * Timber contact -> passive DryStone bearing (the owner-delegated ruling).
			 * Full-field, so a hardcoded-mortar mutation cannot pass.
			 */
			CheckProfileIdentity(
				*this,
				TEXT("timber-on-brick joint profile == DryStone: "),
				Snap.Joints[0].Profile,
				DryStone);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
