// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/PieceInspection.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 6b FINAL — NO STALE LP READOUT SURVIVES A GATE DECLINE, RED (RED B, the review's finding).
 *
 * THE 6b-CORE REVIEW'S BLOCKING FINDING. The cached strain readout is invalidated by exactly one
 * line — ConnectionReadoutCache.Reset() at the TOP of SolveAndBreak (Structure.cpp) — which fires
 * ONCE PER SolveAndBreak, not once per pass. Every below-cap ANSWERING pass refills the cache
 * (CacheMinViolationReadout does a full Init rebuild), but the DECLINE returns
 * (EEquilibriumGateDisposition::DeclinedToRouter — over cap, no geometry, bridge refusal, an
 * Unanswerable LP, or an uncertified mechanism) return WITHOUT refilling it. So a settle whose
 * terminal disposition DECLINES must not leave a prior solve's LP readout in the cache for
 * GetConnectionReadout — and (after RED A) InspectPiece — to serve as present: a joint the router now
 * owns would read a stale LP number, four times too comfortable or worse, with total confidence.
 *
 * THE INVARIANT THIS PINS: after a settle whose gate DECLINED to the router, GetConnectionReadout is
 * ABSENT for every connection (the router owns the overlay), and InspectPiece therefore shows the
 * router's utilisation — never an LP number cached by an earlier, below-cap solve.
 *
 * WHY THE CONSTRUCTION IS A CROSS-CALL CAP FLIP, AND WHAT COULD NOT BE DONE. The review's exact
 * defect — an ANSWERING pass caches, then a LATER pass in the SAME cascade declines — is NOT
 * deterministically constructible through the public API:
 *
 *   - Over-cap and no-geometry declines are whole-structure and constant across a cascade's passes.
 *   - A bridge-refusal decline needs a tombstone / degenerate / out-of-plane normal; SEVERING a
 *     joint only SKIPS it in the bridge (RigidBlockBridge.cpp), so bridge acceptance is MONOTONIC
 *     across a cascade — it can never begin to refuse a remainder it accepted before.
 *   - That leaves only the NUMERICAL declines (an Unanswerable phase-1 failure, or a Farkas-
 *     uncertified mechanism). Those are real but order-dependent, there is no settable solver budget
 *     to force one, and no small fixture the project owns reaches one mid-cascade. See the report.
 *
 * So the natural within-call answered-then-declined cascade was NOT forceable. This test pins the
 * SAME cache-lifecycle seam the review flagged, the cheapest sound way: a below-cap settle CACHES a
 * present LP readout, then a second settle with the cap dropped below the piece count DECLINES, and
 * the invariant demands the stale reading is gone. It EXERCISES the Reset line directly — deleting
 * that line makes this test RED (the stale below-cap readout survives the decline), which is the
 * bite proof reported alongside it.
 *
 * WHAT DEV STILL OWES. Because the cross-call sequence is already protected by the per-call Reset,
 * this test is GREEN ON ARRIVAL — it is a REGRESSION NET on the invariant, not a driver. The
 * within-call numerical-decline window it cannot reach is the latent hole the review named; the
 * defensive fix (a per-pass Reset at the top of BreakByEquilibrium, or a cache clear on every
 * DeclinedToRouter return) closes it, and this net guards that the fix does not regress the reachable
 * behaviour. The report states this plainly so nobody reads a green here as "the hole is closed".
 *
 * THE FIXTURE — THE 6b DETERMINATE TENSION HANG. A free brick hangs below a grounded anchor from one
 * bed joint whose only finite strength axis is tension. Below the cap the LP stands it and caches a
 * clearly-non-zero utilisation (~0.3724); the router strands it and reads ~0. That sharp gap is what
 * makes a stale LP reading VISIBLE after the decline — a leftover 0.3724 where the router says ~0.
 *
 * UNITS are derived here (1 MPa over 1 cm2 = 100 * 100 = 10000 uu), never imported.
 *
 * NEEDS A TICKING WORLD: NO. Every assertion is on the cache, InspectPiece, or the router accessor.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace StaleReadoutAfterDeclineSupport
{
	using namespace DestructionLayout;

	constexpr double ClayDensityGramsPerCubicCm = 1.9;
	constexpr double WytheWidthCm = 10.25;
	constexpr double JointThicknessCm = 1.0;
	constexpr double GravityCmPerSecondSquared = 980.0;
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;
	constexpr double UncappedHere = 1.0e12;
	constexpr double HangPlanXCm = 10.0;
	constexpr double HangHeightCm = 10.0;

	/** Sized so the LP utilisation is a comfortable, clearly-non-zero ~0.37 the router's ~0 can't match. */
	constexpr double TensileMPa = 0.005;

	FPieceBox MakeBox(double CentreX, double SizeX, double CentreZ, double SizeZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(SizeX, WytheWidthCm, SizeZ) * 0.5;
		Box.CentreCm = FVector(CentreX, 0.0, CentreZ);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	FConnectionStrength TensionHangBond()
	{
		FConnectionStrength S;
		S.TensileStrengthMPa = TensileMPa;
		S.CompressiveStrengthMPa = UncappedHere;
		S.ShearCohesionMPa = UncappedHere;
		S.FrictionCoefficient = 0.0;
		return S;
	}

	struct FTensionHang
	{
		FStructure Structure;

		int32 Anchor = INDEX_NONE;
		int32 Hang = INDEX_NONE;
		int32 BedJoint = INDEX_NONE;
	};

	void Build(FTensionHang& Out)
	{
		const FPieceBox AnchorBox = MakeBox(/*X*/ 0.0, HangPlanXCm, /*Z*/ 40.0, HangHeightCm);
		const FPieceBox HangBox = MakeBox(/*X*/ 0.0, HangPlanXCm, /*Z*/ 29.0, HangHeightCm);

		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Hang = Out.Structure.AddPiece(BoxMassKg(HangBox), /*bIsGrounded*/ false, HangBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Anchor, AnchorBox, Out.Hang, HangBox, JointThicknessCm, TensionHangBond(), Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}
	}

	double HangMassKg() { return ClayDensityGramsPerCubicCm * HangPlanXCm * WytheWidthCm * HangHeightCm / 1000.0; }
	double HangWeightUu() { return HangMassKg() * GravityCmPerSecondSquared; }
	double JointAreaSqCm() { return HangPlanXCm * WytheWidthCm; }
	double TensionCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm(); }
	double ExpectedUtilisation() { return HangWeightUu() / TensionCapacityUu(); }

	bool Near(double A, double B, double Tol) { return FMath::Abs(A - B) <= Tol; }

	const FJointInspection* JointRowFor(const FPieceInspection& Inspection, int32 ConnectionIndex)
	{
		for (const FJointInspection& Row : Inspection.Joints)
		{
			if (Row.ConnectionIndex == ConnectionIndex)
			{
				return &Row;
			}
		}
		return nullptr;
	}
}

/**
 * After a settle whose gate declined to the router, no stale LP readout is served — the cache is
 * absent and the overlay reads the router.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStaleReadoutAfterDeclineTest,
	"DestructionGame.Acceptance.StrainReadout.NoStaleReadoutAfterGateDeclines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStaleReadoutAfterDeclineTest::RunTest(const FString& Parameters)
{
	using namespace StaleReadoutAfterDeclineSupport;

	/* ------------------------------------------------------------------ *
	 * PHASE 1 — SETTLE BELOW THE CAP, WHICH CACHES A PRESENT LP READOUT.
	 * ------------------------------------------------------------------ */

	FTensionHang S;
	Build(S);

	if (S.BedJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit the anchor-to-hang bed joint"));
		return false;
	}

	S.Structure.SetEquilibriumGateBlockCap(64); /* 2 pieces << cap => below cap, gate authoritative */
	S.Structure.SolveAndBreak();

	const FStructure::FConnectionReadout Cached = S.Structure.GetConnectionReadout(S.BedJoint);

	AddInfo(FString::Printf(
		TEXT("PHASE 1 (below cap): cached readout present %d, util %.6g (expected LP %.6g)"),
		Cached.bPresent ? 1 : 0, Cached.Utilisation, ExpectedUtilisation()));

	/* Precondition — the below-cap solve DID cache a present, clearly-non-zero LP reading. Without
	 * this the phase-2 decline has nothing stale to leak, and the RED below would prove nothing. */
	TestTrue(TEXT("PRECONDITION: the below-cap settle cached a present LP readout (6b core is built)"),
		Cached.bPresent);
	TestTrue(*FString::Printf(TEXT("PRECONDITION: the cached LP util %.6g is the hand-oracle %.6g, sharply non-zero"),
			Cached.Utilisation, ExpectedUtilisation()),
		Near(Cached.Utilisation, ExpectedUtilisation(), 2.0e-3) && Cached.Utilisation > 0.1);

	/* ------------------------------------------------------------------ *
	 * PHASE 2 — DROP THE CAP BELOW THE PIECE COUNT AND SETTLE AGAIN. The
	 * gate now DECLINES (the same DeclinedToRouter disposition a mid-cascade
	 * numerical refusal takes), so no min-violation LP is solved and nothing
	 * refills the cache. The invariant: the phase-1 LP reading must be GONE.
	 * ------------------------------------------------------------------ */

	S.Structure.SetEquilibriumGateBlockCap(1); /* 2 pieces > cap => the gate declines to the router */
	S.Structure.SolveAndBreak();

	const FStructure::FConnectionReadout AfterDecline = S.Structure.GetConnectionReadout(S.BedJoint);
	const double RouterUtil = S.Structure.GetConnectionUtilisation(S.BedJoint);

	AddInfo(FString::Printf(
		TEXT("PHASE 2 (declined): readout present %d, util %.6g; router util %.6g. A stale reading would be the "
			 "phase-1 %.6g surviving the decline."),
		AfterDecline.bPresent ? 1 : 0, AfterDecline.Utilisation, RouterUtil, Cached.Utilisation));

	/* THE INVARIANT — after the gate declines, the cache serves no LP reading; the router owns the
	 * overlay. Deleting the top-of-SolveAndBreak Reset makes this RED: the phase-1 0.3724 survives. */
	TestEqual(TEXT("DECLINED [invariant]: GetConnectionReadout is absent — no stale LP reading survives the decline"),
		AfterDecline.bPresent, false);

	/* And the player-visible consequence: after RED A wires the overlay to the readout, a stale
	 * present reading here would be SHOWN. Pin that InspectPiece falls back to the router, not the
	 * leftover LP number. (Today InspectPiece already reads the router; this holds after RED A too
	 * BECAUSE the cache is absent — and goes RED with the Reset deleted once RED A is wired.) */
	const FPieceInspection Inspection = InspectPiece(S.Structure, S.Hang);
	const FJointInspection* Row = JointRowFor(Inspection, S.BedJoint);
	if (Row == nullptr)
	{
		AddError(TEXT("INSPECT: InspectPiece must emit a row for the hang's one bed joint"));
		return false;
	}

	TestTrue(
		*FString::Printf(
			TEXT("DECLINED [invariant]: the overlay shows the router util %.6g, not a stale LP reading %.6g"),
			RouterUtil, Cached.Utilisation),
		Near(Row->Utilisation, RouterUtil, 1.0e-9));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
