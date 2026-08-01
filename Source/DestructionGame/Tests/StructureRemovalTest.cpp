// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named differently from every other one in this
 * directory. An anonymous namespace is private to a TRANSLATION UNIT rather than to a
 * file, and a unity build merges many files into one — at which point two anonymous
 * BedJointNormal declarations are the same declaration twice. See CURRENT_STATE.md;
 * the `using namespace` for this one lives inside each RunTest body for the same
 * reason.
 */
namespace StructureRemovalSupport
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

	/**
	 * Mass of a piece whose whole weight loads the given area to the given stress.
	 *
	 * Fixtures are written in terms of STRESS rather than of mass, because stress is
	 * what decides whether a joint gives and it is the only number a reader can check
	 * against a published strength by eye. Every expected utilisation below therefore
	 * follows by construction and survives a retune of the profile it is measured
	 * against — which is why the cascade fixtures rode out the mortar shear cap moving
	 * from 1.4 to 1.3 without an edit.
	 */
	constexpr double MassForStress(double MPa, double AreaSqCm)
	{
		return ForceForMPa(MPa, AreaSqCm) / GravityCmPerSecondSquared;
	}

	/** A standard UK metric brick, 215 x 102.5 x 65 mm, in cm3. */
	constexpr double BrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/**
	 * A standard clay brick, DERIVED rather than hand-set: 1432.44 cm3 at 1.9 g/cm3
	 * is 2.7216 kg.
	 *
	 * NO EXPECTATION IN THIS FILE DEPENDS ON THE VALUE, which is a claim about two
	 * different usages rather than one. Mostly it is the GROUNDED pads and piers, whose
	 * own weight terminates at the earth and never reaches a joint. RemovalDegenerateInputs
	 * also gives it to two UNGROUNDED pieces, and that block asserts support, liveness and
	 * finiteness only — never a force — so a zero brick would change no answer there
	 * either. A real number is simply less distracting than zero.
	 *
	 * IT IS DERIVED rather than hand-set because a copy is a second place for the number to
	 * drift, which is exactly what happened to the 2.72 that used to sit in
	 * StructureTest.cpp. Profiles.MaterialInvariants is the one external anchor: it checks
	 * density x volume against what a brick weighs in the hand, and everything else hangs
	 * off that single check.
	 *
	 * SAFE AT NAMESPACE SCOPE ONLY BECAUSE ClayBrick IS AN AGGREGATE OF LITERALS, so it is
	 * constant-initialised and ready before any dynamic initialisation in any translation
	 * unit. Make it a computed ratio of StructuralConcrete and static initialisation order
	 * across translation units becomes unspecified, at which point this could silently read
	 * ZERO. RemovalDegenerateInputs opens with a guard against exactly that, as
	 * StructureTest.cpp's LoadPath does; it is not a second claim about what a brick weighs.
	 */
	const double BrickMassKg = ClayBrick.DensityGramsPerCubicCm * BrickVolumeCubicCm / 1000.0;

	/** Horizontal interface, normal pointing up at the piece above: a bed joint. */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Vertical interface, normal pointing sideways at the neighbour: a head joint. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);

	/**
	 * What a joint does with the load it is handed, and therefore WHICH CAPACITY the
	 * expected utilisation is measured against.
	 *
	 * The axis-governance guard, and it is not decoration. ComputeUtilisation returns
	 * the WORST of three axes, so a fixture aimed at compression that happens to be
	 * governed by shear proves nothing about the axis it was written for. Every joint in
	 * this file is axis-aligned and carries a vertical load, so exactly one axis is
	 * non-zero — pure compression on a bed joint, pure shear on a head joint — and that
	 * is asserted per joint rather than assumed.
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
	 * Build a structure in place.
	 *
	 * BY REFERENCE. FConnection is copyable and its "has given" latch is per-copy, so
	 * anything that moves connections around by value risks latching a temporary and
	 * leaving the real joint untouched — a wall that never falls. See CURRENT_STATE.md.
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
		 * SIGNED Z of the force the joint carries, in Unreal force units.
		 *
		 * Zero for a joint that has gone, whether it was overloaded or removed: a joint
		 * out of the structure carries nothing, and that zero is what redistribution is
		 * built on. Per ConnectionLoad.h the force belongs to PieceB, so a joint that
		 * names the loaded piece first stores the reaction and is positive.
		 */
		double ForceZUU = 0.0;

		EJointKind Kind = EJointKind::Bed;

		/**
		 * Whether the joint is out of the structure, HOWEVER it went.
		 *
		 * Separate from BreakPass on purpose — see the note below. HasGiven is the
		 * liveness question and a removed joint answers it true while carrying no pass
		 * number at all.
		 */
		bool bGone = false;

		/**
		 * Which CASCADE PASS broke this joint, counted from 1, or INDEX_NONE if no pass
		 * did — including for a joint that went because a piece it held was removed.
		 *
		 * NO SENTINEL, AND THAT IS THE DECISION THIS FILE ENCODES. ConnectionBreakPass is
		 * the recorded order phase 5's visualisation plays back, and a joint that vanished
		 * with its brick did not snap: stamping it makes the replay show a failure that
		 * never happened, and a distinguishable-but-present stamp only moves the problem
		 * to every consumer of the sequence, each of which then has to know to skip it.
		 *
		 * The obvious objection is that INDEX_NONE was documented as meaning "still
		 * intact", so leaving a removal unstamped recreates the GetBreakPass / HasGiven
		 * disagreement recorded in CURRENT_STATE.md. That is a defect in the CONTRACT
		 * rather than in the value: GetBreakPass answers "which pass broke this", and a
		 * removed joint has no answer. Liveness is a different question with its own
		 * accessor, and the pair is a complete encoding with nothing ambiguous in it:
		 *
		 *     intact              HasGiven false, INDEX_NONE
		 *     went with a piece   HasGiven true,  INDEX_NONE
		 *     broke in pass N     HasGiven true,  N >= 1
		 *
		 * CheckRemovalCase asserts all three are separable, and asserts the stronger
		 * property that makes the middle row meaningful: a joint that has gone unstamped
		 * is exactly a joint touching a removed piece.
		 */
		int32 BreakPass = INDEX_NONE;

		/**
		 * Utilisation of the load the joint settles at, derived from the stress the
		 * fixture was written in rather than hand-computed from a mass.
		 *
		 * Zero for a joint that has gone. For a survivor this is the MARGIN, so a
		 * fixture that only just held cannot drift into only just failing unnoticed.
		 */
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
	 * Build, remove, solve, and check one case against its table AND against the
	 * properties that must hold after ANY removal.
	 *
	 * ASSERTED FROM THE TABLE: what each joint carries and on which axis, how hard each
	 * survivor is working, which joints have gone and why, and which pieces are still
	 * held up.
	 *
	 * ASSERTED UNIVERSALLY:
	 *
	 *  - REMOVAL DOES NOT MOVE ANY HANDLE. NumPieces stays the valid handle range and
	 *    NumConnections stays the joint count, because both are array indices and three
	 *    arrays are parallel to the connection one — including ConnectionBreakPass,
	 *    which cannot be rebuilt from anything and so would be permanently wrong.
	 *  - NUMLIVEPIECES IS THE OTHER QUESTION, and it does drop by one per removal. The
	 *    two answers diverging is the whole point of the tombstone.
	 *  - EVERY LOAD IS FINITE. A dead piece must not launder a NaN into the load path,
	 *    and FMath::Max discards a NaN rather than propagating it, so a fault here would
	 *    read as a plausible number rather than as an obvious one.
	 *  - A JOINT THAT HAS GONE UNSTAMPED IS EXACTLY A JOINT TOUCHING A REMOVED PIECE.
	 *    Both directions, derived from the case's removal list rather than from any
	 *    magic value, and this is what makes "removal is not failure" a testable claim
	 *    instead of a naming convention: a stamped joint must have failed under load, and
	 *    a joint whose piece went must not be in the cascade's numbering.
	 *  - A PASS NUMBER IS A PASS NUMBER. Anything not INDEX_NONE is at least 1, so no
	 *    sentinel has crept into a domain phase 5 will iterate.
	 *  - GROUND-REACTION CONSERVATION. Everything the solver still claims to hold up has
	 *    to arrive at the earth: short means load was stranded on the way down, long
	 *    means a share was counted twice.
	 *
	 * (ClassifyForce is called directly, which DESIGN.md warns re-opens the
	 * degenerate-normal hole. Safe here: it makes no break decision, and every normal in
	 * these specs is a real plane — AddConnection rejects the rest.)
	 */
	void CheckRemovalCase(FAutomationTestBase& Test, const FRemovalCase& Case)
	{
		constexpr double Tolerance = 1.0e-6;

		/** Absolute floor, for the rows whose expectation is an exact zero. */
		constexpr double UtilisationTolerance = 1.0e-12;

		/**
		 * And a relative allowance for everything else. The fixtures are written as
		 * MassForStress, which divides by 980 and is then multiplied by 980 again inside
		 * the solver, so the loads carry a rounding error proportional to their own size
		 * rather than a fixed one.
		 */
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

		/*
		 * THE WHOLE POINT, and it is checked before anything is solved. NumPieces is the
		 * valid handle range rather than a live count: callers iterate 0..NumPieces, so a
		 * shrinking answer would silently skip real pieces, which is a catastrophic and
		 * entirely quiet change. IsPieceRemoved is the separate question.
		 */
		Test.TestTrue(
			FString::Printf(TEXT("%s: removal must not change the handle range, %d pieces before and %d after"),
				Case.Description, PieceCountBefore, Structure.NumPieces()),
			Structure.NumPieces() == PieceCountBefore);

		Test.TestTrue(
			FString::Printf(TEXT("%s: removal must not change the joint count, %d before and %d after"),
				Case.Description, ConnectionCountBefore, Structure.NumConnections()),
			Structure.NumConnections() == ConnectionCountBefore);

		/*
		 * AND THE COUNT THAT DOES MOVE. NumPieces answers "how far do the handles go" and
		 * NumLivePieces answers "how much is left"; the two diverging by exactly the
		 * number of removals is the tombstone, observed from outside.
		 */
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

			/*
			 * GetConnection hands back a reference to the connection the STRUCTURE owns,
			 * so this is the real latch and not a copy of it.
			 */
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

			/*
			 * A STAMP IS A CASCADE PASS, so anything that is not INDEX_NONE has to be a
			 * pass number. This is what stops a sentinel being smuggled into the domain
			 * phase 5 iterates.
			 */
			Test.TestTrue(
				FString::Printf(TEXT("%s: connection %d carries break pass %d, which is neither INDEX_NONE nor a pass"),
					Case.Description, Index, Structure.GetBreakPass(Index)),
				Structure.GetBreakPass(Index) == INDEX_NONE || Structure.GetBreakPass(Index) >= 1);

			/*
			 * A joint the cascade stamped must be out of the structure. The reverse does
			 * NOT hold, and the next assertion is what says why.
			 */
			if (Structure.GetBreakPass(Index) != INDEX_NONE)
			{
				Test.TestTrue(
					FString::Printf(TEXT("%s: connection %d is stamped pass %d but reports itself intact"),
						Case.Description, Index, Structure.GetBreakPass(Index)),
					Connection.HasGiven());
			}

			/*
			 * REMOVAL IS NOT FAILURE, in both directions and derived from the fixture
			 * rather than from a magic value. A joint that has gone with no pass stamp is
			 * exactly a joint one of whose pieces was removed — so a removal cannot hide
			 * as a break, and a break cannot hide as a removal.
			 */
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

			/*
			 * Gravity does not change direction because a joint happens to be vertical,
			 * and it does not acquire one because a neighbouring piece was taken away.
			 */
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
		 * GROUND-REACTION CONSERVATION, against the solver's OWN final claim about what it
		 * is holding up rather than against the table, so it keeps its force if the
		 * expectations above are ever revisited. A removed piece is grounded-in-the-spec
		 * but out of the structure, so the joints touching it contribute the zero they are
		 * asserted to carry above and the sum stays honest.
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
	 * What a joint is carrying RIGHT NOW, on the axis it is carrying it on.
	 *
	 * Recomputed from outside rather than asked of the connection, because
	 * FConnection::ApplyForce is the only evaluator there is and it LATCHES — asking a
	 * joint how hard it is working would break it. Same shape the cascade tests already
	 * use; see CURRENT_STATE.md on the missing non-mutating evaluator.
	 *
	 * THE AXIS GUARD, which is why this exists rather than a bare utilisation compare.
	 * ComputeUtilisation returns the WORST of three axes, so an expectation derived from
	 * a compressive strength proves nothing unless compression is what produced the
	 * number. Every joint in the fixtures below is an axis-aligned BED joint under a
	 * vertical load, so shear and tension are exactly zero, every shear and tensile
	 * capacity in play is positive, and the compression axis is therefore the only one
	 * that can govern. Asserted per call rather than assumed.
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
 * A REMOVED PIECE LEAVES A HOLE. Its handle stays valid and every other handle keeps
 * meaning what it meant, because handles are array indices and three arrays hang off
 * them.
 *
 * THE DECISION THIS ENCODES: tombstone the slot and never reuse it. Mark the piece
 * dead, leave the gap, indices stable forever. Scenarios are bounded — a wall is tens
 * to hundreds of pieces, rebuilt fresh each run — so the leak is irrelevant.
 *
 * DO NOT "TIDY" A FREE LIST ON TOP OF THIS. Reusing a slot without a generation counter
 * brings the bug back in a worse form: a stale handle then names a DIFFERENT LIVE piece,
 * which nothing can detect, where a dangling index can at least be range-checked. If
 * slots ever have to be reused, that needs generational handles (index plus a counter
 * bumped on reuse), not a free list.
 *
 * WHY THIS IS THE FIRST TEST OF THE SLICE. Compaction is the obvious implementation and
 * it fails silently: every connection above the hole re-points at the wrong piece, with
 * no crash and no assert, just a structure that holds itself up wrong. Phase 2b sharpened
 * it by adding ConnectionBreakPass, which is NEVER rebuilt because a pass stamp cannot be
 * recomputed from anything — so a re-indexing bug self-heals in ConnectionForces on the
 * next solve and is PERMANENT there, and phase 5 then plays back the wrong joints failing
 * in the wrong order while GetBreakPass and HasGiven agree perfectly.
 *
 * BOTH FIXTURES REMOVE FROM THE MIDDLE, which is the only place that can prove anything:
 * removing the last piece is indistinguishable from compacting it away. The masses are
 * deliberately all different, so a shifted array reads back the wrong piece rather than
 * an identical one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureRemovalKeepsIndicesStableTest,
	"DestructionGame.Core.Structure.RemovalKeepsIndicesStable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureRemovalKeepsIndicesStableTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	/*
	 * FIXTURE ONE: a five-piece structure with the hole in the middle of the array.
	 *
	 *                [ block 4 ]
	 *                [ beam 3  ]
	 *   [pad 0]      [pad 1]      [pad 2]
	 *   ======       ======       ======
	 *
	 * Pad 1 goes. Pads 0 and 2 and the two pieces above them keep their handles, and so
	 * does every joint: connection 2 must still name pieces 2 and 3. Compact the piece
	 * array and pieces 2, 3 and 4 slide down to 1, 2 and 3, at which point either the
	 * connections are rewritten (and connection 2 reads {1, 2}) or they are not (and it
	 * names two entirely different bricks). Both are caught here.
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

			/*
			 * A SURVIVOR IS UNTOUCHED, and the distinct masses are what make that
			 * checkable: under compaction handle 2 returns the 44 kg beam.
			 */
			TestTrue(
				FString::Printf(TEXT("piece %d should still weigh %f kg, got %f"),
					Index, Built[Index].MassKg, Structure.GetPiece(Index).MassKg),
				FMath::IsNearlyEqual(Structure.GetPiece(Index).MassKg, Built[Index].MassKg, 1.0e-9));

			TestTrue(
				FString::Printf(TEXT("piece %d should still be %s"),
					Index, Built[Index].bIsGrounded ? TEXT("grounded") : TEXT("ungrounded")),
				Structure.GetPiece(Index).bIsGrounded == Built[Index].bIsGrounded);

			/*
			 * A piece's own record of where it lives has to agree with the handle it
			 * answers to, or a renumbering compaction would go unnoticed here.
			 */
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
	 * FIXTURE TWO: THE BREAK-PASS ARRAY IS THE ONE THAT CANNOT HEAL.
	 *
	 * A cascade stamps a joint, THEN a piece is removed whose joint sits at a LOWER
	 * connection index. An implementation that deletes the joints touching the dead piece
	 * from the connection array shifts every later joint down one, and ConnectionForces
	 * quietly recovers on the next solve while ConnectionBreakPass is wrong for ever: the
	 * lime joint's pass-1 stamp lands on the cement joint, and phase 5 plays back a joint
	 * failing that never did.
	 *
	 *   [ light 2 ]                    [ block 4 ]
	 *   [ pad 1   ]  REMOVED       [pad 0]     [pad 3]
	 *   ==========                 ======      ======
	 *      c0                       c1 lime     c2 cement
	 *
	 * Pieces 0/3/4 are one loaded structure; pieces 1/2 are a separate stack that shares
	 * nothing with it. The lime joint breaks in pass 1 under 3 MPa against its 2 MPa bond;
	 * the cement joint then takes the whole 6e6 uu over 100 cm2 — 6 MPa against 10 — and
	 * holds. Removing pad 1 afterwards must change neither of those answers.
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

		/*
		 * HISTORY IS NOT REWRITTEN. The lime joint gave under load, in pass 1, and no
		 * later removal may restamp it — the stamp is the only record of WHEN, and it is
		 * what phase 5 replays.
		 */
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
		 * THE THREE STATES, ALL PRESENT AT ONCE AND ALL SEPARABLE, on one structure and
		 * with no sentinel value anywhere: connection 0 went with its pad, connection 1
		 * failed under load in pass 1, connection 2 is still holding. Nothing in the pair
		 * (HasGiven, GetBreakPass) is ambiguous between them.
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
 * A REMOVED PIECE IS OUT OF THE GRAPH: it supports nothing, nothing supports it, and
 * every joint that held it goes with it.
 *
 * Assertions are on the MECHANISM — which joints conduct, what they carry, which pieces
 * the solver still claims to be holding up. There is no world here and nothing moves;
 * DESIGN.md §4 is emphatic that displacement is never a valid break assertion anyway,
 * since two pieces can sever and stay resting exactly where they were.
 *
 * THE GROUNDED CASE IS THE SHARP ONE. Removal is naturally expressed as "take every
 * joint touching this piece out of the structure", and that alone leaves a removed
 * GROUNDED piece still seeding the reachability walk — it would report itself held up
 * while being nowhere in the graph. A piece that is not there cannot be resting on the
 * earth.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureRemovedPieceLeavesTheGraphTest,
	"DestructionGame.Core.Structure.RemovedPieceLeavesTheGraph",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureRemovedPieceLeavesTheGraphTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	/*
	 * Cement mortar and stress-sized masses rather than an unbreakable fixture, because
	 * the utilisation column has to be able to fail. An Unbreakable joint at brick loads
	 * settles around 1e-18, which is far under CheckRemovalCase's 1e-12 absolute floor —
	 * so that row would pass against an exact zero, which is precisely the answer a joint
	 * dropped out of the load path gives. Every non-zero expectation here is a real
	 * fraction of a published strength.
	 */
	const TArray<FRemovalCase> Cases = {
		/*
		 * THE CONTROL, and it has to come first: the same three-high stack with nothing
		 * removed. Without it every case below could be satisfied by an implementation
		 * that reports a structure of nothing but falling pieces. Each block is sized to
		 * put 1 MPa on 100 cm2, so the lower joint carries both at 2 MPa (0.20 of cement
		 * mortar's 10) and the upper carries one at 1 MPa (0.10).
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

		/*
		 * THE MIDDLE PIECE GOES. Both of its joints leave with it, and the brick that was
		 * resting on it has nothing left to rest on — which is precisely the MVP
		 * interaction: pull one brick out and what it was carrying comes down.
		 */
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

		/*
		 * A REMOVED GROUNDED PIECE IS NOT GROUND. It is out of the structure, so it is not
		 * resting on the earth and it is not conducting the earth to anything else.
		 */
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

		/*
		 * REMOVAL IS LOCAL. A piece joined to nothing goes, and the loaded half of the
		 * structure does not notice — the counterpart to the control above, and what stops
		 * "a removal strands things" being satisfied by a removal that strands everything.
		 */
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
 * REMOVING A SUPPORT MOVES ITS SHARE ONTO THE SURVIVORS — the MVP interaction, in the
 * form where the arithmetic is checkable by eye.
 *
 * WHY THE FIXTURES LOOK LIKE THIS. Every case hangs one heavy piece off several grounded
 * pads through pure bed joints, because with the area-weighted split every parallel
 * support of one piece carries the SAME STRESS regardless of its area: the share is
 * proportional to area, so the area cancels and the stress is the total load over the
 * total supporting area. Removing one pad therefore shrinks the total area and raises the
 * stress on every survivor by a factor this test can state exactly, which is
 * redistribution in its most checkable form. A vertical load on an axis-aligned bed joint
 * is also PURE COMPRESSION, so no other axis can be quietly governing.
 *
 * THE CONTROL FIRST, THEN THE REMOVAL, on the same fixture: a test that cannot tell "the
 * load moved" from "the fixture always read that way" is not testing redistribution.
 *
 * THE LAST TWO CASES DELIBERATELY BREAK THAT SHAPE. Everything above is a tree, so a
 * removal can only ever shorten a load path. The two-tier fallback makes the opposite
 * possible — losing a bed joint PROMOTES a piece's head joints, so a removal can ADD
 * edges to the support relation — and that is the only place in this file where a
 * survivor's load arrives on a different AXIS than it left on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureRemovalRedistributesLoadTest,
	"DestructionGame.Core.Structure.RemovalRedistributesLoad",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureRemovalRedistributesLoadTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	/*
	 * One block on two 100 cm2 cement pads, loaded so the pair sits at 3 MPa. Cement
	 * mortar crushes at 10 MPa, so as built each joint is at 0.30 and after one pad goes
	 * the survivor is at 0.60 — the load genuinely doubled and the block is still up.
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

	/*
	 * A beam on two pads with a block on the beam. Removing a PAD is redistribution;
	 * removing the BEAM strands the block, because the beam is its only support.
	 */
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
	 * A piece with one bed joint beneath it and one head joint sideways. A bed joint wins
	 * the tier outright, so the bolted plate carries EXACTLY ZERO while the pad is there
	 * and takes the whole piece the moment it goes — as pure shear, because a vertical
	 * load on a vertical face has no component along the normal.
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
		/*
		 * THE CONTROL. 3 MPa on each of two cement joints is 0.30 of capacity — properly
		 * loaded, comfortably standing.
		 */
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

		/*
		 * THE REMOVAL. Half the bearing area goes, so the survivor takes the whole
		 * 6e6 uu over 100 cm2 — 6 MPa, 0.60 of capacity, and the block stays up. An
		 * implementation that removed the pad and left its share stranded would report
		 * 3 MPa here and a falling block.
		 */
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

		/*
		 * BOTH SUPPORTS GO. Nothing is holding the block up, so there is no static load
		 * path and both joints report zero rather than a plausible-looking weight.
		 */
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

		/*
		 * THE CONTROL FOR THE BEAM FIXTURE. The block puts 2e6 uu through the beam joint
		 * (2 MPa, 0.20); the beam adds its own 2e6 and pushes 4e6 into 200 cm2 of pad, so
		 * each pad joint carries 2e6 at 2 MPa.
		 */
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

		/*
		 * REMOVE A PAD AND THE LOAD REROUTES BELOW WITHOUT CHANGING ANYTHING ABOVE. The
		 * survivor doubles to 4 MPa; the beam-to-block joint is untouched at 2 MPa, which
		 * is what says a removal is local rather than a global re-shuffle.
		 */
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

		/*
		 * REMOVE THE BEAM AND THE BLOCK IS STRANDED. Its only support has gone, and there
		 * is nowhere for its weight to route: everything reports zero and both pieces come
		 * out unsupported. This is the shape of knocking a course out of a wall.
		 */
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

		/*
		 * THE CONTROL FOR THE FALLBACK: while the pad is there the bolted plate is not in
		 * the support relation at all, so it carries exactly nothing.
		 */
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
		 * REMOVAL PROMOTES THE HEAD JOINT. With no bed joint left the piece falls back on
		 * the plate, which takes the whole 5e5 uu as PURE SHEAR — 0.5 MPa against the
		 * bolt's flat 1.0 MPa cohesion, so 0.50 and it holds. The load did not merely
		 * change size, it changed AXIS, and the axis guard above is what says so: the
		 * bolt's friction coefficient is exactly zero, so its shear capacity does not move
		 * with compression and 0.50 can only have come from the shear axis.
		 */
		{
			TEXT("removing the pad promotes the head joint, which takes the piece in shear"),
			PadAndPlate,
			{ 0 },
			{
				{ 0.0, EJointKind::Bed, true, INDEX_NONE, 0.0 },
				{ -ForceForMPa(0.5, 100.0), EJointKind::Head, false, INDEX_NONE,0.5 }
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
 * REMOVAL COMPOSES WITH THE CASCADE: taking a piece out can push a neighbour over
 * capacity, and what follows is an ordinary cascade over the joints that are left.
 *
 * This is the demo. A wall stands, a player pulls a brick, and the load that brick was
 * carrying arrives somewhere that cannot take it.
 *
 * THE PART THAT IS EASY TO GET WRONG: a joint that went with a removed piece must NOT
 * appear in the cascade's pass numbering. It did not fail under load — it was deleted —
 * and phase 5 replays these stamps as the sequence of a collapse. So it is left
 * UNSTAMPED, and INDEX_NONE does NOT mean "still intact": it means "no pass broke this",
 * which is equally true of an intact joint and of a deleted one. Liveness is the other
 * question and HasGiven is its accessor; the pair encodes all three states with no
 * sentinel, which is the decision this file exists to hold in place.
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
	 * THREE PADS, and a removal that breaks nothing. 6 MPa each as built; take one pad
	 * away and the remaining two carry 9 MPa apiece against cement mortar's 10 — a
	 * genuine, large redistribution that the structure absorbs. Without this control
	 * every case below could be satisfied by an implementation that breaks whatever it is
	 * shown.
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

		/*
		 * Captured into a local before it is reported. Argument evaluation order is
		 * unspecified in C++, so calling SolveAndBreak inside the message of a TestTrue
		 * whose condition also reads the structure would be a coin toss over whether the
		 * cascade ran before the assertion looked.
		 */
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
	 * THE DEMO. Two pads at 6 MPa each — 0.60 of capacity, standing. Take one away and the
	 * survivor inherits the whole 12e6 uu over 100 cm2: 12 MPa against 10, so 1.20, and it
	 * gives on the first pass. The block then has nothing left and comes down.
	 *
	 * The removal itself broke nothing: the cascade did, in pass 1, and only the pad joint
	 * that was actually overloaded carries that number.
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

		/*
		 * AND THE REMOVED JOINT IS NOT PART OF THAT PASS. It never failed under load, so
		 * playing this collapse back must not show it giving alongside the joint that did.
		 */
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

		/*
		 * IT HAS SETTLED. Nothing more gives, nothing un-gives, and no stamp is rewritten
		 * — and in particular the removed joint is not retroactively drawn into a pass. A
		 * cascade that re-evaluated it would have to find it "over capacity" at zero load,
		 * which is the shape of a loop that never terminates.
		 */
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
	 * A REMOVAL AFTER A CASCADE, rather than before one. The cement joint survived the
	 * lime joint giving and settled at 0.60; removing the pad under it takes the block's
	 * last support and the whole structure reads as falling — with the two stamps still
	 * telling different stories about how each joint went.
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

		/*
		 * THE TWO JOINTS WENT FOR DIFFERENT REASONS AND STILL SAY SO. Both are out of the
		 * structure; only the one that failed under load is in the collapse sequence.
		 */
		TestTrue(
			FString::Printf(TEXT("the cement joint went with its pad, so HasGiven %d and no pass, got %d"),
				Structure.GetConnection(1).HasGiven() ? 1 : 0, Structure.GetBreakPass(1)),
			Structure.GetConnection(1).HasGiven() && Structure.GetBreakPass(1) == INDEX_NONE);
	}

	return true;
}

/**
 * PASS NUMBERS ARE GLOBAL TO A STRUCTURE, NOT TO A CALL. A second SolveAndBreak numbers
 * its passes from the HIGH-WATER MARK of the stamps already written, so a joint that gave
 * after the player pulled a brick carries a strictly larger number than everything that
 * gave before it.
 *
 * WHY THIS IS REACHABLE ONLY NOW. A second cascade on a settled structure provably breaks
 * nothing — RemovalCascades asserts exactly that — so before removal existed no two calls
 * could both stamp, and per-call numbering was indistinguishable from global numbering.
 * RemovePiece is the one operation that changes the graph between calls, so the removal
 * slice is what makes the distinction observable, and removal is the MVP's own interaction
 * rather than a corner case.
 *
 * WHAT GOES WRONG IF THEY RESTART. Every consumer of the sequence reads a shared number as
 * "these gave simultaneously" (CURRENT_STATE.md states that contract for phase 5's
 * visualisation). Restarting at 1 puts the joint that failed FIRST and the joint that
 * failed LATER — after a player pulled a brick out from under it — on the same number, so
 * the collapse replays in the wrong order with nothing anywhere reading as inconsistent:
 * no NaN, no crash, every liveness and conservation check still green.
 *
 * THE RETURN VALUE IS THE OTHER QUESTION AND STAYS PER-CALL. SolveAndBreak answers "how
 * many passes did THIS call break in", which is what a caller polls to know whether its
 * removal did anything; only the stamps are global. Conflating the two is the obvious
 * wrong fix, so every case below asserts both and they deliberately disagree.
 *
 * AXIS GOVERNANCE. Every joint here is an axis-aligned bed joint under a vertical load, so
 * the load is pure compression and each expectation below is a compressive stress against a
 * compressive strength — checked per joint by CheckSettledCompression rather than assumed,
 * because ComputeUtilisation returns the worst of three axes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBreakPassesContinueAcrossCallsTest,
	"DestructionGame.Core.Structure.BreakPassesContinueAcrossCalls",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBreakPassesContinueAcrossCallsTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	/*
	 * ONE BREAK BEFORE THE REMOVAL, ONE AFTER, and the two must not share a number.
	 *
	 *          [ block 3 ]                12 MPa.cm2 total, over three 100 cm2 pads
	 *         /     |     \
	 *     lime    cement  cement
	 *   [pad 0]  [pad 1]  [pad 2]
	 *   ======   ======   ======
	 *
	 * As built each joint takes 4.0 MPa. Lime crushes at 2.0, so it gives at 2.00 in pass
	 * 1 and is stamped 1; the two cement joints are at 0.40 of their 10.0 and hold. The
	 * re-solve moves the whole load onto 200 cm2 — 6.0 MPa, 0.60 — and the structure
	 * settles, so that call reports ONE breaking pass.
	 *
	 * Then a player pulls pad 1. Its joint is severed and unstamped, and pad 2 inherits
	 * everything: 12.0 MPa against 10.0, so 1.20 and it gives. That break happened strictly
	 * LATER than the lime one and must say so — pass 2, not pass 1 — while the call itself
	 * still reports the one breaking pass it ran.
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

		/*
		 * Captured into a local before it is reported. Argument evaluation order is
		 * unspecified in C++, so calling SolveAndBreak inside the message of a TestTrue
		 * whose condition also reads the structure would be a coin toss over whether the
		 * cascade ran before the assertion looked.
		 */
		const int32 PassesAsBuilt = Structure.SolveAndBreak();

		TestTrue(
			FString::Printf(TEXT("the lime joint at 2.00 should give in exactly one pass, got %d"),
				PassesAsBuilt),
			PassesAsBuilt == 1);

		TestTrue(
			FString::Printf(TEXT("the lime joint should be stamped pass 1, got HasGiven %d and pass %d"),
				Structure.GetConnection(0).HasGiven() ? 1 : 0, Structure.GetBreakPass(0)),
			Structure.GetConnection(0).HasGiven() && Structure.GetBreakPass(0) == 1);

		/*
		 * THE CONTROL, and without it the removal below proves nothing: the structure has
		 * genuinely settled on the two cement joints at 0.60 apiece, so the break that
		 * follows is caused by the removal rather than by a cascade that never stopped.
		 */
		CheckSettledCompression(*this, Structure, 1, 0.6, TEXT("settled after the lime joint gave"));
		CheckSettledCompression(*this, Structure, 2, 0.6, TEXT("settled after the lime joint gave"));

		TestTrue(TEXT("removing one of the two surviving pads should report that it removed a live piece"),
			Structure.RemovePiece(1));

		const int32 PassesAfterRemoval = Structure.SolveAndBreak();

		/*
		 * THE COUNT IS PER-CALL. This call ran one breaking pass, whatever number that pass
		 * stamped — it is what a caller polls to find out whether its removal did anything,
		 * and making it cumulative would be the obvious wrong way to fix the stamps.
		 */
		TestTrue(
			FString::Printf(TEXT("the survivor at 12 MPa should give in exactly one pass of THIS call, got %d"),
				PassesAfterRemoval),
			PassesAfterRemoval == 1);

		/*
		 * AND THE STAMP IS GLOBAL. The cement joint failed strictly after the lime one, so
		 * it cannot carry the lime joint's number: phase 5 reads a shared number as
		 * "these gave simultaneously", and these did not.
		 */
		TestTrue(
			FString::Printf(TEXT("the cement joint gave after the lime one and must be stamped pass 2, got %d"),
				Structure.GetBreakPass(2)),
			Structure.GetBreakPass(2) == 2);

		TestTrue(
			FString::Printf(TEXT("the cement joint gave LATER than the lime one, got stamps %d then %d"),
				Structure.GetBreakPass(0), Structure.GetBreakPass(2)),
			Structure.GetBreakPass(2) > Structure.GetBreakPass(0));

		/*
		 * NOTHING EARLIER IS REWRITTEN. A stamp is history; a second cascade adds to the
		 * sequence and never edits it.
		 */
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
	 * A MULTI-PASS SECOND CALL, which is what separates "continue from the high-water mark"
	 * from "start at one" AND from "start at the last count". The second call runs two
	 * passes and they must be stamped 2 and 3.
	 *
	 * TWO INDEPENDENT SUB-STRUCTURES in one graph, deliberately, so the arithmetic of each
	 * stands alone — passes are a property of the whole structure, and nothing about the
	 * numbering should depend on the two halves being connected.
	 *
	 *   [ block 1 ]                        [ block 5 ]           4.8 MPa.cm2 over 3 pads
	 *       |                             /     |     \
	 *     lime                        lime    bolt   cement
	 *   [pad 0]                     [pad 2]  [pad 3]  [pad 4]
	 *   ======                      ======   ======   ======
	 *
	 * CALL 1 breaks the left-hand lime joint only: 3.0 MPa against 2.0 is 1.50. The
	 * right-hand three sit at 1.6 MPa apiece — lime 0.80, bolt 0.64, cement 0.16 — and all
	 * hold, so the call is one pass and the high-water mark is 1.
	 *
	 * THEN PAD 4 IS PULLED. The cement joint is severed unstamped and the remaining two
	 * share 200 cm2: 2.4 MPa. Lime is at 1.20 and gives (pass 2); the bolt is at 0.96 and
	 * survives that pass by 4%, which is what makes the two passes genuinely sequential
	 * rather than one pass breaking both. The re-solve then hands the bolt the whole load
	 * over 100 cm2 — 4.8 MPa against 2.5, so 1.92 — and it gives in the next pass (pass 3).
	 *
	 * The call ran TWO breaking passes and stamped 2 and 3. Per-call numbering would stamp
	 * 1 and 2; a cumulative count would still report two.
	 *
	 * THE CAPACITY LADDER IS THE CONSTRAINT HERE, so do not retune these masses casually.
	 * Load splits weighted by area, so every joint under one piece carries the SAME STRESS
	 * whatever its area — capacity is the only thing that can order the breaks. Dropping
	 * one of N equal joints raises the stress by N/(N-1), at most 2x, so a two-pass cascade
	 * needs two capacities less than a factor of two apart: lime's 2.0 and the fastener's
	 * 2.5 are the only such pair in the profile library.
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

		/*
		 * THE RIGHT-HAND HALF IS INTACT AS BUILT, checked before anything breaks, because
		 * every expectation after this point depends on all three of its joints surviving
		 * call 1. Measured on the axis that governs, so a fixture that quietly started
		 * failing in shear would say so rather than agreeing by coincidence.
		 */
		Structure.SolveLoads();
		CheckSettledCompression(*this, Structure, 1, 0.8, TEXT("as built"));
		CheckSettledCompression(*this, Structure, 2, 0.64, TEXT("as built"));
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

		/*
		 * THE TWO NEW BREAKS CONTINUE THE SEQUENCE. 2 then 3, because pass 1 is already
		 * spent — and they are still one apart, so continuing from the high-water mark has
		 * not been confused with adding the previous call's count onto every stamp.
		 */
		TestTrue(
			FString::Printf(TEXT("the right-hand lime joint gave in the first pass of the second call and must be stamped 2, got %d"),
				Structure.GetBreakPass(1)),
			Structure.GetBreakPass(1) == 2);

		TestTrue(
			FString::Printf(TEXT("the bolted joint gave one pass later and must be stamped 3, got %d"),
				Structure.GetBreakPass(2)),
			Structure.GetBreakPass(2) == 3);

		/*
		 * THE WHOLE SEQUENCE, read the way phase 5 will read it: three joints that gave in
		 * three separate passes, strictly ordered, and a fourth that went with its pad and
		 * is not in the sequence at all.
		 */
		TestTrue(
			FString::Printf(TEXT("the collapse sequence should read 1 < 2 < 3, got %d, %d, %d"),
				Structure.GetBreakPass(0), Structure.GetBreakPass(1), Structure.GetBreakPass(2)),
			Structure.GetBreakPass(0) < Structure.GetBreakPass(1)
				&& Structure.GetBreakPass(1) < Structure.GetBreakPass(2));

		TestTrue(
			FString::Printf(TEXT("the removed pad's joint is out of the structure and unstamped, got HasGiven %d and pass %d"),
				Structure.GetConnection(3).HasGiven() ? 1 : 0, Structure.GetBreakPass(3)),
			Structure.GetConnection(3).HasGiven() && Structure.GetBreakPass(3) == INDEX_NONE);

		/*
		 * A STAMP IS STILL A PASS NUMBER. Nothing above may have smuggled a sentinel or a
		 * zero into the domain phase 5 iterates.
		 */
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
 * IS PIECE SUPPORTED IS THE LAST SOLVE'S ANSWER, AND REMOVAL DOES NOT REWRITE IT.
 *
 * A CHARACTERISATION TEST. It pins behaviour that is already correct, so it is green from
 * the moment it compiles; what it is for is that the HEADER SAYS SOMETHING ELSE, and
 * nothing until now queried a piece between a removal and the next solve, so the two were
 * free to drift. GetPiece, IsPieceRemoved and GetBreakPass all answer a removed piece
 * immediately; IsPieceSupported reads PieceSupported[], which only SolveLoads writes, so it
 * answers a removed piece only once something has re-solved.
 *
 * THE SCOPED CONTRACT, which is what Structure.h should say:
 *
 *     never solved     false, for every handle — there is no answer yet
 *     removed          the LAST SOLVE'S answer, unchanged, until the next solve
 *     after that solve false, and a removed GROUNDED piece is no longer earth
 *
 * The middle row is the one that bites, and it is stale rather than wrong: the piece is
 * gone and the array still says it was held up. The gameplay shape that reaches it is the
 * MVP's own — remove a piece on player interaction, then ask about its neighbours to decide
 * what to release to dynamics — and asking before re-solving reads the pre-removal world.
 *
 * WHY THE COMMENT AND NOT THE CODE. Clearing one entry of PieceSupported on removal would
 * leave a HALF-STALE array: that one piece current, every neighbour still describing a
 * structure that no longer exists, which is a worse thing to hand a caller than a uniformly
 * stale array with a documented scope. If that is ever reconsidered, the middle row below
 * goes red — that is the point of asserting it — and the header must change in the same
 * commit. Do not quietly relax the assertion to accommodate a code change.
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

	/*
	 * BEFORE ANY SOLVE THERE IS NO ANSWER, and false is the fail-closed one. PieceSupported
	 * is sized by a solve rather than by AddPiece, so this is the same range check an
	 * unknown handle takes — a caller that asks too early is told nothing is held up rather
	 * than being handed a default that reads as held up.
	 */
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

	/*
	 * REMOVING THE BRICK. The answer for the piece that has gone does not move until
	 * something re-solves — and then it is false, for the same reason an unknown handle is
	 * false: it is not a piece any more, so nothing is holding it up.
	 */
	{
		FStructure Structure;
		BuildStructure(Structure, PadAndBrick);
		Structure.SolveLoads();

		TestTrue(TEXT("the brick starts out held up by the pad"), Structure.IsPieceSupported(1));

		TestTrue(TEXT("removing the brick should report that it removed a live piece"),
			Structure.RemovePiece(1));

		TestTrue(TEXT("the brick reads as removed IMMEDIATELY — that accessor needs no solve"),
			Structure.IsPieceRemoved(1));

		/*
		 * THE STALE ROW. Support is the last solve's answer and removal did not touch it,
		 * so a piece that is provably gone still reads as held up. This is the assertion
		 * the header contradicts today.
		 */
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

	/*
	 * REMOVING THE GROUND. Same scoping, on the piece whose removal has the furthest reach:
	 * a grounded piece seeds the reachability walk on its own account rather than through
	 * any joint, so until the re-solve BOTH pieces still read as held up by an earth one of
	 * them is no longer touching.
	 */
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

	/*
	 * SOLVEANDBREAK COUNTS AS THE SOLVE. It runs SolveLoads on every pass, so a caller that
	 * cascades after a removal does not need a bare solve as well — which is why the
	 * degenerate matrix, which cascades between its removals, never sees the stale row.
	 */
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
 * DEGENERATE REMOVALS FAIL CLOSED, and a structure that has been picked apart still
 * answers finitely.
 *
 * A matrix rather than a case per shape, because the interesting property is that NONE of
 * them produce a plausible-looking wrong answer. FMath::Max discards a NaN and FMath::Min
 * replaces it, so a fault in this layer would surface as a confident number rather than as
 * an obvious fault — which is why "never NaN, always finite, never reads intact when
 * broken" is asserted over the whole matrix instead of being spot-checked.
 *
 * AN UNKNOWN HANDLE READS AS REMOVED. That is the fail-closed direction and it matches
 * the accessors either side of it: GetPiece answers a placeholder, IsPieceSupported
 * answers false, GetBreakPass answers INDEX_NONE. A caller filtering with
 * `if (IsPieceRemoved(H)) continue;` then skips a handle that names nothing, rather than
 * walking on into it as though it were a live piece.
 *
 * AND A REMOVED PIECE IS NOT HELD UP, GROUNDED OR NOT. Same answer as an unknown handle
 * and for the same reason rather than as a special case: it is not a piece, so nothing is
 * holding it up, and a removed grounded piece has stopped resting on the earth.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureRemovalDegenerateInputTest,
	"DestructionGame.Core.Structure.RemovalDegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureRemovalDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace StructureRemovalSupport;

	constexpr double Tolerance = 1.0e-6;

	/*
	 * THE DERIVATION RAN. BrickMassKg is computed at namespace scope from ClayBrick, which
	 * is safe only while ClayBrick is constant-initialised — see its comment. This block is
	 * where the value reaches UNGROUNDED pieces, so it is where the guard belongs. Not a
	 * second anchor on what a brick weighs; Profiles.MaterialInvariants owns that.
	 */
	TestTrue(
		FString::Printf(TEXT("the brick mass must derive to something positive, got %f kg"), BrickMassKg),
		BrickMassKg > 0.0);

	/*
	 * OUT-OF-RANGE HANDLES. Each is tried against a freshly built structure so one row
	 * cannot mask the next, and the whole structure is asserted unchanged afterwards.
	 */
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

	/*
	 * REMOVING THE SAME PIECE TWICE. The second call removes nothing and must not restamp
	 * anything: a joint already accounted for does not go a second time.
	 */
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

		/*
		 * ONE REMOVAL, NOT TWO. The live count is what would betray a redundant call that
		 * decremented anyway, which is the obvious way to implement this wrong.
		 */
		TestTrue(
			FString::Printf(TEXT("two pieces less one removed twice should still leave 1 live, got %d"),
				Structure.NumLivePieces()),
			Structure.NumLivePieces() == 1);

		TestTrue(
			FString::Printf(TEXT("the joint went with its piece: HasGiven %d and no pass stamp, got %d"),
				Structure.GetConnection(0).HasGiven() ? 1 : 0, Structure.GetBreakPass(0)),
			Structure.GetConnection(0).HasGiven() && Structure.GetBreakPass(0) == INDEX_NONE);
	}

	/*
	 * THE LAST PIECE. A structure emptied of everything still answers: the handle survives,
	 * nothing is held up, and solving an empty graph is not an error.
	 */
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
	 * THE WHOLE MATRIX AT ONCE: a four-piece structure taken apart one piece at a time, in
	 * an order that includes a grounded piece, a middle piece and a piece already stranded
	 * by an earlier removal. After every step the same properties must hold — finite
	 * loads, a live count that tracks the removals, no joint reading intact while it has
	 * plainly gone, and no removal ever appearing in the cascade's pass numbering.
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

				/*
				 * NOTHING HERE FAILED UNDER LOAD, so nothing may be in a pass. Every
				 * joint that goes in this block goes with a piece, and the collapse
				 * sequence phase 5 replays must stay empty for all of it.
				 */
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
