// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * 2026-09-01 review item 7 — ONE strength evaluator for the router break sweep.
 *
 * BEHAVIOUR UNDER TEST, in one sentence: the router's break sweep
 * (FStructure::BreakByCapacitySweep — the ABOVE-cap / LP-refusal break authority) must
 * decide a joint's break against the SAME weakest-link paired strength
 * (FStructure::EffectiveJointStrength) that the strain readout
 * (GetConnectionUtilisation) and the LP bridge already use, so a cross-material joint
 * loaded past its paired capacity but below its bare-connection capacity is SEVERED by
 * the sweep rather than read as failed by the overlay while the sweep leaves it intact.
 *
 * THE DEFECT (confirmed CONFIRMED in the review). There are three consumers of "what
 * can this joint take", and they do not agree on cross-material joints:
 *   - GetConnectionUtilisation (Structure.cpp) evaluates against EffectiveJointStrength
 *     (the weakest-link pairing min(connection, matA, matB)).
 *   - the LP bridge (RigidBlockBridge) copies EffectiveJointStrength into the LP row.
 *   - BUT BreakByCapacitySweep calls Connections[Index].ApplyForce(...), which evaluates
 *     against the connection's OWN bare Strength member.
 * So for a cross-material joint whose paired strength is STRICTLY WEAKER than its bare
 * strength on the governing axis, and a load sitting BETWEEN the two, the overlay reads
 * the joint failed (utilisation > 1) while the sweep — the one place that actually
 * decides breaks above the cap — reads it intact and never severs it. Two copies of the
 * arithmetic in the one place that decides breaks; the readout and the break disagree.
 *
 * WHY THIS IS RED TODAY, AND FOR THE RIGHT REASON. The divergent case below loads a
 * Timber-post-on-ClayBrick-footing bearing to 21 MPa of pure compression. The paired
 * (weakest-link) crush is the brick's 20 MPa, so the readout reads 21/20 = 1.05 (failed);
 * the bare Unbreakable connection's crush is 1e12 MPa, so ApplyForce reads 21/1e12 ~ 2e-11
 * (comfortably intact). The block cap is forced to 0 so the equilibrium LP DECLINES and
 * the router sweep is the sole break authority — the exact path this defect lives on. The
 * test asserts BOTH halves: the divergence EXISTS (the readout reads > 1 while the bare
 * sweep verdict reads < 1) AND the sweep FAILS TO SEVER (HasGiven == false today). The
 * pair is what proves the two consumers disagree rather than merely that a number is off.
 *
 * WHY COMPRESSION IS THE GOVERNING AXIS, UNAMBIGUOUSLY. The post's centre of mass sits
 * directly over the joint centre (zero eccentricity -> zero moment), the joint normal is
 * vertical, and the only force is gravity on the post — so tension and shear are exactly
 * zero on both the paired and the bare evaluation, and ComputeUtilisation's worst-axis
 * answer IS the compression ratio on both. There is no other axis that could silently
 * govern the sweep verdict or the readout. (Worked through and asserted, not assumed —
 * DESIGN.md §4's warning that ComputeUtilisation returns the worst axis.)
 *
 * THE NO-OP CONTROL. A SINGLE-material joint (ClayBrick-on-ClayBrick in
 * GeneralPurposeMortar) has paired == bare on every axis — the mortar is the weakest link
 * on compression (10 < 20), tension (0.7 < 2.0) and shear (0.9 < 3.0), so
 * min(connection, matA, matB) == connection bit-for-bit. Loaded past the mortar's 10 MPa
 * crush it severs under BOTH the bare and the paired evaluation, TODAY and after the fix.
 * That proves the fix changes only the cross-material divergent case and leaves the
 * single-material path (the whole wall catalogue) bit-identical.
 *
 * THE FIX dev-expert is expected to make (do NOT make it here): evaluate the sweep against
 * EffectiveJointStrength(Index) instead of the bare Connections[Index].Strength — e.g.
 * apply the force to a connection copy carrying the paired strength, exactly as
 * GetConnectionUtilisation already does.
 *
 * UNITS. The MPa <-> uu conversion (1 N = 100 uu, 1 cm2 = 100 mm2 -> 10000 uu per MPa per
 * cm2) is spelled out from first principles below rather than imported from
 * ForceUnitsPerMPaSqCm, so this test fails if that constant is wrong instead of silently
 * agreeing with it (DESIGN.md §3).
 *
 * NEEDS A TICKING WORLD: NO. Gravity is the ordinary FStructure kind (weight is
 * MassKg x 980 inside the world-free solver); every assertion is on solver state
 * (utilisation, HasGiven). Same footing as CrossMaterialBearingWiringTest.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace RouterSweepEffectiveStrengthSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Every piece is this deep on Y, so with a 10 cm face length the bed area is 98 cm2. */
	constexpr double WytheWidthCm = 9.8;

	/** The bearing face is 10 cm long on X; with the 9.8 cm wythe that is 98 cm2. */
	constexpr double FaceLengthCm = 10.0;

	/** A 1 cm mortarless contact — the separation the bed joint is formed across. */
	constexpr double JointThicknessCm = 1.0;

	/** The bed area, worked once: 10 x 9.8 = 98 cm2. */
	constexpr double BedAreaSqCm = FaceLengthCm * WytheWidthCm;

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/*
	 * THE CONVERSION, DERIVED HERE. 1 N = 100 uu, 1 cm2 = 100 mm2, so 1 MPa (= 1 N/mm2) over
	 * 1 cm2 is 100 * 100 = 10000 uu. Independent of ForceUnitsPerMPaSqCm on purpose.
	 */
	constexpr double UuPerMPaSqCm = 100.0 * 100.0;

	/**
	 * The post mass, kilograms, that lands EXACTLY the given bearing stress on the 98 cm2 bed:
	 * weight = mass * 980 uu, over 98 cm2 that is (mass * 980 / 98) uu/cm2, / 10000 -> MPa.
	 * Solving for mass at a target MPa gives mass = MPa * 98 * 10000 / 980 = MPa * 1000. The
	 * masses are chosen test SCALARS isolating the strength path, not realistic weights.
	 */
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
	 * Lay a grounded footing and a post that bears on it through one centred bed joint, tag
	 * each piece with its material (or leave it null), and force the block cap to 0 so the
	 * equilibrium LP DECLINES and the ROUTER capacity sweep is the sole break authority —
	 * the path this defect lives on. The post's centre sits over the joint centre (X = 0),
	 * so the bearing is centred and the joint carries pure vertical compression.
	 *
	 * A null material (nullptr) leaves EffectiveJointStrength returning the bare connection;
	 * a real profile pointer makes the pairing live. The materials of the footing and post
	 * are passed so one builder serves both the cross-material divergent case and the
	 * single-material no-op control.
	 */
	void Build(
		FBearing& Out,
		double PostMassKg,
		const FConnectionStrength& Connection,
		const FMaterialProfile* FootingMaterial,
		const FMaterialProfile* PostMaterial)
	{
		/* Footing top at Z = 20; post bottom at Z = 21; the 1 cm gap is the joint. */
		const FPieceBox FootBox = MakeBox(/*Z*/ 10.0, /*SizeZ*/ 20.0);
		const FPieceBox PostBox = MakeBox(/*Z*/ 31.0, /*SizeZ*/ 20.0);

		/* Footing mass is irrelevant (grounded -> its weight goes to earth, not through the bed). */
		Out.Footing = Out.Structure.AddPiece(50.0, /*bIsGrounded*/ true, FootBox.CentreCm);
		Out.Post = Out.Structure.AddPiece(PostMassKg, /*bIsGrounded*/ false, PostBox.CentreCm);

		Out.Structure.SetPieceMaterial(Out.Footing, FootingMaterial);
		Out.Structure.SetPieceMaterial(Out.Post, PostMaterial);

		FConnection Joint;
		if (MakeInterface(Out.Footing, FootBox, Out.Post, PostBox, JointThicknessCm, Connection, Joint))
		{
			Out.BedJoint = Out.Structure.AddConnection(Joint);
		}

		/* Cap 0: NumPieces() (2) > 0, so the gate declines and the router sweep decides. */
		Out.Structure.SetEquilibriumGateBlockCap(0);
	}
}

/**
 * The router break sweep severs a cross-material joint against the weakest-link paired
 * strength — not the bare connection — matching the readout and the LP bridge.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouterSweepUsesEffectiveJointStrengthTest,
	"DestructionGame.Acceptance.RouterBreakSweep.SeversAgainstTheWeakestLinkStrengthNotTheBareConnection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRouterSweepUsesEffectiveJointStrengthTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RouterSweepEffectiveStrengthSupport;

	/* ------------------------------------------------------------------ *
	 * FIXTURE PRECONDITIONS — the hand-derived numbers only mean what they
	 * say while the profiles carry the strengths they were derived against.
	 * ------------------------------------------------------------------ */
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: the Unbreakable connection's compressive (%g MPa) must dwarf "
			"both materials, so the MATERIAL crush governs the pairing"), Unbreakable.CompressiveStrengthMPa),
		Unbreakable.CompressiveStrengthMPa > 1.0e9);
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: timber compressive must be 21 MPa, profile carries %g"),
			Timber.Strength.CompressiveStrengthMPa),
		Timber.Strength.CompressiveStrengthMPa == 21.0);
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: clay brick compressive must be 20 MPa, profile carries %g"),
			ClayBrick.Strength.CompressiveStrengthMPa),
		ClayBrick.Strength.CompressiveStrengthMPa == 20.0);
	TestTrue(
		FString::Printf(TEXT("PRECONDITION: general-purpose mortar compressive must be 10 MPa, profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	/* ================================================================== *
	 * DIVERGENT CASE — cross-material, paired STRICTLY WEAKER than bare,
	 * loaded BETWEEN the two. The overlay reads failed; the router sweep
	 * must sever it, and TODAY does not (the red).
	 * ================================================================== */
	{
		/*
		 * The load: 21 MPa of pure compression. The paired (weakest-link) crush is the brick's
		 * 20 MPa; the bare Unbreakable crush is 1e12. 21 sits strictly between them, so the
		 * paired evaluation fails (21/20 = 1.05) and the bare evaluation stands (21/1e12 ~ 2e-11).
		 */
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

		/* ---- THE DIVERGENCE EXISTS: paired crush strictly weaker than bare on compression. ---- */
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

		/* ---- SOLVE (non-destructive) AND CONFIRM THE LOAD IS PURE, CENTRED COMPRESSION. ---- */
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
		 * ---- THE TWO CONSUMERS DISAGREE, ON THE SAME SOLVED FORCE. ----
		 *
		 * READOUT (paired): GetConnectionUtilisation reads 21/20 = 1.05 — FAILED. This half
		 * already holds today (B3 wired the readout to the pairing).
		 *
		 * BARE SWEEP VERDICT: the sweep applies the routed force to the connection's OWN bare
		 * strength, exactly what GetConnection(bed).UtilisationUnder computes — 21/1e12 ~ 2e-11,
		 * INTACT. This is why the sweep will not sever, and it documents that the red below is
		 * for the RIGHT reason (bare-strength arithmetic), not a fixture accident.
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

		/*
		 * ---- THE ASSERTION: the router sweep MUST SEVER the joint. ----
		 *
		 * RED TODAY: BreakByCapacitySweep evaluates ApplyForce against the bare 1e12 strength,
		 * reads 2e-11, and leaves the joint intact — so HasGiven stays false. The fix makes the
		 * sweep evaluate against EffectiveJointStrength (20 MPa), read 1.05, and sever it.
		 */
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

	/* ================================================================== *
	 * NO-OP CONTROL — single-material joint, paired == bare on every axis.
	 * Over the mortar crush it severs under BOTH evaluations, today and
	 * after the fix: the fix must not touch this path.
	 * ================================================================== */
	{
		/*
		 * ClayBrick on ClayBrick in GeneralPurposeMortar: the mortar is the weakest link on every
		 * axis (compression 10 < 20, tension 0.7 < 2.0, shear 0.9 < 3.0), so
		 * EffectiveJointStrength == the bare connection bit-for-bit. Load 11 MPa is past the
		 * mortar's 10 MPa crush -> util 1.1 under BOTH the bare and the paired evaluation.
		 */
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

		/*
		 * Severs today AND after the fix — the no-op proof. GREEN on arrival: this is a
		 * characterisation pin that the single-material path is untouched by review item 7.
		 */
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
