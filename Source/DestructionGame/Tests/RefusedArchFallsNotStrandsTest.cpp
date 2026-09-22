// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Ruling (b), 2026-09-02: a masonry run that loses one springing of its arch falls cleanly (0
 * Stranded, released pieces Falling). ReseatSpannedGroups correctly refuses an arch abutted on one
 * side only. The run then falls back to its sign-blind head joints, becomes a mutual-support
 * cycle and reads Stranded, which is a solver limitation, not a structure. It has no load path,
 * so the right answer is Falling (DESIGN.md §4).
 *
 * Router behaviour only: SetEquilibriumGateBlockCap(0) forces the router, as for the 442-block
 * shed. Below the cap the LP may stand a one-sided mortar run on bond (case 21, DESIGN.md §8).
 *
 *   Both abutments (positive control, must stand):
 *      span course   [A=0][B=22.5][C=45][D=67.5]      A on LeftPillar, D on RightPillar
 *      ground       [LeftPillar]  (hole)  [RightPillar]
 *
 *   One abutment (the refused cantilever):
 *      span course   [A=0][B=22.5][C=45]              A on LeftPillar; nothing right of C
 *      ground       [LeftPillar]  (hole ->)
 *
 * Asserts support state, never displacement. The control proves the arch fires when it should,
 * so the cantilever's fall means refusal, not an arch that never fires. Masses and geometry are
 * derived here. No world needed. Named namespace for unity builds.
 */
namespace RefusedArchFallsNotStrandsTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Single wythe, so every joint has full Y overlap. */
	constexpr double WytheWidthCm = 10.25;

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickHeightCm = 6.5;
	constexpr double MortarJointCm = 1.0;

	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;

	// Grounded pillars are course 0; the span is course 1.
	constexpr double PillarCentreZCm = BrickHeightCm / 2.0;          // 3.25
	constexpr double PillarTopZCm = BrickHeightCm;                   // 6.5

	constexpr double SpanBottomZCm = PillarTopZCm + MortarJointCm;   // 7.5
	constexpr double SpanCentreZCm = SpanBottomZCm + BrickHeightCm / 2.0; // 10.75

	// Span columns one pitch apart; B and C have nothing beneath.
	constexpr double AxCm = 0.0 * BrickPitchCm;   // on the left pillar
	constexpr double BxCm = 1.0 * BrickPitchCm;
	constexpr double CxCm = 2.0 * BrickPitchCm;
	constexpr double DxCm = 3.0 * BrickPitchCm;   // on the right pillar (two-sided only)

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

	/** Joins two boxes with mortar; returns the connection handle or INDEX_NONE. */
	int32 Join(FStructure& S, int32 HandleA, const FPieceBox& BoxA, int32 HandleB, const FPieceBox& BoxB)
	{
		FConnection Joint;
		if (MakeInterface(HandleA, BoxA, HandleB, BoxB, MortarJointCm, GeneralPurposeMortar, Joint))
		{
			return S.AddConnection(Joint);
		}
		return INDEX_NONE;
	}

	/** Lays the run; bTwoSided adds the right pillar and D. */
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

	// The fixture was reasoned against this profile value.
	TestTrue(
		FString::Printf(TEXT("FIXTURE: mortar tensile f_x1 must be the mean 0.7 MPa, profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.7);

	// Positive control: both abutments, so the arch fires and the run stands.
	{
		FSpannedRun TwoSided;
		Build(TwoSided, /*bTwoSided*/ true);

		// Cap 0 forces the router.
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

	// One abutment: the refused run must fall cleanly, B and C Falling, 0 Stranded.
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
