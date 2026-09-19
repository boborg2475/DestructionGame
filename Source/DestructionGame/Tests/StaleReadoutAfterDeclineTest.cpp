// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/PieceInspection.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A stale earlier-solve LP readout must not survive a settle whose gate declines to the
 * router. ConnectionReadoutCache.Reset() (top of SolveAndBreak) fires once per call, not
 * once per pass: a below-cap ANSWERING pass refills it, but a DECLINE
 * (EEquilibriumGateDisposition::DeclinedToRouter — over cap, no geometry, bridge refusal,
 * an Unanswerable LP, or an uncertified mechanism) returns without refilling it. After a
 * decline, GetConnectionReadout must be absent for every connection so InspectPiece falls
 * back to the router's utilisation instead of serving a leftover LP number.
 *
 * The review's exact defect — an ANSWERING pass caches, then a LATER pass in the SAME
 * cascade declines — is not constructible through the public API: over-cap/no-geometry
 * declines are whole-structure and constant across a cascade, bridge acceptance is
 * monotonic (severing only skips a joint, never un-accepts one), and the remaining
 * numerical declines are order-dependent with no small fixture that reaches one
 * mid-cascade. So this test exercises the same cache-lifecycle seam across two separate
 * settles instead: a below-cap solve caches a present LP readout, then a second solve with
 * the cap dropped below the piece count declines, and the reading must be gone. Deleting
 * the Reset line makes this RED.
 *
 * Because the cross-call sequence is already protected by the per-call Reset, this test is
 * green on arrival — a regression net on the invariant, not a driver for it. The within-call
 * numerical-decline window is a separate, still-open gap.
 *
 * Fixture: a free brick hangs below a grounded anchor from one bed joint whose only finite
 * strength axis is tension. Below the cap the LP stands it (~0.3724 utilisation); the
 * router strands it (~0) — a gap sharp enough to make a stale reading visible.
 *
 * Units derived here (1 MPa over 1 cm2 = 100*100 = 10000 uu), never imported. No ticking
 * world needed — every assertion reads the cache, InspectPiece, or the router accessor.
 * Named namespace: a unity build merges many files into one translation unit.
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
 * After a settle whose gate declined to the router, no stale LP readout is served — the
 * cache is absent and the overlay reads the router. No ticking world needed; see the file
 * header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStaleReadoutAfterDeclineTest,
	"DestructionGame.Acceptance.StrainReadout.NoStaleReadoutAfterGateDeclines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStaleReadoutAfterDeclineTest::RunTest(const FString& Parameters)
{
	using namespace StaleReadoutAfterDeclineSupport;

	// Phase 1 — settle below the cap, which caches a present LP readout.

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

	/*
	 * Precondition — the below-cap solve cached a present, clearly-non-zero LP reading;
	 * without it the phase-2 decline has nothing stale to leak.
	 */
	TestTrue(TEXT("PRECONDITION: the below-cap settle cached a present LP readout (6b core is built)"),
		Cached.bPresent);
	TestTrue(*FString::Printf(TEXT("PRECONDITION: the cached LP util %.6g is the hand-oracle %.6g, sharply non-zero"),
			Cached.Utilisation, ExpectedUtilisation()),
		Near(Cached.Utilisation, ExpectedUtilisation(), 2.0e-3) && Cached.Utilisation > 0.1);

	/*
	 * Phase 2 — drop the cap below the piece count and settle again. The gate now declines
	 * (the same DeclinedToRouter disposition a mid-cascade numerical refusal takes), so
	 * nothing refills the cache; the phase-1 LP reading must be gone.
	 */

	S.Structure.SetEquilibriumGateBlockCap(1); /* 2 pieces > cap => the gate declines to the router */
	S.Structure.SolveAndBreak();

	const FStructure::FConnectionReadout AfterDecline = S.Structure.GetConnectionReadout(S.BedJoint);
	const double RouterUtil = S.Structure.GetConnectionUtilisation(S.BedJoint);

	AddInfo(FString::Printf(
		TEXT("PHASE 2 (declined): readout present %d, util %.6g; router util %.6g. A stale reading would be the "
			 "phase-1 %.6g surviving the decline."),
		AfterDecline.bPresent ? 1 : 0, AfterDecline.Utilisation, RouterUtil, Cached.Utilisation));

	/*
	 * The invariant — after the gate declines, the cache serves no LP reading; the router
	 * owns the overlay. Deleting the top-of-SolveAndBreak Reset makes this RED: the
	 * phase-1 0.3724 survives.
	 */
	TestEqual(TEXT("DECLINED [invariant]: GetConnectionReadout is absent — no stale LP reading survives the decline"),
		AfterDecline.bPresent, false);

	/*
	 * The player-visible consequence: InspectPiece must fall back to the router's
	 * utilisation, not a leftover LP number, whenever the cache is absent.
	 */
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
