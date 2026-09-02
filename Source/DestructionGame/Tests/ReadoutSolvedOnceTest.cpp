// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE MIN-VIOLATION STRAIN READOUT IS SOLVED ONCE PER BELOW-CAP SETTLE, NOT ONCE PER PASS — RED.
 *
 * THE DEFECT. FStructure::BreakByEquilibrium is called once per cascade pass by SolveAndBreak, and
 * on BOTH of its answering arms it calls CacheMinViolationReadout(Problem) — the Stands arm and the
 * certified-Falls arm. CacheMinViolationReadout runs a WHOLE SEPARATE min-violation LP (a per-group
 * lexicographic-minimax loop, several full simplex solves). The Falls arm continues the cascade
 * whenever it severs anything (disposition AuthoritativeBroke), so a below-cap collapse that answers
 * over N passes solves the readout N times and keeps only the LAST (last-wins into
 * ConnectionReadoutCache). Measured: this is the cause of a 25x suite slowdown (leaning-stack
 * fixtures 0.03 s -> 11 s; the corbel depth test 9 s -> 55 min; the whole default suite 3 min -> 70
 * min).
 *
 * THE INTENDED BEHAVIOUR. Compute the readout EXACTLY ONCE, on the terminal (settled) pass. The
 * terminal pass of any cascade is the one that breaks nothing, i.e. disposition AuthoritativeNoBreak
 * — the Stands arm always returns that; the Falls arm returns it only when nothing was severed this
 * pass. So the readout should be cached only on an AuthoritativeNoBreak pass. This is BIT-IDENTICAL
 * to today's KEPT readout: today's last-wins already keeps the terminal pass's readout, so moving
 * the cache to fire only on the terminal pass changes WHEN the readout is computed, never WHAT ends
 * up in the cache.
 *
 * THE OBSERVABILITY HOOK (test scaffolding, no behaviour). FStructure::GetMinViolationReadoutSolveCount
 * returns how many times CacheMinViolationReadout ran during the last SolveAndBreak — a bare counter
 * bumped at the top of that method and zeroed once at the top of SolveAndBreak (NOT per pass, so it
 * accumulates across a cascade). It is the same shape as PhaseOnePivots on the oracle: it drives no
 * branch, it only lets this test watch how many separate readout LPs a settle pays for.
 *
 * ------------------------------------------------------------------------------------------------
 * TWO FIXTURES, MEASURED AGAINST TODAY'S PRODUCTION (nullrhi, no world) BEFORE THIS FILE WAS WRITTEN.
 * ------------------------------------------------------------------------------------------------
 *
 * FIXTURE A — THE POSITIVE CONTROL: a STANDING eccentric bearing (a single below-cap settle). A
 * brick rests on a narrower grounded seat so its centre of mass sits 4 cm out from the bearing's
 * centre — a genuine BENDING joint (M = N * 4 cm != 0), eccentric enough to need a little bond
 * tension but well within it, so the LP STANDS it. It breaks nothing, so it settles in ONE
 * AuthoritativeNoBreak pass and the readout is solved ONCE both today and after the fix. Its role is
 * to PIN the WHAT: the bending joint reads a PRESENT readout with a specific, measured utilisation,
 * and the counter reads exactly 1 for a legitimate single settle. Measured today:
 *     solves = 1, present = 1, N = 5343.94, M = 21375.76 (= N * 4.0 cm), util = 0.0007448, Supported.
 * This arm is GREEN on arrival — it is the anchor that makes the collapse's "2" the anomaly, and it
 * is where the "chosen bending joint's utilisation is unchanged" assertion lives with a real number.
 *
 * FIXTURE B — THE DRIVER: a TWO-LOAD-PATH OVERHANGING BODY that COLLAPSES below the cap (the
 * TwoLoadPathOverturning fixture's topology). A heavy body rests on two grounded seats both to the
 * LEFT of its centre of mass, so it is past tipping with no admissible equilibrium; the equilibrium
 * LP fells it. This is a genuine MULTI-PASS cascade: pass 1 severs the two bed joints
 * (AuthoritativeBroke), pass 2 breaks nothing (AuthoritativeNoBreak, terminal). Measured today:
 *     SolveAndBreak returns 1 breaking pass, but GetMinViolationReadoutSolveCount() == 2 — the
 *     readout was solved on BOTH answered passes. That 2 is the RED; the fix makes it 1.
 *
 * WHY THE COLLAPSE'S TERMINAL READOUT IS ABSENT, AND WHY THAT IS THE RIGHT "WHAT" GUARD. A released
 * body stays in the FStructure (SolveAndBreak severs its joints but does not remove the piece), so on
 * the terminal pass it is a FLOATING block. The min-violation readout LP keeps the equilibrium rows
 * HARD, and a floating block no force system can balance makes it fail closed (RigidBlockOracle's own
 * "a block no force system can balance" branch) — bPresent false, cache all-absent. So the SETTLED
 * readout of a genuine collapse is ABSENT on every connection, both today and after the fix. Pinning
 * that absence is precisely the WHAT-unchanged guard for THIS defect: it DISCRIMINATES the correct
 * fix (cache on the terminal AuthoritativeNoBreak pass -> floating body -> absent) from a WRONG guard
 * that cached on the AuthoritativeBroke pass instead, which would leave PASS 1's readout — solved on
 * the PRE-sever, fully-contacted, feasible structure, hence PRESENT with numbers — in the cache. A
 * present reading here would mean the fix cached on the wrong pass; absence is the only correct
 * settled answer. (Measured today: both severed bed joints read present = 0.)
 *
 * ASSERTIONS.
 *   Fixture A (green, the WHAT anchor): solves == 1; the bending bed joint's readout is present with
 *     util within tolerance of the measured 0.0007448 and a non-zero bending moment; the brick stands.
 *   Fixture B (the RED): the body loses the earth and both seats keep it with ZERO stranded (so the
 *     collapse is a genuine multi-pass fall, not a router no-op); GetMinViolationReadoutSolveCount()
 *     == 1 (RED: today 2); and every connection's settled readout is ABSENT (the terminal-pass guard).
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on the ordinary way (weight is MassKg * 980 inside
 * FStructure), everything is connected, and every assertion is on solver state, outcome, or the
 * cached readout / its solve counter. Same footing as the two-load-path acceptance test.
 *
 * UNITS are derived here (weight is mass * 980; nothing is imported from the code under test but the
 * producer MakeInterface and the mortar profile), so a wrong production constant disagrees rather
 * than agrees.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace ReadoutSolvedOnceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * SHARED GEOMETRY. Every length is centimetres, at Unreal's default 1 uu = 1 cm.
	 * ================================================================================ */

	/** Fired clay, 1.9 g/cm3 — the same figure every wall fixture uses. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** The single wythe: every piece is this deep on Y, so every joint's Y overlap is full. */
	constexpr double WytheWidthCm = 10.25;

	/** A 1 cm mortar bed: the separation every bed joint is formed across. */
	constexpr double BedJointThicknessCm = 1.0;

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

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

	/* ================================================================================
	 * FIXTURE A — THE STANDING ECCENTRIC BEARING (a genuine bending joint that stands).
	 *
	 * A brick (X in [0,28], centre X = 14) rests on a narrower grounded seat (X in [0,20],
	 * centre X = 10). The bearing's contact is the overlap X in [0,20], whose centre is X = 10;
	 * the brick's weight acts at X = 14, so the bed joint carries a moment M = N * (14 - 10) =
	 * N * 4 cm. e = 4 cm sits just outside the no-tension kern (overlap half-width / 3 = 3.33 cm)
	 * so a little bond tension is needed, well within GeneralPurposeMortar — the LP STANDS it.
	 * ================================================================================ */

	struct FStandingBearing
	{
		FStructure Structure;
		int32 Seat = INDEX_NONE;
		int32 Brick = INDEX_NONE;
		int32 BedJoint = INDEX_NONE;
	};

	void BuildStandingBearing(FStandingBearing& Out)
	{
		const FPieceBox SeatBox = MakeBox(/*X*/ 10.0, /*w*/ 20.0, /*Z*/ 10.0, /*t*/ 20.0);
		const FPieceBox BrickBox = MakeBox(/*X*/ 14.0, /*w*/ 28.0, /*Z*/ 26.0, /*t*/ 10.0);

		Out.Seat = Out.Structure.AddPiece(BoxMassKg(SeatBox), /*bIsGrounded*/ true, SeatBox.CentreCm);
		Out.Brick = Out.Structure.AddPiece(BoxMassKg(BrickBox), /*bIsGrounded*/ false, BrickBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Seat, SeatBox, Out.Brick, BrickBox, BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}
	}

	/* The measured settled readout on the bending bed joint (today's production, nullrhi). */
	constexpr double ControlUtilisation = 0.0007448;
	constexpr double ControlMomentUuCm = 21375.76;

	/* ================================================================================
	 * FIXTURE B — THE TWO-LOAD-PATH OVERHANGING BODY that collapses below the cap.
	 * Both grounded seats sit to the LEFT of the body's centre of mass, so it is past
	 * tipping with no admissible equilibrium and the LP fells it in a multi-pass cascade.
	 * ================================================================================ */

	constexpr double SeatHeightCm = 20.0;
	constexpr double AnchorCentreXCm = 0.0;
	constexpr double AnchorWidthXCm = 5.0;
	constexpr double PivotCentreXCm = 10.0;
	constexpr double PivotWidthXCm = 10.0;
	constexpr double BodyLeftXCm = AnchorCentreXCm - AnchorWidthXCm / 2.0;
	constexpr double BodyLengthXCm = 300.0;
	constexpr double BodyCentreXCm = (BodyLeftXCm + (BodyLeftXCm + BodyLengthXCm)) / 2.0;
	constexpr double BodyThicknessZCm = 40.0;
	constexpr double BodyBottomZCm = SeatHeightCm + BedJointThicknessCm;
	constexpr double BodyCentreZCm = BodyBottomZCm + BodyThicknessZCm / 2.0;

	struct FTwoPathBody
	{
		FStructure Structure;
		int32 Anchor = INDEX_NONE;
		int32 Pivot = INDEX_NONE;
		int32 Body = INDEX_NONE;
		int32 AnchorJoint = INDEX_NONE;
		int32 PivotJoint = INDEX_NONE;
	};

	void BuildTwoPathBody(FTwoPathBody& Out)
	{
		const FPieceBox AnchorBox = MakeBox(AnchorCentreXCm, AnchorWidthXCm, SeatHeightCm / 2.0, SeatHeightCm);
		const FPieceBox PivotBox = MakeBox(PivotCentreXCm, PivotWidthXCm, SeatHeightCm / 2.0, SeatHeightCm);
		const FPieceBox BodyBox = MakeBox(BodyCentreXCm, BodyLengthXCm, BodyCentreZCm, BodyThicknessZCm);

		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Pivot = Out.Structure.AddPiece(BoxMassKg(PivotBox), /*bIsGrounded*/ true, PivotBox.CentreCm);
		Out.Body = Out.Structure.AddPiece(BoxMassKg(BodyBox), /*bIsGrounded*/ false, BodyBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Anchor, AnchorBox, Out.Body, BodyBox, BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.AnchorJoint = Out.Structure.AddConnection(Joint);
		}
		if (MakeInterface(Out.Pivot, PivotBox, Out.Body, BodyBox, BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.PivotJoint = Out.Structure.AddConnection(Joint);
		}
	}

	bool HasEarth(const FStructure& S, int32 Piece)
	{
		const EPieceSupport Support = S.GetPieceSupport(Piece);
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 Stranded = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (!S.IsPieceRemoved(P) && S.GetPieceSupport(P) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}
		return Stranded;
	}
}

/**
 * The min-violation strain readout is solved once per below-cap settle, not once per answered pass.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReadoutSolvedOnceTest,
	"DestructionGame.Acceptance.StrainReadout.ReadoutSolvedOncePerBelowCapCascade",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FReadoutSolvedOnceTest::RunTest(const FString& Parameters)
{
	using namespace ReadoutSolvedOnceTestSupport;

	/* ------------------------------------------------------------------ *
	 * FIXTURE A — THE POSITIVE CONTROL (green): one settle, one readout,
	 * a present bending reading with a specific utilisation. This pins the
	 * WHAT the readout computes and shows the counter reads 1 when it must.
	 * ------------------------------------------------------------------ */

	FStandingBearing A;
	BuildStandingBearing(A);

	if (A.BedJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE A: the producer must emit the seat-to-brick bed joint"));
		return false;
	}

	A.Structure.SolveAndBreak();

	const int32 ControlSolves = A.Structure.GetMinViolationReadoutSolveCount();
	const FStructure::FConnectionReadout ControlReadout = A.Structure.GetConnectionReadout(A.BedJoint);

	AddInfo(FString::Printf(
		TEXT("CONTROL (standing eccentric bearing): solves=%d present=%d N=%.9g M=%.9g util=%.9g support=%d"),
		ControlSolves, ControlReadout.bPresent ? 1 : 0, ControlReadout.NormalUu,
		ControlReadout.MomentUuCm, ControlReadout.Utilisation, (int32)A.Structure.GetPieceSupport(A.Brick)));

	TestTrue(TEXT("CONTROL: the eccentric bearing STANDS (below-cap LP carries the brick)"),
		HasEarth(A.Structure, A.Brick));

	TestEqual(TEXT("CONTROL: a single settle that breaks nothing solves the readout exactly once"),
		ControlSolves, 1);

	TestTrue(TEXT("CONTROL: the bending bed joint reads a PRESENT readout"),
		ControlReadout.bPresent);

	/* A genuine BENDING joint: the moment is the weight times the 4 cm eccentricity, not zero. */
	TestTrue(*FString::Printf(TEXT("CONTROL: the joint bends (M = %.9g ~ measured %.9g, non-zero)"),
			ControlReadout.MomentUuCm, ControlMomentUuCm),
		FMath::Abs(ControlReadout.MomentUuCm - ControlMomentUuCm) <= 1.0 && ControlReadout.MomentUuCm > 1.0);

	/* THE PINNED WHAT: the settled utilisation on the chosen bending joint equals today's value. */
	TestTrue(*FString::Printf(TEXT("CONTROL: settled bending utilisation %.9g == today's %.9g"),
			ControlReadout.Utilisation, ControlUtilisation),
		FMath::Abs(ControlReadout.Utilisation - ControlUtilisation) <= 1.0e-6);

	/* ------------------------------------------------------------------ *
	 * FIXTURE B — THE DRIVER (red): a genuine multi-pass below-cap collapse.
	 * ------------------------------------------------------------------ */

	FTwoPathBody B;
	BuildTwoPathBody(B);

	if (B.AnchorJoint == INDEX_NONE || B.PivotJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE B: the producer must emit both body-seat bed joints"));
		return false;
	}

	const int32 BreakingPasses = B.Structure.SolveAndBreak();
	const int32 CollapseSolves = B.Structure.GetMinViolationReadoutSolveCount();

	AddInfo(FString::Printf(
		TEXT("COLLAPSE (two-load-path body): breakingPasses=%d readoutSolves=%d bodySupport=%d "
			 "anchorJointGiven=%d pivotJointGiven=%d anchorReadoutPresent=%d pivotReadoutPresent=%d"),
		BreakingPasses, CollapseSolves, (int32)B.Structure.GetPieceSupport(B.Body),
		B.Structure.GetConnection(B.AnchorJoint).HasGiven() ? 1 : 0,
		B.Structure.GetConnection(B.PivotJoint).HasGiven() ? 1 : 0,
		B.Structure.GetConnectionReadout(B.AnchorJoint).bPresent ? 1 : 0,
		B.Structure.GetConnectionReadout(B.PivotJoint).bPresent ? 1 : 0));

	/* OUTCOME — the collapse is a genuine multi-pass fall, not a router no-op. The body loses the
	 * earth, both grounded seats keep it, and NOTHING is stranded (so a routing limitation cannot
	 * wear the collapse's clothes). This only happens when the below-cap equilibrium LP is the
	 * authority — the router alone reads the two-load-path body as safe. */
	TestFalse(TEXT("COLLAPSE: the overhanging body has lost its path to the earth"),
		HasEarth(B.Structure, B.Body));
	TestTrue(TEXT("COLLAPSE: the anchor seat keeps the earth"),
		HasEarth(B.Structure, B.Anchor));
	TestTrue(TEXT("COLLAPSE: the pivot seat keeps the earth"),
		HasEarth(B.Structure, B.Pivot));
	TestEqual(TEXT("COLLAPSE: zero pieces stranded — a genuine fall, not a routing knot"),
		StrandedCount(B.Structure), 0);

	/* THE RED — the readout must be solved ONCE across the whole cascade, on the terminal settled
	 * pass. Today it is solved on every answered pass: an AuthoritativeBroke pass 1 and a terminal
	 * AuthoritativeNoBreak pass 2, so this reads 2. The fix caches only on the AuthoritativeNoBreak
	 * pass, making it 1 without changing the kept readout. */
	TestEqual(TEXT("[RED] the min-violation readout is solved exactly once per below-cap settle"),
		CollapseSolves, 1);

	/* THE WHAT-UNCHANGED GUARD — the settled readout of a genuine collapse is ABSENT on every
	 * connection: the released body floats, so the min-violation LP fails closed. A PRESENT reading
	 * here would mean the fix cached on the AuthoritativeBroke (pre-sever, feasible) pass instead of
	 * the terminal one — the exact wrong-pass mistake this pins against. */
	TestFalse(TEXT("WHAT: the settled readout on the severed anchor bed joint is absent"),
		B.Structure.GetConnectionReadout(B.AnchorJoint).bPresent);
	TestFalse(TEXT("WHAT: the settled readout on the severed pivot bed joint is absent"),
		B.Structure.GetConnectionReadout(B.PivotJoint).bPresent);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
