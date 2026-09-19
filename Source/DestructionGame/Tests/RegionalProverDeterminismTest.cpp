// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Slice 5 of the regional collapse prover (REGIONAL_PROVER_PLAN.md slice 5, review item 12) — the
 * determinism guard, green on arrival. The prover's felled set must be invariant under input-order
 * permutation: permuting the order pieces and joints are added to the structure, and the order the
 * seed is supplied, must never change which pieces the prover fells. This is a characterisation
 * net, not a driver — review-expert observed the committed flood uses fixed insertion-order loops
 * (a TArray Frontier walked head-to-tail, PieceJoints in ascending connection index, the Seed
 * TArray in array order), so it is deterministic per build and green the day it lands. Its value is
 * locking that property against a future regression: a hash-order or raw-index dependency slipping
 * into the flood when grow-on-contact (deferred slice 3) lands or the region cap is raised.
 *
 * WHY THE CAP MUST BITE. When RegionBlockCap >= the live block count the flood admits everything
 * and the region is the whole structure regardless of order, making invariance trivial. So both
 * arms pose a cap smaller than the 30-course stack (15 and 25): the flood stops mid-structure and
 * admits a strict subset (the top 14 / top 24 courses), so the admission loop genuinely runs
 * against its budget. The felled set stays invariant because, on this linear chain seeded at the
 * free top, the region reachable within the cap is forced by connectivity (the only live neighbours
 * of the top course are lower, so it can only be the contiguous top sub-stack) — not by admission
 * order. That is the property under guard: the felled set follows from geometry, never from order.
 *
 * WHY NOT A FULL-CAP ARM. Measured while writing this guard: with the cap >= 30 the region is the
 * whole 30-block stack, and that exposes a different, pre-existing effect — the LP's own phase-1
 * verdict is not fully permutation-robust at that size (some column orderings decline rather than
 * certify Falls, so the prover fails closed with 0 released rather than a false collapse). That is
 * the documented characteristic OracleMechanismMultiModeDeterminism pins ("the verdict is not fully
 * permutation-robust even though naming is; a decline is fail-closed") — a property of the shared
 * solver core, not this slice's flood. The biting-cap regions (<= 28 blocks) sit below where it
 * bites — measured 0 divergences across many hundreds of permutations — so both arms stay a clean,
 * strict-invariance guard on the flood, which is what slice 5 owns.
 *
 * TWO PERMUTATION AXES, both order-sensitive inputs to the flood:
 *   - CONSTRUCTION ORDER. The stack is rebuilt with its 30 courses added in a seeded-shuffled order,
 *     relabelling piece and connection indices. This drives PieceJoints (built in ascending
 *     connection index) to list each piece's neighbours in a different order, and the felled set is
 *     read back and compared as course identities (physical course numbers) rather than indices.
 *   - SEED ARRAY ORDER. The multi-course seed is itself shuffled, exercising the admission loop.
 *
 * Asserts on mechanism, never displacement (DESIGN.md §4): the set of course numbers reading
 * Falling, the released count, and zero stranded, in both the base and every permuted run.
 *
 * The felled set is compared between two runs of the same production entry
 * (SolveAndBreak_WithRegionalProver) on physically-identical structures built in different orders,
 * so the invariant is a property of production code against its own inputs and needs no external
 * oracle. Units derived here, never imported.
 *
 * Needs no ticking world: pure FStructure construction and state queries feeding the prover's own
 * FOracleProblem solves. Named namespace: a unity build merges many files into one translation unit.
 */
namespace RegionalProverDeterminismSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * The fixture: the 30-course, 10 cm/course mortared leaning stack (the Falls rung of
	 * LeaningStackAcceptanceTest, and the geometry RegionalProverGroundedCutTest cuts). Units in
	 * centimetres at Unreal's default 1 uu = 1 cm; mass and density are published values unconverted.
	 */

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double ClayDensityGramsPerCubicCm = 1.9;
	constexpr double OffsetPerCourseCm = 10.0;
	constexpr double BedJointThicknessCm = 1.0;
	constexpr double CoursePitchCm = BrickHeightCm + BedJointThicknessCm;

	/** Density-first multiplication order — the PieceMassKg contract; 2.72163125 kg. */
	constexpr double BrickMassKg =
		ClayDensityGramsPerCubicCm * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	constexpr int32 Courses = 30;

	FPieceBox CourseBox(int32 Course)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
		Box.CentreCm = FVector(
			double(Course) * OffsetPerCourseCm,
			0.0,
			BrickHeightCm / 2.0 + double(Course) * CoursePitchCm);
		return Box;
	}

	/**
	 * Lay the 30-course stack, adding pieces in the order CourseAtIndex gives: CourseAtIndex[i] is the
	 * course number laid at piece index i. Connections are then formed by a nested loop over piece
	 * indices, so the CONNECTION order is relabelled by the same construction permutation. Fills
	 * OutIndexOfCourse[course] = the piece index that course landed at, so the caller can read felled
	 * state back by physical course identity.
	 */
	void LayStack(FStructure& S, const TArray<int32>& CourseAtIndex, TArray<int32>& OutIndexOfCourse)
	{
		OutIndexOfCourse.SetNum(Courses);

		for (int32 Index = 0; Index < CourseAtIndex.Num(); ++Index)
		{
			const int32 Course = CourseAtIndex[Index];
			const FPieceBox Box = CourseBox(Course);
			const int32 Piece = S.AddPiece(BrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			OutIndexOfCourse[Course] = Piece;
		}

		for (int32 A = 0; A < CourseAtIndex.Num(); ++A)
		{
			for (int32 B = A + 1; B < CourseAtIndex.Num(); ++B)
			{
				FConnection Joint;
				if (MakeInterface(
						A, CourseBox(CourseAtIndex[A]), B, CourseBox(CourseAtIndex[B]),
						BedJointThicknessCm, GeneralPurposeMortar, Joint))
				{
					S.AddConnection(Joint);
				}
			}
		}
	}

	/** The identity construction order: course c laid at index c. */
	TArray<int32> IdentityOrder()
	{
		TArray<int32> Order;
		Order.SetNumUninitialized(Courses);
		for (int32 I = 0; I < Courses; ++I)
		{
			Order[I] = I;
		}
		return Order;
	}

	/** A seeded Fisher-Yates shuffle of {0..N-1} — fully deterministic per seed. */
	TArray<int32> SeededShuffle(int32 Seed, int32 N)
	{
		TArray<int32> V;
		V.SetNumUninitialized(N);
		for (int32 I = 0; I < N; ++I)
		{
			V[I] = I;
		}
		FRandomStream Rng(Seed);
		for (int32 I = N - 1; I > 0; --I)
		{
			Swap(V[I], V[Rng.RandRange(0, I)]);
		}
		return V;
	}

	/** The felled outcome of one prove: the set of felled COURSE numbers, the released count, stranded. */
	struct FFelled
	{
		TArray<int32> Courses;   // sorted course numbers reading Falling
		int32 Released = 0;
		int32 Stranded = 0;
	};

	/**
	 * Build the stack in the given construction order, seed the prover with the given COURSE seed (in
	 * the given array order, mapped to that build's piece indices), run the isolated prover entry, and
	 * read the felled set back as course identities. RegionBlockCap is passed straight through.
	 */
	FFelled ProveAndReadFelled(
		const TArray<int32>& ConstructionOrder, const TArray<int32>& SeedCourses, int32 Cap)
	{
		FStructure S;
		TArray<int32> IndexOfCourse;
		LayStack(S, ConstructionOrder, IndexOfCourse);

		TArray<int32> Seed;
		Seed.Reserve(SeedCourses.Num());
		for (const int32 Course : SeedCourses)
		{
			Seed.Add(IndexOfCourse[Course]);
		}

		FFelled Out;
		Out.Released = S.SolveAndBreak_WithRegionalProver(Seed, Cap);

		for (int32 Course = 0; Course < Courses; ++Course)
		{
			const int32 Piece = IndexOfCourse[Course];
			if (S.IsPieceRemoved(Piece))
			{
				continue;
			}
			const EPieceSupport Support = S.GetPieceSupport(Piece);
			if (Support == EPieceSupport::Falling)
			{
				Out.Courses.Add(Course);
			}
			else if (Support == EPieceSupport::Stranded)
			{
				++Out.Stranded;
			}
		}
		Out.Courses.Sort();
		return Out;
	}

	FString JoinCourses(const TArray<int32>& V)
	{
		FString Out;
		for (int32 I = 0; I < V.Num(); ++I)
		{
			Out += FString::Printf(TEXT("%d%s"), V[I], I + 1 < V.Num() ? TEXT(",") : TEXT(""));
		}
		return Out;
	}
}

/**
 * The felled set, expressed as physical course identities, is identical across every seeded
 * permutation of construction order and seed order, on two biting caps that each admit a strict
 * subset of the stack. The released count matches and nothing is stranded, in the base and every
 * permuted run. Needs no ticking world; see the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverDeterminismTest,
	"DestructionGame.Core.Structure.RegionalProver.FelledSetIsInvariantUnderInputPermutation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverDeterminismSupport;

	/* The seed is the free top of the stack — three adjacent top courses, so the SEED-ARRAY order is
	 * a genuine multi-element permutation (not a vacuous single element). */
	const TArray<int32> SeedCourses = { 29, 28, 27 };

	struct FArm
	{
		int32 Cap;
		const TCHAR* Label;
	};

	const FArm Arms[] = {
		/* Arm A — a small biting region: cap 15 on a 30-course stack stops the flood mid-stack and
		 * admits only the top sub-stack, felling the top 14 courses {16..29}. */
		{ 15, TEXT("A/biting-cap-15") },
		/* Arm B — a larger biting region: cap 25 fells the top 24 courses {6..29}, still a strict
		 * subset of the stack, so the admission loop still runs against its budget. */
		{ 25, TEXT("B/biting-cap-25") },
	};

	constexpr int32 NumPermutations = 40;

	for (const FArm& Arm : Arms)
	{
		/* The base: identity construction, seed in supplied order. Everything else compares to it. */
		const FFelled Base = ProveAndReadFelled(IdentityOrder(), SeedCourses, Arm.Cap);

		AddInfo(FString::Printf(
			TEXT("%s: base felled {%s} (n=%d), released %d, stranded %d"),
			Arm.Label, *JoinCourses(Base.Courses), Base.Courses.Num(), Base.Released, Base.Stranded));

		/* The fixture must actually exercise the prover: a non-empty fall, else invariance is vacuous. */
		TestTrue(
			*FString::Printf(TEXT("%s: the base prove fells a NON-EMPTY set (else the guard is vacuous)"),
				Arm.Label),
			Base.Courses.Num() > 0);
		TestEqual(
			*FString::Printf(TEXT("%s: the base released count equals the felled count"), Arm.Label),
			Base.Released, Base.Courses.Num());
		TestEqual(
			*FString::Printf(TEXT("%s: nothing is stranded in the base — a decline is not a collapse"),
				Arm.Label),
			Base.Stranded, 0);

		/* The cap genuinely BITES: strictly fewer than every-course-above-the-base falls, so the
		 * admission loop ran against its budget rather than admitting the whole structure. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: the cap BITES — the felled subset (%d) is strictly smaller than the whole "
					 "stack above the base (%d), so admission order is genuinely load-bearing"),
				Arm.Label, Base.Courses.Num(), Courses - 1),
			Base.Courses.Num() < Courses - 1);

		/*
		 * The sweep: permute both construction order and seed order, and demand the felled course
		 * set, released count and stranded count match the base every time. The permutation seed is
		 * printed on any divergence so a failing case can be reproduced and promoted.
		 */
		for (int32 Perm = 0; Perm < NumPermutations; ++Perm)
		{
			const int32 PermSeed = 0x5A5A0000 + Perm;

			const TArray<int32> ConstructionOrder = SeededShuffle(PermSeed, Courses);

			/* Shuffle the seed array itself with a decorrelated stream. */
			TArray<int32> SeedOrder = SeedCourses;
			{
				FRandomStream Rng(PermSeed ^ 0x0F0F0F0F);
				for (int32 I = SeedOrder.Num() - 1; I > 0; --I)
				{
					Swap(SeedOrder[I], SeedOrder[Rng.RandRange(0, I)]);
				}
			}

			const FFelled Got = ProveAndReadFelled(ConstructionOrder, SeedOrder, Arm.Cap);

			const bool bFelledSame = Got.Courses == Base.Courses;
			const bool bReleasedSame = Got.Released == Base.Released;

			if (!bFelledSame || !bReleasedSame || Got.Stranded != 0)
			{
				AddError(FString::Printf(
					TEXT("%s: DIVERGENCE at permutation seed 0x%08X (seedOrder [%s]): felled {%s} (n=%d) "
						 "vs base {%s} (n=%d); released %d vs %d; stranded %d. Promote this seed to a "
						 "named regression case."),
					Arm.Label, PermSeed, *JoinCourses(SeedOrder),
					*JoinCourses(Got.Courses), Got.Courses.Num(),
					*JoinCourses(Base.Courses), Base.Courses.Num(),
					Got.Released, Base.Released, Got.Stranded));
			}

			TestTrue(
				*FString::Printf(
					TEXT("%s: felled course-set is permutation-invariant (perm seed 0x%08X)"),
					Arm.Label, PermSeed),
				bFelledSame);
			TestTrue(
				*FString::Printf(
					TEXT("%s: released count is permutation-invariant (perm seed 0x%08X)"),
					Arm.Label, PermSeed),
				bReleasedSame);
			TestEqual(
				*FString::Printf(
					TEXT("%s: nothing stranded under permutation (perm seed 0x%08X)"),
					Arm.Label, PermSeed),
				Got.Stranded, 0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
