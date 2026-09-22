// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Uniquely named, not anonymous: a unity build merges translation units, so two anonymous
 * BedJointNormal declarations would collide. The `using namespace` stays inside each RunTest
 * for the same reason.
 */
namespace StructureRemovalSupport
{
	using namespace DestructionProfiles;

	/**
	 * Unreal's gravity, 980 cm/s2, spelled out rather than imported so a production error
	 * fails here. MassKg * 980 is already a force in Unreal units: the 1 N = 100 uu
	 * conversion is baked into the 980, not applied on top.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double WeightOf(double MassKg)
	{
		return MassKg * GravityCmPerSecondSquared;
	}

	/**
	 * Force, in Unreal units, that loads the given area to the given stress. Derived from SI
	 * rather than ForceUnitsPerMPaSqCm so a wrong production constant fails here: 1 MPa over
	 * 1 cm2 is 10000 uu.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	/**
	 * Mass of a piece whose weight loads the given area to the given stress. Fixtures are
	 * written in stress so expected utilisations follow by construction and survive profile
	 * retunes.
	 */
	constexpr double MassForStress(double MPa, double AreaSqCm)
	{
		return ForceForMPa(MPa, AreaSqCm) / GravityCmPerSecondSquared;
	}

	/** A standard UK metric brick, 215 x 102.5 x 65 mm, in cm3. */
	constexpr double BrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/**
	 * A standard clay brick, derived rather than hand-set (1432.44 cm3 at 1.9 g/cm3 is
	 * 2.7216 kg) so it cannot drift from the profile. No expectation here depends on it:
	 * grounded pads never load a joint, and RemovalDegenerateInputs asserts no forces.
	 *
	 * Safe at namespace scope only because ClayBrick is an aggregate of literals, so it is
	 * constant-initialised. Derive ClayBrick from another profile and static-init order could
	 * make this read zero; RemovalDegenerateInputs guards against that.
	 */
	const double BrickMassKg = ClayBrick.DensityGramsPerCubicCm * BrickVolumeCubicCm / 1000.0;

	/** Horizontal interface, normal pointing up at the piece above: a bed joint. */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Vertical interface, normal pointing sideways at the neighbour: a head joint. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);

	/**
	 * Which axis a joint carries its load on, and so which capacity its utilisation is
	 * measured against. ComputeUtilisation returns the worst of three axes, so the governing
	 * axis is asserted per joint: bed joints in pure compression, head joints in pure shear.
	 */
	enum class EJointKind : uint8
	{
		/** Substantially vertical normal: bears the load in pure compression. */
		Bed,

		/** Substantially horizontal normal: can only carry it in pure shear. */
		Head,
	};

	struct FPieceSpec
	{
		double MassKg = 0.0;
		bool bIsGrounded = false;
	};

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

	/**
	 * Build a structure in place. By reference: FConnection's "has given" latch is per-copy,
	 * so passing by value can latch a temporary and leave the real joint intact.
	 */
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

	/** What one joint must look like once the removals have been solved through. */
	struct FExpectedJoint
	{
		/**
		 * Signed Z of the joint's force, Unreal force units. Zero for a joint that has gone.
		 * The force acts on PieceB (ConnectionLoad.h), so naming the loaded piece first stores
		 * the positive reaction.
		 */
		double ForceZUU = 0.0;

		EJointKind Kind = EJointKind::Bed;

		/** Whether the joint is out of the structure, broken or removed. Separate from BreakPass. */
		bool bGone = false;

		/**
		 * Which cascade pass broke this joint, from 1, or INDEX_NONE if none did, including a
		 * joint that went with a removed piece. No sentinel: the break sequence is replayed by
		 * phase 5, and a joint that vanished with its brick did not snap. With HasGiven the
		 * encoding is complete:
		 *
		 *     intact              HasGiven false, INDEX_NONE
		 *     went with a piece   HasGiven true,  INDEX_NONE
		 *     broke in pass N     HasGiven true,  N >= 1
		 */
		int32 BreakPass = INDEX_NONE;

		/** Settled utilisation; zero for a joint that has gone. Pins a survivor's margin. */
		double Utilisation = 0.0;
	};

	/** A structure, a sequence of removals, and what must be true afterwards. */
	struct FRemovalCase
	{
		const TCHAR* Description;
		FStructureSpec Spec;

		/** Piece handles to remove, in order, before the structure is solved. */
		TArray<int32> Removals;

		/** In connection-array order. */
		TArray<FExpectedJoint> ExpectedJoints;

		/** Whether each piece is still held up, in piece-array order. */
		TArray<bool> ExpectedSupported;
	};

	/**
	 * Build, remove, solve, and check one case against its table and against properties
	 * that hold after any removal:
	 *
	 *  - NumPieces and NumConnections do not change (handles are array indices);
	 *    NumLivePieces drops by one per removal.
	 *  - Every load is finite (FMath::Max would silently discard a NaN).
	 *  - A joint gone without a pass stamp is exactly a joint touching a removed piece,
	 *    so removal is never recorded as failure.
	 *  - Any stamp other than INDEX_NONE is at least 1.
	 *  - Ground reaction equals the weight the solver claims to hold up.
	 *
	 * ClassifyForce is called directly (DESIGN.md warns about degenerate normals); safe here
	 * because it makes no break decision and AddConnection rejects degenerate normals.
	 */
	void CheckRemovalCase(FAutomationTestBase& Test, const FRemovalCase& Case)
	{
		constexpr double Tolerance = 1.0e-6;

		/** Absolute floor, for the rows whose expectation is an exact zero. */
		constexpr double UtilisationTolerance = 1.0e-12;

		/** Relative allowance: MassForStress divides by 980 and the solver multiplies back, so rounding scales with load. */
		constexpr double RelativeTolerance = 1.0e-9;

		FStructure Structure;
		BuildStructure(Structure, Case.Spec);

		Test.TestTrue(
			FString::Printf(TEXT("%s: expected %d pieces, got %d"),
				Case.Description, Case.Spec.Pieces.Num(), Structure.NumPieces()),
			Structure.NumPieces() == Case.Spec.Pieces.Num());

		Test.TestTrue(
			FString::Printf(TEXT("%s: the table describes %d connections and the structure accepted %d of the %d specified"),
				Case.Description, Case.ExpectedJoints.Num(),
				Structure.NumConnections(), Case.Spec.Connections.Num()),
			Case.ExpectedJoints.Num() == Structure.NumConnections());

		Test.TestTrue(
			FString::Printf(TEXT("%s: expected a row for each of %d pieces, got %d rows"),
				Case.Description, Structure.NumPieces(), Case.ExpectedSupported.Num()),
			Case.ExpectedSupported.Num() == Structure.NumPieces());

		const int32 PieceCountBefore = Structure.NumPieces();
		const int32 ConnectionCountBefore = Structure.NumConnections();

		for (const int32 Handle : Case.Removals)
		{
			Test.TestTrue(
				FString::Printf(TEXT("%s: removing live piece %d should report that it removed one"),
					Case.Description, Handle),
				Structure.RemovePiece(Handle));
		}

		// NumPieces is the handle range, not a live count; callers iterate 0..NumPieces.
		Test.TestTrue(
			FString::Printf(TEXT("%s: removal must not change the handle range, %d pieces before and %d after"),
				Case.Description, PieceCountBefore, Structure.NumPieces()),
			Structure.NumPieces() == PieceCountBefore);

		Test.TestTrue(
			FString::Printf(TEXT("%s: removal must not change the joint count, %d before and %d after"),
				Case.Description, ConnectionCountBefore, Structure.NumConnections()),
			Structure.NumConnections() == ConnectionCountBefore);

		const int32 ExpectedLive = PieceCountBefore - Case.Removals.Num();

		Test.TestTrue(
			FString::Printf(TEXT("%s: %d pieces less %d removed should leave %d live, got %d"),
				Case.Description, PieceCountBefore, Case.Removals.Num(),
				ExpectedLive, Structure.NumLivePieces()),
			Structure.NumLivePieces() == ExpectedLive);

		for (int32 Index = 0; Index < Structure.NumPieces(); ++Index)
		{
			const bool bExpectedRemoved = Case.Removals.Contains(Index);

			Test.TestTrue(
				FString::Printf(TEXT("%s: piece %d should be %s, IsPieceRemoved reports %d"),
					Case.Description, Index,
					bExpectedRemoved ? TEXT("REMOVED") : TEXT("live"),
					Structure.IsPieceRemoved(Index) ? 1 : 0),
				Structure.IsPieceRemoved(Index) == bExpectedRemoved);
		}

		Structure.SolveLoads();

		for (int32 Index = 0; Index < Case.ExpectedSupported.Num(); ++Index)
		{
			Test.TestTrue(
				FString::Printf(TEXT("%s: piece %d support after removal, expected %d, got %d"),
					Case.Description, Index,
					Case.ExpectedSupported[Index] ? 1 : 0,
					Structure.IsPieceSupported(Index) ? 1 : 0),
				Structure.IsPieceSupported(Index) == Case.ExpectedSupported[Index]);
		}

		for (int32 Index = 0; Index < Case.ExpectedJoints.Num(); ++Index)
		{
			const FExpectedJoint& Expected = Case.ExpectedJoints[Index];

			// A reference to the structure's connection, so this reads the real latch.
			const FConnection& Connection = Structure.GetConnection(Index);
			const FVector Force = Structure.GetConnectionForce(Index);

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d should be %s, HasGiven reports %d"),
					Case.Description, Index,
					Expected.bGone ? TEXT("GONE") : TEXT("intact"),
					Connection.HasGiven() ? 1 : 0),
				Connection.HasGiven() == Expected.bGone);

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d should carry break pass %d, got %d"),
					Case.Description, Index, Expected.BreakPass, Structure.GetBreakPass(Index)),
				Structure.GetBreakPass(Index) == Expected.BreakPass);

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d carries break pass %d, which is neither INDEX_NONE nor a pass"),
					Case.Description, Index, Structure.GetBreakPass(Index)),
				Structure.GetBreakPass(Index) == INDEX_NONE || Structure.GetBreakPass(Index) >= 1);

			// A stamped joint must be out of the structure; the reverse need not hold.
			if (Structure.GetBreakPass(Index) != INDEX_NONE)
			{
				Test.TestTrue(
					FString::Printf(TEXT("%s: connection %d is stamped pass %d but reports itself intact"),
						Case.Description, Index, Structure.GetBreakPass(Index)),
					Connection.HasGiven());
			}

			// Removal is not failure: gone-and-unstamped is exactly "touches a removed piece".
			const bool bTouchesRemovedPiece =
				Case.Removals.Contains(Connection.PieceA) || Case.Removals.Contains(Connection.PieceB);

			if (bTouchesRemovedPiece)
			{
				Test.TestTrue(
					FString::Printf(TEXT("%s: connection %d holds removed piece %d/%d and must be out of the structure"),
						Case.Description, Index, Connection.PieceA, Connection.PieceB),
					Connection.HasGiven());

				Test.TestTrue(
					FString::Printf(TEXT("%s: connection %d went with a removed piece and must carry no pass stamp, got %d"),
						Case.Description, Index, Structure.GetBreakPass(Index)),
					Structure.GetBreakPass(Index) == INDEX_NONE);
			}
			else if (Connection.HasGiven())
			{
				Test.TestTrue(
					FString::Printf(TEXT("%s: connection %d gave without a removal, so a pass must own it, got %d"),
						Case.Description, Index, Structure.GetBreakPass(Index)),
					Structure.GetBreakPass(Index) >= 1);
			}

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d should carry Z = %.12g, got %.12g"),
					Case.Description, Index, Expected.ForceZUU, Force.Z),
				FMath::IsNearlyEqual(Force.Z, Expected.ForceZUU,
					FMath::Max(Tolerance, RelativeTolerance * FMath::Abs(Expected.ForceZUU))));

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d load must be vertical and finite, got (%f, %f, %f)"),
					Case.Description, Index, Force.X, Force.Y, Force.Z),
				!Force.ContainsNaN()
					&& FMath::IsFinite(Force.X) && FMath::IsFinite(Force.Y) && FMath::IsFinite(Force.Z)
					&& FMath::IsNearlyZero(Force.X, Tolerance) && FMath::IsNearlyZero(Force.Y, Tolerance));

			const FConnectionLoad Load =
				DestructionForce::ClassifyForce(Force, Connection.InterfaceNormal);

			const double Magnitude = FMath::Abs(Expected.ForceZUU);
			const double ExpectedCompression = Expected.Kind == EJointKind::Bed ? Magnitude : 0.0;
			const double ExpectedShear = Expected.Kind == EJointKind::Bed ? 0.0 : Magnitude;

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

			const double UtilisationSlack = FMath::Max(
				UtilisationTolerance, RelativeTolerance * FMath::Abs(Expected.Utilisation));

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d should sit at utilisation %.12g, got %.12g"),
					Case.Description, Index, Expected.Utilisation, Utilisation),
				FMath::IsNearlyEqual(Utilisation, Expected.Utilisation, UtilisationSlack));
		}

		/*
		 * Ground-reaction conservation, against the solver's own claim of what it holds up.
		 * Joints on a removed grounded piece carry zero (asserted above), so they add nothing.
		 */
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
				TEXT("%s: the structure reports holding up %f but %f reaches the ground"),
				Case.Description, ReportedSupportedWeightUU, GroundReactionUU),
			FMath::IsNearlyEqual(GroundReactionUU, ReportedSupportedWeightUU,
				FMath::Max(Tolerance, RelativeTolerance * ReportedSupportedWeightUU)));
	}

	/**
	 * Check a joint's current utilisation, recomputed from outside because ApplyForce latches.
	 * Also asserts the load is pure compression, since ComputeUtilisation returns the worst
	 * of three axes and a compressive expectation means nothing unless compression governs.
	 */
	void CheckSettledCompression(
		FAutomationTestBase& Test,
		const FStructure& Structure,
		int32 Index,
		double ExpectedUtilisation,
		const TCHAR* What)
	{
		constexpr double Tolerance = 1.0e-6;
		constexpr double RelativeTolerance = 1.0e-9;

		const FConnection& Connection = Structure.GetConnection(Index);

		const FConnectionLoad Load = DestructionForce::ClassifyForce(
			Structure.GetConnectionForce(Index), Connection.InterfaceNormal);

		Test.TestTrue(
			FString::Printf(
				TEXT("%s: connection %d must be loaded in PURE COMPRESSION for its expectation to mean anything, got %f / %f / %f"),
				What, Index, Load.Compression, Load.Shear, Load.Tension),
			Load.Compression > 0.0
				&& FMath::IsNearlyZero(Load.Shear, Tolerance)
				&& FMath::IsNearlyZero(Load.Tension, Tolerance));

		const double Utilisation = DestructionForce::ComputeUtilisation(
			Load, Connection.Strength, Connection.InterfaceAreaSqCm);

		Test.TestTrue(
			FString::Printf(TEXT("%s: connection %d should sit at utilisation %.12g, got %.12g"),
				What, Index, ExpectedUtilisation, Utilisation),
			FMath::IsNearlyEqual(Utilisation, ExpectedUtilisation,
				FMath::Max(Tolerance, RelativeTolerance * ExpectedUtilisation)));
	}
}

/**
 * A removed piece leaves a hole: its slot is tombstoned and never reused, so every handle
 * keeps its meaning. Scenarios are small and rebuilt each run, so the leak is irrelevant.
 *
 * Do not add a free list: a reused slot lets a stale handle name a different live piece
 * undetectably. Slot reuse would need generational handles.
 *
 * Compaction fails silently: connections re-point at the wrong pieces, and
 * ConnectionBreakPass, which cannot be recomputed, stays wrong permanently. Both fixtures
 * remove from the middle (removing the last piece looks like compaction) and use distinct
 * masses so a shifted array reads back the wrong piece.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureRemovalKeepsIndicesStableTest,
	"DestructionGame.Core.Structure.RemovalKeepsIndicesStable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureRemovalKeepsIndicesStableTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	/*
	 * Fixture one: five pieces, the hole in the middle of the array.
	 *
	 *                [ block 4 ]
	 *                [ beam 3  ]
	 *   [pad 0]      [pad 1]      [pad 2]
	 *   ======       ======       ======
	 *
	 * Pad 1 goes. Every other piece and joint keeps its handle: connection 2 must still name
	 * pieces 2 and 3, which compaction would break either way.
	 */
	{
		struct FLabelledPiece
		{
			double MassKg;
			bool bIsGrounded;
		};

		const TArray<FLabelledPiece> Built = {
			{ 11.0, true },  // 0: left pad
			{ 22.0, true },  // 1: middle pad, REMOVED
			{ 33.0, true },  // 2: right pad
			{ 44.0, false }, // 3: beam across all three
			{ 55.0, false }  // 4: block on the beam
		};

		FStructureSpec Spec;
		for (const FLabelledPiece& Piece : Built)
		{
			Spec.Pieces.Add({ Piece.MassKg, Piece.bIsGrounded });
		}

		Spec.Connections = {
			{ 0, 3, BedJointNormal, 100.0, Unbreakable },
			{ 1, 3, BedJointNormal, 100.0, Unbreakable },
			{ 2, 3, BedJointNormal, 100.0, Unbreakable },
			{ 3, 4, BedJointNormal, 100.0, Unbreakable }
		};

		FStructure Structure;
		BuildStructure(Structure, Spec);

		TestTrue(
			FString::Printf(TEXT("the fixture should build 5 pieces and 4 joints, got %d and %d"),
				Structure.NumPieces(), Structure.NumConnections()),
			Structure.NumPieces() == 5 && Structure.NumConnections() == 4);

		TestTrue(TEXT("removing the middle pad should report that it removed a live piece"),
			Structure.RemovePiece(1));

		TestTrue(
			FString::Printf(TEXT("NumPieces is the handle RANGE and must stay 5, got %d"),
				Structure.NumPieces()),
			Structure.NumPieces() == 5);

		TestTrue(
			FString::Printf(TEXT("removal must not touch the joint array, expected 4 joints, got %d"),
				Structure.NumConnections()),
			Structure.NumConnections() == 4);

		for (int32 Index = 0; Index < Built.Num(); ++Index)
		{
			TestTrue(
				FString::Printf(TEXT("piece %d should be %s after removing piece 1, got %d"),
					Index, Index == 1 ? TEXT("REMOVED") : TEXT("live"),
					Structure.IsPieceRemoved(Index) ? 1 : 0),
				Structure.IsPieceRemoved(Index) == (Index == 1));

			if (Index == 1)
			{
				continue;
			}

			// Distinct masses make this checkable: under compaction handle 2 returns the 44 kg beam.
			TestTrue(
				FString::Printf(TEXT("piece %d should still weigh %f kg, got %f"),
					Index, Built[Index].MassKg, Structure.GetPiece(Index).MassKg),
				FMath::IsNearlyEqual(Structure.GetPiece(Index).MassKg, Built[Index].MassKg, 1.0e-9));

			TestTrue(
				FString::Printf(TEXT("piece %d should still be %s"),
					Index, Built[Index].bIsGrounded ? TEXT("grounded") : TEXT("ungrounded")),
				Structure.GetPiece(Index).bIsGrounded == Built[Index].bIsGrounded);

			TestTrue(
				FString::Printf(TEXT("piece %d should still record its own index as %d, got %d"),
					Index, Index, Structure.GetPiece(Index).Index),
				Structure.GetPiece(Index).Index == Index);
		}

		for (int32 Index = 0; Index < Spec.Connections.Num(); ++Index)
		{
			const FConnectionSpec& AsBuilt = Spec.Connections[Index];
			const FConnection& Stored = Structure.GetConnection(Index);

			TestTrue(
				FString::Printf(TEXT("connection %d was built between pieces %d and %d and now names %d and %d"),
					Index, AsBuilt.PieceA, AsBuilt.PieceB, Stored.PieceA, Stored.PieceB),
				Stored.PieceA == AsBuilt.PieceA && Stored.PieceB == AsBuilt.PieceB);

			TestTrue(
				FString::Printf(TEXT("connection %d should keep its %f cm2 interface, got %f"),
					Index, AsBuilt.AreaSqCm, Stored.InterfaceAreaSqCm),
				FMath::IsNearlyEqual(Stored.InterfaceAreaSqCm, AsBuilt.AreaSqCm, 1.0e-9));
		}
	}

	/*
	 * Fixture two: the break-pass array cannot heal. A cascade stamps a joint, then a piece
	 * whose joint sits at a lower index is removed. Deleting that joint would shift the lime
	 * joint's pass-1 stamp onto the cement joint permanently.
	 *
	 *   [ light 2 ]                    [ block 4 ]
	 *   [ pad 1   ]  REMOVED       [pad 0]     [pad 3]
	 *   ==========                 ======      ======
	 *      c0                       c1 lime     c2 cement
	 *
	 * Pieces 1/2 are a separate stack. The lime joint breaks in pass 1 (3 MPa against 2); the
	 * cement joint then carries 6 MPa against 10 and holds. Removing pad 1 must change neither.
	 */
	{
		const FStructureSpec Spec = {
			{
				{ BrickMassKg, true },                  // 0: pad under the lime joint
				{ BrickMassKg, true },                  // 1: pad of the separate stack, REMOVED
				{ MassForStress(0.5, 100.0), false },   // 2: the light piece on it
				{ BrickMassKg, true },                  // 3: pad under the cement joint
				{ MassForStress(3.0, 200.0), false }    // 4: the loaded block
			},
			{
				{ 1, 2, BedJointNormal, 100.0, Unbreakable },
				{ 0, 4, BedJointNormal, 100.0, LimeMortar },
				{ 3, 4, BedJointNormal, 100.0, GeneralPurposeMortar }
			}
		};

		FStructure Structure;
		BuildStructure(Structure, Spec);

		const int32 Passes = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("the lime joint should give in one breaking pass, got %d passes"), Passes),
			Passes == 1);

		TestTrue(
			FString::Printf(TEXT("the lime joint should be stamped pass 1 before any removal, got %d"),
				Structure.GetBreakPass(1)),
			Structure.GetBreakPass(1) == 1);

		TestTrue(TEXT("removing the unrelated pad should report that it removed a live piece"),
			Structure.RemovePiece(1));

		TestTrue(
			FString::Printf(TEXT("removal must not change the joint count, expected 3, got %d"),
				Structure.NumConnections()),
			Structure.NumConnections() == 3);

		// A later removal must not restamp history.
		TestTrue(
			FString::Printf(TEXT("the lime joint should still be stamped pass 1 after the removal, got %d"),
				Structure.GetBreakPass(1)),
			Structure.GetBreakPass(1) == 1);

		TestTrue(
			FString::Printf(TEXT("the cement joint should still be intact and unstamped, got %d"),
				Structure.GetBreakPass(2)),
			Structure.GetBreakPass(2) == INDEX_NONE
				&& !Structure.GetConnection(2).HasGiven());

		/*
		 * All three states at once, separable by (HasGiven, GetBreakPass): connection 0 went
		 * with its pad, 1 broke in pass 1, 2 still holds.
		 */
		TestTrue(
			FString::Printf(TEXT("the removed pad's joint should be out of the structure, HasGiven reports %d"),
				Structure.GetConnection(0).HasGiven() ? 1 : 0),
			Structure.GetConnection(0).HasGiven());

		TestTrue(
			FString::Printf(TEXT("the removed pad's joint did not FAIL, so no pass may own it, got %d"),
				Structure.GetBreakPass(0)),
			Structure.GetBreakPass(0) == INDEX_NONE);

		TestTrue(
			FString::Printf(TEXT("connection 1 was built between 0 and 4 and now names %d and %d"),
				Structure.GetConnection(1).PieceA, Structure.GetConnection(1).PieceB),
			Structure.GetConnection(1).PieceA == 0 && Structure.GetConnection(1).PieceB == 4);

		Structure.SolveLoads();

		const double SurvivorForceZUU = -ForceForMPa(3.0, 200.0);

		TestTrue(
			FString::Printf(TEXT("the cement joint should still carry %.12g after the removal, got %.12g"),
				SurvivorForceZUU, Structure.GetConnectionForce(2).Z),
			FMath::IsNearlyEqual(Structure.GetConnectionForce(2).Z, SurvivorForceZUU,
				FMath::Max(1.0e-6, 1.0e-9 * FMath::Abs(SurvivorForceZUU))));

		TestTrue(TEXT("the loaded block is still held up by the cement joint"),
			Structure.IsPieceSupported(4));

		TestTrue(TEXT("the piece that stood on the removed pad has lost its only support"),
			!Structure.IsPieceSupported(2));
	}

	return true;
}

/**
 * A removed piece is out of the graph: it supports nothing, nothing supports it, and its
 * joints go with it. Asserts on mechanism, not displacement (DESIGN.md §4). The sharp case
 * is a removed grounded piece, which must not still seed the reachability walk.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureRemovedPieceLeavesTheGraphTest,
	"DestructionGame.Core.Structure.RemovedPieceLeavesTheGraph",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureRemovedPieceLeavesTheGraphTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	/*
	 * Cement mortar, not Unbreakable: an Unbreakable joint settles near 1e-18, under the
	 * 1e-12 floor, so its utilisation row could not tell a loaded joint from a dropped one.
	 */
	const TArray<FRemovalCase> Cases = {
		/*
		 * Control: the stack with nothing removed. Each block puts 1 MPa on 100 cm2, so the
		 * lower joint is at 2 MPa (0.20 of cement's 10) and the upper at 0.10.
		 */
		{
			TEXT("a three-high stack with nothing removed stands and carries its weight"),
			{
				{
					{ BrickMassKg, true },
					{ MassForStress(1.0, 100.0), false },
					{ MassForStress(1.0, 100.0), false }
				},
				{
					{ 0, 1, BedJointNormal, 100.0, GeneralPurposeMortar },
					{ 1, 2, BedJointNormal, 100.0, GeneralPurposeMortar }
				}
			},
			{},
			{
				{ -ForceForMPa(2.0, 100.0), EJointKind::Bed, false, INDEX_NONE,0.2 },
				{ -ForceForMPa(1.0, 100.0), EJointKind::Bed, false, INDEX_NONE,0.1 }
			},
			{ true, true, true }
		},

		// The middle piece goes with both its joints, stranding the brick above.
		{
			TEXT("removing the middle of a stack takes both its joints and strands what was above"),
			{
				{
					{ BrickMassKg, true },
					{ MassForStress(1.0, 100.0), false },
					{ MassForStress(1.0, 100.0), false }
				},
				{
					{ 0, 1, BedJointNormal, 100.0, GeneralPurposeMortar },
					{ 1, 2, BedJointNormal, 100.0, GeneralPurposeMortar }
				}
			},
			{ 1 },
			{
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 },
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 }
			},
			{ true, false, false }
		},

		// A removed grounded piece is not ground.
		{
			TEXT("removing the grounded piece leaves nothing held up"),
			{
				{ { BrickMassKg, true }, { MassForStress(1.0, 100.0), false } },
				{ { 0, 1, BedJointNormal, 100.0, GeneralPurposeMortar } }
			},
			{ 0 },
			{ { 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 } },
			{ false, false }
		},

		// Removal is local: removing an unconnected piece strands nothing else.
		{
			TEXT("removing an unconnected piece changes nothing else"),
			{
				{
					{ BrickMassKg, true },
					{ MassForStress(1.0, 100.0), false },
					{ BrickMassKg, true }
				},
				{ { 0, 1, BedJointNormal, 100.0, GeneralPurposeMortar } }
			},
			{ 2 },
			{ { -ForceForMPa(1.0, 100.0), EJointKind::Bed, false, INDEX_NONE,0.1 } },
			{ true, true, false }
		},
	};

	for (const FRemovalCase& Case : Cases)
	{
		CheckRemovalCase(*this, Case);
	}

	return true;
}

/**
 * Removing a support moves its share onto the survivors. Fixtures hang a piece off parallel
 * bed-joint pads: with the area-weighted split every pad carries the same stress (total load
 * over total area), so removing one raises the survivors' stress by an exact factor. Each
 * fixture runs as a control first, then with the removal.
 *
 * The last two cases exercise the two-tier fallback: losing a bed joint promotes the head
 * joints, so a removal adds a support edge and the load moves from compression to shear.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureRemovalRedistributesLoadTest,
	"DestructionGame.Core.Structure.RemovalRedistributesLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureRemovalRedistributesLoadTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	/*
	 * One block on two 100 cm2 cement pads at 3 MPa each (0.30 of 10 MPa); with one pad gone
	 * the survivor is at 0.60 and holds.
	 */
	const FStructureSpec TwoPads = {
		{
			{ BrickMassKg, true }, { BrickMassKg, true },
			{ MassForStress(3.0, 200.0), false }
		},
		{
			{ 0, 2, BedJointNormal, 100.0, GeneralPurposeMortar },
			{ 1, 2, BedJointNormal, 100.0, GeneralPurposeMortar }
		}
	};

	// A beam on two pads with a block on it: removing a pad redistributes, removing the beam strands.
	const FStructureSpec BeamAndBlock = {
		{
			{ BrickMassKg, true }, { BrickMassKg, true },
			{ MassForStress(2.0, 100.0), false },
			{ MassForStress(2.0, 100.0), false }
		},
		{
			{ 0, 2, BedJointNormal, 100.0, GeneralPurposeMortar },
			{ 1, 2, BedJointNormal, 100.0, GeneralPurposeMortar },
			{ 2, 3, BedJointNormal, 100.0, GeneralPurposeMortar }
		}
	};

	/*
	 * One bed joint beneath, one bolted head joint beside. The bed joint wins the tier, so the
	 * plate carries zero until the pad goes, then the whole piece in pure shear.
	 */
	const FStructureSpec PadAndPlate = {
		{
			{ BrickMassKg, true }, { BrickMassKg, true },
			{ MassForStress(0.5, 100.0), false }
		},
		{
			{ 0, 2, BedJointNormal, 100.0, GeneralPurposeMortar },
			{ 1, 2, HeadJointNormal, 100.0, Bolt }
		}
	};

	const TArray<FRemovalCase> Cases = {
		// Control: 3 MPa on each cement joint, 0.30 of capacity.
		{
			TEXT("two pads at 3 MPa each share the block and both hold"),
			TwoPads,
			{},
			{
				{ -ForceForMPa(3.0, 100.0), EJointKind::Bed, false, INDEX_NONE,0.3 },
				{ -ForceForMPa(3.0, 100.0), EJointKind::Bed, false, INDEX_NONE,0.3 }
			},
			{ true, true, true }
		},

		// Half the bearing area goes; the survivor takes 6 MPa (0.60) and holds.
		{
			TEXT("removing one pad moves the whole block onto the survivor, which holds"),
			TwoPads,
			{ 0 },
			{
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 },
				{ -ForceForMPa(3.0, 200.0), EJointKind::Bed, false, INDEX_NONE,0.6 }
			},
			{ false, true, true }
		},

		// Both supports go: no load path, so both joints report zero.
		{
			TEXT("removing both pads leaves the block with no load path at all"),
			TwoPads,
			{ 0, 1 },
			{
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 },
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 }
			},
			{ false, false, false }
		},

		// Beam control: 2 MPa through the beam joint; 4e6 uu over 200 cm2 of pad is 2 MPa each.
		{
			TEXT("a beam on two pads carries the block above it and nothing gives"),
			BeamAndBlock,
			{},
			{
				{ -ForceForMPa(2.0, 100.0), EJointKind::Bed, false, INDEX_NONE,0.2 },
				{ -ForceForMPa(2.0, 100.0), EJointKind::Bed, false, INDEX_NONE,0.2 },
				{ -ForceForMPa(2.0, 100.0), EJointKind::Bed, false, INDEX_NONE,0.2 }
			},
			{ true, true, true, true }
		},

		// The surviving pad doubles to 4 MPa; the joint above stays at 2 MPa.
		{
			TEXT("removing one of the beam's pads doubles the survivor and leaves the joint above alone"),
			BeamAndBlock,
			{ 0 },
			{
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 },
				{ -ForceForMPa(4.0, 100.0), EJointKind::Bed, false, INDEX_NONE,0.4 },
				{ -ForceForMPa(2.0, 100.0), EJointKind::Bed, false, INDEX_NONE,0.2 }
			},
			{ false, true, true, true }
		},

		// Removing the beam strands the block; every joint reports zero.
		{
			TEXT("removing the beam strands the block that rested on it"),
			BeamAndBlock,
			{ 2 },
			{
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 },
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 },
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 }
			},
			{ true, true, false, false }
		},

		// Fallback control: with the pad present the plate is not a support and carries nothing.
		{
			TEXT("a bed joint beneath wins outright and the head joint carries nothing"),
			PadAndPlate,
			{},
			{
				{ -ForceForMPa(0.5, 100.0), EJointKind::Bed, false, INDEX_NONE,0.05 },
				{ 0.0, EJointKind::Head, false, INDEX_NONE,0.0 }
			},
			{ true, true, true }
		},

		/*
		 * Removal promotes the head joint: the plate takes 0.5 MPa in pure shear against the
		 * bolt's flat 1.1 MPa cohesion (mean basis), 0.4545, and holds. The bolt has zero
		 * friction, so 0.4545 can only come from the shear axis.
		 */
		{
			TEXT("removing the pad promotes the head joint, which takes the piece in shear"),
			PadAndPlate,
			{ 0 },
			{
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 },
				{ -ForceForMPa(0.5, 100.0), EJointKind::Head, false, INDEX_NONE, 0.5 / 1.1 }
			},
			{ false, true, true }
		},
	};

	for (const FRemovalCase& Case : Cases)
	{
		CheckRemovalCase(*this, Case);
	}

	return true;
}

/**
 * Removal composes with the cascade: removing a piece can overload a neighbour, and an
 * ordinary cascade follows over the remaining joints. A joint that went with a removed piece
 * stays unstamped; INDEX_NONE means "no pass broke this", not "intact" (HasGiven answers that).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureRemovalCascadesTest,
	"DestructionGame.Core.Structure.RemovalCascades",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureRemovalCascadesTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	constexpr double Tolerance = 1.0e-6;
	constexpr double UtilisationTolerance = 1.0e-9;

	/*
	 * Control: three pads at 6 MPa; remove one and the other two carry 9 MPa against cement's
	 * 10, so nothing breaks.
	 */
	{
		const FStructureSpec Spec = {
			{
				{ BrickMassKg, true }, { BrickMassKg, true }, { BrickMassKg, true },
				{ MassForStress(6.0, 300.0), false }
			},
			{
				{ 0, 3, BedJointNormal, 100.0, GeneralPurposeMortar },
				{ 1, 3, BedJointNormal, 100.0, GeneralPurposeMortar },
				{ 2, 3, BedJointNormal, 100.0, GeneralPurposeMortar }
			}
		};

		FStructure Structure;
		BuildStructure(Structure, Spec);

		// Captured first: C++ argument evaluation order is unspecified.
		const int32 PassesAsBuilt = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("three pads at 6 MPa should stand as built, got %d breaking passes"),
				PassesAsBuilt),
			PassesAsBuilt == 0);

		TestTrue(TEXT("removing one of the three pads should report that it removed a live piece"),
			Structure.RemovePiece(1));

		const int32 Passes = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("9 MPa against cement mortar's 10 should break nothing, got %d passes"),
				Passes),
			Passes == 0);

		TestTrue(TEXT("the block is still held up by the two remaining pads"),
			Structure.IsPieceSupported(3));

		const TArray<int32> Survivors = { 0, 2 };
		for (const int32 Index : Survivors)
		{
			const FVector Force = Structure.GetConnectionForce(Index);
			const double Expected = -ForceForMPa(9.0, 100.0);

			TestTrue(
				FString::Printf(TEXT("surviving pad joint %d should carry %.12g, got %.12g"),
					Index, Expected, Force.Z),
				FMath::IsNearlyEqual(Force.Z, Expected, FMath::Max(Tolerance, 1.0e-9 * FMath::Abs(Expected))));

			TestTrue(
				FString::Printf(TEXT("surviving pad joint %d should still be intact and unstamped, got %d"),
					Index, Structure.GetBreakPass(Index)),
				!Structure.GetConnection(Index).HasGiven()
					&& Structure.GetBreakPass(Index) == INDEX_NONE);
		}

		TestTrue(
			FString::Printf(TEXT("the removed pad's joint is out of the structure but unstamped, got HasGiven %d and pass %d"),
				Structure.GetConnection(1).HasGiven() ? 1 : 0, Structure.GetBreakPass(1)),
			Structure.GetConnection(1).HasGiven() && Structure.GetBreakPass(1) == INDEX_NONE);

		TestTrue(
			FString::Printf(TEXT("three pads less one removed should leave 3 live pieces, got %d"),
				Structure.NumLivePieces()),
			Structure.NumLivePieces() == 3);
	}

	/*
	 * Two pads at 6 MPa (0.60). Remove one and the survivor takes 12 MPa against 10 and gives
	 * in pass 1; only that joint carries the stamp.
	 */
	{
		const FStructureSpec Spec = {
			{
				{ BrickMassKg, true }, { BrickMassKg, true },
				{ MassForStress(6.0, 200.0), false }
			},
			{
				{ 0, 2, BedJointNormal, 100.0, GeneralPurposeMortar },
				{ 1, 2, BedJointNormal, 100.0, GeneralPurposeMortar }
			}
		};

		FStructure Structure;
		BuildStructure(Structure, Spec);

		const int32 PassesAsBuilt = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("two pads at 6 MPa should stand as built, got %d breaking passes"),
				PassesAsBuilt),
			PassesAsBuilt == 0);

		TestTrue(TEXT("removing one pad should report that it removed a live piece"),
			Structure.RemovePiece(0));

		const int32 Passes = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("the survivor at 12 MPa should give in exactly one pass, got %d"), Passes),
			Passes == 1);

		TestTrue(
			FString::Printf(TEXT("the overloaded joint should be stamped pass 1, got %d"),
				Structure.GetBreakPass(1)),
			Structure.GetBreakPass(1) == 1 && Structure.GetConnection(1).HasGiven());

		TestTrue(
			FString::Printf(TEXT("the removed pad's joint must not be stamped by any pass, got %d"),
				Structure.GetBreakPass(0)),
			Structure.GetBreakPass(0) == INDEX_NONE);

		TestTrue(TEXT("the removed pad's joint has still gone, however it went"),
			Structure.GetConnection(0).HasGiven());

		TestTrue(TEXT("the block has nothing holding it up once the survivor gives"),
			!Structure.IsPieceSupported(2));

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			TestTrue(
				FString::Printf(TEXT("connection %d has gone and must carry nothing, got %f"),
					Index, Structure.GetConnectionForce(Index).Z),
				FMath::IsNearlyZero(Structure.GetConnectionForce(Index).Z, Tolerance));
		}

		// Settled: a second cascade breaks nothing and rewrites no stamp.
		const int32 SecondPasses = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("cascading a settled structure should break nothing, got %d further passes"),
				SecondPasses),
			SecondPasses == 0);

		TestTrue(
			FString::Printf(TEXT("the stamps should be unchanged by a second cascade, got %d and %d"),
				Structure.GetBreakPass(0), Structure.GetBreakPass(1)),
			Structure.GetBreakPass(0) == INDEX_NONE
				&& Structure.GetBreakPass(1) == 1);
	}

	/*
	 * A removal after a cascade. The cement joint settles at 0.60 after the lime joint gives;
	 * removing its pad strands the block, and the two stamps still differ.
	 */
	{
		const FStructureSpec Spec = {
			{
				{ BrickMassKg, true }, { BrickMassKg, true },
				{ MassForStress(3.0, 200.0), false }
			},
			{
				{ 0, 2, BedJointNormal, 100.0, LimeMortar },
				{ 1, 2, BedJointNormal, 100.0, GeneralPurposeMortar }
			}
		};

		FStructure Structure;
		BuildStructure(Structure, Spec);

		const int32 PassesAsBuilt = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("the lime joint should give in pass 1 of %d, got a stamp of %d"),
				PassesAsBuilt, Structure.GetBreakPass(0)),
			PassesAsBuilt == 1 && Structure.GetBreakPass(0) == 1);

		const FConnectionLoad SettledLoad = DestructionForce::ClassifyForce(
			Structure.GetConnectionForce(1), Structure.GetConnection(1).InterfaceNormal);

		const double SettledUtilisation = DestructionForce::ComputeUtilisation(
			SettledLoad, Structure.GetConnection(1).Strength, Structure.GetConnection(1).InterfaceAreaSqCm);

		TestTrue(
			FString::Printf(TEXT("the cement joint should settle at 0.6, got %.12g"), SettledUtilisation),
			FMath::IsNearlyEqual(SettledUtilisation, 0.6, UtilisationTolerance));

		TestTrue(TEXT("removing the surviving pad should report that it removed a live piece"),
			Structure.RemovePiece(1));

		const int32 PassesAfterRemoval = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("a removal after a cascade should break nothing further, got %d passes"),
				PassesAfterRemoval),
			PassesAfterRemoval == 0);

		TestTrue(TEXT("the block has lost its last support"),
			!Structure.IsPieceSupported(2));

		TestTrue(
			FString::Printf(TEXT("the lime joint gave under load and must stay stamped pass 1, got %d"),
				Structure.GetBreakPass(0)),
			Structure.GetBreakPass(0) == 1);

		TestTrue(
			FString::Printf(TEXT("the cement joint went with its pad, so HasGiven %d and no pass, got %d"),
				Structure.GetConnection(1).HasGiven() ? 1 : 0, Structure.GetBreakPass(1)),
			Structure.GetConnection(1).HasGiven() && Structure.GetBreakPass(1) == INDEX_NONE);
	}

	return true;
}

/**
 * Pass stamps are global to a structure: a second SolveAndBreak continues from the highest
 * stamp already written. Consumers read a shared number as "gave simultaneously", so
 * restarting at 1 would replay the collapse in the wrong order with no other check failing.
 * Only removal between calls makes this observable.
 *
 * SolveAndBreak's return value stays per-call (what a caller polls to see whether its removal
 * did anything). Every case asserts both, and they deliberately disagree.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBreakPassesContinueAcrossCallsTest,
	"DestructionGame.Core.Structure.BreakPassesContinueAcrossCalls",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBreakPassesContinueAcrossCallsTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	/*
	 * One break before the removal and one after; they must not share a number.
	 *
	 *          [ block 3 ]                12 MPa.cm2 total, over three 100 cm2 pads
	 *         /     |     \
	 *     lime    cement  cement
	 *   [pad 0]  [pad 1]  [pad 2]
	 *   ======   ======   ======
	 *
	 * As built each joint takes 4 MPa: lime (2.0) gives in pass 1, cement settles at 6 MPa
	 * (0.60). Pulling pad 1 puts 12 MPa on pad 2, which gives: stamp 2, though the call
	 * reports one pass.
	 */
	{
		const FStructureSpec Spec = {
			{
				{ BrickMassKg, true }, { BrickMassKg, true }, { BrickMassKg, true },
				{ MassForStress(4.0, 300.0), false }
			},
			{
				{ 0, 3, BedJointNormal, 100.0, LimeMortar },
				{ 1, 3, BedJointNormal, 100.0, GeneralPurposeMortar },
				{ 2, 3, BedJointNormal, 100.0, GeneralPurposeMortar }
			}
		};

		FStructure Structure;
		BuildStructure(Structure, Spec);

		// Captured first: C++ argument evaluation order is unspecified.
		const int32 PassesAsBuilt = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("the lime joint at 2.00 should give in exactly one pass, got %d"),
				PassesAsBuilt),
			PassesAsBuilt == 1);

		TestTrue(
			FString::Printf(TEXT("the lime joint should be stamped pass 1, got HasGiven %d and pass %d"),
				Structure.GetConnection(0).HasGiven() ? 1 : 0, Structure.GetBreakPass(0)),
			Structure.GetConnection(0).HasGiven() && Structure.GetBreakPass(0) == 1);

		// Control: settled at 0.60 each, so the next break is caused by the removal.
		CheckSettledCompression(*this, Structure, 1, 0.6, TEXT("settled after the lime joint gave"));
		CheckSettledCompression(*this, Structure, 2, 0.6, TEXT("settled after the lime joint gave"));

		TestTrue(TEXT("removing one of the two surviving pads should report that it removed a live piece"),
			Structure.RemovePiece(1));

		const int32 PassesAfterRemoval = Structure.SolveAndBreak();

		// The count is per-call; the stamp below is global.
		TestTrue(
			FString::Printf(TEXT("the survivor at 12 MPa should give in exactly one pass of THIS call, got %d"),
				PassesAfterRemoval),
			PassesAfterRemoval == 1);

		TestTrue(
			FString::Printf(TEXT("the cement joint gave after the lime one and must be stamped pass 2, got %d"),
				Structure.GetBreakPass(2)),
			Structure.GetBreakPass(2) == 2);

		TestTrue(
			FString::Printf(TEXT("the cement joint gave LATER than the lime one, got stamps %d then %d"),
				Structure.GetBreakPass(0), Structure.GetBreakPass(2)),
			Structure.GetBreakPass(2) > Structure.GetBreakPass(0));

		TestTrue(
			FString::Printf(TEXT("the lime joint's stamp must survive the second cascade, got %d"),
				Structure.GetBreakPass(0)),
			Structure.GetBreakPass(0) == 1);

		TestTrue(
			FString::Printf(TEXT("the removed pad's joint went with its piece: HasGiven %d and pass %d"),
				Structure.GetConnection(1).HasGiven() ? 1 : 0, Structure.GetBreakPass(1)),
			Structure.GetConnection(1).HasGiven() && Structure.GetBreakPass(1) == INDEX_NONE);

		TestTrue(TEXT("the block has nothing left holding it up"),
			!Structure.IsPieceSupported(3));
	}

	/*
	 * A multi-pass second call, stamped 2 and 3: separates "continue from the high-water mark"
	 * from "start at one" and "start at the last count". Two independent sub-structures in one
	 * graph.
	 *
	 *   [ block 1 ]                        [ block 5 ]           4.8 MPa.cm2 over 3 pads
	 *       |                             /     |     \
	 *     lime                        lime    bolt   cement
	 *   [pad 0]                     [pad 2]  [pad 3]  [pad 4]
	 *   ======                      ======   ======   ======
	 *
	 * Call 1 breaks only the left lime joint (3.0 MPa against 2.0). The right three sit at
	 * 1.6 MPa: lime 0.80, bolt 0.4706 (mean f_c,90 of 3.4), cement 0.16.
	 *
	 * Pulling pad 4 leaves 2.4 MPa on two joints: lime at 1.20 gives (pass 2), the bolt at
	 * 0.706 survives that pass, then takes 4.8 MPa (1.41) and gives (pass 3).
	 *
	 * Do not retune these masses casually. Parallel joints share one stress, so only capacity
	 * orders the breaks, and dropping one of N raises stress by at most 2x. Two-pass needs two
	 * capacities under 2x apart: lime 2.0 and the fastener's 3.4 (1.7x) are the only pair.
	 */
	{
		const FStructureSpec Spec = {
			{
				{ BrickMassKg, true },                  // 0: pad under the left-hand block
				{ MassForStress(3.0, 100.0), false },   // 1: left-hand block, breaks call 1
				{ BrickMassKg, true },                  // 2: lime pad
				{ BrickMassKg, true },                  // 3: bolted pad
				{ BrickMassKg, true },                  // 4: cement pad, REMOVED
				{ MassForStress(1.6, 300.0), false }    // 5: right-hand block
			},
			{
				{ 0, 1, BedJointNormal, 100.0, LimeMortar },
				{ 2, 5, BedJointNormal, 100.0, LimeMortar },
				{ 3, 5, BedJointNormal, 100.0, Bolt },
				{ 4, 5, BedJointNormal, 100.0, GeneralPurposeMortar }
			}
		};

		FStructure Structure;
		BuildStructure(Structure, Spec);

		// Everything below depends on the right-hand joints surviving call 1.
		Structure.SolveLoads();
		CheckSettledCompression(*this, Structure, 1, 0.8, TEXT("as built"));
		CheckSettledCompression(*this, Structure, 2, 1.6 / 3.4, TEXT("as built"));
		CheckSettledCompression(*this, Structure, 3, 0.16, TEXT("as built"));

		const int32 PassesAsBuilt = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("only the left-hand lime joint should give as built, got %d passes"),
				PassesAsBuilt),
			PassesAsBuilt == 1);

		TestTrue(
			FString::Printf(TEXT("the left-hand lime joint should be stamped pass 1, got %d"),
				Structure.GetBreakPass(0)),
			Structure.GetConnection(0).HasGiven() && Structure.GetBreakPass(0) == 1);

		TestTrue(
			FString::Printf(TEXT("the right-hand joints must all still be intact, got %d / %d / %d"),
				Structure.GetConnection(1).HasGiven() ? 1 : 0,
				Structure.GetConnection(2).HasGiven() ? 1 : 0,
				Structure.GetConnection(3).HasGiven() ? 1 : 0),
			!Structure.GetConnection(1).HasGiven()
				&& !Structure.GetConnection(2).HasGiven()
				&& !Structure.GetConnection(3).HasGiven());

		TestTrue(TEXT("removing the cement pad should report that it removed a live piece"),
			Structure.RemovePiece(4));

		const int32 PassesAfterRemoval = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("the removal should cascade in exactly two passes of THIS call, got %d"),
				PassesAfterRemoval),
			PassesAfterRemoval == 2);

		TestTrue(
			FString::Printf(TEXT("the right-hand lime joint gave in the first pass of the second call and must be stamped 2, got %d"),
				Structure.GetBreakPass(1)),
			Structure.GetBreakPass(1) == 2);

		TestTrue(
			FString::Printf(TEXT("the bolted joint gave one pass later and must be stamped 3, got %d"),
				Structure.GetBreakPass(2)),
			Structure.GetBreakPass(2) == 3);

		TestTrue(
			FString::Printf(TEXT("the collapse sequence should read 1 < 2 < 3, got %d, %d, %d"),
				Structure.GetBreakPass(0), Structure.GetBreakPass(1), Structure.GetBreakPass(2)),
			Structure.GetBreakPass(0) < Structure.GetBreakPass(1)
				&& Structure.GetBreakPass(1) < Structure.GetBreakPass(2));

		TestTrue(
			FString::Printf(TEXT("the removed pad's joint is out of the structure and unstamped, got HasGiven %d and pass %d"),
				Structure.GetConnection(3).HasGiven() ? 1 : 0, Structure.GetBreakPass(3)),
			Structure.GetConnection(3).HasGiven() && Structure.GetBreakPass(3) == INDEX_NONE);

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			TestTrue(
				FString::Printf(TEXT("connection %d carries break pass %d, which is neither INDEX_NONE nor a pass"),
					Index, Structure.GetBreakPass(Index)),
				Structure.GetBreakPass(Index) == INDEX_NONE || Structure.GetBreakPass(Index) >= 1);
		}
	}

	return true;
}

/**
 * IsPieceSupported is the last solve's answer; removal does not rewrite it. A
 * characterisation test (green on arrival) pinning the scoped contract:
 *
 *     never solved     false, for every handle
 *     removed          the last solve's answer, unchanged, until the next solve
 *     after that solve false, and a removed grounded piece is no longer earth
 *
 * The middle row is stale by design: clearing one entry on removal would leave a half-stale
 * array, worse than a uniformly stale one. If that changes, update Structure.h in the same
 * commit; do not relax the assertion.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureRemovedPieceSupportNeedsASolveTest,
	"DestructionGame.Core.Structure.RemovedPieceSupportNeedsASolve",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureRemovedPieceSupportNeedsASolveTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	/** A grounded pad with a brick resting on it: the smallest structure that holds. */
	const FStructureSpec PadAndBrick = {
		{ { BrickMassKg, true }, { BrickMassKg, false } },
		{ { 0, 1, BedJointNormal, 100.0, Unbreakable } }
	};

	// Before any solve the answer fails closed to false: PieceSupported is sized by a solve.
	{
		FStructure Structure;
		BuildStructure(Structure, PadAndBrick);

		TestFalse(TEXT("the grounded pad is not yet reported held up, because nothing has solved"),
			Structure.IsPieceSupported(0));

		TestFalse(TEXT("the brick is not yet reported held up, because nothing has solved"),
			Structure.IsPieceSupported(1));

		TestTrue(TEXT("removing the brick before any solve still removes it"),
			Structure.RemovePiece(1));

		TestFalse(TEXT("and it is still not held up"), Structure.IsPieceSupported(1));
	}

	// Removing the brick: its answer is unchanged until a re-solve, then false.
	{
		FStructure Structure;
		BuildStructure(Structure, PadAndBrick);
		Structure.SolveLoads();

		TestTrue(TEXT("the brick starts out held up by the pad"), Structure.IsPieceSupported(1));

		TestTrue(TEXT("removing the brick should report that it removed a live piece"),
			Structure.RemovePiece(1));

		TestTrue(TEXT("the brick reads as removed IMMEDIATELY — that accessor needs no solve"),
			Structure.IsPieceRemoved(1));

		// The stale row: a removed piece still reads as held up until the next solve.
		TestTrue(
			FString::Printf(TEXT("between a removal and the next solve, support is the LAST SOLVE'S answer and still reads %d"),
				Structure.IsPieceSupported(1) ? 1 : 0),
			Structure.IsPieceSupported(1));

		Structure.SolveLoads();

		TestFalse(TEXT("after the next solve the removed brick is not held up"),
			Structure.IsPieceSupported(1));

		TestTrue(TEXT("and the pad it stood on is still resting on the earth"),
			Structure.IsPieceSupported(0));
	}

	// Removing the ground: until the re-solve both pieces still read as held up.
	{
		FStructure Structure;
		BuildStructure(Structure, PadAndBrick);
		Structure.SolveLoads();

		TestTrue(TEXT("both pieces start out held up"),
			Structure.IsPieceSupported(0) && Structure.IsPieceSupported(1));

		TestTrue(TEXT("removing the pad should report that it removed a live piece"),
			Structure.RemovePiece(0));

		TestTrue(
			FString::Printf(TEXT("the removed pad still reads %d and the brick above it %d, because nothing has re-solved"),
				Structure.IsPieceSupported(0) ? 1 : 0, Structure.IsPieceSupported(1) ? 1 : 0),
			Structure.IsPieceSupported(0) && Structure.IsPieceSupported(1));

		Structure.SolveLoads();

		TestFalse(TEXT("after the solve the removed pad is not resting on the earth"),
			Structure.IsPieceSupported(0));

		TestFalse(TEXT("and the brick has lost the only thing that was holding it up"),
			Structure.IsPieceSupported(1));
	}

	// SolveAndBreak runs SolveLoads, so it clears the stale row too.
	{
		FStructure Structure;
		BuildStructure(Structure, PadAndBrick);
		Structure.SolveLoads();

		TestTrue(TEXT("removing the brick should report that it removed a live piece"),
			Structure.RemovePiece(1));

		const int32 Passes = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("removing the top brick overloads nothing, got %d passes"), Passes),
			Passes == 0);

		TestFalse(TEXT("cascading re-solved, so the removed brick is not held up"),
			Structure.IsPieceSupported(1));
	}

	return true;
}

/**
 * Degenerate removals fail closed, and a picked-apart structure still answers finitely.
 * FMath::Max/Min swallow NaN, so a fault would surface as a plausible number; finiteness is
 * asserted over the whole matrix. An unknown handle reads as removed (fail-closed, matching
 * GetPiece, IsPieceSupported and GetBreakPass), and a removed piece is never held up.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureRemovalDegenerateInputTest,
	"DestructionGame.Core.Structure.RemovalDegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureRemovalDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	constexpr double Tolerance = 1.0e-6;

	// Guards BrickMassKg's static initialisation (see its comment); not a weight check.
	TestTrue(
		FString::Printf(TEXT("the brick mass must derive to something positive, got %f kg"), BrickMassKg),
		BrickMassKg > 0.0);

	// Out-of-range handles, each on a fresh structure so one row cannot mask the next.
	{
		struct FBadHandleCase
		{
			const TCHAR* Description;
			int32 Handle;
		};

		const TArray<FBadHandleCase> BadHandles = {
			{ TEXT("INDEX_NONE"), INDEX_NONE },
			{ TEXT("a negative handle"), -7 },
			{ TEXT("one past the last piece"), 3 },
			{ TEXT("far past the last piece"), 4096 },
		};

		for (const FBadHandleCase& Case : BadHandles)
		{
			const FStructureSpec Spec = {
				{ { BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, false } },
				{
					{ 0, 1, BedJointNormal, 100.0, Unbreakable },
					{ 1, 2, BedJointNormal, 100.0, Unbreakable }
				}
			};

			FStructure Structure;
			BuildStructure(Structure, Spec);

			TestFalse(
				FString::Printf(TEXT("removing %s should remove nothing"), Case.Description),
				Structure.RemovePiece(Case.Handle));

			TestTrue(
				FString::Printf(TEXT("%s: the handle range must be unchanged at 3, got %d"),
					Case.Description, Structure.NumPieces()),
				Structure.NumPieces() == 3);

			TestTrue(
				FString::Printf(TEXT("%s: nothing was removed, so 3 pieces are still live, got %d"),
					Case.Description, Structure.NumLivePieces()),
				Structure.NumLivePieces() == 3);

			TestTrue(
				FString::Printf(TEXT("%s: a handle that names no piece reads as removed, got %d"),
					Case.Description, Structure.IsPieceRemoved(Case.Handle) ? 1 : 0),
				Structure.IsPieceRemoved(Case.Handle));

			for (int32 Index = 0; Index < Structure.NumPieces(); ++Index)
			{
				TestFalse(
					FString::Printf(TEXT("%s: piece %d must still be live"), Case.Description, Index),
					Structure.IsPieceRemoved(Index));
			}

			Structure.SolveLoads();

			TestTrue(
				FString::Printf(TEXT("%s: the stack must still stand"), Case.Description),
				Structure.IsPieceSupported(1) && Structure.IsPieceSupported(2));
		}
	}

	// Removing the same piece twice: the second call removes and restamps nothing.
	{
		const FStructureSpec Spec = {
			{ { BrickMassKg, true }, { BrickMassKg, false } },
			{ { 0, 1, BedJointNormal, 100.0, Unbreakable } }
		};

		FStructure Structure;
		BuildStructure(Structure, Spec);

		TestTrue(TEXT("the first removal takes the piece"), Structure.RemovePiece(1));
		TestFalse(TEXT("the second removal of the same piece takes nothing"), Structure.RemovePiece(1));

		TestTrue(
			FString::Printf(TEXT("the handle range must be unchanged at 2, got %d"), Structure.NumPieces()),
			Structure.NumPieces() == 2);

		TestTrue(TEXT("the piece is still removed after the redundant call"),
			Structure.IsPieceRemoved(1));

		TestFalse(TEXT("the grounded piece is untouched by either call"),
			Structure.IsPieceRemoved(0));

		// The live count catches a redundant call that decremented anyway.
		TestTrue(
			FString::Printf(TEXT("two pieces less one removed twice should still leave 1 live, got %d"),
				Structure.NumLivePieces()),
			Structure.NumLivePieces() == 1);

		TestTrue(
			FString::Printf(TEXT("the joint went with its piece: HasGiven %d and no pass stamp, got %d"),
				Structure.GetConnection(0).HasGiven() ? 1 : 0, Structure.GetBreakPass(0)),
			Structure.GetConnection(0).HasGiven() && Structure.GetBreakPass(0) == INDEX_NONE);
	}

	// An emptied structure still answers, and solving it is not an error.
	{
		FStructure Structure;
		const int32 Handle = Structure.AddPiece(BrickMassKg, true);

		TestTrue(TEXT("the lone piece can be removed"), Structure.RemovePiece(Handle));

		TestTrue(
			FString::Printf(TEXT("its handle stays valid, expected 1 piece of range, got %d"),
				Structure.NumPieces()),
			Structure.NumPieces() == 1);

		TestTrue(TEXT("the lone piece reads as removed"), Structure.IsPieceRemoved(Handle));

		TestTrue(
			FString::Printf(TEXT("an emptied structure has no live pieces, got %d"),
				Structure.NumLivePieces()),
			Structure.NumLivePieces() == 0);

		Structure.SolveLoads();

		TestFalse(TEXT("a removed grounded piece is not being held up by the earth"),
			Structure.IsPieceSupported(Handle));

		const int32 Passes = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("cascading an emptied structure should break nothing, got %d passes"),
				Passes),
			Passes == 0);
	}

	/*
	 * Take four pieces apart one at a time (middle, unconnected, grounded, stranded). After
	 * each step: finite loads, a correct live count, and no removal in the pass numbering.
	 */
	{
		const FStructureSpec Spec = {
			{
				{ BrickMassKg, true },                 // 0: pad
				{ MassForStress(1.0, 100.0), false },  // 1: lower block
				{ MassForStress(1.0, 100.0), false },  // 2: upper block
				{ BrickMassKg, true }                  // 3: a second, unconnected pad
			},
			{
				{ 0, 1, BedJointNormal, 100.0, GeneralPurposeMortar },
				{ 1, 2, BedJointNormal, 100.0, GeneralPurposeMortar }
			}
		};

		FStructure Structure;
		BuildStructure(Structure, Spec);

		const TArray<int32> RemovalOrder = { 1, 3, 0, 2 };

		for (int32 Step = 0; Step < RemovalOrder.Num(); ++Step)
		{
			const int32 Handle = RemovalOrder[Step];

			TestTrue(
				FString::Printf(TEXT("step %d: removing live piece %d"), Step, Handle),
				Structure.RemovePiece(Handle));

			TestTrue(
				FString::Printf(TEXT("step %d: the handle range must stay 4, got %d"),
					Step, Structure.NumPieces()),
				Structure.NumPieces() == 4);

			TestTrue(
				FString::Printf(TEXT("step %d: the joint count must stay 2, got %d"),
					Step, Structure.NumConnections()),
				Structure.NumConnections() == 2);

			TestTrue(
				FString::Printf(TEXT("step %d: %d removals should leave %d live pieces, got %d"),
					Step, Step + 1, 4 - (Step + 1), Structure.NumLivePieces()),
				Structure.NumLivePieces() == 4 - (Step + 1));

			const int32 Passes = Structure.SolveAndBreak();

			TestTrue(
				FString::Printf(TEXT("step %d: a removal cannot overload what is left, got %d passes"),
					Step, Passes),
				Passes == 0);

			for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
			{
				const FVector Force = Structure.GetConnectionForce(Index);

				TestTrue(
					FString::Printf(TEXT("step %d: connection %d load must be finite, got (%f, %f, %f)"),
						Step, Index, Force.X, Force.Y, Force.Z),
					!Force.ContainsNaN() && FMath::IsFinite(Force.X)
						&& FMath::IsFinite(Force.Y) && FMath::IsFinite(Force.Z));

				// Every joint here goes with a piece, so none may carry a pass.
				TestTrue(
					FString::Printf(TEXT("step %d: connection %d went with a piece and must carry no pass, got %d"),
						Step, Index, Structure.GetBreakPass(Index)),
					Structure.GetBreakPass(Index) == INDEX_NONE);

				if (Structure.GetConnection(Index).HasGiven())
				{
					TestTrue(
						FString::Printf(TEXT("step %d: connection %d has gone and must carry nothing, got %f"),
							Step, Index, Force.Z),
						FMath::IsNearlyZero(Force.Z, Tolerance));
				}
			}

			for (int32 Index = 0; Index < Structure.NumPieces(); ++Index)
			{
				if (Structure.IsPieceRemoved(Index))
				{
					TestFalse(
						FString::Printf(TEXT("step %d: removed piece %d must not read as held up"),
							Step, Index),
						Structure.IsPieceSupported(Index));
				}
			}
		}

		TestTrue(TEXT("every piece is gone once the whole structure has been removed"),
			Structure.IsPieceRemoved(0) && Structure.IsPieceRemoved(1)
				&& Structure.IsPieceRemoved(2) && Structure.IsPieceRemoved(3));

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			TestTrue(
				FString::Printf(TEXT("connection %d is out of the structure but never failed, got HasGiven %d and pass %d"),
					Index, Structure.GetConnection(Index).HasGiven() ? 1 : 0,
					Structure.GetBreakPass(Index)),
				Structure.GetConnection(Index).HasGiven()
					&& Structure.GetBreakPass(Index) == INDEX_NONE);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
