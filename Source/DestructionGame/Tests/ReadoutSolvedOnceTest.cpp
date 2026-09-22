// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The min-violation strain readout is solved once per below-cap settle, not once per cascade
 * pass. Solving it on every answered pass (keeping only the last) caused a 25x suite slowdown
 * (default suite 3 min -> 70 min). The fix solves it only on the terminal AuthoritativeNoBreak
 * pass; the kept readout is unchanged. GetMinViolationReadoutSolveCount is a test-only counter,
 * reset per SolveAndBreak.
 *
 * A (control): a brick on a narrower seat, 4 cm eccentric, stands. One solve; measured
 * M = 21375.76 uu.cm, utilisation 0.0007448.
 * B (driver): a two-load-path overhanging body collapses in two passes; the readout must be
 * solved once. Its settled readout is absent (the released body floats and the LP fails
 * closed); a present one would mean the pre-sever pass was cached instead.
 *
 * World-free. Named namespace for unity builds.
 */
namespace ReadoutSolvedOnceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Lengths in cm (1 uu = 1 cm).
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Every piece's Y depth, so every joint's Y overlap is full. */
	constexpr double WytheWidthCm = 10.25;

	constexpr double BedJointThicknessCm = 1.0;

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu conversion is already inside it. */
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

	/*
	 * Fixture A: brick X [0,28] on seat X [0,20], so e = 4 cm, just outside the 3.33 cm kern.
	 * Needs a little bond tension, well within GeneralPurposeMortar.
	 */

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

	// Measured settled readout on the bending bed joint.
	constexpr double ControlUtilisation = 0.0007448;
	constexpr double ControlMomentUuCm = 21375.76;

	// Fixture B: both seats lie left of the body's centre of mass, so no equilibrium exists.

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

/** The min-violation strain readout is solved once per below-cap settle. See the file header. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReadoutSolvedOnceTest,
	"DestructionGame.Acceptance.StrainReadout.ReadoutSolvedOncePerBelowCapCascade",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FReadoutSolvedOnceTest::RunTest(const FString& Parameters)
{
	using namespace ReadoutSolvedOnceTestSupport;

	// Fixture A: control.
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

	TestTrue(*FString::Printf(TEXT("CONTROL: the joint bends (M = %.9g ~ measured %.9g, non-zero)"),
			ControlReadout.MomentUuCm, ControlMomentUuCm),
		FMath::Abs(ControlReadout.MomentUuCm - ControlMomentUuCm) <= 1.0 && ControlReadout.MomentUuCm > 1.0);

	TestTrue(*FString::Printf(TEXT("CONTROL: settled bending utilisation %.9g == today's %.9g"),
			ControlReadout.Utilisation, ControlUtilisation),
		FMath::Abs(ControlReadout.Utilisation - ControlUtilisation) <= 1.0e-6);

	// Fixture B: multi-pass below-cap collapse.
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

	// Only the below-cap LP fells this body; the router alone reads it as safe.
	TestFalse(TEXT("COLLAPSE: the overhanging body has lost its path to the earth"),
		HasEarth(B.Structure, B.Body));
	TestTrue(TEXT("COLLAPSE: the anchor seat keeps the earth"),
		HasEarth(B.Structure, B.Anchor));
	TestTrue(TEXT("COLLAPSE: the pivot seat keeps the earth"),
		HasEarth(B.Structure, B.Pivot));
	TestEqual(TEXT("COLLAPSE: zero pieces stranded — a genuine fall, not a routing knot"),
		StrandedCount(B.Structure), 0);

	TestEqual(TEXT("[RED] the min-violation readout is solved exactly once per below-cap settle"),
		CollapseSolves, 1);

	// Absent, since the released body floats; present would mean the pre-sever pass was cached.
	TestFalse(TEXT("WHAT: the settled readout on the severed anchor bed joint is absent"),
		B.Structure.GetConnectionReadout(B.AnchorJoint).bPresent);
	TestFalse(TEXT("WHAT: the settled readout on the severed pivot bed joint is absent"),
		B.Structure.GetConnectionReadout(B.PivotJoint).bPresent);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
