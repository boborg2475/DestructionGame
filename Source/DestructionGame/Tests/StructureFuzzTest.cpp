// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Seeded structural fuzz over the load solver. Named namespace, not anonymous: other test
 * files declare their own Mortar/MakeNaN helpers, which would collide in a unity build.
 */
namespace StructureFuzzSupport
{
	/**
	 * Gravity, 980 cm/s2, spelled out so the test catches a production error. MassKg * 980 is
	 * already a force in uu; applying the 1 N = 100 uu factor again is out by 100x.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * cos(45 deg), the bed/head tier line (DESIGN.md §3). Same literal as production, bit for bit,
	 * since the tier rule is transcribed here; if production changes, copy it, do not re-derive.
	 */
	constexpr double BedJointCosine = 0.70710678118654752440;

	/**
	 * Fixed seed: the same structures every run, so a failure is reproducible. Add coverage by
	 * raising CaseCount, never with fresh randomness. Each case uses BaseSeed + index, so changing
	 * CaseCount never renumbers a case. 12,000 structures take about 100 ms.
	 */
	constexpr uint64 BaseSeed = 20260731ull;
	constexpr int32 CaseCount = 12000;

	// Caps logged failures; the total count is reported separately.
	constexpr int32 MaxReportedFailures = 12;

	/**
	 * SplitMix64 rather than FRandomStream: a documented, stable algorithm, so a seed reproduces
	 * outside the engine, and successive seeds are decorrelated. Integer draws only, so results
	 * do not depend on the compiler's floating point.
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
		 * Profile name and strengths. Set only by GenerateCascadeCase; GenerateCase leaves them
		 * unset (it builds with an unbreakable literal). A null name tells DescribeCase to skip it.
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
	 * Interface normals shared by both generators. Covers both sides of the 45-degree line in the
	 * XZ and YZ planes, up and down, since axis-aligned fixtures hid bugs before. One non-unit
	 * normal, because AddConnection stores it as given and only ApplyForce normalises.
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
	 * One random structure from one seed. Areas span seven orders of magnitude, so an even split
	 * posing as area-weighted fails. Masses include zero, where "supported" and "carries
	 * something" differ. 2 to 8 pieces: every defect found so far appeared in five or fewer.
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

				// Vary declaration order: it flips the stored force's sign and must flip nothing else.
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

			// Only the cascade fuzz assigns profiles.
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
	 * The two-tier support rule of DESIGN.md §3, as joint indices per piece. A transcription of
	 * FStructure::GetJointRole, so not a second opinion on the rule (SupportTierThreshold pins
	 * that). The oracle's independence is in what it does with the relation, not here.
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
					// A bed joint beneath bears the piece; one above holds nothing up.
					Supports[PieceIndex].Add(Index);
				}
			}

			// Head joints are only the fallback when there is no bed joint beneath.
			if (Supports[PieceIndex].Num() == 0)
			{
				Supports[PieceIndex] = MoveTemp(HeadJoints);
			}
		}

		return Supports;
	}

	struct FOracleResult
	{
		/** Whether each piece is held up, after cycles are stranded. */
		TArray<bool> bSupported;

		/** Pieces on a cycle of the support relation. */
		TArray<bool> bStrandedInCycle;

		/** Force on each joint, signed by which end is held up. */
		TArray<double> JointForceZ;

		/** Relaxation reached a fixed point; it cannot on a live cycle. */
		bool bForcesConverged = false;

		/** Two pieces pushed load through one joint: a 2-cycle survived. */
		bool bJointClaimedTwice = false;
	};

	/**
	 * Independent oracle: the same question by different algorithms, so it cannot share
	 * production's bugs. Supportedness is a least fixed point from the grounded pieces (production
	 * walks breadth-first). Cycles come from Warshall transitive closure (production walks per
	 * piece). Stranding a cycle can strand what rested on it, so both loop to a fixed point.
	 * Forces use Jacobi relaxation (production uses Kahn order); on an acyclic relation it settles
	 * bit for bit, so failing to converge means a cycle survived. Only BuildSupports is shared.
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

			/*
			 * "Pushes its load into", over supported ungrounded pieces only. Grounded pieces are
			 * sinks, so two grounded bricks naming each other via a head joint are not a loop.
			 */
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

		// Load path: supports that themselves reach the ground. Nothing is shared into a falling piece.
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

		// Jacobi relaxation: one pass per level plus one to see it settle; PieceCount + 2 is ample.
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

				// A joint's force acts on PieceB; the wrong sign turns compression into tension.
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

	/*
	 * Cascade fuzz: a second generator whose joints can give, to exercise SolveAndBreak.
	 * Same normals and topology as GenerateCase; masses, areas and strengths differ.
	 */

	/** Cascade seed, far from BaseSeed (20260731..20272731) so no seed names two structures. */
	constexpr uint64 CascadeBaseSeed = 30260731ull;
	constexpr int32 CascadeCaseCount = 8000;

	/**
	 * Force in uu that loads the area to the stress: 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa
	 * on 1 cm2 is 10000 uu. Only sizes generator inputs here, so it is not a guard on
	 * ForceUnitsPerMPaSqCm; spelled out for readability.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	/** Mass whose weight loads the area to the stress, so mass rows read against published strengths. */
	constexpr double MassForStress(double MPa, double AreaSqCm)
	{
		return ForceForMPa(MPa, AreaSqCm) / GravityCmPerSecondSquared;
	}

	/**
	 * One random breakable structure from one seed, with GenerateCase's topology rules.
	 * Strengths are real profiles per joint: area-weighted splits give parallel supports equal
	 * stress, so differing strengths are what make a second pass reliable. Unbreakable is included
	 * so heavy structures leave survivors to check. Masses are stress-sized, from 0.03 MPa (under
	 * every bonded tensile limit; DryStone's is 0) to 25 MPa. Areas span 400x, not seven orders,
	 * so the extremes do not decide the outcome before the profile does.
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

		/*
		 * Row labels are nominal, at 100 cm2. Areas are drawn independently, so a joint sees 25x
		 * to 1/16 of the named stress, less again when the weight is split.
		 */
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

				// Vary declaration order, as in GenerateCase.
				const bool bDeclareLowerFirst = Random.NextChanceInThousand(500);
				Joint.PieceA = bDeclareLowerFirst ? A : B;
				Joint.PieceB = bDeclareLowerFirst ? B : A;

				Case.Joints.Add(Joint);
			}
		}

		return Case;
	}

	/** Pass argument to SurvivingGraphOf meaning "after the cascade settled": every given joint is absent. */
	constexpr int32 SettledPass = TNumericLimits<int32>::Max();

	/**
	 * The graph production solved at the start of a cascade pass: the built graph minus every
	 * joint stamped below Pass. Per DESIGN.md §3 a given joint acts as if never built, so the
	 * oracle run on this graph must match production, piece by piece (conservation alone misses
	 * over-stranding). The match is bit for bit: SolveLoads drops given joints before the tier
	 * decision and compaction keeps index order, which matters because five joints settle at
	 * exactly 1.0. Pieces are never removed, so only joint indices are remapped.
	 */
	FFuzzCase SurvivingGraphOf(
		const FFuzzCase& Case, const FStructure& Structure, int32 Pass, TArray<int32>& OutOriginalJointIndex)
	{
		FFuzzCase Reduced;
		Reduced.Pieces = Case.Pieces;
		OutOriginalJointIndex.Reset();

		for (int32 Index = 0; Index < Case.Joints.Num(); ++Index)
		{
			/*
			 * HasGiven via GetConnection reads the structure's own copy; the latch is per-copy, so
			 * breaks on temporaries would be invisible. The stamp says when; an unstamped given
			 * joint (INDEX_NONE) counts as gone before every pass.
			 */
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
	 * Builds a breakable case into an FStructure. Shared so the cascaded copy and the as-built
	 * reference cannot drift. Returns false on any rejection, which is a fixture bug.
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

			Connection.Strength = Case.Joints[Index].Strength;

			bBuiltCleanly &= OutStructure.AddConnection(Connection) == Index;
		}

		return bBuiltCleanly
			&& OutStructure.NumPieces() == Case.Pieces.Num()
			&& OutStructure.NumConnections() == Case.Joints.Num();
	}

	/**
	 * Utilisation ApplyForce would compute, without latching. Transcribes ApplyForce's
	 * normalisation (Connection.cpp) exactly, because callers audit the break decision and five
	 * joints settle at exactly 1.0 (LimeMortar, 400 cm2, 2.0 MPa), so one ulp of drift fails.
	 * If that normalisation changes (e.g. the >1e154 overflow fix), change this in the same commit.
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
 * 12,000 random structures checked against universal properties and an independent oracle.
 * Random graphs found defects hand-written fixtures missed (foundation stranding: 14 of 8,500).
 * Green on arrival, not a red step; it earns its place by failing under deliberate solver
 * mutations, so re-check that with a mutation after changing it.
 *
 * Asserted: ground reaction equals the weight of the pieces reported held up; every force is
 * finite (FMath::Max/Min hide NaN); a supported piece's load is actually routed (weak lower
 * bound, plus per-joint oracle comparison, since conservation misses over-stranding); stranding
 * never travels down to a piece resting on the ground (DESIGN.md §3); support and cycle sets
 * match the oracle; solving breaks nothing and is repeatable.
 *
 * Not asserted: "no joint in tension" (a tilted head-tier face above a piece can hold it in
 * tension, per DESIGN.md §3), and production's internal stranded set, which is only observable
 * through IsPieceSupported.
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

		// Messages are formatted only on failure; eager formatting dominates the runtime.
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

			// Unbreakable: this fuzz is about where load goes, not what gives.
			Connection.Strength = FConnectionStrength{ 1.0e9, 1.0e9, 1.0e9, 0.0 };

			bBuiltCleanly &= Structure.AddConnection(Connection) == Index;
		}

		if (!bBuiltCleanly || Structure.NumPieces() != PieceCount || Structure.NumConnections() != JointCount)
		{
			// The generator emits only valid inputs, so this is a fixture bug, not a finding.
			Report(FString::Printf(
				TEXT("FIXTURE: the generated structure was rejected at the door, %d/%d pieces and %d/%d joints"),
				Structure.NumPieces(), PieceCount, Structure.NumConnections(), JointCount));
			continue;
		}

		Structure.SolveLoads();

		const FOracleResult Oracle = SolveWithOracle(Case);
		const TArray<TArray<int32>> Supports = BuildSupports(Case);

		// Oracle sanity: either flag means it failed to strand a cycle and its answers are meaningless.
		if (!Oracle.bForcesConverged || Oracle.bJointClaimedTwice)
		{
			Report(FString::Printf(
				TEXT("ORACLE: relaxation converged %d, joint claimed by two pieces %d"),
				Oracle.bForcesConverged ? 1 : 0, Oracle.bJointClaimedTwice ? 1 : 0));
		}

		// Tolerance scales with total weight; areas span seven orders of magnitude.
		double TotalWeightUU = 0.0;
		for (const FFuzzPiece& Piece : Case.Pieces)
		{
			TotalWeightUU += Piece.MassKg * StructureFuzzSupport::GravityCmPerSecondSquared;
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

			/*
			 * Stranding never travels downward: a piece resting on the ground stands, unless it
			 * is itself on a cycle through another support.
			 */
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

			const double OwnWeightUU = Case.Pieces[Index].MassKg * StructureFuzzSupport::GravityCmPerSecondSquared;
			ReportedSupportedWeightUU += OwnWeightUU;

			/*
			 * A supported piece's weight must reach its joints. Summed over every touching joint,
			 * which is a lower bound needing no tier rule.
			 */
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

			// Forces stay vertical on tilted joints; FConnection resolves them against the normal.
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

		// Ground reaction must equal the weight the solver claims to hold up.
		if (!FMath::IsNearlyEqual(GroundReactionUU, ReportedSupportedWeightUU, Tolerance))
		{
			Report(FString::Printf(
				TEXT("CONSERVATION: the solver reports holding up %f but %f reaches the ground"),
				ReportedSupportedWeightUU, GroundReactionUU));
		}

		/*
		 * Repeatability: a second solve must match the first (not the oracle, which would only
		 * repeat the earlier assertion). SolveAndBreak re-solves after every break.
		 */
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

		// Bit for bit: identical arithmetic on identical inputs, so any difference is leaked state.
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

	// Floors proving the fuzz generated something; an empty generator passes every property.
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
 * 8,000 random breakable structures, cascaded to rest and checked against universal
 * properties. Structure.Fuzz cannot cascade (unbreakable joints), and breaks change the graph
 * mid-solve: a lost bed joint can promote head joints and close new cycles. Green on arrival;
 * each property was shown to fail under a deliberate mutation (see CURRENT_STATE.md).
 *
 * Asserted: termination within one pass per joint; every counted pass broke something; latch
 * and stamp agree; a broken joint carries exactly zero; no survivor is over capacity; nothing
 * broke that was not over capacity on the graph its pass solved (the only over-eager-break
 * check, since every other property judges only what is left; later passes are rebuilt from
 * the stamps, see SurvivingGraphOf); ground-reaction conservation; support and settled loads
 * match the oracle on the graph minus broken joints; re-cascading changes nothing.
 *
 * Not asserted: order within a pass, which joints break or how many passes (Structure.Cascade
 * covers that), or that each joint broke in the earliest pass it could have. That last check
 * is reachable via the per-pass rebuild and is left for its own change.
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

	// Break-rate distribution, floored below: all-break and no-break generators are equally useless.
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

	// Breaks checked by each half of the over-eager sweep; their sum must cover every break.
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

		// Messages are formatted only on failure.
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

		// Identical copy, solved once and never cascaded: the intact loads pass 1 judged against.
		FStructure AsBuilt;

		if (!BuildCascadeStructure(Case, Structure) || !BuildCascadeStructure(Case, AsBuilt))
		{
			/*
			 * A fixture bug, not a finding. Reports both structures' counts since || short-circuits
			 * and either one may be the one that failed.
			 */
			Report(FString::Printf(
				TEXT("FIXTURE: the generated structure was rejected at the door, %d/%d pieces and %d/%d joints ")
				TEXT("(the as-built reference built %d pieces and %d joints)"),
				Structure.NumPieces(), PieceCount, Structure.NumConnections(), JointCount,
				AsBuilt.NumPieces(), AsBuilt.NumConnections()));
			continue;
		}

		// Non-destructive (asserted in Structure.Fuzz), so AsBuilt stays unlatched.
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

		// Joints never heal, so each counted pass removes at least one: passes <= joints.
		if (!(Passes >= 0 && Passes <= JointCount))
		{
			Report(FString::Printf(
				TEXT("TERMINATION: %d breaking passes over %d joints; the cascade must terminate within one pass per joint"),
				Passes, JointCount));
		}

		// Tolerance scales with total weight.
		double TotalWeightUU = 0.0;
		for (const FFuzzPiece& Piece : Case.Pieces)
		{
			TotalWeightUU += Piece.MassKg * StructureFuzzSupport::GravityCmPerSecondSquared;
		}

		const double Tolerance = FMath::Max(1.0e-6, 1.0e-9 * TotalWeightUU);

		TArray<bool> bPassBrokeSomething;
		bPassBrokeSomething.Init(false, FMath::Max(Passes, 0) + 1);

		double GroundReactionUU = 0.0;
		int32 BrokenHere = 0;

		for (int32 Index = 0; Index < JointCount; ++Index)
		{
			// A reference to the structure's own connection; the latch is per-copy.
			const FConnection& Connection = Structure.GetConnection(Index);
			const FVector Force = Structure.GetConnectionForce(Index);
			const int32 BreakPass = Structure.GetBreakPass(Index);

			// HasGiven (whether) and the stamp (when) must agree.
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

				/*
				 * Over-eager break check, pass 1: a joint stamped 1 must have been over capacity
				 * on AsBuilt (later passes are checked after this loop). Strictly > 1 to match
				 * ApplyForce; written !(x > 1.0) so NaN reports.
				 */
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

			// Forces stay vertical after tilts and neighbouring breaks.
			if (!FMath::IsNearlyZero(Force.X, 1.0e-9) || !FMath::IsNearlyZero(Force.Y, 1.0e-9))
			{
				Report(FString::Printf(TEXT("NOT VERTICAL: joint %d carries (%f, %f, %f)"),
					Index, Force.X, Force.Y, Force.Z));
			}

			/*
			 * A broken joint carries exactly zero (a mechanism check, not displacement; DESIGN.md
			 * §4). Solves clear forces and assign only along load paths, so zero is exact.
			 */
			if (Connection.HasGiven() && Force.Z != 0.0)
			{
				Report(FString::Printf(
					TEXT("BROKEN CARRIES LOAD: joint %d gave in pass %d and still carries %f"),
					Index, BreakPass, Force.Z));
			}

			// Settled means no survivor is over capacity; catches a cascade that stopped early.
			if (!Connection.HasGiven())
			{
				FConnectionLoad Load;
				const double Utilisation = UtilisationUnder(Connection, Force, Load);

				// !(x <= 1) so NaN fails.
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

		// Every counted pass broke something; a gap means the loop ran past settling.
		for (int32 Pass = 1; Pass <= Passes; ++Pass)
		{
			if (!bPassBrokeSomething.IsValidIndex(Pass) || !bPassBrokeSomething[Pass])
			{
				Report(FString::Printf(
					TEXT("EMPTY PASS: pass %d of %d broke no joint, so the cascade should have stopped before it"),
					Pass, Passes));
			}
		}

		/*
		 * Over-eager break check, passes 2+: rebuild the graph each pass solved (see
		 * SurvivingGraphOf, which reproduces its loads bit for bit) and require every joint
		 * stamped N to be over capacity there. Same comparison as pass 1.
		 */
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

		// The settled graph is the built graph minus given joints; the oracle judges that.
		TArray<int32> OriginalJointIndex;
		const FFuzzCase Settled = SurvivingGraphOf(Case, Structure, SettledPass, OriginalJointIndex);

		const FOracleResult Oracle = SolveWithOracle(Settled);
		const TArray<TArray<int32>> Supports = BuildSupports(Settled);

		// Oracle sanity: either flag means it failed to strand a cycle.
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

			// Per-piece support against the oracle; conservation cannot see over-stranding.
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

			/*
			 * Stranding never travels downward, unless the piece is on a cycle, which a break can
			 * create by promoting head joints.
			 */
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

			const double OwnWeightUU = Case.Pieces[Index].MassKg * StructureFuzzSupport::GravityCmPerSecondSquared;
			ReportedSupportedWeightUU += OwnWeightUU;

			/*
			 * A supported piece's weight must reach its joints; catches a broken joint that still
			 * wins the bearing tier. Summed over touching joints, a lower bound needing no tier rule.
			 */
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

		// Surviving joints carry what the oracle gives for the reduced graph (DESIGN.md §3).
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

		// Conservation cannot see over-stranding but is the only check for double-counted shares.
		if (!FMath::IsNearlyEqual(GroundReactionUU, ReportedSupportedWeightUU, Tolerance))
		{
			Report(FString::Printf(
				TEXT("CONSERVATION: the settled structure reports holding up %f but %f reaches the ground"),
				ReportedSupportedWeightUU, GroundReactionUU));
		}

		// Settled: a second cascade changes no break, stamp, load or support.
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
			// Bit for bit: any difference is state carried between runs.
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

	// Floors on the generator: all-break and no-break generators both pass every property silently.
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

	// Floors on the over-eager break check, which examines nothing if no joints are stamped.
	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have broken joints in pass 1 to check as built, got %d"),
			FirstPassBreaks),
		FirstPassBreaks > 0);

	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have broken joints after pass 1 to check against the ")
			TEXT("graph they were decided on, got %d"),
			LaterPassBreaks),
		LaterPassBreaks > 0);

	// Every break is checked, not most: the two halves cover every stamp in [1, Passes].
	TestTrue(
		FString::Printf(TEXT("every break should have been checked against the graph it was decided on, ")
			TEXT("got %d of %d"),
			FirstPassBreaks + LaterPassBreaks, BrokenJoints),
		FirstPassBreaks + LaterPassBreaks == BrokenJoints);

	// A second pass only happens when redistribution overloads a neighbour.
	TestTrue(
		FString::Printf(TEXT("the cascade fuzz should have produced multi-pass cascades, got %d"),
			CasesWithTwoOrMorePasses),
		CasesWithTwoOrMorePasses > 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
