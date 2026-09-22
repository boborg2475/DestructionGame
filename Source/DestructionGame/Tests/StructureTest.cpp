// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Structure.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named, not anonymous: anonymous namespaces collide under a unity build.
namespace StructureTestSupport
{
	using namespace DestructionProfiles;

	/*
	 * Unreal's default gravity, 980 cm/s2, spelled out independently of production.
	 * MassKg * 980 is already a force in uu; the 1 N = 100 uu factor is baked in.
	 * Multiplying by 100 again would be wrong by exactly 100x.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double WeightOf(double MassKg)
	{
		return MassKg * GravityCmPerSecondSquared;
	}

	/** A standard UK metric brick, 215 x 102.5 x 65 mm = 1432.44 cm3. */
	constexpr double BrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/*
	 * Clay brick mass derived from the profile: 1432.44 cm3 at 1.9 g/cm3 = 2.7216 kg
	 * (DESIGN.md §3). Safe at namespace scope only because ClayBrick is
	 * constant-initialised; otherwise cross-TU init order could make this zero.
	 * LoadPath's opening guard checks it.
	 */
	const double BrickMassKg = ClayBrick.DensityGramsPerCubicCm * BrickVolumeCubicCm / 1000.0;

	/** 2667.2 uu. */
	const double BrickWeightUU = WeightOf(BrickMassKg);

	/** A 10 cm x 10 cm interface. */
	constexpr double JointAreaSqCm = 100.0;

	/** Bed joint: normal up, toward PieceB (ConnectionLoad.h), so PieceB sits on PieceA. */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Head joint: vertical interface, normal sideways. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);
	const FVector OpposingHeadJointNormal(-1.0, 0.0, 0.0);

	/** A bed joint declared upper piece first, so the normal points down. */
	const FVector InvertedBedJointNormal(0.0, 0.0, -1.0);

	// Half-extents matching JointAreaSqCm (4 x 5 x 5 = 100), zero on the normal's axis.
	const FVector BedJointHalfExtentCm(5.0, 5.0, 0.0);
	const FVector HeadJointHalfExtentCm(0.0, 5.0, 5.0);
	const FVector DepthwiseHalfExtentCm(5.0, 0.0, 5.0);

	/*
	 * The bed/head threshold, 45 degrees from vertical (DESIGN.md §3).
	 * Same literal as Structure.cpp, bit for bit: 1.0/Sqrt(2.0) lands an ulp away.
	 * If production changes, match it rather than re-deriving it.
	 */
	constexpr double BedJointCosine = 0.70710678118654752440;

	/** A normal tilted this many degrees away from straight up, in the XZ plane. */
	FVector NormalTiltedFromVertical(double Degrees)
	{
		const double Radians = FMath::DegreesToRadians(Degrees);
		return FVector(FMath::Sin(Radians), 0.0, FMath::Cos(Radians));
	}

	// Load-path tests use DestructionProfiles::Unbreakable: they measure routing, not breaking.

	/*
	 * Force in uu that loads the area to the stress. Spelled out independently of
	 * ForceUnitsPerMPaSqCm: 1 N = 100 uu, 1 cm2 = 100 mm2, so 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	/** Stress in MPa a force puts on an area. */
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

		// Zero by default: an unmeasured lever arm, so geometry-free fixtures see no moment.
		FVector CentreCm = FVector::ZeroVector;
		FVector HalfExtentCm = FVector::ZeroVector;
	};

	/** A whole structure as data. */
	struct FStructureSpec
	{
		TArray<FPieceSpec> Pieces;
		TArray<FConnectionSpec> Connections;
	};

	/** How a joint resolves its load. */
	enum class EJointKind : uint8
	{
		/** Vertical normal: pure compression. */
		Bed,

		/** Horizontal normal: pure shear. */
		Head,
	};

	struct FExpectedJoint
	{
		/*
		 * Signed Z of the stored force in uu. It acts on PieceB (ConnectionLoad.h), so
		 * it is negative when the supported piece is PieceB and positive otherwise.
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

	// Builds in place: FConnection's has-given latch is per-copy, so a by-value copy could latch a temporary.
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
	 * Asserts the signed force one connection carries and how it classifies: the
	 * same downward load is compression on a bed joint and shear on a head joint.
	 * Tension is zero only because these fixtures are axis-aligned and geometry-free;
	 * tilted joints are covered by Structure.TiltedJointClassification.
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

		// Gravity stays vertical whatever the joint's orientation.
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
	 * The pieces that hold PieceIndex up, per DESIGN.md §3's two-tier rule: bed
	 * joints beneath, else head joints. A transcription of FStructure::GetJointRole.
	 * It does not model knots (DESIGN.md §3 strands them), so a knotted spec needs
	 * an explicit expectation table.
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

	// Whether the piece reaches ground through supports; directed and cycle-safe.
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

	/** A structure and its expected joint loads and support states. */
	struct FSolveCase
	{
		const TCHAR* Description;
		FStructureSpec Spec;

		/** In connection-array order. */
		TArray<FExpectedJoint> ExpectedJoints;

		/** In piece-array order. */
		TArray<bool> ExpectedSupported;
	};

	/*
	 * Builds, solves and checks one case against its expectation table. The
	 * ground-reaction conservation sum is a cross-check only; it is blind to
	 * over-stranding.
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

		// The loops below are bounded by the expectation arrays, so row counts must match.
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

	// A support state as text for failure messages.
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

	// A joint role as text for failure messages.
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

	// Checks GetPieceSupport agrees with IsPieceSupported: Grounded and Supported mean held up.
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
 * Weight accumulates downward and each connection carries the share it supports.
 * Support is two-tiered (DESIGN.md §3): bed joints beneath, else head joints.
 * Graph-distance routing is wrong: a brick spanning a gap is as far from earth
 * as the brick on it, so the bed joint between them would carry nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureLoadPathTest,
	"DestructionGame.Core.Structure.LoadPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureLoadPathTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	// If ClayBrick stops being constant-initialised BrickMassKg reads zero and every expectation with it.
	TestTrue(
		FString::Printf(TEXT("the brick mass must derive to something positive, got %f kg"), BrickMassKg),
		BrickMassKg > 0.0);

	// No case strands anything, so the conservation cross-check is exact.
	const TArray<FSolveCase> Cases = {
		{
			TEXT("a lone grounded piece carries nothing"),
			{ { { BrickMassKg, true } }, {} },
			{},
			{ true }
		},

		// 2.7216 kg x 980 = 2667.2 uu (DESIGN.md §3).
		{
			TEXT("a piece resting on a grounded piece loads the joint with its own weight"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false } },
				{ { 0, 1, BedJointNormal, JointAreaSqCm } }
			},
			{ { -BrickWeightUU, EJointKind::Bed } },
			{ true, true }
		},

		// Declared upper piece first: the force on PieceB is +W; storing -W would read as tension.
		{
			TEXT("a bed joint declared upper piece first still resolves as compression"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false } },
				{ { 1, 0, InvertedBedJointNormal, JointAreaSqCm } }
			},
			{ { BrickWeightUU, EJointKind::Bed } },
			{ true, true }
		},

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
		 * DESIGN.md §2's worked example: keystone piece 2 reaches ground only through
		 * two head joints, which split the two bricks' weight evenly as shear.
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
			// Equal magnitudes; only classification differs.
			{
				{ -BrickWeightUU, EJointKind::Head },
				{ -BrickWeightUU, EJointKind::Head },
				{ -BrickWeightUU, EJointKind::Bed }
			},
			{ true, true, true, true }
		},

		// The same keystone declared the other way round: signs flip, classifications don't.
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

		// Piece 1 hangs under slab 2; that bed joint bears nothing, so the head joint takes it all.
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
		 * A running-bond wall whose bottom middle brick spans a gap:
		 *
		 *        E1          course E   piece 5
		 *     D1    D2       course D   pieces 3, 4
		 *   Cx  Cy  Cz       course C   pieces 0, 1, 2   (Cy spans the gap)
		 *   ==      ==       earth, missing under Cy
		 *
		 * Cy takes 1.5 bricks from above plus its own and pushes 1.25 out through
		 * each head joint. Graph-distance routing would load its bed joints with zero.
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

		{
			TEXT("a joint between two grounded pieces carries nothing"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, true } },
				{ { 0, 1, HeadJointNormal, JointAreaSqCm } }
			},
			{ { 0.0, EJointKind::Head } },
			{ true, true }
		},

		{
			TEXT("a stack with nothing grounded is unsupported and carries no static load"),
			{
				{ { BrickMassKg, false }, { BrickMassKg, false } },
				{ { 0, 1, BedJointNormal, JointAreaSqCm } }
			},
			{ { 0.0, EJointKind::Bed } },
			{ false, false }
		},

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
		 * F and D are unsupported, so F drops out of C's share and G—C takes the
		 * whole brick. An even split would lose half the weight.
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

		// A two-cycle of head-joint supports; an untracked walk loops forever.
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
 * Pins the 45-degree bed/head threshold from both sides. One piece on two
 * equal-area joints, one tilted: as a bed joint it takes the whole brick; as a
 * head joint the two split evenly. Magnitudes only; see TiltedJointClassification.
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

		double DegreesFromVertical;

		/** Fraction of the piece's weight each joint carries. */
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
 * Characterises inclined joints, including one that holds a piece in tension
 * (accepted by DESIGN.md §3). One brick, one grounded anchor, one inclined joint:
 *
 *     joint BENEATH               joint ABOVE
 *          [brick]                    \  <- anchor above
 *         /                            \
 *        / <- face                      [brick]  hangs off the face
 *     [anchor]                       =========
 *     ========
 *
 * Past 45 degrees the head tier is sign-blind, so a face above the piece
 * supports it in tension; under 45 it is BedAbove and the piece falls. For load
 * W on a face tilted T: normal W cos T, in-plane W sin T.
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
		/** The piece is not held up. */
		Unloaded,

		Compression,

		Tension,
	};

	struct FTiltCase
	{
		const TCHAR* Description;

		double DegreesFromVertical;

		/** Face above the loaded piece, i.e. declared loaded-piece-first. */
		bool bJointAbovePiece;

		bool bExpectedSupported;
		ENormalAxis ExpectedNormalAxis;
	};

	const TArray<FTiltCase> Cases = {
		// Beneath the piece: always compressed.
		{ TEXT("a bed joint 40 degrees beneath a piece is compressed, and sheared too"),
			40.0, false, true, ENormalAxis::Compression },
		{ TEXT("a head joint 46 degrees beneath a piece is compressed"),
			46.0, false, true, ENormalAxis::Compression },
		{ TEXT("a head joint 50 degrees beneath a piece is compressed"),
			50.0, false, true, ENormalAxis::Compression },
		{ TEXT("a head joint 60 degrees beneath a piece is compressed"),
			60.0, false, true, ENormalAxis::Compression },
		{ TEXT("a vertical head joint beneath a piece is pure shear, with nothing on the normal axis"),
			90.0, false, true, ENormalAxis::Compression },

		// Above the piece: under 45 the brick falls; over 45 it hangs in tension.
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
		// Piece 0 is the brick, piece 1 the grounded anchor.
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

		TestTrue(
			FString::Printf(TEXT("%s: the grounded anchor must stay supported"), Case.Description),
			Structure.IsPieceSupported(1));

		const double Radians = FMath::DegreesToRadians(Case.DegreesFromVertical);
		const double NormalMagnitude =
			Case.ExpectedNormalAxis == ENormalAxis::Unloaded ? 0.0 : BrickWeightUU * FMath::Cos(Radians);
		const double ShearMagnitude =
			Case.ExpectedNormalAxis == ENormalAxis::Unloaded ? 0.0 : BrickWeightUU * FMath::Sin(Radians);

		// The stored force acts on PieceB, so declaring the loaded piece first stores it pointing up.
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

		// The two axes must recompose to W.
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
	 * The same brick on the same 50-degree face is measured against a different
	 * strength depending on which side it sits:
	 *
	 *   FACE BENEATH  shear governs:   0.00204320 / (0.2 + 0.6 x 0.00171444) = 0.010164
	 *   FACE ABOVE    tension governs: 0.00171444 / 0.1                      = 0.017144
	 *
	 * Each is pinned to its axis. Neither joint gives.
	 */
	{
		constexpr double UtilisationTolerance = 1.0e-9;
		constexpr double DegreesFromVertical = 50.0;

		const double Radians = FMath::DegreesToRadians(DegreesFromVertical);
		const double NormalUU = BrickWeightUU * FMath::Cos(Radians);
		const double ShearUU = BrickWeightUU * FMath::Sin(Radians);

		const double NormalStressMPa = MPaForForce(NormalUU, JointAreaSqCm);
		const double ShearStressMPa = MPaForForce(ShearUU, JointAreaSqCm);

		// Two identical joints, since ApplyForce latches; only the force's sign differs.
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
 * The structure rejects nonsense handles, masses, centres of mass and interfaces
 * at the door rather than storing them. A NaN centre or rectangle becomes a NaN
 * moment, which reads as intact, so the wall stands when it should fall. Zero
 * extents mean unmeasured bending capacity, so the rectangle rule applies only
 * when one is supplied.
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
		 * Centre of mass, one axis at a time: a guard on X alone misses a broken Y.
		 * An accepted centre must be stored unchanged, not zeroed.
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
				// Refused outright; storing it with the flag cleared would look healthy.
				TestTrue(
					FString::Printf(
						TEXT("%s: nothing may have been stored, the structure holds %d live pieces"),
						Case.Description, Structure.NumLivePieces()),
					Structure.NumLivePieces() == 0);
			}
		}
	}

	// Handles are sequential; also stops an AddPiece that accepts nothing passing the rows above.
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

			// The load split divides by area.
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

			// The rectangle must agree with the area (4 x 5 x 5 = 100): area splits load, the rectangle sets the lever arm.
			{
				TEXT("a bed joint whose rectangle agrees with its area"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, BedJointHalfExtentCm },
				true
			},

			// No rectangle is not a broken one; the rule applies only when one is supplied.
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

			// "Rectangle supplied" means any component non-zero, so a half-filled one is refused.
			{
				TEXT("a rectangle with one extent left at zero is a line, not a face"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, FVector(5.0, 0.0, 0.0) },
				false
			},

			// 4 x -5 x -5 = 100: a product-only guard would accept this and flip every stress sign.
			{
				TEXT("two negative half-extents multiply into a plausible area and are still not a face"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, FVector(-5.0, -5.0, 0.0) },
				false
			},

			// Area-consistent, but an extent on the normal's axis makes a box.
			{
				TEXT("an extent on the normal's own axis is a box, not a face"),
				{ 0, 1, BedJointNormal, JointAreaSqCm, FVector::ZeroVector, FVector(5.0, 5.0, 3.25) },
				false
			},

			// The tolerance, bracketed: relative 1e-12 is rounding noise, 1e-6 is a different face.
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

			// Fails closed: a NaN extent or centre gives a NaN moment, which compares false against "> 1.0".
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

			// A centroid has no sensible bound; only finiteness is checked.
			{
				TEXT("a joint a long way from the origin is still a joint"),
				{ 0, 1, BedJointNormal, JointAreaSqCm,
					FVector(5.625, -4000.0, 7.0), BedJointHalfExtentCm },
				true
			},

			// The other normal axes: a guard hard-coding X and Y as in-plane would refuse these.
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
			 * A tilted normal may not carry a rectangle: its in-plane frame is ambiguous.
			 * MakeInterface only emits axis-aligned normals, so nothing real is refused.
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

			// Tilted normals without a rectangle stay valid; cos(90) is 6.1e-17, not 0.
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

			// Only rows meant to be accepted; a wrongly accepted row has already failed above.
			if (Handle == INDEX_NONE || !Case.bIsAccepted)
			{
				continue;
			}

			// An accepted joint keeps its geometry bit for bit; the door validates, never normalises.
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
 * Solving never breaks a joint, however overloaded; breaking is a separate step.
 * The masses are far past mortar's limits, checked below so the overload is real.
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

	// Compressive capacity 10 MPa x 100 cm2 = 1e7 uu, so the bed joint is at 1.96x.
	const double CompressiveCapacityUU =
		ForceForMPa(GeneralPurposeMortar.CompressiveStrengthMPa, JointAreaSqCm);
	const double MaxShearCapacityUU =
		ForceForMPa(GeneralPurposeMortar.MaxShearStrengthMPa, JointAreaSqCm);

	// One overloaded piece per joint: piece 2 on a bed joint, piece 3 on a head joint.
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

		// Each load must exceed the capacity on its own axis: compression for the bed, shear for the head.
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

	// Solving twice must give the same answer.
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
 * Invariants over many shapes, including chains, cycles and ungrounded structures:
 * loads are finite; support matches SpecReachesGround; ground reaction equals
 * supported weight; no axis-aligned joint is in tension; solving breaks nothing.
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

	Specs.Add({
		TEXT("a bed joint declared upper piece first"),
		{
			{ { BrickMassKg, true }, { BrickMassKg, false } },
			{ { 1, 0, InvertedBedJointNormal, JointAreaSqCm } }
		}
	});

	Specs.Add({
		TEXT("a piece hanging beneath a grounded piece"),
		{
			{ { BrickMassKg, true }, { BrickMassKg, false } },
			{ { 1, 0, BedJointNormal, JointAreaSqCm } }
		}
	});

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

	// Piece 1's whole weight must go to piece 0; an even split would fail conservation.
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

			TestTrue(
				FString::Printf(TEXT("%s: connection %d load must be vertical, got (%f, %f, %f)"),
					Named.Description, Index, Force.X, Force.Y, Force.Z),
				FMath::IsNearlyZero(Force.X, Tolerance) && FMath::IsNearlyZero(Force.Y, Tolerance));

			/*
			 * Tension means the force was stored against the wrong end. Holds only
			 * because every normal here is axis-aligned; a tilted normal can legitimately
			 * produce tension (DESIGN.md §3), so scope this rather than delete it.
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

		// Relative tolerance: one spec weighs 9.8e8 uu.
		const double ConservationTolerance = FMath::Max(Tolerance, 1.0e-9 * TotalSupportedWeightUU);

		TestTrue(
			FString::Printf(
				TEXT("%s: the structure holds up %f but only %f reaches the ground"),
				Named.Description, TotalSupportedWeightUU, GroundReactionUU),
			FMath::IsNearlyEqual(GroundReactionUU, TotalSupportedWeightUU, ConservationTolerance));

		// Out-of-range handles fail closed.
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

		// Unknown handles return an obviously-empty placeholder, not a plausible piece or joint.
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

			// Zero area makes a loaded placeholder read as failed, not intact.
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

		// Stops a solver that returns zero everywhere passing the matrix.
		TestTrue(
			FString::Printf(TEXT("%s: %f of weight is held up but the joints carry %f in total"),
				Named.Description, TotalSupportedWeightUU, TotalCarriedUU),
			(TotalSupportedWeightUU > Tolerance) == (TotalCarriedUU > Tolerance));
	}

	return true;
}

/*
 * A cycle in the support relation must not strand load silently. Kahn ordering
 * never readies pieces in a cycle, so their weight never reaches earth. Such
 * pieces are reported unsupported, the fail-closed direction (DESIGN.md §3).
 * Dividing load round a loop is still unsolved (CURRENT_STATE.md). The
 * invariants are checked against what the solver reports, not the table.
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

		/** In piece-array order. */
		TArray<bool> ExpectedSupported;
	};

	const TArray<FCycleCase> Cases = {
		// Control: a head joint onto a grounded piece is a valid support, no cycle.
		{
			TEXT("a brick held sideways by a grounded neighbour is supported and loads the joint"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false } },
				{ { 0, 1, HeadJointNormal, JointAreaSqCm } }
			},
			{ true, true }
		},

		/*
		 * The minimum repro:
		 *
		 *   G  —  X  —  Y      pieces 0, 1, 2
		 *   ==                 earth, under G only
		 *
		 * X's supports are {G, Y} and Y's are {X}, so each waits for the other.
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

		// Control: a cycle through a grounded piece still orders, since grounded pieces are never in the ordering.
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

		for (int32 Index = 0; Index < Case.ExpectedSupported.Num(); ++Index)
		{
			TestTrue(
				FString::Printf(TEXT("%s: piece %d support, expected %d, got %d"),
					Case.Description, Index,
					Case.ExpectedSupported[Index] ? 1 : 0,
					Structure.IsPieceSupported(Index) ? 1 : 0),
				Structure.IsPieceSupported(Index) == Case.ExpectedSupported[Index]);
		}

		// From here on, measured against what the solver reports.
		double ReportedSupportedWeightUU = 0.0;

		for (int32 PieceIndex = 0; PieceIndex < Case.Spec.Pieces.Num(); ++PieceIndex)
		{
			if (!Structure.IsPieceSupported(PieceIndex) || Case.Spec.Pieces[PieceIndex].bIsGrounded)
			{
				continue;
			}

			const double OwnWeightUU = WeightOf(Case.Spec.Pieces[PieceIndex].MassKg);
			ReportedSupportedWeightUU += OwnWeightUU;

			// Every touching joint: a lower bound that needs no copy of the tier rule.
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

		// Short means stranded or overwritten; long means double-counted.
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
 * Stranding does not propagate downward: only pieces in the knot are stranded
 * (DESIGN.md §3), and pieces beneath keep their support and load. Kahn ordering
 * runs top-down, so stranding every unordered piece would take down pieces below
 * a knot. Conservation can't see that, so support and loads are asserted directly.
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
		 *         Z  —  X  —  Y      pieces 1, 2, 3, joined sideways
		 *         |                  bed joint, Z on the pier
		 *      [ pier ]              piece 0, on the earth
		 *      =======
		 *
		 * X and Y form the knot. Z is not in it, but X names Z, so Z is never ordered.
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
		 *               B                piece 4, resting on Z
		 *               |
		 *         Z  —  X  —  Y          pieces 1, 2, 3
		 *         |
		 *      [ pier ]                  piece 0
		 *      =======
		 *
		 * B stands on Z, so the pier joint carries two bricks.
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
		 * Two courses beneath the knot:
		 *
		 *        Zhigh — X — Y      pieces 2, 3, 4
		 *          |
		 *        Zlow               piece 1
		 *          |
		 *      [ pier ]             piece 0
		 *      =======
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
 * Stranding does propagate upward: a piece whose only support is falling is
 * falling too. This is the only case that needs the solve to iterate.
 *
 *                       B          piece 3, resting on Y through a bed joint
 *                       |
 *      [ G ]  —  X  —   Y          pieces 0, 1, 2, joined sideways
 *      =====
 *
 *   ITERATING     supported = [T,F,F,F]   forces = [0, 0, 0]
 *   SINGLE-PASS   supported = [T,F,F,T]   forces = [0, 0, -2667.2]
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureStrandingPropagatesUpwardTest,
	"DestructionGame.Core.Structure.StrandingPropagatesUpward",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureStrandingPropagatesUpwardTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	const TArray<FSolveCase> Cases = {
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
		 * Both rules at once: Z beneath the knot stands; B above it falls.
		 *
		 *                         B      piece 4, resting on Y
		 *                         |
		 *         Z  —  X  —      Y      pieces 1, 2, 3
		 *         |
		 *      [ pier ]                  piece 0
		 *      =======
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
 * Why a piece is unsupported: Falling (physics) or Stranded (in an unroutable
 * knot, a solver limitation; DESIGN.md §3). DESIGN.md §4's collapse test needs to
 * know no piece was stranded. A piece resting only on a knot is Falling, not
 * Stranded. Each case mirrors an existing fixture.
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

		/** In piece-array order. */
		TArray<EPieceSupport> ExpectedSupport;
	};

	const TArray<FSupportReasonCase> Cases = {
		{
			TEXT("a brick held sideways by a grounded neighbour is SUPPORTED, not stranded"),
			{
				{ { BrickMassKg, true }, { BrickMassKg, false } },
				{ { 0, 1, HeadJointNormal, JointAreaSqCm } }
			},
			{ EPieceSupport::Grounded, EPieceSupport::Supported }
		},

		/*
		 * SupportCycle's minimum repro; X and Y are in the knot.
		 *
		 *   G  —  X  —  Y      pieces 0, 1, 2
		 *   ==                 earth, under G only
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
		 * StrandingIsLocal's repro: Z keeps its support; only X and Y are stranded.
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
		 * Stranded versus Falling: X and Y are in the knot; B is not.
		 *
		 *                       B          piece 3, resting on Y through a bed joint
		 *                       |
		 *      [ G ]  —  X  —   Y          pieces 0, 1, 2
		 *      =====
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

	// Checks the table itself still covers every state.
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

		// Grounded matches the spec's grounding exactly, checked against the spec rather than the table.
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
 * GetPieceSupport has the same scope as IsPieceSupported and fails closed:
 *
 *     never solved      Falling, for every handle
 *     removed           the last solve's answer, unchanged, until the next solve
 *     after that solve  Falling, and a removed grounded piece is no longer earth
 *
 * No separate "removed" state: it would contradict IsPieceSupported until the
 * re-solve. Out-of-range handles are Falling too.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructurePieceSupportDegenerateInputTest,
	"DestructionGame.Core.Structure.PieceSupportDegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructurePieceSupportDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	/** A grounded pad with a brick resting on it. */
	const FStructureSpec PadAndBrick = {
		{ { BrickMassKg, true }, { BrickMassKg, false } },
		{ { 0, 1, BedJointNormal, JointAreaSqCm } }
	};

	// Before any solve everything reads Falling.
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

	// Out-of-range handles against a solved, standing structure.
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

	// A removed brick keeps its reason until a re-solve, then reads Falling.
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

	// A removed grounded piece must stop reading Grounded after the re-solve.
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

	// Removing X from a two-brick knot dissolves it: both X and Y read Falling, not Stranded.
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
 * GetConnectionUtilisation reports each joint's load ratio from the last solve
 * without breaking anything. It must equal, bitwise,
 *
 *     GetConnection(I).UtilisationUnder(GetConnectionForce(I), GetConnectionMoment(I))
 *
 * and also match values derived from stress and strength. The eccentric fixture
 * at the end is needed because geometry-free joints have zero moment, so the
 * identity would hold trivially without it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureConnectionUtilisationTest,
	"DestructionGame.Core.Structure.ConnectionUtilisation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureConnectionUtilisationTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	/** A double with its bit pattern, for exact comparisons. */
	const auto Bits = [](double Value)
	{
		uint64 Raw = 0;
		FMemory::Memcpy(&Raw, &Value, sizeof(Raw));
		return FString::Printf(TEXT("%.17g [%016llx]"), Value, Raw);
	};

	const double MortarCompressiveMPa = GeneralPurposeMortar.CompressiveStrengthMPa;
	const double MortarCohesionMPa = GeneralPurposeMortar.ShearCohesionMPa;

	// Relative, with a floor well below the ~1e-4 expectations so zero stays distinguishable.
	const auto IsClose = [](double Actual, double Expected)
	{
		return FMath::IsNearlyEqual(Actual, Expected, FMath::Max(1.0e-15, FMath::Abs(Expected) * 1.0e-9));
	};

	/*
	 * Three joints, three different utilisations:
	 *
	 *        [2]                  piece 2 rests on 1 through a 200 cm2 bed joint
	 *         |
	 *        [1]---[3]            piece 3 has no bed joint at all, so it hangs off
	 *         |                   piece 1's 100 cm2 HEAD joint and loads it in shear
	 *        [0] grounded         piece 1 rests on 0 through a 100 cm2 bed joint
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

		// Before any solve every joint reads zero.
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

		// Stress over strength, via MPaForForce so a wrong ForceUnitsPerMPaSqCm fails here.
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

			TestTrue(
				FString::Printf(TEXT("%s: joint %d should carry Z = %f, got %f"),
					Expected[Index].Description, Index, Expected[Index].ForceZUU, Force.Z),
				FMath::IsNearlyEqual(Force.Z, Expected[Index].ForceZUU, 1.0e-6));

			const double Utilisation = Structure.GetConnectionUtilisation(Index);

			TestTrue(
				FString::Printf(TEXT("%s: joint %d should be at utilisation %.12e, got %s"),
					Expected[Index].Description, Index, Expected[Index].Utilisation, *Bits(Utilisation)),
				IsClose(Utilisation, Expected[Index].Utilisation));

			// Bitwise identity. Moment and composite depth are passed explicitly since both parameters default.
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

			TestFalse(
				FString::Printf(TEXT("%s: joint %d must still be intact after being asked"),
					Expected[Index].Description, Index),
				Connection.HasGiven());
		}
	}

	/*
	 * A broken joint reads zero because it carries no force, not because of the
	 * latch. A renderer must use HasGiven to draw it as broken.
	 */
	{
		// 20 tonnes on 100 cm2: 19.6 MPa against mortar's 10 MPa, utilisation 1.96.
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

		// Reading must not latch, or the cascade below would report zero passes.
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

		// The given joint still evaluates a load, so the zero above came from the load, not the latch.
		const double WhatWouldHaveBrokenIt = Structure.GetConnection(0).UtilisationUnder(BreakingForce);

		TestTrue(
			FString::Printf(
				TEXT("a given joint still answers what the load WOULD do, expected %.12e, got %s"),
				BreakingUtilisation, *Bits(WhatWouldHaveBrokenIt)),
			IsClose(WhatWouldHaveBrokenIt, BreakingUtilisation));
	}

	/*
	 * A joint under a real moment, so the identity above is not trivially true.
	 *
	 *     [1]  a brick, its weight acting 2 cm along +X of the joint's own centroid
	 *      |
	 *     [0]  grounded pad, one 10 x 10 cm bed joint between them
	 *
	 * One support, so the moment is determinate (MOMENTS_DESIGN.md). Bending stress
	 * |M| / W_sec = 2W / 166.67 exceeds the compressive W / 100, so the opened edge
	 * governs in tension: 5.334e-3, twenty times the moment-free 2.667e-4.
	 */
	{
		constexpr double JointHalfCm = 5.0;

		constexpr double EccentricityCm = 2.0;

		/** (4/3) x h_u x h_v^2, from beam theory. */
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

		TestTrue(
			FString::Printf(TEXT("fixture: the joint should carry one brick downward, got (%f, %f, %f)"),
				Force.X, Force.Y, Force.Z),
			FMath::IsNearlyEqual(Force.Z, -BrickWeightUU, 1.0e-6)
				&& FMath::IsNearlyEqual(Force.X, 0.0, 1.0e-9)
				&& FMath::IsNearlyEqual(Force.Y, 0.0, 1.0e-9));

		// (p - c) x F = (2, 0, 10) x (0, 0, -W) = (0, 2W, 0).
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

		// The opened edge, against mortar's tensile strength.
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

		// Nothing rests on the brick, so composite depth must be zero; otherwise it would relieve the moment.
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

		// Without the moment the joint reads a twentieth, so the identity above is not vacuous.
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

		TestFalse(TEXT("reading an eccentric joint's moment must not break it"),
			Structure.GetConnection(Joint).HasGiven());
	}

	/*
	 * An unknown handle fails closed at TNumericLimits<double>::Max(), not zero,
	 * which would read as healthy (DESIGN.md §2).
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

			// The moment is a load, not a verdict, so zero is the safe answer; Max() would spread NaN.
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
 * GetJointRole reads None for an unknown connection handle or a piece not on the
 * connection (Structure.h). The unnormalisable-normal clause is unreachable, since
 * AddConnection refuses such joints. INDEX_NONE against INDEX_NONE is the sharp
 * row: a placeholder connection's PieceB is also INDEX_NONE. The last two rows
 * stop a fix that returns None unconditionally.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureJointRoleDegenerateInputTest,
	"DestructionGame.Core.Structure.JointRoleDegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureJointRoleDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace StructureTestSupport;

	// A grounded pad with a brick on it: one real joint to be wrong about.
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
		// Default FJointInspection against default FPieceRef: both INDEX_NONE.
		{ TEXT("INDEX_NONE against INDEX_NONE"), INDEX_NONE, INDEX_NONE, EJointRole::None },

		{ TEXT("a handle past the last joint, against INDEX_NONE"), 99, INDEX_NONE, EJointRole::None },
		{ TEXT("a negative handle, against INDEX_NONE"), -5, INDEX_NONE, EJointRole::None },

		// Unknown joint, real piece.
		{ TEXT("a handle past the last joint, against a real piece"), 99, 0, EJointRole::None },
		{ TEXT("INDEX_NONE, against a real piece"), INDEX_NONE, 0, EJointRole::None },

		// Real joint, piece not on it.
		{ TEXT("a real joint, against a piece that is not on it"), 0, 99, EJointRole::None },
		{ TEXT("a real joint, against INDEX_NONE"), 0, INDEX_NONE, EJointRole::None },

		// The same real joint from both ends.
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
