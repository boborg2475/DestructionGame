// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Review item 7 (2026-09-01): the router break sweep (BreakByCapacitySweep, the break authority
 * above the block cap) must judge a joint against the same weakest-link strength
 * (EffectiveJointStrength) as the strain readout and the LP bridge. The defect: the sweep used the
 * connection's bare Strength, so a cross-material joint loaded between its paired and bare
 * capacities read failed on the overlay but was never severed.
 *
 * Divergent case: a Timber post on a ClayBrick footing through an Unbreakable connection, at 21 MPa
 * of pure compression. Paired crush is the brick's 20 MPa (reads 1.05); bare is 1e12 (reads 2e-11).
 * Block cap 0, so the router sweep decides. The load is centred and vertical, so compression is
 * the only non-zero axis. The joint must sever.
 *
 * Control: ClayBrick on ClayBrick in mortar has paired == bare on every axis; at 11 MPa it severs
 * either way, so the fix leaves the single-material path unchanged.
 *
 * MPa to uu spelled out locally (DESIGN.md §3). World-free. Named namespace for unity builds.
 */
namespace RouterSweepEffectiveStrengthSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	constexpr double WytheWidthCm = 9.8;
	constexpr double FaceLengthCm = 10.0;
	constexpr double JointThicknessCm = 1.0;

	/** 10 x 9.8 = 98 cm2. */
	constexpr double BedAreaSqCm = FaceLengthCm * WytheWidthCm;

	/** MassKg * 980 is already a weight in uu; do not apply 1 N = 100 uu again. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 MPa over 1 cm2 is 100 * 100 = 10000 uu. Deliberately independent of ForceUnitsPerMPaSqCm. */
	constexpr double UuPerMPaSqCm = 100.0 * 100.0;

	/** Post mass (kg) giving exactly this bearing stress on the 98 cm2 bed: MPa * 1000. Test scalars, not realistic weights. */
	constexpr double PostMassForBearingMPa(double MPa)
	{
		return MPa * BedAreaSqCm * UuPerMPaSqCm / GravityCmPerSecondSquared;
	}

	struct FBearing
	{
		FStructure Structure;

		int32 Footing = INDEX_NONE;  // grounded footing (material chosen per case)
		int32 Post = INDEX_NONE;     // bears on the footing (material chosen per case)
		int32 BedJoint = INDEX_NONE; // Post - Footing
	};

	FPieceBox MakeBox(double CentreZ, double SizeZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(FaceLengthCm, WytheWidthCm, SizeZ) * 0.5;
		Box.CentreCm = FVector(0.0, 0.0, CentreZ);
		return Box;
	}

	/**
	 * A grounded footing and a post on one centred bed joint (pure vertical compression), with the
	 * given materials (nullptr means bare connection strength). Block cap 0, so the LP declines
	 * and the router sweep decides.
	 */
	void Build(
		FBearing& Out,
		double PostMassKg,
		const FConnectionStrength& Connection,
		const FMaterialProfile* FootingMaterial,
		const FMaterialProfile* PostMaterial)
	{
		// Footing top at Z = 20, post bottom at Z = 21.
		const FPieceBox FootBox = MakeBox(/*Z*/ 10.0, /*SizeZ*/ 20.0);
		const FPieceBox PostBox = MakeBox(/*Z*/ 31.0, /*SizeZ*/ 20.0);

		// Footing mass is irrelevant; it is grounded.
		Out.Footing = Out.Structure.AddPiece(50.0, /*bIsGrounded*/ true, FootBox.CentreCm);
		Out.Post = Out.Structure.AddPiece(PostMassKg, /*bIsGrounded*/ false, PostBox.CentreCm);

		Out.Structure.SetPieceMaterial(Out.Footing, FootingMaterial);
		Out.Structure.SetPieceMaterial(Out.Post, PostMaterial);

		FConnection Joint;
		if (MakeInterface(Out.Footing, FootBox, Out.Post, PostBox, JointThicknessCm, Connection, Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}

		// 2 pieces > cap 0, so the LP declines.
		Out.Structure.SetEquilibriumGateBlockCap(0);
	}
}

/** The router sweep severs against the weakest-link paired strength, not the bare connection. See the file header. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouterSweepUsesEffectiveJointStrengthTest,
	"DestructionGame.Acceptance.RouterBreakSweep.SeversAgainstTheWeakestLinkStrengthNotTheBareConnection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRouterSweepUsesEffectiveJointStrengthTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RouterSweepEffectiveStrengthSupport;

	// The hand-derived numbers assume these profile strengths.
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the Unbreakable connection's compressive (%g MPa) must dwarf "
			"both materials, so the MATERIAL crush governs the pairing"), Unbreakable.CompressiveStrengthMPa),
		Unbreakable.CompressiveStrengthMPa > 1.0e9);
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: timber compressive must be 29 MPa (C24 mean f_c,0), profile carries %g"),
			Timber.Strength.CompressiveStrengthMPa),
		Timber.Strength.CompressiveStrengthMPa == 29.0);
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: clay brick compressive must be 20 MPa, profile carries %g"),
			ClayBrick.Strength.CompressiveStrengthMPa),
		ClayBrick.Strength.CompressiveStrengthMPa == 20.0);
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: general-purpose mortar compressive must be 10 MPa, profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	// Divergent case: cross-material, loaded between paired (20 MPa) and bare (1e12) capacity.
	{
		constexpr double LoadMPa = 21.0;
		const double PostMassKg = PostMassForBearingMPa(LoadMPa);                 // 21000

		FBearing Fx;
		Build(Fx, PostMassKg, Unbreakable, /*Footing*/ &ClayBrick, /*Post*/ &Timber);

		if (Fx.BedJoint == INDEX_NONE)
		{
			AddError(TEXT("[divergent] FIXTURE: the producer must emit the bed joint"));
			return false;
		}

		TestEqual(TEXT("[divergent] FIXTURE: two pieces — the grounded footing and the post"),
			Fx.Structure.NumPieces(), 2);
		TestTrue(TEXT("[divergent] FIXTURE: every piece and joint must know where it is (honest lever arms)"),
			Fx.Structure.HasCompleteGeometry());
		TestTrue(TEXT("[divergent] FIXTURE: the post bears on the footing through a BED joint"),
			Fx.Structure.GetJointRole(Fx.BedJoint, Fx.Post) == EJointRole::BedBeneath);

		// Paired crush is strictly weaker than bare.
		const double PairedCrushMPa = Fx.Structure.EffectiveJointStrength(Fx.BedJoint).CompressiveStrengthMPa;
		const double BareCrushMPa = Fx.Structure.GetConnection(Fx.BedJoint).Strength.CompressiveStrengthMPa;

		TestTrue(
			FString::Printf(TEXT("[divergent] the PAIRED crush must be the brick's 20 MPa (weakest link), got %g"),
				PairedCrushMPa),
			FMath::IsNearlyEqual(PairedCrushMPa, 20.0, 1.0e-9));
		TestTrue(
			FString::Printf(TEXT("[divergent] the BARE crush must be the Unbreakable connection's 1e12 MPa, got %g"),
				BareCrushMPa),
			BareCrushMPa > 1.0e9);
		TestTrue(
			FString::Printf(TEXT("[divergent] PRECONDITION: the load %g MPa must sit strictly between paired %g "
				"and bare %g"), LoadMPa, PairedCrushMPa, BareCrushMPa),
			LoadMPa > PairedCrushMPa && LoadMPa < BareCrushMPa);

		// Non-destructive solve; confirm pure, centred compression.
		Fx.Structure.SolveLoads();

		const FVector BedForce = Fx.Structure.GetConnectionForce(Fx.BedJoint);
		const double PostWeightUu = PostMassKg * GravityCmPerSecondSquared;
		TestTrue(
			FString::Printf(TEXT("[divergent] FIXTURE: the bed carries the post's whole weight |%g| == %g uu"),
				BedForce.Size(), PostWeightUu),
			FMath::IsNearlyEqual(BedForce.Size(), PostWeightUu, 1.0));
		TestTrue(
			FString::Printf(TEXT("[divergent] FIXTURE: the bearing force must be purely vertical (X %g, Y %g ~0), "
				"so the load is pure compression and no other axis can govern"), BedForce.X, BedForce.Y),
			FMath::IsNearlyZero(BedForce.X, 1.0e-6) && FMath::IsNearlyZero(BedForce.Y, 1.0e-6));

		const double BearingStressMPa = PostWeightUu / BedAreaSqCm / UuPerMPaSqCm;
		TestTrue(
			FString::Printf(TEXT("[divergent] FIXTURE: the bearing stress must be exactly %g MPa, worked to %g"),
				LoadMPa, BearingStressMPa),
			FMath::IsNearlyEqual(BearingStressMPa, LoadMPa, 1.0e-9));

		/*
		 * On the same force, the paired readout reads 1.05 (failed) and the bare evaluation reads
		 * 2e-11 (intact), showing the two strengths genuinely diverge here.
		 */
		const double ReadoutUtilisation = Fx.Structure.GetConnectionUtilisation(Fx.BedJoint);
		const double BareSweepUtilisation = Fx.Structure.GetConnection(Fx.BedJoint).UtilisationUnder(
			Fx.Structure.GetConnectionForce(Fx.BedJoint),
			Fx.Structure.GetConnectionMoment(Fx.BedJoint),
			Fx.Structure.GetConnectionCompositeDepthCm(Fx.BedJoint));

		AddInfo(FString::Printf(
			TEXT("[divergent] READOUT (paired, 20 MPa) = %.12g > 1 ; BARE SWEEP (1e12 MPa) = %.6g < 1 — "
				"the two consumers disagree about the same joint under the same load"),
			ReadoutUtilisation, BareSweepUtilisation));

		TestTrue(
			FString::Printf(TEXT("[divergent] the READOUT (paired) must read the joint FAILED (> 1), got %.12g"),
				ReadoutUtilisation),
			ReadoutUtilisation > 1.0);
		TestTrue(
			FString::Printf(TEXT("[divergent] the BARE sweep verdict must read the joint INTACT (< 1), got %.6g — "
				"this is exactly why the router refuses to sever it today"), BareSweepUtilisation),
			BareSweepUtilisation < 1.0);

		// The sweep must sever it; against the bare strength it would not.
		const int32 Passes = Fx.Structure.SolveAndBreak();
		const bool bGiven = Fx.Structure.GetConnection(Fx.BedJoint).HasGiven();

		AddInfo(FString::Printf(
			TEXT("[divergent] after SolveAndBreak: %d breaking pass(es), HasGiven = %s (want true)"),
			Passes, bGiven ? TEXT("true") : TEXT("false")));

		TestTrue(
			TEXT("[divergent] the router sweep MUST sever the joint the paired readout reads as failed "
				"(HasGiven == true). RED today: the sweep uses the bare connection strength and does not sever."),
			bGiven);
	}

	/*
	 * Control: single material, mortar is the weakest link on every axis so paired == bare.
	 * 11 MPa is past the mortar's 10 MPa crush, so both evaluations read 1.1.
	 */
	{
		constexpr double LoadMPa = 11.0;
		const double PostMassKg = PostMassForBearingMPa(LoadMPa);                 // 11000

		FBearing Fx;
		Build(Fx, PostMassKg, GeneralPurposeMortar, /*Footing*/ &ClayBrick, /*Post*/ &ClayBrick);

		if (Fx.BedJoint == INDEX_NONE)
		{
			AddError(TEXT("[control] FIXTURE: the producer must emit the bed joint"));
			return false;
		}

		const double PairedCrushMPa = Fx.Structure.EffectiveJointStrength(Fx.BedJoint).CompressiveStrengthMPa;
		const double BareCrushMPa = Fx.Structure.GetConnection(Fx.BedJoint).Strength.CompressiveStrengthMPa;

		TestTrue(
			FString::Printf(TEXT("[control] paired == bare on compression (single material): %g == %g"),
				PairedCrushMPa, BareCrushMPa),
			FMath::IsNearlyEqual(PairedCrushMPa, BareCrushMPa, 1.0e-9)
				&& FMath::IsNearlyEqual(PairedCrushMPa, 10.0, 1.0e-9));

		Fx.Structure.SolveLoads();

		const double ReadoutUtilisation = Fx.Structure.GetConnectionUtilisation(Fx.BedJoint);
		const double BareSweepUtilisation = Fx.Structure.GetConnection(Fx.BedJoint).UtilisationUnder(
			Fx.Structure.GetConnectionForce(Fx.BedJoint),
			Fx.Structure.GetConnectionMoment(Fx.BedJoint),
			Fx.Structure.GetConnectionCompositeDepthCm(Fx.BedJoint));

		TestTrue(
			FString::Printf(TEXT("[control] both evaluations agree the joint is over capacity: readout %.6g, "
				"bare %.6g, both > 1"), ReadoutUtilisation, BareSweepUtilisation),
			ReadoutUtilisation > 1.0 && BareSweepUtilisation > 1.0);

		// Characterisation pin: the single-material path is unchanged by review item 7.
		const int32 Passes = Fx.Structure.SolveAndBreak();
		const bool bGiven = Fx.Structure.GetConnection(Fx.BedJoint).HasGiven();

		AddInfo(FString::Printf(
			TEXT("[control] after SolveAndBreak: %d breaking pass(es), HasGiven = %s (want true, both before "
				"and after the fix)"), Passes, bGiven ? TEXT("true") : TEXT("false")));

		TestTrue(
			TEXT("[control] the single-material joint over capacity MUST sever (HasGiven == true), unchanged "
				"by the fix — paired == bare, so the bare and paired arithmetic are bit-identical here."),
			bGiven);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
