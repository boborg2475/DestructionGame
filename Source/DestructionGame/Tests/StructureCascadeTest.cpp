// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named, not anonymous, and named differently from every other namespace in this
 * directory. An anonymous namespace is private to a translation unit rather than to a
 * file, and a unity build merges many files into one — at which point two anonymous
 * BedJointNormal declarations are the same declaration twice. See CURRENT_STATE.md; the
 * `using namespace` lives inside each RunTest body for the same reason.
 */
namespace StructureCascadeSupport
{
	using namespace DestructionProfiles;

	/**
	 * Unreal's gravity, 980 cm/s2, spelled out rather than imported from production so
	 * this file fails if production gets it wrong instead of agreeing with it.
	 *
	 * The unit trap: mass is already kg and length already cm, so MassKg * 980 IS a
	 * force in Unreal units — the 1 N = 100 uu conversion is baked into the 980, not
	 * applied on top of it.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double WeightOf(double MassKg)
	{
		return MassKg * GravityCmPerSecondSquared;
	}

	/**
	 * Force, in Unreal units, that loads the given area to the given stress. Spelled
	 * from the SI definitions rather than reusing ForceUnitsPerMPaSqCm, so a wrong
	 * conversion constant in production fails here instead of agreeing: 1 N = 100 uu,
	 * 1 cm2 = 100 mm2, 1 MPa = 1 N/mm2, so one MPa over one cm2 is 10000 uu.
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
	 * Fixtures are written in stress because that is what decides whether a joint gives
	 * and the only number checkable against a published strength by eye:
	 * MassForStress(6.0, 400.0) says what the fixture is for, 24489.795918 kg does not.
	 */
	constexpr double MassForStress(double MPa, double AreaSqCm)
	{
		return ForceForMPa(MPa, AreaSqCm) / GravityCmPerSecondSquared;
	}

	/**
	 * A standard clay brick, 2.72 kg, used only for the grounded pads and piers — their
	 * weight terminates at the earth and never reaches a joint, so the value is
	 * immaterial and a real one is less distracting than zero.
	 */
	constexpr double BrickMassKg = 2.72;

	/** Horizontal interface, normal pointing up at the piece above: a bed joint. */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Vertical interface, normal pointing sideways at the neighbour: a head joint. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);

	/**
	 * What a joint does with the load it is handed, and therefore which capacity the
	 * break decision is measured against.
	 *
	 * The axis-governance guard. ComputeUtilisation returns the worst of three axes, so
	 * a fixture aimed at compression that is actually governed by shear breaks for a
	 * reason the test did not intend. Every fixture here loads exactly one non-zero axis
	 * — a vertical force on an axis-aligned bed joint is pure compression, on an
	 * axis-aligned head joint pure shear — and that is asserted per joint below rather
	 * than assumed.
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
	 * A joint as data, including its own strength.
	 *
	 * StructureTest.cpp's builder gives every joint the same profile, which is right
	 * there — it measures routing and wants nothing to break. Here the order joints give
	 * in is the thing under test, and with the area-weighted split every parallel support
	 * of one piece carries the same stress whatever its area (share is proportional to
	 * area, so the area cancels). Identical profiles would give identical utilisations
	 * and break everything in one pass; differing strengths make the sequence exist.
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
		 * Signed Z of the force the joint carries in the settled structure, uu. Zero for
		 * a joint that has given, since a broken joint is out of the structure. Per
		 * ConnectionLoad.h the force belongs to PieceB, so a joint that names the loaded
		 * piece first stores the equal-and-opposite reaction and is positive.
		 */
		double FinalForceZUU = 0.0;

		EJointKind Kind = EJointKind::Bed;

		/**
		 * The breaking pass that gave this joint, counted from 1, or INDEX_NONE if it
		 * must still be intact when everything settles.
		 *
		 * Order within a pass is not asserted anywhere and must not be: every joint over
		 * capacity gives together, so two joints sharing a number are unordered. Order
		 * between passes is real, and it is what a collapse looks like played back.
		 */
		int32 BreakPass = INDEX_NONE;

		/**
		 * Utilisation of the settled load, hand-derived from the published strength in
		 * each case's comment. Zero for a broken joint. For an intact one it is the
		 * margin — how far the survivor is from giving — so a fixture that only just
		 * held cannot drift into only just failing unnoticed.
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
	 * Build a structure in place. By reference: FConnection is copyable and its
	 * "has given" latch is per-copy, so anything evaluating connections by value latches
	 * a temporary and leaves the real joint untouched — see CheckCascadeCase.
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
	 * Build, cascade, and check one case against its table and against the properties
	 * that must hold for any structure at all.
	 *
	 * The copyable-latch trap, and the assertion that closes it: FConnection is copyable
	 * and the latch is a private bool on the object, so
	 *
	 *     for (FConnection C : Connections) { C.ApplyForce(...); }
	 *
	 * — one missing ampersand — compiles clean, latches every overloaded joint on the
	 * temporary, and leaves the real connections untouched, so the structure reports zero
	 * broken joints under any load and never comes apart. Every assertion below is made
	 * after SolveAndBreak returns, through GetConnection, which hands back a reference to
	 * the connection the structure owns — so a cascade that happened only to copies reads
	 * as a structure where nothing broke.
	 *
	 * From the table: the number of passes; which joints gave and in which pass; what
	 * each carries once settled, and on which axis; how hard each survivor is working;
	 * which pieces are still held up.
	 *
	 * Universally, whatever the table says:
	 *
	 *  - it terminates within one pass per joint — joints never heal and a pass that
	 *    breaks nothing is the last. (Not SolveLoads' own internal bound, provably 2 per
	 *    CURRENT_STATE.md, which nests inside each pass here.)
	 *  - every pass broke something; a gap in the numbering is a loop that does not settle;
	 *  - a broken joint carries nothing and an intact one has no pass stamp;
	 *  - nothing survives over capacity, recomputed from the reported force and profile;
	 *  - ground-reaction conservation against the solver's own final claim — short means
	 *    load was stranded, long means a share was counted twice;
	 *  - calling it again changes nothing: the latch being total, observed from outside.
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
		 * A relative allowance for everything else: MassForStress divides by 980 and the
		 * solver multiplies by 980 again, so the settled loads carry a rounding error
		 * proportional to their own size rather than a fixed one.
		 */
		constexpr double RelativeUtilisationTolerance = 1.0e-9;

		FStructure Structure;
		BuildStructure(Structure, Case.Spec);

		/*
		 * The fixture itself, first: a spec rejected at the door would make every
		 * assertion below a statement about a structure that was never built.
		 */
		Test.TestTrue(
			FString::Printf(TEXT("%s: expected %d pieces, got %d"),
				Case.Description, Case.Spec.Pieces.Num(), Structure.NumPieces()),
			Structure.NumPieces() == Case.Spec.Pieces.Num());

		/*
		 * The table, not the spec, is the authority on what should have been accepted.
		 * For real joints the two counts agree, so this is exactly as strong; the
		 * difference is that a fixture meaning to describe a rejected joint can say so by
		 * giving it no expectation row, which the spec count could not express.
		 */
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

		/*
		 * Termination as a bound rather than a hope: every pass but the last breaks at
		 * least one joint and joints never heal, so the process cannot run longer than
		 * there are joints to break.
		 */
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

			// GetConnection returns a reference the structure owns: the real latch, not a copy.
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

			/*
			 * The two answers have to be one answer: a joint stamped with a pass yet
			 * reporting itself intact — or the reverse — is the copyable-latch defect with
			 * a bookkeeping array papered over the top of it.
			 */
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

			/*
			 * Gravity does not change direction because a joint happens to be vertical,
			 * nor acquire one because a neighbouring joint gave.
			 */
			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d load must be vertical and finite, got (%f, %f, %f)"),
					Case.Description, Index, Force.X, Force.Y, Force.Z),
				!Force.ContainsNaN()
					&& FMath::IsFinite(Force.X) && FMath::IsFinite(Force.Y) && FMath::IsFinite(Force.Z)
					&& FMath::IsNearlyZero(Force.X, Tolerance) && FMath::IsNearlyZero(Force.Y, Tolerance));

			/*
			 * A broken joint carries nothing — that zero is what redistribution means. One
			 * still reporting a share is a wall standing on a joint it has already shed.
			 */
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

			/*
			 * The axis-governance guard: exactly one axis carries anything, so the
			 * utilisation below can only have come from the capacity this fixture aimed at.
			 */
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
			 * Relative, with a floor far below the smallest expectation in any table. A
			 * flat absolute 1e-9 swallowed whole expectations: the Unbreakable fixture
			 * settles at 1.96e-11, so everything in [-1e-9, 1e-9] passed — including exactly
			 * zero, the answer a joint dropped out of the load path gives. The floor serves
			 * the rows expecting an exact 0 and must stay well under the smallest non-zero
			 * expectation in this file, or that row stops being able to fail.
			 */
			const double UtilisationSlack = FMath::Max(
				UtilisationTolerance, RelativeUtilisationTolerance * FMath::Abs(Expected.FinalUtilisation));

			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d should settle at utilisation %.12g, got %.12g"),
					Case.Description, Index, Expected.FinalUtilisation, Utilisation),
				FMath::IsNearlyEqual(Utilisation, Expected.FinalUtilisation, UtilisationSlack));

			/*
			 * Nothing survives over capacity — the definition of "settled". Written
			 * !(x <= 1) rather than x > 1 so a NaN utilisation fails here instead of
			 * slipping through as a joint that is comfortably fine.
			 */
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

		/*
		 * Ground-reaction conservation, against the solver's own final claim about what it
		 * holds up rather than against the table, so it keeps its force if the
		 * expectations above are ever revisited.
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
			/*
			 * Over the joints the structure actually holds, taking piece handles from it
			 * rather than assuming spec row N is structure joint N. Walking the spec instead
			 * crashed on the first row naming a piece that does not exist: AddConnection
			 * rejects the row, but the harness still read Pieces[INDEX_NONE] — and the arity
			 * assertions above are non-fatal TestTrue, so execution ran straight into it.
			 * AddConnection validates both handles, so the ones read here are in range.
			 *
			 * Grounded-ness still comes from the spec, so this stays a statement about the
			 * fixture rather than about what production chose to store.
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

		/*
		 * Out-of-range handles fail closed rather than reading as a joint that broke in
		 * some pass nobody ran.
		 */
		Test.TestTrue(
			FString::Printf(TEXT("%s: an unknown connection has no break pass, got %d"),
				Case.Description, Structure.GetBreakPass(Structure.NumConnections())),
			Structure.GetBreakPass(Structure.NumConnections()) == INDEX_NONE);

		Test.TestTrue(
			FString::Printf(TEXT("%s: connection INDEX_NONE has no break pass, got %d"),
				Case.Description, Structure.GetBreakPass(INDEX_NONE)),
			Structure.GetBreakPass(INDEX_NONE) == INDEX_NONE);

		/*
		 * Settled is a claim about the second call, not only the first: nothing more gives,
		 * nothing un-gives, no stamp is rewritten and no load moves. A structure that
		 * changed here is either re-breaking what it broke or recovering a joint mid-collapse.
		 */
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
			/*
			 * Bit for bit, not nearly-equal: the second run repeats identical arithmetic on
			 * identical inputs, so any difference is carried state rather than rounding.
			 */
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
 * A structure breaks every joint over capacity, moves their load onto the neighbours, and
 * repeats until it settles or has come apart — recording which pass each joint gave in.
 *
 * The policy these cases encode: break every joint over capacity in the same pass, stamp
 * each with that pass number, re-solve, repeat. Order within a pass is arbitrary and
 * deliberately never asserted; order between passes is real, and it is the sequence a
 * collapse plays back as. Chosen over strict worst-joint-first, which costs a solve per
 * joint for the same end state, and over unordered all-at-once, which loses the sequence.
 *
 * No world, no positions, nothing moves. As in StructureTest.cpp gravity is an input to
 * the arithmetic rather than something a simulation applies, and every assertion is on the
 * mechanism: which joints report themselves given, in which pass, what the survivors carry,
 * which pieces are still held up. DESIGN.md §4 is emphatic that displacement is never a
 * valid break assertion — two pieces can sever and stay resting exactly in place.
 *
 * Every case hangs one heavy piece off several grounded pads through pure bed joints,
 * because with the area-weighted split every parallel support carries the same stress
 * whatever its area (the share is proportional to area, so the area cancels). That shape
 * buys three things:
 *
 *  - a joint gives or holds on its profile alone, so the sequence reads straight off
 *    published compressive strengths — lime mortar 2 MPa, cement 10, dry stone 30;
 *  - removing one support shrinks the total area, so every survivor's stress rises by a
 *    factor this test computes exactly: redistribution in its most checkable form;
 *  - a vertical load on an axis-aligned bed joint is pure compression, so no other axis
 *    can be quietly governing the break.
 *
 * The masses are large because the fixtures are — 6 MPa across 400 cm2 takes 24 tonnes —
 * and are written as MassForStress so the stress is the number in the source.
 *
 * The last two cases deliberately break that shape: every fixture above is a tree, so a
 * break can only shorten a load path. The two-tier fallback makes the opposite possible —
 * losing a bed joint promotes a piece's head joints, so breaking a joint adds edges to the
 * support relation. One settles; one closes a cycle and strands both pieces.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCascadeTest,
	"DestructionGame.Core.Structure.Cascade",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCascadeTest::RunTest(const FString& Parameters)
{
	using namespace StructureCascadeSupport;

	const TArray<FCascadeCase> Cases = {
		/*
		 * The control, and it has to come first: a genuinely loaded structure where
		 * nothing gives — 5 MPa on cement mortar's 10 is half capacity. Without it every
		 * case below could be satisfied by an implementation that breaks whatever it sees.
		 */
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

		/*
		 * Every joint over capacity gives in the same pass, in the form where nothing can
		 * be confused with redistribution: three separate pieces, each on its own pad.
		 *
		 *   [p1]      [p3]      [p5]
		 *    |         |         |     lime mortar, 100 cm2, 3 MPa on a 2 MPa bond
		 *   ===       ===       ===
		 *
		 * 3 / 2 = 1.5x on all three, so all three carry pass 1. Worst-joint-first would
		 * stamp them 1, 2 and 3 — same end state, three times the solves, and a collapse
		 * that reads as a sequence where there is none.
		 */
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
		 * The control that settles: a joint gives, its load moves onto the neighbour, and
		 * the structure stands. A test that cannot tell "collapsed" from "shed one joint
		 * and stabilised" is not testing redistribution at all.
		 *
		 *        [ P ]           one piece, two pads
		 *        /   \
		 *   lime      cement     100 cm2 each, 200 cm2 in total
		 *   ====      ======
		 *
		 * As built:  3 MPa on both -> lime 3/2 = 1.5x GIVES, cement 3/10 = 0.30 holds.
		 * Settled:   the cement joint alone carries the lot over 100 cm2 -> 6 MPa,
		 *            6/10 = 0.60, and it holds with 40% to spare.
		 *
		 * An implementation that broke the lime joint and left its share stranded would
		 * report 3 MPa here and the piece falling.
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
		 * A pass that breaks two, then a pass that breaks one — the shape above with the
		 * weak bed doubled, so the first pass is plural and the second follows from it.
		 *
		 *   lime 150 + lime 150 + cement 100 = 400 cm2, loaded to 6 MPa
		 *
		 * Pass 1:  6/2 = 3.0x on both lime joints -> both give together.
		 *          6/10 = 0.60 on the cement joint -> holds.
		 * Pass 2:  100 cm2 left, so 24 MPa; 24/10 = 2.4x -> the cement joint gives.
		 * Pass 3:  nothing left to break, so the cascade ends after two passes.
		 *
		 * The load-bearing assertion is that the two lime joints share pass 1; worst-first
		 * would separate them though their utilisations are equal to the last bit.
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
		 * Redistribution routed sideways, and then back down. Every other fixture here is
		 * a tree whose deepest post-break path is one hop; this is the direction the
		 * fallback rule makes possible and nothing else covers — breaking a joint can add
		 * edges to the support relation. A piece with one bed joint and two head joints
		 * has one support while the bed joint holds, and two the moment it gives.
		 *
		 *   [pier p0] ==bolt 500== [ A p3 ] ==bolt 500== [ B p4 ]
		 *                              |                     |
		 *                          lime 100             cement 100
		 *                         ==========           ===========
		 *                             p1                    p2
		 *
		 * As built, A and B each stand on their own pad and both bolted plates carry
		 * exactly zero: a bed joint beneath wins the tier outright over any number of head
		 * joints, so the plates are not in the support relation at all.
		 *
		 *   pad c0   3 MPa against lime mortar's 2  -> 1.50x, GIVES in pass 1
		 *   pad c1   3 MPa against cement's 10      -> 0.30, holds
		 *
		 * Once c0 has gone A has no bed joint and falls back to both plates, 500 cm2 each,
		 * so the split is even and each takes 1.5e6 uu as pure shear — a vertical load on
		 * a vertical face has no component along the normal.
		 *
		 *   c2  1.5e6 over 500 cm2 = 0.30 MPa; the bolt's cohesion is 1.1 (mean basis,
		 *       re-anchor 2026-08-13) and its mu is exactly 0, so capacity is a flat
		 *       1.1                                       -> 0.2727, holds
		 *   c3  the same 0.2727 — but this half of A goes sideways into B, and B then
		 *       pushes it on down its own pad
		 *   c1  now carries B's own 3e6 plus A's 1.5e6 = 4.5e6 over 100 cm2 = 4.5 MPa
		 *                                               -> 0.45, and it still holds
		 *
		 * Two things only this case covers. c3 names the loaded piece first, so it stores
		 * the equal-and-opposite reaction and its force is positive: this is the only
		 * positively-signed force in the suite that the break sweep then evaluates, and the
		 * axis guard insists it still resolves to pure shear with zero tension. Get the
		 * sign wrong on a bed joint and mortar's 0.7 MPa tensile limit gives at a fraction
		 * of the real capacity.
		 *
		 * And c1's load rises through a two-hop path the break itself created, which
		 * conservation makes bite: 6e6 of supported weight has to reach the earth, and here
		 * it arrives as 4.5e6 + 1.5e6. Letting the broken pad keep winning A's tier leaves
		 * both plates at zero, c1 still at 3e6, and 3e6 of held-up weight arriving nowhere.
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
		 * The fallback that makes a knot — the same rule reaching the opposite outcome,
		 * because edges added to the support relation can close a cycle in it.
		 *
		 *   [pier p0] ==bolt== [ A p4 ] ==bolt== [ B p5 ] ==bolt== [pier p1]
		 *                          |                 |
		 *                      lime 100          lime 100
		 *                     ==========        ==========
		 *                         p2                p3
		 *
		 * Both pads sit at 3/2 = 1.50x and give together in pass 1. In pass 2 A falls back
		 * to {c2, c3} and B to {c3, c4}: they now share c3, so each is the other's support
		 * through it, LoadReturnsToPiece fires on both, and both are stranded — a knot the
		 * break created in a structure that had no cycle when it was built.
		 *
		 * A and B are therefore unsupported and every joint carries nothing, the three
		 * intact bolts included: nothing is holding that pair up, so there is no static
		 * load path, and an unloaded joint never gives — which is why the cascade stops at
		 * one pass. It is not a claim the loop was solved; dividing load round a cycle
		 * needs a rule DESIGN.md §3 says does not exist.
		 *
		 * The support flags and the pass count are the load-bearing assertions, and both
		 * are stated directly rather than left to the universal properties: conservation
		 * reads 0 = 0 on this shape and is blind to everything the case exists for, as
		 * CURRENT_STATE.md records. Read the pieces standing, not the sum.
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
		 * The cascade proper, and the termination case: three joints, three profiles,
		 * three passes, nothing left standing. Each break is caused by the one before it —
		 * the structure holds after passes 1 and 2, and only the third takes the piece down.
		 *
		 *   lime 250 + cement 100 + dry stone 50 = 400 cm2, loaded to 6 MPa
		 *
		 * As built:  6 MPa   lime 6/2 = 3.00x GIVES | cement 0.60 | dry stone 0.20
		 * Pass 2:    150 cm2 left -> 16 MPa         | cement 1.60x GIVES | dry 0.53
		 * Pass 3:     50 cm2 left -> 48 MPa                              | dry 1.60x GIVES
		 *
		 * Every survivor holds with at least 40% to spare and every break is at least 60%
		 * over, so no row is a near miss a retune could flip quietly.
		 *
		 * Termination is the point as much as the order: three passes over three
		 * connections is the bound exactly, one joint per pass being the slowest a cascade
		 * can go. A different bound from the one inside SolveLoads, which CURRENT_STATE.md
		 * records as provably 2 and which nests inside each of these passes.
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
 * A joint that has given stops conducting support — out of the load path, out of the
 * reachability walk, and out of the tier decision.
 *
 * The tier decision is the part that produces plausible-looking wrong numbers, which is
 * why this test is separate from the cascade cases. Support is two-tiered (DESIGN.md §3):
 * a piece rests on the bed joints beneath it, and only a piece with none falls back to its
 * head joints. An implementation that filters broken joints out of the load path but still
 * lets them win the tier hands a piece whose bed joint has gone an empty support list, and
 * reports it falling with its intact head joint at zero — self-consistent, and a wall that
 * collapses when it should have leaned. The right answer is fallback to the head joint,
 * which takes the whole load in shear.
 *
 * Both directions are covered: case 1 is the fallback, case 2 the opposite outcome from
 * the same rule — a piece whose only support has gone has no path to the ground, so it and
 * everything resting on it are unsupported and their joints carry nothing.
 *
 * Case 1 mixes lime mortar with a bolted plate deliberately, so the break and the survival
 * are decided by different numbers and neither can be explained by the other being
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
		/*
		 * The tier fallback.
		 *
		 *                  [ P ]  —— bolted plate, 500 cm2 ——  [ pier ]
		 *                    |                                  ======
		 *               lime pad, 100 cm2
		 *              ==============
		 *
		 * As built the bed joint wins the tier outright and is P's only support, so the
		 * head joint carries exactly zero and cannot break.
		 *
		 *   pad   3 MPa of compression against lime mortar's 2 MPa -> 1.50x, GIVES
		 *   plate carries nothing, so its utilisation is 0 and it survives pass 1
		 *
		 * Once the pad has gone P falls back to its head joint, which takes the whole 3e6
		 * uu as pure shear — a vertical load on a vertical face has no normal component.
		 *
		 *   plate 3e6 uu over 500 cm2 = 0.60 MPa of shear
		 *         bolt cohesion is 1.1 MPa (mean basis, re-anchor 2026-08-13) and mu is
		 *         exactly 0, so friction adds nothing and the capacity is 1.1 flat
		 *         -> 0.5455, holds with 45% spare
		 *
		 * A broken joint still winning the tier reports P falling with 0 on the plate; one
		 * still conducting reports the pad carrying 3e6. The same table excludes both.
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
		 * The other half of the same rule: a broken joint conducts nothing to the
		 * reachability walk either, so what rested on it is falling too.
		 *
		 *        [ B ]     1 MPa on cement mortar -> 0.10, nowhere near giving
		 *          |
		 *        [ M ]
		 *          |       lime pad carrying B and M together: 3 MPa -> 1.50x, GIVES
		 *        =====
		 *
		 * After pass 1 M has no support left, so M and B come out unsupported and both
		 * joints carry zero — the intact one included, which is right for a static solver
		 * with no load path to report, and also why the cascade stops here: an unloaded
		 * joint never gives.
		 *
		 * Letting the broken pad keep conducting reports M and B standing on a joint that
		 * is no longer there.
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
 * The shapes where a cascade could quietly do something indefensible, and the
 * fail-closed answers it has to give instead.
 *
 * Every case runs the same universal properties as the cascade cases — it terminates, no
 * pass is empty, a broken joint carries nothing, an intact one is within capacity,
 * conservation holds, a second run changes nothing — over structures a plausible
 * implementation could get wrong in a different way each time:
 *
 *  - nothing to do at all: an empty structure, and one with no joints — where a cascade
 *    indexing a parallel array by pass number is at its most fragile;
 *  - a joint that must not give: Unbreakable under twenty tonnes is 19.6 MPa against 1e12,
 *    so anything breaking on "loaded a lot" rather than "over its capacity" fails here;
 *  - an island in freefall: two ungrounded pieces, so the static load path is empty, every
 *    joint carries zero and nothing breaks. Worth stating, because "carries nothing" is
 *    ambiguous between given, grounded on both sides, and falling;
 *  - a joint that bears nothing: a piece glued under a grounded slab, so the bed joint is
 *    the wrong way round and is unloaded whatever the slab weighs;
 *  - a joint between two grounded pieces: the earth terminates the flow on both sides.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCascadeDegenerateInputTest,
	"DestructionGame.Core.Structure.CascadeDegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCascadeDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace StructureCascadeSupport;

	/*
	 * 20 tonnes over 100 cm2 is 19.6 MPa: past every real profile in the library and
	 * nowhere near the fixture's 1e12. Spelled from the SI definitions so the expectation
	 * does not inherit a wrong conversion constant.
	 */
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
		 * A joint naming a piece that does not exist. AddConnection rejects it at the door,
		 * so the structure holds no connections and the piece it was meant to hold up is
		 * unsupported — the fail-closed answer.
		 *
		 * It is here for the harness as much as the solver: the conservation sum used to
		 * read Case.Spec.Pieces[Spec.PieceB] straight off the spec, where INDEX_NONE
		 * indexes Pieces[-1], and the arity assertions above are non-fatal TestTrue, so
		 * execution ran straight into it. A harness that crashes is worse than none.
		 *
		 * The ungrounded piece comes first, and that is not cosmetic: the sum's test was
		 * `Pieces[PieceA].bIsGrounded || Pieces[PieceB].bIsGrounded`, and || short-circuits,
		 * so a row naming a grounded piece first never evaluates the second handle. Piece 1
		 * is ungrounded, so the first operand is false and the bad index is reached.
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
