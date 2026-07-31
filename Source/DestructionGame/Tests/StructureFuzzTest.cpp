// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"
#include "Core/Profiles/ConnectionProfiles.h"

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
	 * take about 100 ms, against roughly 0.7 s for the whole suite. Raising the count is
	 * the cheapest coverage available here, but it is not free forever — if it ever
	 * starts to matter, cut the count rather than the determinism.
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
	 * "BaseSeed + index" a legitimate way to get thousands of independent cases — for
	 * both of the generators below, which is why this is phrased without a count.
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

		/**
		 * The joint's own strengths, and the name of the profile they came from.
		 *
		 * Both are left unset by GenerateCase, whose whole point is that NOTHING can
		 * give (it supplies its own unbreakable literal where it builds the structure),
		 * and are filled in by GenerateCascadeCase, where which profile a joint got is
		 * the first thing a maintainer needs to know from a failure report. A null name
		 * is what tells DescribeCase there is no profile worth printing.
		 */
		const TCHAR* ProfileName = nullptr;
		FConnectionStrength Strength;
	};

	struct FFuzzCase
	{
		TArray<FFuzzPiece> Pieces;
		TArray<FFuzzJoint> Joints;
	};

	/**
	 * THE INTERFACE NORMALS BOTH GENERATORS DRAW FROM.
	 *
	 * Shared rather than duplicated because the tier rule is the same rule in both
	 * fuzzes and a normal that stopped appearing in one of them would be coverage lost
	 * silently. The tables that DO differ between the two — masses, areas, strengths —
	 * stay local to their generator, because those differences are the point.
	 *
	 *  - BOTH SIDES OF THE 45-DEGREE LINE are represented, in the XZ and the YZ plane,
	 *    pointing up and pointing down. Every fixture in StructureTest.cpp is
	 *    axis-aligned except two, and CURRENT_STATE.md records what that concealed: an
	 *    invariant asserted over fixtures that all share a hidden property is not an
	 *    invariant.
	 *  - ONE NON-UNIT NORMAL, because AddConnection stores what it is given and only
	 *    ApplyForce normalises.
	 */
	const FVector FuzzNormals[] = {
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
				Joint.Normal = FuzzNormals[Random.NextBelow(UE_ARRAY_COUNT(FuzzNormals))];
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

			// Printed only when the generator gave the joint a profile. The load fuzz
			// makes every joint unbreakable from a literal and has nothing to say here;
			// the cascade fuzz's whole sequence turns on which profile went where.
			const FString Profile = Joint.ProfileName != nullptr
				? FString::Printf(TEXT(" %s"), Joint.ProfileName)
				: FString();

			Text += FString::Printf(TEXT(" C%d: %d->%d n=(%g,%g,%g) A=%g%s ;"),
				Index, Joint.PieceA, Joint.PieceB,
				Joint.Normal.X, Joint.Normal.Y, Joint.Normal.Z, Joint.AreaSqCm, *Profile);
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

	// ------------------------------------------------------------------------------
	// THE CASCADE FUZZ: a SECOND generator, whose joints can actually give.
	//
	// GenerateCase above deliberately cannot cascade — the structures it builds are
	// wired with an unbreakable literal, because that fuzz is about where load GOES and
	// a joint that could give would make it about something else. That is exactly why it
	// cannot exercise SolveAndBreak: no joint in twelve thousand structures can fail.
	//
	// So this is an ADDITION rather than a replacement. Both generators run, over the
	// same normals and the same topology rules; what differs is the masses, the areas
	// and — the whole point — the strengths.
	// ------------------------------------------------------------------------------

	/**
	 * THE SEED FOR THE CASCADE FUZZ, and it is deliberately nowhere near BaseSeed.
	 *
	 * Both fuzzes report a failing seed and a maintainer reproduces a structure from it,
	 * so the two seed ranges must not overlap or "seed 20260754" would name two
	 * different structures. BaseSeed spans 20260731..20272731; this one starts eight
	 * figures away and could not collide even if both case counts were raised by orders
	 * of magnitude.
	 */
	constexpr uint64 CascadeBaseSeed = 30260731ull;
	constexpr int32 CascadeCaseCount = 8000;

	/**
	 * Force, in Unreal units, that loads the given area to the given stress.
	 *
	 * Spelled out from the SI definitions rather than reusing ForceUnitsPerMPaSqCm:
	 * 1 N = 100 uu, 1 cm2 = 100 mm2, 1 MPa = 1 N/mm2, so one MPa over one cm2 is
	 * 10000 uu.
	 *
	 * BE HONEST ABOUT WHAT THAT BUYS HERE, WHICH IS LESS THAN IT BUYS ELSEWHERE. In
	 * ConnectionTest, ConnectionStrengthTest, ProfileLibraryTest and StructureCascadeTest
	 * an independent spelling is a real check — those files assert an expected value
	 * DERIVED from it, so a wrong production constant shows up as a failure rather than
	 * as an agreement. Nothing in this file asserts against a value derived from it: it
	 * only sizes generator inputs, and every property here is either universal or checked
	 * against the oracle, both of which hold whatever the conversion is. It is spelled
	 * this way for readability and for consistency with those four files, not as a guard.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	/**
	 * Mass of a piece whose whole weight loads the given area to the given stress.
	 *
	 * The generator's mass table is written in terms of STRESS for the reason
	 * StructureCascadeTest.cpp gives: stress is what decides whether a joint gives and
	 * it is the only number a reader can check against a published strength by eye.
	 * "MassForStress(1.5, 100)" says what the row is for; 1530.6 kg says nothing.
	 */
	constexpr double MassForStress(double MPa, double AreaSqCm)
	{
		return ForceForMPa(MPa, AreaSqCm) / GravityCmPerSecondSquared;
	}

	/**
	 * One random BREAKABLE structure from one seed.
	 *
	 * Same topology rules as GenerateCase — 2 to 8 pieces, the same normals, the same
	 * both-declaration-orders — because the defects this shape of test finds are
	 * topological and that generator's shape is already justified above. Three tables
	 * differ, and each difference is what makes a cascade possible:
	 *
	 *  - STRENGTHS ARE REAL PROFILES, drawn per joint from the library. Identical
	 *    profiles throughout would make a SEQUENCE far less likely: with the area-weighted
	 *    split every parallel support of one piece carries the same force PER UNIT AREA
	 *    whatever its area, so area alone cannot separate them. (Not the same as equal
	 *    UTILISATION, which the earlier wording here claimed — tilt still separates them,
	 *    because each joint resolves that identical stress onto its own normal and a head
	 *    joint's shear is judged against a different limit from a bed joint's compression.
	 *    So identical strengths would not make pass 2 impossible, only rare.) Differing
	 *    strengths are what make pass 2 reliably exist. The span is deliberate —
	 *    lime mortar crushes at 2 MPa, cement mortar at 10, dry stone at 30, and dry
	 *    stone has NO cohesion at all, so a head joint on it holds only by whatever is
	 *    squeezing it. Unbreakable is in the table on purpose: without a profile that
	 *    cannot give, a heavily loaded structure would simply come apart entirely and
	 *    there would be no survivors to check "nothing standing is over capacity" on.
	 *
	 *  - MASSES ARE STRESS-SIZED RATHER THAN BRICK-SIZED. A 2.72 kg brick on 100 cm2 is
	 *    0.0027 MPa and breaks nothing whatsoever; the band here spans from under every
	 *    BONDED profile's tensile limit (0.03 MPa) to past lime mortar's and every
	 *    fastener's compressive limit (8 MPa), so which joints give depends on where
	 *    the load ended up rather than on the generator having decided in advance.
	 *    "Bonded" is the load-bearing qualifier: DryStone's tensile strength is exactly
	 *    0.0, so ANY tension at all on a dry-stone joint is infinite utilisation and no
	 *    mass row is small enough to be under it.
	 *
	 *  - AREAS SPAN A NARROWER RANGE than the load fuzz's seven orders of magnitude.
	 *    Stress scales as one over the total supporting area, so a 1e-3 cm2 sole support
	 *    multiplies stress by 1e5 and gives under anything, and 1e4 divides it by a
	 *    hundred and gives under nothing — both ends would decide the outcome before the
	 *    profile got a say. 400x is still wide enough that an area-weighted split which
	 *    is really an even split cannot survive, and the wider range is already covered
	 *    by GenerateCase.
	 */
	FFuzzCase GenerateCascadeCase(uint64 Seed)
	{
		struct FNamedStrength
		{
			const TCHAR* Name;
			const FConnectionStrength* Strength;
		};

		static const FNamedStrength Strengths[] = {
			{ TEXT("LimeMortar"), &DestructionProfiles::LimeMortar },
			{ TEXT("GeneralPurposeMortar"), &DestructionProfiles::GeneralPurposeMortar },
			{ TEXT("DryStone"), &DestructionProfiles::DryStone },
			{ TEXT("Nail"), &DestructionProfiles::Nail },
			{ TEXT("Bolt"), &DestructionProfiles::Bolt },
			{ TEXT("Unbreakable"), &DestructionProfiles::Unbreakable },
		};

		static const double Areas[] = { 4.0, 25.0, 100.0, 400.0, 1600.0 };

		// THE LABELS ARE NOMINAL, AT 100 cm2, AND ARE NOT ROW INVARIANTS. Areas is drawn
		// independently of Masses, so the stress a row actually delivers is anywhere from
		// 25x the figure named (a 4 cm2 sole support) to a sixteenth of it (1600 cm2), and
		// further from it again once the weight is split across several supports. Each
		// comment says what the row was CHOSEN against, not what any particular joint sees.
		static const double Masses[] = {
			0.0,                          // legal, and where "supported" and "carries something" come apart
			MassForStress(0.03, 100.0),   // under every BONDED tensile limit; dry stone's is 0.0 and nothing is under that
			MassForStress(0.3, 100.0),    // past mortar's tension and cohesion, under every compressive limit
			MassForStress(1.5, 100.0),    // under lime mortar's 2 MPa crush, over nothing else
			MassForStress(8.0, 100.0),    // past lime mortar and every fastener, under cement mortar's 10
			MassForStress(25.0, 100.0),   // past cement mortar's 10, under dry stone's 30
		};

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
				Joint.Normal = FuzzNormals[Random.NextBelow(UE_ARRAY_COUNT(FuzzNormals))];
				Joint.AreaSqCm = Areas[Random.NextBelow(UE_ARRAY_COUNT(Areas))];

				const FNamedStrength& Profile = Strengths[Random.NextBelow(UE_ARRAY_COUNT(Strengths))];
				Joint.ProfileName = Profile.Name;
				Joint.Strength = *Profile.Strength;

				// WHICH END IS NAMED FIRST is varied deliberately, as in GenerateCase:
				// the stored force is the force on PieceB, so declaration order flips its
				// sign and must not flip anything else.
				const bool bDeclareLowerFirst = Random.NextChanceInThousand(500);
				Joint.PieceA = bDeclareLowerFirst ? A : B;
				Joint.PieceB = bDeclareLowerFirst ? B : A;

				Case.Joints.Add(Joint);
			}
		}

		return Case;
	}

	/**
	 * The pass argument to SurvivingGraphOf meaning "after the cascade came to rest".
	 *
	 * Every joint that ever gave is absent, whichever pass it gave in. Spelled as a
	 * pass number past the end of any cascade rather than as a second function, so the
	 * settled graph is the limiting case of the per-pass one rather than a parallel
	 * concept that could drift away from it.
	 */
	constexpr int32 SettledPass = TNumericLimits<int32>::Max();

	/**
	 * The graph production was solving at the start of a given cascade pass.
	 *
	 * THIS IS WHAT LETS THE ORACLE JUDGE A CASCADE AT ALL. DESIGN.md §3 says a joint
	 * that has given "leaves the support relation entirely... exactly as if it had never
	 * been built", and that is a statement with a checkable consequence: the structure
	 * solved at the start of pass N must carry exactly what the SAME STRUCTURE WITHOUT
	 * THE JOINTS THAT HAD ALREADY GIVEN carries. So the oracle above — least fixed
	 * point, Warshall closure, Jacobi relaxation, none of which production does — is run
	 * on this reduced graph and compared against production's answer, and no new oracle
	 * has to be written or trusted.
	 *
	 * It is also the property that SEES A PIECE WRONGLY STRANDED OR WRONGLY KEPT.
	 * Conservation cannot: over-stranding removes a piece's weight from BOTH sides of
	 * the equation, so the sum still balances while the answer is wrong — demonstrated
	 * on this very code, where a mutated cascade reported 6e6 held up and 6e6 arriving
	 * through two joints that had already broken. Support compared piece by piece
	 * against an independent answer is what catches that.
	 *
	 * THE INTERMEDIATE STATES OF A CASCADE ARE RECOVERABLE FROM OUTSIDE, and this is
	 * where that is spelled out, because it is not obvious. ConnectionBreakPass is the
	 * record: the graph production solved at the start of pass N is exactly the built
	 * graph minus every joint stamped below N, which is determined entirely by stamps a
	 * caller can already read. The rebuild reproduces production's loads BIT FOR BIT,
	 * not merely closely, and that is a property of Structure.cpp rather than luck —
	 * SolveLoads drops a given joint BEFORE the tier decision, so a latched joint and an
	 * absent one produce identical support lists, identical Loaders, identical
	 * reachability, identical Kahn order and identical area sums; and compaction here
	 * preserves index order, so those sums are added in the same sequence and round the
	 * same way. Five joints in this fuzz settle at utilisation exactly 1.0, so "the same
	 * way" is not a detail that could be waved at.
	 *
	 * Pieces are never removed, so piece handles are unchanged and only the joint
	 * indices need mapping back.
	 */
	FFuzzCase SurvivingGraphOf(
		const FFuzzCase& Case, const FStructure& Structure, int32 Pass, TArray<int32>& OutOriginalJointIndex)
	{
		FFuzzCase Reduced;
		Reduced.Pieces = Case.Pieces;
		OutOriginalJointIndex.Reset();

		for (int32 Index = 0; Index < Case.Joints.Num(); ++Index)
		{
			// BOTH ACCESSORS, deliberately, and each is load-bearing for a different
			// reason.
			//
			// HasGiven is read through GetConnection, which hands back a reference to the
			// connection the STRUCTURE owns. FConnection is copyable and its latch is
			// per-copy, so a cascade that happened only to temporaries would read as a
			// structure where nothing broke — and this reduced graph would then be the
			// whole graph, at every pass.
			//
			// The stamp then says WHEN, which is what makes this per-pass rather than only
			// settled. A joint that gave but carries no stamp (INDEX_NONE, which compares
			// below every pass number) is treated as having gone before everything: it is
			// out of the graph whichever pass is asked about, which keeps the settled call
			// exactly what it was before this function grew a pass argument.
			if (Structure.GetConnection(Index).HasGiven() && Structure.GetBreakPass(Index) < Pass)
			{
				continue;
			}

			OutOriginalJointIndex.Add(Index);
			Reduced.Joints.Add(Case.Joints[Index]);
		}

		return Reduced;
	}

	/**
	 * A generated breakable case, built into an FStructure.
	 *
	 * Shared rather than inlined because the cascade fuzz builds the SAME case twice — once
	 * to cascade and once to leave standing as an as-built reference — and two copies of
	 * the build loop is exactly the shape where the reference quietly stops being identical
	 * to the thing it is a reference for.
	 *
	 * Returns false if anything was rejected at the door. The generator only emits inputs
	 * AddPiece and AddConnection must accept, so that is a broken fixture rather than a
	 * finding, and the caller reports it as one.
	 */
	bool BuildCascadeStructure(const FFuzzCase& Case, FStructure& OutStructure)
	{
		bool bBuiltCleanly = true;

		for (int32 Index = 0; Index < Case.Pieces.Num(); ++Index)
		{
			bBuiltCleanly &=
				OutStructure.AddPiece(Case.Pieces[Index].MassKg, Case.Pieces[Index].bIsGrounded) == Index;
		}

		for (int32 Index = 0; Index < Case.Joints.Num(); ++Index)
		{
			FConnection Connection;
			Connection.PieceA = Case.Joints[Index].PieceA;
			Connection.PieceB = Case.Joints[Index].PieceB;
			Connection.InterfaceNormal = Case.Joints[Index].Normal;
			Connection.InterfaceAreaSqCm = Case.Joints[Index].AreaSqCm;

			// THE ONE LINE THAT MAKES THIS A CASCADE FUZZ: a real profile, drawn per
			// joint, so joints genuinely give and give in an order.
			Connection.Strength = Case.Joints[Index].Strength;

			bBuiltCleanly &= OutStructure.AddConnection(Connection) == Index;
		}

		return bBuiltCleanly
			&& OutStructure.NumPieces() == Case.Pieces.Num()
			&& OutStructure.NumConnections() == Case.Joints.Num();
	}

	/**
	 * What FConnection::ApplyForce WOULD have made of this force, without latching anything.
	 *
	 * MIRRORING ApplyForce's NORMALISATION EXACTLY, deliberately, and this is the one place
	 * in the file that is a transcription of production rather than an independent
	 * derivation. Both callers ask a question about the BREAK DECISION — did the cascade
	 * stop at the right moment, did it break something it should not have — so both have to
	 * measure against the same capacity the decision itself used. A recomputation that
	 * normalised differently would disagree in the last bit and report a joint at 1+1e-16
	 * as a spurious failure, and five joints in this fuzz settle at utilisation EXACTLY 1.0
	 * (LimeMortar on 400 cm2 at exactly 2.0 MPa against a 2.0 MPa limit), so one ulp of
	 * drift is not hypothetical.
	 *
	 * IF ApplyForce's NORMALISATION EVER CHANGES, CHANGE THIS TO MATCH; DO NOT RE-DERIVE IT
	 * — the same standing instruction BedJointCosine carries, and for the same reason. The
	 * lines being mirrored are Connection.cpp:36-41, and the live candidate for a change is
	 * the >1e154 overflow hole in the normal guard noted just above them at
	 * Connection.cpp:32-35: a fix there has to move here in the same commit or this file
	 * starts disagreeing with the decision it is auditing. Whether that capacity is the RIGHT number is
	 * ConnectionStrength's own tests' business, not this one's.
	 */
	double UtilisationUnder(const FConnection& Connection, const FVector& Force, FConnectionLoad& OutLoad)
	{
		FVector UnitNormal = Connection.InterfaceNormal;
		const double EffectiveAreaSqCm = UnitNormal.Normalize() ? Connection.InterfaceAreaSqCm : 0.0;

		OutLoad = DestructionForce::ClassifyForce(Force, UnitNormal);
		return DestructionForce::ComputeUtilisation(OutLoad, Connection.Strength, EffectiveAreaSqCm);
	}
}

/**
 * Twelve thousand random structures, checked against properties that must hold for any
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
 *  - SOLVING NEVER BREAKS A JOINT AND IS REPEATABLE. SolveAndBreak re-solves after every
 *    breaking pass; a solve whose answer depended on how many times it had been run would
 *    make the cascade order-dependent.
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

		// Formatted only when something has gone wrong. Twelve thousand cases times fifty
		// assertions is six hundred thousand FString::Printf calls if the message is
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

/**
 * Eight thousand random BREAKABLE structures, cascaded to a standstill and checked
 * against the properties that must hold however a structure comes apart.
 *
 * WHY A SECOND FUZZ RATHER THAN MORE CASES IN THE FIRST. Structure.Fuzz's generator
 * makes every joint unbreakable on purpose, so SolveAndBreak over it is a single pass
 * that breaks nothing — twelve thousand structures and not one cascade among them. The
 * properties above are still worth running on a structure that cannot break (they are
 * about where load goes), so this adds a generator rather than replacing one.
 *
 * WHY IT IS WORTH ITS RUNTIME. Cascade is the most topological thing this solver does
 * and it is the one that CHANGES THE GRAPH WHILE SOLVING IT. Breaking a bed joint can
 * ADD edges to the support relation — the piece falls back onto head joints that were
 * previously outranked — so a break can lengthen a load path, promote a joint into a
 * tier it was not in, and close a cycle that did not exist when the structure was
 * built. Structure.Cascade covers that with one piece on parallel bed joints and two
 * hand-built topologies. Two of this solver's real defects lived in shapes nobody would
 * write by hand.
 *
 * IT IS EXPECTED TO BE GREEN ON ARRIVAL, exactly as Structure.Fuzz was, and for the
 * same reason: the cascade is already implemented and already correct on all of this.
 * That makes it indistinguishable from a test asserting nothing until it is shown to
 * BITE, so every property below was verified by deliberately mutating Structure.cpp and
 * watching it go red. See CURRENT_STATE.md for the standing rule and for the mutations.
 *
 * WHAT IS ASSERTED, and why each earns its place:
 *
 *  - IT TERMINATES WITHIN ONE PASS PER JOINT. Joints never heal, so every counted pass
 *    permanently removes at least one connection.
 *  - EVERY COUNTED PASS BROKE SOMETHING. A gap in the numbering means a pass that broke
 *    nothing and then carried on, which is the shape of a loop that does not settle.
 *  - THE LATCH AND THE STAMP TELL ONE STORY. HasGiven says whether; GetBreakPass says
 *    when; a joint reporting one without the other is the copyable-latch defect with a
 *    bookkeeping array over the top of it.
 *  - A BROKEN JOINT CARRIES NOTHING. That zero IS redistribution: the share it used to
 *    hold has to be somewhere else now.
 *  - NO SURVIVOR IS OVER CAPACITY. The definition of settled. Written !(x <= 1) rather
 *    than x > 1 so a NaN utilisation fails here instead of reading as comfortably fine.
 *  - NOTHING BROKE THAT WAS NOT OVER CAPACITY ON THE GRAPH ITS PASS WAS SOLVED ON, and
 *    this is the only thing in either fuzz that watches the break decision in the "broke
 *    too EAGERLY" direction. Every other property here is judged on the graph MINUS the
 *    joints that gave, so it inherits production's own break set and agrees with it by
 *    construction however wrong that set is — a cascade shearing joints at a tenth of
 *    their capacity satisfies support, load, conservation and no-survivor-over-capacity
 *    alike, because each one is asked only about what is left.
 *
 *    EVERY PASS OF A CASCADE IS RECOVERABLE AFTER THE FACT, which is not obvious and is
 *    why this is one property in two halves. Pass 1's loads are the loads of the intact
 *    structure, so an identically built structure solved once reproduces them. Later
 *    passes look gone — their graph no longer exists once SolveAndBreak returns — but
 *    ConnectionBreakPass is a complete record of it: the graph solved at the start of
 *    pass N is the built graph minus every joint stamped below N, so rebuilding and
 *    solving that recovers pass N's loads bit for bit (SurvivingGraphOf says why the
 *    reproduction is exact rather than close). It leans on no lemma the file was not
 *    already leaning on — "a broken joint leaves the structure as if it had never been
 *    built" is what the LOAD property below uses on every one of the eight thousand
 *    cases. Every break is therefore guarded, 5,742 of 5,742, and the count is floored
 *    against the total so it cannot silently drop back. Shown to bite: mutating
 *    ApplyForce to give at a tenth of capacity produces failures here and NOWHERE ELSE.
 *  - GROUND-REACTION CONSERVATION against the solver's own final claim.
 *  - NO PIECE IS REPORTED HELD UP WITHOUT ITS LOAD ACTUALLY BEING ROUTED, and
 *    SUPPORTEDNESS MATCHES AN INDEPENDENT ANSWER PIECE BY PIECE. Conservation is
 *    STRUCTURALLY BLIND to over-stranding — removing a piece's weight takes it off both
 *    sides of the equation — so it must never be the only thing watching this. It was
 *    demonstrated rather than theorised: under one mutation a fixture reported 6e6 held
 *    up and 6e6 arriving, through two joints that had already broken.
 *  - THE SETTLED LOADS MATCH THE ORACLE ON THE GRAPH MINUS THE BROKEN JOINTS. DESIGN.md
 *    §3 says a joint that has given leaves the structure "exactly as if it had never
 *    been built"; that is the checkable form of it, and it reuses the independently
 *    derived oracle (least fixed point, Warshall closure, Jacobi relaxation) rather
 *    than adding a second one to trust.
 *  - CASCADING A SETTLED STRUCTURE CHANGES NOTHING. The latch being total, from outside.
 *
 * WHAT IS DELIBERATELY NOT ASSERTED:
 *
 *  - ORDER WITHIN A PASS. Every joint over capacity gives together and two joints
 *    sharing a number are unordered with respect to each other. A test that pinned it
 *    would be pinning an array traversal order.
 *  - WHICH joints break, or how many passes any particular structure takes. That is
 *    what Structure.Cascade's hand-built tables are for, where the numbers can be
 *    derived from published strengths and read by eye. Here the structure is random and
 *    only the universal shape of the answer is checkable.
 *  - THAT A JOINT BROKE IN THE EARLIEST PASS IT DESERVED TO. The sweep above is
 *    one-directional — it catches a break that was not deserved, and "no survivor is over
 *    capacity" catches a cascade that stopped a pass early, but nothing here catches a
 *    joint that gave in pass 3 having been over capacity already in pass 2's solve. The
 *    per-pass rebuild makes that reachable too (sweep every joint in the pass-N graph and
 *    require the ones over capacity to be stamped exactly N, since DESIGN.md §3 breaks
 *    ALL of them in the same pass), and it is left for its own change rather than folded
 *    in here: it is a different property, in the opposite direction, and would need its
 *    own mutation to show it bites. Meanwhile Structure.Cascade's hand-derived
 *    utilisations (a joint that must HOLD at 0.30, another at 0.60) pin the order itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCascadeFuzzTest,
	"DestructionGame.Core.Structure.CascadeFuzz",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCascadeFuzzTest::RunTest(const FString& Parameters)
{
	using namespace StructureFuzzSupport;

	const double StartSeconds = FPlatformTime::Seconds();

	int32 FailureCount = 0;
	int32 ReportedCount = 0;

	// THE BREAK-RATE DISTRIBUTION. A generator where everything breaks is as useless as
	// one where nothing does, and neither shows up as a failing property — so it is
	// measured, reported, and floored below.
	int32 TotalPieces = 0;
	int32 TotalJoints = 0;
	int32 BrokenJoints = 0;
	int32 SurvivingJoints = 0;
	int32 FallingPieces = 0;
	int32 MaxPasses = 0;
	int32 CasesStandingAsBuilt = 0;
	int32 CasesWithOnePass = 0;
	int32 CasesWithTwoOrMorePasses = 0;
	int32 CasesEntirelyBroken = 0;

	// How many breaks each half of the over-eager-break sweep actually reached — pass 1
	// against the as-built solve, later passes against the graph each was solved on.
	// Reported and floored, because a property that examines nothing passes in silence,
	// and their sum is floored against the total because "every break is guarded" is a
	// claim that should fail rather than quietly become 88% again.
	int32 FirstPassBreaks = 0;
	int32 LaterPassBreaks = 0;

	for (int32 CaseIndex = 0; CaseIndex < CascadeCaseCount; ++CaseIndex)
	{
		const uint64 Seed = CascadeBaseSeed + static_cast<uint64>(CaseIndex);
		const FFuzzCase Case = GenerateCascadeCase(Seed);

		const int32 PieceCount = Case.Pieces.Num();
		const int32 JointCount = Case.Joints.Num();

		TotalPieces += PieceCount;
		TotalJoints += JointCount;

		// Formatted only when something has gone wrong; building the message eagerly for
		// every assertion in every case is most of the runtime of a test that finds
		// nothing.
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

		// A SECOND, IDENTICAL STRUCTURE, LEFT STANDING. It is never cascaded and never
		// touched again after the single solve below, so it holds the loads the intact
		// structure carried — which is exactly what the pass-1 sweep judged its breaks
		// against. Built from the same generated case through the same builder, so
		// "identical" is structural rather than a promise.
		FStructure AsBuilt;

		if (!BuildCascadeStructure(Case, Structure) || !BuildCascadeStructure(Case, AsBuilt))
		{
			// The generator only emits inputs AddPiece and AddConnection must accept, so
			// this is a broken fixture rather than a finding.
			//
			// BOTH STRUCTURES' COUNTS, because || short-circuits: if Structure built and
			// AsBuilt did not, reporting only Structure's would read as a self-contradiction
			// — "rejected at the door, 5/5 pieces and 7/7 joints".
			Report(FString::Printf(
				TEXT("FIXTURE: the generated structure was rejected at the door, %d/%d pieces and %d/%d joints ")
				TEXT("(the as-built reference built %d pieces and %d joints)"),
				Structure.NumPieces(), PieceCount, Structure.NumConnections(), JointCount,
				AsBuilt.NumPieces(), AsBuilt.NumConnections()));
			continue;
		}

		// Non-destructive by contract and asserted so in Structure.Fuzz, so this leaves
		// AsBuilt intact and every joint of it unlatched.
		AsBuilt.SolveLoads();

		const int32 Passes = Structure.SolveAndBreak();

		MaxPasses = FMath::Max(MaxPasses, Passes);
		if (Passes == 0)
		{
			++CasesStandingAsBuilt;
		}
		else if (Passes == 1)
		{
			++CasesWithOnePass;
		}
		else
		{
			++CasesWithTwoOrMorePasses;
		}

		// TERMINATION, as a bound rather than as a hope. Joints never heal, so every
		// counted pass permanently removes at least one connection and there can be no
		// more passes than there are joints. (NOT SolveLoads' own internal fixpoint,
		// which is a much smaller thing nested inside each of these passes.)
		if (!(Passes >= 0 && Passes <= JointCount))
		{
			Report(FString::Printf(
				TEXT("TERMINATION: %d breaking passes over %d joints; the cascade must terminate within one pass per joint"),
				Passes, JointCount));
		}

		// Scale for the tolerances: the heaviest thing this structure could possibly be
		// holding up.
		double TotalWeightUU = 0.0;
		for (const FFuzzPiece& Piece : Case.Pieces)
		{
			TotalWeightUU += Piece.MassKg * GravityCmPerSecondSquared;
		}

		const double Tolerance = FMath::Max(1.0e-6, 1.0e-9 * TotalWeightUU);

		TArray<bool> bPassBrokeSomething;
		bPassBrokeSomething.Init(false, FMath::Max(Passes, 0) + 1);

		double GroundReactionUU = 0.0;
		int32 BrokenHere = 0;

		for (int32 Index = 0; Index < JointCount; ++Index)
		{
			// GetConnection hands back a reference to the connection the STRUCTURE owns,
			// so this is the real latch and not a copy of it. FConnection is copyable and
			// the latch is per-copy: a cascade that happened only to temporaries would
			// read here as a structure where nothing ever broke.
			const FConnection& Connection = Structure.GetConnection(Index);
			const FVector Force = Structure.GetConnectionForce(Index);
			const int32 BreakPass = Structure.GetBreakPass(Index);

			// THE LATCH AND THE STAMP ARE ONE ANSWER. HasGiven records only WHETHER a
			// joint gave and the stamp only WHEN; a joint with one and not the other is
			// two accessors on the same object telling different stories.
			if (Connection.HasGiven() != (BreakPass != INDEX_NONE))
			{
				Report(FString::Printf(
					TEXT("STAMP: joint %d reports HasGiven %d but break pass %d"),
					Index, Connection.HasGiven() ? 1 : 0, BreakPass));
			}

			if (BreakPass != INDEX_NONE)
			{
				++BrokenJoints;
				++BrokenHere;

				if (!(BreakPass >= 1 && BreakPass <= Passes))
				{
					Report(FString::Printf(
						TEXT("STAMP: joint %d broke in pass %d, outside the %d passes that ran"),
						Index, BreakPass, Passes));
				}
				else
				{
					bPassBrokeSomething[BreakPass] = true;
				}

				// NOTHING BREAKS IN PASS 1 THAT WAS NOT OVER CAPACITY AS BUILT — half of
				// the only assertion in either fuzz that can see an OVER-EAGER break. The
				// other half, for passes 2 and later, is after this loop, where the graph
				// each pass was solved on is rebuilt from the stamps.
				//
				// Everything else here is judged on the graph minus the joints that gave,
				// so it takes production's break set as its premise and cannot disagree
				// with it: mutate the break comparison to give at a TENTH of capacity and
				// support, load, conservation and no-survivor-over-capacity all stay green
				// on the wreckage, because each is only ever asked about what is left.
				//
				// Pass 1 is done here rather than by that rebuild because its graph is one
				// nothing has to reconstruct: the loads are the loads of the INTACT
				// structure, so AsBuilt reproduces them exactly — same graph, same
				// deterministic solve, nothing broken yet — and every joint stamped 1 must
				// have been over its own capacity there.
				//
				// Strictly greater than 1, matching ApplyForce: exactly 1.0 is fully
				// loaded and still holding, and five joints in this fuzz land there
				// exactly. Written !(x > 1.0) so a NaN utilisation reports rather than
				// reading as a break that was justified.
				if (BreakPass == 1)
				{
					++FirstPassBreaks;

					FConnectionLoad AsBuiltLoad;
					const double AsBuiltUtilisation =
						UtilisationUnder(AsBuilt.GetConnection(Index), AsBuilt.GetConnectionForce(Index), AsBuiltLoad);

					if (!(AsBuiltUtilisation > 1.0))
					{
						Report(FString::Printf(
							TEXT("BROKE UNDER CAPACITY: joint %d (%s) gave in pass 1, but as built it carried %f ")
							TEXT("as compression %f / shear %f / tension %f, only utilisation %g"),
							Index, Case.Joints[Index].ProfileName, AsBuilt.GetConnectionForce(Index).Z,
							AsBuiltLoad.Compression, AsBuiltLoad.Shear, AsBuiltLoad.Tension, AsBuiltUtilisation));
					}
				}
			}
			else
			{
				++SurvivingJoints;
			}

			if (Force.ContainsNaN()
				|| !FMath::IsFinite(Force.X) || !FMath::IsFinite(Force.Y) || !FMath::IsFinite(Force.Z))
			{
				Report(FString::Printf(TEXT("NON-FINITE: joint %d carries (%f, %f, %f)"),
					Index, Force.X, Force.Y, Force.Z));
			}

			// Gravity does not change direction because a joint is tilted, and it does not
			// acquire one because a neighbouring joint gave.
			if (!FMath::IsNearlyZero(Force.X, 1.0e-9) || !FMath::IsNearlyZero(Force.Y, 1.0e-9))
			{
				Report(FString::Printf(TEXT("NOT VERTICAL: joint %d carries (%f, %f, %f)"),
					Index, Force.X, Force.Y, Force.Z));
			}

			// A BROKEN JOINT IS OUT OF THE STRUCTURE AND CARRIES NOTHING. Not a
			// displacement claim and not a proxy for one — DESIGN.md §4 is emphatic that
			// two pieces can sever and stay resting exactly in place. This is the
			// mechanism: the share it used to hold has to be somewhere else now, and a
			// broken joint still reporting one is a wall standing on a joint it has shed.
			//
			// EXACTLY ZERO, not nearly zero. Every solve clears the force array to zero
			// and then ASSIGNS a share only along the load paths, which a given joint is
			// filtered out of, so a broken joint's force is never arithmetic on a small
			// number — it is the untouched initial value. Nearly-zero with this case's
			// tolerance would let up to 0.2 uu through for no reason.
			if (Connection.HasGiven() && Force.Z != 0.0)
			{
				Report(FString::Printf(
					TEXT("BROKEN CARRIES LOAD: joint %d gave in pass %d and still carries %f"),
					Index, BreakPass, Force.Z));
			}

			// NOTHING STANDING IS OVER ITS CAPACITY — the definition of "settled", and the
			// property that catches a cascade which stopped a pass early.
			//
			// Measured through UtilisationUnder, which mirrors ApplyForce's normalisation
			// exactly and carries the standing instruction to keep doing so.
			if (!Connection.HasGiven())
			{
				FConnectionLoad Load;
				const double Utilisation = UtilisationUnder(Connection, Force, Load);

				// Written !(x <= 1) rather than x > 1 so a NaN fails here instead of
				// slipping through as a joint that is comfortably fine.
				if (!(Utilisation <= 1.0))
				{
					Report(FString::Printf(
						TEXT("OVER CAPACITY: joint %d (%s) survived the cascade at utilisation %g, ")
						TEXT("carrying %f as compression %f / shear %f / tension %f"),
						Index, Case.Joints[Index].ProfileName, Utilisation, Force.Z,
						Load.Compression, Load.Shear, Load.Tension));
				}
			}

			if (Case.Pieces[Case.Joints[Index].PieceA].bIsGrounded
				|| Case.Pieces[Case.Joints[Index].PieceB].bIsGrounded)
			{
				GroundReactionUU += FMath::Abs(Force.Z);
			}
		}

		if (JointCount > 0 && BrokenHere == JointCount)
		{
			++CasesEntirelyBroken;
		}

		// EVERY COUNTED PASS BROKE SOMETHING. A pass that broke nothing is the last one
		// and is not counted, so a gap in the numbering means the loop carried on past
		// the point where it had settled.
		for (int32 Pass = 1; Pass <= Passes; ++Pass)
		{
			if (!bPassBrokeSomething.IsValidIndex(Pass) || !bPassBrokeSomething[Pass])
			{
				Report(FString::Printf(
					TEXT("EMPTY PASS: pass %d of %d broke no joint, so the cascade should have stopped before it"),
					Pass, Passes));
			}
		}

		// NOTHING BREAKS IN PASS 2 OR LATER THAT WAS NOT OVER CAPACITY IN THE GRAPH THAT
		// PASS WAS ACTUALLY SOLVED ON — the other half of the over-eager break question,
		// and the half that used to be recorded as unreachable from outside a production
		// seam. It is reachable, and ConnectionBreakPass is why.
		//
		// The graph production solved at the start of pass N is the built graph minus
		// every joint stamped below N, which the stamps alone determine. Rebuilding it and
		// solving it once reproduces pass N's loads bit for bit — see SurvivingGraphOf for
		// why that is a property of Structure.cpp rather than a coincidence — so the same
		// question the pass-1 sweep asks of AsBuilt can be asked of every later pass.
		//
		// This is not a new oracle and not a new assumption. The very same lemma, "a
		// broken joint leaves the structure exactly as if it had never been built", is
		// what the LOAD property below already leans on eight thousand times when it
		// compares production's settled forces against the oracle run on the graph with
		// the broken joints removed. Here it is applied at an intermediate pass instead of
		// at the end.
		//
		// Same helper, same comparison and the same label as the pass-1 sweep: strictly
		// greater than 1 to match ApplyForce, written !(x > 1.0) so a NaN utilisation
		// reports rather than reading as a break that was justified.
		for (int32 Pass = 2; Pass <= Passes; ++Pass)
		{
			TArray<int32> AtPassJointIndex;
			const FFuzzCase Reduced = SurvivingGraphOf(Case, Structure, Pass, AtPassJointIndex);

			FStructure AtPass;
			if (!BuildCascadeStructure(Reduced, AtPass))
			{
				Report(FString::Printf(
					TEXT("FIXTURE: the graph solved at the start of pass %d was rejected at the door, ")
					TEXT("%d/%d pieces and %d/%d joints"),
					Pass, AtPass.NumPieces(), Reduced.Pieces.Num(),
					AtPass.NumConnections(), Reduced.Joints.Num()));
				continue;
			}

			// One solve, no break sweep, so nothing latches and these are the loads the
			// pass-N sweep was handed.
			AtPass.SolveLoads();

			for (int32 Index = 0; Index < AtPassJointIndex.Num(); ++Index)
			{
				const int32 OriginalIndex = AtPassJointIndex[Index];

				if (Structure.GetBreakPass(OriginalIndex) != Pass)
				{
					continue;
				}

				++LaterPassBreaks;

				FConnectionLoad AtPassLoad;
				const double AtPassUtilisation =
					UtilisationUnder(AtPass.GetConnection(Index), AtPass.GetConnectionForce(Index), AtPassLoad);

				if (!(AtPassUtilisation > 1.0))
				{
					Report(FString::Printf(
						TEXT("BROKE UNDER CAPACITY: joint %d (%s) gave in pass %d, but on the graph that pass was ")
						TEXT("solved on it carried %f as compression %f / shear %f / tension %f, only utilisation %g"),
						OriginalIndex, Case.Joints[OriginalIndex].ProfileName, Pass,
						AtPass.GetConnectionForce(Index).Z,
						AtPassLoad.Compression, AtPassLoad.Shear, AtPassLoad.Tension, AtPassUtilisation));
				}
			}
		}

		// THE SETTLED GRAPH IS THE GRAPH MINUS THE JOINTS THAT GAVE, and the oracle is
		// asked about exactly that. Its answer is derived by a different algorithm all
		// the way down, so agreement is evidence rather than a transcription agreeing
		// with itself.
		TArray<int32> OriginalJointIndex;
		const FFuzzCase Settled = SurvivingGraphOf(Case, Structure, SettledPass, OriginalJointIndex);

		const FOracleResult Oracle = SolveWithOracle(Settled);
		const TArray<TArray<int32>> Supports = BuildSupports(Settled);

		// The oracle's own sanity. A relaxation that never settles, or a joint two pieces
		// both push through, means the oracle failed to strand a cycle — its answers
		// below would then be meaningless rather than merely different.
		if (!Oracle.bForcesConverged || Oracle.bJointClaimedTwice)
		{
			Report(FString::Printf(
				TEXT("ORACLE: on the settled graph, relaxation converged %d, joint claimed by two pieces %d"),
				Oracle.bForcesConverged ? 1 : 0, Oracle.bJointClaimedTwice ? 1 : 0));
		}

		double ReportedSupportedWeightUU = 0.0;

		for (int32 Index = 0; Index < PieceCount; ++Index)
		{
			const bool bProductionSupported = Structure.IsPieceSupported(Index);

			// SUPPORTEDNESS, PIECE BY PIECE, AGAINST AN INDEPENDENT ANSWER. This is the
			// property conservation cannot have: over-stranding removes a piece's weight
			// from BOTH sides of the conservation equation, so the sum still balances
			// while the answer is wrong.
			if (bProductionSupported != Oracle.bSupported[Index])
			{
				Report(FString::Printf(
					TEXT("SUPPORT: after the cascade, piece %d, production says %d, the oracle on the settled graph says %d"),
					Index, bProductionSupported ? 1 : 0, Oracle.bSupported[Index] ? 1 : 0));
			}

			if (Oracle.bStrandedInCycle[Index] && bProductionSupported)
			{
				Report(FString::Printf(
					TEXT("STRANDING: piece %d is on a cycle of the settled support relation and cannot be routed, ")
					TEXT("but production reports it held up"),
					Index));
			}

			if (!bProductionSupported)
			{
				++FallingPieces;
			}

			if (Case.Pieces[Index].bIsGrounded)
			{
				continue;
			}

			// STRANDING NEVER TRAVELS DOWNWARD, and a break must not make it start. A
			// piece resting on the earth stands up whatever is happening above it —
			// unless it is itself caught in a knot, which a break can CREATE by promoting
			// head joints into the tier a broken bed joint has vacated.
			if (!Oracle.bStrandedInCycle[Index] && !bProductionSupported)
			{
				for (const int32 JointIndex : Supports[Index])
				{
					if (Case.Pieces[OtherEndOf(Settled.Joints[JointIndex], Index)].bIsGrounded)
					{
						Report(FString::Printf(
							TEXT("DOWNWARD STRANDING: after the cascade, piece %d is supported by grounded piece %d ")
							TEXT("through surviving joint %d and is not itself in a knot, yet production reports it falling"),
							Index, OtherEndOf(Settled.Joints[JointIndex], Index), OriginalJointIndex[JointIndex]));
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

			// SUPPORTED MEANS ITS LOAD WENT SOMEWHERE, and after a cascade that is the
			// property with the most to catch: a broken joint that stops carrying load but
			// still WINS the bearing tier leaves the piece above it with an empty support
			// list, and the piece is then reported held up with every joint touching it
			// reading zero. Summed over every joint TOUCHING the piece rather than over
			// its supports, so it needs no transcription of the tier rule to stay in step
			// with — whichever subset turns out to be the supports, the total over all of
			// them is a lower bound.
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
					TEXT("UNROUTED: after the cascade, piece %d is reported held up but only %f of its %f weight ")
					TEXT("is on any joint touching it"),
					Index, TouchingUU, OwnWeightUU));
			}
		}

		// THE SURVIVING JOINTS CARRY WHAT THE ORACLE SAYS THE REDUCED GRAPH CARRIES.
		// The broken ones are covered by "a broken joint carries nothing" above and are
		// simply absent here, which is the point: DESIGN.md §3 says a joint that has
		// given leaves the structure as if it had never been built.
		for (int32 Index = 0; Index < Settled.Joints.Num(); ++Index)
		{
			const double ProductionZ = Structure.GetConnectionForce(OriginalJointIndex[Index]).Z;

			if (!FMath::IsNearlyEqual(ProductionZ, Oracle.JointForceZ[Index], Tolerance))
			{
				Report(FString::Printf(
					TEXT("LOAD: surviving joint %d (%d->%d, area %g, %s), production carries %f, ")
					TEXT("the oracle on the settled graph carries %f"),
					OriginalJointIndex[Index], Settled.Joints[Index].PieceA, Settled.Joints[Index].PieceB,
					Settled.Joints[Index].AreaSqCm, Settled.Joints[Index].ProfileName,
					ProductionZ, Oracle.JointForceZ[Index]));
			}
		}

		// GROUND-REACTION CONSERVATION, against the solver's OWN final claim about what
		// it is holding up. Everything still held up has to arrive somewhere and the only
		// somewhere is the earth. It is kept even though it cannot see over-stranding,
		// because it is the only thing that can see a share counted TWICE.
		if (!FMath::IsNearlyEqual(GroundReactionUU, ReportedSupportedWeightUU, Tolerance))
		{
			Report(FString::Printf(
				TEXT("CONSERVATION: the settled structure reports holding up %f but %f reaches the ground"),
				ReportedSupportedWeightUU, GroundReactionUU));
		}

		// IT HAS SETTLED, which is a claim about the SECOND call and not only about the
		// first. Nothing more gives, nothing un-gives, no stamp is rewritten and no load
		// moves.
		TArray<FVector> SettledForces;
		TArray<int32> SettledPasses;
		SettledForces.Reserve(JointCount);
		SettledPasses.Reserve(JointCount);
		for (int32 Index = 0; Index < JointCount; ++Index)
		{
			SettledForces.Add(Structure.GetConnectionForce(Index));
			SettledPasses.Add(Structure.GetBreakPass(Index));
		}

		TArray<bool> bSettledSupported;
		bSettledSupported.Reserve(PieceCount);
		for (int32 Index = 0; Index < PieceCount; ++Index)
		{
			bSettledSupported.Add(Structure.IsPieceSupported(Index));
		}

		const int32 SecondPasses = Structure.SolveAndBreak();

		if (SecondPasses != 0)
		{
			Report(FString::Printf(
				TEXT("NOT SETTLED: cascading a settled structure should break nothing, got %d further passes"),
				SecondPasses));
		}

		for (int32 Index = 0; Index < JointCount; ++Index)
		{
			// Bit for bit, not nearly-equal: the second run repeats the identical
			// arithmetic on identical inputs, so any difference at all is state carried
			// between runs rather than rounding.
			if (Structure.GetConnectionForce(Index) != SettledForces[Index])
			{
				Report(FString::Printf(
					TEXT("RE-CASCADE: joint %d carried %f and now carries %f"),
					Index, SettledForces[Index].Z, Structure.GetConnectionForce(Index).Z));
			}

			if (Structure.GetBreakPass(Index) != SettledPasses[Index])
			{
				Report(FString::Printf(
					TEXT("RE-CASCADE: joint %d was stamped pass %d and is now stamped %d"),
					Index, SettledPasses[Index], Structure.GetBreakPass(Index)));
			}

			if (Structure.GetConnection(Index).HasGiven() != (SettledPasses[Index] != INDEX_NONE))
			{
				Report(FString::Printf(TEXT("RE-CASCADE: joint %d un-broke on a second cascade"), Index));
			}
		}

		for (int32 Index = 0; Index < PieceCount; ++Index)
		{
			if (Structure.IsPieceSupported(Index) != bSettledSupported[Index])
			{
				Report(FString::Printf(
					TEXT("RE-CASCADE: piece %d was %d and is now %d after a second cascade"),
					Index, bSettledSupported[Index] ? 1 : 0, Structure.IsPieceSupported(Index) ? 1 : 0));
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
		TEXT("%d seeded breakable structures in %.0f ms: %d pieces, %d joints (%d broke, %d survived; ")
		TEXT("%d of the breaks were checked against the graph they were decided on, %d in pass 1 against ")
		TEXT("the as-built solve and %d in later passes against the rebuilt intermediate graph); ")
		TEXT("%d stood as built, %d took one pass, %d took two or more, deepest %d passes; ")
		TEXT("%d came apart entirely, %d pieces reported falling"),
		CascadeCaseCount, ElapsedMilliseconds, TotalPieces, TotalJoints, BrokenJoints, SurvivingJoints,
		FirstPassBreaks + LaterPassBreaks, FirstPassBreaks, LaterPassBreaks,
		CasesStandingAsBuilt, CasesWithOnePass, CasesWithTwoOrMorePasses, MaxPasses,
		CasesEntirelyBroken, FallingPieces));

	// THE GENERATOR ITSELF IS ON TRIAL HERE, and none of the properties above can put it
	// there: a generator where nothing ever breaks satisfies every one of them in
	// silence, and so does one where everything does. These are floors rather than
	// expectations — they exist to prove the fuzz generated the thing it is named for.
	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have built thousands of joints, got %d"), TotalJoints),
		TotalJoints > 4 * CascadeCaseCount);

	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have broken joints, got %d"), BrokenJoints),
		BrokenJoints > 0);

	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have left joints standing, got %d"), SurvivingJoints),
		SurvivingJoints > 0);

	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have produced structures that stand as built, got %d"),
			CasesStandingAsBuilt),
		CasesStandingAsBuilt > 0);

	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have produced falling pieces, got %d"), FallingPieces),
		FallingPieces > 0);

	// THE FLOORS UNDER THE PROPERTY THAT WATCHES THE BREAK DECISION. It examines joints
	// by their stamp, so a change that stopped producing them — or a build that stopped
	// stamping — would leave it examining nothing and passing in silence.
	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have broken joints in pass 1 to check as built, got %d"),
			FirstPassBreaks),
		FirstPassBreaks > 0);

	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have broken joints after pass 1 to check against the ")
			TEXT("graph they were decided on, got %d"),
			LaterPassBreaks),
		LaterPassBreaks > 0);

	// AND EVERY BREAK IS GUARDED, not most of them. Pass 1 is judged against the as-built
	// solve and every later pass against the graph rebuilt from the stamps below it, which
	// between them account for every stamp in [1, Passes] — so a joint that is guarded by
	// neither is a joint stamped outside the passes that ran, and this is where the "88%"
	// this fuzz used to guard fails rather than quietly returning.
	TestTrue(
		FString::Printf(TEXT("every break should have been checked against the graph it was decided on, ")
			TEXT("got %d of %d"),
			FirstPassBreaks + LaterPassBreaks, BrokenJoints),
		FirstPassBreaks + LaterPassBreaks == BrokenJoints);

	// AND THIS IS THE ONE THAT MAKES IT A CASCADE FUZZ RATHER THAN A BREAK FUZZ. A second
	// pass exists only because the first pass's breaks moved load onto a neighbour and
	// put it over capacity — redistribution actually happening, rather than a set of
	// independently overloaded joints all giving at once.
	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have produced multi-pass cascades, got %d"),
			CasesWithTwoOrMorePasses),
		CasesWithTwoOrMorePasses > 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
