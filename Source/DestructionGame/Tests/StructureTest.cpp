// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous. Two anonymous namespaces are the SAME namespace
 * once a unity build puts two test files in one blob, and this file used to declare
 * a Mortar, an Unbreakable and a MakeNaN that three other files also declared. UBT's
 * adaptive unity had kept them apart by luck rather than by design. See
 * CURRENT_STATE.md, and StructureFuzzTest.cpp for the same treatment.
 */
namespace StructureTestSupport
{
	using namespace DestructionProfiles;

	/**
	 * Unreal's default gravity, 980 cm/s2, spelled out here rather than imported
	 * so the test fails if production gets it wrong instead of agreeing with it.
	 *
	 * THE UNIT TRAP, stated once. Mass is already in kilograms and length is
	 * already in centimetres, so MassKg * 980 IS a force in Unreal units — the
	 * 1 N = 100 uu conversion is baked into the 980 rather than applied on top of
	 * it. Cross-check: 2.7216 kg x 9.81 = 26.7 N, x 100 uu/N = 2670, and
	 * 2.7216 x 980 = 2667.2. Those agree. Multiplying by 100 a second time here
	 * would be wrong by exactly 100x, which tuned thresholds hide beautifully.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double WeightOf(double MassKg)
	{
		return MassKg * GravityCmPerSecondSquared;
	}

	/**
	 * A standard UK metric brick, 215 x 102.5 x 65 mm, so 1432.44 cm3.
	 *
	 * Dimensions live in the test rather than in the material library because they
	 * are the brick ACTOR's business (phase 3); a material only owns density.
	 */
	constexpr double BrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/**
	 * A standard clay brick, DERIVED FROM THE PROFILE rather than hand-set:
	 * 1432.44 cm3 at 1.9 g/cm3 is 2.7216 kg. Real dimensions at real density, per
	 * DESIGN.md §3.
	 *
	 * THIS DELIBERATELY BREAKS THE FILE'S OWN RULE that production values are
	 * respelled rather than imported, and the exception is narrow enough to state
	 * exactly. Importing a value normally makes a test agree with a wrong value
	 * instead of failing on it — which is why GravityCmPerSecondSquared and
	 * ForceForMPa below are still spelled out independently. It is safe HERE because
	 * Profiles.MaterialInvariants already anchors density x volume against what a
	 * real brick weighs in the hand, so there is exactly ONE external anchor and this
	 * is downstream of it. A hand-set literal is a SECOND place to be wrong, and it
	 * was: the 2.72 that used to sit here had drifted 1.6 g from the density the
	 * library states, so every load expectation in this file was 1.6 uu light.
	 *
	 * Every expectation here is written as a multiple of BrickWeightUU, so they all
	 * follow the derivation and none of them needed retuning.
	 *
	 * SAFE AT NAMESPACE SCOPE, and the reason is worth keeping: ClayBrick is an
	 * aggregate of literals, so it is CONSTANT-initialised and ready before any
	 * dynamic initialisation in any translation unit. Make it a computed ratio of
	 * StructuralConcrete and it becomes dynamically initialised, static
	 * initialisation order across translation units is unspecified, and this could
	 * silently read ZERO — at which point every force expectation in the file becomes
	 * zero as well and the whole thing passes while asserting nothing. LoadPath opens
	 * with a guard against exactly that; it is not a second claim about what a brick
	 * weighs, only that the derivation ran at all.
	 */
	const double BrickMassKg = ClayBrick.DensityGramsPerCubicCm * BrickVolumeCubicCm / 1000.0;

	/** 2667.2 uu. The number the whole load model is checked against. */
	const double BrickWeightUU = WeightOf(BrickMassKg);

	/** A 10 cm x 10 cm interface, kept round so the area split stays checkable by eye. */
	constexpr double JointAreaSqCm = 100.0;

	/**
	 * Horizontal interface, normal pointing up at the piece above: a bed joint.
	 * Per the convention in ConnectionLoad.h the normal points toward PieceB, so
	 * PieceB is the piece sitting on PieceA.
	 */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Vertical interface, normal pointing sideways at the neighbour: a head joint. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);
	const FVector OpposingHeadJointNormal(-1.0, 0.0, 0.0);

	/** A bed joint pointing DOWN: the same horizontal interface declared upper piece first. */
	const FVector InvertedBedJointNormal(0.0, 0.0, -1.0);

	/**
	 * THE BED / HEAD THRESHOLD, chosen here rather than inherited from production.
	 *
	 * DESIGN.md §3 makes support two-tiered: a piece rests on the bed joints
	 * BENEATH it and hangs from its head joints only if it has none. "Bed joint"
	 * means a substantially VERTICAL interface normal, and something has to say how
	 * vertical that is.
	 *
	 * 45 degrees from vertical — a joint is a bed joint exactly when its normal is
	 * nearer vertical than horizontal. It is the only threshold that does not prefer
	 * one tier over the other, and it needs no material data, which matters because
	 * the tier decision is made before any strength profile is consulted. A
	 * friction-based choice (arctan mu, about 31 degrees for mortar) would be more
	 * physical, but it would make the load path depend on the connection profile,
	 * and it belongs with the generalisation to non-gravity load directions that
	 * DESIGN.md §3 already flags as outstanding.
	 *
	 * SupportTierThreshold pins this from both sides, at 40 and 50 degrees, so the
	 * choice is asserted to within +/- 5 degrees rather than being incidental to
	 * whatever the implementation happens to do. Exactly 45 is deliberately NOT
	 * asserted: a knife-edge tie is not a behaviour worth locking down in floating
	 * point.
	 *
	 * SPELLED AS THE SAME LITERAL PRODUCTION USES, bit for bit, and that is not
	 * laziness. SpecSupportsOf below is the ORACLE for the DegenerateInputs matrix,
	 * and its comment claims to be a line-for-line transcription of RoleOf, threshold
	 * constant included. Written 1.0 / FMath::Sqrt(2.0) it lands an ulp away from the
	 * constexpr constant in Structure.cpp — a difference no axis-aligned normal can
	 * detect and the first tilted one added to that matrix eventually will, and a
	 * divergence in the oracle reads as a production bug. If the production constant
	 * ever changes, change this one to match; do not re-derive it.
	 */
	constexpr double BedJointCosine = 0.70710678118654752440;

	/** A normal tilted this many degrees away from straight up, in the XZ plane. */
	FVector NormalTiltedFromVertical(double Degrees)
	{
		const double Radians = FMath::DegreesToRadians(Degrees);
		return FVector(FMath::Sin(Radians), 0.0, FMath::Cos(Radians));
	}

	/**
	 * DELIBERATELY UNBREAKABLE strengths for the load-path tests, now the shared
	 * DestructionProfiles::Unbreakable rather than a private copy.
	 *
	 * Phase 2a computes what each joint carries; deciding whether that is too much
	 * is a separate step. Making every joint absurdly strong means no plausible
	 * implementation could break one mid-solve, so these cases measure routing and
	 * accumulation and nothing else.
	 *
	 * It is classified TestFixture in the library, which is what stops it reaching a
	 * scenario: the well-formedness sweep in Profiles.ConnectionInvariants demands
	 * every OTHER profile give under absurd load, so an unbreakable joint that
	 * escaped into shippable data would fail a test rather than quietly produce a
	 * wall the player cannot demolish.
	 *
	 * Real mortar, where a joint is meant to be genuinely overloaded, is
	 * DestructionProfiles::GeneralPurposeMortar and is used directly.
	 */

	/**
	 * Force, in Unreal units, that loads the given area to the given stress.
	 * Spelled out rather than reusing ForceUnitsPerMPaSqCm, same reason as above.
	 * 1 N = 100 uu, 1 cm2 = 100 mm2, 1 MPa = 1 N/mm2 -> 10000 uu per MPa per cm2.
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

	double MakeNaN()
	{
		volatile double Zero = 0.0;
		return Zero / Zero;
	}

	double MakeInfinity()
	{
		volatile double Zero = 0.0;
		return 1.0 / Zero;
	}

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
		double AreaSqCm = JointAreaSqCm;
	};

	/** A whole structure as data, so topologies are a table rather than code. */
	struct FStructureSpec
	{
		TArray<FPieceSpec> Pieces;
		TArray<FConnectionSpec> Connections;
	};

	/**
	 * What a joint does with the load it is handed — the whole point of the
	 * directional model, and the thing a magnitude alone cannot show.
	 */
	enum class EJointKind : uint8
	{
		/** Substantially vertical normal: bears the load in pure compression. */
		Bed,

		/** Substantially horizontal normal: can only carry it in pure shear. */
		Head,
	};

	struct FExpectedJoint
	{
		/**
		 * SIGNED Z of the force the solver must store, in Unreal force units.
		 *
		 * Per ConnectionLoad.h the force belonging to a connection is the force
		 * acting on PIECE B — the piece the interface normal points toward. When the
		 * supported piece is PieceB that is its share of the weight, straight down.
		 * When the joint was declared the other way round, with the supported piece
		 * as PieceA, describing the same joint from PieceB means flipping the normal
		 * AND negating the force, so the stored Z is POSITIVE. Getting that backwards
		 * turns compression into tension, and mortar's tensile limit is a hundredth
		 * of its compressive one.
		 */
		double ForceZUU = 0.0;

		EJointKind Kind = EJointKind::Bed;
	};

	FConnection MakeConnection(const FConnectionSpec& Spec, const FConnectionStrength& Strength)
	{
		FConnection Connection;
		Connection.PieceA = Spec.PieceA;
		Connection.PieceB = Spec.PieceB;
		Connection.InterfaceNormal = Spec.Normal;
		Connection.InterfaceAreaSqCm = Spec.AreaSqCm;
		Connection.Strength = Strength;
		return Connection;
	}

	/**
	 * Build a structure in place.
	 *
	 * By reference on purpose. FConnection is copyable and its "has given" latch
	 * is per-copy, so anything that moves connections around by value risks
	 * latching a temporary and leaving the real joint untouched — a wall that
	 * never falls. See CURRENT_STATE.md.
	 */
	void BuildStructure(FStructure& Out, const FStructureSpec& Spec, const FConnectionStrength& Strength)
	{
		for (const FPieceSpec& Piece : Spec.Pieces)
		{
			Out.AddPiece(Piece.MassKg, Piece.bIsGrounded);
		}

		for (const FConnectionSpec& Connection : Spec.Connections)
		{
			Out.AddConnection(MakeConnection(Connection, Strength));
		}
	}

	/**
	 * Assert what one connection carries AND how the joint resolves it.
	 *
	 * Two separate claims, and both earn their place.
	 *
	 * The VECTOR is the force acting on PieceB, so its sign depends on which end of
	 * the joint was declared first. A solver that unconditionally writes
	 * (0, 0, -Share) is right only for joints that happen to name the supported
	 * piece second, and every case written before this one happened to do that.
	 *
	 * The CLASSIFICATION is the payoff the whole directional model exists for: the
	 * same downward load is pure compression on a bed joint and pure shear on a head
	 * joint. Magnitudes alone cannot tell those apart — the keystone case expects
	 * three identical numbers and would read the same if every joint were crushing.
	 * ClassifyForce is what decides which strength the load is eventually compared
	 * against, so a solver output that has never been classified has never been
	 * checked for the thing that matters.
	 *
	 * TENSION IS ZERO FOR EVERY CASE THAT USES THIS HELPER, and that is a property of
	 * the FIXTURES rather than of the model. Every normal in every spec routed through
	 * here is axis-aligned, and for those a static gravity path really does only press
	 * a bed joint together and slide a head joint — so a non-zero tension means the
	 * force was stored against the wrong end of the joint.
	 *
	 * IT DOES NOT GENERALISE TO A TILTED NORMAL, and nobody should read it as a claim
	 * that it does. The head tier is deliberately sign-blind, so a joint more than 45
	 * degrees off vertical sitting ABOVE a piece is still that piece's fallback
	 * support, and it holds the piece by pulling it open. DESIGN.md §3 states that
	 * outcome and accepts it. Structure.TiltedJointClassification characterises it;
	 * this helper cannot express it, which is why that test has its own runner rather
	 * than a third EJointKind.
	 *
	 * (This calls ClassifyForce directly, which DESIGN.md warns re-opens the
	 * degenerate-normal hole. Safe here because it makes no break decision and every
	 * normal in these specs is a real plane — GraphValidation rejects the rest.)
	 */
	void CheckJointLoad(
		FAutomationTestBase& Test,
		const TCHAR* Description,
		int32 Index,
		const FConnection& Connection,
		const FVector& Force,
		const FExpectedJoint& Expected)
	{
		constexpr double Tolerance = 1.0e-6;

		Test.TestTrue(
			FString::Printf(TEXT("%s: connection %d should carry Z = %f, got %f"),
				Description, Index, Expected.ForceZUU, Force.Z),
			FMath::IsNearlyEqual(Force.Z, Expected.ForceZUU, Tolerance));

		/*
		 * Gravity does not change direction because a joint happens to be vertical.
		 * A solver that pushed the load along each joint's own normal would produce
		 * the right magnitudes on the keystone case and entirely the wrong direction.
		 */
		Test.TestTrue(
			FString::Printf(TEXT("%s: connection %d load must be vertical, got (%f, %f, %f)"),
				Description, Index, Force.X, Force.Y, Force.Z),
			FMath::IsNearlyZero(Force.X, Tolerance) && FMath::IsNearlyZero(Force.Y, Tolerance));

		const FConnectionLoad Load =
			DestructionForce::ClassifyForce(Force, Connection.InterfaceNormal);

		const double Magnitude = FMath::Abs(Expected.ForceZUU);
		const double ExpectedCompression = Expected.Kind == EJointKind::Bed ? Magnitude : 0.0;
		const double ExpectedShear = Expected.Kind == EJointKind::Bed ? 0.0 : Magnitude;

		Test.TestTrue(
			FString::Printf(
				TEXT("%s: connection %d should resolve to compression %f / shear %f / tension 0, got %f / %f / %f"),
				Description, Index, ExpectedCompression, ExpectedShear,
				Load.Compression, Load.Shear, Load.Tension),
			FMath::IsNearlyEqual(Load.Compression, ExpectedCompression, Tolerance)
				&& FMath::IsNearlyEqual(Load.Shear, ExpectedShear, Tolerance)
				&& FMath::IsNearlyZero(Load.Tension, Tolerance));
	}

	/**
	 * The pieces that hold PieceIndex up, per DESIGN.md §3's two-tier rule.
	 *
	 * HONEST ABOUT WHAT THIS IS WORTH. It re-derives the tier rule from the spec
	 * rather than calling into Core/Structure, but it is a line-for-line transcription
	 * of RoleOf, threshold constant included, so it is NOT a second opinion about
	 * whether the tier rule is right. It catches transcription slips — a sign flipped,
	 * a case dropped, the fallback applied when there IS a bed joint — and nothing
	 * conceptual: if the two-tier rule itself were wrong, this would be wrong in
	 * exactly the same way and agree enthusiastically.
	 *
	 * SpecReachesGround, built on top of it, is a different matter and earns the
	 * stronger claim ONLY FOR STRUCTURES THAT STRAND NOTHING: it walks upward from one
	 * piece to the earth, where production walks outward from every grounded piece at
	 * once. Same relation, genuinely different algorithm, so the reachability answer
	 * really is checked twice.
	 *
	 * WHERE LOAD CANNOT BE ROUTED IT IS NOT A SECOND OPINION AT ALL. DESIGN.md §3 says
	 * the pieces caught in an unroutable knot are reported falling even though they do
	 * reach the earth through the support relation — that is a rule about the solver's
	 * inability to divide load round a loop, and this walk knows nothing about it, so
	 * it would confidently call those pieces supported. Any spec containing a knot must
	 * therefore be given an EXPLICIT expectation table (SupportCycle, StrandingIsLocal,
	 * StrandingPropagatesUpward) rather than being dropped into the DegenerateInputs
	 * matrix, where this walk is the expectation. Every spec in that matrix today
	 * strands nothing; check that before adding one.
	 *
	 * A connection is described by a normal pointing toward PieceB, so to ask what
	 * it means for THIS piece the normal is first turned to point at this piece.
	 * Pointing substantially UP at it, the joint is a bed joint beneath it and bears
	 * it. Pointing substantially DOWN at it, the bed joint is ABOVE it — that is
	 * something resting on this piece, or something this piece is glued underneath,
	 * and neither holds it up. Anything in between is a head joint, and head joints
	 * are supports only when there is no bed joint at all.
	 *
	 * The relation is DIRECTED, which is the whole correction: raw connectivity made
	 * a short sideways path exclude the bed joint that was actually carrying the
	 * wall.
	 */
	TArray<int32> SpecSupportsOf(const FStructureSpec& Spec, int32 PieceIndex)
	{
		TArray<int32> BedSupports;
		TArray<int32> HeadNeighbours;

		for (const FConnectionSpec& Connection : Spec.Connections)
		{
			FVector UnitNormal = Connection.Normal;
			if (!UnitNormal.Normalize())
			{
				continue;
			}

			int32 Other = INDEX_NONE;
			double NormalZTowardPiece = 0.0;

			if (Connection.PieceB == PieceIndex)
			{
				Other = Connection.PieceA;
				NormalZTowardPiece = UnitNormal.Z;
			}
			else if (Connection.PieceA == PieceIndex)
			{
				Other = Connection.PieceB;
				NormalZTowardPiece = -UnitNormal.Z;
			}
			else
			{
				continue;
			}

			if (FMath::Abs(NormalZTowardPiece) > BedJointCosine)
			{
				if (NormalZTowardPiece > 0.0)
				{
					BedSupports.Add(Other);
				}
			}
			else
			{
				HeadNeighbours.Add(Other);
			}
		}

		return BedSupports.Num() > 0 ? BedSupports : HeadNeighbours;
	}

	/**
	 * Does this piece reach the ground through SUPPORTS?
	 *
	 * Not through connections. Being joined to a neighbour is not support, and
	 * neither is being joined to something that hangs off you: two pieces can each
	 * hang from the other and neither reach the earth, which is why this walk is
	 * directed and tracks what it has already visited.
	 */
	bool SpecReachesGround(const FStructureSpec& Spec, int32 PieceIndex)
	{
		TSet<int32> Visited;
		TArray<int32> Frontier;

		Visited.Add(PieceIndex);
		Frontier.Add(PieceIndex);

		while (Frontier.Num() > 0)
		{
			const int32 Current = Frontier.Pop();

			if (Spec.Pieces[Current].bIsGrounded)
			{
				return true;
			}

			for (const int32 Support : SpecSupportsOf(Spec, Current))
			{
				if (!Visited.Contains(Support))
				{
					Visited.Add(Support);
					Frontier.Add(Support);
				}
			}
		}

		return false;
	}

	/** A structure, what every joint must carry, and which pieces must be held up. */
	struct FSolveCase
	{
		const TCHAR* Description;
		FStructureSpec Spec;

		/** What each connection carries and how it resolves, in connection-array order. */
		TArray<FExpectedJoint> ExpectedJoints;

		/** Whether each piece is held up, in piece-array order. */
		TArray<bool> ExpectedSupported;
	};

	/**
	 * Build, solve, and check one case against its expectation table.
	 *
	 * Expectations are EXPLICIT here rather than derived from SpecReachesGround. For
	 * the knot cases that walk does not model the stranding rule at all (see its own
	 * comment); for the rest an explicit table is what makes the load figures, which
	 * it cannot produce, checkable at the same time.
	 *
	 * The conservation sum at the end is a CROSS-CHECK, not the assertion these tests
	 * turn on. Over-stranding preserves conservation exactly — the piece's weight
	 * leaves both sides of the equation at once — so it is blind to the whole class of
	 * defect the knot cases exist for. It is here to catch the arithmetic of
	 * "everything except the unroutable contribution" being off, and it is why LoadPath
	 * runs through here too rather than keeping its own near-identical loop.
	 */
	void CheckSolveCase(FAutomationTestBase& Test, const FSolveCase& Case)
	{
		constexpr double Tolerance = 1.0e-6;

		FStructure Structure;
		BuildStructure(Structure, Case.Spec, Unbreakable);

		Test.TestTrue(
			FString::Printf(TEXT("%s: expected %d pieces, got %d"),
				Case.Description, Case.Spec.Pieces.Num(), Structure.NumPieces()),
			Structure.NumPieces() == Case.Spec.Pieces.Num());

		Test.TestTrue(
			FString::Printf(TEXT("%s: expected %d connections, got %d"),
				Case.Description, Case.Spec.Connections.Num(), Structure.NumConnections()),
			Structure.NumConnections() == Case.Spec.Connections.Num());

		Structure.SolveLoads();

		/*
		 * Both loops below are bounded by the EXPECTATION arrays, so a case that gained
		 * a piece or a joint and forgot its row would quietly assert nothing about it.
		 */
		Test.TestTrue(
			FString::Printf(TEXT("%s: expected a row for each of %d connections, got %d rows"),
				Case.Description, Structure.NumConnections(), Case.ExpectedJoints.Num()),
			Case.ExpectedJoints.Num() == Structure.NumConnections());

		Test.TestTrue(
			FString::Printf(TEXT("%s: expected a row for each of %d pieces, got %d rows"),
				Case.Description, Structure.NumPieces(), Case.ExpectedSupported.Num()),
			Case.ExpectedSupported.Num() == Structure.NumPieces());

		for (int32 Index = 0; Index < Case.ExpectedSupported.Num(); ++Index)
		{
			Test.TestTrue(
				FString::Printf(TEXT("%s: piece %d support, expected %d, got %d"),
					Case.Description, Index,
					Case.ExpectedSupported[Index] ? 1 : 0,
					Structure.IsPieceSupported(Index) ? 1 : 0),
				Structure.IsPieceSupported(Index) == Case.ExpectedSupported[Index]);
		}

		for (int32 Index = 0; Index < Case.ExpectedJoints.Num(); ++Index)
		{
			CheckJointLoad(
				Test,
				Case.Description,
				Index,
				Structure.GetConnection(Index),
				Structure.GetConnectionForce(Index),
				Case.ExpectedJoints[Index]);
		}

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			Test.TestFalse(
				FString::Printf(TEXT("%s: connection %d must still be intact after solving"),
					Case.Description, Index),
				Structure.GetConnection(Index).HasGiven());
		}

		double ExpectedGroundReactionUU = 0.0;
		for (int32 Index = 0; Index < Case.ExpectedSupported.Num(); ++Index)
		{
			if (Case.ExpectedSupported[Index] && !Case.Spec.Pieces[Index].bIsGrounded)
			{
				ExpectedGroundReactionUU += WeightOf(Case.Spec.Pieces[Index].MassKg);
			}
		}

		double GroundReactionUU = 0.0;
		for (int32 Index = 0; Index < Case.Spec.Connections.Num(); ++Index)
		{
			const FConnectionSpec& Spec = Case.Spec.Connections[Index];
			if (Case.Spec.Pieces[Spec.PieceA].bIsGrounded || Case.Spec.Pieces[Spec.PieceB].bIsGrounded)
			{
				GroundReactionUU += FMath::Abs(Structure.GetConnectionForce(Index).Z);
			}
		}

		Test.TestTrue(
			FString::Printf(TEXT("%s: %f of weight is held up but %f reaches the ground"),
				Case.Description, ExpectedGroundReactionUU, GroundReactionUU),
			FMath::IsNearlyEqual(GroundReactionUU, ExpectedGroundReactionUU,
				FMath::Max(Tolerance, 1.0e-9 * ExpectedGroundReactionUU)));
	}

	/**
	 * A support state as text, so a failure names the answer instead of a number.
	 *
	 * The default arm is deliberately not a fifth name: it is what fires if the enum
	 * grows a value nobody wired in here, and "an unknown state" in a failure message
	 * is a better outcome than a plausible-looking label.
	 */
	const TCHAR* NameOfSupport(EPieceSupport State)
	{
		switch (State)
		{
		case EPieceSupport::Falling:   return TEXT("Falling");
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		default:                       return TEXT("an unknown state");
		}
	}

	/**
	 * The one invariant that ties the new accessor to the old one, checked on every
	 * piece of every case rather than on the interesting ones.
	 *
	 * IsPieceSupported is the composite answer and 38 tests read it; GetPieceSupport
	 * explains it. If the two are ever computed apart they will drift, and the drift
	 * would be silent — both would keep returning plausible answers about the same
	 * structure. This is what makes them one answer rather than two.
	 *
	 * It also pins the SHAPE of the enum, which nothing else does: exactly two of the
	 * four states mean "held up", so a fifth state added later has to be classified
	 * here on purpose rather than quietly landing on whichever side the switch fell
	 * through to.
	 */
	void CheckSupportAgreesWithReason(
		FAutomationTestBase& Test,
		const TCHAR* Description,
		const FStructure& Structure,
		int32 PieceIndex)
	{
		const EPieceSupport State = Structure.GetPieceSupport(PieceIndex);

		const bool bHeldUp = State == EPieceSupport::Grounded || State == EPieceSupport::Supported;

		Test.TestTrue(
			FString::Printf(
				TEXT("%s: piece %d reads %s but IsPieceSupported says %d — the two must tell one story"),
				Description, PieceIndex, NameOfSupport(State),
				Structure.IsPieceSupported(PieceIndex) ? 1 : 0),
			bHeldUp == Structure.IsPieceSupported(PieceIndex));

		Test.TestTrue(
			FString::Printf(TEXT("%s: piece %d reads %s, which is not one of the four states"),
				Description, PieceIndex, NameOfSupport(State)),
			State == EPieceSupport::Falling || State == EPieceSupport::Grounded
				|| State == EPieceSupport::Supported || State == EPieceSupport::Stranded);
	}
}

/**
 * A structure accumulates weight downward and hands each connection the share of
 * it that connection actually supports.
 *
 * Pure arithmetic over a graph — no world, no ticking solver, no positions — so
 * gravity is not "on" or "off" here in the DESIGN.md §4 sense: weight is an input
 * to the maths rather than something a simulation applies. The assertion is on
 * the mechanism, the force each joint carries, never on anything moving.
 *
 * SUPPORT IS TWO-TIERED (DESIGN.md §3). A piece's supports are the bed joints
 * BENEATH it — connections whose interface normal is substantially vertical — and
 * only a piece with none of those falls back to its head joints. Routing by graph
 * distance to the ground instead is wrong in the exact case the game is about: a
 * brick spanning a gap ends up the same distance from the earth as the brick
 * resting on top of it, so the bed joint between them carries nothing and the
 * keystone bears none of the wall above.
 *
 * The cases are chosen so that each rule changes an answer on its own:
 * accumulation is separable from the piece count (unequal masses), the area split
 * is separable from an even split (unequal areas), grounding is separable from
 * merely being connected (the ungrounded stack), a bed joint beneath is separable
 * from a bed joint above (the hanging case), and the tier rule is separable from
 * pure graph distance (the running-bond wall). One case that satisfied all of them
 * at once would prove much less.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureLoadPathTest,
	"DestructionGame.Core.Structure.LoadPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureLoadPathTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	/*
	 * THE DERIVATION RAN. BrickMassKg is computed at namespace scope from ClayBrick,
	 * which is safe only while ClayBrick is constant-initialised — see its comment. If
	 * that ever stops being true this reads zero, and a zero brick makes every force
	 * expectation in the file zero as well, so the suite would go green while asserting
	 * nothing. Not a second anchor on what a brick weighs; Profiles.MaterialInvariants
	 * owns that and this makes no claim about the value.
	 */
	TestTrue(
		FString::Printf(TEXT("the brick mass must derive to something positive, got %f kg"), BrickMassKg),
		BrickMassKg > 0.0);

	/*
	 * FSolveCase and CheckSolveCase, shared with the stranding tests. These cases used
	 * to carry a structurally identical local type and a near-identical loop; folding
	 * them buys the GROUND-REACTION CONSERVATION cross-check for free, which is a real
	 * gain here — every case below strands nothing, so the sum is exact and it catches
	 * a share that was computed correctly and then written to the wrong joint.
	 */
	const TArray<FSolveCase> Cases = {
		/*
		 * Nothing above it and nowhere to pass load to. The base case that stops a
		 * solver inventing a load out of a lone brick sitting on the earth.
		 */
		{
			TEXT("a lone grounded piece carries nothing"),
			{ { { BrickMassKg, true } }, {} },
			{},
			{ true }
		},

		/*
		 * One brick on one grounded brick: the joint carries exactly one brick.
		 * 2.7216 kg x 980 = 2667.2 uu, DESIGN.md §3's worked figure.
		 */
		{
			TEXT("a piece resting on a grounded piece loads the joint with its own weight"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false } },
				{ { 0, 1, BedJointNormal, JointAreaSqCm } }
			},
			{ { -BrickWeightUU, EJointKind::Bed } },
			{ true, true }
		},

		/*
		 * THE SAME JOINT, DECLARED UPPER PIECE FIRST. Piece 1 is still the brick on
		 * top; the only change is that it is now PieceA and the normal points down at
		 * the piece below, which ConnectionLoad.h's convention allows and a wall
		 * builder that walks its courses downward would produce naturally.
		 *
		 * The force belonging to a connection is the force acting on PIECE B, so
		 * describing this joint from the lower piece means negating it: +2666, up.
		 * Store -2666 instead and dot((0,0,-W),(0,0,-1)) is POSITIVE — the joint
		 * reads as tension, and mortar's tensile limit is 0.1 MPa against 10 MPa
		 * compressive, so a joint in perfectly ordinary compression would give at
		 * one percent of its real capacity.
		 */
		{
			TEXT("a bed joint declared upper piece first still resolves as compression"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false } },
				{ { 1, 0, InvertedBedJointNormal, JointAreaSqCm } }
			},
			{ { BrickWeightUU, EJointKind::Bed } },
			{ true, true }
		},

		/*
		 * THE HEART OF IT. Three high: the lower joint carries two bricks, the
		 * upper carries one. A solver that loads each joint with only the piece
		 * directly above it gets the upper joint right and the lower one half.
		 */
		{
			TEXT("a stack of three accumulates: the lower joint carries two pieces, the upper one"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, false } },
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				{ -2.0 * BrickWeightUU, EJointKind::Bed },
				{ -BrickWeightUU, EJointKind::Bed }
			},
			{ true, true, true }
		},

		/*
		 * Accumulation is by WEIGHT, not by piece count. Deliberately lopsided
		 * masses: counting pieces would give 2 and 1 units of something and could
		 * not produce 3920 and 2940.
		 */
		{
			TEXT("accumulation sums mass, not piece count"),
			{
				{ { BrickMassKg, true }, { 1.0, false }, { 3.0, false } },
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				{ -WeightOf(4.0), EJointKind::Bed },
				{ -WeightOf(3.0), EJointKind::Bed }
			},
			{ true, true, true }
		},

		/*
		 * Two supports of equal area split evenly. The physically defensible
		 * default, and the one that has to degenerate correctly.
		 */
		{
			TEXT("two supports of equal area split the load evenly"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, true }, { BrickMassKg, false } },
				{
					{ 0, 2, BedJointNormal, JointAreaSqCm },
					{ 1, 2, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				{ -BrickWeightUU / 2.0, EJointKind::Bed },
				{ -BrickWeightUU / 2.0, EJointKind::Bed }
			},
			{ true, true, true }
		},

		/*
		 * Unequal areas split in proportion to area: 100 and 300 cm2 take a
		 * quarter and three quarters. An even split would give 1333 to both, so
		 * this is the case that distinguishes "weighted by area" from "shared".
		 */
		{
			TEXT("unequal supports split in proportion to interface area"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, true }, { BrickMassKg, false } },
				{
					{ 0, 2, BedJointNormal, JointAreaSqCm },
					{ 1, 2, BedJointNormal, 3.0 * JointAreaSqCm }
				}
			},
			{
				{ -BrickWeightUU * 0.25, EJointKind::Bed },
				{ -BrickWeightUU * 0.75, EJointKind::Bed }
			},
			{ true, true, true }
		},

		/*
		 * DESIGN.md §2's worked example, end to end. Piece 2 spans a gap like a
		 * keystone: its only path to ground runs sideways through two VERTICAL
		 * head joints, so it and the brick on top of it push their whole combined
		 * weight through those. The force is straight down regardless — it is
		 * FConnection that turns the same downward vector into shear here and
		 * compression on the bed joint above, which is exactly why the solver must
		 * not try to orient the force along the interface normal itself.
		 *
		 * Piece 3 loads joint 2 with one brick; piece 2 then passes two bricks'
		 * weight down, split evenly across the two equal head joints.
		 */
		{
			TEXT("a piece supported only by head joints routes its whole weight sideways"),
			{
				{
					{ BrickMassKg, true },
					{ BrickMassKg, true },
					{ BrickMassKg, false },
					{ BrickMassKg, false }
				},
				{
					{ 0, 2, HeadJointNormal, JointAreaSqCm },
					{ 1, 2, OpposingHeadJointNormal, JointAreaSqCm },
					{ 2, 3, BedJointNormal, JointAreaSqCm }
				}
			},
			/*
			 * Three identical magnitudes, and the classification is the only thing
			 * that tells them apart: the two head joints are in pure shear, where
			 * masonry is weak, and the bed joint above is in pure compression, where
			 * it is strong. Asserting 2666 three times would read the same if the
			 * solver had crushed all three.
			 */
			{
				{ -BrickWeightUU, EJointKind::Head },
				{ -BrickWeightUU, EJointKind::Head },
				{ -BrickWeightUU, EJointKind::Bed }
			},
			{ true, true, true, true }
		},

		/*
		 * THE SAME KEYSTONE, every joint declared supported-piece-first. Nothing
		 * about the structure changed; only which end of each connection was named
		 * PieceA. Every force therefore flips sign, and every classification must
		 * come out identical — that invariance is what makes per-piece bookkeeping
		 * safe, and it is stated in ConnectionLoad.h as the orientation convention.
		 */
		{
			TEXT("declaration order flips the stored force but not the classification"),
			{
				{
					{ BrickMassKg, true },
					{ BrickMassKg, true },
					{ BrickMassKg, false },
					{ BrickMassKg, false }
				},
				{
					{ 2, 0, OpposingHeadJointNormal, JointAreaSqCm },
					{ 2, 1, HeadJointNormal, JointAreaSqCm },
					{ 3, 2, InvertedBedJointNormal, JointAreaSqCm }
				}
			},
			{
				{ BrickWeightUU, EJointKind::Head },
				{ BrickWeightUU, EJointKind::Head },
				{ BrickWeightUU, EJointKind::Bed }
			},
			{ true, true, true, true }
		},

		/*
		 * A BED JOINT ABOVE A PIECE DOES NOT HOLD IT UP. Piece 1 is glued under the
		 * grounded slab 2 and also touches the grounded piece 0 sideways. The bed
		 * joint is the wrong way round to bear anything — it is the slab resting on
		 * nothing and the brick hanging off it — so piece 1 has no bed joint beneath
		 * it and falls back to its one head joint, which then takes the whole brick.
		 *
		 * Routing by graph distance splits the weight evenly between the two, which
		 * is how a load path ends up running upward through a joint in tension.
		 */
		{
			TEXT("a bed joint above a piece does not support it; the head joint takes it all"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, true } },
				{
					{ 1, 2, BedJointNormal, JointAreaSqCm },
					{ 0, 1, HeadJointNormal, JointAreaSqCm }
				}
			},
			{
				{ 0.0, EJointKind::Bed },
				{ -BrickWeightUU, EJointKind::Head }
			},
			{ true, true, true }
		},

		/*
		 * THE CASE THAT DOES NOT EXIST ANYWHERE ELSE, and the one the two-tier rule
		 * was written for: a running-bond wall with two bottom bricks pulled out, so
		 * the middle of the bottom course spans a gap, and TWO WHOLE COURSES stacked
		 * above it.
		 *
		 *        E1          course E   piece 5
		 *     D1    D2       course D   pieces 3, 4
		 *   Cx  Cy  Cz       course C   pieces 0, 1, 2   (Cy spans the gap)
		 *   ==      ==       earth, missing under Cy
		 *
		 * Cx and Cz rest on the earth. Cy reaches it only sideways, through its two
		 * head joints. D1 straddles Cx and Cy, D2 straddles Cy and Cz, E1 sits on
		 * both — running bond, every bed joint the same area, so every split is even.
		 *
		 * Under the tier rule Cy is genuinely load-bearing. E1 pushes half a brick
		 * into each of D1 and D2; each of those carries 1.5 bricks and puts 0.75 into
		 * each of its two bed joints; Cy therefore receives 1.5 bricks from above,
		 * adds its own, and pushes 2.5 bricks out sideways — 1.25 through each head
		 * joint. Ground reaction: 2 bricks into Cx and 2 into Cz, which is exactly
		 * the four ungrounded bricks the wall is holding up.
		 *
		 * Routing by graph distance gives Cy the same depth as D1 and D2, so the bed
		 * joints onto it carry ZERO, Cy's head joints see only its own weight split
		 * two ways, and the answer stops changing however many courses are added.
		 * That is the bug: the keystone bears none of the wall, and DESIGN.md §4's
		 * shear test cannot fire because the shear never arrives.
		 *
		 * Connection 1 is deliberately declared keystone-first — a builder walking a
		 * course left to right names Cy before Cz — so the sign convention is under
		 * test inside the case that matters rather than only in isolation.
		 */
		{
			TEXT("a running-bond wall routes two courses through a spanning brick's head joints"),
			{
				{
					{ BrickMassKg, true },  // 0: Cx, on the earth
					{ BrickMassKg, false }, // 1: Cy, spanning the gap
					{ BrickMassKg, true },  // 2: Cz, on the earth
					{ BrickMassKg, false }, // 3: D1
					{ BrickMassKg, false }, // 4: D2
					{ BrickMassKg, false }  // 5: E1
				},
				{
					{ 0, 1, HeadJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 0, 3, BedJointNormal, JointAreaSqCm },
					{ 1, 3, BedJointNormal, JointAreaSqCm },
					{ 1, 4, BedJointNormal, JointAreaSqCm },
					{ 2, 4, BedJointNormal, JointAreaSqCm },
					{ 3, 5, BedJointNormal, JointAreaSqCm },
					{ 4, 5, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				{ -1.25 * BrickWeightUU, EJointKind::Head }, // Cx <- Cy
				{ +1.25 * BrickWeightUU, EJointKind::Head }, // Cy -> Cz, declared keystone-first
				{ -0.75 * BrickWeightUU, EJointKind::Bed },  // Cx <- D1
				{ -0.75 * BrickWeightUU, EJointKind::Bed },  // Cy <- D1
				{ -0.75 * BrickWeightUU, EJointKind::Bed },  // Cy <- D2
				{ -0.75 * BrickWeightUU, EJointKind::Bed },  // Cz <- D2
				{ -0.50 * BrickWeightUU, EJointKind::Bed },  // D1 <- E1
				{ -0.50 * BrickWeightUU, EJointKind::Bed }   // D2 <- E1
			},
			{ true, true, true, true, true, true }
		},

		/*
		 * A grounded piece terminates the flow, so a joint between two grounded
		 * pieces has nothing to carry: neither one is holding the other up. This
		 * is what stops load circulating sideways along a foundation course.
		 */
		{
			TEXT("a joint between two grounded pieces carries nothing"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, true } },
				{ { 0, 1, HeadJointNormal, JointAreaSqCm } }
			},
			{ { 0.0, EJointKind::Head } },
			{ true, true }
		},

		/*
		 * Connected, but to nothing that reaches the earth. Being joined to a
		 * neighbour is not support; this is the distinction the whole grounding
		 * flag exists to make, and eventually it is what "the wall fell" means.
		 * Nothing is holding this stack up, so there is no static load path and
		 * the joint carries zero rather than a plausible-looking weight.
		 */
		{
			TEXT("a stack with nothing grounded is unsupported and carries no static load"),
			{
				{ { BrickMassKg, false }, { BrickMassKg, false } },
				{ { 0, 1, BedJointNormal, JointAreaSqCm } }
			},
			{ { 0.0, EJointKind::Bed } },
			{ false, false }
		},

		/*
		 * A grounded piece elsewhere in the structure does not help an island that
		 * cannot reach it. Support is a path, not the mere presence of ground.
		 */
		{
			TEXT("an island cannot borrow support from a grounded piece it does not touch"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, false } },
				{ { 1, 2, BedJointNormal, JointAreaSqCm } }
			},
			{ { 0.0, EJointKind::Bed } },
			{ true, false, false }
		},

		/*
		 * A SUPPORT THAT IS ITSELF FALLING IS NOT A SUPPORT.
		 *
		 *          D          piece 3, floating
		 *          |  bed joint, D above F
		 *   G  —   C  —  F    pieces 0, 1, 2, joined sideways
		 *   ==                earth, under G only
		 *
		 * C has no bed joint beneath it, so it falls back to BOTH its head joints —
		 * sideways to the grounded G, and sideways to F. F, though, has a bed joint
		 * beneath it (D), so the tier rule discards the C—F joint from F's OWN support
		 * list. That makes this deliberately NOT the cycle case: the ordering runs to
		 * completion, and the walk over supports already, correctly, reports F and D
		 * unsupported. The only question is where C's weight goes.
		 *
		 * Splitting it evenly writes 1333 to the C—F joint, credits it to a piece that
		 * is itself falling and never becomes ready, and loses it there. Two things
		 * break at once. Only half of C's weight arrives at the earth, so ground
		 * reaction conservation fails by 2x. And G—C, the one joint genuinely carrying
		 * C down to the ground, reports HALF its true load — in phase 2b that is a
		 * joint at 1.9x utilisation reading 0.95x and standing there for ever.
		 *
		 * So a support with no path to ground drops out of the area total AND of the
		 * share, and G—C takes the whole 2666. Dropping it is always safe: a supported
		 * ungrounded piece was only marked supported because the walk reached it
		 * THROUGH a supported support, so at least one always survives the filter and
		 * there is never nothing left to divide by. It is also what Structure.h already
		 * promises — a connection with no path to ground reports zero — which today
		 * holds for C—F's far side (D—F carries zero) but not for C—F itself.
		 */
		{
			TEXT("a support that does not itself reach the ground takes none of the load"),
			{
				{
					{ BrickMassKg, true },  // 0: G, on the earth
					{ BrickMassKg, false }, // 1: C, held sideways by G and (nominally) F
					{ BrickMassKg, false }, // 2: F, resting on D and reaching nothing
					{ BrickMassKg, false }  // 3: D, floating
				},
				{
					{ 0, 1, HeadJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 3, 2, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				{ -BrickWeightUU, EJointKind::Head }, // G <- C: the whole brick, not half
				{ 0.0, EJointKind::Head },            // C -> F: F is falling, so it bears nothing
				{ 0.0, EJointKind::Bed }              // D -> F: nothing here is held up at all
			},
			{ true, true, false, false }
		},

		/*
		 * MUTUAL LATERAL SUPPORT: two pieces each hanging from the other, with no
		 * path to the earth between them. Neither has a bed joint, so both fall back
		 * to the head joint and each names the other as its support — a two-cycle in
		 * the support relation. Both are correctly unsupported, and a walk that does
		 * not track where it has been will sit here forever.
		 */
		{
			TEXT("two pieces hanging from each other support neither"),
			{
				{ { BrickMassKg, false }, { BrickMassKg, false } },
				{ { 0, 1, HeadJointNormal, JointAreaSqCm } }
			},
			{ { 0.0, EJointKind::Head } },
			{ false, false }
		},
	};

	for (const FSolveCase& Case : Cases)
	{
		CheckSolveCase(*this, Case);
	}

	return true;
}

/**
 * Where the line between a bed joint and a head joint sits.
 *
 * The two-tier rule turns entirely on "substantially vertical", so that phrase
 * needs a number or the tier a joint lands in is whatever the implementation
 * happened to do. The threshold chosen here is 45 degrees from vertical — see the
 * justification beside BedJointCosine above — and these cases pin it from both
 * sides rather than asserting the exact tie, which is not a behaviour worth
 * locking down in floating point.
 *
 * One piece, held by two joints of EQUAL AREA, so area cannot explain the answer:
 * one joint at a varying tilt, the other flat vertical. If the tilted joint counts
 * as a bed joint it is the only support and takes the entire brick; if it counts
 * as a head joint the piece has no bed joint at all, falls back to both head
 * joints, and they split the brick evenly. 2666 against 1333 — nothing subtle
 * about the gap, and no tolerance can blur it.
 *
 * The 0 and 90 degree rows are the anchors. 0 degrees also states something the
 * tier rule needs on its own: a bed joint beneath wins OUTRIGHT, taking the whole
 * load rather than sharing it with a head joint that could only have carried it in
 * shear anyway.
 *
 * SCOPE, because it is narrower than it looks. This asserts WHICH TIER a tilted
 * joint lands in and therefore what share it takes — magnitudes only. It never
 * calls ClassifyForce, and its tilted joint is declared { 0, 2, tilted }, naming the
 * loaded piece SECOND, which is the one orientation of an inclined face that cannot
 * produce tension. Structure.TiltedJointClassification covers the other orientation
 * and the compression/shear/tension split that neither of these tests reaches.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureSupportTierThresholdTest,
	"DestructionGame.Core.Structure.SupportTierThreshold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureSupportTierThresholdTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	struct FThresholdCase
	{
		const TCHAR* Description;

		/** Tilt of the first joint's normal away from straight up, degrees. */
		double DegreesFromVertical;

		/** Fraction of the piece's weight each joint should end up carrying. */
		double ExpectedTiltedShare;
		double ExpectedFlatShare;
	};

	const TArray<FThresholdCase> Cases = {
		{ TEXT("a flat bed joint beneath takes the whole load, not a share of it"), 0.0, 1.0, 0.0 },
		{ TEXT("a joint 40 degrees off vertical still bears as a bed joint"), 40.0, 1.0, 0.0 },
		{ TEXT("a joint 50 degrees off vertical is a head joint and shares"), 50.0, 0.5, 0.5 },
		{ TEXT("a fully vertical joint is a head joint and shares"), 90.0, 0.5, 0.5 },
	};

	constexpr double Tolerance = 1.0e-6;

	for (const FThresholdCase& Case : Cases)
	{
		const FStructureSpec Spec = {
			{ { BrickMassKg, true }, { BrickMassKg, true }, { BrickMassKg, false } },
			{
				{ 0, 2, NormalTiltedFromVertical(Case.DegreesFromVertical), JointAreaSqCm },
				{ 1, 2, HeadJointNormal, JointAreaSqCm }
			}
		};

		FStructure Structure;
		BuildStructure(Structure, Spec, Unbreakable);
		Structure.SolveLoads();

		const double ExpectedTiltedZ = -Case.ExpectedTiltedShare * BrickWeightUU;
		const double ExpectedFlatZ = -Case.ExpectedFlatShare * BrickWeightUU;

		const FVector TiltedForce = Structure.GetConnectionForce(0);
		const FVector FlatForce = Structure.GetConnectionForce(1);

		TestTrue(
			FString::Printf(TEXT("%s: the tilted joint should carry Z = %f, got %f"),
				Case.Description, ExpectedTiltedZ, TiltedForce.Z),
			FMath::IsNearlyEqual(TiltedForce.Z, ExpectedTiltedZ, Tolerance));

		TestTrue(
			FString::Printf(TEXT("%s: the flat head joint should carry Z = %f, got %f"),
				Case.Description, ExpectedFlatZ, FlatForce.Z),
			FMath::IsNearlyEqual(FlatForce.Z, ExpectedFlatZ, Tolerance));

		/*
		 * Whichever tier it lands in, the load itself is still gravity and still
		 * points straight down. Only what the joint DOES with it changes.
		 */
		TestTrue(
			FString::Printf(TEXT("%s: loads must be vertical, got (%f, %f, %f) and (%f, %f, %f)"),
				Case.Description,
				TiltedForce.X, TiltedForce.Y, TiltedForce.Z,
				FlatForce.X, FlatForce.Y, FlatForce.Z),
			FMath::IsNearlyZero(TiltedForce.X, Tolerance) && FMath::IsNearlyZero(TiltedForce.Y, Tolerance)
				&& FMath::IsNearlyZero(FlatForce.X, Tolerance) && FMath::IsNearlyZero(FlatForce.Y, Tolerance));
	}

	return true;
}

/**
 * What an INCLINED joint does with the load it is handed — including pulling the
 * piece open, which is an accepted outcome of the model and not a defect.
 *
 * A CHARACTERISATION TEST, stated plainly. Every assertion below passes against the
 * solver as it stands. It drives no new behaviour and it is not a red step; it exists
 * because the behaviour it pins is currently asserted NOWHERE and is easy to mistake
 * for a bug on first encounter.
 *
 * WHAT IS UNCOVERED TODAY. Every normal in every other case in this file is
 * axis-aligned, with one exception: SupportTierThreshold tilts a joint, but it asserts
 * magnitudes only, never calls ClassifyForce, and declares that joint { 0, 2, tilted } —
 * loaded piece SECOND — which is the single orientation of an inclined face that cannot
 * produce tension. So the classification of a tilted joint has no coverage at all, and
 * two comments in this file that read as properties of the MODEL ("tension is always
 * zero", "a gravity load path never pulls a joint open") are in fact properties of the
 * FIXTURES. Both are now scoped; this is what they are scoped against.
 *
 * THE SHAPE. One brick, one anchor on the earth, one inclined joint between them, and
 * the only thing that varies is the tilt and WHICH SIDE OF THE PIECE THE FACE IS ON:
 *
 *     joint BENEATH               joint ABOVE
 *     anchor is PieceA            LOADED PIECE IS PieceA
 *     normal points up at         normal points up at the anchor, so it points
 *     the loaded piece            DOWN at the loaded piece
 *
 *          [brick]                    \  <- anchor above
 *         /                            \
 *        / <- face                      [brick]  hangs off the face
 *     [anchor]                       =========
 *     ========
 *
 * Both are the same tilt and the same brick, so both carry the same magnitude. What
 * differs is the axis, and that is the whole finding:
 *
 *   RoleOf is SIGN-BLIND ABOVE 45 DEGREES. It asks |NormalZTowardPiece| > cos45 and
 *   only then looks at the sign, so a joint tilted past 45 sitting ABOVE a piece is
 *   classified Head — and a head joint is a fallback support. The piece therefore hangs
 *   from the face over it and the stored force resolves as TENSION.
 *
 * DESIGN.md §3 says exactly this and accepts it: "a brick hanging off an inclined face
 * really is being pulled off it", and a joint under 45 above the piece is BedAbove,
 * bears nothing, and the piece falls instead. Same outcome by a different route, which
 * is why the 45 line looks like a cliff and is not one. The 40-degree rows below are
 * that other route: unsupported, zero load, no tension anywhere.
 *
 * THE NUMBERS, for a vertical load W through a face tilted T from vertical. The normal
 * component is W cos T and the in-plane component is W sin T, so at 46 degrees — the
 * angle DESIGN.md names — 69% of the load is normal to the face and the rest is shear.
 * A bed joint at 40 degrees is likewise NOT pure compression: 77% compression and 64%
 * shear. Nothing in the axis-aligned suite can say that, because there sin or cos is
 * always exactly 0 or 1.
 *
 * Expected magnitudes are derived from the trigonometry; production resolves the same
 * quantity as a dot product and a subtraction, so they agree on the values and not on
 * the method. The assertion that matters is WHICH AXIS the normal component lands on,
 * and that no arrangement of arithmetic can fake.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureTiltedJointClassificationTest,
	"DestructionGame.Core.Structure.TiltedJointClassification",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureTiltedJointClassificationTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	/** Which axis the component normal to the interface ends up on. */
	enum class ENormalAxis : uint8
	{
		/** The piece is not held up at all, so there is no load to resolve. */
		Unloaded,

		/** The face is being squeezed. */
		Compression,

		/** The face is being pulled open. */
		Tension,
	};

	struct FTiltCase
	{
		const TCHAR* Description;

		/** Tilt of the interface normal away from straight up, degrees. */
		double DegreesFromVertical;

		/**
		 * Whether the inclined face sits ABOVE the loaded piece.
		 *
		 * Which is the same thing as declaring the joint LOADED PIECE FIRST: the normal
		 * points toward PieceB, so naming the loaded piece as PieceA and the anchor as
		 * PieceB puts the face over the loaded piece's head.
		 */
		bool bJointAbovePiece;

		bool bExpectedSupported;
		ENormalAxis ExpectedNormalAxis;
	};

	const TArray<FTiltCase> Cases = {
		/*
		 * BENEATH THE PIECE — the ordinary arrangement, and the control. Whichever tier
		 * it lands in the face is under the brick, so it is squeezed. Compression at 40
		 * is the bed tier; compression at 50 and beyond is the head tier still being
		 * pressed, which is what makes the ABOVE rows the interesting ones rather than
		 * "tilted joints are weird".
		 */
		{ TEXT("a bed joint 40 degrees beneath a piece is compressed, and sheared too"),
			40.0, false, true, ENormalAxis::Compression },
		{ TEXT("a head joint 46 degrees beneath a piece is compressed"),
			46.0, false, true, ENormalAxis::Compression },
		{ TEXT("a head joint 50 degrees beneath a piece is compressed"),
			50.0, false, true, ENormalAxis::Compression },
		{ TEXT("a head joint 60 degrees beneath a piece is compressed"),
			60.0, false, true, ENormalAxis::Compression },
		/*
		 * The anchor row: at 90 the face is vertical, cos is zero, and the normal axis
		 * carries nothing whichever side it is on. Which axis that vanishing component
		 * lands on is still asserted, because it is the SIGN of the dot product, and a
		 * sign does not become meaningless just because the magnitude has.
		 */
		{ TEXT("a vertical head joint beneath a piece is pure shear, with nothing on the normal axis"),
			90.0, false, true, ENormalAxis::Compression },

		/*
		 * ABOVE THE PIECE — the finding. Under 45 the joint is BedAbove, bears nothing,
		 * and the brick has no support left, so it falls and the joint reports zero.
		 * Over 45 the sign-blind head tier makes the same face a support, and holding a
		 * brick up by a face above it means pulling that face open.
		 */
		{ TEXT("a bed joint 40 degrees above a piece bears nothing and the piece falls"),
			40.0, true, false, ENormalAxis::Unloaded },
		{ TEXT("a head joint 46 degrees above a piece supports it IN TENSION"),
			46.0, true, true, ENormalAxis::Tension },
		{ TEXT("a head joint 50 degrees above a piece supports it IN TENSION"),
			50.0, true, true, ENormalAxis::Tension },
		{ TEXT("a head joint 60 degrees above a piece supports it IN TENSION"),
			60.0, true, true, ENormalAxis::Tension },
		{ TEXT("a vertical head joint above a piece is pure shear, with nothing on the normal axis"),
			90.0, true, true, ENormalAxis::Tension },
	};

	constexpr double Tolerance = 1.0e-6;

	for (const FTiltCase& Case : Cases)
	{
		/*
		 * Piece 0 is the brick being held up; piece 1 is the anchor on the earth. The
		 * normal is the SAME VECTOR in both arrangements — only which end is named
		 * first changes, which is precisely the thing that has never been varied on a
		 * tilted joint before.
		 */
		const FVector Normal = NormalTiltedFromVertical(Case.DegreesFromVertical);

		const FConnectionSpec JointSpec = Case.bJointAbovePiece
			? FConnectionSpec{ 0, 1, Normal, JointAreaSqCm }
			: FConnectionSpec{ 1, 0, Normal, JointAreaSqCm };

		const FStructureSpec Spec = {
			{ { BrickMassKg, false }, { BrickMassKg, true } },
			{ JointSpec }
		};

		FStructure Structure;
		BuildStructure(Structure, Spec, Unbreakable);

		TestTrue(
			FString::Printf(TEXT("%s: the spec is well formed, got %d pieces and %d connections"),
				Case.Description, Structure.NumPieces(), Structure.NumConnections()),
			Structure.NumPieces() == 2 && Structure.NumConnections() == 1);

		Structure.SolveLoads();

		TestTrue(
			FString::Printf(TEXT("%s: the hanging piece support, expected %d, got %d"),
				Case.Description,
				Case.bExpectedSupported ? 1 : 0,
				Structure.IsPieceSupported(0) ? 1 : 0),
			Structure.IsPieceSupported(0) == Case.bExpectedSupported);

		/*
		 * The anchor is on the earth and is never in question; a case where it came out
		 * unsupported would make every load figure below meaningless.
		 */
		TestTrue(
			FString::Printf(TEXT("%s: the grounded anchor must stay supported"), Case.Description),
			Structure.IsPieceSupported(1));

		const double Radians = FMath::DegreesToRadians(Case.DegreesFromVertical);
		const double NormalMagnitude =
			Case.ExpectedNormalAxis == ENormalAxis::Unloaded ? 0.0 : BrickWeightUU * FMath::Cos(Radians);
		const double ShearMagnitude =
			Case.ExpectedNormalAxis == ENormalAxis::Unloaded ? 0.0 : BrickWeightUU * FMath::Sin(Radians);

		/*
		 * THE SIGN, which is what makes the two arrangements differ at all. The stored
		 * force is the force on PieceB, so naming the loaded piece FIRST stores the
		 * equal-and-opposite reaction, pointing UP. Same joint, same brick, opposite
		 * sign — and it is that sign, dotted into the same normal, that lands the load
		 * on tension rather than compression.
		 */
		double ExpectedForceZ = 0.0;
		if (Case.bExpectedSupported)
		{
			ExpectedForceZ = Case.bJointAbovePiece ? BrickWeightUU : -BrickWeightUU;
		}

		const FVector Force = Structure.GetConnectionForce(0);

		TestTrue(
			FString::Printf(TEXT("%s: the joint should carry Z = %f, got %f"),
				Case.Description, ExpectedForceZ, Force.Z),
			FMath::IsNearlyEqual(Force.Z, ExpectedForceZ, Tolerance));

		/*
		 * Still gravity, still straight down, however the face is angled. A solver that
		 * pushed the load along the interface normal would produce the right normal and
		 * shear magnitudes here and entirely the wrong direction.
		 */
		TestTrue(
			FString::Printf(TEXT("%s: the load must be vertical, got (%f, %f, %f)"),
				Case.Description, Force.X, Force.Y, Force.Z),
			FMath::IsNearlyZero(Force.X, Tolerance) && FMath::IsNearlyZero(Force.Y, Tolerance));

		const FConnectionLoad Load =
			DestructionForce::ClassifyForce(Force, Structure.GetConnection(0).InterfaceNormal);

		const double ExpectedCompression =
			Case.ExpectedNormalAxis == ENormalAxis::Compression ? NormalMagnitude : 0.0;
		const double ExpectedTension =
			Case.ExpectedNormalAxis == ENormalAxis::Tension ? NormalMagnitude : 0.0;

		TestTrue(
			FString::Printf(
				TEXT("%s: should resolve to compression %f / shear %f / tension %f, got %f / %f / %f"),
				Case.Description, ExpectedCompression, ShearMagnitude, ExpectedTension,
				Load.Compression, Load.Shear, Load.Tension),
			FMath::IsNearlyEqual(Load.Compression, ExpectedCompression, Tolerance)
				&& FMath::IsNearlyEqual(Load.Shear, ShearMagnitude, Tolerance)
				&& FMath::IsNearlyEqual(Load.Tension, ExpectedTension, Tolerance));

		/*
		 * The two components are the legs of a right triangle on the hypotenuse W, so
		 * they cannot both be got wrong in a way that still squares up. This is what
		 * stops the expectations above being satisfied by a solver that put the whole
		 * load on one axis and the trigonometry only in this test.
		 */
		const double Resolved = FMath::Sqrt(
			FMath::Square(Load.Compression + Load.Tension) + FMath::Square(Load.Shear));

		TestTrue(
			FString::Printf(TEXT("%s: the axes must recompose to the load, expected %f, got %f"),
				Case.Description, FMath::Abs(ExpectedForceZ), Resolved),
			FMath::IsNearlyEqual(Resolved, FMath::Abs(ExpectedForceZ), Tolerance));

		TestFalse(
			FString::Printf(TEXT("%s: the joint must still be intact after solving"), Case.Description),
			Structure.GetConnection(0).HasGiven());
	}

	/*
	 * WHY THE AXIS IS WORTH A TEST OF ITS OWN: the same brick on the same face is
	 * measured against a DIFFERENT STRENGTH depending on which side of it the face
	 * sits, and for mortar those strengths are two orders of magnitude apart.
	 *
	 * One 2.7216 kg brick on 100 cm2 of mortar through a face at 50 degrees. All three
	 * axes worked out, because ComputeUtilisation returns the WORST and a test aimed at
	 * tension that is silently governed by shear proves nothing:
	 *
	 *   normal component  W cos50 = 1714.44 uu -> 0.00171444 MPa
	 *   shear component   W sin50 = 2043.20 uu -> 0.00204320 MPa
	 *
	 *   FACE BENEATH (compression)
	 *     compression 0.00171444 / 10.0      = 0.000171   <- not it
	 *     shear       0.00204320 / (0.2 + 0.6 x 0.00171444 = 0.201029)
	 *                                        = 0.010164   <- GOVERNS
	 *     tension     0 / 0.1                = 0
	 *
	 *   FACE ABOVE (tension)
	 *     compression 0                      = 0
	 *     shear       0.00204320 / 0.2       = 0.010216   <- not it, and only just
	 *     tension     0.00171444 / 0.1       = 0.017144   <- GOVERNS
	 *
	 * The shear figures are within 0.5% of each other, so a test asserting only "the
	 * hanging joint is worse off" could pass on the shear axis alone. Each utilisation
	 * is therefore pinned to the axis it must come from, computed here from the SI
	 * definitions rather than from ForceUnitsPerMPaSqCm, so a wrong conversion constant
	 * shows up as a failure instead of an agreement.
	 *
	 * AND NEITHER JOINT GIVES. DESIGN.md §3 says such a joint "fails almost at once",
	 * which is a statement about the RATIO of mortar's tensile limit to its compressive
	 * one and not about this load: one brick over 100 cm2 is 1.7% of the tensile limit.
	 * Asserted, so nobody reads the design note as a promise the solver is not making.
	 */
	{
		constexpr double UtilisationTolerance = 1.0e-9;
		constexpr double DegreesFromVertical = 50.0;

		const double Radians = FMath::DegreesToRadians(DegreesFromVertical);
		const double NormalUU = BrickWeightUU * FMath::Cos(Radians);
		const double ShearUU = BrickWeightUU * FMath::Sin(Radians);

		const double NormalStressMPa = MPaForForce(NormalUU, JointAreaSqCm);
		const double ShearStressMPa = MPaForForce(ShearUU, JointAreaSqCm);

		/*
		 * IDENTICAL JOINTS. Same normal, same area, same mortar — ApplyForce never looks
		 * at the piece handles, so there is nothing else for them to differ in. The only
		 * difference is the SIGN of the force the solver stores, which the cases above
		 * assert is decided by which end of the joint was named first. Two objects rather
		 * than one because ApplyForce latches, and a shared one would make the second
		 * HasGiven check depend on the first call.
		 */
		FConnection Beneath;
		Beneath.InterfaceNormal = NormalTiltedFromVertical(DegreesFromVertical);
		Beneath.InterfaceAreaSqCm = JointAreaSqCm;
		Beneath.Strength = GeneralPurposeMortar;

		FConnection Hanging = Beneath;

		const double BeneathUtilisation = Beneath.ApplyForce(FVector(0.0, 0.0, -BrickWeightUU));
		const double AboveUtilisation = Hanging.ApplyForce(FVector(0.0, 0.0, BrickWeightUU));

		/*
		 * Mohr-Coulomb: only compression buys friction, so the hanging joint's shear
		 * capacity is bare cohesion.
		 */
		const double ExpectedBeneath =
			ShearStressMPa
			/ (GeneralPurposeMortar.ShearCohesionMPa
				+ GeneralPurposeMortar.FrictionCoefficient * NormalStressMPa);
		const double ExpectedAbove = NormalStressMPa / GeneralPurposeMortar.TensileStrengthMPa;

		TestTrue(
			FString::Printf(
				TEXT("the compressed face should be governed by SHEAR at %f, got %f"),
				ExpectedBeneath, BeneathUtilisation),
			FMath::IsNearlyEqual(BeneathUtilisation, ExpectedBeneath, UtilisationTolerance));

		TestTrue(
			FString::Printf(
				TEXT("the hanging face should be governed by TENSION at %f, got %f"),
				ExpectedAbove, AboveUtilisation),
			FMath::IsNearlyEqual(AboveUtilisation, ExpectedAbove, UtilisationTolerance));

		TestTrue(
			FString::Printf(
				TEXT("the hanging face carries the same load nearer failure, %f against %f"),
				AboveUtilisation, BeneathUtilisation),
			AboveUtilisation > BeneathUtilisation);

		TestTrue(
			FString::Printf(
				TEXT("one brick on 100 cm2 of mortar breaks neither face, got %f and %f"),
				BeneathUtilisation, AboveUtilisation),
			!Beneath.HasGiven() && !Hanging.HasGiven());
	}

	return true;
}

/**
 * The structure owns the graph, so nonsense handles and impossible interfaces are
 * rejected here rather than being carried around as healthy-looking joints.
 *
 * This closes a hole logged from the phase 1 review: an FConnection with
 * PieceA == PieceB, or with both handles left at INDEX_NONE, currently reads as a
 * perfectly fine joint under any load — while a zero interface area correctly
 * reads as failed. Nothing below the structure has the piece array needed to tell
 * the difference.
 *
 * Rejection means the connection is NOT added. Storing it and hoping the solver
 * skips it would leave a joint in the array that no rule applies to.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureGraphValidationTest,
	"DestructionGame.Core.Structure.GraphValidation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureGraphValidationTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	const double NaNValue = MakeNaN();
	const double InfinityValue = MakeInfinity();

	/*
	 * Pieces first: a mass nobody can make sense of must not enter the structure,
	 * because every load downstream of it would be equally meaningless and NaN
	 * launders itself into plausible-looking numbers once it is in the array.
	 */
	{
		struct FPieceCase
		{
			const TCHAR* Description;
			double MassKg;
			bool bIsAccepted;
		};

		const TArray<FPieceCase> PieceCases = {
			{ TEXT("a real brick"), BrickMassKg, true },
			{ TEXT("a massless piece"), 0.0, true },
			{ TEXT("a negative mass"), -BrickMassKg, false },
			{ TEXT("a NaN mass"), NaNValue, false },
			{ TEXT("an infinite mass"), InfinityValue, false },
		};

		for (const FPieceCase& Case : PieceCases)
		{
			FStructure Structure;
			const int32 Handle = Structure.AddPiece(Case.MassKg, false);
			const int32 ExpectedHandle = Case.bIsAccepted ? 0 : INDEX_NONE;
			const int32 ExpectedCount = Case.bIsAccepted ? 1 : 0;

			TestTrue(
				FString::Printf(TEXT("%s: expected handle %d, got %d"),
					Case.Description, ExpectedHandle, Handle),
				Handle == ExpectedHandle);

			TestTrue(
				FString::Printf(TEXT("%s: expected %d pieces, got %d"),
					Case.Description, ExpectedCount, Structure.NumPieces()),
				Structure.NumPieces() == ExpectedCount);
		}
	}

	/*
	 * Handles are sequential, and the returned handle is what a connection refers
	 * to. Without this the rejection cases above could be satisfied by an AddPiece
	 * that never accepts anything.
	 */
	{
		FStructure Structure;

		const int32 FirstHandle = Structure.AddPiece(BrickMassKg, true);
		const int32 SecondHandle = Structure.AddPiece(BrickMassKg, false);

		TestTrue(
			FString::Printf(TEXT("the first piece should be handle 0, got %d"), FirstHandle),
			FirstHandle == 0);
		TestTrue(
			FString::Printf(TEXT("the second piece should be handle 1, got %d"), SecondHandle),
			SecondHandle == 1);
		TestTrue(
			FString::Printf(TEXT("two pieces should have been added, got %d"), Structure.NumPieces()),
			Structure.NumPieces() == 2);

		TestTrue(
			FString::Printf(TEXT("piece 1 should know its own index, got %d"), Structure.GetPiece(1).Index),
			Structure.GetPiece(1).Index == 1);
		TestTrue(
			FString::Printf(TEXT("piece 1 keeps its mass, expected %f, got %f"),
				BrickMassKg, Structure.GetPiece(1).MassKg),
			FMath::IsNearlyEqual(Structure.GetPiece(1).MassKg, BrickMassKg, 1.0e-9));
		TestTrue(TEXT("piece 0 is grounded"), Structure.GetPiece(0).bIsGrounded);
		TestFalse(TEXT("piece 1 is not grounded"), Structure.GetPiece(1).bIsGrounded);
	}

	// Connections, against a structure that always has exactly two pieces, 0 and 1.
	{
		struct FConnectionCase
		{
			const TCHAR* Description;
			FConnectionSpec Spec;
			bool bIsAccepted;
		};

		const TArray<FConnectionCase> ConnectionCases = {
			{
				TEXT("a real bed joint between two real pieces"),
				{ 0, 1, BedJointNormal, JointAreaSqCm }, true
			},
			{
				TEXT("a joint from a piece to itself"),
				{ 0, 0, BedJointNormal, JointAreaSqCm }, false
			},
			{
				TEXT("an unset PieceA handle"),
				{ INDEX_NONE, 1, BedJointNormal, JointAreaSqCm }, false
			},
			{
				TEXT("an unset PieceB handle"),
				{ 0, INDEX_NONE, BedJointNormal, JointAreaSqCm }, false
			},
			{
				TEXT("both handles unset, as a default-constructed connection has them"),
				{ INDEX_NONE, INDEX_NONE, BedJointNormal, JointAreaSqCm }, false
			},
			{
				TEXT("a PieceB handle past the end of the piece array"),
				{ 0, 2, BedJointNormal, JointAreaSqCm }, false
			},
			{
				TEXT("a negative PieceA handle"),
				{ -5, 1, BedJointNormal, JointAreaSqCm }, false
			},

			/*
			 * Area has to be rejected at construction, not at solve time: the load
			 * split divides by the total supporting area, and there is no sensible
			 * number to divide by when that total is zero or NaN.
			 */
			{
				TEXT("a zero interface area"),
				{ 0, 1, BedJointNormal, 0.0 }, false
			},
			{
				TEXT("a negative interface area"),
				{ 0, 1, BedJointNormal, -JointAreaSqCm }, false
			},
			{
				TEXT("a NaN interface area"),
				{ 0, 1, BedJointNormal, NaNValue }, false
			},
			{
				TEXT("an infinite interface area"),
				{ 0, 1, BedJointNormal, InfinityValue }, false
			},

			/*
			 * A normal that will not normalise describes no interface plane, so
			 * there is no joint here to load.
			 */
			{
				TEXT("a zero-length interface normal"),
				{ 0, 1, FVector::ZeroVector, JointAreaSqCm }, false
			},
			{
				TEXT("a NaN interface normal"),
				{ 0, 1, FVector(NaNValue, NaNValue, NaNValue), JointAreaSqCm }, false
			},

			// A non-unit normal is a legitimate description of the same plane.
			{
				TEXT("a non-unit interface normal"),
				{ 0, 1, FVector(0.0, 0.0, 5.0), JointAreaSqCm }, true
			},
		};

		for (const FConnectionCase& Case : ConnectionCases)
		{
			FStructure Structure;
			Structure.AddPiece(BrickMassKg, true);
			Structure.AddPiece(BrickMassKg, false);

			const int32 Handle = Structure.AddConnection(MakeConnection(Case.Spec, Unbreakable));
			const int32 ExpectedHandle = Case.bIsAccepted ? 0 : INDEX_NONE;
			const int32 ExpectedCount = Case.bIsAccepted ? 1 : 0;

			TestTrue(
				FString::Printf(TEXT("%s: expected handle %d, got %d"),
					Case.Description, ExpectedHandle, Handle),
				Handle == ExpectedHandle);

			TestTrue(
				FString::Printf(TEXT("%s: expected %d connections, got %d"),
					Case.Description, ExpectedCount, Structure.NumConnections()),
				Structure.NumConnections() == ExpectedCount);
		}
	}

	return true;
}

/**
 * Solving is not destructive, however overloaded the structure is.
 *
 * Phase 2a computes loads; breaking is a separate, deliberate step. The obvious
 * shortcut — solve by calling FConnection::ApplyForce on each joint — would break
 * joints as a side effect of asking what they carry, which is wrong twice over:
 * ApplyForce latches, so a solve could not be re-run, and a joint that gave
 * mid-solve reports zero afterwards, making the load the solver just computed a
 * lie about a joint that no longer exists.
 *
 * NOTE what this means for anything that wants a utilisation while solving: there
 * is currently NO non-mutating way to evaluate a joint. If a later phase needs
 * to trial a load distribution, FConnection needs a const evaluator alongside the
 * committing call. This test only pins down that solving must not commit.
 *
 * The masses are deliberately absurd so that the load is past EVERY axis of a
 * real mortar joint, not merely past the weakest — worked through below so the
 * test cannot be quietly satisfied by a load that was never over the limit.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureSolveIsNonDestructiveTest,
	"DestructionGame.Core.Structure.SolveIsNonDestructive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureSolveIsNonDestructiveTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	// 20 tonnes on each of two 100 cm2 joints: 1.96e7 uu through each.
	constexpr double AbsurdMassKg = 20000.0;
	const double ExpectedPerJointUU = WeightOf(AbsurdMassKg);

	/*
	 * Mortar's compressive limit is 10 MPa over 100 cm2 = 1e7 uu, so the bed joint
	 * is at 1.96 and would give. The head joint sees the same force as shear against
	 * a capacity of at most its ceiling, 1.3 MPa = 1.3e6 uu, so it is fifteen times
	 * over. Both axes are past their limit; neither joint may latch anyway.
	 *
	 * BOTH CAPACITIES COME FROM THE PROFILE rather than being restated, so retuning
	 * mortar cannot leave this test comparing the load against a limit the joint no
	 * longer has. The masses above are absurd by two orders of magnitude, so no
	 * plausible retune makes them stop being an overload.
	 */
	const double CompressiveCapacityUU =
		ForceForMPa(GeneralPurposeMortar.CompressiveStrengthMPa, JointAreaSqCm);
	const double MaxShearCapacityUU =
		ForceForMPa(GeneralPurposeMortar.MaxShearStrengthMPa, JointAreaSqCm);

	/*
	 * TWO SEPARATE OVERLOADED PIECES, one per joint, which the two-tier rule of
	 * DESIGN.md §3 is what forces. Hanging both joints off a single piece — the
	 * shape this fixture had while routing went by graph distance — no longer loads
	 * them both: a piece with a bed joint beneath it does not fall back to its head
	 * joints, so the head joint would correctly carry zero and the compression and
	 * shear assertions below would be asserting things about an unloaded joint. A
	 * test that passes because its fixture stopped loading what it claims to load is
	 * a false pass, and this one has to keep BOTH axes genuinely past capacity or it
	 * proves nothing about a solve that declines to break them.
	 *
	 * So: piece 2 rests on grounded piece 0 through a bed joint, and piece 3 has
	 * nothing beneath it at all and hangs sideways off grounded piece 1 through its
	 * one head joint. Each carries its whole 20 tonnes.
	 */
	const FStructureSpec Spec = {
		{
			{ BrickMassKg, true },
			{ BrickMassKg, true },
			{ AbsurdMassKg, false },
			{ AbsurdMassKg, false }
		},
		{
			{ 0, 2, BedJointNormal, JointAreaSqCm },
			{ 1, 3, HeadJointNormal, JointAreaSqCm }
		}
	};

	FStructure Structure;
	BuildStructure(Structure, Spec, GeneralPurposeMortar);

	Structure.SolveLoads();

	constexpr double Tolerance = 1.0e-6;

	for (int32 Index = 0; Index < Spec.Connections.Num(); ++Index)
	{
		const FVector Force = Structure.GetConnectionForce(Index);

		TestTrue(
			FString::Printf(TEXT("connection %d should carry %f downward, got %f"),
				Index, ExpectedPerJointUU, Force.Z),
			FMath::IsNearlyEqual(Force.Z, -ExpectedPerJointUU, Tolerance));

		/*
		 * Without this the test could pass on a solver that computed nothing: a
		 * zero load leaves every joint intact for entirely the wrong reason.
		 *
		 * EACH JOINT AGAINST THE CAPACITY IT ACTUALLY USES. Connection 0 is the bed
		 * joint and resolves this load as pure compression; connection 1 is the head
		 * joint and resolves it as pure shear. Checking both bounds against both
		 * joints — as this did — asserted that the bed joint was past a shear capacity
		 * it carries none of, which is true but says nothing about whether the joint is
		 * overloaded on the axis that governs it.
		 */
		if (Index == 0)
		{
			TestTrue(
				FString::Printf(TEXT("the bed joint's load %f must be past mortar's compressive capacity %f"),
					FMath::Abs(Force.Z), CompressiveCapacityUU),
				FMath::Abs(Force.Z) > CompressiveCapacityUU);
		}
		else
		{
			TestTrue(
				FString::Printf(TEXT("the head joint's load %f must be past mortar's capped shear capacity %f"),
					FMath::Abs(Force.Z), MaxShearCapacityUU),
				FMath::Abs(Force.Z) > MaxShearCapacityUU);
		}

		TestFalse(
			FString::Printf(TEXT("connection %d must still be intact after an overloaded solve"), Index),
			Structure.GetConnection(Index).HasGiven());
	}

	/*
	 * Solving twice must give the same answer. A destructive solve would report a
	 * different, smaller load the second time round.
	 */
	Structure.SolveLoads();

	for (int32 Index = 0; Index < Spec.Connections.Num(); ++Index)
	{
		const FVector Force = Structure.GetConnectionForce(Index);

		TestTrue(
			FString::Printf(TEXT("re-solving connection %d should still give %f, got %f"),
				Index, ExpectedPerJointUU, Force.Z),
			FMath::IsNearlyEqual(Force.Z, -ExpectedPerJointUU, Tolerance));

		TestFalse(
			FString::Printf(TEXT("connection %d must still be intact after re-solving"), Index),
			Structure.GetConnection(Index).HasGiven());
	}

	return true;
}

/**
 * Properties that must hold across every structure, rather than particular
 * numbers — those are the LoadPath test's job.
 *
 * Five invariants, each guarding a way the model can fail silently:
 *
 *  - Every load is finite and never NaN. FMath::Max discards a NaN and FMath::Min
 *    replaces it, so a NaN mass or area that got as far as the arithmetic comes
 *    out the other side as a confident, plausible number rather than an obvious
 *    fault. The counterpart guard is at the door — GraphValidation — but this
 *    asserts the consequence, which is what actually matters.
 *  - Support is checked against an INDEPENDENT walk over the SUPPORT relation of
 *    the spec, so production is measured against a second opinion rather than
 *    itself. Note "support", not "connectivity": a piece hanging under a grounded
 *    slab is joined to the ground and still not held up by it.
 *  - GROUND-REACTION CONSERVATION. Everything the structure holds up must arrive
 *    at the earth: the loads on connections touching a grounded piece sum to
 *    exactly the weight of the supported, ungrounded pieces. The weaker form this
 *    replaced — no single joint carries more than the total — had 2x of slack on
 *    the triangle, where a solver that treated every neighbour as a support could
 *    put the full 1333 on all three joints and still pass.
 *  - NO JOINT IS EVER IN TENSION. Static gravity presses a bed joint together and
 *    slides a head joint; it never pulls one open. A non-zero tension means the
 *    force was stored against the wrong end of the connection, which is only
 *    visible once the solver's output is classified.
 *  - Solving never breaks a joint, over the whole matrix and not just the
 *    hand-picked case above.
 *
 * The shapes deliberately include the pathological ones: a long chain where the
 * base joint carries twenty pieces, a cycle where a naive downward walk could loop
 * forever or double-count, a structure with no ground at all, and the two-tier
 * cases where being attached to the earth is not the same as resting on it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureDegenerateInputTest,
	"DestructionGame.Core.Structure.DegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	struct FNamedSpec
	{
		const TCHAR* Description;
		FStructureSpec Spec;
	};

	TArray<FNamedSpec> Specs;

	Specs.Add({ TEXT("an empty structure"), {} });

	Specs.Add({
		TEXT("a lone ungrounded piece"),
		{ { { BrickMassKg, false } }, {} }
	});

	Specs.Add({
		TEXT("a massless piece on a grounded piece"),
		{
			{ { BrickMassKg, true }, { 0.0, false } },
			{ { 0, 1, BedJointNormal, JointAreaSqCm } }
		}
	});

	Specs.Add({
		TEXT("a very heavy piece on a grounded piece"),
		{
			{ { BrickMassKg, true }, { 1.0e6, false } },
			{ { 0, 1, BedJointNormal, JointAreaSqCm } }
		}
	});

	/*
	 * The same two bricks as above with the joint declared upper piece first, which
	 * ConnectionLoad.h's convention allows. The stored force must flip sign with it;
	 * leaving it downward turns a plainly compressed joint into a tensile one.
	 */
	Specs.Add({
		TEXT("a bed joint declared upper piece first"),
		{
			{ { BrickMassKg, true }, { BrickMassKg, false } },
			{ { 1, 0, InvertedBedJointNormal, JointAreaSqCm } }
		}
	});

	/*
	 * Attached to the earth, and not held up by it: the piece hangs UNDER a grounded
	 * slab, so its one bed joint is above it and bears nothing. Connectivity says
	 * supported, support says otherwise, and the two answers differ only here.
	 */
	Specs.Add({
		TEXT("a piece hanging beneath a grounded piece"),
		{
			{ { BrickMassKg, true }, { BrickMassKg, false } },
			{ { 1, 0, BedJointNormal, JointAreaSqCm } }
		}
	});

	/*
	 * Mutual lateral support: each falls back to the head joint and names the other,
	 * and neither reaches the earth. A support walk that does not track where it has
	 * been loops here.
	 */
	Specs.Add({
		TEXT("two pieces hanging from each other"),
		{
			{ { BrickMassKg, false }, { BrickMassKg, false } },
			{ { 0, 1, HeadJointNormal, JointAreaSqCm } }
		}
	});

	Specs.Add({
		TEXT("a very small and a very large interface sharing one piece"),
		{
			{ { BrickMassKg, true }, { BrickMassKg, true }, { BrickMassKg, false } },
			{
				{ 0, 2, BedJointNormal, 1.0e-6 },
				{ 1, 2, BedJointNormal, 1.0e6 }
			}
		}
	});

	/*
	 * A piece held sideways by one grounded neighbour and one FALLING neighbour.
	 * Piece 2 rests on the floating piece 3 and reaches nothing, so piece 1's whole
	 * weight has to go through the joint to piece 0. Splitting it evenly instead
	 * credits half of it to a piece that is itself falling, and that half never
	 * arrives anywhere — which only the conservation invariant can see, since every
	 * individual load still looks entirely plausible.
	 */
	Specs.Add({
		TEXT("a piece held by one grounded and one falling neighbour"),
		{
			{
				{ BrickMassKg, true }, { BrickMassKg, false },
				{ BrickMassKg, false }, { BrickMassKg, false }
			},
			{
				{ 0, 1, HeadJointNormal, JointAreaSqCm },
				{ 1, 2, HeadJointNormal, JointAreaSqCm },
				{ 3, 2, BedJointNormal, JointAreaSqCm }
			}
		}
	});

	/*
	 * A cycle: everything connected to everything, one piece grounded. A downward
	 * walk that does not track what it has already visited can loop here, and one
	 * that revisits can double-count the same weight.
	 */
	Specs.Add({
		TEXT("a fully connected triangle with one grounded piece"),
		{
			{ { BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, false } },
			{
				{ 0, 1, BedJointNormal, JointAreaSqCm },
				{ 1, 2, HeadJointNormal, JointAreaSqCm },
				{ 0, 2, HeadJointNormal, JointAreaSqCm }
			}
		}
	});

	// A cycle with nothing grounded at all: the same trap with no exit.
	Specs.Add({
		TEXT("a fully connected triangle with nothing grounded"),
		{
			{ { BrickMassKg, false }, { BrickMassKg, false }, { BrickMassKg, false } },
			{
				{ 0, 1, BedJointNormal, JointAreaSqCm },
				{ 1, 2, HeadJointNormal, JointAreaSqCm },
				{ 0, 2, HeadJointNormal, JointAreaSqCm }
			}
		}
	});

	/*
	 * A twenty-high chain, so the base joint carries nineteen bricks. Long enough
	 * that an accumulation error compounds visibly rather than hiding in tolerance.
	 */
	{
		FStructureSpec Chain;
		constexpr int32 ChainHeight = 20;
		for (int32 Index = 0; Index < ChainHeight; ++Index)
		{
			Chain.Pieces.Add({ BrickMassKg, Index == 0 });
			if (Index > 0)
			{
				Chain.Connections.Add({ Index - 1, Index, BedJointNormal, JointAreaSqCm });
			}
		}
		Specs.Add({ TEXT("a twenty-high chain"), Chain });
	}

	/*
	 * The running-bond wall from LoadPath, here for the conservation invariant: all
	 * four ungrounded bricks must arrive at the two grounded ones, whatever route
	 * the solver picks through the keystone.
	 */
	Specs.Add({
		TEXT("a running-bond wall spanning a gap"),
		{
			{
				{ BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, true },
				{ BrickMassKg, false }, { BrickMassKg, false }, { BrickMassKg, false }
			},
			{
				{ 0, 1, HeadJointNormal, JointAreaSqCm },
				{ 1, 2, HeadJointNormal, JointAreaSqCm },
				{ 0, 3, BedJointNormal, JointAreaSqCm },
				{ 1, 3, BedJointNormal, JointAreaSqCm },
				{ 1, 4, BedJointNormal, JointAreaSqCm },
				{ 2, 4, BedJointNormal, JointAreaSqCm },
				{ 3, 5, BedJointNormal, JointAreaSqCm },
				{ 4, 5, BedJointNormal, JointAreaSqCm }
			}
		}
	});

	// Two grounded islands and one floating island, all in one structure.
	Specs.Add({
		TEXT("two grounded islands and one floating island"),
		{
			{
				{ BrickMassKg, true }, { BrickMassKg, false },
				{ BrickMassKg, true }, { BrickMassKg, false },
				{ BrickMassKg, false }, { BrickMassKg, false }
			},
			{
				{ 0, 1, BedJointNormal, JointAreaSqCm },
				{ 2, 3, BedJointNormal, JointAreaSqCm },
				{ 4, 5, BedJointNormal, JointAreaSqCm }
			}
		}
	});

	constexpr double Tolerance = 1.0e-6;

	for (const FNamedSpec& Named : Specs)
	{
		FStructure Structure;
		BuildStructure(Structure, Named.Spec, Unbreakable);

		TestTrue(
			FString::Printf(TEXT("%s: every piece in the spec is well formed, expected %d, got %d"),
				Named.Description, Named.Spec.Pieces.Num(), Structure.NumPieces()),
			Structure.NumPieces() == Named.Spec.Pieces.Num());

		TestTrue(
			FString::Printf(TEXT("%s: every connection in the spec is well formed, expected %d, got %d"),
				Named.Description, Named.Spec.Connections.Num(), Structure.NumConnections()),
			Structure.NumConnections() == Named.Spec.Connections.Num());

		Structure.SolveLoads();

		double TotalSupportedWeightUU = 0.0;
		for (int32 Index = 0; Index < Named.Spec.Pieces.Num(); ++Index)
		{
			const bool bExpectedSupported = SpecReachesGround(Named.Spec, Index);

			TestTrue(
				FString::Printf(TEXT("%s: piece %d support, expected %d, got %d"),
					Named.Description, Index,
					bExpectedSupported ? 1 : 0,
					Structure.IsPieceSupported(Index) ? 1 : 0),
				Structure.IsPieceSupported(Index) == bExpectedSupported);

			if (bExpectedSupported && !Named.Spec.Pieces[Index].bIsGrounded)
			{
				TotalSupportedWeightUU += WeightOf(Named.Spec.Pieces[Index].MassKg);
			}
		}

		double TotalCarriedUU = 0.0;
		double GroundReactionUU = 0.0;

		for (int32 Index = 0; Index < Named.Spec.Connections.Num(); ++Index)
		{
			const FConnectionSpec& Spec = Named.Spec.Connections[Index];
			const FVector Force = Structure.GetConnectionForce(Index);

			TestFalse(
				FString::Printf(TEXT("%s: connection %d load must never be NaN, got (%f, %f, %f)"),
					Named.Description, Index, Force.X, Force.Y, Force.Z),
				Force.ContainsNaN());

			TestTrue(
				FString::Printf(TEXT("%s: connection %d load must be finite, got (%f, %f, %f)"),
					Named.Description, Index, Force.X, Force.Y, Force.Z),
				FMath::IsFinite(Force.X) && FMath::IsFinite(Force.Y) && FMath::IsFinite(Force.Z));

			/*
			 * Never sideways: gravity does not change direction because a joint
			 * happens to be vertical.
			 */
			TestTrue(
				FString::Printf(TEXT("%s: connection %d load must be vertical, got (%f, %f, %f)"),
					Named.Description, Index, Force.X, Force.Y, Force.Z),
				FMath::IsNearlyZero(Force.X, Tolerance) && FMath::IsNearlyZero(Force.Y, Tolerance));

			/*
			 * AND NEVER UPWARD — but "upward" is a statement about the joint, not
			 * about world Z. The force belonging to a connection is the force acting
			 * on PieceB, so which way it points depends on which end was declared
			 * first; the sign flips and the classification does not. For an
			 * AXIS-ALIGNED joint, static gravity presses a bed joint together and
			 * slides a head joint, so a non-zero TENSION is the declaration-independent
			 * way to say the load is running the wrong way through it.
			 *
			 * SCOPED TO THESE FIXTURES, DELIBERATELY. This is a property of the specs
			 * above — every normal in every one of them is axis-aligned — and NOT a
			 * property of the model. The head tier is sign-blind, so a joint past 45
			 * degrees off vertical sitting ABOVE a piece supports it in tension, which
			 * DESIGN.md §3 states outright and accepts: a piece hanging off an inclined
			 * face really is being pulled off it. A fuzz over these shapes with
			 * axis-aligned normals found no violation; the same fuzz with tilted normals
			 * produced tension in 337 of 6000 structures.
			 *
			 * So the first tilted normal added to this matrix will turn this row red,
			 * and that will be a DISCOVERY rather than a regression. Characterise the
			 * numbers in Structure.TiltedJointClassification, then scope this assertion
			 * to the axis-aligned rows rather than deleting it.
			 */
			const FConnectionLoad Load = DestructionForce::ClassifyForce(
				Force, Structure.GetConnection(Index).InterfaceNormal);

			TestTrue(
				FString::Printf(
					TEXT("%s: connection %d carries %f in tension; no AXIS-ALIGNED gravity load path pulls a joint open"),
					Named.Description, Index, Load.Tension),
				FMath::IsNearlyZero(Load.Tension, Tolerance));

			TestFalse(
				FString::Printf(TEXT("%s: connection %d must still be intact after solving"),
					Named.Description, Index),
				Structure.GetConnection(Index).HasGiven());

			TotalCarriedUU += FMath::Abs(Force.Z);

			if (Named.Spec.Pieces[Spec.PieceA].bIsGrounded || Named.Spec.Pieces[Spec.PieceB].bIsGrounded)
			{
				GroundReactionUU += FMath::Abs(Force.Z);
			}
		}

		/*
		 * GROUND-REACTION CONSERVATION. Everything held up has to arrive somewhere,
		 * and the only somewhere is the earth, so the joints touching a grounded
		 * piece carry exactly the weight of the supported ungrounded pieces —
		 * no more, and no less.
		 *
		 * This is what pins the cycle and the split cases. The weaker "no joint
		 * carries more than the total" it replaces left 2x of slack on the triangle:
		 * a solver treating every neighbour as a support could report 1333 on all
		 * three joints and pass, because no single number exceeded the total.
		 *
		 * Relative tolerance, because one spec weighs 9.8e8 uu and another splits
		 * across areas twelve orders of magnitude apart.
		 */
		const double ConservationTolerance = FMath::Max(Tolerance, 1.0e-9 * TotalSupportedWeightUU);

		TestTrue(
			FString::Printf(
				TEXT("%s: the structure holds up %f but only %f reaches the ground"),
				Named.Description, TotalSupportedWeightUU, GroundReactionUU),
			FMath::IsNearlyEqual(GroundReactionUU, TotalSupportedWeightUU, ConservationTolerance));

		/*
		 * Out-of-range handles fail closed rather than reading as a healthy,
		 * unloaded, grounded piece.
		 */
		const int32 PastTheEnd = Named.Spec.Pieces.Num();

		TestFalse(
			FString::Printf(TEXT("%s: an unknown piece is not supported"), Named.Description),
			Structure.IsPieceSupported(PastTheEnd));
		TestFalse(
			FString::Printf(TEXT("%s: piece INDEX_NONE is not supported"), Named.Description),
			Structure.IsPieceSupported(INDEX_NONE));

		TestTrue(
			FString::Printf(TEXT("%s: an unknown connection carries nothing"), Named.Description),
			Structure.GetConnectionForce(Named.Spec.Connections.Num()).IsNearlyZero());
		TestTrue(
			FString::Printf(TEXT("%s: connection INDEX_NONE carries nothing"), Named.Description),
			Structure.GetConnectionForce(INDEX_NONE).IsNearlyZero());

		/*
		 * The accessors themselves, which are documented to hand back a placeholder
		 * for a handle they do not know. Never exercised before, and the failure they
		 * guard against is silent: a placeholder that read as a real massless
		 * grounded piece, or as a real zero-area joint, would let a caller walk off
		 * the end of the array and get a plausible answer instead of an obvious one.
		 */
		{
			const FStructurePiece& UnknownPiece = Structure.GetPiece(PastTheEnd);
			const FStructurePiece& NonePiece = Structure.GetPiece(INDEX_NONE);

			TestTrue(
				FString::Printf(TEXT("%s: an unknown piece has no index, mass or ground, got %d / %f / %d"),
					Named.Description, UnknownPiece.Index, UnknownPiece.MassKg,
					UnknownPiece.bIsGrounded ? 1 : 0),
				UnknownPiece.Index == INDEX_NONE
					&& FMath::IsNearlyZero(UnknownPiece.MassKg, Tolerance)
					&& !UnknownPiece.bIsGrounded);

			TestTrue(
				FString::Printf(TEXT("%s: piece INDEX_NONE has no index, mass or ground, got %d / %f / %d"),
					Named.Description, NonePiece.Index, NonePiece.MassKg,
					NonePiece.bIsGrounded ? 1 : 0),
				NonePiece.Index == INDEX_NONE
					&& FMath::IsNearlyZero(NonePiece.MassKg, Tolerance)
					&& !NonePiece.bIsGrounded);

			const FConnection& UnknownJoint = Structure.GetConnection(Named.Spec.Connections.Num());
			const FConnection& NoneJoint = Structure.GetConnection(INDEX_NONE);

			/*
			 * Zero area matters as much as the unset handles: it is what makes the
			 * placeholder read as a FAILED joint rather than an intact one if anybody
			 * ever puts a load through it.
			 */
			TestTrue(
				FString::Printf(TEXT("%s: an unknown connection joins nothing, got %d -> %d over %f cm2"),
					Named.Description, UnknownJoint.PieceA, UnknownJoint.PieceB,
					UnknownJoint.InterfaceAreaSqCm),
				UnknownJoint.PieceA == INDEX_NONE && UnknownJoint.PieceB == INDEX_NONE
					&& FMath::IsNearlyZero(UnknownJoint.InterfaceAreaSqCm, Tolerance)
					&& !UnknownJoint.HasGiven());

			TestTrue(
				FString::Printf(TEXT("%s: connection INDEX_NONE joins nothing, got %d -> %d over %f cm2"),
					Named.Description, NoneJoint.PieceA, NoneJoint.PieceB,
					NoneJoint.InterfaceAreaSqCm),
				NoneJoint.PieceA == INDEX_NONE && NoneJoint.PieceB == INDEX_NONE
					&& FMath::IsNearlyZero(NoneJoint.InterfaceAreaSqCm, Tolerance)
					&& !NoneJoint.HasGiven());
		}

		/*
		 * Nothing supported means nothing carried, and something supported means
		 * something carried. Stops the whole matrix being satisfied by a solver
		 * that returns zero everywhere.
		 */
		TestTrue(
			FString::Printf(TEXT("%s: %f of weight is held up but the joints carry %f in total"),
				Named.Description, TotalSupportedWeightUU, TotalCarriedUU),
			(TotalSupportedWeightUU > Tolerance) == (TotalCarriedUU > Tolerance));
	}

	return true;
}

/**
 * A cycle in the support relation must not strand load SILENTLY.
 *
 * Accumulation orders pieces by Kahn's algorithm over "who rests on me". Pieces in
 * a cycle never become ready, so their joints keep a load of zero and the weight
 * they carry never reaches the earth — while the reachability walk, which has no
 * such ordering problem, happily reports those same pieces as supported. Two
 * accessors on the same object, answering the same solve, contradicting each other
 * with no signal at all.
 *
 * The shape is not exotic. It is a bottom course of three bricks with the ground
 * missing under two of them, which is DESIGN.md §4's shear test with a two-brick
 * gap rather than a one-brick gap — the next scenario on the list, not a curiosity.
 * Head joints are the whole point of that test, and a piece with no bed joint
 * beneath it falls back to ALL its head joints, so neighbours in a course name each
 * other as supports and the cycle appears the moment the gap is two bricks wide.
 *
 * THE DECISION, and it is a decision: pieces whose load the solver cannot route to
 * earth are reported UNSUPPORTED.
 *
 * The alternative was an explicit signal — a return value from SolveLoads, or a
 * "did this solve resolve" query. Unsupported wins on three counts. It needs no new
 * API, so nothing can read the old answer and miss the new flag. It makes the two
 * accessors agree again, which is precisely the defect: IsPieceSupported false and
 * GetConnectionForce zero tell one consistent story, and Structure.h already says a
 * connection with no path to ground reports zero. And it is the fail-closed
 * direction — DESIGN.md §3 says a piece with no path to ground is unsupported and
 * that this is what "it fell" will mean, so a piece the static solver cannot hold
 * up being reported as not held up is the conservative answer rather than a novel
 * one. Terminating rather than looping was always right; being quiet about it was
 * not.
 *
 * NOTE this deliberately does NOT claim the cycle is solved. Dividing load round a
 * loop needs a rule we do not have (CURRENT_STATE.md). It claims only that the
 * stall is visible from outside.
 *
 * Because that answer could reasonably change later, the invariants below are
 * written against what the solver REPORTS rather than against the expectation
 * table, so they keep their force under any resolution:
 *
 *  - Every ungrounded piece the solver calls supported must have at least its own
 *    weight on the joints touching it. Summing over every touching joint rather
 *    than over its support list keeps this free of any transcription of the tier
 *    rule — it is a lower bound whichever joints turn out to be the supports.
 *  - Ground reaction equals the weight of exactly the pieces the solver says it is
 *    holding up. This one ALSO pins a claim that stranding is currently propping up
 *    from underneath: Structure.cpp writes ConnectionForces by ASSIGNMENT rather
 *    than accumulation, justified in a comment on the grounds that two mutually
 *    supporting pieces never both become ready. True today, and nothing pins it —
 *    so seeding the cycle to fix the stall would silently turn that assignment into
 *    an overwrite, with the second piece's share clobbering the first's and the
 *    difference vanishing. Conservation is what notices.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureSupportCycleTest,
	"DestructionGame.Core.Structure.SupportCycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureSupportCycleTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	struct FCycleCase
	{
		const TCHAR* Description;
		FStructureSpec Spec;

		/** Whether each piece is held up, in piece-array order. */
		TArray<bool> ExpectedSupported;
	};

	const TArray<FCycleCase> Cases = {
		/*
		 * THE CONTROL, and it is the minimum repro minus one brick. Two bricks in a
		 * course, the left one grounded: piece 1 has no bed joint, falls back to its
		 * one head joint, and the earth is on the other side of it. No cycle, because
		 * piece 0 is grounded and grounded pieces never enter the ordering at all.
		 *
		 * This must keep working. A fix that answered the cycle by distrusting head
		 * joints, or by calling anything that falls back unsupported, would take this
		 * with it — and this case is the one DESIGN.md §3's worked example rests on.
		 */
		{
			TEXT("a brick held sideways by a grounded neighbour is supported and loads the joint"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false } },
				{ { 0, 1, HeadJointNormal, JointAreaSqCm } }
			},
			{ true, true }
		},

		/*
		 * THE MINIMUM REPRO. Add one more brick to that course:
		 *
		 *   G  —  X  —  Y      pieces 0, 1, 2
		 *   ==                 earth, under G only
		 *
		 * Neither X nor Y has a bed joint, so each falls back to every head joint it
		 * has. X's supports are {G, Y}; Y's supports are {X}. X waits for Y and Y waits
		 * for X, the ready set seeds EMPTY, and not one force is ever written — yet the
		 * reachability walk reports both X and Y supported, because Y does reach the
		 * earth through X. 5333 uu of brick is being held up according to one accessor
		 * and 0 uu arrives at the ground according to the other.
		 */
		{
			TEXT("a two-brick gap strands the course, and the stall is visible from outside"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, false } },
				{
					{ 0, 1, HeadJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm }
				}
			},
			{ true, false, false }
		},

		/*
		 * The same stall at a longer stride, so a fix cannot be a special case for
		 * adjacent pairs: a three-brick gap makes a cycle of three.
		 */
		{
			TEXT("a three-brick gap strands the course the same way"),
			{
				{
					{ BrickMassKg, true }, { BrickMassKg, false },
					{ BrickMassKg, false }, { BrickMassKg, false }
				},
				{
					{ 0, 1, HeadJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 2, 3, HeadJointNormal, JointAreaSqCm }
				}
			},
			{ true, false, false, false }
		},

		/*
		 * THE SECOND CONTROL: a cycle that is NOT stranded and must stay solved.
		 * Pieces 0 and 2 name each other through the head joint, so the support
		 * relation genuinely has a two-cycle in it — but piece 0 is grounded and never
		 * enters the ordering, so piece 2 becomes ready immediately and the whole
		 * structure resolves. "Contains a cycle" is therefore not the same as "cannot
		 * be ordered", and a fix that rejected the first would break this.
		 */
		{
			TEXT("a cycle through a grounded piece still resolves"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, false } },
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 0, 2, HeadJointNormal, JointAreaSqCm }
				}
			},
			{ true, true, true }
		},
	};

	constexpr double Tolerance = 1.0e-6;

	for (const FCycleCase& Case : Cases)
	{
		FStructure Structure;
		BuildStructure(Structure, Case.Spec, Unbreakable);

		TestTrue(
			FString::Printf(TEXT("%s: expected %d pieces, got %d"),
				Case.Description, Case.Spec.Pieces.Num(), Structure.NumPieces()),
			Structure.NumPieces() == Case.Spec.Pieces.Num());

		TestTrue(
			FString::Printf(TEXT("%s: expected %d connections, got %d"),
				Case.Description, Case.Spec.Connections.Num(), Structure.NumConnections()),
			Structure.NumConnections() == Case.Spec.Connections.Num());

		TestTrue(
			FString::Printf(TEXT("%s: expected a row for each of %d pieces, got %d rows"),
				Case.Description, Structure.NumPieces(), Case.ExpectedSupported.Num()),
			Case.ExpectedSupported.Num() == Structure.NumPieces());

		Structure.SolveLoads();

		/*
		 * THE DECISION, asserted directly: a piece the solver cannot route to earth is
		 * not held up.
		 */
		for (int32 Index = 0; Index < Case.ExpectedSupported.Num(); ++Index)
		{
			TestTrue(
				FString::Printf(TEXT("%s: piece %d support, expected %d, got %d"),
					Case.Description, Index,
					Case.ExpectedSupported[Index] ? 1 : 0,
					Structure.IsPieceSupported(Index) ? 1 : 0),
				Structure.IsPieceSupported(Index) == Case.ExpectedSupported[Index]);
		}

		/*
		 * From here on, everything is measured against what the solver REPORTS, so it
		 * survives the decision above being revisited.
		 */
		double ReportedSupportedWeightUU = 0.0;

		for (int32 PieceIndex = 0; PieceIndex < Case.Spec.Pieces.Num(); ++PieceIndex)
		{
			if (!Structure.IsPieceSupported(PieceIndex) || Case.Spec.Pieces[PieceIndex].bIsGrounded)
			{
				continue;
			}

			const double OwnWeightUU = WeightOf(Case.Spec.Pieces[PieceIndex].MassKg);
			ReportedSupportedWeightUU += OwnWeightUU;

			/*
			 * Every joint that touches this piece, not just the ones the tier rule
			 * picks as its supports. A piece pushes its weight out through some subset
			 * of what touches it, so the total over all of them is a lower bound
			 * whichever subset that turns out to be — and deriving it needs no copy of
			 * the tier rule to be kept in step with production.
			 */
			double TouchingUU = 0.0;
			for (int32 Index = 0; Index < Case.Spec.Connections.Num(); ++Index)
			{
				const FConnectionSpec& Spec = Case.Spec.Connections[Index];
				if (Spec.PieceA == PieceIndex || Spec.PieceB == PieceIndex)
				{
					TouchingUU += FMath::Abs(Structure.GetConnectionForce(Index).Z);
				}
			}

			TestTrue(
				FString::Printf(
					TEXT("%s: piece %d is reported held up but only %f of its %f weight is on any joint touching it"),
					Case.Description, PieceIndex, TouchingUU, OwnWeightUU),
				TouchingUU + Tolerance >= OwnWeightUU);
		}

		double GroundReactionUU = 0.0;
		for (int32 Index = 0; Index < Case.Spec.Connections.Num(); ++Index)
		{
			const FConnectionSpec& Spec = Case.Spec.Connections[Index];
			const FVector Force = Structure.GetConnectionForce(Index);

			TestTrue(
				FString::Printf(TEXT("%s: connection %d load must be finite, got (%f, %f, %f)"),
					Case.Description, Index, Force.X, Force.Y, Force.Z),
				!Force.ContainsNaN() && FMath::IsFinite(Force.Z));

			TestFalse(
				FString::Printf(TEXT("%s: connection %d must still be intact after solving"),
					Case.Description, Index),
				Structure.GetConnection(Index).HasGiven());

			if (Case.Spec.Pieces[Spec.PieceA].bIsGrounded || Case.Spec.Pieces[Spec.PieceB].bIsGrounded)
			{
				GroundReactionUU += FMath::Abs(Force.Z);
			}
		}

		/*
		 * Ground reaction against the solver's OWN claim about what it is holding up.
		 * Short means load was stranded; long means it was double-counted; and an
		 * assignment that became an overwrite shows up here as short by exactly the
		 * clobbered share.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: the solver reports holding up %f but only %f reaches the ground"),
				Case.Description, ReportedSupportedWeightUU, GroundReactionUU),
			FMath::IsNearlyEqual(GroundReactionUU, ReportedSupportedWeightUU,
				FMath::Max(Tolerance, 1.0e-9 * ReportedSupportedWeightUU)));
	}

	return true;
}

/**
 * STRANDING DOES NOT PROPAGATE DOWNWARD. A brick resting on the earth is standing up
 * no matter what is happening above it.
 *
 * DESIGN.md §3: "A piece is unsupported when it genuinely has no load path to the
 * ground. That is what 'it fell' means, and it is a statement about the structure,
 * never about the solver." Un-orderability is a solver artefact. Load that cannot be
 * routed is reported as falling "and only for the pieces actually caught in it";
 * pieces BENEATH such a knot keep their support and carry everything except the
 * unroutable contribution.
 *
 * THE DEFECT THIS PINS. The Kahn ordering runs TOP-DOWN — a piece is ready only once
 * everything resting on it has been processed — so a piece comes out unordered when a
 * knot sits anywhere ABOVE it, not only when it is in one. Stranding every unordered
 * piece is therefore a strictly larger set than "pieces in a knot": it walks down to
 * the first grounded piece, and the next pass walks it back up through everything
 * resting on what it just stranded. One unroutable pair takes down four of five
 * pieces.
 *
 * WHY NO EXISTING TEST SEES IT, and what these cases do differently. Over-stranding
 * PRESERVES CONSERVATION EXACTLY, because the piece's weight leaves both sides of the
 * equation at once — a 4000-case fuzz over random 2-5 piece graphs found zero
 * conservation violations while finding 14 wrongly-falling pieces. Every invariant in
 * SupportCycle and DegenerateInputs is measured against what the solver REPORTS, which
 * is deliberate and is blind here. So the missing control is an ungrounded piece
 * BED-JOINTED TO THE GROUND and head-jointed to an unroutable pair, with its
 * supportedness and its bed joint's load asserted DIRECTLY.
 *
 * The knot must also not shed its weight downward. Every case states the load beneath
 * it exactly, so an implementation that "resolved" the knot by pushing X and Y's
 * weight into the pier reads 4 or 5 bricks where the table says 2.
 *
 * The controls for the other direction live in SupportCycle: a brick held sideways by
 * a grounded neighbour, and a genuine cycle through a grounded piece, must both stay
 * solved. Narrowing the stranded set must not widen it anywhere else.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureStrandingIsLocalTest,
	"DestructionGame.Core.Structure.StrandingIsLocal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureStrandingIsLocalTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	const TArray<FSolveCase> Cases = {
		/*
		 * THE REPRO, minimal.
		 *
		 *         Z  —  X  —  Y      pieces 1, 2, 3, joined sideways
		 *         |                  bed joint, Z on the pier
		 *      [ pier ]              piece 0, on the earth
		 *      =======
		 *
		 * X and Y have no bed joint, so each falls back to every head joint it has:
		 * X's supports are {Z, Y} and Y's are {X}. That pair is the unroutable knot,
		 * and it is the WHOLE knot — Z is not in it. Z has a bed joint onto a grounded
		 * pier and is standing on the earth.
		 *
		 * Because X names Z as one of its supports, Z's pending-loader count never
		 * reaches zero, so Z never becomes orderable. Stranding on un-orderability
		 * therefore strands Z, and the next pass strands anything resting on Z. That is
		 * the solver's ordering problem being reported as the structure falling down.
		 *
		 * The pier joint carries Z's own weight and nothing else: X and Y are falling,
		 * and a falling piece contributes nothing to what is beneath it.
		 */
		{
			TEXT("a piece bed-jointed to the ground is supported however unroutable the course above it"),
			{
				{
					{ BrickMassKg, true },  // 0: pier, on the earth
					{ BrickMassKg, false }, // 1: Z, resting squarely on the pier
					{ BrickMassKg, false }, // 2: X, in the knot
					{ BrickMassKg, false }  // 3: Y, in the knot
				},
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 2, 3, HeadJointNormal, JointAreaSqCm }
				}
			},
			{
				{ -BrickWeightUU, EJointKind::Bed }, // pier <- Z: one brick, not zero
				{ 0.0, EJointKind::Head },           // Z -> X: X is falling, so it bears nothing
				{ 0.0, EJointKind::Head }            // X -> Y: nothing here is held up at all
			},
			{ true, true, false, false }
		},

		/*
		 * THE SAME KNOT WITH A BRICK ON THE PIECE BENEATH IT, which is where the
		 * cascade shows: today one unroutable pair takes down four of these five.
		 *
		 *               B                piece 4, resting on Z
		 *               |
		 *         Z  —  X  —  Y          pieces 1, 2, 3
		 *         |
		 *      [ pier ]                  piece 0
		 *      =======
		 *
		 * B is held up by Z, Z reaches the earth, so B stands and the pier joint carries
		 * TWO bricks. Not three and not four: X and Y are falling and hand nothing down.
		 */
		{
			TEXT("a brick resting on a grounded-through piece stands, and its weight reaches the earth"),
			{
				{
					{ BrickMassKg, true },  // 0: pier
					{ BrickMassKg, false }, // 1: Z
					{ BrickMassKg, false }, // 2: X, in the knot
					{ BrickMassKg, false }, // 3: Y, in the knot
					{ BrickMassKg, false }  // 4: B, resting on Z
				},
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 2, 3, HeadJointNormal, JointAreaSqCm },
					{ 1, 4, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				{ -2.0 * BrickWeightUU, EJointKind::Bed }, // pier <- Z: Z and B
				{ 0.0, EJointKind::Head },                 // Z -> X
				{ 0.0, EJointKind::Head },                 // X -> Y
				{ -BrickWeightUU, EJointKind::Bed }        // Z <- B
			},
			{ true, true, false, false, true }
		},

		/*
		 * TWO COURSES BENEATH THE KNOT, so a fix cannot be a special case for "the piece
		 * directly under it". The reviewer's description of the defect is that stranding
		 * walks down to the first GROUNDED piece, which is two joints away here.
		 *
		 *        Zhigh — X — Y      pieces 2, 3, 4
		 *          |
		 *        Zlow               piece 1
		 *          |
		 *      [ pier ]             piece 0
		 *      =======
		 *
		 * Neither Zlow nor Zhigh is in the knot. The lower joint carries both of them,
		 * the upper carries Zhigh alone.
		 */
		{
			TEXT("stranding does not walk down two courses to the earth"),
			{
				{
					{ BrickMassKg, true },  // 0: pier
					{ BrickMassKg, false }, // 1: Zlow
					{ BrickMassKg, false }, // 2: Zhigh
					{ BrickMassKg, false }, // 3: X, in the knot
					{ BrickMassKg, false }  // 4: Y, in the knot
				},
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, BedJointNormal, JointAreaSqCm },
					{ 2, 3, HeadJointNormal, JointAreaSqCm },
					{ 3, 4, HeadJointNormal, JointAreaSqCm }
				}
			},
			{
				{ -2.0 * BrickWeightUU, EJointKind::Bed }, // pier <- Zlow: two bricks
				{ -BrickWeightUU, EJointKind::Bed },       // Zlow <- Zhigh: one brick
				{ 0.0, EJointKind::Head },                 // Zhigh -> X
				{ 0.0, EJointKind::Head }                  // X -> Y
			},
			{ true, true, true, false, false }
		},
	};

	for (const FSolveCase& Case : Cases)
	{
		CheckSolveCase(*this, Case);
	}

	return true;
}

/**
 * STRANDING DOES PROPAGATE UPWARD. A piece whose only support is falling is falling
 * too, and its joint carries nothing.
 *
 * The other half of the rule, and the one thing that genuinely needs the solve to
 * iterate: stranding a knot changes who reaches the ground, which changes who may take
 * a share, which can strand a piece that rested only on what was just stranded. Every
 * other case in the suite settles in one pass, and it was verified that a single-pass
 * solver — mark the stranded set unsupported and stop — reproduces every piece flag and
 * every connection force on all 26 structures the suite currently solves, so the loop
 * could be deleted and the suite would stay green. That is branching control flow with
 * nothing behind it.
 *
 * THE SHAPE THAT DISCRIMINATES, and the numbers that separate the two:
 *
 *                       B          piece 3, resting on Y through a bed joint
 *                       |
 *      [ G ]  —  X  —   Y          pieces 0, 1, 2, joined sideways
 *      =====
 *
 *   ITERATING     supported = [T,F,F,F]   forces = [0, 0, 0]
 *   SINGLE-PASS   supported = [T,F,F,T]   forces = [0, 0, -2667.2]
 *
 * The single-pass answer is self-contradictory in exactly the way the whole stranding
 * rule exists to prevent: it calls B held up while writing B's weight onto a joint
 * whose far end it simultaneously calls falling. B is ordered FIRST — it is a leaf, so
 * it is the only piece the Kahn pass can seed with — which is why the case has to have
 * something above the knot to see this at all.
 *
 * BOTH CASES NOW DISCRIMINATE THE LOOP, and both pass against the solver as built.
 * The numbers above are case 1's: delete the fixpoint and it reports B supported with
 * -2667.2 on the Y-B joint whose far end it simultaneously calls falling. Case 2 adds
 * the second claim — Z, which is beneath the knot rather than in it, must KEEP its
 * support and its weight on the pier joint while B loses both — so it fails a
 * single-pass solver and an over-eager stranding rule in opposite directions.
 *
 * (An earlier revision of this comment said case 1 merely passed and was not a real
 * test. That was true before the stranding rule was narrowed to the knot; it is not
 * true now, and CURRENT_STATE.md points maintainers here to decide whether the loop
 * can go. It cannot: run these two with the loop removed before believing otherwise.)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureStrandingPropagatesUpwardTest,
	"DestructionGame.Core.Structure.StrandingPropagatesUpward",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureStrandingPropagatesUpwardTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	const TArray<FSolveCase> Cases = {
		/*
		 * Nothing beneath the knot: G is grounded, so the whole course above it goes.
		 * B rests on Y, Y is in the knot, and B has no other support — so B falls, and
		 * the joint holding it must report zero rather than the weight of a brick that
		 * is on its way down.
		 */
		{
			TEXT("a brick resting on a stranded piece is itself falling and its joint carries nothing"),
			{
				{
					{ BrickMassKg, true },  // 0: G, on the earth
					{ BrickMassKg, false }, // 1: X, in the knot
					{ BrickMassKg, false }, // 2: Y, in the knot
					{ BrickMassKg, false }  // 3: B, resting on Y
				},
				{
					{ 0, 1, HeadJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 2, 3, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				{ 0.0, EJointKind::Head }, // G -> X
				{ 0.0, EJointKind::Head }, // X -> Y
				{ 0.0, EJointKind::Bed }   // Y <- B: B is falling with Y, not resting on it
			},
			{ true, false, false, false }
		},

		/*
		 * BOTH RULES AT ONCE, which is the only case that states them as separate
		 * claims. Z is beneath the knot and must keep its support; B is above it and
		 * must lose its own.
		 *
		 *                         B      piece 4, resting on Y
		 *                         |
		 *         Z  —  X  —      Y      pieces 1, 2, 3
		 *         |
		 *      [ pier ]                  piece 0
		 *      =======
		 *
		 * Downward: the pier joint carries Z's one brick. Upward: B is held up by
		 * nothing but Y, so it falls and the Y-B joint is unloaded. Reporting B
		 * supported here would be worse than in the case above, because the structure
		 * really does have a standing part for it to be confused with.
		 */
		{
			TEXT("stranding travels up to what rests on the knot and not down to what carries it"),
			{
				{
					{ BrickMassKg, true },  // 0: pier
					{ BrickMassKg, false }, // 1: Z, bed-jointed to the pier
					{ BrickMassKg, false }, // 2: X, in the knot
					{ BrickMassKg, false }, // 3: Y, in the knot
					{ BrickMassKg, false }  // 4: B, resting on Y
				},
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 2, 3, HeadJointNormal, JointAreaSqCm },
					{ 3, 4, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				{ -BrickWeightUU, EJointKind::Bed }, // pier <- Z: Z alone, and Z alone stands
				{ 0.0, EJointKind::Head },           // Z -> X
				{ 0.0, EJointKind::Head },           // X -> Y
				{ 0.0, EJointKind::Bed }             // Y <- B
			},
			{ true, true, false, false, false }
		},
	};

	for (const FSolveCase& Case : Cases)
	{
		CheckSolveCase(*this, Case);
	}

	return true;
}

/**
 * WHY a piece has no support, and not merely whether it has none.
 *
 * IsPieceSupported answers "whether", never "why", and two completely different things
 * produce the same false:
 *
 *   - REAL PHYSICS. Nothing is holding the piece up. Its supports are gone, or they are
 *     falling themselves.
 *   - A SOLVER LIMITATION. The piece is in an unroutable knot — two bricks that have both
 *     lost their bed joints fall back onto each other's head joints, so each is the
 *     other's support, and there is no rule here for dividing load round a loop. The
 *     solver plays safe and calls them falling. Documented, deliberate, conservative
 *     (DESIGN.md §3) — and not physics.
 *
 * WHY THAT MATTERS ENOUGH TO ADD AN ACCESSOR FOR IT. DESIGN.md §4's headline collapse
 * test is "pull bricks until it topples, confirm it falls at the predicted number", and
 * THAT ASSERTION IS IDENTICAL IN BOTH CASES. It would pass, stay green through a retune
 * of every strength in the library, and be calibrated against the number of removals that
 * happens to make a cycle rather than against the number that overloads a joint. Three
 * bricks out of the bottom course of a five-brick wall is enough to produce one, which is
 * roughly what a player does. The collapse test needs a precondition it cannot currently
 * express — no piece was STRANDED when it fell — and this is that seam.
 *
 * STRANDED IS ONLY FOR PIECES IN THE KNOT, and the distinction is the reason the two
 * upward-propagation rows are in this table. A piece resting only on a knot comes out
 * FALLING: it is not itself unroutable, it has simply lost the one thing carrying it, and
 * that is ordinary physics. Production already computes exactly this set — the fixpoint
 * strands on "does my load come back round to me", never on un-orderability — so the
 * distinction falls out rather than being invented here.
 *
 * THE TOPOLOGIES ARE NOT NEW. Every case mirrors a fixture that already exists in this
 * file, and its expectation row must agree piece for piece with what that test says about
 * IsPieceSupported — CheckSupportAgreesWithReason asserts that agreement directly rather
 * than by transcription. What is new is only the reason column.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructurePieceSupportReasonTest,
	"DestructionGame.Core.Structure.PieceSupportReason",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructurePieceSupportReasonTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	struct FSupportReasonCase
	{
		const TCHAR* Description;
		FStructureSpec Spec;

		/** Why each piece is or is not held up, in piece-array order. */
		TArray<EPieceSupport> ExpectedSupport;
	};

	const TArray<FSupportReasonCase> Cases = {
		/*
		 * SupportCycle's first control. Piece 1 has no bed joint and falls back to its one
		 * head joint, whose far end is the earth. Nothing here is in a knot, so the
		 * fallback must read as ordinary support — a reason column that called every
		 * head-jointed piece stranded would pass every other row in this table.
		 */
		{
			TEXT("a brick held sideways by a grounded neighbour is SUPPORTED, not stranded"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false } },
				{ { 0, 1, HeadJointNormal, JointAreaSqCm } }
			},
			{ EPieceSupport::Grounded, EPieceSupport::Supported }
		},

		/*
		 * SupportCycle's minimum repro, and the case this whole accessor exists for:
		 *
		 *   G  —  X  —  Y      pieces 0, 1, 2
		 *   ==                 earth, under G only
		 *
		 * X's supports are {G, Y}, Y's are {X}, so each is ultimately its own support and
		 * both are IN the knot. IsPieceSupported reports them falling and cannot say that
		 * the reason is the solver rather than the structure.
		 */
		{
			TEXT("both bricks over a two-brick gap are STRANDED — they are in the knot"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, false } },
				{
					{ 0, 1, HeadJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm }
				}
			},
			{ EPieceSupport::Grounded, EPieceSupport::Stranded, EPieceSupport::Stranded }
		},

		/*
		 * The same stall at a longer stride, so the reason cannot be a special case for
		 * adjacent pairs: all three pieces of a three-cycle are in it.
		 */
		{
			TEXT("all three bricks over a three-brick gap are STRANDED"),
			{
				{
					{ BrickMassKg, true }, { BrickMassKg, false },
					{ BrickMassKg, false }, { BrickMassKg, false }
				},
				{
					{ 0, 1, HeadJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 2, 3, HeadJointNormal, JointAreaSqCm }
				}
			},
			{
				EPieceSupport::Grounded, EPieceSupport::Stranded,
				EPieceSupport::Stranded, EPieceSupport::Stranded
			}
		},

		/*
		 * SupportCycle's second control: the support relation genuinely contains a cycle,
		 * pieces 0 and 2 naming each other, but piece 0 is grounded and never enters the
		 * ordering, so everything resolves. "Contains a cycle" is not "is stranded", and a
		 * reason derived from cycle detection alone rather than from the solve would call
		 * pieces 0 and 2 stranded here.
		 */
		{
			TEXT("a cycle through a grounded piece resolves, so nothing in it is stranded"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, false } },
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 0, 2, HeadJointNormal, JointAreaSqCm }
				}
			},
			{ EPieceSupport::Grounded, EPieceSupport::Supported, EPieceSupport::Supported }
		},

		/*
		 * StrandingIsLocal's repro. Z is BENEATH the knot rather than in it, and keeps both
		 * its support and its reason: the knot above it is X and Y and nothing else.
		 *
		 *         Z  —  X  —  Y      pieces 1, 2, 3
		 *         |
		 *      [ pier ]              piece 0
		 *      =======
		 */
		{
			TEXT("a piece bed-jointed to the ground beneath a knot is SUPPORTED; only the knot is stranded"),
			{
				{
					{ BrickMassKg, true },  // 0: pier, on the earth
					{ BrickMassKg, false }, // 1: Z, resting squarely on the pier
					{ BrickMassKg, false }, // 2: X, in the knot
					{ BrickMassKg, false }  // 3: Y, in the knot
				},
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 2, 3, HeadJointNormal, JointAreaSqCm }
				}
			},
			{
				EPieceSupport::Grounded, EPieceSupport::Supported,
				EPieceSupport::Stranded, EPieceSupport::Stranded
			}
		},

		/*
		 * The same knot with a brick resting on the piece BENEATH it. B is held up by Z,
		 * which reaches the earth, so B is plainly Supported while X and Y are stranded two
		 * joints away — the reason must be per piece, not per structure.
		 */
		{
			TEXT("a brick on the standing part is SUPPORTED while the knot beside it is stranded"),
			{
				{
					{ BrickMassKg, true },  // 0: pier
					{ BrickMassKg, false }, // 1: Z
					{ BrickMassKg, false }, // 2: X, in the knot
					{ BrickMassKg, false }, // 3: Y, in the knot
					{ BrickMassKg, false }  // 4: B, resting on Z
				},
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 2, 3, HeadJointNormal, JointAreaSqCm },
					{ 1, 4, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				EPieceSupport::Grounded, EPieceSupport::Supported, EPieceSupport::Stranded,
				EPieceSupport::Stranded, EPieceSupport::Supported
			}
		},

		/*
		 * Two courses beneath the knot, so a reason that walked down from the knot to the
		 * first grounded piece — which is what stranding on un-orderability does — would
		 * report Zlow and Zhigh stranded here. They are two joints apart and both standing.
		 */
		{
			TEXT("two courses beneath a knot are both SUPPORTED"),
			{
				{
					{ BrickMassKg, true },  // 0: pier
					{ BrickMassKg, false }, // 1: Zlow
					{ BrickMassKg, false }, // 2: Zhigh
					{ BrickMassKg, false }, // 3: X, in the knot
					{ BrickMassKg, false }  // 4: Y, in the knot
				},
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, BedJointNormal, JointAreaSqCm },
					{ 2, 3, HeadJointNormal, JointAreaSqCm },
					{ 3, 4, HeadJointNormal, JointAreaSqCm }
				}
			},
			{
				EPieceSupport::Grounded, EPieceSupport::Supported, EPieceSupport::Supported,
				EPieceSupport::Stranded, EPieceSupport::Stranded
			}
		},

		/*
		 * STRANDED VERSUS FALLING, which is the distinction IsPieceSupported cannot make and
		 * the one that costs nothing extra to expose. StrandingPropagatesUpward's first case:
		 *
		 *                       B          piece 3, resting on Y through a bed joint
		 *                       |
		 *      [ G ]  —  X  —   Y          pieces 0, 1, 2
		 *      =====
		 *
		 * X and Y are in the knot. B is NOT — its load walks down through Y, X and G to the
		 * earth without ever coming back round to B — it has simply lost the only thing
		 * holding it up, which is physics. IsPieceSupported says false for all three; only
		 * this accessor can say that two of them are the solver's answer and one is the
		 * structure's.
		 */
		{
			TEXT("a brick resting only on a knot is FALLING, not stranded — it is not in the knot"),
			{
				{
					{ BrickMassKg, true },  // 0: G, on the earth
					{ BrickMassKg, false }, // 1: X, in the knot
					{ BrickMassKg, false }, // 2: Y, in the knot
					{ BrickMassKg, false }  // 3: B, resting on Y
				},
				{
					{ 0, 1, HeadJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 2, 3, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				EPieceSupport::Grounded, EPieceSupport::Stranded,
				EPieceSupport::Stranded, EPieceSupport::Falling
			}
		},

		/*
		 * All four states in one structure, which is the row that stops any two of them
		 * being conflated. The pier is Grounded, Z beneath the knot is Supported, X and Y
		 * are Stranded, and B on top of the knot is Falling.
		 */
		{
			TEXT("one structure showing all four states at once"),
			{
				{
					{ BrickMassKg, true },  // 0: pier
					{ BrickMassKg, false }, // 1: Z, bed-jointed to the pier
					{ BrickMassKg, false }, // 2: X, in the knot
					{ BrickMassKg, false }, // 3: Y, in the knot
					{ BrickMassKg, false }  // 4: B, resting on Y
				},
				{
					{ 0, 1, BedJointNormal, JointAreaSqCm },
					{ 1, 2, HeadJointNormal, JointAreaSqCm },
					{ 2, 3, HeadJointNormal, JointAreaSqCm },
					{ 3, 4, BedJointNormal, JointAreaSqCm }
				}
			},
			{
				EPieceSupport::Grounded, EPieceSupport::Supported, EPieceSupport::Stranded,
				EPieceSupport::Stranded, EPieceSupport::Falling
			}
		},
	};

	/*
	 * A floor on the table itself, so a case list that lost its only Falling row — or was
	 * trimmed to the shapes that happen to pass — fails rather than passing in silence.
	 * Cheap, and it is the same reason the fuzzes floor their generators.
	 */
	TMap<EPieceSupport, int32> StatesExpected;

	for (const FSupportReasonCase& Case : Cases)
	{
		FStructure Structure;
		BuildStructure(Structure, Case.Spec, Unbreakable);

		TestTrue(
			FString::Printf(TEXT("%s: expected %d pieces, got %d"),
				Case.Description, Case.Spec.Pieces.Num(), Structure.NumPieces()),
			Structure.NumPieces() == Case.Spec.Pieces.Num());

		TestTrue(
			FString::Printf(TEXT("%s: expected %d connections, got %d"),
				Case.Description, Case.Spec.Connections.Num(), Structure.NumConnections()),
			Structure.NumConnections() == Case.Spec.Connections.Num());

		TestTrue(
			FString::Printf(TEXT("%s: expected a row for each of %d pieces, got %d rows"),
				Case.Description, Structure.NumPieces(), Case.ExpectedSupport.Num()),
			Case.ExpectedSupport.Num() == Structure.NumPieces());

		Structure.SolveLoads();

		for (int32 Index = 0; Index < Case.ExpectedSupport.Num(); ++Index)
		{
			++StatesExpected.FindOrAdd(Case.ExpectedSupport[Index]);

			TestTrue(
				FString::Printf(TEXT("%s: piece %d should be %s, got %s"),
					Case.Description, Index,
					NameOfSupport(Case.ExpectedSupport[Index]),
					NameOfSupport(Structure.GetPieceSupport(Index))),
				Structure.GetPieceSupport(Index) == Case.ExpectedSupport[Index]);

			CheckSupportAgreesWithReason(*this, Case.Description, Structure, Index);
		}

		/*
		 * A GROUNDED PIECE IS NEVER MERELY SUPPORTED, and the converse: nothing that is not
		 * resting on the earth may read Grounded. Asserted against the spec rather than
		 * against the expectation column, so a row that got the two the wrong way round is
		 * caught by the fixture instead of agreeing with itself.
		 */
		for (int32 Index = 0; Index < Case.Spec.Pieces.Num(); ++Index)
		{
			TestTrue(
				FString::Printf(TEXT("%s: piece %d is grounded %d but reads %s"),
					Case.Description, Index,
					Case.Spec.Pieces[Index].bIsGrounded ? 1 : 0,
					NameOfSupport(Structure.GetPieceSupport(Index))),
				(Structure.GetPieceSupport(Index) == EPieceSupport::Grounded)
					== Case.Spec.Pieces[Index].bIsGrounded);
		}
	}

	const TArray<EPieceSupport> AllStates = {
		EPieceSupport::Falling, EPieceSupport::Grounded,
		EPieceSupport::Supported, EPieceSupport::Stranded
	};

	for (const EPieceSupport State : AllStates)
	{
		const int32* Count = StatesExpected.Find(State);

		TestTrue(
			FString::Printf(TEXT("the table must expect %s somewhere, and expects it %d times"),
				NameOfSupport(State), Count != nullptr ? *Count : 0),
			Count != nullptr && *Count > 0);
	}

	return true;
}

/**
 * THE REASON HAS THE SAME SCOPE AS THE ANSWER IT EXPLAINS, and fails closed everywhere
 * else.
 *
 * GetPieceSupport reads solver output, exactly as IsPieceSupported does, so it inherits
 * that contract rather than inventing a second one. Structure.RemovedPieceSupportNeedsASolve
 * pins the three rows for the boolean; these are the same three rows for the reason, and
 * they must not disagree with it for one instant:
 *
 *     never solved      Falling, for every handle — there is no answer yet
 *     removed           the LAST SOLVE'S answer, unchanged, until the next solve
 *     after that solve  Falling, and a removed GROUNDED piece is no longer earth
 *
 * NO FIFTH ENUMERATOR FOR "REMOVED", and this is the decision the middle row forces. A
 * removed piece is not grounded, supported, falling or stranded — it is not a piece — so a
 * Removed state looks obviously right. It cannot be had: IsPieceRemoved answers
 * immediately and this accessor cannot, so a Removed value would have to be produced
 * before the re-solve, at which point it contradicts a stale Supported coming out of
 * IsPieceSupported about the same piece in the same instant. That is precisely the
 * two-accessors-one-solve defect the stranding rule exists to close, and reintroducing it
 * on the accessor whose job is to EXPLAIN the other would be worse than leaving it. A
 * removed piece therefore folds into Falling — nothing is holding it up, which is true —
 * and IsPieceRemoved stays the accessor for whether it is a piece at all.
 *
 * AN OUT-OF-RANGE HANDLE IS FALLING for the same reason IsPieceSupported answers false and
 * IsPieceRemoved answers true: it is the fail-closed direction, and the accessors either
 * side of it already point that way. Stranded would be worse than useless — it is a
 * positive claim that the solver hit a knot, about a structure that does not exist.
 *
 * A MATRIX RATHER THAN A CASE PER SHAPE, because the property is that NONE of these
 * produce a plausible-looking wrong answer. The failure mode this guards is not a crash:
 * it is an accessor that reads Supported or Grounded for something that is not there,
 * which downstream is indistinguishable from a piece that is genuinely fine.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructurePieceSupportDegenerateInputTest,
	"DestructionGame.Core.Structure.PieceSupportDegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructurePieceSupportDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	/** A grounded pad with a brick resting on it: the smallest structure that holds. */
	const FStructureSpec PadAndBrick = {
		{ { BrickMassKg, true }, { BrickMassKg, false } },
		{ { 0, 1, BedJointNormal, JointAreaSqCm } }
	};

	/*
	 * BEFORE ANY SOLVE THERE IS NO ANSWER, and Falling is the fail-closed one. The state
	 * array is sized by a solve rather than by AddPiece, so an early caller takes the same
	 * range check an unknown handle does — and is told nothing is held up, rather than being
	 * handed a zero-initialised default that reads as resting on the earth.
	 */
	{
		FStructure Structure;
		BuildStructure(Structure, PadAndBrick, Unbreakable);

		for (int32 Index = 0; Index < Structure.NumPieces(); ++Index)
		{
			TestTrue(
				FString::Printf(TEXT("before any solve piece %d has no reason yet and reads %s"),
					Index, NameOfSupport(Structure.GetPieceSupport(Index))),
				Structure.GetPieceSupport(Index) == EPieceSupport::Falling);

			CheckSupportAgreesWithReason(*this, TEXT("before any solve"), Structure, Index);
		}
	}

	/*
	 * OUT-OF-RANGE HANDLES, against a solved structure that is standing — so an accessor
	 * that ran off the end of a populated array would have real Grounded and Supported
	 * entries lying next to it to pick up.
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
			{ TEXT("one past the last piece"), 2 },
			{ TEXT("far past the last piece"), 4096 },
		};

		FStructure Structure;
		BuildStructure(Structure, PadAndBrick, Unbreakable);
		Structure.SolveLoads();

		TestTrue(TEXT("the fixture must actually be standing, or the bad handles prove nothing"),
			Structure.GetPieceSupport(0) == EPieceSupport::Grounded
				&& Structure.GetPieceSupport(1) == EPieceSupport::Supported);

		for (const FBadHandleCase& Case : BadHandles)
		{
			TestTrue(
				FString::Printf(TEXT("%s names no piece, so nothing is holding it up, got %s"),
					Case.Description, NameOfSupport(Structure.GetPieceSupport(Case.Handle))),
				Structure.GetPieceSupport(Case.Handle) == EPieceSupport::Falling);

			CheckSupportAgreesWithReason(*this, Case.Description, Structure, Case.Handle);
		}
	}

	/*
	 * REMOVING THE BRICK. The reason for the piece that has gone does not move until
	 * something re-solves — and then it is Falling, for the same reason an unknown handle is:
	 * it is not a piece any more, so nothing is holding it up.
	 */
	{
		FStructure Structure;
		BuildStructure(Structure, PadAndBrick, Unbreakable);
		Structure.SolveLoads();

		TestTrue(
			FString::Printf(TEXT("the brick starts out Supported, got %s"),
				NameOfSupport(Structure.GetPieceSupport(1))),
			Structure.GetPieceSupport(1) == EPieceSupport::Supported);

		TestTrue(TEXT("removing the brick should report that it removed a live piece"),
			Structure.RemovePiece(1));

		/*
		 * THE STALE ROW, and the one that decides against a fifth enumerator. The piece is
		 * provably gone — IsPieceRemoved says so immediately — and the reason still reads the
		 * last solve's Supported, in lockstep with IsPieceSupported still reading true.
		 */
		TestTrue(TEXT("the brick reads as removed IMMEDIATELY — that accessor needs no solve"),
			Structure.IsPieceRemoved(1));

		TestTrue(
			FString::Printf(
				TEXT("between a removal and the next solve the reason is the LAST SOLVE'S and still reads %s"),
				NameOfSupport(Structure.GetPieceSupport(1))),
			Structure.GetPieceSupport(1) == EPieceSupport::Supported);

		CheckSupportAgreesWithReason(*this, TEXT("a removed brick before the re-solve"), Structure, 1);

		Structure.SolveLoads();

		TestTrue(
			FString::Printf(TEXT("after the next solve the removed brick is Falling, got %s"),
				NameOfSupport(Structure.GetPieceSupport(1))),
			Structure.GetPieceSupport(1) == EPieceSupport::Falling);

		TestTrue(
			FString::Printf(TEXT("and the pad it stood on is still resting on the earth, got %s"),
				NameOfSupport(Structure.GetPieceSupport(0))),
			Structure.GetPieceSupport(0) == EPieceSupport::Grounded);

		CheckSupportAgreesWithReason(*this, TEXT("a removed brick after the re-solve"), Structure, 1);
		CheckSupportAgreesWithReason(*this, TEXT("the pad under a removed brick"), Structure, 0);
	}

	/*
	 * REMOVING THE GROUND, which is the removal with the furthest reach: a grounded piece
	 * seeds the reachability walk on its own account rather than through any joint, so a
	 * removed one that still read Grounded would be claiming an earth it is not touching —
	 * and unlike a bare false, that reason states the claim out loud.
	 */
	{
		FStructure Structure;
		BuildStructure(Structure, PadAndBrick, Unbreakable);
		Structure.SolveLoads();

		TestTrue(TEXT("removing the pad should report that it removed a live piece"),
			Structure.RemovePiece(0));

		TestTrue(
			FString::Printf(TEXT("the removed pad still reads %s and the brick %s, because nothing has re-solved"),
				NameOfSupport(Structure.GetPieceSupport(0)),
				NameOfSupport(Structure.GetPieceSupport(1))),
			Structure.GetPieceSupport(0) == EPieceSupport::Grounded
				&& Structure.GetPieceSupport(1) == EPieceSupport::Supported);

		Structure.SolveLoads();

		TestTrue(
			FString::Printf(TEXT("a removed GROUNDED piece is no longer earth and must read Falling, got %s"),
				NameOfSupport(Structure.GetPieceSupport(0))),
			Structure.GetPieceSupport(0) == EPieceSupport::Falling);

		TestTrue(
			FString::Printf(TEXT("and the brick has lost the only thing holding it up, got %s"),
				NameOfSupport(Structure.GetPieceSupport(1))),
			Structure.GetPieceSupport(1) == EPieceSupport::Falling);

		for (int32 Index = 0; Index < Structure.NumPieces(); ++Index)
		{
			CheckSupportAgreesWithReason(*this, TEXT("after the ground was removed"), Structure, Index);
		}
	}

	/*
	 * A REMOVED PIECE INSIDE A KNOT IS FALLING, NOT STRANDED, which is the one place the two
	 * decisions above meet. Pull X out of a two-brick knot and Y is left head-jointed to
	 * nothing but a piece that is gone: the knot is dissolved, so neither is stranded any
	 * more, and calling the removed piece stranded would put a solver limitation in the
	 * report of a structure that no longer has one.
	 */
	{
		const FStructureSpec Knot = {
			{ { BrickMassKg, true }, { BrickMassKg, false }, { BrickMassKg, false } },
			{
				{ 0, 1, HeadJointNormal, JointAreaSqCm },
				{ 1, 2, HeadJointNormal, JointAreaSqCm }
			}
		};

		FStructure Structure;
		BuildStructure(Structure, Knot, Unbreakable);
		Structure.SolveLoads();

		TestTrue(
			FString::Printf(TEXT("the knot must be there to start with, got %s and %s"),
				NameOfSupport(Structure.GetPieceSupport(1)),
				NameOfSupport(Structure.GetPieceSupport(2))),
			Structure.GetPieceSupport(1) == EPieceSupport::Stranded
				&& Structure.GetPieceSupport(2) == EPieceSupport::Stranded);

		TestTrue(TEXT("removing X should report that it removed a live piece"),
			Structure.RemovePiece(1));

		Structure.SolveLoads();

		TestTrue(
			FString::Printf(TEXT("the removed piece is Falling rather than stranded, got %s"),
				NameOfSupport(Structure.GetPieceSupport(1))),
			Structure.GetPieceSupport(1) == EPieceSupport::Falling);

		TestTrue(
			FString::Printf(TEXT("Y is joined to nothing that exists, so it is Falling too, got %s"),
				NameOfSupport(Structure.GetPieceSupport(2))),
			Structure.GetPieceSupport(2) == EPieceSupport::Falling);

		TestTrue(
			FString::Printf(TEXT("and the earth is unmoved, got %s"),
				NameOfSupport(Structure.GetPieceSupport(0))),
			Structure.GetPieceSupport(0) == EPieceSupport::Grounded);

		for (int32 Index = 0; Index < Structure.NumPieces(); ++Index)
		{
			CheckSupportAgreesWithReason(*this, TEXT("a knot with a piece pulled out of it"), Structure, Index);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
