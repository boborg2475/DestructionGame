// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Regional prover cap invariant (REGIONAL_PROVER_PLAN.md §1, review item 12). A guard, green on
 * arrival; mutating AdmitToRegion's prospective-boundary arithmetic turns it red.
 *
 * The flood bounds region R plus grounded boundary ring B at RegionBlockCap, a per-action block
 * budget (D8, DESIGN §8). Across a sweep of seeds and caps: (1) any posed count is in [1, cap];
 * (2) it equals a reference flood that uses the same BFS admission order but recomputes the
 * boundary from scratch each time, where production maintains it incrementally. An off-by-one in
 * the incremental arithmetic shows up as a mismatch even when the result still fits the cap.
 *
 * Fixture: the 30-course leaning stack, a linear chain, so the count is a clean function of seed
 * and cap. Asserts on block counts only. No ticking world. Named namespace for unity builds.
 */
namespace RegionalProverCapInvariantSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double ClayDensityGramsPerCubicCm = 1.9;
	constexpr double OffsetPerCourseCm = 10.0;
	constexpr double BedJointThicknessCm = 1.0;
	constexpr double CoursePitchCm = BrickHeightCm + BedJointThicknessCm;

	/** Density first, matching PieceMassKg's order; 2.72163125 kg. */
	constexpr double BrickMassKg =
		ClayDensityGramsPerCubicCm * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	constexpr int32 Courses = 30;

	struct FStack
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
	};

	/** Course i centred at (i*10, 0, 3.25 + i*7.5); course 0 grounded, each course bedded below. */
	void LayStack(FStack& OutStack)
	{
		for (int32 Course = 0; Course < Courses; ++Course)
		{
			FPieceBox Box;
			Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				double(Course) * OffsetPerCourseCm,
				0.0,
				BrickHeightCm / 2.0 + double(Course) * CoursePitchCm);

			OutStack.Structure.AddPiece(BrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			OutStack.Boxes.Add(Box);
		}

		for (int32 First = 0; First < OutStack.Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < OutStack.Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(
						First, OutStack.Boxes[First], Second, OutStack.Boxes[Second],
						BedJointThicknessCm, GeneralPurposeMortar, Joint))
				{
					OutStack.Structure.AddConnection(Joint);
				}
			}
		}
	}

	/**
	 * Reference flood: same BFS order as production, but the boundary is recomputed from scratch at
	 * each admit. Returns |R| + |B|, or 0 if nothing is admitted. Run on a pristine structure.
	 */
	int32 ReferenceUnionCount(const FStructure& S, const TArray<int32>& Seed, int32 Cap)
	{
		const int32 N = S.NumPieces();

		// Undirected adjacency over intact connections.
		TArray<TArray<int32>> Adjacency;
		Adjacency.SetNum(N);

		for (int32 Index = 0; Index < S.NumConnections(); ++Index)
		{
			const FConnection& Connection = S.GetConnection(Index);

			if (Connection.HasGiven())
			{
				continue;
			}

			const int32 A = Connection.PieceA;
			const int32 B = Connection.PieceB;

			if (Adjacency.IsValidIndex(A) && Adjacency.IsValidIndex(B))
			{
				Adjacency[A].Add(B);
				Adjacency[B].Add(A);
			}
		}

		auto IsLive = [&S, N](int32 Piece)
		{
			return Piece >= 0 && Piece < N && !S.IsPieceRemoved(Piece);
		};

		TSet<int32> Region;

		// The one-hop frontier of R, recomputed.
		auto InducedBoundary = [&](const TSet<int32>& R) -> TSet<int32>
		{
			TSet<int32> Ring;

			for (const int32 Piece : R)
			{
				for (const int32 Other : Adjacency[Piece])
				{
					if (IsLive(Other) && !R.Contains(Other))
					{
						Ring.Add(Other);
					}
				}
			}

			return Ring;
		};

		// Admit iff |R + {c}| + |boundary(R + {c})| fits the cap.
		TArray<int32> Frontier;

		auto TryAdmit = [&](int32 Candidate) -> bool
		{
			if (!IsLive(Candidate) || Region.Contains(Candidate))
			{
				return false;
			}

			TSet<int32> Prospective = Region;
			Prospective.Add(Candidate);

			const int32 PosedIfAdmitted = Prospective.Num() + InducedBoundary(Prospective).Num();

			if (PosedIfAdmitted > Cap)
			{
				return false;
			}

			Region = MoveTemp(Prospective);
			Frontier.Add(Candidate);
			return true;
		};

		for (const int32 SeedPiece : Seed)
		{
			TryAdmit(SeedPiece);
		}

		for (int32 Head = 0; Head < Frontier.Num(); ++Head)
		{
			const int32 Piece = Frontier[Head];

			for (const int32 Other : Adjacency[Piece])
			{
				if (!IsLive(Other) || Region.Contains(Other))
				{
					continue;
				}

				TryAdmit(Other);
			}
		}

		if (Region.Num() == 0)
		{
			return 0;
		}

		return Region.Num() + InducedBoundary(Region).Num();
	}
}

/** The posed region+boundary count fits the cap and matches the reference, across seeds and caps. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverCapInvariantTest,
	"DestructionGame.Core.Structure.RegionalProver.RegionUnionBoundaryCapInvariantHoldsAcrossSeedsAndCaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverCapInvariantTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverCapInvariantSupport;

	// Fixture precondition: a 30-block chain.
	{
		FStack Precondition;
		LayStack(Precondition);
		TestEqual(TEXT("FIXTURE: 30 pieces"), Precondition.Structure.NumPieces(), Courses);
		TestEqual(TEXT("FIXTURE: exactly one bed joint per course above the base"),
			Precondition.Structure.NumConnections(), Courses - 1);
		TestTrue(TEXT("FIXTURE: the stack has complete geometry"),
			Precondition.Structure.HasCompleteGeometry());
	}

	/*
	 * Seeds at top, base and middle; caps from below the seed neighbourhood to above the structure.
	 * Fresh fixtures per row, since the prove severs joints.
	 */
	struct FRow { int32 Seed; int32 Cap; };
	const TArray<FRow> Rows = {
		{ 29, 2 }, { 29, 5 }, { 29, 13 }, { 29, 15 }, { 29, 20 }, { 29, 29 }, { 29, 30 }, { 29, 31 }, { 29, 512 },
		{ 0, 5 }, { 0, 15 }, { 0, 30 }, { 0, 40 },
		{ 15, 2 }, { 15, 3 }, { 15, 10 }, { 15, 29 }, { 15, 30 }, { 15, 100 },
	};

	int32 PosingRows = 0;

	for (const FRow& Row : Rows)
	{
		FStack Live;
		LayStack(Live);

		FStack Pristine;
		LayStack(Pristine);
		const int32 Reference = ReferenceUnionCount(Pristine.Structure, { Row.Seed }, Row.Cap);

		Live.Structure.SolveAndBreak_WithRegionalProver({ Row.Seed }, Row.Cap);
		const int32 Posed = Live.Structure.GetLastRegionalProblemBlockCount();

		const int32 PosedOrZero = (Posed == INDEX_NONE) ? 0 : Posed;

		// (2) Matches the reference.
		TestEqual(
			*FString::Printf(
				TEXT("seed %d cap %d: posed union count (%d) must equal the independently-computed ")
				TEXT("flood count (%d)"),
				Row.Seed, Row.Cap, PosedOrZero, Reference),
			PosedOrZero, Reference);

		// (1) A posed count is in [1, cap].
		if (Reference >= 1)
		{
			++PosingRows;

			TestTrue(
				*FString::Printf(
					TEXT("seed %d cap %d: a posed problem must record a positive block count (%d)"),
					Row.Seed, Row.Cap, PosedOrZero),
				PosedOrZero >= 1);

			TestTrue(
				*FString::Printf(
					TEXT("seed %d cap %d: posed union count (%d) must fit the region cap (%d) — the ")
					TEXT("flood must count R and B, or a per-action block budget is silently overspent"),
					Row.Seed, Row.Cap, PosedOrZero, Row.Cap),
				PosedOrZero >= 1 && PosedOrZero <= Row.Cap);
		}
	}

	TestTrue(
		*FString::Printf(
			TEXT("SWEEP SANITY: at least one row must actually pose a region, or the invariant is ")
			TEXT("vacuous (posing rows %d)"),
			PosingRows),
		PosingRows > 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
