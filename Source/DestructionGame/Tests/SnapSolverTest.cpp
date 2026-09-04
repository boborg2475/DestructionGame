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

/**
 * BEHAVIOR 2b, the SECOND snap kind: brick SAME-COURSE END-TO-END (the head joint).
 * A ClayBrick placed BESIDE an existing ClayBrick on the SAME course (same Z,
 * abutting its END face) offers a BrickSameCourse candidate at the coordinating-grid
 * pose (existing centre + one same-course pitch in X = 22.5, SAME Y, SAME Z) that
 * auto-forms exactly one HEAD joint to it, plus a Free fallback ranked last.
 *
 * THE HEAD JOINT IS THE PERPEND, NOT THE BED. The shared face is an END face, so the
 * interface normal is HORIZONTAL (+/-X); JointForContact with a +/-X normal returns
 * the WEAK GeneralPurposeMortarPerpend, not the strong bed GeneralPurposeMortar. The
 * profile is pinned FULL-FIELD so a bed-normal mistake (mortar) or a plain-mortar
 * substitution fails — the solver must DERIVE this via JointForContact with a
 * horizontal normal, never hardcode it.
 *
 * THE SAME-COURSE PITCH IS READ, NOT RE-DERIVED: brick length 21.5 + 1 cm head joint
 * = the 22.5 cm coordinating pitch in X, at the SAME Z (not a course up).
 *
 * Guard: the same-course pose must NOT ALSO be emitted as a BrickNextCourse candidate
 * — the requested pose is at the same Z, not a course up. (The next-course logic only
 * ever offers poses one course up at Z=7.5, so a next-course candidate landing at the
 * same-course pose would be a genuine cross-wire.)
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

	// One existing full brick at the origin.
	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	/*
	 * The piece being placed: same size, requested BESIDE it on the same course
	 * toward +X (same Y, same Z=0). 21 is near the 22.5 pitch and well inside the
	 * 30 cm snap radius.
	 */
	const FVector Requested(21.0, 0.0, 0.0);
	const FPieceBox Placed{ Requested, HalfBrick };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	/*
	 * Same-course pitch in X = BrickSizeCm.X + JointThicknessCm = 22.5, on the +X
	 * side the requested pose leans toward, SAME Y and SAME Z: origin + (22.5,0,0).
	 * Tight tolerance — this is an exact coordinate.
	 */
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
		// Guard: a BrickNextCourse candidate must never land at the same-course pose.
		if (C.Kind == ESnapKind::BrickNextCourse
			&& C.CentreCm.Equals(ExpectedSameCourseCentre, Tol))
		{
			bNextCourseAtSameCoursePose = true;
		}
	}

	// 1. The same-course candidate exists at the exact +X coordinating pose.
	const bool bFoundSameCourse = SameCourseIndex != INDEX_NONE;
	TestTrue(
		TEXT("a BrickSameCourse candidate exists at origin + (22.5, 0, 0)"),
		bFoundSameCourse);

	// 3. A Free fallback candidate exists at the requested pose.
	const bool bFoundFree = FreeIndex != INDEX_NONE;
	TestTrue(TEXT("a Free fallback candidate exists"), bFoundFree);

	if (bFoundSameCourse)
	{
		const FSnapCandidate& Snap = Candidates[SameCourseIndex];

		/*
		 * 2. Exactly ONE joint, to the existing brick (index 0 into NearbyBoxes),
		 * whose profile is the WEAK perpend. The solver must DERIVE this via
		 * JointForContact with a +/-X (horizontal, head-face) normal — pinned
		 * full-field so a bed-normal mistake or a plain-mortar substitution fails.
		 */
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

		/*
		 * The offset field carries the ranking key's meaning: the Euclidean distance
		 * from the requested pose (21,0,0) to the snapped pose (22.5,0,0) = 1.5.
		 */
		TestEqual(TEXT("same-course OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, 1.5, 1.0e-6);
	}

	if (bFoundFree)
	{
		// 3 (cont). The Free candidate sits at the requested pose and forms no joint.
		const FSnapCandidate& Free = Candidates[FreeIndex];
		TestTrue(TEXT("Free candidate sits at the requested pose"),
			Free.CentreCm.Equals(Requested, Tol));
		TestEqual(TEXT("Free candidate forms no joints"), Free.Joints.Num(), 0);
	}

	// 3 (cont). Free stays LAST — behind every snap.
	if (bFoundFree)
	{
		TestEqual(TEXT("Free is the last candidate"),
			FreeIndex, Candidates.Num() - 1);
	}

	// 1 (cont). An in-range snap BEATS free: the same-course candidate precedes it.
	if (bFoundSameCourse && bFoundFree)
	{
		TestTrue(
			TEXT("BrickSameCourse is ranked ahead of Free (appears earlier)"),
			SameCourseIndex < FreeIndex);
	}

	// Guard: no BrickNextCourse candidate spuriously at the same-course pose.
	TestFalse(
		TEXT("no BrickNextCourse candidate lands at the same-course pose (22.5,0,0)"),
		bNextCourseAtSameCoursePose);

	return true;
}

/**
 * BEHAVIOR 2b, EMERGENT WALL JOINT SET (regression pin). The point of both snap
 * kinds together: a brick laid into a running-bond wall PAST the first course forms
 * BOTH a bed joint (down onto the brick below) AND a head joint (across to the
 * adjacent brick on its own course), and the pose-keyed merge coalesces the two into
 * ONE candidate carrying BOTH joints — the first case producing the joint set a real
 * wall needs. This guards that merge.
 *
 * Fixture: course 0 has bricks at (0,0,0) [idx 0] and (22.5,0,0) [idx 1]; course 1
 * has a brick at (11.25,0,7.5) [idx 2]. A brick placed at the running-bond pose
 * (33.75,0,7.5) beds onto the course-0 brick@22.5 (next-course, +Z bed normal ->
 * GeneralPurposeMortar) AND abuts the course-1 brick@11.25 end-to-end (same-course,
 * +/-X head normal -> GeneralPurposeMortarPerpend).
 *
 * Kind is deliberately NOT asserted: which neighbour emits the merged candidate first
 * decides its label, a known ordering nit logged separately. Joints are matched to
 * neighbours by OtherPieceIndex, never by array order.
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

	// Two bricks on course 0, one on course 1.
	const FPieceBox Course0Left{ FVector(0.0, 0.0, 0.0), HalfBrick };   // idx 0
	const FPieceBox Course0Right{ FVector(22.5, 0.0, 0.0), HalfBrick }; // idx 1
	const FPieceBox Course1Mid{ FVector(11.25, 0.0, 7.5), HalfBrick };  // idx 2
	const TArray<FPieceBox> NearbyBoxes = { Course0Left, Course0Right, Course1Mid };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick, ClayBrick };

	// The brick being placed: the running-bond pose that both beds onto idx 1 and
	// abuts idx 2 end-to-end.
	const FVector Requested(33.75, 0.0, 7.5);
	const FPieceBox Placed{ Requested, HalfBrick };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedCentre(33.75, 0.0, 7.5);
	const double Tol = KINDA_SMALL_NUMBER;

	const int32 BedNeighbour = 1;  // course-0 brick@22.5, one course below -> bed.
	const int32 HeadNeighbour = 2; // course-1 brick@11.25, same course -> head.

	// Every non-Free candidate sitting at the merged pose. Expect exactly one.
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

		// It carries BOTH joints — the bed and the head — merged into one candidate.
		TestEqual(TEXT("merged candidate forms two joints (bed + head)"),
			Snap.Joints.Num(), 2);

		if (Snap.Joints.Num() == 2)
		{
			// Match joints to neighbours by OtherPieceIndex, not array order.
			const FFormedJoint* BedJoint = Snap.Joints.FindByPredicate(
				[BedNeighbour](const FFormedJoint& J)
				{ return J.OtherPieceIndex == BedNeighbour; });
			const FFormedJoint* HeadJoint = Snap.Joints.FindByPredicate(
				[HeadNeighbour](const FFormedJoint& J)
				{ return J.OtherPieceIndex == HeadNeighbour; });

			// The OtherPieceIndex set is exactly {bed neighbour, head neighbour}.
			TestNotNull(TEXT("a joint to the course-0 brick@22.5 (bed neighbour) exists"),
				BedJoint);
			TestNotNull(TEXT("a joint to the course-1 brick@11.25 (head neighbour) exists"),
				HeadJoint);

			if (BedJoint != nullptr)
			{
				// Bed joint: +Z normal -> strong bed mortar, full-field.
				CheckProfileIdentity(
					*this,
					TEXT("bed joint (to brick@22.5) == GeneralPurposeMortar: "),
					BedJoint->Profile,
					GeneralPurposeMortar);
			}
			if (HeadJoint != nullptr)
			{
				// Head joint: +/-X normal -> weak perpend, full-field.
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

#endif // WITH_DEV_AUTOMATION_TESTS
