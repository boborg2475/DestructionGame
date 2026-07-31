// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named differently from every other one in this
 * directory. An anonymous namespace is private to a TRANSLATION UNIT rather than to a
 * file, and a unity build merges many files into one — at which point two anonymous
 * BedJointNormal declarations are the same declaration twice. See CURRENT_STATE.md;
 * the `using namespace` for this one lives inside each RunTest body for the same
 * reason.
 */
namespace StructureCascadeSupport
{
	using namespace DestructionProfiles;

	/**
	 * Unreal's gravity, 980 cm/s2, spelled out here rather than imported from
	 * production so this file fails if production gets it wrong instead of agreeing
	 * with it.
	 *
	 * THE UNIT TRAP, stated once. Mass is already kilograms and length already
	 * centimetres, so MassKg * 980 IS a force in Unreal units — the 1 N = 100 uu
	 * conversion is baked into the 980 rather than applied on top of it.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double WeightOf(double MassKg)
	{
		return MassKg * GravityCmPerSecondSquared;
	}

	/**
	 * Force, in Unreal units, that loads the given area to the given stress.
	 *
	 * Spelled out from the SI definitions rather than reusing ForceUnitsPerMPaSqCm, so
	 * a wrong conversion constant in production shows up here as a failure instead of
	 * as an agreement: 1 N = 100 uu, 1 cm2 = 100 mm2, 1 MPa = 1 N/mm2, so one MPa over
	 * one cm2 is 10000 uu.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	/** The same boundary the other way: stress, in MPa, that a force puts on an area. */
	constexpr double MPaForForce(double ForceUnits, double AreaSqCm)
	{
		return ForceUnits / (100.0 * 100.0 * AreaSqCm);
	}

	/**
	 * Mass of a piece whose whole weight loads the given area to the given stress.
	 *
	 * Fixtures are written in terms of STRESS rather than of mass, because stress is
	 * what decides whether a joint gives and it is the only number a reader can check
	 * against a published strength by eye. "MassForStress(6.0, 400.0)" says exactly
	 * what the fixture is for; 24489.795918 kg says nothing at all.
	 */
	constexpr double MassForStress(double MPa, double AreaSqCm)
	{
		return ForceForMPa(MPa, AreaSqCm) / GravityCmPerSecondSquared;
	}

	/**
	 * A standard clay brick, 2.72 kg, used only for the GROUNDED pads and piers in
	 * these fixtures — their own weight terminates at the earth and never reaches a
	 * joint, so the value is immaterial and a real one is less distracting than zero.
	 */
	constexpr double BrickMassKg = 2.72;

	/** Horizontal interface, normal pointing up at the piece above: a bed joint. */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Vertical interface, normal pointing sideways at the neighbour: a head joint. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);

	/**
	 * What a joint does with the load it is handed, and therefore WHICH CAPACITY the
	 * break decision is measured against.
	 *
	 * This is the axis-governance guard, and it is not decoration. ComputeUtilisation
	 * returns the WORST of three axes, so a fixture aimed at compression that happens to
	 * be governed by shear breaks for a reason the test did not intend and proves
	 * nothing about the one it did. Every fixture in this file loads its joints on
	 * EXACTLY ONE non-zero axis — a vertical force on an axis-aligned bed joint is pure
	 * compression, on an axis-aligned head joint pure shear — so the governing axis is
	 * not merely dominant, it is the only one carrying anything. That is asserted
	 * per joint below rather than assumed.
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

	/**
	 * A joint as data, INCLUDING ITS OWN STRENGTH.
	 *
	 * StructureTest.cpp's builder gives every joint in a structure the same profile,
	 * which is right there — it measures routing and wants nothing to break. Cascade
	 * is the opposite: the ORDER joints give in is the thing under test, and with the
	 * area-weighted split every parallel support of one piece carries the same STRESS
	 * whatever its area (share is proportional to area, so stress is total over total
	 * area and the area cancels). Identical profiles would therefore give identical
	 * utilisations and every joint would break in the same pass. Differing strengths
	 * are what make the sequence exist at all.
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
		 * SIGNED Z of the force the joint carries in the SETTLED structure, uu.
		 *
		 * Zero for a joint that has given: a broken joint is out of the structure and
		 * carries nothing, which is the zero redistribution is built on. Per
		 * ConnectionLoad.h the force belongs to PieceB, so a joint that names the
		 * loaded piece first stores the equal-and-opposite reaction and is positive.
		 */
		double FinalForceZUU = 0.0;

		EJointKind Kind = EJointKind::Bed;

		/**
		 * The breaking pass that gave this joint, counted from 1, or INDEX_NONE if it
		 * must still be intact when everything settles.
		 *
		 * ORDER WITHIN A PASS IS NOT ASSERTED ANYWHERE and must not be: every joint
		 * over capacity in the same pass gives together, so two joints that share a
		 * number are unordered with respect to each other. Order BETWEEN passes is
		 * real, and it is what a collapse looks like when it is played back.
		 */
		int32 BreakPass = INDEX_NONE;

		/**
		 * Utilisation of the settled load, hand-derived from the published strength in
		 * each case's comment.
		 *
		 * Zero for a broken joint, because it carries nothing. For an intact one this
		 * is the MARGIN: it records how far the survivor is from giving, so a fixture
		 * that only just held cannot drift into only just failing unnoticed.
		 */
		double FinalUtilisation = 0.0;
	};

	/** A structure, the sequence it must break in, and what is left standing. */
	struct FCascadeCase
	{
		const TCHAR* Description;
		FStructureSpec Spec;

		/**
		 * Number of passes that broke at least one joint. Zero for a structure that
		 * stands as built.
		 */
		int32 ExpectedPasses = 0;

		/** In connection-array order. */
		TArray<FExpectedJoint> ExpectedJoints;

		/** Whether each piece is still held up, in piece-array order. */
		TArray<bool> ExpectedSupported;
	};

	/**
	 * Build a structure in place.
	 *
	 * BY REFERENCE, AND THAT IS THE POINT OF THIS WHOLE FILE'S CARE AROUND COPIES.
	 * FConnection is copyable and its "has given" latch is per-copy, so anything that
	 * evaluates connections by value latches a temporary and leaves the real joint
	 * untouched — see the trap described on CheckCascadeCase.
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

	/**
	 * Build, cascade, and check one case against its table AND against the properties
	 * that must hold for any structure at all.
	 *
	 * THE COPYABLE-LATCH TRAP, and the assertion that closes it. FConnection is
	 * copyable and the latch is a private bool on the object, so
	 *
	 *     for (FConnection C : Connections) { C.ApplyForce(...); }
	 *
	 * — one missing ampersand — compiles clean, evaluates every joint, latches every
	 * overloaded one ON THE TEMPORARY, and leaves the real connections untouched. The
	 * structure then reports ZERO broken joints under any load whatsoever and never
	 * comes apart. Nothing in the type prevents it and no test before this one could
	 * see it, because phase 2a never latched anything. Every assertion below is made
	 * AFTER SolveAndBreak has returned, through GetConnection, which hands back a
	 * reference to the connection the structure actually owns — so a cascade that
	 * happened only to copies reads as a structure where nothing broke.
	 *
	 * ASSERTED FROM THE TABLE:
	 *
	 *  - the number of breaking passes;
	 *  - which joints have given, and IN WHICH PASS;
	 *  - what each joint carries once it has settled, and on which axis;
	 *  - how hard each surviving joint is working, so a margin cannot silently erode;
	 *  - which pieces are still held up.
	 *
	 * ASSERTED UNIVERSALLY, whatever the table says:
	 *
	 *  - IT TERMINATES, and within the only bound the latch allows. Joints never heal,
	 *    so a pass that breaks nothing ends the process and no pass can break nothing
	 *    and still continue: the count is therefore at most the connection count. (This
	 *    is NOT SolveLoads' own internal pass bound, which CURRENT_STATE.md records as
	 *    provably 2 — a different and much smaller thing, nested inside each pass here.)
	 *  - EVERY PASS BROKE SOMETHING. A gap in the numbering would mean a pass that broke
	 *    nothing and then carried on, which is the shape of a loop that does not settle.
	 *  - A BROKEN JOINT CARRIES NOTHING and an intact one has no pass stamp. Two
	 *    accessors on the same object have to tell one story.
	 *  - NOTHING SURVIVES OVER CAPACITY. The whole point of settling: recompute each
	 *    intact joint's utilisation from the force the structure reports and its own
	 *    profile, and if one is above 1 the cascade stopped early.
	 *  - GROUND-REACTION CONSERVATION against the solver's own final claim. Everything
	 *    still held up has to arrive at the earth — short means load was stranded on the
	 *    way down, long means a share was counted twice.
	 *  - CALLING IT AGAIN CHANGES NOTHING. A settled structure breaks nothing more, no
	 *    joint un-breaks, and no pass stamp is rewritten. That is the latch being total,
	 *    observed from outside.
	 *
	 * (The classification below calls ClassifyForce directly, which DESIGN.md warns
	 * re-opens the degenerate-normal hole. Safe here: it makes no break decision, and
	 * every normal in these specs is a real plane — AddConnection rejects the rest.)
	 */
	void CheckCascadeCase(FAutomationTestBase& Test, const FCascadeCase& Case)
	{
		constexpr double Tolerance = 1.0e-6;

		/** Absolute floor, for the rows whose expectation is an exact zero. */
		constexpr double UtilisationTolerance = 1.0e-12;

		/**
		 * And a relative allowance for everything else. The fixtures are written as
		 * MassForStress, which divides by 980 and is then multiplied by 980 again inside
		 * the solver, so the settled loads carry a rounding error proportional to their
		 * own size rather than a fixed one.
		 */
		constexpr double RelativeUtilisationTolerance = 1.0e-9;

		FStructure Structure;
		BuildStructure(Structure, Case.Spec);

		// The fixture itself, first. A spec rejected at the door would make every
		// assertion below a statement about a structure that was never built.
		Test.TestTrue(
			FString::Printf(TEXT("%s: expected %d pieces, got %d"),
				Case.Description, Case.Spec.Pieces.Num(), Structure.NumPieces()),
			Structure.NumPieces() == Case.Spec.Pieces.Num());

		// THE TABLE, NOT THE SPEC, IS THE AUTHORITY ON WHAT SHOULD HAVE BEEN ACCEPTED.
		// For every case that describes real joints these are the same number, so this is
		// exactly as strong as comparing against the spec: a joint AddConnection rejected
		// where the table expected one still fails here. The difference is that a fixture
		// which MEANS to describe a rejected joint can now say so, by giving it no
		// expectation row — inexpressible while the spec count was the reference.
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

		// TERMINATION, as a bound rather than as a hope. Every pass that is not the last
		// breaks at least one joint, joints never heal, so the process cannot run longer
		// than there are joints to break.
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

			// GetConnection hands back a reference to the connection the STRUCTURE owns,
			// so this is the real latch and not a copy of it.
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

			// The two answers have to be one answer. A joint stamped with a pass and
			// reporting itself intact — or the reverse — is the copyable-latch defect
			// with a bookkeeping array papered over the top of it.
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

			// Gravity does not change direction because a joint happens to be vertical,
			// and it does not acquire one because a neighbouring joint gave.
			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d load must be vertical and finite, got (%f, %f, %f)"),
					Case.Description, Index, Force.X, Force.Y, Force.Z),
				!Force.ContainsNaN()
					&& FMath::IsFinite(Force.X) && FMath::IsFinite(Force.Y) && FMath::IsFinite(Force.Z)
					&& FMath::IsNearlyZero(Force.X, Tolerance) && FMath::IsNearlyZero(Force.Y, Tolerance));

			// A BROKEN JOINT IS OUT OF THE STRUCTURE AND CARRIES NOTHING. That zero is
			// what redistribution means: the load it used to hold has to be somewhere
			// else now, and a broken joint still reporting a share is a wall that sheds
			// a joint and keeps standing on it.
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

			// THE AXIS-GOVERNANCE GUARD. Exactly one axis carries anything, so the
			// utilisation below can only have come from the capacity this fixture was
			// aimed at. Without this a case aimed at compression that was silently
			// governed by shear would still break at the right moment and prove nothing.
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

			// RELATIVE, WITH A FLOOR FAR BELOW THE SMALLEST EXPECTATION IN ANY TABLE.
			// A flat absolute 1e-9 was wide enough to swallow whole expectations: the
			// Unbreakable fixture settles at 1.96e-11, so every value in [-1e-9, 1e-9]
			// passed — INCLUDING EXACTLY ZERO, which is precisely the answer a joint
			// dropped out of the load path would give. The floor exists for the rows
			// expecting exactly 0 (a broken joint carries nothing, and that zero is
			// exact), and must stay well under the smallest non-zero expectation
			// anywhere in this file or that row stops being able to fail.
			const double UtilisationSlack = FMath::Max(
				UtilisationTolerance, RelativeUtilisationTolerance * FMath::Abs(Expected.FinalUtilisation));

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d should settle at utilisation %.12g, got %.12g"),
					Case.Description, Index, Expected.FinalUtilisation, Utilisation),
				FMath::IsNearlyEqual(Utilisation, Expected.FinalUtilisation, UtilisationSlack));

			// NOTHING SURVIVES OVER CAPACITY — the definition of "settled". Written
			// !(x <= 1) rather than x > 1 so a NaN utilisation fails here instead of
			// slipping through as a joint that is comfortably fine.
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

		// GROUND-REACTION CONSERVATION, against the solver's OWN final claim about what
		// it is holding up rather than against the table, so it keeps its force if the
		// expectations above are ever revisited.
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
			// OVER THE JOINTS THE STRUCTURE ACTUALLY HOLDS, and taking their piece
			// handles from it rather than assuming spec row N is structure joint N.
			//
			// This used to walk the SPEC and index Case.Spec.Pieces by the spec's own
			// handles, which is a crash waiting for the first row that names a piece
			// that does not exist: AddConnection rejects such a row, but the harness
			// still read Pieces[INDEX_NONE] — Pieces[-1] — and took the whole process
			// down. The arity assertions above are TestTrue and therefore non-fatal, so
			// execution runs straight past them into it, and a harness that crashes
			// instead of failing is worse than no harness. AddConnection validates both
			// handles at the door, so the ones read here cannot be out of range.
			//
			// Grounded-ness still comes from the SPEC, so this stays a statement about
			// the fixture rather than about what production chose to store.
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

		// Out-of-range handles fail closed rather than reading as a joint that broke in
		// some pass nobody ran.
		Test.TestTrue(
			FString::Printf(TEXT("%s: an unknown connection has no break pass, got %d"),
				Case.Description, Structure.GetBreakPass(Structure.NumConnections())),
			Structure.GetBreakPass(Structure.NumConnections()) == INDEX_NONE);

		Test.TestTrue(
			FString::Printf(TEXT("%s: connection INDEX_NONE has no break pass, got %d"),
				Case.Description, Structure.GetBreakPass(INDEX_NONE)),
			Structure.GetBreakPass(INDEX_NONE) == INDEX_NONE);

		// IT HAS SETTLED, which is a claim about the second call and not only about the
		// first. Nothing more gives, nothing un-gives, no stamp is rewritten and no load
		// moves. Joints never heal (the latch in FConnection is total), so a structure
		// that changed here would either be re-breaking what it already broke or
		// recovering a joint mid-collapse.
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
			// Bit for bit, not nearly-equal: the second run repeats the identical
			// arithmetic on identical inputs, so any difference at all is state carried
			// between runs rather than rounding.
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
 * A structure breaks every joint that is over capacity, moves their load onto the
 * neighbours, and repeats until it either settles or has come apart — recording WHICH
 * PASS each joint gave in.
 *
 * THE POLICY, decided here and encoded by these cases: break EVERY joint over capacity
 * in the same pass, stamp each with the pass number that broke it, re-solve, repeat.
 * Ordering WITHIN a pass is arbitrary and is deliberately not asserted anywhere; two
 * joints that give together carry the same number and nothing distinguishes them.
 * Ordering BETWEEN passes is real, and it is what makes a collapse watchable — it is
 * the sequence phase 5's visualisation and the planned piece-creation timestamps will
 * play back. Chosen over strict worst-joint-first, which costs a solve per joint for the
 * same end state, and over unordered all-at-once, which loses the sequence entirely.
 *
 * NO WORLD, NO POSITIONS, NOTHING MOVES. As in StructureTest.cpp, gravity is an input to
 * the arithmetic rather than something a simulation applies, and every assertion is on
 * the MECHANISM: which joints report themselves given, in which pass, what the survivors
 * carry, which pieces are still held up. DESIGN.md §4 is emphatic that displacement is
 * never a valid break assertion — two pieces can sever and stay resting exactly in
 * place — and here there is no displacement to be tempted by.
 *
 * WHY THE FIXTURES LOOK THE WAY THEY DO. Every case hangs one heavy piece off several
 * grounded pads through pure bed joints, because with the area-weighted split every
 * parallel support of one piece carries the SAME STRESS regardless of its area: the
 * share is proportional to area, so the area cancels and the stress is simply the total
 * load over the total supporting area. Three consequences make it the right shape here:
 *
 *  - a joint gives or holds on its PROFILE alone, so the sequence is read directly off
 *    published compressive strengths — lime mortar 2 MPa, cement mortar 10, dry stone 30;
 *  - removing one support shrinks the total area, so the stress on every survivor rises
 *    by a factor this test can compute exactly, which is redistribution in its most
 *    checkable form;
 *  - a vertical load on an axis-aligned bed joint is PURE COMPRESSION — shear and tension
 *    are exactly zero — so no other axis can be quietly governing the break.
 *
 * The masses are large because the fixtures are: putting 6 MPa across 400 cm2 takes 24
 * tonnes. They are written as MassForStress so the stress, which is the number that
 * matters, is the number in the source.
 *
 * THE LAST TWO CASES DELIBERATELY BREAK THAT SHAPE, because it has a hidden property:
 * every one of the fixtures above is a TREE, so a break can only ever shorten a load
 * path. The two-tier fallback makes the opposite possible — losing a bed joint promotes
 * a piece's head joints, so breaking a joint ADDS edges to the support relation — and
 * those two cases are the only ones here where a survivor's load rises through a path
 * the break itself created. One settles; one closes a cycle and strands both pieces.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCascadeTest,
	"DestructionGame.Core.Structure.Cascade",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCascadeTest::RunTest(const FString& Parameters)
{
	using namespace StructureCascadeSupport;

	const TArray<FCascadeCase> Cases = {
		// THE CONTROL, and it has to come first: a genuinely loaded structure where
		// nothing gives. 5 MPa on cement mortar's 10 is half capacity — comfortably
		// loaded, comfortably standing.
		//
		// Without it every case below could be satisfied by an implementation that
		// breaks whatever it is shown.
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

		// EVERY JOINT OVER CAPACITY GIVES IN THE SAME PASS, in the form where nothing
		// can be confused with redistribution: three separate pieces, each on its own
		// pad, no interaction between them at all.
		//
		//   [p1]      [p3]      [p5]
		//    |         |         |     lime mortar, 100 cm2, 3 MPa on a 2 MPa bond
		//   ===       ===       ===
		//
		// 3 / 2 = 1.5x on every one of them. Under the policy under test all three
		// carry pass 1. Under strict worst-joint-first they would carry 1, 2 and 3 —
		// same final state, three times the solves, and a collapse that reads as a
		// sequence where there is none. That is the whole discrimination.
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

		// THE CONTROL THAT SETTLES, and the case a cascade test is worthless without: a
		// joint gives, its load moves onto the neighbour, and the structure then STANDS.
		// A test that cannot tell "collapsed" from "shed one joint and stabilised" is not
		// testing redistribution at all.
		//
		//        [ P ]           one piece, two pads
		//        /   \
		//   lime      cement     100 cm2 each, 200 cm2 in total
		//   ====      ======
		//
		// As built:  3 MPa on both -> lime 3/2 = 1.5x GIVES, cement 3/10 = 0.30 holds.
		// Settled:   the cement joint alone carries the lot over 100 cm2 -> 6 MPa,
		//            6/10 = 0.60, and it holds with 40% to spare.
		//
		// The load genuinely MOVED: the survivor carried half the weight before the
		// break and all of it after. An implementation that broke the lime joint and
		// left its share stranded would report 3 MPa here and the piece falling.
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

		// A PASS THAT BREAKS TWO, THEN A PASS THAT BREAKS ONE. The same shape as above
		// with the weak bed doubled, so the first pass is plural and the second is a
		// consequence of it.
		//
		//   lime 150 + lime 150 + cement 100 = 400 cm2, loaded to 6 MPa
		//
		// Pass 1:  6/2 = 3.0x on both lime joints -> both give together.
		//          6/10 = 0.60 on the cement joint -> holds.
		// Pass 2:  100 cm2 left, so 24 MPa; 24/10 = 2.4x -> the cement joint gives.
		// Pass 3:  nothing left to break, so the cascade ends after two passes.
		//
		// The load-bearing assertion is that the two lime joints share pass 1. Worst-
		// first would separate them (1 and 2, then 3) even though they are identical and
		// their utilisations are equal to the last bit.
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

		// REDISTRIBUTION ROUTED SIDEWAYS, AND THEN BACK DOWN. Every other cascade fixture
		// in this file is a tree, and the deepest post-break load path in any of them is
		// one hop. This is the direction the fallback rule makes possible and nothing
		// else covers: BREAKING A JOINT CAN ADD EDGES TO THE SUPPORT RELATION. A piece
		// with one bed joint and two head joints has exactly ONE support while the bed
		// joint holds, and TWO the moment it gives.
		//
		//   [pier p0] ==bolt 500== [ A p3 ] ==bolt 500== [ B p4 ]
		//                              |                     |
		//                          lime 100             cement 100
		//                         ==========           ===========
		//                             p1                    p2
		//
		// AS BUILT each of A and B stands on its own pad, and both bolted plates carry
		// EXACTLY ZERO — a bed joint beneath wins the tier outright over any number of
		// head joints, so the plates are not in the support relation at all.
		//
		//   pad c0   3 MPa against lime mortar's 2  -> 1.50x, GIVES in pass 1
		//   pad c1   3 MPa against cement's 10      -> 0.30, holds
		//
		// ONCE c0 HAS GONE A has no bed joint and falls back to BOTH plates. They are
		// 500 cm2 each so the split is even and each takes 1.5e6 uu, as PURE SHEAR — a
		// vertical load on a vertical face has no component along the normal.
		//
		//   c2  1.5e6 over 500 cm2 = 0.30 MPa; the bolt's cohesion is 1.0 and its mu is
		//       exactly 0, so capacity is a flat 1.0     -> 0.30, holds
		//   c3  the same 0.30 — but this half of A goes SIDEWAYS INTO B, and B then
		//       pushes it on down its own pad
		//   c1  now carries B's own 3e6 PLUS A's 1.5e6 = 4.5e6 over 100 cm2 = 4.5 MPa
		//                                               -> 0.45, and it still holds
		//
		// TWO THINGS ONLY THIS CASE COVERS.
		//
		// c3 NAMES THE LOADED PIECE FIRST, so what it stores is the equal-and-opposite
		// reaction and its force is POSITIVE. StructureTest.cpp pins that sign convention
		// for load VALUES, but nothing before this composes sign -> classify -> break:
		// this is the only positively-signed force in the suite that the break sweep then
		// evaluates, and the axis guard above insists it still resolves to pure shear
		// with zero tension. Get the sign wrong on a bed joint and mortar's 0.1 MPa
		// tensile limit gives at one percent of the real capacity, so the composition is
		// worth stating once.
		//
		// AND c1's LOAD RISES THROUGH A TWO-HOP PATH THE BREAK ITSELF CREATED. It is
		// conservation that makes that bite rather than the table alone: 6e6 of supported
		// weight has to reach the earth, and here it arrives as 4.5e6 + 1.5e6. An
		// implementation that let the broken pad keep winning A's tier leaves both plates
		// reading zero, c1 still at 3e6, and 3e6 of held-up weight arriving nowhere.
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
				{ -ForceForMPa(0.3, 500.0), EJointKind::Head, INDEX_NONE, 0.30 },
				{ +ForceForMPa(0.3, 500.0), EJointKind::Head, INDEX_NONE, 0.30 }
			},
			{ true, true, true, true, true }
		},

		// THE FALLBACK THAT MAKES A KNOT — the same rule as above reaching the opposite
		// outcome, because edges added to the support relation can close a CYCLE in it.
		//
		//   [pier p0] ==bolt== [ A p4 ] ==bolt== [ B p5 ] ==bolt== [pier p1]
		//                          |                 |
		//                      lime 100          lime 100
		//                     ==========        ==========
		//                         p2                p3
		//
		// Both pads sit at 3/2 = 1.50x and GIVE TOGETHER in pass 1. In pass 2 A has no
		// bed joint and falls back to {c2, c3}; B falls back to {c3, c4}. They now SHARE
		// c3, so each is the other's support through it, LoadReturnsToPiece fires on
		// both, and both are stranded — a knot the break itself created, in a structure
		// that had no cycle in it when it was built.
		//
		// A and B are therefore reported UNSUPPORTED and every joint carries nothing —
		// the three intact bolts included, which is the honest static answer (nothing is
		// holding that pair up, so there is no static load path) and also why nothing
		// more can break: an unloaded joint never gives, so the cascade stops at ONE
		// pass. It is not a claim the loop was solved; dividing load round a cycle needs
		// a rule DESIGN.md §3 says does not exist.
		//
		// THE SUPPORT FLAGS AND THE PASS COUNT ARE THE LOAD-BEARING ASSERTIONS HERE, and
		// both are asserted directly rather than left to the universal properties.
		// Conservation reads 0 = 0 on this shape — nothing claimed held up, nothing
		// arriving at the earth — so it is structurally blind to everything this case
		// exists for, exactly as CURRENT_STATE.md records. Read the four pieces still
		// standing and the two that are not; do not read the sum.
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

		// THE CASCADE PROPER, and the termination case: three joints, three profiles,
		// three passes, and nothing left standing. Each break is caused by the one
		// before it — the structure holds after pass 1 and after pass 2, and only the
		// third takes the piece down.
		//
		//   lime 250 + cement 100 + dry stone 50 = 400 cm2, loaded to 6 MPa
		//
		// As built:  6 MPa   lime 6/2 = 3.00x GIVES | cement 0.60 | dry stone 0.20
		// Pass 2:    150 cm2 left -> 16 MPa         | cement 1.60x GIVES | dry 0.53
		// Pass 3:     50 cm2 left -> 48 MPa                              | dry 1.60x GIVES
		//
		// Every survivor holds with at least 40% to spare and every break is at least
		// 60% over, so no row here is a near miss that a retune could flip quietly.
		//
		// TERMINATION IS THE POINT AS MUCH AS THE ORDER. Three passes over three
		// connections is the bound exactly — one joint per pass is the slowest a cascade
		// can possibly go, because a pass that breaks nothing is the last one. Note this
		// is a DIFFERENT bound from the one inside SolveLoads, which CURRENT_STATE.md
		// records as provably 2 and which runs nested inside each of these passes.
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
 * A JOINT THAT HAS GIVEN STOPS CONDUCTING SUPPORT — out of the load path, out of the
 * reachability walk, and out of the TIER DECISION.
 *
 * This is the first of phase 2a's deliberate omissions and it is the heart of
 * redistribution. SolveLoads builds each piece's support list from every connection
 * touching it and never consults HasGiven, which is exactly correct while nothing can
 * break and exactly wrong the moment something does.
 *
 * THE TIER DECISION IS THE PART THAT PRODUCES PLAUSIBLE-LOOKING WRONG NUMBERS, and it is
 * why this test exists separately from the cascade cases. Support is two-tiered
 * (DESIGN.md §3): a piece rests on the bed joints BENEATH it, and only a piece with NONE
 * falls back to its head joints. An implementation that filters broken joints out of the
 * load path but still lets them win the tier gives a piece whose bed joint has gone an
 * empty support list — so it reports the piece FALLING and its intact head joint
 * carrying ZERO, which is a perfectly self-consistent answer and a wall that collapses
 * when it should have leaned. The right answer is that the piece falls back to the head
 * joint and the joint takes the whole load in shear.
 *
 * BOTH DIRECTIONS ARE COVERED. Case 1 is the fallback: losing the bed joint must promote
 * the head joint. Case 2 is the opposite outcome from the same rule: a piece whose ONLY
 * support has gone has no path to the ground at all, so it and everything resting on it
 * are reported unsupported and their joints carry nothing.
 *
 * TWO DISSIMILAR PROFILES IN CASE 1, DELIBERATELY. The bed joint is lime mortar and the
 * head joint is a bolted plate, so the break and the survival are decided by different
 * numbers from different code sections and neither can be explained by the other being
 * mistuned. It is not a claim that anybody builds walls this way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCascadeStopsConductingTest,
	"DestructionGame.Core.Structure.CascadeStopsConducting",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCascadeStopsConductingTest::RunTest(const FString& Parameters)
{
	using namespace StructureCascadeSupport;

	const TArray<FCascadeCase> Cases = {
		// THE TIER FALLBACK.
		//
		//                  [ P ]  —— bolted plate, 500 cm2 ——  [ pier ]
		//                    |                                  ======
		//               lime pad, 100 cm2
		//              ==============
		//
		// As built P has a bed joint beneath it, so that joint wins the tier outright and
		// is P's ONLY support: the head joint carries exactly zero and cannot break.
		//
		//   pad   3 MPa of compression against lime mortar's 2 MPa -> 1.50x, GIVES
		//   plate carries nothing, so its utilisation is 0 and it survives pass 1
		//
		// Once the pad has gone P has no bed joint beneath it at all and falls back to
		// its head joint, which then takes the WHOLE 3e6 uu — as pure shear, because a
		// vertical load on a vertical face has no component along the normal.
		//
		//   plate 3e6 uu over 500 cm2 = 0.60 MPa of shear
		//         bolt cohesion is 1.0 MPa and mu is exactly 0, so friction adds
		//         nothing and the capacity is 1.0 flat -> 0.60, holds with 40% spare
		//
		// A broken joint still winning the tier reports P FALLING with 0 on the plate.
		// A broken joint still conducting reports the pad carrying 3e6 and nothing
		// broken downstream. Both are excluded here by the same table.
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
				{ -ForceForMPa(3.0, 100.0), EJointKind::Head, INDEX_NONE, 0.6 }
			},
			{ true, true, true }
		},

		// THE OTHER HALF OF THE SAME RULE: a broken joint conducts nothing to the
		// reachability walk either, so what rested on it is falling too.
		//
		//        [ B ]     1 MPa on cement mortar -> 0.10, nowhere near giving
		//          |
		//        [ M ]
		//          |       lime pad carrying B and M together: 3 MPa -> 1.50x, GIVES
		//        =====
		//
		// After pass 1 M has no support left, so M and B both come out unsupported and
		// BOTH joints carry zero — including the intact one, which is right for a static
		// solver: nothing is holding that island up, so there is no static load path to
		// report. It is also why the cascade stops here rather than running on: an intact
		// joint on a falling island is unloaded, and an unloaded joint never gives.
		//
		// An implementation that let the broken pad keep conducting reports M and B
		// standing on a joint that is no longer there.
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
 * The shapes where a cascade could quietly do something indefensible, and the
 * fail-closed answers it has to give instead.
 *
 * Every case here runs the same universal properties as the cascade cases — it
 * terminates, no pass is empty, a broken joint carries nothing, an intact one is within
 * capacity, conservation holds, and a second run changes nothing — over structures
 * chosen so that a plausible implementation could get each one wrong in a different way:
 *
 *  - NOTHING TO DO AT ALL. An empty structure and a structure with no joints. A cascade
 *    that indexed a parallel array by pass number would be at its most fragile here.
 *  - A JOINT THAT MUST NOT BE ALLOWED TO GIVE. The shared Unbreakable fixture under
 *    twenty tonnes: 19.6 MPa against a strength of 1e12, so the utilisation is 2e-11 and
 *    an implementation that breaks on "loaded a lot" rather than on "over ITS capacity"
 *    fails here and nowhere else.
 *  - AN ISLAND IN FREEFALL. Two ungrounded pieces stacked: nothing holds them up, so the
 *    static load path is empty, every joint carries zero and NOTHING BREAKS. That is the
 *    honest answer for a static solver and it is worth stating, because "carries nothing"
 *    is ambiguous between given, grounded on both sides, and falling.
 *  - A JOINT THAT BEARS NOTHING. A piece glued UNDER a grounded slab: the bed joint is
 *    the wrong way round to hold it up, so the piece is unsupported and the joint is
 *    unloaded whatever the slab weighs.
 *  - A JOINT BETWEEN TWO GROUNDED PIECES. The earth terminates the flow on both sides,
 *    so there is nothing here to break however the pieces are loaded.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCascadeDegenerateInputTest,
	"DestructionGame.Core.Structure.CascadeDegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCascadeDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace StructureCascadeSupport;

	// 20 tonnes over 100 cm2 is 19.6 MPa, which is past every real profile in the
	// library and nowhere near the test fixture's 1e12. Spelled from the SI definitions
	// so the expectation does not inherit a wrong conversion constant.
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

		// A JOINT NAMING A PIECE THAT DOES NOT EXIST. AddConnection rejects it at the
		// door, so the structure holds no connections at all and the piece it was meant
		// to hold up is simply unsupported — which is the fail-closed answer.
		//
		// IT IS HERE FOR THE HARNESS AS MUCH AS FOR THE SOLVER. The checker has to reach
		// that conclusion by reporting a failure, not by taking the process down: the
		// conservation sum below used to read Case.Spec.Pieces[Spec.PieceB] straight off
		// the spec, and INDEX_NONE there indexes Pieces[-1]. The arity assertions above
		// are TestTrue, which is non-fatal, so execution runs straight on past them into
		// it. A harness that crashes instead of failing is worse than no harness.
		//
		// THE UNGROUNDED PIECE COMES FIRST, AND THAT IS NOT COSMETIC. The sum's test was
		// `Pieces[PieceA].bIsGrounded || Pieces[PieceB].bIsGrounded`, and || short-
		// circuits — so a row naming a GROUNDED piece first never evaluates the second
		// handle and sails past the defect it was written to catch. Piece 1 is the
		// ungrounded one, so the first operand is false and the bad index is reached.
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
