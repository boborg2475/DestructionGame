// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Regression test that the equilibrium gate fells a body held by two load paths past its tipping
 * point (PROMOTION_DESIGN.md §6 Slice 2; green since 2026-08-27).
 *
 * The removed single-body overturning guard (DESIGN.md §5.7) stood aside whenever a body had a
 * second load path, so such a body read safe however far past tipping. The equilibrium LP reasons
 * about the whole structure, finds no admissible force system at self-weight (lambda* < 1), and
 * the body falls.
 *
 * Fixture: one heavy overhanging body on two grounded seats, both on the same side of its centre
 * of mass:
 *
 *      body centroid (X = 147.5)  ---------------------------------------+
 *      +------------------------------------------------------------------+   <- one rigid body,
 *      |  [S2] [S1]                                                       |      300 x 40 x 10.25
 *      +--#----#----------------------------------------------------------+      overhang -->
 *         ||   ||
 *      ===||===||=====  the earth        S2 anchor at X=0 (w=5), S1 pivot at X=10 (w=10)
 *
 * Past tipping: gravity rotates the body about the pivot's right edge (X = 15), so everything left
 * of it lifts and only mortar bond holds it down. The overturning moment outruns even the fully
 * plastic tension block by ~3.4x. Z thickness does not affect the lever, so the body is thick,
 * not tall, to add margin.
 *
 * The router alone zeroes the moment for N >= 2 supports (DESIGN.md §5.3), so without the gate the
 * body reads Supported. The RigidBlockOracle must report Falls for the same structure; that is
 * asserted as a precondition.
 *
 * Outcome test (DESIGN.md §4): the body loses the earth, seats keep it, nothing Stranded. No
 * displacement, no specific joint's HasGiven. Statics, section and unit conversion are derived
 * here, not imported. No world needed. Named namespace for unity builds.
 */
namespace TwoLoadPathOverturningTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Geometry, in cm.

	/** Fired clay, 1.9 g/cm3. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Width on Y of the body and both seats, so every joint's Y overlap is full. */
	constexpr double WytheWidthCm = 10.25;

	/** Mortar bed thickness under the body. */
	constexpr double BedJointThicknessCm = 1.0;

	/** Seat height; both seats are left of the body's centroid. */
	constexpr double SeatHeightCm = 20.0;

	/** The anchor: leftmost seat, whose bond holds the lifting side down. */
	constexpr double AnchorCentreXCm = 0.0;
	constexpr double AnchorWidthXCm = 5.0;

	/** The pivot: rightmost seat; the body tips about its right edge. */
	constexpr double PivotCentreXCm = 10.0;
	constexpr double PivotWidthXCm = 10.0;

	/** The pivot's right edge: the fulcrum. */
	constexpr double FulcrumXCm = PivotCentreXCm + PivotWidthXCm / 2.0;

	/** The body's left edge is flush with the anchor's, so both seats are under it. */
	constexpr double BodyLeftXCm = AnchorCentreXCm - AnchorWidthXCm / 2.0;
	constexpr double BodyLengthXCm = 300.0;
	constexpr double BodyRightXCm = BodyLeftXCm + BodyLengthXCm;
	constexpr double BodyCentreXCm = (BodyLeftXCm + BodyRightXCm) / 2.0;

	/** Thickness on Z. Weight and overturning scale with it; the lever does not. */
	constexpr double BodyThicknessZCm = 40.0;

	/** The body sits one bed above the seat tops. */
	constexpr double BodyBottomZCm = SeatHeightCm + BedJointThicknessCm;
	constexpr double BodyCentreZCm = BodyBottomZCm + BodyThicknessZCm / 2.0;

	/** MassKg * 980 is already a weight in uu (1 N = 100 uu is inside it). */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * 1 MPa over 1 cm2 is 100 N = 10000 uu. Deliberately not imported from production, so a wrong
	 * ForceUnitsPerMPaSqCm fails this file.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/*
	 * Independent statics: one rigid body's moments about one fulcrum against the most generous
	 * plastic bond. The RigidBlockOracle LP is a second confirmation in the test.
	 */

	constexpr double BodyMassKg =
		ClayDensityGramsPerCubicCm * BodyLengthXCm * WytheWidthCm * BodyThicknessZCm / 1000.0;

	constexpr double BodyWeightUu = BodyMassKg * GravityCmPerSecondSquared;

	/** Bed joint face areas: seat width across the full wythe. */
	constexpr double AnchorAreaSqCm = AnchorWidthXCm * WytheWidthCm;
	constexpr double PivotAreaSqCm = PivotWidthXCm * WytheWidthCm;

	/** Overturning moment about the fulcrum, uu.cm: W * (X_centroid - X_fulcrum). */
	double OverturningMomentUuCm()
	{
		return BodyWeightUu * (BodyCentreXCm - FulcrumXCm);
	}

	/**
	 * Most generous restoring moment: the fully plastic tension block. Each joint has two contact
	 * points (as in the oracle); each left of the fulcrum pulls at f_t over half the area times its
	 * lever. The pivot's right contact is on the fulcrum and restores nothing. Up to 3x the elastic
	 * first-crack capacity.
	 */
	double MaxPlasticRestoringMomentUuCm(double BondMPa)
	{
		const double AnchorPerContactUu = BondMPa * ForceUnitsPerMPaSqCmHere * (AnchorAreaSqCm / 2.0);
		const double PivotPerContactUu = BondMPa * ForceUnitsPerMPaSqCmHere * (PivotAreaSqCm / 2.0);

		const double AnchorLeftLeverCm = FulcrumXCm - (AnchorCentreXCm - AnchorWidthXCm / 2.0);
		const double AnchorRightLeverCm = FulcrumXCm - (AnchorCentreXCm + AnchorWidthXCm / 2.0);
		const double PivotLeftLeverCm = FulcrumXCm - (PivotCentreXCm - PivotWidthXCm / 2.0);

		return AnchorPerContactUu * AnchorLeftLeverCm
			+ AnchorPerContactUu * AnchorRightLeverCm
			+ PivotPerContactUu * PivotLeftLeverCm;
	}

	struct FTwoPathBody
	{
		FStructure Structure;

		int32 Anchor = INDEX_NONE;
		int32 Pivot = INDEX_NONE;
		int32 Body = INDEX_NONE;

		int32 AnchorJoint = INDEX_NONE;
		int32 PivotJoint = INDEX_NONE;
	};

	FPieceBox MakeBox(double CentreX, double WidthX, double CentreZ, double ThicknessZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(WidthX, WytheWidthCm, ThicknessZ) * 0.5;
		Box.CentreCm = FVector(CentreX, 0.0, CentreZ);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/** Lay the two seats and the body; join only the two body-seat pairs. */
	void Build(FTwoPathBody& Out)
	{
		const FPieceBox AnchorBox = MakeBox(AnchorCentreXCm, AnchorWidthXCm, SeatHeightCm / 2.0, SeatHeightCm);
		const FPieceBox PivotBox = MakeBox(PivotCentreXCm, PivotWidthXCm, SeatHeightCm / 2.0, SeatHeightCm);
		const FPieceBox BodyBox = MakeBox(BodyCentreXCm, BodyLengthXCm, BodyCentreZCm, BodyThicknessZCm);

		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Pivot = Out.Structure.AddPiece(BoxMassKg(PivotBox), /*bIsGrounded*/ true, PivotBox.CentreCm);
		Out.Body = Out.Structure.AddPiece(BoxMassKg(BodyBox), /*bIsGrounded*/ false, BodyBox.CentreCm);

		FConnection Joint;

		if (MakeInterface(Out.Anchor, AnchorBox, Out.Body, BodyBox,
				BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.AnchorJoint = Out.Structure.AddConnection(Joint);
		}

		if (MakeInterface(Out.Pivot, PivotBox, Out.Body, BodyBox,
				BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.PivotJoint = Out.Structure.AddConnection(Joint);
		}
	}

	/** Live pieces that have lost their path to the earth; Stranded counts as fallen. */
	TArray<int32> FallenPieces(const FTwoPathBody& Fixture)
	{
		TArray<int32> Fallen;

		for (int32 Piece = 0; Piece < Fixture.Structure.NumPieces(); ++Piece)
		{
			if (Fixture.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Fixture.Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				Fallen.Add(Piece);
			}
		}

		return Fallen;
	}

	int32 StrandedCount(const FTwoPathBody& Fixture)
	{
		int32 Stranded = 0;

		for (int32 Piece = 0; Piece < Fixture.Structure.NumPieces(); ++Piece)
		{
			if (!Fixture.Structure.IsPieceRemoved(Piece)
				&& Fixture.Structure.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}

		return Stranded;
	}

	/*
	 * The same fixture through FStructureBinding, for the block-cap test, which drives
	 * SolveAndBreak/ApplyResults like the acceptance tests. Same constants as above.
	 */

	struct FTwoPathBinding
	{
		FStructureBinding Binding;

		int32 Anchor = INDEX_NONE;
		int32 Pivot = INDEX_NONE;
		int32 Body = INDEX_NONE;

		int32 AnchorJoint = INDEX_NONE;
		int32 PivotJoint = INDEX_NONE;
	};

	void BuildBinding(FTwoPathBinding& Out)
	{
		const FPieceBox AnchorBox = MakeBox(AnchorCentreXCm, AnchorWidthXCm, SeatHeightCm / 2.0, SeatHeightCm);
		const FPieceBox PivotBox = MakeBox(PivotCentreXCm, PivotWidthXCm, SeatHeightCm / 2.0, SeatHeightCm);
		const FPieceBox BodyBox = MakeBox(BodyCentreXCm, BodyLengthXCm, BodyCentreZCm, BodyThicknessZCm);

		Out.Anchor = Out.Binding.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, /*Actor*/ nullptr, AnchorBox);
		Out.Pivot = Out.Binding.AddPiece(BoxMassKg(PivotBox), /*bIsGrounded*/ true, /*Actor*/ nullptr, PivotBox);
		Out.Body = Out.Binding.AddPiece(BoxMassKg(BodyBox), /*bIsGrounded*/ false, /*Actor*/ nullptr, BodyBox);

		FConnection Joint;

		if (MakeInterface(Out.Anchor, AnchorBox, Out.Body, BodyBox,
				BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.AnchorJoint = Out.Binding.AddConnection(Joint);
		}

		if (MakeInterface(Out.Pivot, PivotBox, Out.Body, BodyBox,
				BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.PivotJoint = Out.Binding.AddConnection(Joint);
		}
	}

	/** True when a live piece has lost every path to the earth. */
	bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		if (S.IsPieceRemoved(Piece))
		{
			return false;
		}

		const EPieceSupport Support = S.GetPieceSupport(Piece);
		return Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported;
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
}

/** A body on two load paths, past its tipping point, must fall. See the file header. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTwoLoadPathOverturningTest,
	"DestructionGame.Acceptance.Overturning.ABodyOnTwoLoadPathsPastTippingMustFall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTwoLoadPathOverturningTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace TwoLoadPathOverturningTestSupport;

	// Strength-basis preconditions; the verdict depends on them.

	TestEqual(TEXT("FIXTURE: the seats are bonded with the mean-basis 0.70 flexural bond"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.7);

	TestEqual(TEXT("FIXTURE: the mortar's coded crushing strength is M10's 10 MPa"),
		GeneralPurposeMortar.CompressiveStrengthMPa, 10.0);

	// The fixture must be past tipping even against the plastic bond.

	const double OverturningUuCm = OverturningMomentUuCm();
	const double RestoringUuCm = MaxPlasticRestoringMomentUuCm(GeneralPurposeMortar.TensileStrengthMPa);
	const double OverturningRatio = OverturningUuCm / RestoringUuCm;

	AddInfo(FString::Printf(
		TEXT("DERIVED: body weight %.10g uu, centroid X %.10g, fulcrum X %.10g; overturning %.10g "
			 "uu.cm against the MOST plastic restoring %.10g uu.cm at f_t = %g MPa => ratio %.10g (>1 falls)"),
		BodyWeightUu, BodyCentreXCm, FulcrumXCm, OverturningUuCm, RestoringUuCm,
		GeneralPurposeMortar.TensileStrengthMPa, OverturningRatio));

	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the body must be past tipping under the MOST charitable plastic bond by a "
				 "clear margin; overturning/restoring is %.10g and must exceed 2"),
			OverturningRatio),
		OverturningRatio > 2.0);

	// Build and check topology: three pieces, two bed joints under the body, complete geometry.

	FTwoPathBody Fixture;
	Build(Fixture);

	if (Fixture.AnchorJoint == INDEX_NONE || Fixture.PivotJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit both body-seat bed joints"));
		return false;
	}

	TestEqual(TEXT("FIXTURE: three pieces — two seats and one body"),
		Fixture.Structure.NumPieces(), 3);

	TestEqual(TEXT("FIXTURE: exactly two joints — the two load paths, and no seat-seat joint"),
		Fixture.Structure.NumConnections(), 2);

	TestTrue(TEXT("FIXTURE: every piece and joint must know where it is, or there is no eccentricity"),
		Fixture.Structure.HasCompleteGeometry());

	TestEqual(TEXT("FIXTURE: the anchor bed joint is the seat's face across the full wythe"),
		Fixture.Structure.GetConnection(Fixture.AnchorJoint).InterfaceAreaSqCm, AnchorAreaSqCm);

	TestEqual(TEXT("FIXTURE: the pivot bed joint is the seat's face across the full wythe"),
		Fixture.Structure.GetConnection(Fixture.PivotJoint).InterfaceAreaSqCm, PivotAreaSqCm);

	TestTrue(TEXT("FIXTURE: the anchor joint BEARS the body (a bed joint beneath it) — two load paths, not shear"),
		Fixture.Structure.GetJointRole(Fixture.AnchorJoint, Fixture.Body) == EJointRole::BedBeneath);

	TestTrue(TEXT("FIXTURE: the pivot joint BEARS the body (a bed joint beneath it) — the guard's blind spot"),
		Fixture.Structure.GetJointRole(Fixture.PivotJoint, Fixture.Body) == EJointRole::BedBeneath);

	// Cross-check: the RigidBlockOracle must find no admissible equilibrium at self-weight.

	RigidBlockOracle::FOracleProblem Problem;
	FString BridgeWhy;

	const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Fixture.Structure, Problem, BridgeWhy);

	TestTrue(
		*FString::Printf(TEXT("CROSS-CHECK: the oracle bridge must accept this 2D structure (%s)"), *BridgeWhy),
		bBridged);

	if (bBridged)
	{
		const RigidBlockOracle::FOracleResult Oracle = RigidBlockOracle::SolveRigidBlock(Problem);
		const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Oracle);

		AddInfo(FString::Printf(
			TEXT("CROSS-CHECK: oracle answered %d, lambda* %.10g, %d pivots — Falls means lambda* < 1"),
			Oracle.bAnswered ? 1 : 0, Oracle.Lambda, Oracle.SimplexIterations));

		TestTrue(TEXT("CROSS-CHECK: the oracle must ANSWER this fixture (a refusal cannot license the gate)"),
			Oracle.bAnswered);

		TestEqual(
			TEXT("CROSS-CHECK: the oracle must find NO equilibrium at self-weight (Falls) — this is what "
				 "licenses the Slice 2 gate to bring the body down"),
			static_cast<int32>(Outcome), static_cast<int32>(RigidBlockOracle::EOracleOutcome::Falls));

		TestTrue(
			*FString::Printf(TEXT("CROSS-CHECK: lambda* %.10g must sit clearly below 1"), Oracle.Lambda),
			Oracle.bAnswered && Oracle.Lambda < 0.9);
	}

	// Solve once non-destructively to record the as-built state, then cascade.

	Fixture.Structure.SolveLoads();

	const EPieceSupport BodyBefore = Fixture.Structure.GetPieceSupport(Fixture.Body);

	const int32 Passes = Fixture.Structure.SolveAndBreak();

	const TArray<int32> Fallen = FallenPieces(Fixture);
	const int32 Stranded = StrandedCount(Fixture);

	AddInfo(FString::Printf(
		TEXT("PRODUCTION today: body support-as-built %d (2=Supported); cascade ran %d breaking pass(es); "
			 "%d piece(s) fell; %d stranded"),
		static_cast<int32>(BodyBefore), Passes, Fallen.Num(), Stranded));

	// Nothing Stranded, so the verdict is not a routing limitation.

	TestEqual(
		TEXT("PRECONDITION: no piece may be Stranded — a verdict decided by the solver declining to route "
			 "is not a verdict about a leaning body"),
		Stranded, 0);

	// No admissible equilibrium at self-weight, so the body must lose the earth (DESIGN.md §7 gap 1).

	TestTrue(
		*FString::Printf(
			TEXT("REGRESSION: a body past its tipping point on two load paths must lose the earth; its "
				 "support reads %d after the cascade (want NOT Grounded/Supported), and the cascade ran %d "
				 "pass(es). The equilibrium gate fells it (LP infeasible at self-weight); the interim guard "
				 "it replaced could not, because N>=2 zeroed the moment and it excluded any second load path"),
			static_cast<int32>(Fixture.Structure.GetPieceSupport(Fixture.Body)), Passes),
		Fallen.Contains(Fixture.Body));

	TestTrue(TEXT("RED: the two grounded seats are the earth and must keep it — only the body falls"),
		Fixture.Structure.GetPieceSupport(Fixture.Anchor) == EPieceSupport::Grounded
			&& Fixture.Structure.GetPieceSupport(Fixture.Pivot) == EPieceSupport::Grounded);

	return true;
}

/**
 * The equilibrium gate is scoped by a block cap: authoritative at or below it, declining to the
 * router above it, which keeps synchronous LP work off large structures (§12 D2⁗).
 *
 * The cap is injectable (SetEquilibriumGateBlockCap), so the same 3-piece body is run twice:
 * cap 8 (gate authoritative, must catch and release the body) and cap 2 (gate declines; the bonded
 * body stands). The cap compares against live block count (NumPieces, pinned to 3); if that
 * definition changes, re-derive the caps.
 *
 * Driven through FStructureBinding SolveAndBreak + ApplyResults, so "caught" is a real release.
 * Outcome assertions only (DESIGN.md §4). No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTwoLoadPathGateScopedByBlockCapTest,
	"DestructionGame.Acceptance.Overturning.TheEquilibriumGateIsScopedByBlockCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTwoLoadPathGateScopedByBlockCapTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace TwoLoadPathOverturningTestSupport;

	// Preconditions: the same past-tipping, LP-falls body as above.

	TestEqual(TEXT("FIXTURE: the seats are bonded with the mean-basis 0.70 flexural bond"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.7);

	const double OverturningRatio =
		OverturningMomentUuCm() / MaxPlasticRestoringMomentUuCm(GeneralPurposeMortar.TensileStrengthMPa);

	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the body must be past tipping under the MOST charitable plastic bond; "
				 "overturning/restoring is %.10g and must exceed 2"),
			OverturningRatio),
		OverturningRatio > 2.0);

	// Built once for preconditions; each cascade run below builds fresh.
	FTwoPathBinding Probe;
	BuildBinding(Probe);

	if (Probe.AnchorJoint == INDEX_NONE || Probe.PivotJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit both body-seat bed joints"));
		return false;
	}

	const FStructure& ProbeS = Probe.Binding.GetStructure();

	// The block count the cap compares against, pinned so the caps sit either side of it.
	TestEqual(TEXT("FIXTURE: three live blocks — the count the cap gates on"), ProbeS.NumPieces(), 3);

	TestEqual(TEXT("FIXTURE: exactly two load paths beneath the body"),
		ProbeS.NumConnections(), 2);

	// Cross-check: the LP finds no equilibrium at self-weight.
	RigidBlockOracle::FOracleProblem Problem;
	FString BridgeWhy;

	const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(ProbeS, Problem, BridgeWhy);

	if (TestTrue(
			*FString::Printf(TEXT("CROSS-CHECK: the oracle bridge must accept this 2D structure (%s)"), *BridgeWhy),
			bBridged))
	{
		const RigidBlockOracle::FOracleResult Oracle = RigidBlockOracle::SolveRigidBlock(Problem);

		AddInfo(FString::Printf(
			TEXT("CROSS-CHECK: oracle answered %d, lambda* %.10g — Falls means lambda* < 1"),
			Oracle.bAnswered ? 1 : 0, Oracle.Lambda));

		TestTrue(
			TEXT("CROSS-CHECK: the LP must find NO equilibrium at self-weight (lambda* < 1) — this is "
				 "the body the gate must catch when it is authoritative"),
			Oracle.bAnswered
				&& RigidBlockOracle::OutcomeOf(Oracle) == RigidBlockOracle::EOracleOutcome::Falls
				&& Oracle.Lambda < 0.9);
	}

	// Run the pipeline twice, changing only the cap; fresh builds since SolveAndBreak is destructive.

	struct FRun
	{
		int32 Passes = 0;
		int32 Released = 0;
		int32 Stranded = 0;
		bool bBodyLostEarth = false;
		EPieceSupport BodySupport = EPieceSupport::Falling;
		bool bSeatsGrounded = false;
	};

	auto RunAtCap = [](int32 Cap) -> FRun
	{
		FTwoPathBinding Fx;
		BuildBinding(Fx);
		Fx.Binding.SetEquilibriumGateBlockCap(Cap);

		FRun R;
		R.Passes = Fx.Binding.SolveAndBreak();
		R.Released = Fx.Binding.ApplyResults();

		const FStructure& S = Fx.Binding.GetStructure();
		R.Stranded = StrandedCount(S);
		R.bBodyLostEarth = HasLostTheEarth(S, Fx.Body);
		R.BodySupport = S.GetPieceSupport(Fx.Body);
		R.bSeatsGrounded = S.GetPieceSupport(Fx.Anchor) == EPieceSupport::Grounded
			&& S.GetPieceSupport(Fx.Pivot) == EPieceSupport::Grounded;
		return R;
	};

	// At or below the cap the gate is authoritative and must catch the body.
	constexpr int32 AuthoritativeCap = 8;
	const FRun Auth = RunAtCap(AuthoritativeCap);

	// Above the cap the gate declines to the router.
	constexpr int32 DeclineCap = 2;
	const FRun Decline = RunAtCap(DeclineCap);

	AddInfo(FString::Printf(
		TEXT("CAP=%d (>=3, authoritative): passes %d, released %d, body-lost-earth %d, body support %d, "
			 "stranded %d. CAP=%d (<3, declines): passes %d, released %d, body-lost-earth %d, body support "
			 "%d, stranded %d. (support 1=Grounded,2=Supported,3=Stranded,0=Falling)"),
		AuthoritativeCap, Auth.Passes, Auth.Released, Auth.bBodyLostEarth ? 1 : 0,
		static_cast<int32>(Auth.BodySupport), Auth.Stranded,
		DeclineCap, Decline.Passes, Decline.Released, Decline.bBodyLostEarth ? 1 : 0,
		static_cast<int32>(Decline.BodySupport), Decline.Stranded));

	// Nothing Stranded in either run.
	TestEqual(TEXT("PRECONDITION: nothing Stranded at or below the cap"), Auth.Stranded, 0);
	TestEqual(TEXT("PRECONDITION: nothing Stranded above the cap"), Decline.Stranded, 0);

	TestTrue(TEXT("BOTH RUNS: the two grounded seats keep the earth — only the body is ever at stake"),
		Auth.bSeatsGrounded && Decline.bSeatsGrounded);

	/*
	 * Above the cap the body stands: the router's overturning check (PieceOverturnsOffItsSupports)
	 * spares a bonded, tension-tied body (f_t = 0.7). Dry seats would fell it.
	 */

	TestTrue(
		*FString::Printf(
			TEXT("ABOVE CAP: the body is bonded (tension-tied), so the router's overturning check spares "
				 "it — it keeps the earth (support %d) and nothing is released (%d)"),
			static_cast<int32>(Decline.BodySupport), Decline.Released),
		!Decline.bBodyLostEarth && Decline.Released == 0);

	// Below the cap the gate is authoritative and must catch and release the body.

	TestTrue(
		*FString::Printf(
			TEXT("RED, AT/BELOW CAP: with the gate authoritative the LP-infeasible body must lose the "
				 "earth and be released; it reads support %d, released %d, passes %d. Today production "
				 "stands it — no equilibrium gate exists — so this is the behaviour Slice 2 adds"),
			static_cast<int32>(Auth.BodySupport), Auth.Released, Auth.Passes),
		Auth.bBodyLostEarth && Auth.Released >= 1);

	// The cap alone flips the verdict on the same body.
	TestTrue(
		TEXT("RED, THE SEAM: the block cap alone must decide the gate's authority — the same body is "
			 "caught at/below the cap and NOT caught above it"),
		Auth.bBodyLostEarth && !Decline.bBodyLostEarth);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
