// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Leaning-stack acceptance set: the self-weight fixture that demands a stability check
 * (DESIGN.md §7 gaps 1 and 6), and the red test that drove the overturning guard.
 *
 * A single column of mortared bricks, each course offset 10 cm along the length. Overturning
 * demand at the bottom bed joint grows as m^2 (m courses above), while its restoring capacity is
 * fixed, so a tall enough stack must fall.
 *
 * Why not the originally approved 2 cm/course at 5/10/15/20 courses: at 2 cm the mean bond holds
 * the stack to m >= 40-51, so every row honestly stands. Dry stone cannot be used either: zero
 * tension makes the model condemn any joint outside the kern, while a real dry stack rocks and
 * stands (the missing no-tension rocking model, a separate gap).
 *
 * Verdicts are ruled at MEAN bond (DESIGN.md §3): UK NA Table NA.6 gives characteristic
 * 0.3-0.5 MPa for clay units, mean runs about 2x, so the bracket is 0.6-1.0 MPa. Every verdict
 * clears both ends:
 *
 *     row  courses  m   first-crack demand      verdict     margin
 *      1      5      4   0.0854 MPa             STANDS      7.0x under the 0.6 floor
 *      2      8      7   0.2734 MPa             STANDS      2.2x under the 0.6 floor
 *      3     30     29   4.899 MPa              FALLS       4.9x over the 1.0 ceiling
 *      4     40     39   8.890 MPa              FALLS       8.9x over the 1.0 ceiling
 *
 * demand(m) = M/W - N/A at the bottom bed joint, with M = w*(d/2)*m^2, W = t*(L-d)^2/6,
 * N = m*w, A = t*(L-d). First-crack (elastic) is the right criterion for a brittle bond. The
 * falling rows also overturn under a generous rigid-plastic block at 1.0 MPa (1.59x and 2.90x).
 * The guard's bond figure must land between 0.2734 and 4.899 MPa.
 *
 * Why joint checks alone read the falling rows as safe: each course sits on one course, so the
 * whole stack is one corbelling body and gets a composite section D = 7.5*m. W_c grows as m^2,
 * cancelling the demand's m^2, so every joint reads the same composite tension at any height
 * (0.01388 MPa). Only ~2.15 courses actually cross any vertical plane (DESIGN.md §5.5, open).
 * The equilibrium guard (DESIGN.md §7) brings the rows down, and it must use a mean-basis bond:
 * at characteristic 0.10 MPa it would wrongly condemn corbel A, which survives 5.8x at 0.6.
 *
 * Assertions (DESIGN.md §4): STANDS rows assert nothing fell and no pass broke anything. FALLS
 * rows assert every course above the base lost the earth, the base kept it, and nothing is
 * Stranded. No displacement and no specific joint's HasGiven. The FStackCase pin fields are
 * unarmed machinery for the next known-wrong row.
 *
 * No world needed. Named namespace for unity builds. Only MakeInterface and the mortar profile
 * are imported (the profile's figures are asserted); statics, moduli, units and the bond bracket
 * are derived here so a wrong production constant disagrees.
 */
namespace LeaningStackTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Geometry, cm (1 uu = 1 cm).

	/** The standard brick. */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	/** Fired clay, 1.9 g/cm3. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/**
	 * Offset of each course past the one below, along the length. Large enough that the bond
	 * cannot rescue the tall rows at fixture-sized heights; one offset so rows vary only height.
	 */
	constexpr double OffsetPerCourseCm = 10.0;

	/** 1 cm mortar bed, so the course pitch is 7.5 cm. */
	constexpr double BedJointThicknessCm = 1.0;
	constexpr double CoursePitchCm = BrickHeightCm + BedJointThicknessCm;

	// Units and strength basis. Cited, not imported.

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. Deliberately not
	 * ForceUnitsPerMPaSqCm, so a wrong production constant fails here.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/**
	 * Mean bed-joint bond bracket, MPa. UK NA to BS EN 1996-1-1 Table NA.6 gives characteristic
	 * f_xk1 of 0.3-0.5 for clay units; mean runs about 2x (DESIGN.md §3). Verdicts must hold at both ends.
	 */
	constexpr double MeanBondFloorMPa = 0.6;
	constexpr double MeanBondCeilingMPa = 1.0;

	// Independent oracle: rigid-body moments about one joint, not the solver's support-graph method.

	/** Each bed joint is the overlap rectangle of two offset courses. */
	constexpr double OverlapCm = BrickLengthCm - OffsetPerCourseCm;
	constexpr double BedAreaSqCm = OverlapCm * BrickWidthCm;

	/** W = t * overlap^2 / 6, about the axis the stack leans over. */
	constexpr double BedModulusCm3 = BrickWidthCm * OverlapCm * OverlapCm / 6.0;

	/** Density-first order, matching PieceMassKg; 2.72163125 kg. */
	constexpr double BrickMassKg =
		ClayDensityGramsPerCubicCm * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 2667.198625 uu. */
	constexpr double BrickWeightUu = BrickMassKg * GravityCmPerSecondSquared;

	/**
	 * Overturning moment about the bottom bed joint's centroid, uu.cm. Course i is at i*d and the
	 * joint at d/2, so the levers sum to d*(m^2)/2 over i = 1..m.
	 */
	double BottomJointMomentUuCm(int32 CoursesAbove)
	{
		return BrickWeightUu * (OffsetPerCourseCm / 2.0)
			* double(CoursesAbove) * double(CoursesAbove);
	}

	/** First-crack bond tension demand at the bottom bed joint, MPa: M/W - N/A. */
	double FirstCrackDemandMPa(int32 CoursesAbove)
	{
		const double BendingUuPerSqCm = BottomJointMomentUuCm(CoursesAbove) / BedModulusCm3;
		const double MeanCompressionUuPerSqCm =
			double(CoursesAbove) * BrickWeightUu / BedAreaSqCm;

		return (BendingUuPerSqCm - MeanCompressionUuPerSqCm) / ForceUnitsPerMPaSqCmHere;
	}

	/**
	 * Rigid-plastic cross-check for FALLS rows: overturning about the bearing edge against the
	 * full bond block f * A * (overlap/2). Demand/capacity, so > 1 overturns. About 3x more
	 * forgiving than first-crack; it proves a falling row falls even under the kindest reading.
	 */
	double PlasticOverturningRatio(int32 CoursesAbove, double BondMPa)
	{
		// Body centroid d*(m+1)/2, less the bearing's leading edge at L/2.
		const double EdgeLeverCm =
			OffsetPerCourseCm * double(CoursesAbove + 1) / 2.0 - BrickLengthCm / 2.0;

		const double OverturningUuCm =
			double(CoursesAbove) * BrickWeightUu * EdgeLeverCm;

		const double RestoringUuCm =
			BondMPa * ForceUnitsPerMPaSqCmHere * BedAreaSqCm * (OverlapCm / 2.0);

		return OverturningUuCm / RestoringUuCm;
	}

	// The table.

	enum class EVerdict : uint8
	{
		/** Everything above the base comes down. */
		Falls,

		/** The bond holds with margin at the low end of the mean bracket. */
		Stands,
	};

	const TCHAR* VerdictName(EVerdict Verdict)
	{
		return Verdict == EVerdict::Falls ? TEXT("FALLS") : TEXT("STANDS");
	}

	struct FStackCase
	{
		int32 Number = 0;
		const TCHAR* Title = nullptr;

		/** What this row's pair varies, for failure messages. */
		const TCHAR* Isolates = nullptr;

		int32 Courses = 0;
		EVerdict Verdict = EVerdict::Stands;

		/**
		 * Known-wrong answer pins for a red row, so a regression within the failure is loud.
		 * Delete in the edit that fixes the row. Currently unset.
		 */
		int32 DropsToday = INDEX_NONE;
		double WorstUtilisationToday = 0.0;
	};

	TArray<FStackCase> AllStackCases()
	{
		TArray<FStackCase> Cases;

		// Row 1, shallow control: 0.0854 MPa. A guard with no bond term would condemn it.
		Cases.Add({ 1, TEXT("5 courses"), TEXT("height (vs case 4)"),
			/*Courses*/ 5, EVerdict::Stands });

		/*
		 * Row 2, tallest robust stand: 0.2734 MPa, 2.2x under the floor (9 courses would be
		 * only 1.67x). The guard's bond figure must exceed this.
		 */
		Cases.Add({ 2, TEXT("8 courses"), TEXT("height (vs case 3)"),
			/*Courses*/ 8, EVerdict::Stands });

		// Row 3, shortest robust fall: 4.899 MPa, 4.9x over the ceiling; 1.59x even rigid-plastic.
		Cases.Add({ 3, TEXT("30 courses"), TEXT("height (vs case 2)"),
			/*Courses*/ 30, EVerdict::Falls });

		// Row 4, anchor fall: 8.890 MPa, 8.9x at first crack, 2.90x rigid-plastic.
		Cases.Add({ 4, TEXT("40 courses"), TEXT("height (vs case 1)"),
			/*Courses*/ 40, EVerdict::Falls });

		return Cases;
	}

	// The fixture.

	struct FStack
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;

		/** The joint between the grounded base and course 1 (the critical one, m = n-1). */
		int32 BottomJoint = INDEX_NONE;
	};

	/**
	 * Lay a stack: course i centred at (i*d, 0, 3.25 + i*7.5), course 0 grounded. Every pair is
	 * offered to MakeInterface, which accepts only consecutive courses.
	 */
	void LayStack(int32 Courses, FStack& OutStack)
	{
		for (int32 Course = 0; Course < Courses; ++Course)
		{
			FPieceBox Box;
			Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				double(Course) * OffsetPerCourseCm,
				0.0,
				BrickHeightCm / 2.0 + double(Course) * CoursePitchCm);

			OutStack.Structure.AddPiece(BrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			OutStack.Boxes.Add(Box);
		}

		for (int32 First = 0; First < OutStack.Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < OutStack.Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(
						First, OutStack.Boxes[First], Second, OutStack.Boxes[Second],
						BedJointThicknessCm, GeneralPurposeMortar, Joint))
				{
					const int32 Handle = OutStack.Structure.AddConnection(Joint);

					if (First == 0 && Second == 1)
					{
						OutStack.BottomJoint = Handle;
					}
				}
			}
		}
	}

	/** Live pieces with no path to the earth. Stranded counts as fallen. */
	TArray<int32> FallenPieces(const FStack& Stack)
	{
		TArray<int32> Fallen;

		for (int32 Piece = 0; Piece < Stack.Structure.NumPieces(); ++Piece)
		{
			if (Stack.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Stack.Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				Fallen.Add(Piece);
			}
		}

		return Fallen;
	}

	/** Live pieces the solver could not route. A precondition, never a verdict. */
	int32 StrandedCount(const FStack& Stack)
	{
		int32 Stranded = 0;

		for (int32 Piece = 0; Piece < Stack.Structure.NumPieces(); ++Piece)
		{
			if (!Stack.Structure.IsPieceRemoved(Piece)
				&& Stack.Structure.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}

		return Stranded;
	}

	/** One row's readings as built, then after the cascade. */
	struct FStackResult
	{
		bool bLaid = false;

		/** From the non-destructive solve, before anything breaks. */
		double WorstUtilisation = 0.0;
		int32 WorstJoint = INDEX_NONE;
		double BottomUtilisation = 0.0;
		double BottomMomentUuCm = 0.0;
		double BottomCompositeDepthCm = 0.0;

		/** Passes that broke at least one joint. */
		int32 Passes = 0;

		TArray<int32> Fallen;
		int32 Stranded = 0;
	};

	/** Lay, read, then cascade. Readings describe the stack as built. */
	void RunStackCase(
		FAutomationTestBase& Test, const FStackCase& Case, FStack& OutStack, FStackResult& OutResult)
	{
		LayStack(Case.Courses, OutStack);

		if (OutStack.Structure.NumConnections() != Case.Courses - 1
			|| OutStack.BottomJoint == INDEX_NONE)
		{
			Test.AddError(FString::Printf(
				TEXT("case %d (%s): FIXTURE the producer must emit exactly one bed joint per ")
				TEXT("course above the base — %d course(s) got %d joint(s)"),
				Case.Number, Case.Title, Case.Courses, OutStack.Structure.NumConnections()));

			return;
		}

		OutResult.bLaid = true;

		OutStack.Structure.SolveLoads();

		for (int32 Index = 0; Index < OutStack.Structure.NumConnections(); ++Index)
		{
			const double Utilisation = OutStack.Structure.GetConnectionUtilisation(Index);

			if (Utilisation > OutResult.WorstUtilisation)
			{
				OutResult.WorstUtilisation = Utilisation;
				OutResult.WorstJoint = Index;
			}
		}

		OutResult.BottomUtilisation =
			OutStack.Structure.GetConnectionUtilisation(OutStack.BottomJoint);
		OutResult.BottomMomentUuCm =
			OutStack.Structure.GetConnectionMoment(OutStack.BottomJoint).Size();
		OutResult.BottomCompositeDepthCm =
			OutStack.Structure.GetConnectionCompositeDepthCm(OutStack.BottomJoint);

		OutResult.Passes = OutStack.Structure.SolveAndBreak();
		OutResult.Fallen = FallenPieces(OutStack);
		OutResult.Stranded = StrandedCount(OutStack);
	}

	/** Log the derived and solver readings for a row. */
	void ReportStackCase(
		FAutomationTestBase& Test, const FStackCase& Case, const FStackResult& Result)
	{
		if (!Result.bLaid)
		{
			return;
		}

		const int32 CoursesAbove = Case.Courses - 1;

		Test.AddInfo(FString::Printf(
			TEXT("case %d (%s): %s. DERIVED bottom-joint M %.10g uu.cm, first-crack demand ")
			TEXT("%.10g MPa against mean bond %.2g-%.2g, plastic edge ratio %.10g at the ")
			TEXT("ceiling. SOLVER bottom joint utilisation %.10g, |M| %.10g uu.cm, composite ")
			TEXT("depth %.10g cm; worst joint %d at %.10g; passes %d; fallen %d; stranded %d"),
			Case.Number, Case.Title, VerdictName(Case.Verdict),
			BottomJointMomentUuCm(CoursesAbove), FirstCrackDemandMPa(CoursesAbove),
			MeanBondFloorMPa, MeanBondCeilingMPa,
			PlasticOverturningRatio(CoursesAbove, MeanBondCeilingMPa),
			Result.BottomUtilisation, Result.BottomMomentUuCm, Result.BottomCompositeDepthCm,
			Result.WorstJoint, Result.WorstUtilisation, Result.Passes,
			Result.Fallen.Num(), Result.Stranded));
	}

	/**
	 * Preconditions for a row's verdict: correct fixture shape, nothing Stranded, and a ruling
	 * that clears the whole mean bracket (so no row drifts into the ambiguous 9-19 course band).
	 */
	void CheckFixture(
		FAutomationTestBase& Test, const FStackCase& Case, const FStack& Stack,
		const FStackResult& Result)
	{
		const FString Where = FString::Printf(TEXT("case %d (%s)"), Case.Number, Case.Title);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: FIXTURE every piece and joint must know where it is, or there is no ")
				TEXT("eccentricity anywhere and the whole ladder reads a centred load"),
				*Where),
			Stack.Structure.HasCompleteGeometry());

		for (int32 Index = 0; Index < Stack.Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Stack.Structure.GetConnection(Index);

			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: FIXTURE joint %d must be the overlap rectangle two offset ")
					TEXT("courses really share"),
					*Where, Index),
				Joint.InterfaceAreaSqCm, BedAreaSqCm);

			const int32 Upper = FMath::Max(Joint.PieceA, Joint.PieceB);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: FIXTURE joint %d must BEAR its upper course — a stack with a ")
					TEXT("joint in the shear tier is a different fixture"),
					*Where, Index),
				Stack.Structure.GetJointRole(Index, Upper) == EJointRole::BedBeneath);
		}

		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: no piece may be Stranded — a verdict decided by the solver declining ")
				TEXT("to route is not a verdict about a leaning stack"),
				*Where),
			Result.Stranded, 0);

		const int32 CoursesAbove = Case.Courses - 1;
		const double DemandMPa = FirstCrackDemandMPa(CoursesAbove);

		if (Case.Verdict == EVerdict::Stands)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: RULING a STANDS row must clear the conservative end of the mean ")
					TEXT("bond bracket with real margin; demand %.10g MPa against %.2g needs ")
					TEXT("2x and has %.10g"),
					*Where, DemandMPa, MeanBondFloorMPa, MeanBondFloorMPa / DemandMPa),
				DemandMPa * 2.0 <= MeanBondFloorMPa);
		}
		else
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: RULING a FALLS row must clear the generous end of the mean bond ")
					TEXT("bracket with real margin; demand %.10g MPa against %.2g needs 2x ")
					TEXT("and has %.10g"),
					*Where, DemandMPa, MeanBondCeilingMPa, DemandMPa / MeanBondCeilingMPa),
				DemandMPa >= MeanBondCeilingMPa * 2.0);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: RULING a FALLS row must overturn even under the most charitable ")
					TEXT("reading, a rigid-plastic bond block at the mean ceiling; the edge ")
					TEXT("ratio is %.10g and must exceed 1.5"),
					*Where, PlasticOverturningRatio(CoursesAbove, MeanBondCeilingMPa)),
				PlasticOverturningRatio(CoursesAbove, MeanBondCeilingMPa) > 1.5);
		}

		/*
		 * Bond tension must govern. The only competitor is edge compression (no shear on
		 * horizontal beds); at row 4 that is 8.98 MPa against ~10, while tension is 8.9x over.
		 */
		const double EdgeCompressionMPa = DemandMPa
			+ 2.0 * double(CoursesAbove) * BrickWeightUu / BedAreaSqCm / ForceUnitsPerMPaSqCmHere;

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: RULING bond tension must govern the verdict by a clear margin over ")
				TEXT("mortar crushing (tension %.10g of its %.2g ceiling; compression %.10g ")
				TEXT("of ~10)"),
				*Where, DemandMPa / MeanBondCeilingMPa, MeanBondCeilingMPa,
				EdgeCompressionMPa / 10.0),
			DemandMPa / MeanBondCeilingMPa > 5.0 * (EdgeCompressionMPa / 10.0)
				|| Case.Verdict == EVerdict::Stands);
	}
}

/**
 * The catalogue: four heights of one lean, each with its real verdict. Cases 1 and 2 are
 * controls against a guard that condemns every offset stack. Cases 3 and 4 read ~0.1388 on the
 * joint checks, like case 2, so only the equilibrium gate (BreakByEquilibrium) brings them
 * down. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLeaningStackCatalogueTest,
	"DestructionGame.Acceptance.LeaningStack.Catalogue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLeaningStackCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace LeaningStackTestSupport;

	/*
	 * Pin the profile figures. The coded bond is now a mean (0.70), inside this file's
	 * independent 0.6-1.0 bracket. Keep the bracket independent so a profile retune fails here.
	 */
	TestEqual(TEXT("FIXTURE: the mortar's coded bond is the mean-basis 0.70 (re-anchor flip 2026-08-14)"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.7);

	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: the coded mean bond %g must sit strictly inside this file's independent ")
			TEXT("0.6-1.0 mean bracket, or the verdict table needs re-ruling"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa > 0.6 - 1.0e-12
			&& GeneralPurposeMortar.TensileStrengthMPa < 1.0);

	TestEqual(TEXT("FIXTURE: the mortar's coded crushing strength is M10's 10 MPa"),
		GeneralPurposeMortar.CompressiveStrengthMPa, 10.0);

	// 10.25 * 11.5^2 / 6 = 225.927 cm3.
	TestEqual(TEXT("FIXTURE: the overlap rectangle's modulus is t * overlap^2 / 6"),
		BedModulusCm3, 10.25 * 11.5 * 11.5 / 6.0);

	const TArray<FStackCase> Cases = AllStackCases();

	TestEqual(TEXT("FIXTURE: the catalogue is four cases"), Cases.Num(), 4);

	for (const FStackCase& Case : Cases)
	{
		FStack Stack;
		FStackResult Result;

		RunStackCase(*this, Case, Stack, Result);
		ReportStackCase(*this, Case, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		CheckFixture(*this, Case, Stack, Result);

		const FString Where = FString::Printf(
			TEXT("case %d (%s) [isolates %s]"), Case.Number, Case.Title, Case.Isolates);

		if (Case.Verdict == EVerdict::Stands)
		{
			// Both halves: a severed bed can leave everything in place, so check breaks too.
			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means nothing lost the earth; %d piece(s) did"),
					*Where, Result.Fallen.Num()),
				Result.Fallen.Num(), 0);

			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means no joint gave; the cascade ran %d breaking pass(es)"),
					*Where, Result.Passes),
				Result.Passes, 0);

			continue;
		}

		// 4.9-8.9 MPa of demand against at most 1.0: everything above the base must come down.
		TArray<int32> WronglyStanding;

		for (int32 Course = 1; Course < Case.Courses; ++Course)
		{
			if (!Result.Fallen.Contains(Course))
			{
				WronglyStanding.Add(Course);
			}
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s: a lean the bond cannot hold comes down — every course above the ")
				TEXT("base must lose the earth, and %d of %d kept it; the bottom joint's ")
				TEXT("first-crack demand is %.10g MPa against a mean bond of at most %.2g, ")
				TEXT("and the model instead read the worst joint (%d) at %.10g of coded ")
				TEXT("capacity by crediting a %.10g cm composite section"),
				*Where, WronglyStanding.Num(), Case.Courses - 1,
				FirstCrackDemandMPa(Case.Courses - 1), MeanBondCeilingMPa,
				Result.WorstJoint, Result.WorstUtilisation, Result.BottomCompositeDepthCm),
			WronglyStanding.Num(), 0);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the grounded base is not what failed; it must keep the earth"),
				*Where),
			Stack.Structure.GetPieceSupport(0) == EPieceSupport::Grounded);

		// Known-red pins, armed only when a row sets DropsToday.
		if (Case.DropsToday != INDEX_NONE)
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: KNOWN-RED PIN — the model drops %d piece(s) here today. If you ")
					TEXT("just fixed this row, delete its DropsToday and WorstUtilisationToday; ")
					TEXT("if nothing here was meant to change, a regression moved a wrong ")
					TEXT("answer to a different wrong answer"),
					*Where, Case.DropsToday),
				Result.Fallen.Num(), Case.DropsToday);

			TestTrue(
				*FString::Printf(
					TEXT("%s: KNOWN-RED PIN — the model reads the worst joint at %.17g today ")
					TEXT("(height-independent: the composite m^2 cancels the demand's m^2); ")
					TEXT("it read %.17g"),
					*Where, Case.WorstUtilisationToday, Result.WorstUtilisation),
				FMath::Abs(Result.WorstUtilisation - Case.WorstUtilisationToday)
					<= Case.WorstUtilisationToday * 1.0e-9);
		}
	}

	return true;
}

/**
 * Height alone decides the outcome: cases 2 and 4 differ only in course count, and 8 courses
 * (0.2734 MPa) must stand while 40 (8.890 MPa) fall. States the discrimination as one relation
 * that a height-independent reading cannot fake. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLeaningStackHeightTest,
	"DestructionGame.Acceptance.LeaningStack.TheHeightAloneDecidesTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLeaningStackHeightTest::RunTest(const FString& Parameters)
{
	using namespace LeaningStackTestSupport;

	const TArray<FStackCase> Cases = AllStackCases();

	const FStackCase& Shorter = Cases[1];
	const FStackCase& Taller = Cases[3];

	TestTrue(TEXT("FIXTURE: the pair really is two heights of one lean"),
		Shorter.Courses < Taller.Courses);

	FStack ShorterStack;
	FStackResult ShorterResult;
	RunStackCase(*this, Shorter, ShorterStack, ShorterResult);
	ReportStackCase(*this, Shorter, ShorterResult);

	FStack TallerStack;
	FStackResult TallerResult;
	RunStackCase(*this, Taller, TallerStack, TallerResult);
	ReportStackCase(*this, Taller, TallerResult);

	if (!ShorterResult.bLaid || !TallerResult.bLaid)
	{
		return false;
	}

	TestTrue(
		*FString::Printf(
			TEXT("the same lean at two heights must separate: %d courses at %.10g MPa of ")
			TEXT("bond demand stand while %d at %.10g fall; the model dropped %d piece(s) ")
			TEXT("from the short stack and %d from the tall one"),
			Shorter.Courses, FirstCrackDemandMPa(Shorter.Courses - 1),
			Taller.Courses, FirstCrackDemandMPa(Taller.Courses - 1),
			ShorterResult.Fallen.Num(), TallerResult.Fallen.Num()),
		TallerResult.Fallen.Num() > ShorterResult.Fallen.Num());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
