// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/PieceInspection.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A stale LP readout must not survive a settle whose gate declines to the router
 * (DeclinedToRouter). ConnectionReadoutCache.Reset() runs once per SolveAndBreak call; a decline
 * does not refill it, so GetConnectionReadout must be absent and InspectPiece must show the
 * router's utilisation.
 *
 * A mid-cascade decline after an answering pass cannot be built through the public API, so this
 * uses two settles: below the cap (caches a reading), then with the cap below the piece count
 * (declines). Green on arrival, a regression net: deleting the Reset makes it red. The
 * within-call numerical-decline window is still an open gap.
 *
 * Fixture: a brick hangs from a grounded anchor on a tension-only joint; the LP reads ~0.3724,
 * the router ~0. Units derived here (1 MPa over 1 cm2 = 10000 uu). No world. Named namespace.
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

	/** Gives an LP utilisation of ~0.37, clearly distinct from the router's ~0. */
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

/** After a declined settle the readout cache is absent and the overlay reads the router. No world. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStaleReadoutAfterDeclineTest,
	"DestructionGame.Acceptance.StrainReadout.NoStaleReadoutAfterGateDeclines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStaleReadoutAfterDeclineTest::RunTest(const FString& Parameters)
{
	using namespace StaleReadoutAfterDeclineSupport;

	// Phase 1: settle below the cap, caching an LP readout.

	FTensionHang S;
	Build(S);

	if (S.BedJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit the anchor-to-hang bed joint"));
		return false;
	}

	S.Structure.SetEquilibriumGateBlockCap(64); // 2 pieces, below the cap: the gate answers
	S.Structure.SolveAndBreak();

	const FStructure::FConnectionReadout Cached = S.Structure.GetConnectionReadout(S.BedJoint);

	AddInfo(FString::Printf(
		TEXT("PHASE 1 (below cap): cached readout present %d, util %.6g (expected LP %.6g)"),
		Cached.bPresent ? 1 : 0, Cached.Utilisation, ExpectedUtilisation()));

	// Precondition: a non-zero LP reading is cached, so phase 2 has something stale to leak.
	TestTrue(TEXT("PRECONDITION: the below-cap settle cached a present LP readout (6b core is built)"),
		Cached.bPresent);
	TestTrue(*FString::Printf(TEXT("PRECONDITION: the cached LP util %.6g is the hand-oracle %.6g, sharply non-zero"),
			Cached.Utilisation, ExpectedUtilisation()),
		Near(Cached.Utilisation, ExpectedUtilisation(), 2.0e-3) && Cached.Utilisation > 0.1);

	// Phase 2: with the cap below the piece count the gate declines, so the reading must be gone.

	S.Structure.SetEquilibriumGateBlockCap(1); // 2 pieces, above the cap: the gate declines
	S.Structure.SolveAndBreak();

	const FStructure::FConnectionReadout AfterDecline = S.Structure.GetConnectionReadout(S.BedJoint);
	const double RouterUtil = S.Structure.GetConnectionUtilisation(S.BedJoint);

	AddInfo(FString::Printf(
		TEXT("PHASE 2 (declined): readout present %d, util %.6g; router util %.6g. A stale reading would be the "
			 "phase-1 %.6g surviving the decline."),
		AfterDecline.bPresent ? 1 : 0, AfterDecline.Utilisation, RouterUtil, Cached.Utilisation));

	// Without the Reset in SolveAndBreak, the phase-1 0.3724 would survive here.
	TestEqual(TEXT("DECLINED [invariant]: GetConnectionReadout is absent — no stale LP reading survives the decline"),
		AfterDecline.bPresent, false);

	// Player-visible: InspectPiece falls back to the router's utilisation.
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
