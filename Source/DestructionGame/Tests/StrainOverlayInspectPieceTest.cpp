// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/PieceInspection.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 6b FINAL — THE STRAIN OVERLAY CONSUMES THE LP READOUT BELOW THE CAP, RED (RED A).
 *
 * 6b-core (StrainReadoutWiringTest, green) cached a below-cap min-violation readout onto FStructure,
 * queryable as GetConnectionReadout(Index) -> {bPresent, NormalUu, MomentUuCm, ViolationUu,
 * Utilisation}, keyed back to production connections through the bridge's ConnectionOfJoint
 * provenance. But NOTHING PLAYER-VISIBLE reads it yet: the per-joint strain number a debugger and the
 * overlay draw comes from InspectPiece (Core/PieceInspection.cpp), which fills
 *
 *     Joint.Utilisation = Structure.GetConnectionUtilisation(Index)
 *
 * — the ROUTER's FConnection::UtilisationUnder over the routed ConnectionForces. This slice switches
 * InspectPiece to the LP readout BELOW the cap, and keeps the router ABOVE it (readout absent =>
 * fall back). This is the FIRST time the min-violation readout reaches something a player reads.
 *
 * THE SEAM THIS PINS: PieceInspection.cpp:55, where every FJointInspection.Utilisation is set. Below
 * the cap it must come from GetConnectionReadout(Index).Utilisation when bPresent; above the cap (or
 * when the readout is absent) it must stay GetConnectionUtilisation(Index).
 *
 * THE FIXTURE — THE 6b DETERMINATE TENSION HANG (same shape as StrainReadoutWiringTest). A single
 * free brick hangs BELOW a grounded anchor from one bed joint whose only finite strength axis is
 * TENSION (compression / cohesion uncapped, friction zero). It is the DISCRIMINATING fixture the
 * slice needs, because the two sources read sharply different numbers on the SAME joint:
 *
 *   - THE ROUTER cannot route an upward tension hang. It has nothing beneath it, so the router
 *     strands it, its routed joint force is ~0, and GetConnectionUtilisation reads ~0.
 *   - THE LP carries the whole weight across the bed joint in tension and reads
 *     Utilisation = W / (f_t * Conv * A) ~ 0.3724 (worked by hand below, independent of the LP).
 *
 * So InspectPiece's joint Utilisation matching the LP readout (~0.3724) AND differing sharply from
 * the router (~0) on the same connection is ONLY possible if InspectPiece switched to the LP source.
 * That difference IS the ruling: below the cap the shown utilisation is the LP-primal (first-crack)
 * demand/capacity, not the router's biaxial/composite-relieved downward-routing estimate. TENSION is
 * the sole capped axis, so no other axis can silently govern the number.
 *
 * THE ABOVE-CAP ARM proves the fallback: with the gate declining (cap forced below the piece count,
 * exactly as StrainReadoutWiringTest reaches the decline path cheaply on a 2-piece fixture) the
 * readout is absent, and InspectPiece must show the router's GetConnectionUtilisation unchanged. That
 * arm is a CHARACTERISATION — green today because InspectPiece already reads the router everywhere —
 * kept so a fix that read the readout unconditionally (and so lost the fallback) would be caught.
 *
 * UNITS are derived here (1 MPa over 1 cm2 = 100 * 100 = 10000 uu), never imported, so a wrong
 * production constant disagrees rather than agrees.
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on the ordinary way (weight is MassKg * 980 inside
 * FStructure), everything is connected, and every assertion is on InspectPiece output, the cached
 * readout, or the router accessor — the same footing as the 6b-core wiring test.
 *
 * VISUAL VALIDATION AT GREEN TIME: this fixture is the one to render. It is the smallest structure
 * whose overlay number changes when the source switches (router ~0 -> LP ~0.37 on the one visible
 * joint), so a before/after capture of the hang's bed joint shows the readout becoming player-visible.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace StrainOverlayInspectPieceSupport
{
	using namespace DestructionLayout;

	/* ================================================================================
	 * UNITS AND GEOMETRY, derived here so a wrong production constant fails rather than agrees.
	 * ================================================================================ */

	/** Fired clay, 1.9 g/cm3 — the same figure every wall fixture uses. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** The single wythe: every piece is this deep on Y, so every joint's Y overlap is full. */
	constexpr double WytheWidthCm = 10.25;

	/** A 1 cm mortar bed joint — the separation the anchor and the hang are formed across. */
	constexpr double JointThicknessCm = 1.0;

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. NOT imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/** Above the oracle's UncappedStrengthMPa (1e9): a strength row this large is not assembled. */
	constexpr double UncappedHere = 1.0e12;

	/** The hang's plan face: 10 cm on X, the full wythe on Y. */
	constexpr double HangPlanXCm = 10.0;
	constexpr double HangHeightCm = 10.0;

	/**
	 * The tension bond, sized so Utilisation = W / (f_t*Conv*A) is a comfortable ~0.37: the hang
	 * STANDS with a wide margin (so the gate never fells it — the overlay change is additive) yet the
	 * utilisation is a substantial, clearly-non-zero number the router's stranded ~0 cannot match.
	 */
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

	/**
	 * A joint with a WEAK finite TENSION bond and every other axis uncapped, so the ONLY strength
	 * row that can carry demand is tension — which makes the hang's governing axis unambiguous.
	 */
	FConnectionStrength TensionHangBond()
	{
		FConnectionStrength S;
		S.TensileStrengthMPa = TensileMPa;       /* finite: the tension row IS written */
		S.CompressiveStrengthMPa = UncappedHere; /* no crushing row */
		S.ShearCohesionMPa = UncappedHere;       /* no friction row */
		S.FrictionCoefficient = 0.0;
		/* MaxShearStrengthMPa unbounded by default => no ceiling row. */
		return S;
	}

	struct FTensionHang
	{
		FStructure Structure;

		int32 Anchor = INDEX_NONE; /* grounded, above */
		int32 Hang = INDEX_NONE;   /* free, below */

		int32 BedJoint = INDEX_NONE; /* the one joint: Anchor above, Hang below */
	};

	void Build(FTensionHang& Out)
	{
		/* Anchor: grounded, bottom face at Z = 35. */
		const FPieceBox AnchorBox = MakeBox(/*X*/ 0.0, HangPlanXCm, /*Z*/ 40.0, HangHeightCm);

		/* Hang: free, top face at Z = 34 — one 1 cm bed joint below the anchor. */
		const FPieceBox HangBox = MakeBox(/*X*/ 0.0, HangPlanXCm, /*Z*/ 29.0, HangHeightCm);

		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Hang = Out.Structure.AddPiece(BoxMassKg(HangBox), /*bIsGrounded*/ false, HangBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.Anchor, AnchorBox, Out.Hang, HangBox, JointThicknessCm, TensionHangBond(), Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}
	}

	/* ================================================================================
	 * THE INDEPENDENT HAND ORACLE — the same statics as StrainReadoutWiringTest, rebuilt from
	 * geometry and density here so the expected number does not depend on the LP or the cache.
	 * ================================================================================ */

	double HangMassKg()
	{
		return ClayDensityGramsPerCubicCm * HangPlanXCm * WytheWidthCm * HangHeightCm / 1000.0;
	}
	double HangWeightUu() { return HangMassKg() * GravityCmPerSecondSquared; }

	/** The bed face: 10 cm on X by the full wythe on Y. */
	double JointAreaSqCm() { return HangPlanXCm * WytheWidthCm; }

	double TensionCapacityUu() { return TensileMPa * ForceUnitsPerMPaSqCmHere * JointAreaSqCm(); }

	double ExpectedUtilisation() { return HangWeightUu() / TensionCapacityUu(); }

	bool Near(double A, double B, double Tol) { return FMath::Abs(A - B) <= Tol; }

	/** The one FJointInspection row on a piece that names the given connection, or nullptr. */
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
 * Below the cap, InspectPiece's per-joint Utilisation is the LP min-violation readout, not the
 * router's per-joint estimate; above the cap it falls back to the router.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStrainOverlayInspectPieceTest,
	"DestructionGame.Acceptance.StrainReadout.InspectPieceShowsLPUtilisationBelowTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStrainOverlayInspectPieceTest::RunTest(const FString& Parameters)
{
	using namespace StrainOverlayInspectPieceSupport;

	/* ------------------------------------------------------------------ *
	 * FIXTURE — the determinate tension hang, two pieces, one bed joint.
	 * ------------------------------------------------------------------ */

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

	/* ------------------------------------------------------------------ *
	 * BELOW THE CAP — settle, then inspect. The equilibrium gate is
	 * authoritative (2 pieces << cap), stands the hang, and caches the LP
	 * readout; InspectPiece must SHOW that readout's utilisation.
	 * ------------------------------------------------------------------ */

	Below.Structure.SetEquilibriumGateBlockCap(64); /* 2 pieces << cap => below cap */
	Below.Structure.SolveAndBreak();

	TestTrue(TEXT("BELOW CAP: the hang reads Supported (the LP carries it) — additive overlay change"),
		Below.Structure.GetPieceSupport(Below.Hang) == EPieceSupport::Supported);

	const FStructure::FConnectionReadout Readout = Below.Structure.GetConnectionReadout(Below.BedJoint);
	const double RouterUtil = Below.Structure.GetConnectionUtilisation(Below.BedJoint);

	/* Precondition — 6b core is built, so the cache is present below the cap. If this fails the
	 * fixture is not exercising the below-cap readout at all and the RED below proves nothing. */
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

	/* RED — the row's utilisation must be the LP readout's, which matches the hand oracle. Today it
	 * is set from GetConnectionUtilisation (the router), which strands the hang and reads ~0. */
	TestTrue(
		*FString::Printf(
			TEXT("BELOW CAP [RED]: InspectPiece util %.6g == LP readout util %.6g (== hand oracle %.6g)"),
			BelowRow->Utilisation, Readout.Utilisation, ExpectedUtilisation()),
		Near(BelowRow->Utilisation, Readout.Utilisation, 1.0e-9)
			&& Near(BelowRow->Utilisation, ExpectedUtilisation(), 2.0e-3));

	/* RED — and it must NOT be the router's estimate. The router strands the hang and reads ~0, so a
	 * sharp difference on the SAME joint is the proof InspectPiece switched to the LP source. */
	TestTrue(
		*FString::Printf(
			TEXT("BELOW CAP [RED]: InspectPiece util %.6g differs sharply from the router's %.6g — the LP is the source"),
			BelowRow->Utilisation, RouterUtil),
		FMath::Abs(BelowRow->Utilisation - RouterUtil) > 0.1);

	/* ------------------------------------------------------------------ *
	 * ABOVE THE CAP — the gate declines, no LP readout is cached, and the
	 * overlay must fall back to the router. Reached cheaply by forcing the
	 * cap below the piece count, exactly as StrainReadoutWiringTest does.
	 * CHARACTERISATION: green today (InspectPiece already reads the router)
	 * and must stay green so a fix cannot lose the fallback.
	 * ------------------------------------------------------------------ */

	FTensionHang Above;
	Build(Above);
	Above.Structure.SetEquilibriumGateBlockCap(1); /* 2 pieces > cap => declines to router */
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
