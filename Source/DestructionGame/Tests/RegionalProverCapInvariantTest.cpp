// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * REGIONAL COLLAPSE PROVER (REGIONAL_PROVER_PLAN.md §1, review item 12) — THE R-UNION-B CAP
 * INVARIANT, A PROPERTY / INVARIANT GUARD, GREEN ON ARRIVAL. Not a driver: it pins an arithmetic
 * contract that already holds on the slice-2 flood, so it earns its place by catching a future
 * regression rather than by driving a build. Its bite is proven by mutation in the test-expert
 * report (perturbing AdmitToRegion's prospective-boundary arithmetic turns it red).
 *
 * WHAT IT GUARDS. The flood in SolveAndBreak_WithRegionalProver bounds the WHOLE posed problem —
 * the interior region R PLUS the grounded frontier ring B — at RegionBlockCap (the plan's stopping
 * gate "region + grounded boundary <= region cap", §1 gate 1). A cap is chosen as a per-action
 * BLOCK BUDGET (D8, DESIGN §8), so the posed count is a latency-load promise, and an off-by-one in
 * AdmitToRegion's prospective-boundary arithmetic (the Boundary.Num() minus contains(candidate)
 * plus NewRing.Num() term) would silently overspend the budget by the ring, or underspend and
 * shrink the region below what the cap allows. RegionalProverRegionUnionBoundaryFitsTheCap pins ONE
 * (seed {29}, cap 15) point of this; this test pins the whole contract across a SWEEP of seeds and
 * caps so the arithmetic cannot drift at a point the single example does not touch.
 *
 * TWO PROPERTIES, ASSERTED FOR EVERY (SEED, CAP) IN THE TABLE:
 *   (1) THE POSED COUNT FITS THE CAP. Whenever a prove actually poses a problem
 *       (GetLastRegionalProblemBlockCount() >= 1), the posed union count is in [1, RegionBlockCap].
 *       A posed count above the cap is the overspend the latency budget forbids.
 *   (2) THE POSED COUNT EQUALS AN INDEPENDENT REFERENCE. The posed union count equals the block
 *       count a SEPARATELY-CODED flood computes from the same seed and cap. The reference is derived
 *       DIFFERENTLY from production on the axis that matters: production maintains its boundary ring
 *       INCREMENTALLY (Boundary.Add / Boundary.Remove as pieces are admitted, and a prospective
 *       count assembled from Boundary.Num() minus (Boundary.Contains(candidate) ? 1 : 0) plus
 *       NewRing.Num()), whereas the reference RECOMPUTES the induced one-hop frontier of R FROM
 *       SCRATCH as a set operation at every admit test. The two agree only if that incremental
 *       arithmetic is exactly right — so a +1 / -1 slip in it is caught here even though the final
 *       union still, wrongly, fit the cap.
 *
 * WHY THE REFERENCE IS NOT A WORTHLESS MIRROR. It shares production's ADMISSION ORDER (a greedy BFS
 * over joint-hops, seeds first) because on a non-chain the admitted SET — and thus the count —
 * depends on order, so a reference that ignored order would disagree for a reason unrelated to the
 * bug it guards. What it does NOT share is the thing under guard: the prospective-boundary SIZE
 * computation. That is recomputed independently (a fresh NeighboursOf(R) minus R union), which is
 * exactly the quantity an off-by-one would corrupt. The value is in those two being coded apart on
 * that axis, per the plan's "so an off-by-one in AdmitToRegion's prospective-boundary arithmetic is
 * caught".
 *
 * THE FIXTURE: the 30-course, 10 cm/course mortared leaning stack (the row-3 FALLS rung of
 * LeaningStackAcceptanceTest) — a linear chain, one bed joint per course. A chain is the honest
 * shape for a CAP-ARITHMETIC guard: the posed count is a clean function of seed and cap
 * (min(cap, reachable) once the seed's neighbourhood fits), so a divergence is unambiguously an
 * arithmetic fault rather than a topology accident.
 *
 * ASSERT ON MECHANISM, NEVER DISPLACEMENT. The only quantities read are the posed BLOCK COUNT (a
 * count the oracle was handed) and set sizes — mechanism-adjacent and immune to jitter. No
 * centimetre of movement is read.
 *
 * NEEDS A TICKING WORLD: NO. Each row is one in-process SolveAndBreak_WithRegionalProver call and a
 * state query; no Chaos, no world tick. UNITS ARE DERIVED HERE, never imported. NAMED NAMESPACE,
 * not anonymous: a unity build merges many files into one translation unit.
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

	/** Density-first multiplication order — the PieceMassKg contract; 2.72163125 kg. */
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
	 * THE INDEPENDENT REFERENCE. Flood a region from the seed by joint-hops, greedily, in the same
	 * BFS/piece order production admits, and return the union block count |R + B| — but recompute the
	 * induced boundary ring FROM SCRATCH at every admit test (NeighboursOf(R) minus R), rather than
	 * maintaining it incrementally as production does. Returns 0 when no piece is admitted (the
	 * seed's neighbourhood does not fit the cap), matching production's "no pose" (block count 0 /
	 * INDEX_NONE).
	 *
	 * The adjacency is read off the INTACT structure — the state the flood sees at the top of the
	 * call, before any stitch severs a joint — via GetConnection, so the reference is computed on a
	 * pristine copy of the fixture.
	 */
	int32 ReferenceUnionCount(const FStructure& S, const TArray<int32>& Seed, int32 Cap)
	{
		const int32 N = S.NumPieces();

		/* Undirected joint-hop adjacency over intact connections only. */
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

		/* The induced one-hop frontier of Region, recomputed as a set operation. */
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

		/* Admit the candidate iff |R + {c}| + |boundary(R + {c})| still fits the cap. */
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

/**
 * The posed region+boundary block count fits the cap and equals an independently-coded flood's
 * count, across a sweep of seeds and caps.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverCapInvariantTest,
	"DestructionGame.Core.Structure.RegionalProver.RegionUnionBoundaryCapInvariantHoldsAcrossSeedsAndCaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverCapInvariantTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverCapInvariantSupport;

	/* FIXTURE PRECONDITION, ONCE: a 30-block chain, one bed joint per course above the base. */
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
	 * THE SWEEP. Seeds at the top, base, and middle; caps spanning below the seed neighbourhood, at
	 * cutting sizes, and above the structure. Each row is an independent fixture (the prove severs
	 * joints, so it must not be reused). "At least one row poses" is asserted at the end so an
	 * accidental all-decline sweep cannot pass vacuously.
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

		/* (2) The posed count equals the independent reference — the off-by-one net. */
		TestEqual(
			*FString::Printf(
				TEXT("seed %d cap %d: posed union count (%d) must equal the independently-computed ")
				TEXT("flood count (%d)"),
				Row.Seed, Row.Cap, PosedOrZero, Reference),
			PosedOrZero, Reference);

		/* (1) When a prove poses, the posed count fits the cap and is at least one. */
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
