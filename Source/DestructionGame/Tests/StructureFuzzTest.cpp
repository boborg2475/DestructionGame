// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A SEEDED STRUCTURAL FUZZ over the load solver.
 *
 * Everything here lives in a NAMED namespace rather than an anonymous one. The other
 * test files in this directory each declare their own Mortar, MakeNaN and
 * BedJointNormal in anonymous namespaces, which only works while UBT keeps them in
 * separate translation units; a unity build that merged two of them would collide.
 * This file costs a tenth of a second on every test run and should not also add a
 * standing risk of breaking the build for reasons unrelated to it.
 */
namespace StructureFuzzSupport
{
	/**
	 * Unreal's gravity, 980 cm/s2, spelled out rather than imported so the test fails
	 * if production gets it wrong instead of agreeing with it. Mass is already in
	 * kilograms and length already in centimetres, so MassKg * 980 IS a force in
	 * Unreal units — the 1 N = 100 uu conversion is baked into the 980 and applying it
	 * a second time is the standard way to be wrong by exactly 100x.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * cos(45 degrees): where the bed/head tier line sits, per DESIGN.md §3.
	 *
	 * Spelled as the same literal production uses, bit for bit, for the reason
	 * StructureTest.cpp's copy gives: the tier rule is transcribed rather than
	 * re-derived here (see BuildSupports), so an ulp of difference would read as a
	 * production bug on a normal tilted near 45 degrees. If the production constant
	 * ever changes, change this one to match; do not re-derive it.
	 */
	constexpr double BedJointCosine = 0.70710678118654752440;

	/**
	 * THE SEED, and the whole reason this test is allowed in the suite.
	 *
	 * The same structures every run, in the same order. A generator seeded from the
	 * clock would find more shapes over time and would also fail on a Tuesday for a
	 * structure nobody can reproduce, and a flaky test is worse than no test. New
	 * coverage comes from raising CaseCount or widening the tables below — both
	 * deterministic — never from fresh randomness.
	 *
	 * Each case is generated from ITS OWN seed, BaseSeed + case index, so one failing
	 * seed reproduces exactly one structure regardless of what the cases before it
	 * did. Changing CaseCount therefore never renumbers an existing case.
	 *
	 * THE COST, measured rather than guessed: 4,000 structures took 32 ms and 12,000
	 * take about 100 ms, against roughly 250 ms for the rest of the suite. Raising the
	 * count is the cheapest coverage available here, but it is not free forever — if it
	 * ever starts to matter, cut the count rather than the determinism.
	 */
	constexpr uint64 BaseSeed = 20260731ull;
	constexpr int32 CaseCount = 12000;

	/**
	 * A failing solver would otherwise write tens of thousands of log lines. The first
	 * few are what a maintainer turns into a named regression test; the rest are noise,
	 * and the count is reported separately so nothing is hidden.
	 */
	constexpr int32 MaxReportedFailures = 12;

	/**
	 * SplitMix64, written out rather than taken from FRandomStream.
	 *
	 * Two reasons, both about reproducibility. It is a documented, stable algorithm, so
	 * a seed means the same structure in a decade and in a scratch script outside the
	 * engine, where FRandomStream's stream is an engine implementation detail. And its
	 * successive seeds are decorrelated by construction, which is what makes
	 * "BaseSeed + index" a legitimate way to get four thousand independent cases.
	 *
	 * Integer draws only. Nothing here depends on floating-point rounding, so the same
	 * seed builds the same structure on any compiler.
	 */
	struct FSeededRandom
	{
		explicit FSeededRandom(uint64 InSeed)
			: State(InSeed)
		{
		}

		uint64 Next()
		{
			State += 0x9E3779B97F4A7C15ull;
			uint64 Z = State;
			Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
			Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
			return Z ^ (Z >> 31);
		}

		int32 NextBelow(int32 Bound)
		{
			return static_cast<int32>(Next() % static_cast<uint64>(Bound));
		}

		bool NextChanceInThousand(int32 Chance)
		{
			return NextBelow(1000) < Chance;
		}

		uint64 State;
	};

	struct FFuzzPiece
	{
		double MassKg = 0.0;
		bool bIsGrounded = false;
	};

	struct FFuzzJoint
	{
		int32 PieceA = INDEX_NONE;
		int32 PieceB = INDEX_NONE;
		FVector Normal = FVector::ZAxisVector;
		double AreaSqCm = 0.0;
	};

	struct FFuzzCase
	{
		TArray<FFuzzPiece> Pieces;
		TArray<FFuzzJoint> Joints;
	};

	/**
	 * One random structure from one seed.
	 *
	 * The tables are deliberately small and deliberately awkward:
	 *
	 *  - NORMALS INCLUDE TILTED ONES. Every fixture in StructureTest.cpp is
	 *    axis-aligned except two, and CURRENT_STATE.md records what that concealed: an
	 *    invariant asserted over fixtures that all share a hidden property is not an
	 *    invariant. Both sides of the 45-degree line are represented, in the XZ and the
	 *    YZ plane, pointing up and pointing down, plus one non-unit normal because
	 *    AddConnection stores what it is given and only ApplyForce normalises.
	 *  - AREAS SPAN SEVEN ORDERS OF MAGNITUDE, so an area-weighted split that is
	 *    actually an even split cannot survive, and so the division has to stay sane
	 *    when one share is a ten-millionth of the other.
	 *  - MASSES INCLUDE ZERO. A massless piece is legal (AddPiece accepts it) and it is
	 *    the case where "supported" and "carries something" come apart.
	 *
	 * Sizes stay small — 2 to 8 pieces — on purpose. The defects this shape of test
	 * finds are topological, and every one found so far appears in graphs of five or
	 * fewer pieces; larger graphs cost runtime and mostly re-find the same knots inside
	 * something harder to read in a failure report.
	 */
	FFuzzCase GenerateCase(uint64 Seed)
	{
		static const FVector Normals[] = {
			FVector(0.0, 0.0, 1.0),   // flat bed joint
			FVector(0.0, 0.0, -1.0),  // the same interface, declared upper piece first
			FVector(1.0, 0.0, 0.0),   // head joint
			FVector(-1.0, 0.0, 0.0),
			FVector(0.0, 1.0, 0.0),
			FVector(0.6427876096865393, 0.0, 0.7660444431189780),  // 40 deg off vertical: bed tier
			FVector(0.7660444431189780, 0.0, 0.6427876096865393),  // 50 deg: head tier, pressed
			FVector(0.0, 0.7660444431189780, -0.6427876096865393), // 130 deg: head tier, face above
			FVector(0.0, 0.5, 0.8660254037844387),                 // 30 deg, out of the XZ plane
			FVector(3.0, 0.0, 4.0),                                // non-unit, 36.87 deg
		};

		static const double Areas[] = { 50.0, 100.0, 300.0, 1.0e-3, 1.0e4 };
		static const double Masses[] = { 0.0, 1.0, 2.72, 5.0, 20.0 };

		constexpr int32 GroundedChanceInThousand = 300;
		constexpr int32 JointChanceInThousand = 450;

		FSeededRandom Random(Seed);
		FFuzzCase Case;

		const int32 PieceCount = 2 + Random.NextBelow(7);
		for (int32 Index = 0; Index < PieceCount; ++Index)
		{
			FFuzzPiece Piece;
			Piece.MassKg = Masses[Random.NextBelow(UE_ARRAY_COUNT(Masses))];
			Piece.bIsGrounded = Random.NextChanceInThousand(GroundedChanceInThousand);
			Case.Pieces.Add(Piece);
		}

		for (int32 A = 0; A < PieceCount; ++A)
		{
			for (int32 B = A + 1; B < PieceCount; ++B)
			{
				if (!Random.NextChanceInThousand(JointChanceInThousand))
				{
					continue;
				}

				FFuzzJoint Joint;
				Joint.Normal = Normals[Random.NextBelow(UE_ARRAY_COUNT(Normals))];
				Joint.AreaSqCm = Areas[Random.NextBelow(UE_ARRAY_COUNT(Areas))];

				// WHICH END IS NAMED FIRST is varied deliberately. The stored force is
				// the force on PieceB, so declaration order flips its sign and must not
				// flip anything else; a wall builder walking its courses downward
				// produces the "wrong" order naturally.
				const bool bDeclareLowerFirst = Random.NextChanceInThousand(500);
				Joint.PieceA = bDeclareLowerFirst ? A : B;
				Joint.PieceB = bDeclareLowerFirst ? B : A;

				Case.Joints.Add(Joint);
			}
		}

		return Case;
	}

	/** The structure as one line, so a failing seed can be pasted into a new test. */
	FString DescribeCase(uint64 Seed, const FFuzzCase& Case)
	{
		FString Text = FString::Printf(TEXT("seed %llu ->"), static_cast<unsigned long long>(Seed));

		for (int32 Index = 0; Index < Case.Pieces.Num(); ++Index)
		{
			Text += FString::Printf(TEXT(" P%d(m=%g,g=%d)"),
				Index, Case.Pieces[Index].MassKg, Case.Pieces[Index].bIsGrounded ? 1 : 0);
		}

		Text += TEXT(" |");

		for (int32 Index = 0; Index < Case.Joints.Num(); ++Index)
		{
			const FFuzzJoint& Joint = Case.Joints[Index];
			Text += FString::Printf(TEXT(" C%d: %d->%d n=(%g,%g,%g) A=%g ;"),
				Index, Joint.PieceA, Joint.PieceB,
				Joint.Normal.X, Joint.Normal.Y, Joint.Normal.Z, Joint.AreaSqCm);
		}

		return Text;
	}

	int32 OtherEndOf(const FFuzzJoint& Joint, int32 PieceIndex)
	{
		if (Joint.PieceA == PieceIndex)
		{
			return Joint.PieceB;
		}

		if (Joint.PieceB == PieceIndex)
		{
			return Joint.PieceA;
		}

		return INDEX_NONE;
	}

	/**
	 * The two-tier support rule of DESIGN.md §3, as joint indices per piece.
	 *
	 * HONEST ABOUT WHAT THIS IS WORTH, in the same terms as SpecSupportsOf in
	 * StructureTest.cpp: it is a transcription of RoleOf, threshold literal included,
	 * so it is NOT a second opinion about whether the tier rule is right. If the rule
	 * itself were wrong this would be wrong identically and agree enthusiastically.
	 * SupportTierThreshold is what pins the rule; this only shares its input with the
	 * oracle below, exactly as the reviewer's scratch harness did.
	 *
	 * The independence that matters is downstream of here — what is DONE with the
	 * relation. Production walks it breadth-first from the ground and accumulates in a
	 * Kahn topological order; the oracle takes a least fixed point and finds its cycles
	 * by transitive closure. Those two really are different algorithms, and that is
	 * where the value of the comparison lies.
	 */
	TArray<TArray<int32>> BuildSupports(const FFuzzCase& Case)
	{
		TArray<TArray<int32>> Supports;
		Supports.SetNum(Case.Pieces.Num());

		for (int32 PieceIndex = 0; PieceIndex < Case.Pieces.Num(); ++PieceIndex)
		{
			TArray<int32> HeadJoints;

			for (int32 Index = 0; Index < Case.Joints.Num(); ++Index)
			{
				const FFuzzJoint& Joint = Case.Joints[Index];

				FVector UnitNormal = Joint.Normal;
				if (!UnitNormal.Normalize())
				{
					continue;
				}

				double NormalZTowardPiece = 0.0;
				if (Joint.PieceB == PieceIndex)
				{
					NormalZTowardPiece = UnitNormal.Z;
				}
				else if (Joint.PieceA == PieceIndex)
				{
					NormalZTowardPiece = -UnitNormal.Z;
				}
				else
				{
					continue;
				}

				if (!(FMath::Abs(NormalZTowardPiece) > BedJointCosine))
				{
					HeadJoints.Add(Index);
				}
				else if (NormalZTowardPiece > 0.0)
				{
					// A bed joint BENEATH bears the piece; one ABOVE is something
					// resting on it and holds nothing up, so it is dropped entirely.
					Supports[PieceIndex].Add(Index);
				}
			}

			// The fallback, and only the fallback: one bed joint beneath wins outright
			// over any number of head joints.
			if (Supports[PieceIndex].Num() == 0)
			{
				Supports[PieceIndex] = MoveTemp(HeadJoints);
			}
		}

		return Supports;
	}

	struct FOracleResult
	{
		/** Whether each piece is held up, once cycles have been stranded. */
		TArray<bool> bSupported;

		/** Pieces the closure found on a cycle of the support relation. */
		TArray<bool> bStrandedInCycle;

		/** What each joint carries, signed by which end is held up. */
		TArray<double> JointForceZ;

		/** The relaxation reached a fixed point, which it cannot do on a live cycle. */
		bool bForcesConverged = false;

		/** Two pieces both pushed load through the same joint — a 2-cycle survived. */
		bool bJointClaimedTwice = false;
	};

	/**
	 * THE ORACLE — the same question answered by a different algorithm.
	 *
	 * Deliberately NOT a transcription of Structure.cpp. An oracle that mirrors
	 * production's algorithm agrees with production's bugs and is worth nothing; the
	 * entire value of this file is that the two are derived differently and still land
	 * on the same numbers.
	 *
	 *   SUPPORTEDNESS is a LEAST FIXED POINT: start with the grounded pieces and repeat
	 *   "a piece is held up if any of its supports is held up" until nothing changes.
	 *   Production does a breadth-first walk outward from the ground over the reversed
	 *   relation. Same answer, opposite direction, no shared traversal.
	 *
	 *   CYCLES are found by WARSHALL TRANSITIVE CLOSURE: build the adjacency of
	 *   "p pushes load into s" over supported ungrounded pieces, close it, and any piece
	 *   that reaches itself is on a cycle. Production instead runs a per-piece
	 *   breadth-first walk asking whether the load leaving a piece comes back to it.
	 *   Closure sees every cycle at once and cannot depend on visit order.
	 *
	 *   The two interact — stranding a cycle changes who reaches the ground, which can
	 *   strand what rested on it — so both are wrapped in an outer loop to a fixed
	 *   point, which is the one structural feature the two implementations must share
	 *   because the problem has it.
	 *
	 *   FORCES are computed by JACOBI RELAXATION rather than in a topological order.
	 *   Every piece recomputes what it received from scratch each pass, from the
	 *   previous pass's answer; on an acyclic relation this stops changing after at
	 *   most one pass per level, bit for bit, because it is the same arithmetic on the
	 *   same values. That it converges at all is therefore an assertion in itself: a
	 *   surviving cycle never settles. Production's Kahn ordering visits each piece
	 *   exactly once and would deadlock instead — which is the whole reason the
	 *   stranding rule exists.
	 *
	 * Only the tier rule is shared, through BuildSupports, and its comment says why.
	 */
	FOracleResult SolveWithOracle(const FFuzzCase& Case)
	{
		const int32 PieceCount = Case.Pieces.Num();
		const int32 JointCount = Case.Joints.Num();

		const TArray<TArray<int32>> Supports = BuildSupports(Case);

		FOracleResult Result;
		Result.JointForceZ.Init(0.0, JointCount);
		Result.bStrandedInCycle.Init(false, PieceCount);

		TArray<bool>& bSupported = Result.bSupported;

		for (;;)
		{
			// Least fixed point of "held up by something that is itself held up".
			bSupported.Init(false, PieceCount);
			for (int32 Index = 0; Index < PieceCount; ++Index)
			{
				bSupported[Index] = Case.Pieces[Index].bIsGrounded;
			}

			bool bChanged = true;
			while (bChanged)
			{
				bChanged = false;
				for (int32 Index = 0; Index < PieceCount; ++Index)
				{
					if (bSupported[Index] || Result.bStrandedInCycle[Index])
					{
						continue;
					}

					for (const int32 JointIndex : Supports[Index])
					{
						if (bSupported[OtherEndOf(Case.Joints[JointIndex], Index)])
						{
							bSupported[Index] = true;
							bChanged = true;
							break;
						}
					}
				}
			}

			// Adjacency of "pushes its load into", over supported ungrounded pieces
			// only. A grounded piece is a SINK — the earth absorbs what arrives and
			// passes nothing on — so leaving it out is what stops two bricks side by
			// side on the ground, each naming the other through their head joint, from
			// reading as a loop.
			TArray<bool> Reaches;
			Reaches.Init(false, PieceCount * PieceCount);

			for (int32 Index = 0; Index < PieceCount; ++Index)
			{
				if (!bSupported[Index] || Case.Pieces[Index].bIsGrounded)
				{
					continue;
				}

				for (const int32 JointIndex : Supports[Index])
				{
					const int32 Support = OtherEndOf(Case.Joints[JointIndex], Index);
					if (bSupported[Support] && !Case.Pieces[Support].bIsGrounded)
					{
						Reaches[Index * PieceCount + Support] = true;
					}
				}
			}

			for (int32 Via = 0; Via < PieceCount; ++Via)
			{
				for (int32 From = 0; From < PieceCount; ++From)
				{
					if (!Reaches[From * PieceCount + Via])
					{
						continue;
					}

					for (int32 To = 0; To < PieceCount; ++To)
					{
						if (Reaches[Via * PieceCount + To])
						{
							Reaches[From * PieceCount + To] = true;
						}
					}
				}
			}

			bool bStrandedAnything = false;
			for (int32 Index = 0; Index < PieceCount; ++Index)
			{
				if (Reaches[Index * PieceCount + Index] && !Result.bStrandedInCycle[Index])
				{
					Result.bStrandedInCycle[Index] = true;
					bStrandedAnything = true;
				}
			}

			if (!bStrandedAnything)
			{
				break;
			}
		}

		// The load path of each piece: its supports that themselves reach the ground.
		// A share handed to something falling never arrives, so it is not a share.
		TArray<TArray<int32>> LoadPaths;
		LoadPaths.SetNum(PieceCount);

		for (int32 Index = 0; Index < PieceCount; ++Index)
		{
			for (const int32 JointIndex : Supports[Index])
			{
				if (bSupported[OtherEndOf(Case.Joints[JointIndex], Index)])
				{
					LoadPaths[Index].Add(JointIndex);
				}
			}
		}

		// Jacobi relaxation to a fixed point. Bounded by one pass per level of the
		// relation plus one to observe that it settled; PieceCount + 2 is generous.
		TArray<double> Received;
		Received.Init(0.0, PieceCount);

		for (int32 Pass = 0; Pass < PieceCount + 2; ++Pass)
		{
			TArray<double> NextReceived;
			NextReceived.Init(0.0, PieceCount);

			for (int32 Index = 0; Index < PieceCount; ++Index)
			{
				if (!bSupported[Index] || Case.Pieces[Index].bIsGrounded)
				{
					continue;
				}

				const double TotalUU =
					Received[Index] + Case.Pieces[Index].MassKg * GravityCmPerSecondSquared;

				double TotalAreaSqCm = 0.0;
				for (const int32 JointIndex : LoadPaths[Index])
				{
					TotalAreaSqCm += Case.Joints[JointIndex].AreaSqCm;
				}

				if (!(TotalAreaSqCm > 0.0))
				{
					continue;
				}

				for (const int32 JointIndex : LoadPaths[Index])
				{
					const int32 Support = OtherEndOf(Case.Joints[JointIndex], Index);
					NextReceived[Support] +=
						TotalUU * (Case.Joints[JointIndex].AreaSqCm / TotalAreaSqCm);
				}
			}

			if (NextReceived == Received)
			{
				Result.bForcesConverged = true;
				break;
			}

			Received = MoveTemp(NextReceived);
		}

		TArray<int32> ClaimedBy;
		ClaimedBy.Init(INDEX_NONE, JointCount);

		for (int32 Index = 0; Index < PieceCount; ++Index)
		{
			if (!bSupported[Index] || Case.Pieces[Index].bIsGrounded)
			{
				continue;
			}

			const double TotalUU =
				Received[Index] + Case.Pieces[Index].MassKg * GravityCmPerSecondSquared;

			double TotalAreaSqCm = 0.0;
			for (const int32 JointIndex : LoadPaths[Index])
			{
				TotalAreaSqCm += Case.Joints[JointIndex].AreaSqCm;
			}

			if (!(TotalAreaSqCm > 0.0))
			{
				continue;
			}

			for (const int32 JointIndex : LoadPaths[Index])
			{
				const FFuzzJoint& Joint = Case.Joints[JointIndex];
				const double ShareUU = TotalUU * (Joint.AreaSqCm / TotalAreaSqCm);

				// The force belonging to a joint is the force acting on PieceB, so it
				// points down when the loaded piece is named second and up when the same
				// joint was declared the other way round. Get this backwards and a
				// plainly compressed joint reads as tension.
				Result.JointForceZ[JointIndex] = Joint.PieceB == Index ? -ShareUU : ShareUU;

				if (ClaimedBy[JointIndex] != INDEX_NONE)
				{
					Result.bJointClaimedTwice = true;
				}
				ClaimedBy[JointIndex] = Index;
			}
		}

		return Result;
	}
}

/**
 * Four thousand random structures, checked against properties that must hold for any
 * structure and against an independently derived oracle.
 *
 * WHY THIS EXISTS AS A TEST RATHER THAN A SCRIPT. The hand-written cases in
 * StructureTest.cpp encode the shapes somebody thought of — a stack, a running bond, a
 * keystone, a knot. Two real defects in this solver lived in shapes nobody would think
 * to write, and were found only by generating thousands of random graphs and checking
 * them against a second implementation: the foundation-stranding bug appeared in 14 of
 * 8,500 random graphs and in zero of the deliberate ones. That machinery lived in
 * throwaway scratch scripts and was rebuilt from scratch every review round. Here it
 * protects every future change instead, which matters immediately: phase 2b re-solves
 * half-collapsed structures after every break, so oddly shaped, partly stranded graphs
 * stop being exotic and become the ordinary input.
 *
 * IT IS EXPECTED TO BE GREEN ON ARRIVAL, and that is stated plainly rather than dressed
 * up. It drives no new behaviour and it is not a red step. What justifies its place in
 * the suite is that it BITES: mutating Structure.cpp to strand on un-orderability
 * rather than on being one's own support, or to drop the falling-support filter from
 * the split, turns it red with a named seed. If a change to this file or to the solver
 * ever leaves it unable to fail, it is no longer worth its runtime — verify that with a
 * deliberate mutation rather than assuming.
 *
 * WHAT IS ASSERTED, and why each one earns its place:
 *
 *  - GROUND REACTION EQUALS THE WEIGHT OF EXACTLY THE PIECES THE SOLVER SAYS IT IS
 *    HOLDING UP. Short means load was stranded on the way down; long means it was
 *    double-counted. Measured against the solver's own claim, so it survives the
 *    stranding rule being revisited.
 *  - NO FORCE IS EVER NaN AND EVERY FORCE IS FINITE. FMath::Max discards a NaN and
 *    FMath::Min replaces it, so a NaN that reaches the arithmetic comes out as a
 *    confident, plausible number rather than an obvious fault.
 *  - NO PIECE IS REPORTED SUPPORTED WITHOUT ITS LOAD ACTUALLY BEING ROUTED. This is the
 *    property conservation cannot see: over-stranding removes a piece's weight from
 *    BOTH sides of the conservation equation, so the sum still balances while the
 *    answer is wrong. Asserted twice over, weakly and strongly — the weak form is a
 *    lower bound needing no tier rule at all (a supported ungrounded piece has at least
 *    its own weight on the joints touching it), the strong form is the per-joint
 *    comparison against the oracle, where a piece the ordering never reached shows up
 *    as a joint carrying zero where the oracle carries a share.
 *  - STRANDING NEVER TRAVELS DOWNWARD. A piece whose support list contains a grounded
 *    piece, and which is not itself in a knot, is never reported falling. DESIGN.md §3:
 *    un-orderability is a solver artefact, and a brick resting on the earth is standing
 *    up no matter what is happening above it.
 *  - SUPPORTEDNESS AND THE CYCLE-STRANDED SET MATCH THE ORACLE, piece by piece.
 *  - SOLVING NEVER BREAKS A JOINT AND IS REPEATABLE. Phase 2b will re-solve after every
 *    break; a solve whose answer depended on how many times it had been run would make
 *    the cascade order-dependent.
 *
 * WHAT IS DELIBERATELY NOT ASSERTED:
 *
 *  - "No joint is ever in tension." That holds for axis-aligned fixtures and is false
 *    for the model: the head tier is sign-blind, so a face tilted past 45 degrees
 *    sitting above a piece supports it by being pulled open, which DESIGN.md §3 states
 *    and accepts. An earlier fuzz with tilted normals produced tension in 337 of 6,000
 *    structures. Structure.TiltedJointClassification characterises it.
 *  - Production's internal stranded set, piece by piece. Only its INTERSECTION with the
 *    observable is checkable: IsPieceSupported cannot distinguish "in a knot" from
 *    "resting only on a knot", since both are correctly reported falling. So the
 *    oracle's cycle set is asserted one way — a piece on a cycle must be reported
 *    unsupported — and the composite answer, which is what callers actually see, is
 *    compared in both directions.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureFuzzTest,
	"DestructionGame.Core.Structure.Fuzz",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureFuzzTest::RunTest(const FString& Parameters)
{
	using namespace StructureFuzzSupport;

	const double StartSeconds = FPlatformTime::Seconds();

	int32 FailureCount = 0;
	int32 ReportedCount = 0;

	// Cheap headline numbers, so a run that silently generated nothing is visible.
	int32 TotalPieces = 0;
	int32 TotalJoints = 0;
	int32 StrandedPieces = 0;
	int32 FallingPieces = 0;

	for (int32 CaseIndex = 0; CaseIndex < CaseCount; ++CaseIndex)
	{
		const uint64 Seed = BaseSeed + static_cast<uint64>(CaseIndex);
		const FFuzzCase Case = GenerateCase(Seed);

		const int32 PieceCount = Case.Pieces.Num();
		const int32 JointCount = Case.Joints.Num();

		TotalPieces += PieceCount;
		TotalJoints += JointCount;

		// Formatted only when something has gone wrong. Four thousand cases times fifty
		// assertions is two hundred thousand FString::Printf calls if the message is
		// built eagerly, which is most of the runtime of a test that finds nothing.
		auto Report = [this, &FailureCount, &ReportedCount, Seed, &Case](const FString& What)
		{
			++FailureCount;
			if (ReportedCount < MaxReportedFailures)
			{
				++ReportedCount;
				AddError(FString::Printf(TEXT("%s\n    %s"), *What, *DescribeCase(Seed, Case)));
			}
		};

		FStructure Structure;

		bool bBuiltCleanly = true;
		for (int32 Index = 0; Index < PieceCount; ++Index)
		{
			bBuiltCleanly &= Structure.AddPiece(Case.Pieces[Index].MassKg, Case.Pieces[Index].bIsGrounded) == Index;
		}

		for (int32 Index = 0; Index < JointCount; ++Index)
		{
			FConnection Connection;
			Connection.PieceA = Case.Joints[Index].PieceA;
			Connection.PieceB = Case.Joints[Index].PieceB;
			Connection.InterfaceNormal = Case.Joints[Index].Normal;
			Connection.InterfaceAreaSqCm = Case.Joints[Index].AreaSqCm;

			// Absurdly strong on every axis. The generator is free to build a joint
			// carrying twenty tonnes, and this test is about where load GOES; a joint
			// that could give would make it about something else. Solving is
			// non-destructive anyway, and that is asserted below rather than assumed.
			Connection.Strength = FConnectionStrength{ 1.0e9, 1.0e9, 1.0e9, 0.0 };

			bBuiltCleanly &= Structure.AddConnection(Connection) == Index;
		}

		if (!bBuiltCleanly || Structure.NumPieces() != PieceCount || Structure.NumConnections() != JointCount)
		{
			// The generator only emits inputs AddPiece and AddConnection must accept, so
			// this is a broken fixture rather than a finding — and reporting it as a
			// solver failure would send somebody chasing a bug that is in this file.
			Report(FString::Printf(
				TEXT("FIXTURE: the generated structure was rejected at the door, %d/%d pieces and %d/%d joints"),
				Structure.NumPieces(), PieceCount, Structure.NumConnections(), JointCount));
			continue;
		}

		Structure.SolveLoads();

		const FOracleResult Oracle = SolveWithOracle(Case);
		const TArray<TArray<int32>> Supports = BuildSupports(Case);

		// The oracle's own sanity. A relaxation that never settles, or a joint two
		// pieces both push through, means the oracle failed to strand a cycle — its
		// answers below would then be meaningless rather than merely different.
		if (!Oracle.bForcesConverged || Oracle.bJointClaimedTwice)
		{
			Report(FString::Printf(
				TEXT("ORACLE: relaxation converged %d, joint claimed by two pieces %d"),
				Oracle.bForcesConverged ? 1 : 0, Oracle.bJointClaimedTwice ? 1 : 0));
		}

		// Scale for the tolerances: the heaviest thing this structure could possibly be
		// holding up. Areas span seven orders of magnitude, so a share can be a
		// ten-millionth of the total and an absolute tolerance alone would be either
		// useless or meaningless.
		double TotalWeightUU = 0.0;
		for (const FFuzzPiece& Piece : Case.Pieces)
		{
			TotalWeightUU += Piece.MassKg * GravityCmPerSecondSquared;
		}

		const double Tolerance = FMath::Max(1.0e-6, 1.0e-9 * TotalWeightUU);

		double ReportedSupportedWeightUU = 0.0;
		double GroundReactionUU = 0.0;

		for (int32 Index = 0; Index < PieceCount; ++Index)
		{
			const bool bProductionSupported = Structure.IsPieceSupported(Index);

			if (bProductionSupported != Oracle.bSupported[Index])
			{
				Report(FString::Printf(
					TEXT("SUPPORT: piece %d, production says %d, the oracle says %d"),
					Index, bProductionSupported ? 1 : 0, Oracle.bSupported[Index] ? 1 : 0));
			}

			if (Oracle.bStrandedInCycle[Index])
			{
				++StrandedPieces;

				if (bProductionSupported)
				{
					Report(FString::Printf(
						TEXT("STRANDING: piece %d is on a cycle of the support relation and cannot be routed, ")
						TEXT("but production reports it held up"),
						Index));
				}
			}

			if (!bProductionSupported)
			{
				++FallingPieces;
			}

			if (Case.Pieces[Index].bIsGrounded)
			{
				continue;
			}

			// STRANDING NEVER TRAVELS DOWNWARD. A piece that rests on the earth stands
			// up whatever is happening above it — unless it is itself caught in a knot,
			// which it can be through some OTHER support, so the oracle's cycle set is
			// the exclusion rather than a blanket one.
			if (!Oracle.bStrandedInCycle[Index] && !bProductionSupported)
			{
				for (const int32 JointIndex : Supports[Index])
				{
					if (Case.Pieces[OtherEndOf(Case.Joints[JointIndex], Index)].bIsGrounded)
					{
						Report(FString::Printf(
							TEXT("DOWNWARD STRANDING: piece %d is supported by grounded piece %d ")
							TEXT("through joint %d and is not itself in a knot, yet production reports it falling"),
							Index, OtherEndOf(Case.Joints[JointIndex], Index), JointIndex));
						break;
					}
				}
			}

			if (!bProductionSupported)
			{
				continue;
			}

			const double OwnWeightUU = Case.Pieces[Index].MassKg * GravityCmPerSecondSquared;
			ReportedSupportedWeightUU += OwnWeightUU;

			// SUPPORTED MEANS ITS LOAD WENT SOMEWHERE. Summed over every joint TOUCHING
			// the piece rather than over its supports, so it needs no transcription of
			// the tier rule to stay in step with: whichever subset turns out to be the
			// supports, the total over all of them is a lower bound. A piece the
			// accumulation never ordered has zero here and its own weight expected.
			double TouchingUU = 0.0;
			for (int32 JointIndex = 0; JointIndex < JointCount; ++JointIndex)
			{
				if (Case.Joints[JointIndex].PieceA == Index || Case.Joints[JointIndex].PieceB == Index)
				{
					TouchingUU += FMath::Abs(Structure.GetConnectionForce(JointIndex).Z);
				}
			}

			if (TouchingUU + Tolerance < OwnWeightUU)
			{
				Report(FString::Printf(
					TEXT("UNROUTED: piece %d is reported held up but only %f of its %f weight ")
					TEXT("is on any joint touching it"),
					Index, TouchingUU, OwnWeightUU));
			}
		}

		for (int32 Index = 0; Index < JointCount; ++Index)
		{
			const FVector Force = Structure.GetConnectionForce(Index);

			if (Force.ContainsNaN()
				|| !FMath::IsFinite(Force.X) || !FMath::IsFinite(Force.Y) || !FMath::IsFinite(Force.Z))
			{
				Report(FString::Printf(TEXT("NON-FINITE: joint %d carries (%f, %f, %f)"),
					Index, Force.X, Force.Y, Force.Z));
			}

			// Gravity does not change direction because a joint happens to be tilted.
			// It is FConnection that resolves this vector against the interface normal;
			// a solver that pushed the load along the normal itself would produce
			// plausible magnitudes and entirely the wrong direction.
			if (!FMath::IsNearlyZero(Force.X, 1.0e-9) || !FMath::IsNearlyZero(Force.Y, 1.0e-9))
			{
				Report(FString::Printf(TEXT("NOT VERTICAL: joint %d carries (%f, %f, %f)"),
					Index, Force.X, Force.Y, Force.Z));
			}

			if (!FMath::IsNearlyEqual(Force.Z, Oracle.JointForceZ[Index], Tolerance))
			{
				Report(FString::Printf(
					TEXT("LOAD: joint %d (%d->%d, area %g), production carries %f, the oracle carries %f"),
					Index, Case.Joints[Index].PieceA, Case.Joints[Index].PieceB,
					Case.Joints[Index].AreaSqCm, Force.Z, Oracle.JointForceZ[Index]));
			}

			if (Structure.GetConnection(Index).HasGiven())
			{
				Report(FString::Printf(TEXT("BROKEN: joint %d gave during a solve, which computes and never breaks"),
					Index));
			}

			if (Case.Pieces[Case.Joints[Index].PieceA].bIsGrounded
				|| Case.Pieces[Case.Joints[Index].PieceB].bIsGrounded)
			{
				GroundReactionUU += FMath::Abs(Force.Z);
			}
		}

		// GROUND-REACTION CONSERVATION, against the solver's OWN claim about what it is
		// holding up. Everything held up has to arrive somewhere and the only somewhere
		// is the earth.
		if (!FMath::IsNearlyEqual(GroundReactionUU, ReportedSupportedWeightUU, Tolerance))
		{
			Report(FString::Printf(
				TEXT("CONSERVATION: the solver reports holding up %f but %f reaches the ground"),
				ReportedSupportedWeightUU, GroundReactionUU));
		}

		// REPEATABILITY. Solving is documented as non-destructive, which is worth
		// nothing unless a second solve gives the identical answer — phase 2b re-solves
		// after every break, and a solver whose output drifted with the number of calls
		// would make the cascade depend on how often it was asked.
		//
		// Measured against THE FIRST SOLVE, not against the oracle. Comparing the second
		// solve to the oracle instead looks equivalent — the first solve is already
		// asserted to match the oracle — and is not: it merely repeats the earlier
		// assertion, and it reports a piece as "changed on the second solve" when
		// nothing changed at all. That was written here, and a mutation caught it, which
		// is a fair summary of why a green test has to be shown to bite.
		TArray<FVector> FirstForces;
		FirstForces.Reserve(JointCount);
		for (int32 Index = 0; Index < JointCount; ++Index)
		{
			FirstForces.Add(Structure.GetConnectionForce(Index));
		}

		TArray<bool> bFirstSupported;
		bFirstSupported.Reserve(PieceCount);
		for (int32 Index = 0; Index < PieceCount; ++Index)
		{
			bFirstSupported.Add(Structure.IsPieceSupported(Index));
		}

		Structure.SolveLoads();

		// Bit for bit, not nearly-equal. Re-solving repeats the identical arithmetic on
		// the identical inputs, so any difference at all is a solver carrying state
		// between solves rather than rounding.
		for (int32 Index = 0; Index < JointCount; ++Index)
		{
			if (Structure.GetConnectionForce(Index) != FirstForces[Index])
			{
				Report(FString::Printf(
					TEXT("RE-SOLVE: joint %d carried %f and now carries %f"),
					Index, FirstForces[Index].Z, Structure.GetConnectionForce(Index).Z));
			}
		}

		for (int32 Index = 0; Index < PieceCount; ++Index)
		{
			if (Structure.IsPieceSupported(Index) != bFirstSupported[Index])
			{
				Report(FString::Printf(
					TEXT("RE-SOLVE: piece %d was %d on the first solve and is %d on the second"),
					Index, bFirstSupported[Index] ? 1 : 0, Structure.IsPieceSupported(Index) ? 1 : 0));
			}
		}
	}

	if (FailureCount > ReportedCount)
	{
		AddError(FString::Printf(
			TEXT("%d further failures were not reported; the first %d above are the ones to turn into named cases"),
			FailureCount - ReportedCount, ReportedCount));
	}

	const double ElapsedMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	AddInfo(FString::Printf(
		TEXT("%d seeded structures in %.0f ms: %d pieces, %d joints, %d pieces on an unroutable cycle, %d reported falling"),
		CaseCount, ElapsedMilliseconds, TotalPieces, TotalJoints, StrandedPieces, FallingPieces));

	// A generator that produced nothing would satisfy every property above in silence.
	// These are floors, not expectations: they exist to prove the fuzz ran.
	TestTrue(
		FString::Printf(TEXT("the fuzz should have built thousands of joints, got %d"), TotalJoints),
		TotalJoints > 4 * CaseCount);

	TestTrue(
		FString::Printf(TEXT("the fuzz should have produced unroutable knots, got %d"), StrandedPieces),
		StrandedPieces > 0);

	TestTrue(
		FString::Printf(TEXT("the fuzz should have produced falling pieces, got %d"), FallingPieces),
		FallingPieces > 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
