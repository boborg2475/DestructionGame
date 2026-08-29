// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A PIECE HANGS BY ITS FASTENER'S WITHDRAWAL CAPACITY, AND ONLY BY THAT.
 *
 * SHED_PATH.md Phase C, slice C1 (evolution step 5): fastener withdrawal goes live so a joint can
 * carry TENSION up to the fastener's capacity. The end-state the /goal needs is the overhang FIXED
 * to the wall, hanging from it, and this is the smallest fixture that isolates the one capability
 * that makes that possible: a piece held UP purely by a joint pulling against gravity. No
 * compression, no shear, no friction, no bending moment — the load is straight-down weight and the
 * only thing between it and the floor is the joint's tension.
 *
 * =========================================================================================
 * THE FIXTURE — ONE HANGING PIECE, ONE JOINT, PURE AXIAL TENSION
 * =========================================================================================
 *
 *          =====[STUB]=====        grounded stub, 10 x 10 x 10 cm, the overhead support
 *               |  #  |            one bed joint (the fastener), 100 cm2, normal +Z
 *          +----+-----+----+
 *          |                |
 *          |    [BLOCK]     |      121.6 kg of clay, 40 x 40 x 40 cm, hanging in mid-air
 *          |                |
 *          +----------------+      centroid directly below the joint => zero moment
 *
 * The grounded STUB is the fixed support (a ceiling boss / a wall bracket). The BLOCK hangs one
 * bed-joint-thickness below it, connected by a SINGLE joint whose in-plane overlap is the stub's
 * 10 x 10 footprint — 100 cm2, one fastener's tributary patch, deliberately far smaller than the
 * block's own 40 x 40 face so the capacity is a fastener capacity rather than tens of courses of
 * masonry. The block's centre of mass sits directly beneath the joint centre, so the weight makes
 * no moment about it: the joint carries pure NORMAL TENSION equal to the block's weight, and the
 * verdict turns on one inequality — weight against f_t * area.
 *
 * =========================================================================================
 * WHAT VARIES, AND WHY THE TABLE IS THE TEST
 * =========================================================================================
 *
 * The geometry and the mass are held FIXED across the whole table; the ONLY thing that changes row
 * to row is the connection profile, hence its withdrawal capacity `TensileStrengthMPa`. So the
 * verdict flipping across the rows can only be the withdrawal capacity talking — every other axis
 * is identical. This is DESIGN.md §2's promise ("the same frame built with nails vs screws vs
 * bolts genuinely behaves differently under load") stated as an OUTCOME rather than a reading:
 *
 *      DryStone   f_t = 0      capacity 0            => FALLS  (dry stone cannot hang at all)
 *      Nail       f_t = 0.071  capacity   71,000 uu  => FALLS  (weight 119,168 uu outruns it 1.68x)
 *      Screw      f_t = 0.54   capacity  540,000 uu  => STANDS (holds 4.53x the weight)
 *      Bolt       f_t = 1.61   capacity1,610,000 uu  => STANDS
 *
 * The DryStone row is the keyed-on-DATA proof: a joint with no tensile bond writes no tension
 * column at all, so the block has no way to hang and comes down — exactly the classic no-tension
 * masonry the model reduces to when the withdrawal capacity is absent.
 *
 * =========================================================================================
 * WHAT IS ASSERTED, AND WHY EACH FORM
 * =========================================================================================
 *
 *   - THE MECHANISM, VIA THE ORACLE'S lambda*. With gravity live the rigid-block LP reports the
 *     load multiplier at which the hang fails, which for a pure axial tension is exactly
 *     capacity / weight. That number is hand-derived here from f_t, the area and the local unit
 *     conversion, and asserted against what the LP returns — so a wrong tension row, a wrong area,
 *     or a wrong conversion DISAGREES with this file rather than being absorbed. This is the
 *     immune-to-jitter mechanism assertion: lambda* is a ratio, not a displacement.
 *
 *   - THE OUTCOME, VIA PRODUCTION. After SolveAndBreak the block must read Supported when the
 *     fastener holds and must have lost the earth (Falling) when it does not. Displacement is
 *     never read — a severed hang can rest exactly in place for one frame; what is asserted is the
 *     support state the game acts on. Nothing may be Stranded, so a fall is a genuine tension
 *     failure and not a routing artefact wearing its clothes.
 *
 * =========================================================================================
 * UNITS — SPELLED OUT LOCALLY SO A WRONG CONSTANT FAILS THIS TEST RATHER THAN AGREEING
 * =========================================================================================
 *
 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa (= 1 N/mm2) over 1 cm2 is 100 * 100 = 10000 uu. This
 * is DELIBERATELY NOT DestructionForce::ForceUnitsPerMPaSqCm nor the oracle's own private copy: if
 * that boundary constant is out by the notorious 100x, this file must go red rather than nod along.
 * Weight is MassKg * 980 (the 1 N = 100 uu factor already inside the 980), and mass is the clay
 * density times the true volume.
 *
 * NEEDS A TICKING WORLD: NO. FStructure is arithmetic over a graph and the fixture is three boxes;
 * gravity is on (weight = mass * 980), everything is connected, and every assertion is on the
 * oracle, the outcome, or solver state. Same footing as the two-load-path and beam acceptance
 * tests.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit, at which
 * point two anonymous namespaces are the same namespace and identically-named helpers collide.
 */
namespace HangingWithdrawalTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * GEOMETRY. Every length is centimetres, at Unreal's default 1 uu = 1 cm.
	 * ================================================================================ */

	/** Fired clay, 1.9 g/cm3 — the density every wall fixture in the suite uses. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** The hanging block: a compact 40 cm cube, so its weight is real without a metres-tall column. */
	constexpr double BlockSideCm = 40.0;

	/** The overhead stub: a 10 x 10 x 10 cm grounded boss. Its footprint IS the joint patch. */
	constexpr double StubSideCm = 10.0;

	/** The bed joint the fastener forms across — the gap between the block's top and the stub's foot. */
	constexpr double JointThicknessCm = 1.0;

	/** The block floats clear of the floor; its centre and the stub's are stacked on the same X-Y line. */
	constexpr double BlockCentreZCm = 100.0;
	constexpr double BlockTopZCm = BlockCentreZCm + BlockSideCm / 2.0;
	constexpr double StubCentreZCm = BlockTopZCm + JointThicknessCm + StubSideCm / 2.0;

	/** The joint is the stub's 10 x 10 footprint: one fastener's tributary patch, not the block's face. */
	constexpr double JointAreaSqCm = StubSideCm * StubSideCm;

	/* ================================================================================
	 * UNITS AND THE INDEPENDENT STATICS. None of it imported from the code under test.
	 * ================================================================================ */

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * 1 MPa over 1 cm2 is 10000 uu (1 N = 100 uu, 1 cm2 = 100 mm2). DELIBERATELY a local literal
	 * and not the production boundary constant, so a wrong conversion fails here.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	constexpr double BlockMassKg =
		ClayDensityGramsPerCubicCm * BlockSideCm * BlockSideCm * BlockSideCm / 1000.0;

	constexpr double BlockWeightUu = BlockMassKg * GravityCmPerSecondSquared;

	/** The most tension the joint can carry: f_t over its whole patch, in uu. Zero for a dry joint. */
	double WithdrawalCapacityUu(double TensileStrengthMPa)
	{
		return TensileStrengthMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm;
	}

	/* ================================================================================
	 * THE FIXTURE.
	 * ================================================================================ */

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

	/** Lay the grounded stub and the hanging block, joined by the one fastener under test. */
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

	/** True when a live piece has lost every path to the earth — the outcome a dropped hang shows. */
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

	/* ------------------------------------------------------------------ *
	 * PRECONDITIONS ON THE WITHDRAWAL BASIS — the whole verdict turns on
	 * these numbers, so they are pinned to the published EN 1995 figures
	 * rather than read from the profile (a test that read the profile would
	 * agree with a wrong profile).
	 * ------------------------------------------------------------------ */

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

		/*
		 * NOT A KNIFE EDGE. The weight and the capacity must differ by a clear margin so the
		 * verdict is unambiguous and a small crediting error cannot flip it. Dry stone (capacity 0)
		 * satisfies this trivially — the whole weight is unopposed.
		 */
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

		/*
		 * THE JOINT MUST BE A VERTICAL BED JOINT — normal along Z — so the block's weight is pure
		 * NORMAL tension on it. A joint that had come out with an in-plane normal would load the
		 * fastener in shear, and this whole test would be about the wrong axis.
		 */
		const FVector Normal = Fixture.Structure.GetConnection(Fixture.Joint).InterfaceNormal.GetSafeNormal();
		TestTrue(
			*FString::Printf(TEXT("%s: FIXTURE the hang joint must be horizontal (normal +/-Z): normal (%g, %g, %g)"),
				Row.Name, Normal.X, Normal.Y, Normal.Z),
			FMath::Abs(Normal.Z) > 0.999999 && FMath::Abs(Normal.X) < 1.0e-6 && FMath::Abs(Normal.Y) < 1.0e-6);

		TestEqual(
			*FString::Printf(TEXT("%s: FIXTURE the joint patch is the stub's footprint, 100 cm2"), Row.Name),
			Fixture.Structure.GetConnection(Fixture.Joint).InterfaceAreaSqCm, JointAreaSqCm);

		/* ------------------------------------------------------------------ *
		 * THE MECHANISM, VIA THE ORACLE. With gravity live the LP reports the
		 * load multiplier at which the hang fails; for pure axial tension that
		 * is exactly capacity / weight. Hand-derived here, asserted there.
		 * ------------------------------------------------------------------ */

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

			/*
			 * lambda* IS capacity / weight for a pure hang. This is the assertion that pins the
			 * tension row's arithmetic: the LP's own load multiplier must equal the ratio this file
			 * derived independently from f_t, the area and the unit conversion. Two per cent.
			 */
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

		/* ------------------------------------------------------------------ *
		 * THE OUTCOME, VIA PRODUCTION. Settle under gravity and read the block's
		 * support. Held => Supported; over-capacity or dry => lost the earth.
		 * ------------------------------------------------------------------ */

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
