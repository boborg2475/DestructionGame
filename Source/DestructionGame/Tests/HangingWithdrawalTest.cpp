// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A piece hangs by its fastener's withdrawal capacity alone (SHED_PATH.md Phase C, slice C1). A
 * 121.6 kg, 40 cm clay block hangs below a grounded 10 cm stub by one 100 cm2 bed joint, centroid
 * directly below, so the joint carries pure normal tension equal to the weight. Only the joint
 * profile changes between rows: DryStone and Nail fall (the weight is 1.68x the nail's capacity),
 * Screw and Bolt stand (the screw holds 4.53x the weight).
 *
 * Mechanism: the oracle's lambda* must equal capacity / weight, derived here. Outcome: after
 * SolveAndBreak the block is Supported or has lost the earth, and nothing is Stranded. Never
 * displacement.
 *
 * Units: 1 MPa over 1 cm2 = 10000 uu, written locally rather than using ForceUnitsPerMPaSqCm so a
 * 100x error there fails this test. Weight = MassKg * 980. No ticking world. Named namespace for
 * unity builds.
 */
namespace HangingWithdrawalTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Geometry, cm.
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	constexpr double BlockSideCm = 40.0;

	/** The grounded stub; its footprint is the joint patch. */
	constexpr double StubSideCm = 10.0;

	constexpr double JointThicknessCm = 1.0;

	/** Block and stub centres share an X-Y line. */
	constexpr double BlockCentreZCm = 100.0;
	constexpr double BlockTopZCm = BlockCentreZCm + BlockSideCm / 2.0;
	constexpr double StubCentreZCm = BlockTopZCm + JointThicknessCm + StubSideCm / 2.0;

	/** One fastener's tributary patch, not the block's face. */
	constexpr double JointAreaSqCm = StubSideCm * StubSideCm;

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu factor is inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 MPa over 1 cm2 = 10000 uu. A local literal, not the production constant, on purpose. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	constexpr double BlockMassKg =
		ClayDensityGramsPerCubicCm * BlockSideCm * BlockSideCm * BlockSideCm / 1000.0;

	constexpr double BlockWeightUu = BlockMassKg * GravityCmPerSecondSquared;

	/** Maximum joint tension, f_t over the patch, in uu. */
	double WithdrawalCapacityUu(double TensileStrengthMPa)
	{
		return TensileStrengthMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm;
	}

	struct FHang
	{
		FStructure Structure;

		int32 Stub = INDEX_NONE;
		int32 Block = INDEX_NONE;
		int32 Joint = INDEX_NONE;
	};

	FPieceBox MakeBox(double SideXY, double CentreZ, double ThicknessZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(SideXY, SideXY, ThicknessZ) * 0.5;
		Box.CentreCm = FVector(0.0, 0.0, CentreZ);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/** The grounded stub and hanging block, joined by the fastener under test. */
	void Build(const FConnectionStrength& Fastener, FHang& Out)
	{
		const FPieceBox StubBox = MakeBox(StubSideCm, StubCentreZCm, StubSideCm);
		const FPieceBox BlockBox = MakeBox(BlockSideCm, BlockCentreZCm, BlockSideCm);

		Out.Stub = Out.Structure.AddPiece(BoxMassKg(StubBox), /*bIsGrounded*/ true, StubBox.CentreCm);
		Out.Block = Out.Structure.AddPiece(BoxMassKg(BlockBox), /*bIsGrounded*/ false, BlockBox.CentreCm);

		FConnection Joint;

		if (MakeInterface(Out.Stub, StubBox, Out.Block, BlockBox,
				JointThicknessCm, Fastener, Joint))
		{
			Out.Joint = Out.Structure.AddConnection(Joint);
		}
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 Stranded = 0;

		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (!S.IsPieceRemoved(Piece) && S.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}

		return Stranded;
	}

	/** Whether a live piece has lost every path to the earth. */
	bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		if (S.IsPieceRemoved(Piece))
		{
			return false;
		}

		const EPieceSupport Support = S.GetPieceSupport(Piece);
		return Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHangingWithdrawalTest,
	"DestructionGame.Acceptance.Tension.AFastenedPieceHangsByItsWithdrawalCapacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FHangingWithdrawalTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace HangingWithdrawalTestSupport;

	// Withdrawal values pinned to EN 1995, not read from the profiles.
	TestEqual(TEXT("FIXTURE: dry stone has no tensile bond — the keyed-on-data control"),
		DryStone.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("FIXTURE: the nail's mean-density withdrawal is 0.071 MPa (EN 1995-1-1 8.3.2)"),
		Nail.TensileStrengthMPa, 0.071);
	TestEqual(TEXT("FIXTURE: the screw's mean-density withdrawal is 0.54 MPa (EN 1995-1-1 8.7.2)"),
		Screw.TensileStrengthMPa, 0.54);
	TestEqual(TEXT("FIXTURE: the bolt's washer-bearing capacity is 1.61 MPa"),
		Bolt.TensileStrengthMPa, 1.61);

	AddInfo(FString::Printf(
		TEXT("DERIVED: block %g kg => weight %.10g uu, hanging on a %g cm2 joint patch"),
		BlockMassKg, BlockWeightUu, JointAreaSqCm));

	struct FRow
	{
		const TCHAR* Name;
		const FConnectionStrength* Fastener;
	};

	const FRow Rows[4] = {
		{ TEXT("DryStone"), &DryStone },
		{ TEXT("Nail"),     &Nail },
		{ TEXT("Screw"),    &Screw },
		{ TEXT("Bolt"),     &Bolt },
	};

	for (const FRow& Row : Rows)
	{
		const double CapacityUu = WithdrawalCapacityUu(Row.Fastener->TensileStrengthMPa);
		const bool bExpectStands = BlockWeightUu < CapacityUu;

		// Weight and capacity must differ clearly, so a small error can't flip the verdict.
		TestTrue(
			*FString::Printf(
				TEXT("%s: FIXTURE must not be a knife edge — weight %.10g uu vs capacity %.10g uu"),
				Row.Name, BlockWeightUu, CapacityUu),
			FMath::Abs(BlockWeightUu - CapacityUu) > 0.2 * FMath::Max(BlockWeightUu, CapacityUu));

		FHang Fixture;
		Build(*Row.Fastener, Fixture);

		if (Fixture.Joint == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("%s: FIXTURE: the producer must emit the hang's bed joint"), Row.Name));
			return false;
		}

		TestEqual(FString::Printf(TEXT("%s: FIXTURE two pieces — the stub and the block"), Row.Name),
			Fixture.Structure.NumPieces(), 2);
		TestTrue(FString::Printf(TEXT("%s: FIXTURE every piece and joint must know where it is"), Row.Name),
			Fixture.Structure.HasCompleteGeometry());

		// A Z-normal joint, so the weight is pure tension rather than shear.
		const FVector Normal = Fixture.Structure.GetConnection(Fixture.Joint).InterfaceNormal.GetSafeNormal();
		TestTrue(
			*FString::Printf(TEXT("%s: FIXTURE the hang joint must be horizontal (normal +/-Z): normal (%g, %g, %g)"),
				Row.Name, Normal.X, Normal.Y, Normal.Z),
			FMath::Abs(Normal.Z) > 0.999999 && FMath::Abs(Normal.X) < 1.0e-6 && FMath::Abs(Normal.Y) < 1.0e-6);

		TestEqual(
			*FString::Printf(TEXT("%s: FIXTURE the joint patch is the stub's footprint, 100 cm2"), Row.Name),
			Fixture.Structure.GetConnection(Fixture.Joint).InterfaceAreaSqCm, JointAreaSqCm);

		// Mechanism: for pure axial tension the oracle's lambda* is capacity / weight.
		RigidBlockOracle::FOracleProblem Problem;
		FString BridgeWhy;

		const bool bBridged =
			RigidBlockOracle::BuildRigidBlockProblem(Fixture.Structure, Problem, BridgeWhy);

		TestTrue(
			*FString::Printf(TEXT("%s: the oracle bridge must accept this 2D hang (%s)"), Row.Name, *BridgeWhy),
			bBridged);

		if (bBridged)
		{
			const RigidBlockOracle::FOracleResult Oracle = RigidBlockOracle::SolveRigidBlock(Problem);
			const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Oracle);

			const double ExpectedLambda = BlockWeightUu > 0.0 ? CapacityUu / BlockWeightUu : 0.0;

			AddInfo(FString::Printf(
				TEXT("%s: capacity %.10g uu / weight %.10g uu => expected lambda* %.10g; oracle answered %d, "
					 "lambda* %.10g, outcome %d (2=Stands,1=Falls,0=Unanswerable)"),
				Row.Name, CapacityUu, BlockWeightUu, ExpectedLambda, Oracle.bAnswered ? 1 : 0,
				Oracle.Lambda, static_cast<int32>(Outcome)));

			TestTrue(
				*FString::Printf(TEXT("%s: the oracle must ANSWER the hang (a refusal proves nothing)"), Row.Name),
				Oracle.bAnswered);

			TestEqual(
				*FString::Printf(
					TEXT("%s: the LP verdict must match the withdrawal inequality — capacity %.10g vs weight %.10g"),
					Row.Name, CapacityUu, BlockWeightUu),
				static_cast<int32>(Outcome),
				static_cast<int32>(bExpectStands
					? RigidBlockOracle::EOracleOutcome::Stands
					: RigidBlockOracle::EOracleOutcome::Falls));

			// Pins the tension row's arithmetic against the independent ratio, within 2%.
			if (Oracle.bAnswered)
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: the LP's lambda* (%.10g) must equal capacity/weight (%.10g) — the tension "
							 "row carries exactly the fastener's withdrawal, no more, no less"),
						Row.Name, Oracle.Lambda, ExpectedLambda),
					FMath::Abs(Oracle.Lambda - ExpectedLambda) <= 0.02 * FMath::Max(ExpectedLambda, 1.0e-9));
			}
		}

		// Outcome: held reads Supported; over capacity or dry loses the earth.
		Fixture.Structure.SolveLoads();
		const int32 Passes = Fixture.Structure.SolveAndBreak();

		const EPieceSupport BlockSupport = Fixture.Structure.GetPieceSupport(Fixture.Block);
		const int32 Stranded = StrandedCount(Fixture.Structure);

		AddInfo(FString::Printf(
			TEXT("%s: PRODUCTION ran %d breaking pass(es); block support %d (1=Grounded,2=Supported,"
				 "3=Stranded,0=Falling); %d stranded"),
			Row.Name, Passes, static_cast<int32>(BlockSupport), Stranded));

		TestEqual(
			*FString::Printf(TEXT("%s: nothing may be Stranded — a fall must be a tension failure, not a routing artefact"),
				Row.Name),
			Stranded, 0);

		TestTrue(TEXT("BOTH: the grounded stub is the earth and keeps it"),
			Fixture.Structure.GetPieceSupport(Fixture.Stub) == EPieceSupport::Grounded);

		if (bExpectStands)
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: the fastener HOLDS (%.10g uu capacity > %.10g uu weight), so the block must hang — "
						 "read Supported after the cascade, support %d"),
					Row.Name, CapacityUu, BlockWeightUu, static_cast<int32>(BlockSupport)),
				static_cast<int32>(BlockSupport), static_cast<int32>(EPieceSupport::Supported));
		}
		else
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s: the fastener CANNOT HOLD (%.10g uu capacity < %.10g uu weight), so the block must "
						 "lose the earth — support %d, cascade ran %d pass(es)"),
					Row.Name, CapacityUu, BlockWeightUu, static_cast<int32>(BlockSupport), Passes),
				HasLostTheEarth(Fixture.Structure, Fixture.Block));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
