// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Unit tests for the snap-candidate solver (BUILD_MODE_PLAN.md behavior 2a).
 *
 * World-free: no gravity, solver or ticking. Assertions are on the returned candidate
 * poses, their joints and their order, never on load or displacement.
 *
 * Running-bond numbers come from Core/Layout.h: a 21.5 x 10.25 x 6.5 brick on 1 cm
 * joints gives the 22.5 x 11.25 x 7.5 grid, so the half-brick stagger is +11.25 in X
 * and a course is +7.5 in Z.
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
	 * THE SAME BRICK TURNED THROUGH 90 DEGREES — the piece a corner return and a Y-run
	 * wall are made of. A UK metric brick is 21.5 x 10.25 x 6.5, so the stretcher laid
	 * along X is (10.75, 5.125, 3.25) and the same brick running along Y is
	 * (5.125, 10.75, 3.25). Nothing about it is a different piece: the solver's
	 * brick-sized gate has to accept BOTH footprints or a rotated brick snaps to nothing.
	 */
	const FVector HalfBrickRotated(5.125, 10.75, 3.25);

	/** The X and Y components of a vector swapped — the mirror an X-run maps to a Y-run by. */
	FVector SwapXY(const FVector& V)
	{
		return FVector(V.Y, V.X, V.Z);
	}

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

	/*
	 * The brick being placed: the running-bond pose that both beds onto idx 1 and
	 * abuts idx 2 end-to-end.
	 */
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

/**
 * BEHAVIOR 2d, the TIMBER CENTERED-ON-a-brick snap. A TIMBER piece (a plank/lintel,
 * NOT brick-sized) placed above and roughly over a brick snaps to sit CENTERED on the
 * brick's top face: horizontal centre aligned to the brick, resting one joint-thickness
 * above it, auto-forming exactly one PASSIVE DryStone bearing joint to it.
 *
 * THE POSE IS EXACT, NOT FITTED. Horizontal centre snaps to the brick centre (X=0, Y=0);
 * the height is the brick TOP face + one bed joint + the placed piece's HALF height:
 * 3.25 + 1.0 + 1.5 = 5.75. So the snapped centre is (0, 0, 5.75). The requested pose is
 * deliberately off-centre in X (2) and low (5) so "centering" and the one-joint rise are
 * both observable, and well inside the 30 cm snap radius.
 *
 * THE JOINT IS DERIVED, NOT HARDCODED. A timber face is not compression-dominant, so
 * JointForContact(Timber, ClayBrick, +/-Z) returns the passive DryStone bearing
 * REGARDLESS of the normal (the passive-bearing ruling). Pinned FULL-FIELD so a mortar
 * substitution or a DryStone hardcode that ignores the materials cannot pass.
 *
 * GUARD: a TIMBER lintel is NOT brick-sized (60 x 10.25 x 3.0 full dims != the 21.5 x
 * 10.25 x 6.5 coordinating brick), so the brick kinds — which gate on IsBrickSized —
 * must NOT fire. Assert no BrickNextCourse/BrickSameCourse candidate appears, and that
 * the Free fallback still exists and is ranked LAST behind the timber snap.
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

	// One existing full brick at the origin: top face at Z = HalfBrick.Z = 3.25.
	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	/*
	 * A TIMBER lintel/plank: wider in X than the brick, thin in Z — deliberately NOT
	 * brick-sized (full dims 60 x 10.25 x 3.0). Requested above the brick, off-centre
	 * in X and low in Z so centering and the one-joint rise are both observable.
	 */
	const FVector PlacedHalfExtent(30.0, 5.125, 1.5);
	const FVector Requested(2.0, 0.0, 5.0);
	const FPieceBox Placed{ Requested, PlacedHalfExtent };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	/*
	 * Centered on the brick top: X = brick centre X (0), Y = brick centre Y (0), Z =
	 * brick top (3.25) + one joint (1.0) + placed half-height (1.5) = 5.75. An exact
	 * coordinate, tight tolerance.
	 */
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

	// 1. The timber centered-on candidate exists at the exact centered pose (0,0,5.75).
	const bool bFoundCentered = CenteredIndex != INDEX_NONE;
	TestTrue(
		TEXT("a TimberCentered candidate exists at the brick-centred pose (0,0,5.75)"),
		bFoundCentered);

	// 3. A Free fallback candidate exists at the requested pose.
	const bool bFoundFree = FreeIndex != INDEX_NONE;
	TestTrue(TEXT("a Free fallback candidate exists"), bFoundFree);

	if (bFoundCentered)
	{
		const FSnapCandidate& Snap = Candidates[CenteredIndex];

		/*
		 * 2. Exactly ONE joint, to the existing brick (index 0 into NearbyBoxes),
		 * whose profile is the PASSIVE DryStone bearing. The solver must DERIVE this
		 * via JointForContact(Timber, ClayBrick, +/-Z) — pinned FULL-FIELD so a mortar
		 * substitution fails (DryStone != mortar on compression, both bond axes,
		 * friction and the shear ceiling).
		 */
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

		/*
		 * The offset field carries the ranking key's meaning: the Euclidean distance
		 * from the requested pose (2,0,5) to the snapped pose (0,0,5.75) =
		 * sqrt(4 + 0.5625) = sqrt(4.5625). Derived independently from the distance
		 * formula, not read back from production.
		 */
		TestEqual(TEXT("timber centered OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, FMath::Sqrt(4.5625), 1.0e-6);
	}

	if (bFoundFree)
	{
		// 3 (cont). The Free candidate sits at the requested pose and forms no joint.
		const FSnapCandidate& Free = Candidates[FreeIndex];
		TestTrue(TEXT("Free candidate sits at the requested pose"),
			Free.CentreCm.Equals(Requested, Tol));
		TestEqual(TEXT("Free candidate forms no joints"), Free.Joints.Num(), 0);

		// 3 (cont). Free stays LAST — behind every snap.
		TestEqual(TEXT("Free is the last candidate"),
			FreeIndex, Candidates.Num() - 1);
	}

	// 1 (cont). An in-range snap BEATS free: the timber candidate precedes it.
	if (bFoundCentered && bFoundFree)
	{
		TestTrue(
			TEXT("TimberCentered is ranked ahead of Free (appears earlier)"),
			CenteredIndex < FreeIndex);
	}

	/*
	 * 4. Guard: a non-brick-sized TIMBER lintel must not cross-fire the brick kinds.
	 * The brick snaps gate on IsBrickSized; this lintel is not brick-sized, so it may
	 * only ever be TimberCentered (+ later timber kinds) or Free.
	 */
	TestFalse(
		TEXT("no BrickNextCourse/BrickSameCourse candidate for a non-brick-sized timber lintel"),
		bAnyBrickKind);

	return true;
}


/**
 * BEHAVIOR 2d, LINTEL BEARS ON ALL IT SPANS (RED driver). A timber lintel centered
 * on a brick rests on EVERY brick it overlaps, not just the one it centred on — so
 * the single TimberCentered candidate must carry one passive DryStone bearing per
 * spanned brick.
 *
 * Fixture: a course of three ClayBricks at X = 0, 22.5, 45 (all centre Z = 0, extent
 * HalfBrick). A TIMBER lintel of half-extent (30, 5.125, 1.5) — full length 60 cm —
 * centred on brick@22.5 spans X in [-7.5, 52.5], overlapping all three bricks. The
 * requested pose (24, 0, 5) leans toward brick@22.5 and above, so the centered-on
 * pose is (22.5, 0, 5.75): brick centre X/Y, brick top (3.25) + joint (1.0) + placed
 * half-height (1.5).
 *
 * Assert on the ONE candidate at (22.5,0,5.75): exactly one such candidate, three
 * joints, OtherPieceIndex set == {0,1,2} (order-independent), all three full-field
 * DryStone. Other TimberCentered candidates centred on brick@0 or brick@45 may also
 * exist at their own poses — not forbidden, simply not asserted on.
 *
 * RED today: the candidate carries a single bearing to brick@22.5 only.
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

	// A course of three bricks the lintel will span.
	const FPieceBox Brick0{ FVector(0.0, 0.0, 0.0), HalfBrick };   // idx 0
	const FPieceBox Brick1{ FVector(22.5, 0.0, 0.0), HalfBrick };  // idx 1
	const FPieceBox Brick2{ FVector(45.0, 0.0, 0.0), HalfBrick };  // idx 2
	const TArray<FPieceBox> NearbyBoxes = { Brick0, Brick1, Brick2 };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick, ClayBrick };

	/*
	 * A TIMBER lintel, full length 60 cm: centred on brick@22.5 it spans [-7.5, 52.5]
	 * and rests on all three bricks. Requested near brick@22.5 and above.
	 */
	const FVector PlacedHalfExtent(30.0, 5.125, 1.5);
	const FVector Requested(24.0, 0.0, 5.0);
	const FPieceBox Placed{ Requested, PlacedHalfExtent };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	// Centered on brick@22.5: (22.5, 0, brick top 3.25 + joint 1.0 + half-height 1.5).
	const FVector ExpectedCentre(22.5, 0.0, 5.75);
	const double Tol = KINDA_SMALL_NUMBER;

	// Every TimberCentered candidate AT the brick@22.5 pose. Expect exactly one.
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

		// It bears on ALL three spanned bricks: three joints.
		TestEqual(TEXT("lintel candidate forms three bearing joints (one per spanned brick)"),
			Snap.Joints.Num(), 3);

		if (Snap.Joints.Num() == 3)
		{
			/*
			 * The OtherPieceIndex set is exactly {0, 1, 2}, order-independent — each
			 * spanned brick bedded once, none twice. Match by index, never array order.
			 */
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
					// Timber contact -> passive DryStone bearing, full-field.
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
 * BEHAVIOR 2e, the TIMBER EDGE-FLUSH snap. A TIMBER plank NARROWER than the brick in
 * X, requested off toward the brick's +X horizontal edge, snaps so its NEAR (+X) face
 * aligns FLUSH with the brick's +X face rather than centering — still resting one
 * joint-thickness on top and forming a passive DryStone bearing. This is the second
 * timber snap the /goal names ("timber centered-on / edge-of a brick").
 *
 * THE POSE IS EXACT, NOT FITTED. The brick at the origin has extent HalfBrick, so its
 * +X face is at X = 10.75 and its top at Z = 3.25. The plank's half-extent in X is 5
 * (10 cm wide — deliberately NARROWER than the 21.5 cm brick, so edge-flush is a
 * DISTINCT pose from centered). Flushing the plank's +X face to the brick's +X face
 * puts the plank centre at X = brick +X face - plank half-X = 10.75 - 5 = 5.75; Y stays
 * on the brick centre (0); Z is brick top (3.25) + one joint (1.0) + plank half-height
 * (1.5) = 5.75. So the snapped centre is (5.75, 0, 5.75), and the flush edge reads back
 * as 5.75 + 5 = 10.75 == the brick's +X face. The requested pose (8, 0, 5) leans toward
 * that +X edge and above.
 *
 * THE JOINT IS DERIVED, NOT HARDCODED. A timber face is not compression-dominant, so
 * JointForContact(Timber, ClayBrick, +/-Z) returns the passive DryStone bearing.
 * Pinned FULL-FIELD so a mortar substitution or a materials-ignoring DryStone hardcode
 * cannot pass. The plank at X=5.75 spans [0.75, 10.75], overlapping ONLY brick 0 — so
 * exactly one bearing.
 *
 * RANKING: the requested pose (8,·) is nearer the edge-flush pose (offset sqrt(5.625) ~
 * 2.37) than the centered pose (0,0,5.75) (offset sqrt(64.5625) ~ 8.04), so the
 * edge-flush candidate must rank AHEAD of any TimberCentered candidate. The solver MAY
 * still offer TimberCentered — that is not forbidden; only its ranking behind the
 * edge-flush snap is pinned. Free stays LAST.
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

	// One existing full brick at the origin: +X face at X = 10.75, top at Z = 3.25.
	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	/*
	 * A TIMBER plank NARROWER than the brick in X (half-extent 5 -> 10 cm wide), so
	 * flush-to-edge is a distinct pose from centered. Requested off toward the +X edge
	 * and above.
	 */
	const FVector PlacedHalfExtent(5.0, 5.125, 1.5);
	const FVector Requested(8.0, 0.0, 5.0);
	const FPieceBox Placed{ Requested, PlacedHalfExtent };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	/*
	 * +X face flush: X = brick +X face (10.75) - plank half-X (5) = 5.75; Y = brick
	 * centre (0); Z = brick top (3.25) + joint (1.0) + plank half-height (1.5) = 5.75.
	 * An exact coordinate, tight tolerance.
	 */
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

	// 1. The edge-flush candidate exists at the exact +X-flush pose (5.75, 0, 5.75).
	const bool bFoundEdgeFlush = EdgeFlushIndex != INDEX_NONE;
	TestTrue(
		TEXT("a TimberEdgeFlush candidate exists at the +X-flush pose (5.75,0,5.75)"),
		bFoundEdgeFlush);

	// 3. A Free fallback candidate exists at the requested pose.
	const bool bFoundFree = FreeIndex != INDEX_NONE;
	TestTrue(TEXT("a Free fallback candidate exists"), bFoundFree);

	if (bFoundEdgeFlush)
	{
		const FSnapCandidate& Snap = Candidates[EdgeFlushIndex];

		/*
		 * 1 (cont). The flush edge reads back exactly: plank centre X + plank half-X
		 * == brick +X face. Independently derived from the geometry, not from a
		 * production constant.
		 */
		const double PlankPlusXFace = Snap.CentreCm.X + PlacedHalfExtent.X;
		const double BrickPlusXFace = Existing.CentreCm.X + HalfBrick.X;
		TestEqual(
			TEXT("plank +X face is flush with the brick +X face (both 10.75)"),
			PlankPlusXFace, BrickPlusXFace, 1.0e-6);

		/*
		 * 2. Exactly ONE joint, to the existing brick (index 0), whose profile is the
		 * PASSIVE DryStone bearing. The plank at X=5.75 spans [0.75, 10.75], overlapping
		 * only brick 0. The solver must DERIVE the profile via JointForContact(Timber,
		 * ClayBrick, ...) — pinned FULL-FIELD so a mortar substitution fails.
		 */
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

		/*
		 * The offset field carries the ranking key's meaning: the Euclidean distance
		 * from the requested pose (8,0,5) to the snapped pose (5.75,0,5.75) =
		 * sqrt(2.25^2 + 0.75^2) = sqrt(5.625). Derived independently from the distance
		 * formula, not read back from production.
		 */
		TestEqual(TEXT("edge-flush OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, FMath::Sqrt(5.625), 1.0e-6);
	}

	if (bFoundFree)
	{
		// 3 (cont). The Free candidate sits at the requested pose and forms no joint.
		const FSnapCandidate& Free = Candidates[FreeIndex];
		TestTrue(TEXT("Free candidate sits at the requested pose"),
			Free.CentreCm.Equals(Requested, Tol));
		TestEqual(TEXT("Free candidate forms no joints"), Free.Joints.Num(), 0);

		// 3 (cont). Free stays LAST — behind every snap.
		TestEqual(TEXT("Free is the last candidate"),
			FreeIndex, Candidates.Num() - 1);
	}

	// 1 (cont). An in-range snap BEATS free: the edge-flush candidate precedes it.
	if (bFoundEdgeFlush && bFoundFree)
	{
		TestTrue(
			TEXT("TimberEdgeFlush is ranked ahead of Free (appears earlier)"),
			EdgeFlushIndex < FreeIndex);
	}

	/*
	 * 4. Edge-flush wins near an edge. The solver MAY also offer a TimberCentered
	 * candidate (at (0,0,5.75)); that is allowed. But because the requested pose leans
	 * toward the +X edge, the edge-flush pose is nearer than centered, so when both
	 * exist the edge-flush candidate must rank AHEAD of the centered one.
	 */
	if (bFoundEdgeFlush && CenteredIndex != INDEX_NONE)
	{
		TestTrue(
			TEXT("TimberEdgeFlush is ranked ahead of TimberCentered when the cursor is near the edge"),
			EdgeFlushIndex < CenteredIndex);
	}

	return true;
}

/**
 * BEHAVIOR 2e, MERGE BUG DRIVER — brick-WIDTH plank must not carry DUPLICATE bearings.
 *
 * When a timber plank is exactly the brick's X/Y footprint, its TimberCentered pose and
 * its TimberEdgeFlush pose COINCIDE: edge-flush X = BrickXFace - SignX*Placed.ExtentX,
 * and with Placed.ExtentX == Other.ExtentX that collapses to the brick centre — the
 * centred pose. EmitOrMerge keys candidates by POSE and Appends the joint lists of any
 * coincident emission, so the same contact-based bearing set is added TWICE: the merged
 * candidate ends up carrying two DryStone bearings to the SAME OtherPieceIndex, silently
 * doubling that brick's bearing capacity. A wall plate cut to brick width hits this on
 * every brick.
 *
 * Fixture: one ClayBrick at the origin (extent HalfBrick); a TIMBER plank of half-extent
 * (10.75, 5.125, 1.5) — the SAME X/Y footprint as the brick (so centred == edge-flush),
 * thin in Z (full 3.0, so NOT brick-sized: the brick kinds do not fire). Requested (1,0,5)
 * leans +X and above; SignX = +1. Both timber poses land at (0, 0, brick top 3.25 + joint
 * 1.0 + plank half-height 1.5) = (0,0,5.75).
 *
 * Assert on the single timber-kind candidate at (0,0,5.75): it forms EXACTLY ONE bearing
 * joint (to brick 0, full-field DryStone), not two. RED today: it carries two duplicate
 * joints to index 0.
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

	// One existing full brick at the origin.
	const FPieceBox Existing{ FVector(0.0, 0.0, 0.0), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	/*
	 * A TIMBER plank with the SAME X/Y footprint as the brick (half-extent 10.75 x 5.125)
	 * so the centred and edge-flush poses coincide, thin in Z (half 1.5 -> full 3.0, not
	 * brick-sized). Requested off toward +X and above.
	 */
	const FVector PlacedHalfExtent(10.75, 5.125, 1.5);
	const FVector Requested(1.0, 0.0, 5.0);
	const FPieceBox Placed{ Requested, PlacedHalfExtent };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	// The coincident centred/edge-flush pose: brick centre X/Y, brick top + joint + half-height.
	const FVector ExpectedCentre(0.0, 0.0, 5.75);
	const double Tol = KINDA_SMALL_NUMBER;

	/*
	 * Every timber-kind candidate at the coincident pose. Scoped to that pose so the
	 * already-logged brick-kind misnomer for brick-width timber (at OTHER poses) cannot
	 * trip this; here the plank is not brick-sized anyway, so no brick kinds fire.
	 */
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

		/*
		 * The core assertion: ONE bearing, not two. The merge Appends the contact-based
		 * bearing set once per coincident emission; with centred and edge-flush landing on
		 * the same pose the naive merge doubles it. There is only one brick to rest on, so
		 * the correct count is exactly one.
		 */
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
 * BEHAVIOR 2e, WALL-PLATE PIN — a brick-width plate spanning a course keeps its THREE
 * DISTINCT bearings, not duplicates. The companion to the brick-width duplicate driver:
 * it pins that the fix drops only the duplicate, never a real bearing.
 *
 * Fixture: a course of three ClayBricks at X = 0, 22.5, 45 (tops level, centre Z = 0,
 * extent HalfBrick). A TIMBER plate of half-extent (33.75, 5.125, 1.5) — full length
 * 67.5 cm — centred on brick@22.5 spans X in [-11.25, 56.25] and rests on all three.
 * Requested (24, 0, 5) leans toward brick@22.5 and above, so the centred pose is
 * (22.5, 0, brick top 3.25 + joint 1.0 + plate half-height 1.5) = (22.5, 0, 5.75).
 *
 * Assert on the ONE candidate at (22.5,0,5.75): exactly one such candidate, THREE bearing
 * joints, OtherPieceIndex set == {0,1,2} (order-independent, so no index bedded twice),
 * all full-field DryStone. GREEN today (the three poses do not coincide, so no duplicate
 * arises here) — it guards against an over-aggressive dedupe fix that would drop real
 * bearings.
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

	// A course of three bricks the plate spans.
	const FPieceBox Brick0{ FVector(0.0, 0.0, 0.0), HalfBrick };   // idx 0
	const FPieceBox Brick1{ FVector(22.5, 0.0, 0.0), HalfBrick };  // idx 1
	const FPieceBox Brick2{ FVector(45.0, 0.0, 0.0), HalfBrick };  // idx 2
	const TArray<FPieceBox> NearbyBoxes = { Brick0, Brick1, Brick2 };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick, ClayBrick };

	// A TIMBER plate, full length 67.5 cm: centred on brick@22.5 it rests on all three.
	const FVector PlacedHalfExtent(33.75, 5.125, 1.5);
	const FVector Requested(24.0, 0.0, 5.0);
	const FPieceBox Placed{ Requested, PlacedHalfExtent };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, Timber, NearbyBoxes, NearbyMaterials, Settings);

	// Centred on brick@22.5: (22.5, 0, brick top 3.25 + joint 1.0 + plate half-height 1.5).
	const FVector ExpectedCentre(22.5, 0.0, 5.75);
	const double Tol = KINDA_SMALL_NUMBER;

	// Every timber-kind candidate AT the brick@22.5 pose. Expect exactly one.
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

		// It bears on ALL three spanned bricks: three joints, no more (no duplicate).
		TestEqual(TEXT("plate candidate forms three bearing joints (one per spanned brick)"),
			Snap.Joints.Num(), 3);

		if (Snap.Joints.Num() == 3)
		{
			/*
			 * The OtherPieceIndex set is exactly {0, 1, 2}, order-independent — each spanned
			 * brick bedded ONCE, none twice. Three joints found across three distinct indices
			 * proves distinctness with no duplicate.
			 */
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
 * BEHAVIOR 2b, OCCUPANCY FILTER (RED driver). A snap whose placed-piece box would land
 * INSIDE an already-placed piece — a true volume intersection, positive overlap on ALL
 * THREE axes at once — must be DROPPED, however near the cursor it is. This is what stops
 * the solver offering a brick inside a brick.
 *
 * INTERPENETRATION (the drop rule dev must implement), derived independently of any
 * production constant: for the candidate box at CentreCm with the PLACED half-extent, and
 * a nearby box at its own centre/half-extent,
 *     Abs(dCentre.X) < ExtentX_placed + ExtentX_other  AND
 *     Abs(dCentre.Y) < ExtentY_placed + ExtentY_other  AND
 *     Abs(dCentre.Z) < ExtentZ_placed + ExtentZ_other
 * ALL THREE strictly true => the boxes share volume => DROP. A pose that is merely
 * TOUCHING (equal on an axis: the joint-separated snaps) is NOT an interpenetration and
 * must survive — see the companion FSnapSolverContactPoseSurvives test. sumExtent for two
 * full bricks is (21.5, 10.25, 6.5).
 *
 * Fixture: two ClayBricks already placed — brick@(0,0,0) [idx 0] and brick@(11.25,0,7.5)
 * [idx 1], the second sitting running-bond next-course on the first (an occupied cell). A
 * new ClayBrick is requested at (11.25,0,15) — CLEAR of both bricks (dZ to brick 1 is 7.5
 * > sumZ 6.5), directly above the occupied cell. The natural best snap is brick 0's +X
 * next-course pose, (0,0,0)+(11.25,0,7.5) = (11.25,0,7.5) — EXACTLY brick 1's cell, so
 * that candidate's box is coincident with brick 1 (interpenetrates on all three axes) and
 * must be dropped. It is also the NEAREST snap (offset 7.5 < the next survivor's 11.25),
 * so without the filter it would rank first among snaps.
 *
 * Clear survivors genuinely exist: brick 1's own +X next-course pose (22.5,0,15) [gapped in
 * Z], and brick 0's same-course end-to-end pose (22.5,0,0) [gapped in X], neither of which
 * interpenetrates anything. The requested pose is itself clear, so the Free fallback also
 * survives.
 *
 * RED today: the solver has no occupancy filter, so the (11.25,0,7.5) candidate IS
 * returned (and ranks first among snaps).
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

	// Two bricks: brick 0 at the origin, brick 1 one running-bond course up on it.
	const FPieceBox Brick0{ FVector(0.0, 0.0, 0.0), HalfBrick };      // idx 0
	const FPieceBox Brick1{ FVector(11.25, 0.0, 7.5), HalfBrick };    // idx 1 (occupied cell)
	const TArray<FPieceBox> NearbyBoxes = { Brick0, Brick1 };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick };

	/*
	 * Requested directly above brick 1's cell, clear of both bricks (dZ to brick 1 is
	 * 15 - 7.5 = 7.5 > sumZ 6.5). Brick 0's +X next-course pose is exactly brick 1's
	 * cell (11.25,0,7.5) — the occupied pose that must be dropped.
	 */
	const FVector Requested(11.25, 0.0, 15.0);
	const FPieceBox Placed{ Requested, HalfBrick };
	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const FVector OccupiedCell(11.25, 0.0, 7.5);
	const FVector ClearSurvivor(22.5, 0.0, 15.0); // brick 1's +X next-course, gapped in Z.
	const double Tol = KINDA_SMALL_NUMBER;

	/*
	 * 1. THE OCCUPANCY DROP. No returned SNAP candidate may sit at the occupied cell — its box
	 * would be coincident with brick 1 (interpenetration on all three axes). The filter applies
	 * to snap candidates; the Free fallback is honoured verbatim (the "place anywhere" escape),
	 * and survives here only because this fixture's requested pose is itself clear.
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

	/*
	 * 2. THE FILTER DID NOT EMPTY THE LIST. A clear next-course survivor genuinely exists —
	 * brick 1's own +X next-course pose (22.5,0,15), which is gapped from brick 1 in Z (dZ
	 * 7.5 > sumZ 6.5) and so is not an interpenetration. Its box must not overlap any nearby
	 * piece on all three axes; assert it survives and forms its bed joint to brick 1.
	 */
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

	/*
	 * 3. Belt-and-braces: NO returned candidate's box interpenetrates ANY nearby piece.
	 * Derived here independently from the box-overlap definition above so the filter is
	 * pinned by behaviour, not by agreeing with a production helper. (The Free fallback at
	 * the clear requested pose is covered by this too.)
	 */
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
 * BEHAVIOR 2b, CONTACT POSE SURVIVES (regression pin). The occupancy filter must drop only
 * true interpenetrations, never a legitimate joint-separated contact. A next-course snap
 * sits one joint-thickness ABOVE the brick it beds on — its box overlaps that brick in X
 * and Y but is GAPPED in Z (dZ 7.5 > sumZ 6.5), so it is a CONTACT, not an overlap, and
 * must remain offered.
 *
 * Fixture: one ClayBrick at (0,0,0); a ClayBrick requested at (11.0,0,7.5), whose +X
 * next-course pose is (11.25,0,7.5) — one joint above brick 0. This is the exact pose the
 * BEHAVIOR 2a test already pins; here it re-asserts it survives the occupancy filter, so a
 * too-aggressive filter that treated the joint-contact as an overlap would fail.
 *
 * Green today (no filter to over-drop yet) and must stay green after the filter lands.
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

	// Requested just below/beside the next-course pose so the +X stagger is the natural snap.
	const FVector Requested(11.0, 0.0, 7.5);
	const FPieceBox Placed{ Requested, HalfBrick };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	// The next-course pose one joint above brick 0: gapped in Z, so a contact, not an overlap.
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
 * CR-2a, THE CORNER RETURN — behaviour in one sentence: a brick-sized piece laid with
 * its long axis PERPENDICULAR to a brick-sized neighbour's, on the same course, is
 * offered FOUR BrickCornerReturn poses (one at each end of the neighbour x one for each
 * of the neighbour's two width faces the return can finish flush with), each carrying
 * exactly ONE joint to that neighbour whose profile is the quoin's full
 * GeneralPurposeMortar — never the perpend.
 *
 * THE GEOMETRY, DERIVED HERE AND NOT READ BACK FROM PRODUCTION. The neighbour is an
 * X-long brick at (0,0,3.25), half-extent (10.75, 5.125, 3.25), so its END faces are at
 * X = +/-10.75 and its WIDTH faces at Y = +/-5.125. The return is the same brick turned
 * to run along Y, half-extent (5.125, 10.75, 3.25). Abutting the +X end across one 1 cm
 * joint puts the return's near long face at X = 11.75, so its centre is at
 * X = 10.75 + 1 + 5.125 = 16.875. Finishing FLUSH with the neighbour's -Y width face puts
 * the return's own end face at Y = -5.125, so its centre is at Y = -5.125 + 10.75 = 5.625;
 * flush with the +Y face mirrors that to Y = -5.625. The -X end mirrors X. Four poses:
 * (+/-16.875, +/-5.625, 3.25).
 *
 * WHY FLUSH AT ALL, AND WHY IT IS THE ASSERTION THAT MATTERS: flushness is what makes the
 * pair an L rather than a header laid through the wall, and the L is what earns the
 * bonded-quoin mortar (DESIGN §8 2026-09-15 + its KNOWN LIMIT). A return whose end face
 * stuck out past BOTH of the neighbour's width faces would be a closer, and a perpend.
 *
 * THE PROFILE IS THE MIGRATION PIN (CR-2 (C)). The solver forms this joint across a
 * HORIZONTAL (+/-X) interface — exactly the normal the three-argument JointForContact
 * answers GeneralPurposeMortarPerpend for. Only the BOXED overload, which reads the two
 * footprints, returns full mortar here. So a corner candidate carrying the perpend is
 * precisely the "emitted the pose but never migrated the call site" failure, and the
 * full-field comparison is what separates the two (they differ ONLY on cohesion 0.9 vs
 * 0.2 and tension 0.7 vs 0.1).
 *
 * AND THE POSE IS A REAL FACE. Each returned pose is fed back through
 * DestructionLayout::MakeInterface — the same composition Placement.cpp performs — and
 * must yield the brick's 10.25 x 6.5 = 66.625 cm2 END-face area with a horizontal normal.
 * That is what proves the emitted centre is one joint off the neighbour and not merely
 * near it: an interpenetrating or gapped pose forms no face at all, or a different one.
 *
 * NEEDS A TICKING WORLD: NO. Deterministic geometry over boxes; nothing is solved and
 * nothing moves, so no displacement is (or could be) asserted.
 *
 * RED TODAY: IsBrickSized rejects the rotated footprint, so the rotated brick is offered
 * nothing but the Free fallback and no BrickCornerReturn candidate exists.
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

	// One existing X-long brick, resting on the ground (bottom face at Z = 0).
	const FPieceBox Existing{ FVector(0.0, 0.0, 3.25), HalfBrick };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.
	const double Tol = KINDA_SMALL_NUMBER;

	/*
	 * SECTION ONE — ALL FOUR RETURNS. The cursor sits ON the neighbour's centre, which is
	 * equidistant (17.7878 cm) from all four corner poses and inside the 30 cm radius, so
	 * every one of them is in range at once. Their relative ORDER is therefore a tie and is
	 * deliberately not asserted here; section three pins ranking with a cursor that breaks it.
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

	// Distance from the cursor to every one of them: sqrt(16.875^2 + 5.625^2).
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

		// 1. Exactly one candidate per pose — four distinct returns, none duplicated.
		TestEqual(*(Prefix + TEXT("exactly one BrickCornerReturn candidate exists")),
			AtPose.Num(), 1);
		if (AtPose.Num() != 1)
		{
			continue;
		}

		const FSnapCandidate& Snap = Candidates[AtPose[0]];

		/*
		 * 2. ONE joint, to the neighbour (index 0), carrying the QUOIN's full mortar. The
		 * full-field comparison is the whole point: mortar and perpend are separated only
		 * by cohesion and tension, so a partial check would accept the unmigrated answer.
		 */
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

		/*
		 * 3. THE POSE IS A REAL QUOIN FACE. Composed the way Placement.cpp composes a
		 * joint, the returned centre must form an interface with the neighbour whose area
		 * is the brick's END face — the 10.25 cm width overlap by the 6.5 cm course
		 * overlap = 66.625 cm2 — across a HORIZONTAL normal. Derived from the brick's own
		 * dimensions, not from any production constant.
		 */
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

	/*
	 * 4. A ROTATED BRICK TAKES NO BED AND NO HEAD FROM A CROSSED NEIGHBOUR. Running bond
	 * is a property of pieces laid the SAME way: a brick turned across the wall does not
	 * bed half a brick along its neighbour's length, and it does not make a head joint
	 * with it. The only thing this pairing offers is the corner return.
	 */
	TestFalse(
		TEXT("no BrickNextCourse/BrickSameCourse candidate for a rotated brick beside an X-long one"),
		bAnyBedOrHeadKind);

	// 5. The Free fallback is still there, at the requested pose, last, forming nothing.
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
	 * SECTION TWO — RANKING IS BY DISTANCE, as for every other kind. A cursor at
	 * (16, 5, 3.25) is 1.0753 cm from the +X/-Y-flush return and 11.1 cm from the
	 * +X/+Y-flush one, and the two -X returns are 33 cm away — outside the radius. So the
	 * nearest corner return must be the FIRST candidate returned.
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
 * CR-2a, THE Y-RUN GRID — behaviour in one sentence: running bond steps along the
 * NEIGHBOUR'S LONG AXIS, so a Y-long brick beside a Y-long neighbour is offered its
 * same-course pose at N +/- (0, 22.5, 0) and its next-course pose at N + (0, +/-11.25,
 * 7.5), and never the X-stepped poses an X-long wall would take.
 *
 * WHY IT MATTERS: today the grid is X-only (SnapSolver.cpp steps HalfStaggerX and
 * SameCoursePitchX off the X axis unconditionally), so a wall running along Y cannot
 * grow at all — which is half of what a corner is for. The corner return lays the first
 * brick of the second leg; this is what lets the leg continue.
 *
 * THE NUMBERS ARE THE SAME COORDINATING GRID, MIRRORED, and are derived here rather than
 * read back: brick length 21.5 + one 1 cm head joint = the 22.5 cm same-course pitch, half
 * of it (11.25) is the running-bond stagger, and brick height 6.5 + one 1 cm bed joint =
 * the 7.5 cm course rise. For a Y-long neighbour those first two run along Y.
 *
 * THE JOINTS ARE THE ORDINARY ONES: the next-course pose beds onto the neighbour across a
 * VERTICAL normal (full GeneralPurposeMortar) and the same-course pose abuts it end to end
 * across a HORIZONTAL one between two pieces whose long axes AGREE — a head joint, so the
 * weak GeneralPurposeMortarPerpend. Neither is a corner, which is exactly why this test is
 * separate from the corner-return one: it pins that widening the brick-sized gate did not
 * turn every Y-long pairing into a quoin.
 *
 * NEEDS A TICKING WORLD: NO.
 *
 * RED TODAY: IsBrickSized rejects both rotated footprints, so the solver offers nothing
 * but Free.
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

	// One existing Y-long brick on the ground: the first brick of a wall running along Y.
	const FPieceBox Existing{ FVector(0.0, 0.0, 3.25), HalfBrickRotated };
	const TArray<FPieceBox> NearbyBoxes = { Existing };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick };

	/*
	 * The cursor leans +Y and one course up, so the +Y stagger is the nearest pose. Both
	 * the next-course pose (offset 0.25) and the same-course pose (offset 13.7295) are
	 * inside the 30 cm radius from here, so one solve exercises both.
	 */
	const FVector Requested(0.0, 11.0, 10.75);
	const FPieceBox Placed{ Requested, HalfBrickRotated };
	const FSnapSettings Settings;

	const TArray<FSnapCandidate> Candidates = SolveSnapCandidates(
		Placed, ClayBrick, NearbyBoxes, NearbyMaterials, Settings);

	const FVector ExpectedNextCourse(0.0, 11.25, 10.75);
	const FVector ExpectedSameCourse(0.0, 22.5, 3.25);
	const double Tol = KINDA_SMALL_NUMBER;

	/*
	 * The X-STEPPED poses an X-only grid produces for this neighbour. They are physically
	 * wrong for a Y-long pair — (11.25, 0, 10.75) would bed a brick half over open air
	 * across the wall's thickness — so their ABSENCE is asserted, which is what tells a
	 * long-axis-aware grid from a widened brick-sized gate alone.
	 */
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

	// 1. The next-course pose steps HALF A BRICK ALONG Y and one course up.
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

	// 2. The same-course pose steps ONE WHOLE PITCH ALONG Y, at the same height.
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
			/*
			 * TWO PIECES RUNNING THE SAME WAY MAKE A HEAD JOINT, NOT A CORNER — the weak
			 * perpend. Pinned full-field, so a boxed-inference call that read the pairing as
			 * crossed (and handed it the quoin's mortar, 4.5x the bond) fails here.
			 */
			CheckProfileIdentity(
				*this,
				TEXT("Y-run head joint profile == GeneralPurposeMortarPerpend: "),
				Snap.Joints[0].Profile,
				GeneralPurposeMortarPerpend);
		}
		TestEqual(TEXT("Y-run same-course OffsetFromRequestedCm is the distance to the snap"),
			Snap.OffsetFromRequestedCm, FMath::Sqrt(11.5 * 11.5 + 7.5 * 7.5), 1.0e-6);
	}

	// 3. No X-stepped pose survives for a Y-long neighbour.
	TestFalse(
		TEXT("no candidate at the X-stepped poses (11.25,0,10.75) / (22.5,0,3.25)"),
		bAnyXSteppedPose);

	// 4. Free is still the last resort.
	TestTrue(TEXT("a Free fallback candidate exists"), FreeIndex != INDEX_NONE);
	if (FreeIndex != INDEX_NONE)
	{
		TestEqual(TEXT("Free is the last candidate"), FreeIndex, Candidates.Num() - 1);
	}

	return true;
}

/**
 * CR-2a, THE MIRROR PROPERTY — behaviour in one sentence: a wall running along Y grows
 * exactly as a wall running along X does, reflected through the plane x = y.
 *
 * WHY A PROPERTY AND NOT MORE ROWS. "Steps along the neighbour's long axis" is a
 * TOPOLOGICAL claim about the whole candidate set, and a hand-written row table only ever
 * covers the poses somebody thought of — an implementation that emitted the right Y poses
 * but kept a stray X one, dropped a candidate, re-ordered the ranking or changed a profile
 * would pass a row table aimed at the two poses. So the ORACLE here is the X-run answer
 * itself, mapped through the reflection: the two solves are run independently and every
 * candidate is compared index for index, which pins count, order, kind, pose, offset and
 * every formed joint at once.
 *
 * THE ORACLE IS INDEPENDENTLY DERIVED in the only sense that matters: it is not a
 * re-implementation of the grid arithmetic (which would agree with a wrong solver), it is
 * a SYMMETRY the physics has and the code currently does not. Reflecting a brick wall
 * through x = y gives a brick wall; nothing in the coordinating grid distinguishes the two
 * axes. The settings object is deliberately left unmirrored — FSnapSettings names the
 * brick's LENGTH (21.5) and its WIDTH (10.25), which are properties of the brick, not of
 * the world axes, so a solver that reads the pitch off BrickSizeCm.X may keep doing so as
 * long as it applies it along the NEIGHBOUR'S long axis.
 *
 * NO TIES TO BREAK in either row: each cursor leans clear of one pose, so both
 * configurations rank their candidates strictly and an index-for-index comparison is
 * meaningful.
 *
 * TWO ROWS, BECAUSE THE SYMMETRY HAS TWO HALVES. The COLLINEAR row reflects a
 * running-bond pair (bed + head); the CROSSED row reflects a corner return, where the
 * placed brick runs across its neighbour — the reflection maps an X-long wall with a
 * Y-long return onto a Y-long wall with an X-long return, so the four corner poses must
 * come back reflected, in the same ranked order, carrying the same quoin mortar. Adding a
 * third arrangement should be a row here, never a new test.
 *
 * NEEDS A TICKING WORLD: NO.
 *
 * GREEN ON ARRIVAL for the crossed row: CR-2a landed both the long-axis-aware grid and the
 * corner return, so this row pins behaviour that already exists rather than driving any. It
 * is the review's non-blocking ask, and it earns its runtime by making the reflection
 * property cover the corner kind too — the mutation that proves it bites is in the report.
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
	 * One arrangement, stated in the X frame. The Y frame is derived from it by reflection
	 * and never written out, which is the point: a transcribed "expected Y answer" would be
	 * a second hand-written table, not a symmetry.
	 *
	 * ExpectedXCandidates is the VACUITY GUARD — if both solves collapsed to the Free
	 * fallback alone the mirror would hold trivially, so the X frame's own candidate count
	 * is pinned before the two are compared.
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
		/*
		 * COLLINEAR: an X-long brick beside an X-long neighbour, the cursor leaning +X and
		 * one course up. Two snaps (next-course, same-course) plus Free.
		 */
		{ TEXT("collinear stretchers"),
			FVector(0.0, 0.0, 3.25), HalfBrick, HalfBrick,
			FVector(11.0, 0.0, 10.75), 3 },

		/*
		 * CROSSED: the same neighbour, but the placed brick turned to run along Y — a corner
		 * return. The cursor at (16, 5, 3.25) is 1.075 cm from the +X-end / -Y-flush return at
		 * (16.875, 5.625, 3.25) and 10.66 cm from the +X-end / +Y-flush one at
		 * (16.875, -5.625, 3.25); the two -X-end returns are 32.9 and 34.6 cm away, outside
		 * the 30 cm radius. So two corner snaps plus Free, strictly ranked — and under the
		 * reflection the cursor becomes (5, 16, 3.25) against a Y-long neighbour, which is the
		 * same corner seen from the other leg.
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

		// The Y frame: the same fixture reflected through x = y, piece by piece.
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
 * CR-2a REVIEW FINDING B2, THE CROSSED BED — behaviour in one sentence: a brick's
 * NextCourse candidate forms a BED joint to EVERY brick-sized neighbour whose top face it
 * rests on, whatever way that neighbour runs.
 *
 * WHY THIS IS THE ONE THAT BONDS A CORNER. A bricklayer's quoin is not one joint; it is
 * the alternate-course LAP. The course-1 stretcher over an L's corner sits half on the leg
 * it continues and half on the RETURN laid across it, and that second bed is what makes
 * the return carry load from above. Today the bed/head branch is gated on
 * bSameOrientation, so a crossed neighbour under the placed brick contributes nothing at
 * all: the quoin is held by its vertical joint alone, and a vertical joint routes no
 * vertical load (SolveLoads sends weight down beds). The mechanism already exists in the
 * file — BearingsAtPose is the contact sweep a timber lintel uses to find every support it
 * spans — it is simply not applied to the brick bed.
 *
 * THE FIXTURE IS THE REVIEWER'S OWN, REDUCED TO TWO NEIGHBOURS. Numbers derived here from
 * the brick (21.5 x 10.25 x 6.5 on 1 cm joints), not read back from production:
 *
 *   piece 0, X-long  at (45.000, 0.000, 3.25) half (10.75, 5.125, 3.25)
 *            spans X [34.25, 55.75]  Y [-5.125,  5.125]  Z [0, 6.5]
 *   piece 1, Y-long  at (61.875, 5.625, 3.25) half ( 5.125, 10.75, 3.25)
 *            spans X [56.75, 67.00]  Y [-5.125, 16.375]  Z [0, 6.5]
 *   placed,  X-long  at (56.250, 0.000,10.75) half (10.75, 5.125, 3.25)
 *            spans X [45.50, 67.00]  Y [-5.125,  5.125]  Z [7.5, 14.0]
 *
 * The placed brick's underside is at Z = 7.5, one 1 cm bed joint above both neighbours'
 * tops at Z = 6.5 — so it RESTS ON BOTH. Against piece 0 the footprints overlap
 * [45.5, 55.75] x [-5.125, 5.125] = 10.25 x 10.25; against piece 1 they overlap
 * [56.75, 67] x [-5.125, 5.125] = 10.25 x 10.25. Both faces are therefore
 * 10.25 x 10.25 = 105.0625 cm2 — a full brick width square, which is exactly what a lap
 * over a return is.
 *
 * WHY THE POSE IS ASSERTED AND THE KIND IS ONLY ASSERTED NOT-FREE. The candidate at
 * (56.25, 0, 10.75) is a MERGED one (piece 0's next-course pose; once the gate widens,
 * piece 1's bed joins it), and CURRENT_STATE's snap-solver item (vii) records that a
 * merged candidate's single Kind label is order-dependent and due to be replaced by a
 * truthful multi-kind label. Pinning the exact enumerator would make this test fight that
 * rework for no gain, so the geometry is pinned instead: the exact centre, one course
 * (7.5 cm) above the neighbours, which no other candidate in the set shares.
 *
 * THE ASSERTION IS THE MECHANISM, NOT A DISPLACEMENT — a connection that exists, names the
 * right neighbour, carries the right profile and composes to the right face. Nothing here
 * is solved and nothing moves.
 *
 * NEEDS A TICKING WORLD: NO.
 *
 * RED TODAY: the best candidate carries ONE joint (to piece 0). The bed onto the crossed
 * return is missing entirely.
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

	// The X leg's end brick, and the rotated return laid across it.
	const FPieceBox LegEnd{ FVector(45.0, 0.0, 3.25), HalfBrick };
	const FPieceBox Return{ FVector(61.875, 5.625, 3.25), HalfBrickRotated };
	const TArray<FPieceBox> NearbyBoxes = { LegEnd, Return };
	const TArray<FMaterialProfile> NearbyMaterials = { ClayBrick, ClayBrick };

	// The course-1 corner brick, asked for exactly at its running-bond pose.
	const FVector Requested(56.25, 0.0, 10.75);
	const FPieceBox Placed{ Requested, HalfBrick };

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.
	const double Tol = KINDA_SMALL_NUMBER;

	/*
	 * The lap area, spelled out from the brick rather than imported: the placed brick and
	 * each neighbour share a full brick width in both in-plane axes.
	 */
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

	/*
	 * 1. THE BEST CANDIDATE IS THE COURSE-UP POSE. The cursor sits exactly on it, so its
	 * offset is zero and nothing else in the set can outrank it. Z = 10.75 is one course
	 * (brick 6.5 + bed joint 1.0) above the neighbours' 3.25.
	 */
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

	/*
	 * 2. TWO JOINTS — one to each neighbour it rests on. Looked up BY NEIGHBOUR INDEX
	 * rather than by position, since the order joints are appended in is an implementation
	 * detail the behaviour does not depend on.
	 */
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

		/*
		 * 3. A BED IS A BED WHICHEVER WAY THE NEIGHBOUR RUNS. The contact normal is
		 * vertical, so the boxed inference classifies Bed before orientation is ever
		 * consulted — full GeneralPurposeMortar, never the crossed pair's quoin reasoning
		 * and never the perpend. Full-field, because mortar and perpend differ only on
		 * cohesion and tension.
		 */
		CheckProfileIdentity(
			*this,
			Prefix + TEXT("profile == GeneralPurposeMortar: "),
			Joint->Profile,
			GeneralPurposeMortar);

		/*
		 * 4. THE JOINT IS A REAL FACE, composed the way Placement.cpp composes one: a
		 * 10.25 x 10.25 = 105.0625 cm2 horizontal lap across a VERTICAL normal. The normal
		 * is what makes the area mean something — a face this size could only otherwise be
		 * a coincidence of two side overlaps.
		 */
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
