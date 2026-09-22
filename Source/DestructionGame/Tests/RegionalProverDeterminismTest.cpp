// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Regional prover slice 5 (REGIONAL_PROVER_PLAN.md): the felled set must not depend on the order
 * pieces, joints or seeds are supplied. A characterisation guard, green on arrival, against a future
 * hash-order or index dependency in the flood.
 *
 * Both caps (15, 25) are below the 30-course stack so admission stops mid-structure; with a full
 * cap invariance would be trivial. A cap >= 30 is omitted because the LP's own phase-1 verdict is
 * not fully permutation-robust at that size (it declines, failing closed; see
 * OracleMechanismMultiModeDeterminism).
 *
 * Permutes construction order (compared by course number, not index) and seed order. Asserts the
 * Falling course set, released count and zero stranded, never displacement (DESIGN.md §4). No world.
 */
namespace RegionalProverDeterminismSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// The 30-course mortared leaning stack from LeaningStackAcceptanceTest (Falls rung).

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double ClayDensityGramsPerCubicCm = 1.9;
	constexpr double OffsetPerCourseCm = 10.0;
	constexpr double BedJointThicknessCm = 1.0;
	constexpr double CoursePitchCm = BrickHeightCm + BedJointThicknessCm;

	/** Density first, per the PieceMassKg contract; 2.72163125 kg. */
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
	 * Lay the stack with CourseAtIndex[i] at piece index i; connection order follows. Fills
	 * OutIndexOfCourse so results can be read by course.
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

	/** Course c at index c. */
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

	/** Seeded Fisher-Yates shuffle of {0..N-1}. */
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

	/** One prove's outcome, by course number. */
	struct FFelled
	{
		TArray<int32> Courses;   // sorted course numbers reading Falling
		int32 Released = 0;
		int32 Stranded = 0;
	};

	/** Build in the given order, seed by course, run the prover, and read the felled courses back. */
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

/** Felled courses, released count and zero stranded are identical across permutations, at both caps. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverDeterminismTest,
	"DestructionGame.Core.Structure.RegionalProver.FelledSetIsInvariantUnderInputPermutation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverDeterminismSupport;

	// Three top courses, so seed order is a real permutation.
	const TArray<int32> SeedCourses = { 29, 28, 27 };

	struct FArm
	{
		int32 Cap;
		const TCHAR* Label;
	};

	const FArm Arms[] = {
		// Fells the top 14 courses {16..29}.
		{ 15, TEXT("A/biting-cap-15") },
		// Fells the top 24 courses {6..29}.
		{ 25, TEXT("B/biting-cap-25") },
	};

	constexpr int32 NumPermutations = 40;

	for (const FArm& Arm : Arms)
	{
		// Base run: identity order.
		const FFelled Base = ProveAndReadFelled(IdentityOrder(), SeedCourses, Arm.Cap);

		AddInfo(FString::Printf(
			TEXT("%s: base felled {%s} (n=%d), released %d, stranded %d"),
			Arm.Label, *JoinCourses(Base.Courses), Base.Courses.Num(), Base.Released, Base.Stranded));

		// A non-empty fall, or invariance is vacuous.
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

		// The cap must bite: fewer than all courses above the base fall.
		TestTrue(
			*FString::Printf(
				TEXT("%s: the cap BITES — the felled subset (%d) is strictly smaller than the whole "
					 "stack above the base (%d), so admission order is genuinely load-bearing"),
				Arm.Label, Base.Courses.Num(), Courses - 1),
			Base.Courses.Num() < Courses - 1);

		// Permute both orders; the seed is printed on divergence for reproduction.
		for (int32 Perm = 0; Perm < NumPermutations; ++Perm)
		{
			const int32 PermSeed = 0x5A5A0000 + Perm;

			const TArray<int32> ConstructionOrder = SeededShuffle(PermSeed, Courses);

			// Decorrelated stream for the seed order.
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
