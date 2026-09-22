// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/*
 * Named, not anonymous: anonymous namespaces collapse under a unity build, and
 * this file's Mortar/Unbreakable/MakeNaN once collided with three other files.
 */
namespace StructureTestSupport
{
	using namespace DestructionProfiles;

	/*
	 * Unreal's default gravity, 980 cm/s2, spelled out so the test fails if
	 * production gets it wrong rather than agreeing with it.
	 *
	 * Unit trap: mass is already kg and length cm, so MassKg * 980 IS a force in
	 * uu — the 1 N = 100 uu conversion is baked into the 980, not applied on top.
	 * Multiplying by 100 again here would be wrong by exactly 100x.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double WeightOf(double MassKg)
	{
		return MassKg * GravityCmPerSecondSquared;
	}

	/** A standard UK metric brick, 215 x 102.5 x 65 mm, so 1432.44 cm3. A material owns density, not dimensions. */
	constexpr double BrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/*
	 * A standard clay brick derived from the profile, not hand-set: 1432.44 cm3
	 * at 1.9 g/cm3 is 2.7216 kg (DESIGN.md §3). Deriving rather than respelling is
	 * safe because MaterialInvariants already anchors density x volume to a real
	 * brick; a hand-set value here once drifted 1.6 g from the library.
	 *
	 * Safe at namespace scope only because ClayBrick is an aggregate of literals,
	 * so constant-initialised. A computed ratio would be dynamically initialised
	 * and cross-TU init order is unspecified — this could read zero, zeroing every
	 * force expectation while the suite stays green. LoadPath's opening guard checks it.
	 */
	const double BrickMassKg = ClayBrick.DensityGramsPerCubicCm * BrickVolumeCubicCm / 1000.0;

	/** 2667.2 uu. The number the whole load model is checked against. */
	const double BrickWeightUU = WeightOf(BrickMassKg);

	/** A 10 cm x 10 cm interface, kept round so the area split stays checkable by eye. */
	constexpr double JointAreaSqCm = 100.0;

	/** Horizontal interface, normal up at the piece above: a bed joint. Normal points toward PieceB (ConnectionLoad.h), so PieceB sits on PieceA. */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Vertical interface, normal pointing sideways at the neighbour: a head joint. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);
	const FVector OpposingHeadJointNormal(-1.0, 0.0, 0.0);

	/** A bed joint pointing DOWN: the same horizontal interface declared upper piece first. */
	const FVector InvertedBedJointNormal(0.0, 0.0, -1.0);

	/*
	 * Rectangles agreeing with JointAreaSqCm, one per axis a normal can lie on:
	 * the 10 x 10 cm face gives halves of 5, and 4 x 5 x 5 = 100. Zero sits on the
	 * normal's own axis; an extent on the third axis would describe a solid.
	 */
	const FVector BedJointHalfExtentCm(5.0, 5.0, 0.0);
	const FVector HeadJointHalfExtentCm(0.0, 5.0, 5.0);
	const FVector DepthwiseHalfExtentCm(5.0, 0.0, 5.0);

	/*
	 * The bed/head threshold: 45 degrees from vertical, a bed joint being nearer
	 * vertical than horizontal (DESIGN.md §3). It needs no material data, which
	 * matters since the tier decision precedes any strength profile.
	 * SupportTierThreshold pins it from both sides at 40 and 50 degrees; exactly
	 * 45 is a knife-edge tie not worth locking down in floating point.
	 *
	 * Spelled as the same literal production uses, bit for bit: written
	 * 1.0/FMath::Sqrt(2.0) it lands an ulp away from Structure.cpp's constexpr,
	 * invisible on an axis-aligned normal but not on the first tilted one. If the
	 * production constant changes, match it; don't re-derive it.
	 */
	constexpr double BedJointCosine = 0.70710678118654752440;

	/** A normal tilted this many degrees away from straight up, in the XZ plane. */
	FVector NormalTiltedFromVertical(double Degrees)
	{
		const double Radians = FMath::DegreesToRadians(Degrees);
		return FVector(FMath::Sin(Radians), 0.0, FMath::Cos(Radians));
	}

	/*
	 * Load-path tests use the shared DestructionProfiles::Unbreakable: they
	 * measure routing and accumulation, not whether a joint gives. It's
	 * TestFixture-classified so ConnectionInvariants would catch an escaped copy.
	 * Genuinely-overloaded joints use GeneralPurposeMortar directly.
	 */

	/*
	 * Force, in uu, that loads the given area to the given stress. Spelled out
	 * rather than reusing ForceUnitsPerMPaSqCm: 1 N = 100 uu, 1 cm2 = 100 mm2,
	 * 1 MPa = 1 N/mm2 -> 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	/** The same boundary the other way: stress in MPa a force puts on an area. */
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

		/*
		 * The joint's rectangle, zero by default on purpose. Zero extents are not
		 * a degenerate joint but an unmeasured lever arm; with no moment the area
		 * alone answers a centred load bit for bit, which keeps every geometry-free
		 * fixture in this file valid.
		 */
		FVector CentreCm = FVector::ZeroVector;
		FVector HalfExtentCm = FVector::ZeroVector;
	};

	/** A whole structure as data, so topologies are a table rather than code. */
	struct FStructureSpec
	{
		TArray<FPieceSpec> Pieces;
		TArray<FConnectionSpec> Connections;
	};

	/** What a joint does with the load it is handed: the directional model a magnitude alone cannot show. */
	enum class EJointKind : uint8
	{
		/** Substantially vertical normal: bears the load in pure compression. */
		Bed,

		/** Substantially horizontal normal: can only carry it in pure shear. */
		Head,
	};

	struct FExpectedJoint
	{
		/*
		 * Signed Z of the force the solver must store, in uu. The stored force acts
		 * on PieceB (ConnectionLoad.h): when the supported piece is PieceB that is
		 * its weight, straight down; declared the other way round the stored Z is
		 * positive. Getting the sign backwards turns compression into tension, and
		 * mortar's tensile limit is a hundredth of its compressive one.
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
		Connection.InterfaceCentreCm = Spec.CentreCm;
		Connection.InterfaceHalfExtentCm = Spec.HalfExtentCm;
		Connection.Strength = Strength;
		return Connection;
	}

	/*
	 * Build a structure in place. By reference on purpose: FConnection's has-given
	 * latch is per-copy, so passing connections by value risks latching a
	 * temporary and leaving the real joint untouched (CURRENT_STATE.md).
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

	/*
	 * Assert what one connection carries AND how the joint resolves it: two claims.
	 * The vector's sign depends on which end was declared first, so a solver that
	 * always writes (0, 0, -Share) is only right when the supported piece is named
	 * second. Classification is the payoff of the directional model: the same
	 * downward load is compression on a bed joint and shear on a head joint, which
	 * magnitude alone can't tell apart, and it decides which strength the load is
	 * later compared against.
	 *
	 * Tension reads zero here as a property of the fixtures, not the model: every
	 * spec routed through this helper is axis-aligned and geometry-free, so every
	 * lever arm is zero. An eccentric load can pull a joint open, but that tension
	 * is carried as a moment downstream, never reaching this struct (a brick on one
	 * head joint reads zero here but 0.4157 in HangingBrickPeelsRatherThanShears).
	 * Tilted normals don't generalise: the sign-blind head tier holds a piece by
	 * pulling it open, characterised in Structure.TiltedJointClassification.
	 *
	 * Calls ClassifyForce directly (DESIGN.md's degenerate-normal hole) — safe
	 * here: no break decision, and every normal is a real plane.
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

		// Gravity stays vertical however the joint is oriented; a solver pushing load along the normal gets the keystone magnitudes right and the direction wrong.
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

	/*
	 * The pieces that hold PieceIndex up, per DESIGN.md §3's two-tier rule. A
	 * line-for-line transcription of FStructure::GetJointRole, threshold included,
	 * so it catches transcription slips (a flipped sign, a dropped case) rather
	 * than being a second opinion on the rule. SpecReachesGround builds the
	 * stronger, genuinely-different reachability check on top.
	 *
	 * This has no opinion on unroutable knots: DESIGN.md §3 reports knotted pieces
	 * as falling even though they reach earth through the support relation, and
	 * this walk would call them supported. Any spec with a knot needs an explicit
	 * expectation table, not the DegenerateInputs matrix — every spec there
	 * strands nothing; check that before adding one.
	 *
	 * The normal is turned to point at this piece: substantially up is a bed joint
	 * beneath that bears it; down is a bed joint above that doesn't; between is a
	 * head joint, a support only with no bed joint at all. The relation being
	 * directed is the correction — raw connectivity let a sideways path exclude the
	 * bed joint carrying the wall.
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

	/*
	 * Does this piece reach the ground through supports, not mere connections?
	 * Being joined to a neighbour isn't support; two pieces can each hang from the
	 * other and reach nothing, so this walk is directed and tracks what it visited.
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

	/*
	 * Build, solve, and check one case against its explicit expectation table.
	 * Explicit rather than derived from SpecReachesGround, which doesn't model
	 * stranding for knot cases and can't produce the load figures.
	 *
	 * The conservation sum is a cross-check, not the main assertion: over-stranding
	 * preserves conservation exactly, so it's blind to the knot defect class. It
	 * catches the rest of the arithmetic, which is why LoadPath runs through here
	 * rather than keeping its own near-identical loop.
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

		// The loops below are bounded by the expectation arrays, so a case that gained a piece or joint and forgot its row asserts nothing about it.
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

	/*
	 * A support state as text, so a failure names the answer, not a number. The
	 * default arm is not a fifth name: it fires if the enum grows a value nobody
	 * wired in, and "an unknown state" beats a plausible-looking label.
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

	/* A joint role as text, same reasoning as NameOfSupport; the default arm reads as unknown rather than a plausible tier. */
	const TCHAR* NameOfJointRole(EJointRole Role)
	{
		switch (Role)
		{
		case EJointRole::None:       return TEXT("None");
		case EJointRole::BedBeneath: return TEXT("BedBeneath");
		case EJointRole::BedAbove:   return TEXT("BedAbove");
		case EJointRole::Head:       return TEXT("Head");
		default:                     return TEXT("an unknown role");
		}
	}

	/*
	 * Ties GetPieceSupport (the reason) to IsPieceSupported (the composite answer),
	 * checked on every piece of every case: computed apart they would drift
	 * silently, both still plausible. Also pins the enum shape — exactly two of the
	 * four states mean "held up", so a fifth must be classified here on purpose.
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

/*
 * A structure accumulates weight downward and hands each connection the share it
 * supports. Pure arithmetic over a graph (no world, no positions), so the
 * assertion is on the mechanism, the force each joint carries, never on movement.
 *
 * Support is two-tiered (DESIGN.md §3): a piece's supports are the bed joints
 * beneath it, and only a piece with none falls back to head joints. Routing by
 * graph distance is wrong in the case the game is about: a brick spanning a gap
 * is the same distance from earth as the brick on it, so the bed joint between
 * them would carry nothing.
 *
 * Each case changes an answer on its own: accumulation vs. piece count (unequal
 * masses), area vs. even split (unequal areas), grounding vs. connection (the
 * ungrounded stack), bed-beneath vs. bed-above (the hanging case), tier rule vs.
 * graph distance (the running-bond wall).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureLoadPathTest,
	"DestructionGame.Core.Structure.LoadPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureLoadPathTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	// Guards the derivation: if ClayBrick ever stops being constant-initialised BrickMassKg reads zero, zeroing every force expectation while the suite stays green. MaterialInvariants owns the real anchor.
	TestTrue(
		FString::Printf(TEXT("the brick mass must derive to something positive, got %f kg"), BrickMassKg),
		BrickMassKg > 0.0);

	/*
	 * FSolveCase and CheckSolveCase, shared with the stranding tests. Every case
	 * below strands nothing, so the ground-reaction-conservation cross-check is
	 * exact and catches a share written to the wrong joint.
	 */
	const TArray<FSolveCase> Cases = {
		// Base case: nothing above, nowhere to pass load. Stops a solver inventing load from a lone grounded brick.
		{
			TEXT("a lone grounded piece carries nothing"),
			{ { { BrickMassKg, true } }, {} },
			{},
			{ true }
		},

		// One brick on a grounded brick: the joint carries one brick, 2.7216 kg x 980 = 2667.2 uu (DESIGN.md §3).
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
		 * The same joint declared upper piece first: piece 1 is still the top brick
		 * but now PieceA with the normal pointing down, which ConnectionLoad.h
		 * allows. The stored force acts on PieceB, so it negates to +2666, up. Store
		 * -2666 and dot((0,0,-W),(0,0,-1)) is positive — reads as tension, and
		 * mortar's tensile limit is a hundredth of its compressive one.
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

		// Three high: the lower joint carries two bricks and the upper one. A solver loading each joint with only the piece directly above gets the lower half wrong.
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

		// Accumulation is by weight, not piece count: lopsided masses, since counting pieces can't produce 3920 and 2940.
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

		// Two supports of equal area split evenly: the defensible default, and the one that must degenerate correctly.
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

		// Unequal areas split in proportion: 100 and 300 cm2 take a quarter and three quarters, distinguishing "weighted by area" from an even "shared" split.
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
		 * DESIGN.md §2's worked example. Piece 2 spans a gap like a keystone: its
		 * only path to ground runs sideways through two head joints, so it and the
		 * brick on top push their combined weight through those. The force is
		 * straight down regardless — FConnection turns it into shear here and
		 * compression above, so the solver must never orient force along the normal.
		 * Piece 2 passes two bricks' weight down, split evenly across the two joints.
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
			// Three identical magnitudes, told apart only by classification: two head joints in pure shear (the weak axis), the bed joint above in pure compression.
			{
				{ -BrickWeightUU, EJointKind::Head },
				{ -BrickWeightUU, EJointKind::Head },
				{ -BrickWeightUU, EJointKind::Bed }
			},
			{ true, true, true, true }
		},

		/*
		 * The same keystone, every joint declared supported-piece-first: only which
		 * end is PieceA changes, so every force flips sign but every classification
		 * stays identical. That invariance (ConnectionLoad.h's orientation
		 * convention) is what makes per-piece bookkeeping safe.
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
		 * A bed joint above a piece does not hold it up. Piece 1 hangs under
		 * grounded slab 2 and touches grounded piece 0 sideways; the bed joint is
		 * the wrong way round to bear anything, so piece 1 falls back to its one head
		 * joint, which takes the whole brick. Routing by graph distance would split
		 * it evenly instead.
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
		 * The case the two-tier rule was written for: a running-bond wall with the
		 * middle of the bottom course spanning a gap, two courses stacked above.
		 *
		 *        E1          course E   piece 5
		 *     D1    D2       course D   pieces 3, 4
		 *   Cx  Cy  Cz       course C   pieces 0, 1, 2   (Cy spans the gap)
		 *   ==      ==       earth, missing under Cy
		 *
		 * Cx and Cz rest on earth; Cy reaches it only sideways through two head
		 * joints. Every bed joint is equal area, so every split is even. Under the
		 * tier rule Cy is load-bearing: it receives 1.5 bricks from above, adds its
		 * own, and pushes 2.5 out sideways — 1.25 per head joint; 0.75 per bed joint
		 * above; 2 bricks each into Cx and Cz. Graph-distance routing gives Cy the
		 * same depth as D1/D2, so the bed joints onto it carry zero and DESIGN.md
		 * §4's shear test can't fire. Connection 1 is declared keystone-first, so the
		 * sign convention is under test in the case that matters.
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

		// A grounded piece terminates the flow: a joint between two grounded pieces carries nothing, stopping load circulating along a foundation course.
		{
			TEXT("a joint between two grounded pieces carries nothing"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, true } },
				{ { 0, 1, HeadJointNormal, JointAreaSqCm } }
			},
			{ { 0.0, EJointKind::Head } },
			{ true, true }
		},

		// Connected but to nothing that reaches earth: joining a neighbour is not support, so nothing holds this stack up and the joint carries zero.
		{
			TEXT("a stack with nothing grounded is unsupported and carries no static load"),
			{
				{ { BrickMassKg, false }, { BrickMassKg, false } },
				{ { 0, 1, BedJointNormal, JointAreaSqCm } }
			},
			{ { 0.0, EJointKind::Bed } },
			{ false, false }
		},

		// A grounded piece elsewhere doesn't help an island that can't reach it: support is a path, not the mere presence of ground.
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
		 * A support that is itself falling is not a support.
		 *
		 *          D          piece 3, floating
		 *          |  bed joint, D above F
		 *   G  —   C  —  F    pieces 0, 1, 2, joined sideways
		 *   ==                earth, under G only
		 *
		 * C has no bed joint, so it falls back to both head joints: to grounded G
		 * and to F. F has a bed joint beneath it (D), so the tier rule discards C—F
		 * from F's supports — not the cycle case; the walk correctly reports F and D
		 * unsupported. The question is where C's weight goes. Splitting evenly
		 * credits half to falling F and loses it, failing conservation by 2x and
		 * leaving G—C reading half its true load. So a support with no path to ground
		 * drops out of the area total and the share, and G—C takes the whole 2666.
		 * Dropping it is safe: a piece marked supported was reached through a
		 * supported support, so at least one always survives the filter.
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

		// Mutual lateral support: neither has a bed joint, so both name the other as their head-joint support (a two-cycle). Both unsupported; an untracked walk loops here forever.
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

/*
 * Where the line between a bed joint and a head joint sits. The two-tier rule
 * turns on "substantially vertical", so that phrase needs a number: 45 degrees
 * from vertical (BedJointCosine). These cases pin it from both sides rather than
 * asserting the exact tie.
 *
 * One piece held by two equal-area joints, so area can't explain the answer: one
 * at a varying tilt, the other flat vertical. A tilted bed joint is the only
 * support and takes the whole brick; a tilted head joint means the piece falls
 * back to both and splits evenly — 2666 against 1333. The 0-degree row also
 * states that a bed joint beneath wins outright over a head joint.
 *
 * Magnitudes only: it never calls ClassifyForce, and its tilted joint is declared
 * { 0, 2, tilted }, the one orientation that can't produce tension.
 * Structure.TiltedJointClassification covers the other orientation and the split.
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

		// Whichever tier it lands in, the load is still gravity, straight down; only what the joint does with it changes.
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

/*
 * What an inclined joint does with its load, including pulling the piece open, an
 * accepted outcome of the model. A characterisation test: every assertion already
 * passes, and exists because the behaviour is pinned nowhere else and reads as a
 * bug on first encounter. Every other normal in this file is axis-aligned, so two
 * comments elsewhere ("tension is always zero", "gravity never pulls a joint
 * open") are properties of the fixtures, not the model; this scopes them. (An
 * eccentric load pulls a square joint open too, but that is a stress carried as a
 * moment, invisible here — see Structure.HangingBrickPeelsRatherThanShears.)
 *
 * The shape: one brick, one earthed anchor, one inclined joint, varying the tilt
 * and which side of the piece the face is on:
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
 * Both carry the same magnitude; the axis differs. GetJointRole is sign-blind
 * above 45 degrees (|NormalZTowardPiece| > cos45, then reads the sign), so a joint
 * past 45 sitting above a piece is Head, a fallback support, and the piece hangs
 * in tension (DESIGN.md §3 accepts this). Under 45 it's BedAbove, bears nothing,
 * and the piece falls — same outcome, different route, so 45 only looks like a
 * cliff. Numbers for load W through a face tilted T: normal W cos T, in-plane
 * W sin T. Expected magnitudes come from trigonometry, production from a dot
 * product — they agree on values, not method; the assertion is which axis the
 * normal component lands on.
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

		/*
		 * Whether the inclined face sits above the loaded piece, the same as
		 * declaring it loaded-piece-first: the normal points toward PieceB, so
		 * naming the loaded piece PieceA puts the face over its head.
		 */
		bool bJointAbovePiece;

		bool bExpectedSupported;
		ENormalAxis ExpectedNormalAxis;
	};

	const TArray<FTiltCase> Cases = {
		// Beneath the piece: the control. Whichever tier it lands in the face is under the brick and squeezed (bed at 40, head still pressed at 50+), which makes the above rows the interesting ones.
		{ TEXT("a bed joint 40 degrees beneath a piece is compressed, and sheared too"),
			40.0, false, true, ENormalAxis::Compression },
		{ TEXT("a head joint 46 degrees beneath a piece is compressed"),
			46.0, false, true, ENormalAxis::Compression },
		{ TEXT("a head joint 50 degrees beneath a piece is compressed"),
			50.0, false, true, ENormalAxis::Compression },
		{ TEXT("a head joint 60 degrees beneath a piece is compressed"),
			60.0, false, true, ENormalAxis::Compression },
		// The anchor row: at 90 the face is vertical, cos is zero, and the normal axis carries nothing. Still asserted, since it's the sign of the dot product.
		{ TEXT("a vertical head joint beneath a piece is pure shear, with nothing on the normal axis"),
			90.0, false, true, ENormalAxis::Compression },

		// Above the piece, the finding: under 45 it's BedAbove, bears nothing, brick falls; over 45 the sign-blind head tier makes the same face a support, pulling it open.
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
		// Piece 0 is the held-up brick, piece 1 the grounded anchor. Same normal in both arrangements; only which end is named first changes.
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

		// The anchor is grounded and never in question; an unsupported anchor would make every load figure below meaningless.
		TestTrue(
			FString::Printf(TEXT("%s: the grounded anchor must stay supported"), Case.Description),
			Structure.IsPieceSupported(1));

		const double Radians = FMath::DegreesToRadians(Case.DegreesFromVertical);
		const double NormalMagnitude =
			Case.ExpectedNormalAxis == ENormalAxis::Unloaded ? 0.0 : BrickWeightUU * FMath::Cos(Radians);
		const double ShearMagnitude =
			Case.ExpectedNormalAxis == ENormalAxis::Unloaded ? 0.0 : BrickWeightUU * FMath::Sin(Radians);

		// The sign distinguishes the two arrangements: the stored force acts on PieceB, so naming the loaded piece first stores the reaction pointing up, landing the load on tension rather than compression.
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

		// Still gravity, straight down, however the face is angled; a solver pushing load along the normal gets the magnitudes right and the direction wrong.
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

		// Legs of a right triangle on hypotenuse W: they can't both be wrong and still square up, stopping a solver that put the whole load on one axis.
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
	 * Why the axis is worth its own test: the same brick on the same face is
	 * measured against a different strength depending on which side it sits, two
	 * orders of magnitude apart for mortar. One brick on 100 cm2 through a face at
	 * 50 degrees, all three axes worked since ComputeUtilisation returns the worst:
	 *
	 *   normal  W cos50 = 1714.44 uu -> 0.00171444 MPa
	 *   shear   W sin50 = 2043.20 uu -> 0.00204320 MPa
	 *
	 *   FACE BENEATH  shear governs:   0.00204320 / (0.2 + 0.6 x 0.00171444) = 0.010164
	 *   FACE ABOVE    tension governs: 0.00171444 / 0.1                      = 0.017144
	 *
	 * The two faces' shear figures are within 0.5%, so a test asserting only "the
	 * hanging joint is worse off" could pass on shear alone. Each is pinned to its
	 * axis, computed from SI definitions rather than ForceUnitsPerMPaSqCm so a wrong
	 * conversion fails here. Neither joint gives: one brick is 1.7% of the tensile
	 * limit, so DESIGN.md §3's "fails almost at once" is about the strength ratio,
	 * not this load.
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
		 * Identical joints (ApplyForce ignores piece handles); only the sign of the
		 * stored force differs. Two objects, not one, because ApplyForce latches and
		 * a shared one would make the second HasGiven check depend on the first.
		 */
		FConnection Beneath;
		Beneath.InterfaceNormal = NormalTiltedFromVertical(DegreesFromVertical);
		Beneath.InterfaceAreaSqCm = JointAreaSqCm;
		Beneath.Strength = GeneralPurposeMortar;

		FConnection Hanging = Beneath;

		const double BeneathUtilisation = Beneath.ApplyForce(FVector(0.0, 0.0, -BrickWeightUU));
		const double AboveUtilisation = Hanging.ApplyForce(FVector(0.0, 0.0, BrickWeightUU));

		// Mohr-Coulomb: only compression buys friction, so the hanging joint's shear capacity is bare cohesion.
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

/*
 * The structure owns the graph, so nonsense handles and impossible interfaces are
 * rejected here rather than carried as healthy-looking joints. A connection with
 * PieceA == PieceB or both handles INDEX_NONE once read as a fine joint under any
 * load; rejection means it is not added, not stored and skipped.
 *
 * It also validates the piece's centre of mass (Structure.h refuses a non-finite
 * one): a NaN centre launders through two cross products into a NaN moment, which
 * reads as intact at the break decision, so the wall stands rather than falls, the
 * wrong direction. And the joint's rectangle, for the same reason: a rectangle
 * that disagrees with its area is a plausible number with the wrong lever arm, the
 * same fault class as a normal inconsistent with its A/B pairing (Layout.h).
 *
 * Zero extents are not degenerate but unmeasured bending capacity, so the
 * consistency rule applies only when a rectangle was supplied. Every tilted
 * fixture relies on that: a tilted normal may carry no rectangle and stay a good
 * geometry-free joint, keeping TiltedJointClassification and SupportTierThreshold
 * buildable.
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

	// Pieces first: a nonsense mass must not enter the structure, since NaN launders into plausible-looking numbers once it's in the array.
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

		/*
		 * The same door with a centre of mass, which no row above covers. A NaN
		 * centre is worse than a NaN mass: it becomes a lever arm the moment a joint
		 * centroid is subtracted from it, feeding the moment's cross product, and
		 * since every NaN comparison is false the break decision reads the joint as
		 * intact — the wall stands rather than falls (Structure.h).
		 *
		 * Checked one axis at a time as well as all three: a guard written as a
		 * length test, or on X alone, passes a vector whose Y is broken, and Y is the
		 * axis every bed joint here bends about.
		 *
		 * An accepted centre must survive intact, so a door that refused everything
		 * can't pass, nor one that "fixes" a bad centre by zeroing it — a zeroed
		 * centre invents a lever arm of metres on a wall laid off-origin.
		 */
		struct FCentreCase
		{
			const TCHAR* Description;
			FVector CentreCm;
			bool bIsAccepted;
		};

		const TArray<FCentreCase> CentreCases = {
			{ TEXT("a brick that knows where it was laid"), FVector(11.25, 0.0, 3.25), true },
			{ TEXT("a piece whose centre is the world origin"), FVector::ZeroVector, true },
			{ TEXT("a centre a long way from the origin"), FVector(-4000.0, 250.0, 9000.0), true },
			{ TEXT("a centre whose X is NaN"), FVector(NaNValue, 0.0, 3.25), false },
			{ TEXT("a centre whose Y is NaN"), FVector(11.25, NaNValue, 3.25), false },
			{ TEXT("a centre whose Z is NaN"), FVector(11.25, 0.0, NaNValue), false },
			{ TEXT("a wholly NaN centre"), FVector(NaNValue, NaNValue, NaNValue), false },
			{ TEXT("a centre whose X is infinite"), FVector(InfinityValue, 0.0, 3.25), false },
			{ TEXT("a centre whose Y is infinite"), FVector(11.25, InfinityValue, 3.25), false },
			{ TEXT("a centre whose Z is minus infinite"), FVector(11.25, 0.0, -InfinityValue), false },
		};

		for (const FCentreCase& Case : CentreCases)
		{
			FStructure Structure;

			const int32 Handle = Structure.AddPiece(BrickMassKg, false, Case.CentreCm);

			const int32 ExpectedHandle = Case.bIsAccepted ? 0 : INDEX_NONE;
			const int32 ExpectedCount = Case.bIsAccepted ? 1 : 0;

			TestTrue(
				FString::Printf(TEXT("%s: expected handle %d, got %d"),
					Case.Description, ExpectedHandle, Handle),
				Handle == ExpectedHandle);

			TestTrue(
				FString::Printf(TEXT("%s: expected %d pieces in the structure, got %d"),
					Case.Description, ExpectedCount, Structure.NumPieces()),
				Structure.NumPieces() == ExpectedCount);

			if (Case.bIsAccepted)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s: should be stored bit for bit as given, (%g, %g, %g); it reads (%g, %g, %g)"),
						Case.Description,
						Case.CentreCm.X, Case.CentreCm.Y, Case.CentreCm.Z,
						Structure.GetPiece(0).CentreOfMassCm.X,
						Structure.GetPiece(0).CentreOfMassCm.Y,
						Structure.GetPiece(0).CentreOfMassCm.Z),
					Structure.GetPiece(0).CentreOfMassCm == Case.CentreCm);

				TestTrue(
					FString::Printf(TEXT("%s: and should be flagged as knowing where it is"),
						Case.Description),
					Structure.GetPiece(0).bHasCentreOfMass);
			}
			else
			{
				// Refused outright, not stored with the flag cleared: that would read as "nobody said where it is", a healthy state, so the refusal must be an absent piece.
				TestTrue(
					FString::Printf(
						TEXT("%s: nothing may have been stored, the structure holds %d live pieces"),
						Case.Description, Structure.NumLivePieces()),
					Structure.NumLivePieces() == 0);
			}
		}
	}

	// Handles are sequential and are what a connection refers to: without this the rejection cases above could be satisfied by an AddPiece that never accepts anything.
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
			 * Area is rejected at construction, not solve time: the load split
			 * divides by total supporting area, and zero or NaN gives nothing sensible.
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

			// A normal that will not normalise describes no interface plane, so there is no joint here to load.
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

			/*
			 * The joint's rectangle, and the state this door makes inexpressible: an
			 * extent disagreeing with its area is the same fault class as a normal
			 * disagreeing with its A/B pairing (Layout.h). Area governs the load split,
			 * the rectangle the lever arm, and mortar's tensile strength is a hundredth
			 * of its compressive one, so a lever arm out by a factor moves the governing
			 * axis. Area is 100 cm2 throughout, so a consistent rectangle is 4 x 5 x 5.
			 */
			{
				TEXT("a bed joint whose rectangle agrees with its area"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, BedJointHalfExtentCm },
				true
			},

			/*
			 * No rectangle at all is not a broken one: zero extents mean unmeasured
			 * bending capacity, a healthy joint the area alone answers exactly. So the
			 * consistency rule is conditional on a rectangle being supplied;
			 * unconditional, it rejects 4 x 0 x 0 against 100 and takes every
			 * geometry-free fixture with it.
			 */
			{
				TEXT("no rectangle at all is a joint with no bending capacity known, not a broken one"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, FVector::ZeroVector },
				true
			},

			{
				TEXT("a rectangle 10 percent smaller than the area it claims"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, FVector(5.0, 4.5, 0.0) },
				false
			},
			{
				TEXT("a rectangle twice the area it claims"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, FVector(5.0, 10.0, 0.0) },
				false
			},

			/*
			 * Partially filled in, a value assembled by hand and stopped halfway. One
			 * extent at zero collapses the rectangle to a line, and 4 x 5 x 0 = 0
			 * against 100 is a disagreement. "Is a rectangle supplied" must be answered
			 * as "any component non-zero", not per-axis, or this row sails through as no
			 * geometry.
			 */
			{
				TEXT("a rectangle with one extent left at zero is a line, not a face"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, FVector(5.0, 0.0, 0.0) },
				false
			},

			/*
			 * The one that fails open: two negative halves multiply into a plausible
			 * 4 x -5 x -5 = 100, so a guard comparing only the product against area
			 * accepts an inside-out rectangle. Its section modulus would be negative and
			 * every downstream stress would flip sign.
			 */
			{
				TEXT("two negative half-extents multiply into a plausible area and are still not a face"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, FVector(-5.0, -5.0, 0.0) },
				false
			},

			/*
			 * Area-consistent and still not a face: 4 x 5 x 5 is exactly 100, but the Z
			 * extent is on the normal's own axis, making it a box. The in-plane frame is
			 * the two world axes that aren't the separation axis; a third extent means a
			 * different idea of which axes those are.
			 */
			{
				TEXT("an extent on the normal's own axis is a box, not a face"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, FVector(5.0, 5.0, 3.25) },
				false
			},

			/*
			 * The tolerance, bracketed from both sides. Two derivations of one face can
			 * disagree in the last few bits, so the rule can't be exact equality, but
			 * can't be slack or it misses a rectangle describing a different face. A
			 * relative 1e-12 is re-derivation noise and is accepted; 1e-6 isn't
			 * reachable by rounding and is refused.
			 */
			{
				TEXT("a rectangle off by a relative 1e-12 is two derivations of one face"),
				{ 0, 1, BedJointNormal, JointAreaSqCm,
					FVector::ZeroVector, FVector(5.0, 5.0 * (1.0 - 1.0e-12), 0.0) },
				true
			},
			{
				TEXT("a rectangle off by a relative 1e-6 is a different face"),
				{ 0, 1, BedJointNormal, JointAreaSqCm,
					FVector::ZeroVector, FVector(5.0, 5.0 * (1.0 - 1.0e-6), 0.0) },
				false
			},

			/*
			 * Degenerate geometry fails closed, same as a degenerate area. A NaN
			 * half-extent divides into a NaN section modulus, and NaN compares false
			 * against "> 1.0", so the joint reports itself fine forever. The centre is
			 * checked too: subtracted from a centre of mass for a lever arm, a NaN there
			 * launders into a NaN moment.
			 */
			{
				TEXT("a NaN half-extent"),
				{ 0, 1, BedJointNormal, JointAreaSqCm,
					FVector::ZeroVector, FVector(5.0, NaNValue, 0.0) },
				false
			},
			{
				TEXT("an infinite half-extent"),
				{ 0, 1, BedJointNormal, JointAreaSqCm,
					FVector::ZeroVector, FVector(5.0, InfinityValue, 0.0) },
				false
			},
			{
				TEXT("a NaN interface centre"),
				{ 0, 1, BedJointNormal, JointAreaSqCm,
					FVector(NaNValue, 0.0, 0.0), BedJointHalfExtentCm },
				false
			},
			{
				TEXT("an infinite interface centre"),
				{ 0, 1, BedJointNormal, JointAreaSqCm,
					FVector(0.0, 0.0, InfinityValue), BedJointHalfExtentCm },
				false
			},

			// A centroid is a world position with no sensible bound: a wall laid off the origin puts every joint far from zero, so only finiteness is a rule.
			{
				TEXT("a joint a long way from the origin is still a joint"),
				{ 0, 1, BedJointNormal, JointAreaSqCm,
					FVector(5.625, -4000.0, 7.0), BedJointHalfExtentCm },
				true
			},

			/*
			 * The other two axes a normal can lie on: a guard hard-coding X and Y as
			 * in-plane would pass every bed-joint row above and refuse both of these.
			 */
			{
				TEXT("a rectangle on a head joint's normal"),
				{ 0, 1, HeadJointNormal, JointAreaSqCm,
					FVector::ZeroVector, HeadJointHalfExtentCm },
				true
			},
			{
				TEXT("a rectangle on a normal along Y"),
				{ 0, 1, FVector(0.0, 1.0, 0.0), JointAreaSqCm,
					FVector::ZeroVector, DepthwiseHalfExtentCm },
				true
			},
			{
				TEXT("a rectangle on a bed joint declared upper piece first"),
				{ 0, 1, InvertedBedJointNormal, JointAreaSqCm,
					FVector::ZeroVector, BedJointHalfExtentCm },
				true
			},
			{
				TEXT("a rectangle on a non-unit normal, which describes the same plane"),
				{ 0, 1, FVector(0.0, 0.0, 5.0), JointAreaSqCm,
					FVector::ZeroVector, BedJointHalfExtentCm },
				true
			},

			/*
			 * A tilted normal may not carry a rectangle: the in-plane frame is only
			 * defined when there's a separation axis, and a normal 40 degrees off
			 * vertical has two candidates, so choosing would silently pick a section
			 * modulus. MakeInterface only emits axis-aligned normals, so nothing the
			 * producer builds is refused. The 1e-6 row keeps the line bright: a normal a
			 * millionth off an axis wasn't rounded off an exact one, it meant something
			 * else.
			 */
			{
				TEXT("a rectangle on a normal tilted 40 degrees"),
				{ 0, 1, NormalTiltedFromVertical(40.0), JointAreaSqCm,
					FVector::ZeroVector, BedJointHalfExtentCm },
				false
			},
			{
				TEXT("a rectangle on a normal tilted 1 degree"),
				{ 0, 1, NormalTiltedFromVertical(1.0), JointAreaSqCm,
					FVector::ZeroVector, BedJointHalfExtentCm },
				false
			},
			{
				TEXT("a rectangle on a normal a millionth off an axis"),
				{ 0, 1, FVector(1.0e-6, 0.0, 1.0), JointAreaSqCm,
					FVector::ZeroVector, BedJointHalfExtentCm },
				false
			},

			/*
			 * And every tilted fixture keeps working, the other half of the rule.
			 * TiltedJointClassification builds joints at 40, 46, 50, 60 and 90 degrees;
			 * refusing a tilted normal outright would delete its fixture and
			 * SupportTierThreshold's. The 90-degree row isn't incidental: cos(90) is
			 * 6.1e-17, not 0, so that normal isn't axis-aligned by any exact test and
			 * must still be a good geometry-free joint.
			 */
			{
				TEXT("a tilted normal with no rectangle is still a joint, at 40 degrees"),
				{ 0, 1, NormalTiltedFromVertical(40.0), JointAreaSqCm },
				true
			},
			{
				TEXT("a tilted normal with no rectangle is still a joint, at 50 degrees"),
				{ 0, 1, NormalTiltedFromVertical(50.0), JointAreaSqCm },
				true
			},
			{
				TEXT("a normal a hair off vertical with no rectangle is still a joint, at 90 degrees"),
				{ 0, 1, NormalTiltedFromVertical(90.0), JointAreaSqCm },
				true
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

			/*
			 * Scoped to rows supposed to be accepted, not merely rows that were: a row
			 * wrongly let through already said so twice above, and an equality check on
			 * its degenerate value only reports that a NaN isn't equal to itself.
			 */
			if (Handle == INDEX_NONE || !Case.bIsAccepted)
			{
				continue;
			}

			/*
			 * An accepted joint keeps its rectangle bit for bit. The door validates and
			 * doesn't normalise: zeroing a slightly-off rectangle would turn every
			 * refusal above into silent acceptance of a joint with no bending capacity.
			 * Refuse or store; nothing else.
			 */
			const FConnection& Stored = Structure.GetConnection(Handle);

			TestTrue(
				FString::Printf(
					TEXT("%s: the stored interface centre should be (%.10g, %.10g, %.10g), got")
					TEXT(" (%.10g, %.10g, %.10g)"),
					Case.Description,
					Case.Spec.CentreCm.X, Case.Spec.CentreCm.Y, Case.Spec.CentreCm.Z,
					Stored.InterfaceCentreCm.X,
					Stored.InterfaceCentreCm.Y,
					Stored.InterfaceCentreCm.Z),
				Stored.InterfaceCentreCm == Case.Spec.CentreCm);

			TestTrue(
				FString::Printf(
					TEXT("%s: the stored half-extent should be (%.10g, %.10g, %.10g), got")
					TEXT(" (%.10g, %.10g, %.10g)"),
					Case.Description,
					Case.Spec.HalfExtentCm.X, Case.Spec.HalfExtentCm.Y, Case.Spec.HalfExtentCm.Z,
					Stored.InterfaceHalfExtentCm.X,
					Stored.InterfaceHalfExtentCm.Y,
					Stored.InterfaceHalfExtentCm.Z),
				Stored.InterfaceHalfExtentCm == Case.Spec.HalfExtentCm);
		}
	}

	return true;
}

/*
 * Solving is not destructive, however overloaded the structure. Loads are
 * computed; breaking is a separate step. Solving via ApplyForce would break
 * joints as a side effect: it latches, so the solve couldn't be re-run and a
 * joint that gave mid-solve reports zero, making the computed load a lie. This
 * test only pins that solving must not commit. The masses are absurd so the load
 * is past every axis of a real mortar joint, worked below so the test can't be
 * satisfied by a load that was never over the limit.
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
	 * Compressive limit is 10 MPa over 100 cm2 = 1e7 uu, so the bed joint is at
	 * 1.96x; the head joint sees the same force as shear against at most 1.3e6 uu,
	 * fifteen times over. Both axes are past limit. Capacities come from the
	 * profile, so no plausible retune stops these being an overload.
	 */
	const double CompressiveCapacityUU =
		ForceForMPa(GeneralPurposeMortar.CompressiveStrengthMPa, JointAreaSqCm);
	const double MaxShearCapacityUU =
		ForceForMPa(GeneralPurposeMortar.MaxShearStrengthMPa, JointAreaSqCm);

	/*
	 * Two separate overloaded pieces, one per joint, forced by the two-tier rule
	 * (DESIGN.md §3): a piece with a bed joint beneath doesn't fall back to head
	 * joints, so one piece can't load both. Piece 2 rests on grounded piece 0
	 * through a bed joint; piece 3 hangs sideways off grounded piece 1 through its
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
		 * Without this a solver that computed nothing would pass, a zero load leaving
		 * every joint intact for the wrong reason. Each joint against the capacity it
		 * uses: connection 0 (bed) is pure compression, connection 1 (head) pure
		 * shear. Checking both bounds against both would assert the bed joint past a
		 * shear capacity it carries none of, silent on the governing axis.
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

	// Solving twice must give the same answer; a destructive solve would report a smaller load the second time.
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

/*
 * Properties that must hold across every structure, not particular numbers
 * (LoadPath's job). Five invariants, each guarding a silent failure:
 *
 *  - Every load is finite and never NaN. FMath::Max/Min launder a NaN mass or
 *    area into a plausible number; the door guard is in GraphValidation, but this
 *    asserts the consequence.
 *  - Support is checked against SpecReachesGround, an independent walk — a second
 *    opinion. "Support", not "connectivity": a piece under a grounded slab is
 *    joined to the ground and not held up by it.
 *  - Ground-reaction conservation: loads touching a grounded piece sum to exactly
 *    the weight of the supported ungrounded pieces. The weaker "no joint carries
 *    more than the total" had 2x slack on the triangle.
 *  - No joint is ever in tension. Static gravity presses a bed joint and slides a
 *    head joint; non-zero tension means the force was stored against the wrong end.
 *  - Solving never breaks a joint, over the whole matrix.
 *
 * Shapes include the pathological: a twenty-high chain, a cycle a naive walk could
 * loop or double-count, a structure with no ground, and the two-tier cases where
 * attachment to earth isn't resting on it.
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

	// The same two bricks declared upper piece first (ConnectionLoad.h allows it): the stored force must flip sign, or a compressed joint reads as tensile.
	Specs.Add({
		TEXT("a bed joint declared upper piece first"),
		{
			{ { BrickMassKg, true }, { BrickMassKg, false } },
			{ { 1, 0, InvertedBedJointNormal, JointAreaSqCm } }
		}
	});

	// Attached to earth but not held up by it: the piece hangs under a grounded slab, so its one bed joint is above it and bears nothing.
	Specs.Add({
		TEXT("a piece hanging beneath a grounded piece"),
		{
			{ { BrickMassKg, true }, { BrickMassKg, false } },
			{ { 1, 0, BedJointNormal, JointAreaSqCm } }
		}
	});

	// Mutual lateral support: each names the other as its head-joint support, neither reaching earth. An untracked support walk loops here.
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
	 * A piece held sideways by one grounded and one falling neighbour. Piece 2
	 * rests on floating piece 3 and reaches nothing, so piece 1's whole weight goes
	 * through the joint to piece 0; splitting evenly credits half to a falling piece
	 * and loses it, visible only to the conservation invariant.
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

	// A cycle: everything connected, one piece grounded. An untracked downward walk can loop here or double-count the same weight.
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

	// A twenty-high chain, so the base joint carries nineteen bricks: long enough that an accumulation error compounds visibly rather than hiding in tolerance.
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

	// The running-bond wall from LoadPath, here for conservation: all four ungrounded bricks must reach the two grounded ones, whatever route the solver takes through the keystone.
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

			// Never sideways: gravity does not change direction because a joint happens to be vertical.
			TestTrue(
				FString::Printf(TEXT("%s: connection %d load must be vertical, got (%f, %f, %f)"),
					Named.Description, Index, Force.X, Force.Y, Force.Z),
				FMath::IsNearlyZero(Force.X, Tolerance) && FMath::IsNearlyZero(Force.Y, Tolerance));

			/*
			 * And never upward, but "upward" is about the joint, not world Z: the
			 * stored force acts on PieceB, so the sign flips with declaration order
			 * while the classification doesn't. Non-zero tension is the
			 * declaration-independent way to say the load runs the wrong way.
			 *
			 * Scoped to these fixtures: every normal here is axis-aligned, so this is
			 * a property of the specs. The sign-blind head tier supports a piece in
			 * tension past 45 degrees (DESIGN.md §3), and a fuzz with tilted normals
			 * produced tension in 337 of 6000 structures. So the first tilted normal
			 * added here turns this row red as a discovery — characterise it, then
			 * scope this assertion to the axis-aligned rows rather than deleting it.
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
		 * Ground-reaction conservation: joints touching a grounded piece carry
		 * exactly the weight of the supported ungrounded pieces. The weaker "no joint
		 * carries more than the total" left 2x slack on the triangle. Relative
		 * tolerance, since one spec weighs 9.8e8 uu and another splits across areas
		 * twelve orders of magnitude apart.
		 */
		const double ConservationTolerance = FMath::Max(Tolerance, 1.0e-9 * TotalSupportedWeightUU);

		TestTrue(
			FString::Printf(
				TEXT("%s: the structure holds up %f but only %f reaches the ground"),
				Named.Description, TotalSupportedWeightUU, GroundReactionUU),
			FMath::IsNearlyEqual(GroundReactionUU, TotalSupportedWeightUU, ConservationTolerance));

		// Out-of-range handles fail closed rather than reading as a healthy, unloaded, grounded piece.
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
		 * The accessors themselves, documented to return a placeholder for an unknown
		 * handle. The failure they guard is silent: a placeholder reading as a real
		 * massless grounded piece, or a real zero-area joint, would let a caller walk
		 * off the array end and get a plausible answer instead of an obvious one.
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

			// Zero area matters as much as the unset handles: it makes the placeholder read as a failed joint, not an intact one, if a load goes through it.
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

		// Nothing supported means nothing carried, and vice versa: stops the matrix being satisfied by a solver that returns zero everywhere.
		TestTrue(
			FString::Printf(TEXT("%s: %f of weight is held up but the joints carry %f in total"),
				Named.Description, TotalSupportedWeightUU, TotalCarriedUU),
			(TotalSupportedWeightUU > Tolerance) == (TotalCarriedUU > Tolerance));
	}

	return true;
}

/*
 * A cycle in the support relation must not strand load silently.
 *
 * Accumulation orders pieces by Kahn's algorithm over "who rests on me". Pieces
 * in a cycle never become ready, so their joints keep a zero load and the weight
 * never reaches earth, while the reachability walk reports them supported: two
 * accessors on one solve, contradicting each other with no signal. The shape is a
 * bottom course of three bricks with the ground missing under two of them, the
 * cycle appearing once the gap is two bricks wide.
 *
 * The decision: pieces whose load the solver can't route to earth are reported
 * unsupported. This needs no new API, makes the two accessors agree (matching
 * Structure.h's "a connection with no path to ground reports zero"), and is the
 * fail-closed direction (DESIGN.md §3). It does not claim the cycle is solved —
 * dividing load round a loop needs a rule we don't have (CURRENT_STATE.md) — only
 * that the stall is visible from outside.
 *
 * Because that could change, the invariants below are written against what the
 * solver reports, not the expectation table:
 *
 *  - Every ungrounded piece called supported has at least its own weight on the
 *    joints touching it. Summing over every touching joint keeps this free of any
 *    tier-rule transcription — a lower bound whichever joints are the supports.
 *  - Ground reaction equals the weight of exactly the pieces the solver holds up.
 *    This also pins Structure.cpp writing ConnectionForces by assignment: seeding
 *    the cycle would turn that into an overwrite, the second piece's share
 *    clobbering the first's, which conservation notices.
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
		 * The control, minimum repro minus one brick: two bricks, the left grounded.
		 * Piece 1 has no bed joint and falls back to its one head joint onto earth —
		 * no cycle, since grounded pieces never enter the ordering. A fix that
		 * distrusted head joints would take this with it, and DESIGN.md §3's worked
		 * example rests on it.
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
		 * The minimum repro. Add one brick to that course:
		 *
		 *   G  —  X  —  Y      pieces 0, 1, 2
		 *   ==                 earth, under G only
		 *
		 * Neither X nor Y has a bed joint, so X's supports are {G, Y} and Y's are {X}.
		 * Each waits for the other, the ready set seeds empty, and no force is written
		 * — yet the reachability walk reports both supported. 5333 uu held up per one
		 * accessor, 0 uu reaching ground per the other.
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

		// The same stall at a longer stride, so a fix can't special-case adjacent pairs: a three-brick gap makes a cycle of three.
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
		 * The second control: a cycle that isn't stranded and must stay solved.
		 * Pieces 0 and 2 name each other, a genuine two-cycle, but piece 0 is grounded
		 * and never enters the ordering, so piece 2 becomes ready immediately.
		 * "Contains a cycle" is not "cannot be ordered".
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

		// The decision, asserted directly: a piece the solver cannot route to earth is not held up.
		for (int32 Index = 0; Index < Case.ExpectedSupported.Num(); ++Index)
		{
			TestTrue(
				FString::Printf(TEXT("%s: piece %d support, expected %d, got %d"),
					Case.Description, Index,
					Case.ExpectedSupported[Index] ? 1 : 0,
					Structure.IsPieceSupported(Index) ? 1 : 0),
				Structure.IsPieceSupported(Index) == Case.ExpectedSupported[Index]);
		}

		// From here on, everything is measured against what the solver reports, so it survives the decision above being revisited.
		double ReportedSupportedWeightUU = 0.0;

		for (int32 PieceIndex = 0; PieceIndex < Case.Spec.Pieces.Num(); ++PieceIndex)
		{
			if (!Structure.IsPieceSupported(PieceIndex) || Case.Spec.Pieces[PieceIndex].bIsGrounded)
			{
				continue;
			}

			const double OwnWeightUU = WeightOf(Case.Spec.Pieces[PieceIndex].MassKg);
			ReportedSupportedWeightUU += OwnWeightUU;

			// Every joint touching this piece, not just the tier rule's supports: a lower bound whichever subset is the real supports, needing no copy of the tier rule.
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
		 * Ground reaction against the solver's own claim: short means stranded, long
		 * means double-counted, and an assignment gone overwrite shows up as short by
		 * the clobbered share.
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

/*
 * Stranding does not propagate downward. A brick resting on the earth stands up
 * whatever is happening above it. Un-orderability is a solver artefact (DESIGN.md
 * §3): load that can't be routed is reported as falling only for the pieces caught
 * in the knot; pieces beneath keep their support and carry everything except the
 * unroutable contribution.
 *
 * The defect: the Kahn ordering runs top-down, so a piece comes out unordered when
 * a knot sits anywhere above it, not only when it's in one. Stranding every
 * unordered piece walks down to the first grounded piece and the next pass back up,
 * so one unroutable pair takes down four of five pieces. No existing test sees it
 * because over-stranding preserves conservation exactly (a 4000-case fuzz found 0
 * conservation violations but 14 wrongly-falling pieces), and every SupportCycle
 * and DegenerateInputs invariant is measured against what the solver reports. So
 * the missing control is an ungrounded piece bed-jointed to ground and
 * head-jointed to an unroutable pair, its support and bed-joint load asserted directly.
 *
 * The knot must also not shed weight downward: every case states the load beneath
 * it exactly, so pushing X and Y's weight into the pier reads 4 or 5 bricks where
 * the table says 2. The other-direction controls live in SupportCycle — narrowing
 * the stranded set must not widen it elsewhere.
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
		 * The repro, minimal.
		 *
		 *         Z  —  X  —  Y      pieces 1, 2, 3, joined sideways
		 *         |                  bed joint, Z on the pier
		 *      [ pier ]              piece 0, on the earth
		 *      =======
		 *
		 * X and Y have no bed joint: X's supports are {Z, Y}, Y's are {X}, the
		 * unroutable knot. Z is not in it — bed-jointed onto a grounded pier — but
		 * because X names Z, Z's pending-loader count never reaches zero, so
		 * un-orderability stranding strands Z and then anything resting on it. The
		 * pier joint carries Z's own weight only: X and Y are falling and hand nothing down.
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
		 * The same knot with a brick on the piece beneath it, where the cascade shows.
		 *
		 *               B                piece 4, resting on Z
		 *               |
		 *         Z  —  X  —  Y          pieces 1, 2, 3
		 *         |
		 *      [ pier ]                  piece 0
		 *      =======
		 *
		 * B is held up by Z, Z reaches earth, so B stands and the pier joint carries
		 * two bricks, not three or four — X and Y are falling and hand nothing down.
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
		 * Two courses beneath the knot, so a fix can't special-case the piece directly
		 * under it: stranding would walk down to the first grounded piece, two joints
		 * away here.
		 *
		 *        Zhigh — X — Y      pieces 2, 3, 4
		 *          |
		 *        Zlow               piece 1
		 *          |
		 *      [ pier ]             piece 0
		 *      =======
		 *
		 * Neither Zlow nor Zhigh is in the knot. The lower joint carries both, the
		 * upper carries Zhigh alone.
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

/*
 * Stranding does propagate upward. A piece whose only support is falling is
 * falling too, and its joint carries nothing.
 *
 * The other half of the rule, and the one thing needing the solve to iterate:
 * stranding a knot changes who reaches ground, which can strand a piece that
 * rested only on what was just stranded. Every other case settles in one pass, and
 * a single-pass solver was verified to reproduce every flag and force on all 26
 * structures the suite solves — so the loop could be deleted and stay green unless
 * a case like this exists.
 *
 *                       B          piece 3, resting on Y through a bed joint
 *                       |
 *      [ G ]  —  X  —   Y          pieces 0, 1, 2, joined sideways
 *      =====
 *
 *   ITERATING     supported = [T,F,F,F]   forces = [0, 0, 0]
 *   SINGLE-PASS   supported = [T,F,F,T]   forces = [0, 0, -2667.2]
 *
 * The single-pass answer is self-contradictory: it calls B held up while writing
 * B's weight onto a joint whose far end it calls falling. B is ordered first (the
 * only leaf), so the case needs something above the knot to see this. Case 2 adds
 * the second claim — Z, beneath the knot, keeps its support and pier-joint weight
 * while B loses both — failing a single-pass solver and an over-eager stranding
 * rule in opposite directions. Run these two with the loop removed before doubting
 * they discriminate it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureStrandingPropagatesUpwardTest,
	"DestructionGame.Core.Structure.StrandingPropagatesUpward",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureStrandingPropagatesUpwardTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	const TArray<FSolveCase> Cases = {
		// Nothing beneath the knot: G is grounded, so the whole course goes. B rests on Y in the knot with no other support, so B falls and its joint reports zero, not a brick on its way down.
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
		 * Both rules at once, the only case stating them separately: Z beneath the
		 * knot keeps its support; B above it loses its own.
		 *
		 *                         B      piece 4, resting on Y
		 *                         |
		 *         Z  —  X  —      Y      pieces 1, 2, 3
		 *         |
		 *      [ pier ]                  piece 0
		 *      =======
		 *
		 * Downward: the pier joint carries Z's one brick. Upward: B is held up only by
		 * Y, so it falls and the Y-B joint is unloaded — worse to get wrong than the
		 * case above, since this structure has a standing part to confuse it with.
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

/*
 * Why a piece has no support, not merely whether. IsPieceSupported answers
 * "whether", and two things produce the same false:
 *
 *   - Physics: nothing holds the piece up; its supports are gone or falling.
 *   - A solver limitation: the piece is in an unroutable knot, where there's no
 *     rule for dividing load round a loop, so the solver conservatively calls it
 *     falling (DESIGN.md §3) — not physics.
 *
 * Why an accessor: DESIGN.md §4's collapse test ("pull bricks until it topples,
 * confirm it falls at the predicted number") reads identically in both cases, so
 * it could be calibrated against the removal count that makes a cycle rather than
 * overloads a joint. It needs a precondition it can't express — no piece was
 * stranded when it fell — and this is that seam.
 *
 * Stranded is only for pieces in the knot: a piece resting only on a knot comes out
 * Falling, having simply lost what carried it. Production already computes this set
 * (the fixpoint strands on "does my load come back to me"), so the distinction
 * falls out. Every case mirrors an existing fixture and must agree with it on
 * IsPieceSupported (CheckSupportAgreesWithReason); only the reason column is new.
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
		// SupportCycle's first control. Piece 1 has no bed joint and falls back to its one head joint onto earth; nothing is in a knot, so the fallback reads as ordinary support.
		{
			TEXT("a brick held sideways by a grounded neighbour is SUPPORTED, not stranded"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false } },
				{ { 0, 1, HeadJointNormal, JointAreaSqCm } }
			},
			{ EPieceSupport::Grounded, EPieceSupport::Supported }
		},

		/*
		 * SupportCycle's minimum repro, the case this accessor exists for:
		 *
		 *   G  —  X  —  Y      pieces 0, 1, 2
		 *   ==                 earth, under G only
		 *
		 * X's supports are {G, Y}, Y's are {X}, so both are in the knot.
		 * IsPieceSupported reports them falling but can't say the reason is the solver.
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

		// The same stall at a longer stride, so the reason can't special-case adjacent pairs: all three pieces of a three-cycle are in it.
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

		// SupportCycle's second control: a genuine cycle (pieces 0 and 2), but piece 0 is grounded and never enters the ordering, so everything resolves. "Contains a cycle" is not "is stranded".
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
		 * StrandingIsLocal's repro. Z is beneath the knot, keeping its support; the
		 * knot above it is X and Y and nothing else.
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

		// The same knot with a brick on the piece beneath it. B is held up by Z, which reaches earth, so B is Supported while X and Y are stranded two joints away: the reason must be per piece.
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

		// Two courses beneath the knot: a reason walking down to the first grounded piece would report Zlow and Zhigh stranded, though both are standing.
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
		 * Stranded versus Falling, the distinction IsPieceSupported can't make.
		 * StrandingPropagatesUpward's first case:
		 *
		 *                       B          piece 3, resting on Y through a bed joint
		 *                       |
		 *      [ G ]  —  X  —   Y          pieces 0, 1, 2
		 *      =====
		 *
		 * X and Y are in the knot; B is not, having simply lost the only thing holding
		 * it up. IsPieceSupported says false for all three; only this accessor says two
		 * are the solver's answer and one the structure's.
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

		// All four states in one structure, stopping any two of them being conflated: pier Grounded, Z beneath the knot Supported, X/Y Stranded, B on top Falling.
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

	// A floor on the table itself, so a case list that lost its only Falling row fails rather than passing in silence.
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

		// A grounded piece is never merely Supported, and nothing off the earth reads Grounded. Asserted against the spec, not the expectation column, so a row with the two swapped is caught by the fixture.
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

/*
 * The reason has the same scope as the answer it explains, and fails closed
 * everywhere else. GetPieceSupport reads solver output as IsPieceSupported does, so
 * it inherits that contract. RemovedPieceSupportNeedsASolve pins these same three
 * rows for the boolean, and the reason must not disagree with it for an instant:
 *
 *     never solved      Falling, for every handle — there is no answer yet
 *     removed           the last solve's answer, unchanged, until the next solve
 *     after that solve  Falling, and a removed grounded piece is no longer earth
 *
 * No fifth "removed" enumerator: it would have to be produced before the re-solve,
 * contradicting a stale Supported from IsPieceSupported in the same instant —
 * exactly the two-accessors-one-solve defect the stranding rule closes. A removed
 * piece folds into Falling (nothing holds it up), and IsPieceRemoved stays the
 * accessor for whether it's a piece at all. An out-of-range handle is Falling for
 * the same fail-closed reason; Stranded would be a positive claim about a knot in a
 * structure that doesn't exist.
 *
 * A matrix, because the property is that none of these read Supported or Grounded
 * for something that isn't there, indistinguishable downstream from a fine piece.
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

	// Before any solve there is no answer, and Falling is the fail-closed one. The state array is sized by a solve, so an early caller is told nothing is held up rather than handed a default that reads as resting on earth.
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

	// Out-of-range handles against a solved, standing structure: an accessor running off the end of a populated array would have real Grounded/Supported entries next to it to pick up.
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

	// Removing the brick. The gone piece's reason doesn't move until a re-solve, then it's Falling, like an unknown handle: it's not a piece any more.
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

		// The stale row, deciding against a fifth enumerator: the piece is provably gone (IsPieceRemoved), yet the reason still reads the last solve's Supported, in lockstep with IsPieceSupported.
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
	 * Removing the ground, the removal with the furthest reach: a grounded piece
	 * seeds the reachability walk on its own account, so a removed one still reading
	 * Grounded would claim an earth it isn't touching, and unlike a bare false the
	 * reason states that claim out loud.
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

	// A removed piece inside a knot is Falling, not Stranded, where the two decisions above meet. Pull X out of a two-brick knot and Y is left head-jointed to a gone piece: the knot is dissolved.
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

/*
 * A solved structure can be asked how loaded each joint is, without the question
 * damaging anything. The companion to Connection.UtilisationQuery: FConnection owns
 * the non-mutating evaluator, and this applies it to the force the last solve
 * routed through each joint — a colour per connection, every frame, phase 5 needs.
 *
 * The number is already obtainable by composing ClassifyForce and
 * ComputeUtilisation over GetConnectionForce, so this accessor exists to stop a
 * renderer becoming a third hand-copy of the break decision. The identity
 *
 *     GetConnectionUtilisation(I)
 *         == GetConnection(I).UtilisationUnder(GetConnectionForce(I), GetConnectionMoment(I))
 *
 * is asserted bitwise for every joint, alongside expectations derived from stress
 * and strength — the identity alone would pass on two consistent wrong answers, the
 * derived numbers alone wouldn't catch a private copy drifting an ulp away. The
 * moment argument is why there's an eccentric fixture at the end: it's defaulted,
 * so an identity written with the force alone supplies zero, and on a geometry-free
 * fixture the accessor's moment is zero too — coincidence, not agreement.
 *
 * Gravity only, no world, so this is a unit test on the mechanism (a ratio). Which
 * axis governs, worked since ComputeUtilisation returns the worst of three:
 *
 *   - The two bed joints take a vertical load through a normal exactly +Z, so
 *     compression is the only non-zero axis.
 *   - The head joint takes the same load through a normal exactly +X, so its shear
 *     capacity is bare cohesion (0.2 MPa, compressive stress being zero).
 *
 * The head joint and upper bed joint carry one brick each, and their expectations
 * differ by exactly a hundred: fifty from mortar resisting crushing fifty times
 * better than sliding (10 MPa against 0.2), two from the bed joint's double area.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureConnectionUtilisationTest,
	"DestructionGame.Core.Structure.ConnectionUtilisation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureConnectionUtilisationTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	/** The bit pattern of a double, for the assertions that compare EXACTLY. */
	const auto Bits = [](double Value)
	{
		uint64 Raw = 0;
		FMemory::Memcpy(&Raw, &Value, sizeof(Raw));
		return FString::Printf(TEXT("%.17g [%016llx]"), Value, Raw);
	};

	const double MortarCompressiveMPa = GeneralPurposeMortar.CompressiveStrengthMPa;
	const double MortarCohesionMPa = GeneralPurposeMortar.ShearCohesionMPa;

	// Relative, with an absolute floor for exact-zero rows. Non-zero expectations are around 1e-4, so a joint dropped from the load path (exactly zero) is still distinguishable from a lightly loaded one.
	const auto IsClose = [](double Actual, double Expected)
	{
		return FMath::IsNearlyEqual(Actual, Expected, FMath::Max(1.0e-15, FMath::Abs(Expected) * 1.0e-9));
	};

	/*
	 * A standing structure, three joints, three different answers.
	 *
	 *        [2]                  piece 2 rests on 1 through a 200 cm2 bed joint
	 *         |
	 *        [1]---[3]            piece 3 has no bed joint at all, so it hangs off
	 *         |                   piece 1's 100 cm2 HEAD joint and loads it in shear
	 *        [0] grounded         piece 1 rests on 0 through a 100 cm2 bed joint
	 *
	 * Three distinct utilisations, spanning two governing axes and a factor of a
	 * hundred, so an accessor returning one number per joint (or per joint kind)
	 * cannot pass.
	 */
	{
		const FStructureSpec Spec = {
			{ { BrickMassKg, true }, { BrickMassKg }, { BrickMassKg }, { BrickMassKg } },
			{
				{ 0, 1, BedJointNormal, JointAreaSqCm },
				{ 1, 2, BedJointNormal, JointAreaSqCm * 2.0 },
				{ 1, 3, HeadJointNormal, JointAreaSqCm },
			}
		};

		FStructure Structure;
		BuildStructure(Structure, Spec, GeneralPurposeMortar);

		// Before any solve there is no routed load, so every joint reads zero: the same scope GetConnectionForce documents, since this is that force evaluated.
		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const double Unsolved = Structure.GetConnectionUtilisation(Index);

			TestTrue(
				FString::Printf(TEXT("before a solve joint %d carries no load, so it reads 0, got %s"),
					Index, *Bits(Unsolved)),
				Unsolved == 0.0);
		}

		Structure.SolveLoads();

		struct FExpectedUtilisation
		{
			const TCHAR* Description;
			double ForceZUU;
			double Utilisation;
		};

		// Derived from stress against strength, with MPaForForce spelling out the conversion rather than importing it, so a wrong ForceUnitsPerMPaSqCm fails here. At 2667.168 uu a brick over 100 cm2 is 2.667168e-3 MPa.
		const TArray<FExpectedUtilisation> Expected = {
			{
				TEXT("the bottom bed joint carries three bricks in compression"),
				-3.0 * BrickWeightUU,
				MPaForForce(3.0 * BrickWeightUU, JointAreaSqCm) / MortarCompressiveMPa
			},
			{
				TEXT("the upper bed joint carries one brick over twice the area"),
				-BrickWeightUU,
				MPaForForce(BrickWeightUU, JointAreaSqCm * 2.0) / MortarCompressiveMPa
			},
			{
				TEXT("the head joint carries one brick in SHEAR, against bare cohesion"),
				-BrickWeightUU,
				MPaForForce(BrickWeightUU, JointAreaSqCm) / MortarCohesionMPa
			},
		};

		TestEqual(TEXT("fixture precondition: three joints were built"),
			Structure.NumConnections(), Expected.Num());

		for (int32 Index = 0; Index < Expected.Num(); ++Index)
		{
			const FConnection& Connection = Structure.GetConnection(Index);
			const FVector Force = Structure.GetConnectionForce(Index);

			// Precondition: the solve routed what this case assumes it routed.
			TestTrue(
				FString::Printf(TEXT("%s: joint %d should carry Z = %f, got %f"),
					Expected[Index].Description, Index, Expected[Index].ForceZUU, Force.Z),
				FMath::IsNearlyEqual(Force.Z, Expected[Index].ForceZUU, 1.0e-6));

			const double Utilisation = Structure.GetConnectionUtilisation(Index);

			TestTrue(
				FString::Printf(TEXT("%s: joint %d should be at utilisation %.12e, got %s"),
					Expected[Index].Description, Index, Expected[Index].Utilisation, *Bits(Utilisation)),
				IsClose(Utilisation, Expected[Index].Utilisation));

			/*
			 * The seam. Bitwise, because this accessor must not be a second
			 * evaluation of the same arithmetic. The moment is passed explicitly:
			 * UtilisationUnder's moment parameter is defaulted, so an assertion
			 * without it supplies zero, and on this geometry-free fixture the
			 * accessor's moment is zero too, so the identity would hold trivially
			 * however far the two had drifted. Bitten on below, where a joint carries a
			 * real one. Composite depth likewise, opposite polarity: it's a relief, so
			 * dropping it reads too pessimistic where dropping the moment reads too
			 * optimistic.
			 */
			const FVector Moment = Structure.GetConnectionMoment(Index);
			const double CompositeDepthCm = Structure.GetConnectionCompositeDepthCm(Index);

			TestTrue(
				FString::Printf(
					TEXT("%s: joint %d is geometry-free and must carry no moment and no composite depth, got (%f, %f, %f) and %f"),
					Expected[Index].Description, Index, Moment.X, Moment.Y, Moment.Z,
					CompositeDepthCm),
				Moment.IsZero() && CompositeDepthCm == 0.0);

			TestTrue(
				FString::Printf(
					TEXT("%s: joint %d must be exactly UtilisationUnder(GetConnectionForce, GetConnectionMoment, GetConnectionCompositeDepthCm), %s vs %s"),
					Expected[Index].Description, Index,
					*Bits(Utilisation),
					*Bits(Connection.UtilisationUnder(Force, Moment, CompositeDepthCm))),
				Utilisation == Connection.UtilisationUnder(Force, Moment, CompositeDepthCm));

			// Asking must not have broken anything: solving is non-destructive and so is reading.
			TestFalse(
				FString::Printf(TEXT("%s: joint %d must still be intact after being asked"),
					Expected[Index].Description, Index),
				Connection.HasGiven());
		}
	}

	/*
	 * A broken joint reads zero, and the zero comes from the load, not the latch.
	 * UtilisationUnder is pure arithmetic, so the zero here means "no force goes
	 * through this joint any more". A renderer must consult HasGiven and GetBreakPass
	 * to draw a broken joint as broken; a low ratio never means intact.
	 */
	{
		// 20 tonnes on a 100 cm2 mortar bed joint: 1.96e7 uu is 19.6 MPa against mortar's 10 MPa, so utilisation 1.96 and the joint gives in pass 1.
		constexpr double CrushingMassKg = 20000.0;

		const FStructureSpec Spec = {
			{ { BrickMassKg, true }, { CrushingMassKg } },
			{ { 0, 1, BedJointNormal, JointAreaSqCm } }
		};

		FStructure Structure;
		BuildStructure(Structure, Spec, GeneralPurposeMortar);

		const FVector BreakingForce(0.0, 0.0, -WeightOf(CrushingMassKg));
		const double BreakingUtilisation =
			MPaForForce(WeightOf(CrushingMassKg), JointAreaSqCm) / MortarCompressiveMPa;

		Structure.SolveLoads();

		const double BeforeBreaking = Structure.GetConnectionUtilisation(0);

		TestTrue(
			FString::Printf(TEXT("fixture precondition: the joint is over capacity at %.12e, got %s"),
				BreakingUtilisation, *Bits(BeforeBreaking)),
			IsClose(BeforeBreaking, BreakingUtilisation) && BreakingUtilisation > 1.0);

		/*
		 * Solving and reading break nothing. If asking for the utilisation
		 * had latched, this cascade would find the joint already given and
		 * report zero passes — the fail-open direction, a wall that never falls.
		 */
		TestFalse(TEXT("reading a utilisation must not break the joint"),
			Structure.GetConnection(0).HasGiven());

		TestEqual(TEXT("the cascade must break the joint in one pass"), Structure.SolveAndBreak(), 1);

		TestTrue(TEXT("fixture precondition: the joint has given"), Structure.GetConnection(0).HasGiven());
		TestEqual(TEXT("fixture precondition: it was stamped pass 1"), Structure.GetBreakPass(0), 1);

		const FVector AfterBreaking = Structure.GetConnectionForce(0);

		TestTrue(
			FString::Printf(TEXT("a broken joint carries no force, got (%f, %f, %f)"),
				AfterBreaking.X, AfterBreaking.Y, AfterBreaking.Z),
			AfterBreaking.IsZero());

		const double Broken = Structure.GetConnectionUtilisation(0);

		TestTrue(
			FString::Printf(TEXT("a broken joint is at zero utilisation because it carries nothing, got %s"),
				*Bits(Broken)),
			Broken == 0.0);

		// The discriminator between the two readings of that zero: the joint still answers what the crushing load would do. If the latch suppressed the arithmetic this would be zero too, and the two designs would be indistinguishable.
		const double WhatWouldHaveBrokenIt = Structure.GetConnection(0).UtilisationUnder(BreakingForce);

		TestTrue(
			FString::Printf(
				TEXT("a given joint still answers what the load WOULD do, expected %.12e, got %s"),
				BreakingUtilisation, *Bits(WhatWouldHaveBrokenIt)),
			IsClose(WhatWouldHaveBrokenIt, BreakingUtilisation));
	}

	/*
	 * A joint under a moment, the only fixture that makes the seam above mean
	 * anything: every fixture above is geometry-free, so its moment is zero and the
	 * identity holds even if the moment were deleted from Structure.cpp. A joint
	 * carrying a real one is the only thing that can tell those apart.
	 *
	 *     [1]  a brick, its weight acting 2 cm along +X of the joint's own centroid
	 *      |
	 *     [0]  grounded pad, one 10 x 10 cm bed joint between them
	 *
	 * Exactly one support, the only determinate case: a piece on several supports
	 * keeps the area split and carries no moment (MOMENTS_DESIGN.md's N >= 2 rule).
	 *
	 * The arithmetic, from brick dimensions and published strengths:
	 *
	 *   W       = 1.9 g/cm3 x 21.5 x 10.25 x 6.5 cm / 1000 x 980 cm/s2 = 2667.198625 uu
	 *   M       = (p - c) x F, so |M| = 2 cm x W                       = 5334.39725 uu.cm
	 *   W_sec   = (4/3) x 5 x 5^2                                      = 166.666... cm3
	 *   sigma_b = |M| / W_sec                                          = 32.0063835 uu/cm2
	 *   sigma_n = W / 100 cm2, in COMPRESSION so it closes the joint    = 26.67198625 uu/cm2
	 *   opened edge   = sigma_b - sigma_n = 5.33439725 uu/cm2 = 5.33439725e-4 MPa
	 *   squeezed edge = sigma_b + sigma_n = 58.678369  uu/cm2 = 5.8678369e-3 MPa
	 *
	 * where 10,000 uu per MPa.cm2 comes from MPaForForce spelling out 1 N = 100 uu
	 * and 1 cm2 = 100 mm2 rather than reading production's constant.
	 *
	 * Which axis governs, worked for all three since ComputeUtilisation returns the
	 * worst:
	 *
	 *   tension     5.33439725e-4 / 0.10 MPa = 5.33439725e-3   <- governs
	 *   compression 5.8678369e-3  / 10.0 MPa = 5.8678369e-4
	 *   shear       exactly zero: the load is vertical and the normal is exactly +Z
	 *
	 * So the moment moves this joint from 2.6671986250e-4 to 5.33439725e-3, a factor
	 * of exactly 20. That gap makes the seam assertion bite: an accessor without the
	 * moment lands on the first number, the break decision on the second.
	 */
	{
		/** Half of a 10 x 10 cm face, so 4 x 5 x 5 = 100 cm2 exactly matches the area. */
		constexpr double JointHalfCm = 5.0;

		/** How far the brick's weight acts from the centroid of the face carrying it. */
		constexpr double EccentricityCm = 2.0;

		/** (4/3) x h_u x h_v^2 for that face — beam theory, not a code figure. */
		constexpr double SectionModulusCm3 =
			(4.0 / 3.0) * JointHalfCm * JointHalfCm * JointHalfCm;

		const FVector JointCentreCm(0.0, 0.0, 0.0);
		const FVector BrickCentreOfMassCm(EccentricityCm, 0.0, 10.0);

		FStructure Structure;

		const int32 Pad = Structure.AddPiece(BrickMassKg, true, FVector(0.0, 0.0, -10.0));
		const int32 Brick = Structure.AddPiece(BrickMassKg, false, BrickCentreOfMassCm);

		FConnectionSpec JointSpec;
		JointSpec.PieceA = Pad;
		JointSpec.PieceB = Brick;
		JointSpec.Normal = BedJointNormal;
		JointSpec.AreaSqCm = JointAreaSqCm;
		JointSpec.CentreCm = JointCentreCm;
		JointSpec.HalfExtentCm = BedJointHalfExtentCm;

		const int32 Joint = Structure.AddConnection(MakeConnection(JointSpec, GeneralPurposeMortar));

		TestTrue(
			FString::Printf(TEXT("fixture: the eccentric joint should have been accepted, got handle %d"), Joint),
			Joint != INDEX_NONE);

		TestTrue(
			TEXT("fixture: every piece and every joint here knows where it is, so the geometry is complete"),
			Structure.HasCompleteGeometry());

		Structure.SolveLoads();

		const FVector Force = Structure.GetConnectionForce(Joint);

		// Precondition: the solve routed one brick straight down. PieceB is the loaded brick, so the stored force acts on it, downward (ConnectionLoad.h).
		TestTrue(
			FString::Printf(TEXT("fixture: the joint should carry one brick downward, got (%f, %f, %f)"),
				Force.X, Force.Y, Force.Z),
			FMath::IsNearlyEqual(Force.Z, -BrickWeightUU, 1.0e-6)
				&& FMath::IsNearlyEqual(Force.X, 0.0, 1.0e-9)
				&& FMath::IsNearlyEqual(Force.Y, 0.0, 1.0e-9));

		// (p - c) x F, derived here: p - c is (2, 0, 10), F is (0, 0, -W), so the cross product is (0, 2W, 0), a moment about world Y bending the face across X. No torsion (the component about the normal) here.
		const FVector ExpectedMoment =
			FVector::CrossProduct(BrickCentreOfMassCm - JointCentreCm, Force);

		const FVector Moment = Structure.GetConnectionMoment(Joint);

		TestTrue(
			FString::Printf(
				TEXT("the joint must report the moment its load path puts on it, (p-c) x F = (%.9g, %.9g, %.9g), got (%.9g, %.9g, %.9g)"),
				ExpectedMoment.X, ExpectedMoment.Y, ExpectedMoment.Z,
				Moment.X, Moment.Y, Moment.Z),
			Moment.Equals(ExpectedMoment, FMath::Abs(EccentricityCm * BrickWeightUU) * 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("fixture: the expected moment must not itself be zero, or this block asserts nothing; it is (%.9g, %.9g, %.9g)"),
				ExpectedMoment.X, ExpectedMoment.Y, ExpectedMoment.Z),
			!ExpectedMoment.IsNearlyZero());

		/* The opened edge, against mortar's flexural bond strength. Tension governs. */
		const double BendingMPa =
			MPaForForce(BrickWeightUU * EccentricityCm / SectionModulusCm3, 1.0);
		const double NormalMPa = MPaForForce(BrickWeightUU, JointAreaSqCm);

		const double ExpectedTension = (BendingMPa - NormalMPa) / GeneralPurposeMortar.TensileStrengthMPa;
		const double ExpectedCompression =
			(BendingMPa + NormalMPa) / GeneralPurposeMortar.CompressiveStrengthMPa;

		TestTrue(
			FString::Printf(
				TEXT("fixture: the OPENED edge must be the governing axis, tension %.12e against compression %.12e"),
				ExpectedTension, ExpectedCompression),
			ExpectedTension > ExpectedCompression);

		const double Utilisation = Structure.GetConnectionUtilisation(Joint);

		TestTrue(
			FString::Printf(
				TEXT("an eccentrically loaded joint should read %.12e, got %s"),
				ExpectedTension, *Bits(Utilisation)),
			IsClose(Utilisation, ExpectedTension));

		/*
		 * The seam, in full: bitwise against the evaluator handed all three parts of
		 * what the solve routed. This joint has no composite depth, asserted rather
		 * than assumed (nothing rests on the brick), which also keeps the factor of
		 * twenty below intact — a deep beam here would relieve the moment the row
		 * exists to show being carried.
		 */
		const double CompositeDepthCm = Structure.GetConnectionCompositeDepthCm(Joint);

		TestTrue(
			FString::Printf(
				TEXT("fixture: nothing rests on the eccentric brick, so its joint must carry no composite depth; it carries %g"),
				CompositeDepthCm),
			CompositeDepthCm == 0.0);

		TestTrue(
			FString::Printf(
				TEXT("joint %d must be exactly UtilisationUnder(GetConnectionForce, GetConnectionMoment, GetConnectionCompositeDepthCm), %s vs %s"),
				Joint, *Bits(Utilisation),
				*Bits(Structure.GetConnection(Joint).UtilisationUnder(Force, Moment, CompositeDepthCm))),
			Utilisation
				== Structure.GetConnection(Joint).UtilisationUnder(Force, Moment, CompositeDepthCm));

		// The half that proves the seam isn't vacuous: on the force alone the same joint reads a twentieth of the break-decision number. Were they equal, the assertion above would pass on an accessor ignoring the moment.
		const double WithoutTheMoment = Structure.GetConnection(Joint).UtilisationUnder(Force);

		TestTrue(
			FString::Printf(
				TEXT("the moment must be load-bearing: with it the joint reads %s, without it %s, and those must differ"),
				*Bits(Utilisation), *Bits(WithoutTheMoment)),
			Utilisation != WithoutTheMoment);

		TestTrue(
			FString::Printf(
				TEXT("and by the factor the arithmetic predicts: %.12e against a moment-free %.12e"),
				Utilisation, WithoutTheMoment),
			IsClose(WithoutTheMoment, NormalMPa / GeneralPurposeMortar.CompressiveStrengthMPa));

		// Asking must not have broken it: reading stays non-destructive here too.
		TestFalse(TEXT("reading an eccentric joint's moment must not break it"),
			Structure.GetConnection(Joint).HasGiven());
	}

	/*
	 * A handle that names no joint fails closed: TNumericLimits<double>::Max(), not
	 * zero. Zero is "unloaded and healthy", the answer that must never come back for
	 * something that isn't a joint (the DESIGN.md §2 degenerate-normal hole). A
	 * renderer handed a stale handle paints it failing, not the healthiest joint on
	 * screen. Falls out of the composition: GetConnection returns a placeholder whose
	 * zero interface area routes through the guard that already fails closed.
	 */
	{
		const FStructureSpec Spec = {
			{ { BrickMassKg, true }, { BrickMassKg } },
			{ { 0, 1, BedJointNormal, JointAreaSqCm } }
		};

		FStructure Structure;
		BuildStructure(Structure, Spec, GeneralPurposeMortar);
		Structure.SolveLoads();

		const TArray<int32> BadHandles = { INDEX_NONE, -7, Structure.NumConnections(), 999999 };

		for (const int32 Handle : BadHandles)
		{
			const double Utilisation = Structure.GetConnectionUtilisation(Handle);

			TestTrue(
				FString::Printf(TEXT("handle %d names no joint and must read as failed, got %s"),
					Handle, *Bits(Utilisation)),
				Utilisation == TNumericLimits<double>::Max());

			/*
			 * The moment fails closed the other way, not inconsistently: a moment is a
			 * load, not a verdict, so zero says "nothing levers this", the conservative
			 * answer for a non-joint — the utilisation above is what must read failed. A
			 * Max() moment would be an invented load, NaN everywhere downstream.
			 */
			const FVector Moment = Structure.GetConnectionMoment(Handle);

			TestTrue(
				FString::Printf(TEXT("handle %d names no joint and must carry no moment, got (%f, %f, %f)"),
					Handle, Moment.X, Moment.Y, Moment.Z),
				Moment.IsZero());
		}
	}

	return true;
}

/*
 * A pair that doesn't name a real joint on a real piece has no tier and reads
 * None. The oracle is Structure.h's contract, word for word: "None for a handle
 * naming no connection, for a piece not on this connection, and for a normal that
 * won't normalise." This matrix walks the first two clauses, separately.
 *
 * The third is unreachable through this door, and saying so is the point:
 * AddConnection refuses a connection whose normal won't normalise, so no stored
 * connection has one. Nor does any row trip two clauses at once — INDEX_NONE
 * against INDEX_NONE looks like it should, but the unknown-handle guard is first
 * and returns before the piece is compared, which makes it the sharp row: an
 * implementation reaching for a placeholder connection finds one whose PieceB is
 * also INDEX_NONE and lets the two unidentified things agree.
 *
 * No caller reaches this today (SolveLoads iterates real indices), but the
 * presenter is being written in a shape that can: FJointInspection and FPieceRef
 * both default their indices to INDEX_NONE, so GetJointRole against a default row
 * and an unresolved ref is a call somebody will write without noticing.
 *
 * Failing open here fails to the strongest answer: BedBeneath is the top support
 * tier (DESIGN.md §3), so a degenerate pair answered BedBeneath reports a brick
 * held up by a joint that doesn't exist — the one answer a readout must never draw
 * as reassuring. The last two rows are load-bearing: returning None unconditionally
 * passes every other row, so without a real joint asked from both ends (BedAbove
 * from below, BedBeneath from above) the cheapest wrong fix would pass.
 *
 * Pure geometry, no solve: GetJointRole reads a normal and two handles.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureJointRoleDegenerateInputTest,
	"DestructionGame.Core.Structure.JointRoleDegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureJointRoleDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	/*
	 * A grounded pad with a brick on it, one bed joint at index 0: the smallest
	 * structure with a real joint to be wrong about. A matrix against an empty
	 * structure couldn't tell "None because degenerate" from "None because there's
	 * nothing here", and the last two rows would have nothing to assert.
	 */
	const FStructureSpec PadAndBrick = {
		{ { BrickMassKg, true }, { BrickMassKg, false } },
		{ { 0, 1, BedJointNormal, JointAreaSqCm } }
	};

	FStructure Structure;
	BuildStructure(Structure, PadAndBrick, Unbreakable);

	TestTrue(TEXT("the fixture must have exactly the two pieces and the one joint it claims"),
		Structure.NumPieces() == 2 && Structure.NumConnections() == 1);

	struct FRoleCase
	{
		const TCHAR* Description;
		int32 ConnectionIndex;
		int32 PieceIndex;
		EJointRole Expected;
	};

	const TArray<FRoleCase> Cases = {
		/*
		 * The pair the presenter will produce, and the sharpest row: a default
		 * FJointInspection's connection index against a default FPieceRef's piece
		 * index. Both are INDEX_NONE, so an implementation reaching for a placeholder
		 * connection finds one whose PieceB is also INDEX_NONE, the fail-open
		 * direction (the hazard FStructureBinding::ResolvePiece is named after).
		 */
		{ TEXT("INDEX_NONE against INDEX_NONE"), INDEX_NONE, INDEX_NONE, EJointRole::None },

		// The same trap via the other two shapes of unknown handle, so a guard written only against INDEX_NONE doesn't close the matrix.
		{ TEXT("a handle past the last joint, against INDEX_NONE"), 99, INDEX_NONE, EJointRole::None },
		{ TEXT("a negative handle, against INDEX_NONE"), -5, INDEX_NONE, EJointRole::None },

		// An unknown joint asked about a real piece: clause one stands alone here.
		{ TEXT("a handle past the last joint, against a real piece"), 99, 0, EJointRole::None },
		{ TEXT("INDEX_NONE, against a real piece"), INDEX_NONE, 0, EJointRole::None },

		// Clause two alone: a real, well-formed joint asked about a piece it doesn't touch.
		{ TEXT("a real joint, against a piece that is not on it"), 0, 99, EJointRole::None },
		{ TEXT("a real joint, against INDEX_NONE"), 0, INDEX_NONE, EJointRole::None },

		// The two rows that stop the lazy fix: the same joint from both ends. The normal points at the brick, so it's bed-beneath the brick and bed-above the pad; answering None unconditionally fails both.
		{ TEXT("the real joint, from the pad it sits on"), 0, 0, EJointRole::BedAbove },
		{ TEXT("the real joint, from the brick it holds up"), 0, 1, EJointRole::BedBeneath },
	};

	for (const FRoleCase& Case : Cases)
	{
		const EJointRole Role = Structure.GetJointRole(Case.ConnectionIndex, Case.PieceIndex);

		TestTrue(
			FString::Printf(
				TEXT("%s: GetJointRole(%d, %d) should be %s, got %s"),
				Case.Description, Case.ConnectionIndex, Case.PieceIndex,
				NameOfJointRole(Case.Expected), NameOfJointRole(Role)),
			Role == Case.Expected);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
