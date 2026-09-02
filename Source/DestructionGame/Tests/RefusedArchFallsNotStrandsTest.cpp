// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * RULING (b), 2026-09-02: A MASONRY RUN THAT LOSES ONE SPRINGING OF ITS ARCH FALLS CLEANLY —
 * 0 STRANDED, THE RELEASED PIECES READING Falling — RATHER THAN THE ROUTING/STRANDING ARTEFACT
 * THE ROUTER PRODUCES TODAY.
 *
 * THE MECHANISM, AND WHY IT MIS-CLASSIFIES TODAY. A run of bricks with no seat of their own spans
 * a hole. ReseatSpannedGroups groups them through their intact head joints and re-seats them onto
 * the abutments they push against — but ONLY if something seated stands on BOTH sides of the
 * group's centre (its opposition gate, FVector::DotProduct(...) < 0). That gate is CORRECT and is
 * not what this test touches: a run with an abutment on ONE side only is a cantilever with nothing
 * to thrust into, and granting it an arch would hang a wall's free end off open air. So the gate
 * refuses, `continue`s, and the seatless run keeps the two-tier fallback SolveLoads already gave
 * it: its HEAD joints. The head tier is sign-blind, so each middle brick lists BOTH its neighbours
 * as "supports", the run becomes a mutual-support chain, LoadReturnsToPiece finds the cycle, and
 * every middle brick comes out STRANDED (EPieceSupport::Stranded, the enumerator 3). That is a
 * SOLVER LIMITATION wearing a collapse's clothes: the run genuinely has no load path to the earth
 * (its one real hope, the arch, was refused), so the honest answer is Falling (enumerator 0), and
 * a collapse test must read 0 Stranded (DESIGN.md §4) so a false knot cannot pass for structure.
 *
 * THIS IS ABOVE-CAP ROUTER BEHAVIOUR, DELIBERATELY. Below the 200-block cap the equilibrium LP is
 * the authority and, per the case-21 scope ruling (DESIGN.md §8, 2026-08-13/14), MAY stand a
 * one-sided mortar run on rigid-plastic bond cohesion the model distrusts — that is the distrusted
 * but scoped LP credit, and it is NOT what this test asserts. The fixture is forced onto the ROUTER
 * with SetEquilibriumGateBlockCap(0) (every non-empty structure is then above the cap), which is
 * the same authority the 442-block realistic shed runs under — where the corner-undermine currently
 * reports 10 stranded (8 back-wall + 2 side-wall refused-arch bricks) instead of an honest fall.
 * A below-cap fixture must NOT assert Falling for a mortar run; the LP would stand it (case 21).
 *
 * THE FIXTURE — ONE RUN, TWO SEATINGS, SO THE TEST DISCRIMINATES. A course of full bricks bridges
 * a hole. The two middle bricks (B, C) have no seat of their own; the run is abutted on the left by
 * a seated brick (A) standing on a grounded pillar. A boolean adds a MIRRORED right abutment (D on
 * its own grounded pillar):
 *
 *   BOTH ABUTMENTS (positive control, must STAND):
 *      span course   [A=0][B=22.5][C=45][D=67.5]      A on LeftPillar, D on RightPillar
 *      ground       [LeftPillar]  (hole)  [RightPillar]
 *      -> group {B,C} abutted on BOTH sides -> arch fires -> B, C Supported, 0 stranded.
 *
 *   ONE ABUTMENT (the refused cantilever, the RED):
 *      span course   [A=0][B=22.5][C=45]              A on LeftPillar; nothing right of C
 *      ground       [LeftPillar]  (hole ->)
 *      -> group {B,C} abutted on ONE side -> opposition gate refuses -> today B, C read
 *         Stranded (3); under ruling (b) they must read Falling (0) with 0 stranded.
 *
 * THE ASSERTIONS, per DESIGN.md §4 — MECHANISM, NEVER DISPLACEMENT. Two pieces can sever and rest
 * exactly in place, so how far anything moved says nothing; this reads support STATE off the solver.
 *   - Refused cantilever: B and C read Falling (GetPieceSupport == Falling), AND the whole
 *     structure has ZERO Stranded pieces (the invariant that holds whether it stands or falls).
 *   - Positive control: the SAME run with both abutments reads B and C Supported and 0 Stranded,
 *     which can only happen if the arch FIRED — so a red cantilever is "the one-sided arch is
 *     refused and its run falls", not "the arch never fires at all".
 *
 * NOTHING IS IMPORTED FROM THE CODE UNDER TEST EXCEPT THE PRODUCER (MakeInterface) AND THE MORTAR
 * PROFILE. Masses come from box volume x density here; the geometry is spelled out; no production
 * constant is reached for, so a wrong one disagrees with this test rather than agreeing with it.
 *
 * NEEDS A TICKING WORLD: NO. FStructure is plain arithmetic over a graph; gravity is mass x 980,
 * everything is connected, and every assertion is on solver support state. Same footing as the
 * two-load-path overturning and spanned-hole tests.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit, at
 * which point two anonymous namespaces are the SAME namespace and identically-named helpers clash.
 */
namespace RefusedArchFallsNotStrandsTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Fired clay, 1.9 g/cm3 — the figure every wall fixture in the suite uses, spelled out here. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Single wythe: every piece is this wide on Y, so every joint has a full Y overlap. */
	constexpr double WytheWidthCm = 10.25;

	/** UK metric brick length and height; the mortar joint that makes the grid 22.5 x 7.5. */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickHeightCm = 6.5;
	constexpr double MortarJointCm = 1.0;

	/** The coordinating pitch along a course: a brick plus its head joint. */
	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;

	/* --- The grounded pillars (course 0) and the span course (course 1). --- */

	/** A pillar is one grounded brick; its top face is where the span brick's bed joint forms. */
	constexpr double PillarCentreZCm = BrickHeightCm / 2.0;          // 3.25
	constexpr double PillarTopZCm = BrickHeightCm;                   // 6.5

	/** The span course sits one mortar joint above the pillar tops. */
	constexpr double SpanBottomZCm = PillarTopZCm + MortarJointCm;   // 7.5
	constexpr double SpanCentreZCm = SpanBottomZCm + BrickHeightCm / 2.0; // 10.75

	/*
	 * THE FOUR COLUMNS OF THE SPAN COURSE, one pitch apart. A sits over the LEFT pillar (X = 0),
	 * D sits over the RIGHT pillar (X = 67.5). B and C are between them with NOTHING beneath — the
	 * seatless run. The LEFT pillar is always present; the RIGHT pillar and D are added only for the
	 * two-sided positive control.
	 */
	constexpr double AxCm = 0.0 * BrickPitchCm;   // 0     — seated on the left pillar (abutment)
	constexpr double BxCm = 1.0 * BrickPitchCm;   // 22.5  — no seat
	constexpr double CxCm = 2.0 * BrickPitchCm;   // 45    — no seat
	constexpr double DxCm = 3.0 * BrickPitchCm;   // 67.5  — seated on the right pillar (abutment)

	struct FSpannedRun
	{
		FStructure Structure;

		int32 LeftPillar = INDEX_NONE;
		int32 RightPillar = INDEX_NONE;

		int32 A = INDEX_NONE;
		int32 B = INDEX_NONE;
		int32 C = INDEX_NONE;
		int32 D = INDEX_NONE;
	};

	FPieceBox BrickBoxAt(double CentreXCm, double CentreZCm)
	{
		FPieceBox Box;
		Box.CentreCm = FVector(CentreXCm, 0.0, CentreZCm);
		Box.ExtentCm = FVector(BrickLengthCm, WytheWidthCm, BrickHeightCm) * 0.5;
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	int32 AddBrick(FStructure& S, const FPieceBox& Box, bool bGrounded)
	{
		return S.AddPiece(BoxMassKg(Box), bGrounded, Box.CentreCm);
	}

	/** Join two boxes with a mortar joint, appending the connection; returns its handle or INDEX_NONE. */
	int32 Join(FStructure& S, int32 HandleA, const FPieceBox& BoxA, int32 HandleB, const FPieceBox& BoxB)
	{
		FConnection Joint;
		if (MakeInterface(HandleA, BoxA, HandleB, BoxB, MortarJointCm, GeneralPurposeMortar, Joint))
		{
			return S.AddConnection(Joint);
		}
		return INDEX_NONE;
	}

	/**
	 * Lay the run. With bTwoSided, the mirrored right abutment (right pillar + D) is added so the
	 * group is abutted on both sides; without it, the run cantilevers off the single left abutment.
	 */
	void Build(FSpannedRun& Out, bool bTwoSided)
	{
		FStructure& S = Out.Structure;

		const FPieceBox LeftPillarBox = BrickBoxAt(AxCm, PillarCentreZCm);
		const FPieceBox ABox = BrickBoxAt(AxCm, SpanCentreZCm);
		const FPieceBox BBox = BrickBoxAt(BxCm, SpanCentreZCm);
		const FPieceBox CBox = BrickBoxAt(CxCm, SpanCentreZCm);

		Out.LeftPillar = AddBrick(S, LeftPillarBox, /*bGrounded*/ true);
		Out.A = AddBrick(S, ABox, /*bGrounded*/ false);
		Out.B = AddBrick(S, BBox, /*bGrounded*/ false);
		Out.C = AddBrick(S, CBox, /*bGrounded*/ false);

		/* The left abutment's seat, and the two head joints that build the group A-B-C. */
		Join(S, Out.LeftPillar, LeftPillarBox, Out.A, ABox);
		Join(S, Out.A, ABox, Out.B, BBox);
		Join(S, Out.B, BBox, Out.C, CBox);

		if (bTwoSided)
		{
			const FPieceBox RightPillarBox = BrickBoxAt(DxCm, PillarCentreZCm);
			const FPieceBox DBox = BrickBoxAt(DxCm, SpanCentreZCm);

			Out.RightPillar = AddBrick(S, RightPillarBox, /*bGrounded*/ true);
			Out.D = AddBrick(S, DBox, /*bGrounded*/ false);

			Join(S, Out.RightPillar, RightPillarBox, Out.D, DBox);
			Join(S, Out.C, CBox, Out.D, DBox);
		}
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (!S.IsPieceRemoved(Piece) && S.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++N;
			}
		}
		return N;
	}

	FString SupportName(EPieceSupport Support)
	{
		switch (Support)
		{
		case EPieceSupport::Falling: return TEXT("Falling");
		case EPieceSupport::Grounded: return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded: return TEXT("Stranded");
		default: return TEXT("?");
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRefusedArchFallsNotStrandsTest,
	"DestructionGame.Core.Structure.ARefusedOneSidedArchFallsRatherThanStranding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRefusedArchFallsNotStrandsTest::RunTest(const FString& Parameters)
{
	using namespace RefusedArchFallsNotStrandsTestSupport;

	/*
	 * THE EXPECTED BEHAVIOUR IS TIED TO THE MORTAR PROFILE, so assert the profile still carries the
	 * figures this fixture was reasoned against rather than importing them.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE: mortar tensile f_x1 must be the mean 0.7 MPa, profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.7);

	/*
	 * ===================================================================================
	 * POSITIVE CONTROL — BOTH ABUTMENTS. The arch fires, so the seatless run STANDS. This is what
	 * makes the red discriminate: it proves the run is capable of standing when abutted both sides,
	 * so the cantilever's fall is "the one-sided arch is refused", not "the arch never fires".
	 * ===================================================================================
	 */
	{
		FSpannedRun TwoSided;
		Build(TwoSided, /*bTwoSided*/ true);

		/* Force the ROUTER: cap 0 puts every non-empty structure above the equilibrium gate. */
		TwoSided.Structure.SetEquilibriumGateBlockCap(0);

		TestTrue(
			TEXT("FIXTURE: the two-sided run must know where every piece and joint is, or the arch "
				 "reseat is a silent no-op"),
			TwoSided.Structure.HasCompleteGeometry());

		TwoSided.Structure.SolveAndBreak();

		const EPieceSupport TwoSidedB = TwoSided.Structure.GetPieceSupport(TwoSided.B);
		const EPieceSupport TwoSidedC = TwoSided.Structure.GetPieceSupport(TwoSided.C);

		AddInfo(FString::Printf(
			TEXT("POSITIVE CONTROL (both abutments): B reads %s, C reads %s, stranded = %d"),
			*SupportName(TwoSidedB), *SupportName(TwoSidedC), StrandedCount(TwoSided.Structure)));

		TestEqual(
			TEXT("POSITIVE CONTROL: with an abutment on BOTH sides the arch fires and B is Supported"),
			TwoSidedB, EPieceSupport::Supported);

		TestEqual(
			TEXT("POSITIVE CONTROL: with an abutment on BOTH sides the arch fires and C is Supported"),
			TwoSidedC, EPieceSupport::Supported);

		TestEqual(
			TEXT("POSITIVE CONTROL: a standing two-sided arch strands nobody"),
			StrandedCount(TwoSided.Structure), 0);
	}

	/*
	 * ===================================================================================
	 * THE RED — ONE ABUTMENT. The opposition gate refuses the cantilever (correctly), and the
	 * refused run must FALL CLEANLY: B and C read Falling and the whole structure has 0 Stranded.
	 * Today they read Stranded (3) via the sign-blind head-joint fallback chain, so this is red.
	 * ===================================================================================
	 */
	{
		FSpannedRun OneSided;
		Build(OneSided, /*bTwoSided*/ false);

		OneSided.Structure.SetEquilibriumGateBlockCap(0);

		TestTrue(
			TEXT("FIXTURE: the one-sided run must know where every piece and joint is, or the "
				 "opposition gate never runs"),
			OneSided.Structure.HasCompleteGeometry());

		OneSided.Structure.SolveAndBreak();

		const EPieceSupport OneSidedB = OneSided.Structure.GetPieceSupport(OneSided.B);
		const EPieceSupport OneSidedC = OneSided.Structure.GetPieceSupport(OneSided.C);

		AddInfo(FString::Printf(
			TEXT("REFUSED CANTILEVER (one abutment): B reads %s, C reads %s, stranded = %d"),
			*SupportName(OneSidedB), *SupportName(OneSidedC), StrandedCount(OneSided.Structure)));

		/*
		 * THE INVARIANT THAT HOLDS WHETHER THE RUN STANDS OR FALLS: a refused-arch run is a solver
		 * limitation only if it reports Stranded. Ruling (b) is that it has genuinely no support,
		 * so nothing in the structure may read Stranded.
		 */
		TestEqual(
			TEXT("RED: the refused one-sided arch strands NOBODY — the released run has genuinely no "
				 "support path, so 0 Stranded (today the two middle bricks read Stranded)"),
			StrandedCount(OneSided.Structure), 0);

		TestEqual(
			TEXT("RED: middle brick B of a refused one-sided arch has lost the earth and reads Falling "
				 "(today it reads Stranded, enumerator 3)"),
			OneSidedB, EPieceSupport::Falling);

		TestEqual(
			TEXT("RED: middle brick C of a refused one-sided arch has lost the earth and reads Falling "
				 "(today it reads Stranded, enumerator 3)"),
			OneSidedC, EPieceSupport::Falling);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
