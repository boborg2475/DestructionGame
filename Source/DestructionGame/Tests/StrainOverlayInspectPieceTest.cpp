// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/PieceInspection.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Slice 6b final: below the equilibrium cap, InspectPiece's per-joint Utilisation comes from the
 * cached LP readout (GetConnectionReadout) when present; above the cap, or when absent, it stays
 * the router's GetConnectionUtilisation. This is where the min-violation readout first reaches the
 * player.
 *
 * Fixture: one free brick hanging below a grounded anchor from a bed joint whose only finite axis is
 * tension. The router cannot route an upward hang and reads ~0; the LP carries the weight and reads
 * W / (f_t * Conv * A) ~ 0.3724, derived by hand here. Matching the LP and differing from the router
 * proves the switch.
 *
 * The above-cap arm (cap forced below the piece count) checks the router fallback survives.
 *
 * Units derived here, not imported. No world needed. Named namespace for unity builds.
 */
namespace StrainOverlayInspectPieceSupport
{
	using namespace DestructionLayout;

	/** Fired clay, 1.9 g/cm3. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Depth on Y of every piece, so each joint's Y overlap is full. */
	constexpr double WytheWidthCm = 10.25;

	/** Bed joint thickness between anchor and hang. */
	constexpr double JointThicknessCm = 1.0;

	/** MassKg * 980 is already a weight in uu (1 N = 100 uu is inside it). */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 MPa over 1 cm2 = 10000 uu. Not imported, so a wrong production constant fails. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/** Above the oracle's UncappedStrengthMPa (1e9), so no row is assembled for it. */
	constexpr double UncappedHere = 1.0e12;

	/** The hang's plan face: 10 cm on X, full wythe on Y. */
	constexpr double HangPlanXCm = 10.0;
	constexpr double HangHeightCm = 10.0;

	/** Tension bond sized for utilisation ~0.37: stands with margin, yet clearly non-zero. */
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

	/** Weak finite tension, every other axis uncapped, so tension is the only governing axis. */
	FConnectionStrength TensionHangBond()
	{
		FConnectionStrength S;
		S.TensileStrengthMPa = TensileMPa;       // finite: the tension row is written
		S.CompressiveStrengthMPa = UncappedHere; // no crushing row
		S.ShearCohesionMPa = UncappedHere;       // no friction row
		S.FrictionCoefficient = 0.0;
		// MaxShearStrengthMPa is unbounded by default, so no ceiling row.
		return S;
	}

	struct FTensionHang
	{
		FStructure Structure;

		int32 Anchor = INDEX_NONE; // grounded, above
		int32 Hang = INDEX_NONE;   // free, below

		int32 BedJoint = INDEX_NONE; // the one joint: Anchor above, Hang below
	};

	void Build(FTensionHang& Out)
	{
		// Anchor: grounded, bottom face at Z = 35.
		const FPieceBox AnchorBox = MakeBox(/*X*/ 0.0, HangPlanXCm, /*Z*/ 40.0, HangHeightCm);

		// Hang: free, top face at Z = 34, one 1 cm bed joint below the anchor.
		const FPieceBox HangBox = MakeBox(/*X*/ 0.0, HangPlanXCm, /*Z*/ 29.0, HangHeightCm);

		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Hang = Out.Structure.AddPiece(BoxMassKg(HangBox), /*bIsGrounded*/ false, HangBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Anchor, AnchorBox, Out.Hang, HangBox, JointThicknessCm, TensionHangBond(), Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}
	}

	// Hand oracle, from geometry and density, independent of the LP and the cache.

	double HangMassKg()
	{
		return ClayDensityGramsPerCubicCm * HangPlanXCm * WytheWidthCm * HangHeightCm / 1000.0;
	}
	double HangWeightUu() { return HangMassKg() * GravityCmPerSecondSquared; }

	/** The bed face area. */
	double JointAreaSqCm() { return HangPlanXCm * WytheWidthCm; }

	double TensionCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm(); }

	double ExpectedUtilisation() { return HangWeightUu() / TensionCapacityUu(); }

	bool Near(double A, double B, double Tol) { return FMath::Abs(A - B) <= Tol; }

	/** The inspection row for this connection, or nullptr. */
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

/** Below the cap InspectPiece shows the LP readout's utilisation; above it, the router's. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStrainOverlayInspectPieceTest,
	"DestructionGame.Acceptance.StrainReadout.InspectPieceShowsLPUtilisationBelowTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStrainOverlayInspectPieceTest::RunTest(const FString& Parameters)
{
	using namespace StrainOverlayInspectPieceSupport;

	// Fixture: the tension hang, two pieces, one bed joint.

	FTensionHang Below;
	Build(Below);

	if (Below.BedJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit the anchor-to-hang bed joint"));
		return false;
	}

	TestEqual(TEXT("FIXTURE: two pieces — a grounded anchor and one hang below it"),
		Below.Structure.NumPieces(), 2);
	TestEqual(TEXT("FIXTURE: one joint — the overhead bed joint the hang tension-hangs from"),
		Below.Structure.NumConnections(), 1);
	TestTrue(TEXT("FIXTURE: every piece and joint must know where it is, or there is no bridge"),
		Below.Structure.HasCompleteGeometry());

	AddInfo(FString::Printf(
		TEXT("HAND ORACLE: W %.6g uu, area %.6g cm2, f_t %.6g MPa => capacity %.6g uu, expected LP util %.6g"),
		HangWeightUu(), JointAreaSqCm(), TensileMPa, TensionCapacityUu(), ExpectedUtilisation()));

	// Below the cap the gate stands the hang and caches the LP readout.

	Below.Structure.SetEquilibriumGateBlockCap(64); // 2 pieces, well below the cap
	Below.Structure.SolveAndBreak();

	TestTrue(TEXT("BELOW CAP: the hang reads Supported (the LP carries it) — additive overlay change"),
		Below.Structure.GetPieceSupport(Below.Hang) == EPieceSupport::Supported);

	const FStructure::FConnectionReadout Readout = Below.Structure.GetConnectionReadout(Below.BedJoint);
	const double RouterUtil = Below.Structure.GetConnectionUtilisation(Below.BedJoint);

	// Precondition: without the cached readout the checks below prove nothing.
	TestTrue(TEXT("PRECONDITION: the cached LP readout is present below the cap (6b core is built)"),
		Readout.bPresent);

	const FPieceInspection BelowInspection = InspectPiece(Below.Structure, Below.Hang);

	TestTrue(TEXT("INSPECT: the hang is a live piece"), BelowInspection.bIsPiece);

	const FJointInspection* BelowRow = JointRowFor(BelowInspection, Below.BedJoint);
	if (BelowRow == nullptr)
	{
		AddError(TEXT("INSPECT: InspectPiece must emit a row for the hang's one bed joint"));
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("BELOW CAP: InspectPiece joint util %.6g; cached LP readout util %.6g; router util %.6g; expected LP %.6g"),
		BelowRow->Utilisation, Readout.Utilisation, RouterUtil, ExpectedUtilisation()));

	// The row's utilisation must be the LP readout's, matching the hand oracle.
	TestTrue(
		*FString::Printf(
			TEXT("BELOW CAP [RED]: InspectPiece util %.6g == LP readout util %.6g (== hand oracle %.6g)"),
			BelowRow->Utilisation, Readout.Utilisation, ExpectedUtilisation()),
		Near(BelowRow->Utilisation, Readout.Utilisation, 1.0e-9)
			&& Near(BelowRow->Utilisation, ExpectedUtilisation(), 2.0e-3));

	// And differ sharply from the router's ~0 on the same joint.
	TestTrue(
		*FString::Printf(
			TEXT("BELOW CAP [RED]: InspectPiece util %.6g differs sharply from the router's %.6g — the LP is the source"),
			BelowRow->Utilisation, RouterUtil),
		FMath::Abs(BelowRow->Utilisation - RouterUtil) > 0.1);

	// Above the cap no readout is cached, so InspectPiece must fall back to the router.

	FTensionHang Above;
	Build(Above);
	Above.Structure.SetEquilibriumGateBlockCap(1); // 2 pieces > cap, so the gate declines
	Above.Structure.SolveAndBreak();

	const FStructure::FConnectionReadout AboveReadout = Above.Structure.GetConnectionReadout(Above.BedJoint);
	const double AboveRouterUtil = Above.Structure.GetConnectionUtilisation(Above.BedJoint);

	TestEqual(TEXT("ABOVE CAP: no cached LP readout — the min-violation LP is solved below the cap only"),
		AboveReadout.bPresent, false);

	const FPieceInspection AboveInspection = InspectPiece(Above.Structure, Above.Hang);
	const FJointInspection* AboveRow = JointRowFor(AboveInspection, Above.BedJoint);
	if (AboveRow == nullptr)
	{
		AddError(TEXT("INSPECT: InspectPiece must emit a row for the hang's one bed joint above the cap too"));
		return false;
	}

	AddInfo(FString::Printf(
		TEXT("ABOVE CAP: InspectPiece joint util %.6g; router util %.6g (readout absent => fall back)"),
		AboveRow->Utilisation, AboveRouterUtil));

	TestTrue(
		*FString::Printf(
			TEXT("ABOVE CAP: InspectPiece util %.6g == router util %.6g (readout absent => fall back to the router)"),
			AboveRow->Utilisation, AboveRouterUtil),
		Near(AboveRow->Utilisation, AboveRouterUtil, 1.0e-9));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
