// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Uniquely named, not anonymous: unity builds merge files into one translation unit. */
namespace StructureCascadeSupport
{
	using namespace DestructionProfiles;

	/**
	 * Unreal's gravity, 980 cm/s2, spelled out independently of production. MassKg * 980 is
	 * already a force in uu; the 1 N = 100 uu factor is baked in, not applied on top.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double WeightOf(double MassKg)
	{
		return MassKg * GravityCmPerSecondSquared;
	}

	/**
	 * Force in uu that loads an area to a stress. Spelled from SI rather than
	 * ForceUnitsPerMPaSqCm so a wrong production constant fails here: 1 MPa over 1 cm2 = 10000 uu.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	/** Inverse: stress in MPa that a force puts on an area. */
	constexpr double MPaForForce(double ForceUnits, double AreaSqCm)
	{
		return ForceUnits / (100.0 * 100.0 * AreaSqCm);
	}

	/** Mass whose weight loads the area to the stress, so fixtures read in MPa. */
	constexpr double MassForStress(double MPa, double AreaSqCm)
	{
		return ForceForMPa(MPa, AreaSqCm) / GravityCmPerSecondSquared;
	}

	/** Used only for grounded pads and piers, whose weight never reaches a joint. */
	constexpr double BrickMassKg = 2.72;

	/** Bed joint: normal points up at the piece above. */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Head joint: normal points sideways at the neighbour. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);

	/**
	 * Which single axis a joint's load falls on. ComputeUtilisation takes the worst of three
	 * axes, so each fixture loads exactly one, asserted per joint.
	 */
	enum class EJointKind : uint8
	{
		/** Vertical normal: pure compression. */
		Bed,

		/** Horizontal normal: pure shear. */
		Head,
	};

	struct FPieceSpec
	{
		double MassKg = 0.0;
		bool bIsGrounded = false;
	};

	/**
	 * A joint as data, with its own strength. The area-weighted split gives parallel supports
	 * equal stress, so differing strengths are what create a break sequence.
	 */
	struct FConnectionSpec
	{
		int32 PieceA = INDEX_NONE;
		int32 PieceB = INDEX_NONE;
		FVector Normal = FVector::ZAxisVector;
		double AreaSqCm = 100.0;
		FConnectionStrength Strength;
	};

	struct FStructureSpec
	{
		TArray<FPieceSpec> Pieces;
		TArray<FConnectionSpec> Connections;
	};

	/** What one joint must look like once the cascade has settled. */
	struct FExpectedJoint
	{
		/**
		 * Signed settled force Z, uu; zero once given. The force acts on PieceB
		 * (ConnectionLoad.h), so a joint naming the loaded piece first is positive.
		 */
		double FinalForceZUU = 0.0;

		EJointKind Kind = EJointKind::Bed;

		/** Pass that broke it, from 1, or INDEX_NONE if intact. Order within a pass is not asserted. */
		int32 BreakPass = INDEX_NONE;

		/** Settled utilisation, hand-derived in each case's comment; zero once given. */
		double FinalUtilisation = 0.0;
	};

	/** A structure, the sequence it must break in, and what is left standing. */
	struct FCascadeCase
	{
		const TCHAR* Description;
		FStructureSpec Spec;

		/** Number of passes that broke at least one joint. */
		int32 ExpectedPasses = 0;

		/** In connection-array order. */
		TArray<FExpectedJoint> ExpectedJoints;

		/** Whether each piece is still held up, in piece-array order. */
		TArray<bool> ExpectedSupported;
	};

	/** Build a structure in place (by reference: the given latch is per-copy). */
	void BuildStructure(FStructure& Out, const FStructureSpec& Spec)
	{
		for (const FPieceSpec& Piece : Spec.Pieces)
		{
			Out.AddPiece(Piece.MassKg, Piece.bIsGrounded);
		}

		for (const FConnectionSpec& Joint : Spec.Connections)
		{
			FConnection Connection;
			Connection.PieceA = Joint.PieceA;
			Connection.PieceB = Joint.PieceB;
			Connection.InterfaceNormal = Joint.Normal;
			Connection.InterfaceAreaSqCm = Joint.AreaSqCm;
			Connection.Strength = Joint.Strength;
			Out.AddConnection(Connection);
		}
	}

	/**
	 * Build, cascade, and check one case against its table and the universal properties:
	 * terminates within one pass per joint, every pass broke something, broken joints carry
	 * nothing, nothing survives over capacity, ground reaction equals supported weight, and a
	 * second call changes nothing.
	 *
	 * Assertions read through GetConnection, which returns the owned connection, so a cascade
	 * that latched only copies (`for (FConnection C : ...)`) reads as nothing broken.
	 * ClassifyForce is called directly, which is safe here: it makes no break decision and
	 * AddConnection rejects degenerate normals.
	 */
	void CheckCascadeCase(FAutomationTestBase& Test, const FCascadeCase& Case)
	{
		constexpr double Tolerance = 1.0e-6;

		/** Absolute floor, for rows expecting exactly zero. */
		constexpr double UtilisationTolerance = 1.0e-12;

		/** Relative: MassForStress divides by 980 and the solver multiplies back, so error scales with size. */
		constexpr double RelativeUtilisationTolerance = 1.0e-9;

		FStructure Structure;
		BuildStructure(Structure, Case.Spec);

		// The fixture itself first: a rejected spec would make every later assertion meaningless.
		Test.TestTrue(
			FString::Printf(TEXT("%s: expected %d pieces, got %d"),
				Case.Description, Case.Spec.Pieces.Num(), Structure.NumPieces()),
			Structure.NumPieces() == Case.Spec.Pieces.Num());

		// The table, not the spec, counts accepted joints, so a rejected joint simply has no row.
		Test.TestTrue(
			FString::Printf(TEXT("%s: the table describes %d connections and the structure accepted %d of the %d specified"),
				Case.Description, Case.ExpectedJoints.Num(),
				Structure.NumConnections(), Case.Spec.Connections.Num()),
			Case.ExpectedJoints.Num() == Structure.NumConnections());

		Test.TestTrue(
			FString::Printf(TEXT("%s: expected a row for each of %d pieces, got %d rows"),
				Case.Description, Structure.NumPieces(), Case.ExpectedSupported.Num()),
			Case.ExpectedSupported.Num() == Structure.NumPieces());

		const int32 Passes = Structure.SolveAndBreak();

		Test.TestTrue(
			FString::Printf(TEXT("%s: expected %d breaking passes, got %d"),
				Case.Description, Case.ExpectedPasses, Passes),
			Passes == Case.ExpectedPasses);

		// Each pass breaks at least one joint and joints never heal, so passes <= joints.
		Test.TestTrue(
			FString::Printf(TEXT("%s: %d passes over %d connections; the cascade must terminate within one pass per joint"),
				Case.Description, Passes, Structure.NumConnections()),
			Passes >= 0 && Passes <= Structure.NumConnections());

		for (int32 Index = 0; Index < Case.ExpectedSupported.Num(); ++Index)
		{
			Test.TestTrue(
				FString::Printf(TEXT("%s: piece %d support after the cascade, expected %d, got %d"),
					Case.Description, Index,
					Case.ExpectedSupported[Index] ? 1 : 0,
					Structure.IsPieceSupported(Index) ? 1 : 0),
				Structure.IsPieceSupported(Index) == Case.ExpectedSupported[Index]);
		}

		TArray<bool> bPassBrokeSomething;
		bPassBrokeSomething.Init(false, FMath::Max(Passes, 0) + 1);

		for (int32 Index = 0; Index < Case.ExpectedJoints.Num(); ++Index)
		{
			const FExpectedJoint& Expected = Case.ExpectedJoints[Index];

			// A reference to the owned connection: the real latch, not a copy.
			const FConnection& Connection = Structure.GetConnection(Index);
			const FVector Force = Structure.GetConnectionForce(Index);

			const bool bExpectedBroken = Expected.BreakPass != INDEX_NONE;

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d should be %s after the cascade, HasGiven reports %d"),
					Case.Description, Index,
					bExpectedBroken ? TEXT("BROKEN") : TEXT("intact"),
					Connection.HasGiven() ? 1 : 0),
				Connection.HasGiven() == bExpectedBroken);

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d should have broken in pass %d, got %d"),
					Case.Description, Index, Expected.BreakPass, Structure.GetBreakPass(Index)),
				Structure.GetBreakPass(Index) == Expected.BreakPass);

			// Latch and pass stamp must agree; a mismatch hides the copyable-latch defect.
			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d reports HasGiven %d but break pass %d"),
					Case.Description, Index, Connection.HasGiven() ? 1 : 0, Structure.GetBreakPass(Index)),
				Connection.HasGiven() == (Structure.GetBreakPass(Index) != INDEX_NONE));

			if (Structure.GetBreakPass(Index) != INDEX_NONE)
			{
				const int32 Pass = Structure.GetBreakPass(Index);

				Test.TestTrue(
					FString::Printf(TEXT("%s: connection %d broke in pass %d, outside the %d passes that ran"),
						Case.Description, Index, Pass, Passes),
					Pass >= 1 && Pass <= Passes);

				if (bPassBrokeSomething.IsValidIndex(Pass))
				{
					bPassBrokeSomething[Pass] = true;
				}
			}

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d should carry Z = %f once settled, got %f"),
					Case.Description, Index, Expected.FinalForceZUU, Force.Z),
				FMath::IsNearlyEqual(Force.Z, Expected.FinalForceZUU, Tolerance));

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d load must be vertical and finite, got (%f, %f, %f)"),
					Case.Description, Index, Force.X, Force.Y, Force.Z),
				!Force.ContainsNaN()
					&& FMath::IsFinite(Force.X) && FMath::IsFinite(Force.Y) && FMath::IsFinite(Force.Z)
					&& FMath::IsNearlyZero(Force.X, Tolerance) && FMath::IsNearlyZero(Force.Y, Tolerance));

			if (Connection.HasGiven())
			{
				Test.TestTrue(
					FString::Printf(TEXT("%s: connection %d has given and must carry nothing, got %f"),
						Case.Description, Index, Force.Z),
					FMath::IsNearlyZero(Force.Z, Tolerance));
			}

			const FConnectionLoad Load =
				DestructionForce::ClassifyForce(Force, Connection.InterfaceNormal);

			const double Magnitude = FMath::Abs(Expected.FinalForceZUU);
			const double ExpectedCompression = Expected.Kind == EJointKind::Bed ? Magnitude : 0.0;
			const double ExpectedShear = Expected.Kind == EJointKind::Bed ? 0.0 : Magnitude;

			// Exactly one axis is loaded, so utilisation comes from the intended capacity.
			Test.TestTrue(
				FString::Printf(
					TEXT("%s: connection %d should resolve to compression %f / shear %f / tension 0, got %f / %f / %f"),
					Case.Description, Index, ExpectedCompression, ExpectedShear,
					Load.Compression, Load.Shear, Load.Tension),
				FMath::IsNearlyEqual(Load.Compression, ExpectedCompression, Tolerance)
					&& FMath::IsNearlyEqual(Load.Shear, ExpectedShear, Tolerance)
					&& FMath::IsNearlyZero(Load.Tension, Tolerance));

			const double Utilisation = DestructionForce::ComputeUtilisation(
				Load, Connection.Strength, Connection.InterfaceAreaSqCm);

			/*
			 * Relative, with a floor well below the smallest non-zero expectation (Unbreakable
			 * settles at 1.96e-11). A flat 1e-9 would accept zero there.
			 */
			const double UtilisationSlack = FMath::Max(
				UtilisationTolerance, RelativeUtilisationTolerance * FMath::Abs(Expected.FinalUtilisation));

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d should settle at utilisation %.12g, got %.12g"),
					Case.Description, Index, Expected.FinalUtilisation, Utilisation),
				FMath::IsNearlyEqual(Utilisation, Expected.FinalUtilisation, UtilisationSlack));

			// Nothing survives over capacity. Written !(x <= 1) so a NaN utilisation fails.
			if (!Connection.HasGiven())
			{
				Test.TestFalse(
					FString::Printf(TEXT("%s: connection %d survived the cascade at utilisation %f, above its capacity"),
						Case.Description, Index, Utilisation),
					!(Utilisation <= 1.0));
			}
		}

		for (int32 Pass = 1; Pass <= Passes; ++Pass)
		{
			Test.TestTrue(
				FString::Printf(TEXT("%s: pass %d of %d broke no joint, so the cascade should have stopped before it"),
					Case.Description, Pass, Passes),
				bPassBrokeSomething.IsValidIndex(Pass) && bPassBrokeSomething[Pass]);
		}

		// Ground-reaction conservation, against the solver's own support claim rather than the table.
		double ReportedSupportedWeightUU = 0.0;
		for (int32 Index = 0; Index < Case.Spec.Pieces.Num(); ++Index)
		{
			if (Structure.IsPieceSupported(Index) && !Case.Spec.Pieces[Index].bIsGrounded)
			{
				ReportedSupportedWeightUU += WeightOf(Case.Spec.Pieces[Index].MassKg);
			}
		}

		double GroundReactionUU = 0.0;
		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			/*
			 * Walk the structure's joints, not the spec: a rejected spec row with an INDEX_NONE
			 * handle once crashed the harness. Grounded-ness still comes from the spec.
			 */
			const FConnection& Connection = Structure.GetConnection(Index);

			const bool bTouchesTheEarth =
				(Case.Spec.Pieces.IsValidIndex(Connection.PieceA)
					&& Case.Spec.Pieces[Connection.PieceA].bIsGrounded)
				|| (Case.Spec.Pieces.IsValidIndex(Connection.PieceB)
					&& Case.Spec.Pieces[Connection.PieceB].bIsGrounded);

			if (bTouchesTheEarth)
			{
				GroundReactionUU += FMath::Abs(Structure.GetConnectionForce(Index).Z);
			}
		}

		Test.TestTrue(
			FString::Printf(
				TEXT("%s: the settled structure reports holding up %f but %f reaches the ground"),
				Case.Description, ReportedSupportedWeightUU, GroundReactionUU),
			FMath::IsNearlyEqual(GroundReactionUU, ReportedSupportedWeightUU,
				FMath::Max(Tolerance, 1.0e-9 * ReportedSupportedWeightUU)));

		// Out-of-range handles fail closed.
		Test.TestTrue(
			FString::Printf(TEXT("%s: an unknown connection has no break pass, got %d"),
				Case.Description, Structure.GetBreakPass(Structure.NumConnections())),
			Structure.GetBreakPass(Structure.NumConnections()) == INDEX_NONE);

		Test.TestTrue(
			FString::Printf(TEXT("%s: connection INDEX_NONE has no break pass, got %d"),
				Case.Description, Structure.GetBreakPass(INDEX_NONE)),
			Structure.GetBreakPass(INDEX_NONE) == INDEX_NONE);

		// Settled means a second call changes nothing: no breaks, no un-breaks, no load moves.
		TArray<FVector> SettledForces;
		TArray<int32> SettledPasses;
		TArray<bool> bSettledSupported;

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			SettledForces.Add(Structure.GetConnectionForce(Index));
			SettledPasses.Add(Structure.GetBreakPass(Index));
		}

		for (int32 Index = 0; Index < Structure.NumPieces(); ++Index)
		{
			bSettledSupported.Add(Structure.IsPieceSupported(Index));
		}

		const int32 SecondPasses = Structure.SolveAndBreak();

		Test.TestTrue(
			FString::Printf(TEXT("%s: cascading a settled structure should break nothing, got %d further passes"),
				Case.Description, SecondPasses),
			SecondPasses == 0);

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			// Bit for bit: identical arithmetic on identical inputs, so any difference is carried state.
			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d carried %f and now carries %f"),
					Case.Description, Index, SettledForces[Index].Z, Structure.GetConnectionForce(Index).Z),
				Structure.GetConnectionForce(Index) == SettledForces[Index]);

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d was stamped pass %d and is now stamped %d"),
					Case.Description, Index, SettledPasses[Index], Structure.GetBreakPass(Index)),
				Structure.GetBreakPass(Index) == SettledPasses[Index]);

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d un-broke on a second cascade"), Case.Description, Index),
				Structure.GetConnection(Index).HasGiven() == (SettledPasses[Index] != INDEX_NONE));
		}

		for (int32 Index = 0; Index < Structure.NumPieces(); ++Index)
		{
			Test.TestTrue(
				FString::Printf(TEXT("%s: piece %d was %d and is now %d after a second cascade"),
					Case.Description, Index,
					bSettledSupported[Index] ? 1 : 0,
					Structure.IsPieceSupported(Index) ? 1 : 0),
				Structure.IsPieceSupported(Index) == bSettledSupported[Index]);
		}
	}
}

/**
 * SolveAndBreak breaks every joint over capacity in one pass, stamps the pass number,
 * re-solves, and repeats until settled. Order between passes is asserted; order within a
 * pass is not. No world: assertions are on the mechanism, never displacement (DESIGN.md §4).
 *
 * Most cases hang one heavy piece off grounded pads through pure bed joints. The
 * area-weighted split gives every support the same stress, so the sequence reads straight off
 * compressive strengths (lime 2 MPa, cement 10, dry stone 30), and losing a support raises
 * survivors' stress by an exact factor. The two head-joint cases show a break adding support
 * edges: one settles, one closes a cycle and strands both pieces.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCascadeTest,
	"DestructionGame.Core.Structure.Cascade",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCascadeTest::RunTest(const FString& Parameters)
{
	using namespace StructureCascadeSupport;

	const TArray<FCascadeCase> Cases = {
		// Control: 5 MPa on cement's 10 is half capacity, so nothing gives.
		{
			TEXT("a joint at half capacity breaks nothing and keeps carrying its load"),
			{
				{ { BrickMassKg, true }, { MassForStress(5.0, 100.0), false } },
				{ { 0, 1, BedJointNormal, 100.0, GeneralPurposeMortar } }
			},
			0,
			{ { -ForceForMPa(5.0, 100.0), EJointKind::Bed, INDEX_NONE, 0.5 } },
			{ true, true }
		},

		// Three separate pieces on lime pads at 3 MPa (1.5x): all give in pass 1, not 1/2/3.
		{
			TEXT("three independently overloaded joints all give in the same pass"),
			{
				{
					{ BrickMassKg, true }, { MassForStress(3.0, 100.0), false },
					{ BrickMassKg, true }, { MassForStress(3.0, 100.0), false },
					{ BrickMassKg, true }, { MassForStress(3.0, 100.0), false }
				},
				{
					{ 0, 1, BedJointNormal, 100.0, LimeMortar },
					{ 2, 3, BedJointNormal, 100.0, LimeMortar },
					{ 4, 5, BedJointNormal, 100.0, LimeMortar }
				}
			},
			1,
			{
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ 0.0, EJointKind::Bed, 1, 0.0 }
			},
			{ true, false, true, false, true, false }
		},

		/*
		 * One piece on a lime pad and a cement pad, 100 cm2 each, at 3 MPa. Lime gives (1.5x);
		 * cement then carries 6 MPa alone (0.60) and the structure stands.
		 */
		{
			TEXT("a joint gives, its share moves to the neighbour, and the structure stands"),
			{
				{
					{ BrickMassKg, true }, { BrickMassKg, true },
					{ MassForStress(3.0, 200.0), false }
				},
				{
					{ 0, 2, BedJointNormal, 100.0, LimeMortar },
					{ 1, 2, BedJointNormal, 100.0, GeneralPurposeMortar }
				}
			},
			1,
			{
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ -ForceForMPa(3.0, 200.0), EJointKind::Bed, INDEX_NONE, 0.6 }
			},
			{ true, true, true }
		},

		/*
		 * Lime 150 + lime 150 + cement 100 cm2 at 6 MPa. Pass 1: both lime joints give (3.0x),
		 * cement holds (0.60). Pass 2: cement alone at 24 MPa gives (2.4x).
		 */
		{
			TEXT("two joints give together, and the survivor gives on the next pass"),
			{
				{
					{ BrickMassKg, true }, { BrickMassKg, true }, { BrickMassKg, true },
					{ MassForStress(6.0, 400.0), false }
				},
				{
					{ 0, 3, BedJointNormal, 150.0, LimeMortar },
					{ 1, 3, BedJointNormal, 150.0, LimeMortar },
					{ 2, 3, BedJointNormal, 100.0, GeneralPurposeMortar }
				}
			},
			2,
			{
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ 0.0, EJointKind::Bed, 2, 0.0 }
			},
			{ true, true, true, false }
		},

		/*
		 * A break adds support edges: the share routes sideways through a head joint and down
		 * the neighbour's pad.
		 *
		 *   [pier p0] ==bolt 500== [ A p3 ] ==bolt 500== [ B p4 ]
		 *                              |                     |
		 *                          lime 100             cement 100
		 *
		 * As built the bed joints win the tier, so the plates carry zero. Lime c0 gives at
		 * 1.50x; A falls back to both plates, 1.5e6 uu each in pure shear (0.30 MPa against the
		 * bolt's flat 1.1, 0.2727). c1 then carries 4.5 MPa (0.45) and holds. c3 names the
		 * loaded piece first, so its force is positive and must still resolve to pure shear.
		 */
		{
			TEXT("a break routes a share sideways through a head joint and down the neighbour's pad"),
			{
				{
					{ BrickMassKg, true },                // 0: the pier
					{ BrickMassKg, true },                // 1: A's pad
					{ BrickMassKg, true },                // 2: B's pad
					{ MassForStress(3.0, 100.0), false }, // 3: A
					{ MassForStress(3.0, 100.0), false }  // 4: B
				},
				{
					{ 1, 3, BedJointNormal, 100.0, LimeMortar },
					{ 2, 4, BedJointNormal, 100.0, GeneralPurposeMortar },
					{ 0, 3, HeadJointNormal, 500.0, Bolt },
					{ 3, 4, HeadJointNormal, 500.0, Bolt }
				}
			},
			1,
			{
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ -ForceForMPa(4.5, 100.0), EJointKind::Bed, INDEX_NONE, 0.45 },
				{ -ForceForMPa(0.3, 500.0), EJointKind::Head, INDEX_NONE, 0.3 / 1.1 },
				{ +ForceForMPa(0.3, 500.0), EJointKind::Head, INDEX_NONE, 0.3 / 1.1 }
			},
			{ true, true, true, true, true }
		},

		/*
		 * The same fallback closing a cycle.
		 *
		 *   [pier p0] ==bolt== [ A p4 ] ==bolt== [ B p5 ] ==bolt== [pier p1]
		 *                          |                 |
		 *                      lime 100          lime 100
		 *
		 * Both pads give at 1.50x. A falls back to {c2, c3} and B to {c3, c4}; sharing c3 makes
		 * each the other's support, LoadReturnsToPiece fires, and both are stranded with every
		 * joint unloaded (DESIGN.md §3 has no rule for load round a cycle). Conservation reads
		 * 0 = 0 here, so the support flags and pass count are the real assertions.
		 */
		{
			TEXT("falling back to head joints can strand two pieces in a knot the break created"),
			{
				{
					{ BrickMassKg, true },                // 0: A's pier
					{ BrickMassKg, true },                // 1: B's pier
					{ BrickMassKg, true },                // 2: A's pad
					{ BrickMassKg, true },                // 3: B's pad
					{ MassForStress(3.0, 100.0), false }, // 4: A
					{ MassForStress(3.0, 100.0), false }  // 5: B
				},
				{
					{ 2, 4, BedJointNormal, 100.0, LimeMortar },
					{ 3, 5, BedJointNormal, 100.0, LimeMortar },
					{ 0, 4, HeadJointNormal, 500.0, Bolt },
					{ 4, 5, HeadJointNormal, 500.0, Bolt },
					{ 5, 1, HeadJointNormal, 500.0, Bolt }
				}
			},
			1,
			{
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ 0.0, EJointKind::Head, INDEX_NONE, 0.0 },
				{ 0.0, EJointKind::Head, INDEX_NONE, 0.0 },
				{ 0.0, EJointKind::Head, INDEX_NONE, 0.0 }
			},
			{ true, true, true, true, false, false }
		},

		/*
		 * Three-stage cascade, and the termination bound (three passes for three joints).
		 * Lime 250 + cement 100 + dry stone 50 cm2 at 6 MPa:
		 *
		 *   Pass 1:  6 MPa   lime 3.00x GIVES | cement 0.60 | dry 0.20
		 *   Pass 2:  16 MPa  cement 1.60x GIVES | dry 0.53
		 *   Pass 3:  48 MPa  dry 1.60x GIVES
		 *
		 * Every margin is at least 40%, so no row is a near miss.
		 */
		{
			TEXT("a three-stage cascade takes the piece down and stops of its own accord"),
			{
				{
					{ BrickMassKg, true }, { BrickMassKg, true }, { BrickMassKg, true },
					{ MassForStress(6.0, 400.0), false }
				},
				{
					{ 0, 3, BedJointNormal, 250.0, LimeMortar },
					{ 1, 3, BedJointNormal, 100.0, GeneralPurposeMortar },
					{ 2, 3, BedJointNormal, 50.0, DryStone }
				}
			},
			3,
			{
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ 0.0, EJointKind::Bed, 2, 0.0 },
				{ 0.0, EJointKind::Bed, 3, 0.0 }
			},
			{ true, true, true, false }
		},
	};

	for (const FCascadeCase& Case : Cases)
	{
		CheckCascadeCase(*this, Case);
	}

	return true;
}

/**
 * A given joint stops conducting support: out of the load path, the reachability walk, and
 * the tier decision (DESIGN.md §3: bed joints first, head joints only if none). A broken bed
 * joint that still wins the tier would leave a piece falling with its intact head joint at
 * zero. Case 1 is the fallback; case 2 is a piece whose only support has gone.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCascadeStopsConductingTest,
	"DestructionGame.Core.Structure.CascadeStopsConducting",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCascadeStopsConductingTest::RunTest(const FString& Parameters)
{
	using namespace StructureCascadeSupport;

	const TArray<FCascadeCase> Cases = {
		/*
		 * P on a 100 cm2 lime pad, bolted to a pier by a 500 cm2 plate. The pad gives at 1.50x;
		 * P falls back to the plate, which takes 3e6 uu as 0.60 MPa of shear against the bolt's
		 * flat 1.1 MPa (mu = 0): 0.5455, holds.
		 */
		{
			TEXT("a piece whose only bed joint has given falls back to its head joint"),
			{
				{
					{ BrickMassKg, true },  // 0: the pad on the earth
					{ BrickMassKg, true },  // 1: the pier on the earth
					{ MassForStress(3.0, 100.0), false } // 2: P
				},
				{
					{ 0, 2, BedJointNormal, 100.0, LimeMortar },
					{ 1, 2, HeadJointNormal, 500.0, Bolt }
				}
			},
			1,
			{
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ -ForceForMPa(3.0, 100.0), EJointKind::Head, INDEX_NONE, 0.6 / 1.1 }
			},
			{ true, true, true }
		},

		/*
		 * B on M (cement, 0.10) on a lime pad at 3 MPa (1.50x, gives). M and B are then
		 * unsupported and both joints carry zero, so the cascade stops.
		 */
		{
			TEXT("a piece supported only through a broken joint is reported unsupported"),
			{
				{
					{ BrickMassKg, true },                // 0: the pad on the earth
					{ MassForStress(2.0, 100.0), false }, // 1: M
					{ MassForStress(1.0, 100.0), false }  // 2: B, resting on M
				},
				{
					{ 0, 1, BedJointNormal, 100.0, LimeMortar },
					{ 1, 2, BedJointNormal, 100.0, GeneralPurposeMortar }
				}
			},
			1,
			{
				{ 0.0, EJointKind::Bed, 1, 0.0 },
				{ 0.0, EJointKind::Bed, INDEX_NONE, 0.0 }
			},
			{ true, false, false }
		},
	};

	for (const FCascadeCase& Case : Cases)
	{
		CheckCascadeCase(*this, Case);
	}

	return true;
}

/**
 * Degenerate shapes and their fail-closed answers, under the same universal properties: empty
 * structures and no joints, an Unbreakable joint under 20 t, a free-falling island, a piece
 * hanging under a grounded slab, a rejected joint, and a joint between two grounded pieces.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCascadeDegenerateInputTest,
	"DestructionGame.Core.Structure.CascadeDegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCascadeDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace StructureCascadeSupport;

	// 20 t over 100 cm2 is 19.6 MPa: past every real profile, nowhere near Unbreakable's 1e12.
	constexpr double AbsurdMassKg = 20000.0;
	const double UnbreakableUtilisation =
		MPaForForce(WeightOf(AbsurdMassKg), 100.0) / Unbreakable.CompressiveStrengthMPa;

	const TArray<FCascadeCase> Cases = {
		{
			TEXT("an empty structure"),
			{},
			0,
			{},
			{}
		},

		{
			TEXT("pieces with no joints between them"),
			{
				{ { BrickMassKg, true }, { MassForStress(9.0, 100.0), false } },
				{}
			},
			0,
			{},
			{ true, false }
		},

		{
			TEXT("a deliberately unbreakable joint under twenty tonnes"),
			{
				{ { BrickMassKg, true }, { AbsurdMassKg, false } },
				{ { 0, 1, BedJointNormal, 100.0, Unbreakable } }
			},
			0,
			{ { -WeightOf(AbsurdMassKg), EJointKind::Bed, INDEX_NONE, UnbreakableUtilisation } },
			{ true, true }
		},

		{
			TEXT("an island in freefall carries nothing, so nothing in it can break"),
			{
				{
					{ MassForStress(9.0, 100.0), false },
					{ MassForStress(9.0, 100.0), false }
				},
				{ { 0, 1, BedJointNormal, 100.0, LimeMortar } }
			},
			0,
			{ { 0.0, EJointKind::Bed, INDEX_NONE, 0.0 } },
			{ false, false }
		},

		{
			TEXT("a piece hanging beneath a grounded slab is held up by nothing"),
			{
				{ { BrickMassKg, true }, { MassForStress(9.0, 100.0), false } },
				{ { 1, 0, BedJointNormal, 100.0, LimeMortar } }
			},
			0,
			{ { 0.0, EJointKind::Bed, INDEX_NONE, 0.0 } },
			{ true, false }
		},

		/*
		 * AddConnection rejects a joint naming a missing piece, leaving the piece unsupported.
		 * Also guards the harness, which once read Pieces[INDEX_NONE]. The ungrounded piece is
		 * first so the || in the grounded check does not short-circuit past the bad handle.
		 */
		{
			TEXT("a joint naming a piece that does not exist is rejected at the door"),
			{
				{ { BrickMassKg, true }, { MassForStress(9.0, 100.0), false } },
				{ { 1, INDEX_NONE, BedJointNormal, 100.0, LimeMortar } }
			},
			0,
			{},
			{ true, false }
		},

		{
			TEXT("a joint between two grounded pieces has nothing to carry and nothing to break"),
			{
				{ { MassForStress(9.0, 100.0), true }, { MassForStress(9.0, 100.0), true } },
				{ { 0, 1, HeadJointNormal, 100.0, LimeMortar } }
			},
			0,
			{ { 0.0, EJointKind::Head, INDEX_NONE, 0.0 } },
			{ true, true }
		},
	};

	for (const FCascadeCase& Case : Cases)
	{
		CheckCascadeCase(*this, Case);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
