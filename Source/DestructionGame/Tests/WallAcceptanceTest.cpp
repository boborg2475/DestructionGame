// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The wall acceptance set: twenty-two configurations, each asserting what real masonry does, not
 * what the solver computes. Catalogue: claude_plans/WALL_CASES.html. Matched-pair discrimination
 * lives in per-joint readings and the LP oracle, not in outcomes.
 *
 * Verdicts (DESIGN.md §4, outcome not mechanism): STANDS = nothing fell and no joint gave; LOCAL
 * LOSS = the fallen set is exactly the named set; COLLAPSE = a named region fell and a named
 * survivor region kept the ground. Displacement is never a break assertion.
 *
 * Grid, brick weight, N-to-uu conversion and strengths are re-derived here, not imported, so a wrong
 * production constant shows up. Named namespace because unity builds merge anonymous ones.
 */
namespace WallAcceptanceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// UK metric brick with a 1 cm joint: one cell = 22.5 cm, one course = 7.5 cm. Positions are in cells.
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickDepthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double JointCm = 1.0;

	constexpr double CellPitchCm = BrickLengthCm + JointCm;
	constexpr double CoursePitchCm = BrickHeightCm + JointCm;
	constexpr double HalfCellCm = CellPitchCm * 0.5;

	/** Half bat: (brick - joint) / 2. */
	constexpr double HalfBatLengthCm = (BrickLengthCm - JointCm) * 0.5;

	/** Left face of every wall; cell 0 is the first full brick. */
	constexpr double LeftFaceCm = -BrickLengthCm * 0.5;

	/** Shortest closing piece at a course's left end; shorter slivers are dropped. */
	constexpr double MinClosingPieceCm = 4.0;

	/** g/cm3, ClayBrick's density spelled out so a changed profile shows up. */
	constexpr double ClayBrickDensityGramsPerCubicCm = 1.9;

	/** kg x 980 cm/s2 is already weight in uu; applying 1 N = 100 uu again is a 100x error (DESIGN.md §3). */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double FullBrickWeightUu =
		ClayBrickDensityGramsPerCubicCm * BrickLengthCm * BrickDepthCm * BrickHeightCm / 1000.0
		* GravityCmPerSecondSquared;

	/** uu per MPa per cm2, not imported from ForceUnitsPerMPaSqCm so a wrong one disagrees. */
	constexpr double ForceUnitsPerMPaSqCmHere = 10000.0;

	// Mean shear bond f_v0 (Gooch et al. 2023/2025), asserted against the profile.
	constexpr double MortarShearCohesionMPa = 0.9;

	// Mean flexural bond f_x1, asserted against the profile.
	constexpr double MortarFlexuralBondMPa = 0.7;

	// Rest of the Mohr-Coulomb set, asserted in SpanIsReadInTheJointNotInTheOutcome.
	constexpr double MortarFrictionCoefficient = 0.75;
	constexpr double MortarMaxShearStrengthMPa = 2.0;
	constexpr double MortarCompressiveStrengthMPa = 10.0;

	/** A head joint: a brick's end face. */
	constexpr double HeadJointAreaSqCm = BrickDepthCm * BrickHeightCm;

	enum class EBond : uint8
	{
		/** Alternate courses offset half a cell, ends closed with half bats. */
		Running,

		/** Every course identical; head joints line up. */
		Stack,
	};

	enum class EVerdict : uint8
	{
		Stands,
		LocalLoss,
		Collapse,
	};

	const TCHAR* VerdictName(EVerdict Verdict)
	{
		switch (Verdict)
		{
		case EVerdict::Stands:    return TEXT("STANDS");
		case EVerdict::LocalLoss: return TEXT("LOCAL LOSS");
		default:                  return TEXT("COLLAPSE");
		}
	}

	/**
	 * A rectangle in (course, cell) space: courses inclusive, cells strictly between the bounds.
	 * Brick centres sit on quarter cells, so bounds never need a tolerance.
	 */
	struct FWallRegion
	{
		int32 CourseLo = 0;
		int32 CourseHi = 0;
		double CellLo = 0.0;
		double CellHi = 0.0;
	};

	struct FWallCase
	{
		int32 Number = 0;
		const TCHAR* Title = nullptr;
		EVerdict Verdict = EVerdict::Stands;

		/** What this row's matched pair varies, printed on failure. Null for an unpaired row. */
		const TCHAR* Isolates = nullptr;

		int32 Courses = 0;

		/** Full bricks in an even course. */
		int32 Cells = 0;

		EBond Bond = EBond::Running;

		/** First course that steps out, or INDEX_NONE. Every course from it steps out again. */
		int32 CorbelFromCourse = INDEX_NONE;
		double CorbelStepCm = 0.0;

		/** A course whose end brick projects half a cell past the face, or INDEX_NONE. */
		int32 ProjectingCourse = INDEX_NONE;

		/** What the player deletes, applied after the intact wall is checked. */
		TArrayView<const FWallRegion> Cuts;

		/** What must lose its path to earth: exact for a local loss, a lower bound for a collapse. */
		TArrayView<const FWallRegion> MustFall;

		/** What must keep it. Read only for a collapse. */
		TArrayView<const FWallRegion> MustStand;

		/**
		 * Pieces the model wrongly drops today, or INDEX_NONE. Pins a known-red row so a regression
		 * cannot hide inside it. Delete when the row is fixed; never move it without saying why.
		 */
		int32 DropsToday = INDEX_NONE;

		/** Live pieces the solver cannot route today. Nonzero is a characterised defect (DESIGN.md §4). */
		int32 StrandsToday = 0;

		/** Pinned pre-cascade worst reading, or 0.0 when not pinned. Case 22 collapses on a knife edge. */
		double PreCascadeWorstToday = 0.0;
	};

	/*
	 * The fixture's own bricklayer, since six cases are not running-bond rectangles. Each course is
	 * a right face and a rightmost-piece length, laid right to left so the cut piece lands at the
	 * left end, clear of corbels and headers. TheFixtureLaysTheWallTheProducerLays pins a flush wall
	 * against Layout::RunningBond.
	 */

	/** The right face of a flush wall of this many cells. */
	double FlushRightFaceCm(int32 Cells)
	{
		return (Cells - 1) * CellPitchCm + BrickLengthCm * 0.5;
	}

	/** Where a course's right face is and how long its rightmost piece is. */
	void CourseGeometry(const FWallCase& Case, int32 Course, double& OutRightCm, double& OutFirstLenCm)
	{
		OutRightCm = FlushRightFaceCm(Case.Cells);

		OutFirstLenCm = (Case.Bond == EBond::Stack || (Course % 2) == 0)
			? BrickLengthCm
			: HalfBatLengthCm;

		if (Case.ProjectingCourse == Course)
		{
			OutRightCm += HalfCellCm;
			OutFirstLenCm = BrickLengthCm;
		}

		if (Case.CorbelFromCourse != INDEX_NONE && Course >= Case.CorbelFromCourse)
		{
			OutRightCm += (Course - Case.CorbelFromCourse + 1) * Case.CorbelStepCm;
			OutFirstLenCm = BrickLengthCm;
		}
	}

	/** Mass in kg from geometry (cm3 x g/cm3 / 1000); no force conversion applies to mass. */
	double PieceMassKgHere(const FPieceBox& Box)
	{
		return ClayBrickDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/** A wall plus each piece's (course, cell). */
	struct FWall
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
		TArray<int32> CourseOf;
		TArray<double> CellOf;

		int32 NumPieces() const { return Boxes.Num(); }
	};

	bool RegionContains(const FWallRegion& Region, int32 Course, double Cell)
	{
		return Course >= Region.CourseLo
			&& Course <= Region.CourseHi
			&& Cell > Region.CellLo
			&& Cell < Region.CellHi;
	}

	bool AnyRegionContains(TArrayView<const FWallRegion> Regions, int32 Course, double Cell)
	{
		for (const FWallRegion& Region : Regions)
		{
			if (RegionContains(Region, Course, Cell))
			{
				return true;
			}
		}

		return false;
	}

	/** Lay the wall the case describes, before anything is cut out of it. */
	void LayWall(const FWallCase& Case, FWall& OutWall)
	{
		TArray<TArray<int32>> HandlesInCourse;

		for (int32 Course = 0; Course < Case.Courses; ++Course)
		{
			const double CentreZCm = BrickHeightCm * 0.5 + Course * CoursePitchCm;

			double RightCm = 0.0;
			double LenCm = 0.0;
			CourseGeometry(Case, Course, RightCm, LenCm);

			TArray<int32> Handles;

			while (RightCm - LeftFaceCm >= MinClosingPieceCm)
			{
				double LeftCm = RightCm - LenCm;

				if (LeftCm < LeftFaceCm)
				{
					LeftCm = LeftFaceCm;
					LenCm = RightCm - LeftCm;

					if (LenCm < MinClosingPieceCm)
					{
						break;
					}
				}

				FPieceBox Box;
				Box.CentreCm = FVector((LeftCm + RightCm) * 0.5, 0.0, CentreZCm);
				Box.ExtentCm = FVector(LenCm, BrickDepthCm, BrickHeightCm) * 0.5;

				// Without the centre of mass every corbel reads as centred.
				const int32 Handle = OutWall.Structure.AddPiece(
					PieceMassKgHere(Box), Course == 0, Box.CentreCm);

				OutWall.Boxes.Add(Box);
				OutWall.CourseOf.Add(Course);
				OutWall.CellOf.Add(Box.CentreCm.X / CellPitchCm);
				Handles.Add(Handle);

				RightCm = LeftCm - JointCm;
				LenCm = BrickLengthCm;
			}

			HandlesInCourse.Add(MoveTemp(Handles));
		}

		// Offer neighbours and the whole course below; MakeInterface rejects pairs that share no face.
		for (int32 Course = 0; Course < HandlesInCourse.Num(); ++Course)
		{
			const TArray<int32>& Row = HandlesInCourse[Course];

			for (int32 Index = 0; Index < Row.Num(); ++Index)
			{
				for (int32 Other = Index + 1; Other < Row.Num(); ++Other)
				{
					FConnection Joint;

					if (MakeInterface(
							Row[Index], OutWall.Boxes[Row[Index]],
							Row[Other], OutWall.Boxes[Row[Other]],
							JointCm, GeneralPurposeMortar, Joint))
					{
						OutWall.Structure.AddConnection(Joint);
					}
				}

				if (Course == 0)
				{
					continue;
				}

				for (const int32 Below : HandlesInCourse[Course - 1])
				{
					FConnection Joint;

					if (MakeInterface(
							Below, OutWall.Boxes[Below],
							Row[Index], OutWall.Boxes[Row[Index]],
							JointCm, GeneralPurposeMortar, Joint))
					{
						OutWall.Structure.AddConnection(Joint);
					}
				}
			}
		}
	}

	/** Every piece the case cuts away, in handle order. */
	TArray<int32> CutPieces(const FWallCase& Case, const FWall& Wall)
	{
		TArray<int32> Cut;

		for (int32 Piece = 0; Piece < Wall.NumPieces(); ++Piece)
		{
			if (AnyRegionContains(Case.Cuts, Wall.CourseOf[Piece], Wall.CellOf[Piece]))
			{
				Cut.Add(Piece);
			}
		}

		return Cut;
	}

	/** Live pieces with no path to earth. Stranded counts as fallen here; StrandedCount reports it apart. */
	TArray<int32> FallenPieces(const FWall& Wall)
	{
		TArray<int32> Fallen;

		for (int32 Piece = 0; Piece < Wall.Structure.NumPieces(); ++Piece)
		{
			if (Wall.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Wall.Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				Fallen.Add(Piece);
			}
		}

		return Fallen;
	}

	/** Live pieces the solver could not route. A precondition, not a verdict (DESIGN.md §4). */
	int32 StrandedCount(const FWall& Wall)
	{
		int32 Stranded = 0;

		for (int32 Piece = 0; Piece < Wall.Structure.NumPieces(); ++Piece)
		{
			if (!Wall.Structure.IsPieceRemoved(Piece)
				&& Wall.Structure.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}

		return Stranded;
	}

	/** Worst utilisation over live joints, and its pair. */
	double WorstUtilisation(const FWall& Wall, int32& OutPieceA, int32& OutPieceB)
	{
		double Worst = 0.0;
		OutPieceA = INDEX_NONE;
		OutPieceB = INDEX_NONE;

		for (int32 Index = 0; Index < Wall.Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Wall.Structure.GetConnection(Index);

			if (Joint.HasGiven())
			{
				continue;
			}

			const double Utilisation = Wall.Structure.GetConnectionUtilisation(Index);

			if (Utilisation > Worst)
			{
				Worst = Utilisation;
				OutPieceA = Joint.PieceA;
				OutPieceB = Joint.PieceB;
			}
		}

		return Worst;
	}

	/** A piece list as (course, cell), capped for the log. */
	FString DescribePieces(const FWall& Wall, const TArray<int32>& Pieces)
	{
		if (Pieces.Num() == 0)
		{
			return TEXT("{}");
		}

		constexpr int32 MaxShown = 24;

		FString Line = TEXT("{");

		for (int32 Index = 0; Index < Pieces.Num() && Index < MaxShown; ++Index)
		{
			Line += FString::Printf(
				TEXT("%sc%d/%g"),
				Index == 0 ? TEXT("") : TEXT(", "),
				Wall.CourseOf[Pieces[Index]],
				Wall.CellOf[Pieces[Index]]);
		}

		if (Pieces.Num() > MaxShown)
		{
			Line += FString::Printf(TEXT(", +%d more"), Pieces.Num() - MaxShown);
		}

		return Line + TEXT("}");
	}

	/** What one case's wall did after laying, cutting and cascading. */
	struct FWallResult
	{
		bool bLaid = false;

		int32 PiecesLaid = 0;
		int32 PiecesCut = 0;

		/** Passes that broke a joint, as built and after the cut. */
		int32 IntactPasses = 0;
		int32 CutPasses = 0;

		/** Whether the intact wall stood; meaningful only for a cutting case. */
		bool bIntactStood = false;

		/** Pieces dropped before the cut. */
		int32 IntactFallen = 0;

		/** Read as built: HasCompleteGeometry ignores removed pieces, so asking after the cut hides defects. */
		bool bCompleteGeometryAsBuilt = false;

		TArray<int32> Fallen;

		/** Of those, how many the solver could not route. */
		int32 Stranded = 0;

		double Worst = 0.0;
		int32 WorstPieceA = INDEX_NONE;
		int32 WorstPieceB = INDEX_NONE;

		/** Worst reading before the cascade (post-cut if cutting). `Worst` is post-cascade, survivors only. */
		double PreCascadeWorst = 0.0;
		int32 PreCascadeWorstPieceA = INDEX_NONE;
		int32 PreCascadeWorstPieceB = INDEX_NONE;
	};

	/**
	 * Lay, cut, cascade, record. No assertions: this half is pure and cached, and CheckWallFixture
	 * re-runs for every caller so a fixture failure does not depend on test order.
	 */
	void SolveWallCase(const FWallCase& Case, FWall& OutWall, FWallResult& OutResult)
	{
		LayWall(Case, OutWall);

		OutResult.PiecesLaid = OutWall.NumPieces();

		if (OutResult.PiecesLaid == 0)
		{
			return;
		}

		OutResult.bCompleteGeometryAsBuilt = OutWall.Structure.HasCompleteGeometry();

		// As-built reading; a cutting row overwrites it with the post-cut one.
		OutWall.Structure.SolveLoads();

		OutResult.PreCascadeWorst = WorstUtilisation(
			OutWall, OutResult.PreCascadeWorstPieceA, OutResult.PreCascadeWorstPieceB);

		OutResult.IntactPasses = OutWall.Structure.SolveAndBreak();
		OutResult.IntactFallen = FallenPieces(OutWall).Num();
		OutResult.bIntactStood = OutResult.IntactPasses == 0 && OutResult.IntactFallen == 0;

		if (Case.Cuts.Num() > 0)
		{
			const TArray<int32> Cut = CutPieces(Case, OutWall);

			OutResult.PiecesCut = Cut.Num();

			if (Cut.Num() == 0)
			{
				return;
			}

			for (const int32 Piece : Cut)
			{
				OutWall.Structure.RemovePiece(Piece);
			}

			OutWall.Structure.SolveLoads();

			OutResult.PreCascadeWorst = WorstUtilisation(
				OutWall, OutResult.PreCascadeWorstPieceA, OutResult.PreCascadeWorstPieceB);

			OutResult.CutPasses = OutWall.Structure.SolveAndBreak();
		}

		OutResult.Fallen = FallenPieces(OutWall);
		OutResult.Stranded = StrandedCount(OutWall);
		OutResult.Worst = WorstUtilisation(OutWall, OutResult.WorstPieceA, OutResult.WorstPieceB);
		OutResult.bLaid = true;
	}

	/** Preconditions a row must meet before its verdict means anything; asserted once per caller. */
	void CheckWallFixture(
		FAutomationTestBase& Test,
		const FWallCase& Case,
		const FWall& Wall,
		const FWallResult& Result)
	{
		if (Result.PiecesLaid == 0)
		{
			Test.AddError(FString::Printf(
				TEXT("case %d (%s): FIXTURE laid no bricks at all"), Case.Number, Case.Title));

			return;
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("case %d (%s): FIXTURE every brick and every joint must know where it is, or ")
				TEXT("there are no moments and every corbel reads as centred"),
				Case.Number, Case.Title),
			Result.bCompleteGeometryAsBuilt);

		if (Case.Cuts.Num() > 0)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("case %d (%s): FIXTURE the wall must stand before the player cuts it; it ")
					TEXT("broke joints in %d pass(es) and dropped %d piece(s) as built"),
					Case.Number, Case.Title,
					Result.IntactPasses, Result.IntactFallen),
				Result.bIntactStood);

			if (Result.PiecesCut == 0)
			{
				Test.AddError(FString::Printf(
					TEXT("case %d (%s): FIXTURE the cut regions named no brick at all"),
					Case.Number, Case.Title));

				return;
			}
		}

		// FallenPieces folds Stranded in, so without this a row could pass for the wrong reason.
		Test.TestEqual(
			*FString::Printf(
				TEXT("case %d (%s): FIXTURE %s — a verdict decided by the solver declining to divide ")
				TEXT("load round a loop is a statement about the solver rather than about the wall, ")
				TEXT("and %d of the %d piece(s) that came down here got there that way"),
				Case.Number, Case.Title,
				Case.StrandsToday == 0
					? TEXT("no live piece may be Stranded")
					: *FString::Printf(
						TEXT("this row is CHARACTERISED as stranding %d live piece(s) — a known ")
						TEXT("solver limitation, not an expectation"),
						Case.StrandsToday),
				Result.Stranded, Result.Fallen.Num()),
			Result.Stranded, Case.StrandsToday);
	}

	/** One cached wall and its result. */
	struct FSolvedWall
	{
		FWall Wall;
		FWallResult Result;
	};

	/** Cache key: exactly the fields the solve reads (geometry and cuts), not number, title or verdict. */
	FString SolveKeyOf(const FWallCase& Case)
	{
		FString Key = FString::Printf(
			TEXT("%d|%d|%d|%d|%.17g|%d"),
			Case.Courses, Case.Cells, static_cast<int32>(Case.Bond),
			Case.CorbelFromCourse, Case.CorbelStepCm, Case.ProjectingCourse);

		for (const FWallRegion& Region : Case.Cuts)
		{
			Key += FString::Printf(
				TEXT("|%d,%d,%.17g,%.17g"),
				Region.CourseLo, Region.CourseHi, Region.CellLo, Region.CellHi);
		}

		return Key;
	}

	/** Solved once per key. Held by pointer because TMap moves values on growth and callers keep references. */
	const FSolvedWall& SolvedWallCase(const FWallCase& Case)
	{
		static TMap<FString, TUniquePtr<FSolvedWall>> Cache;

		const FString Key = SolveKeyOf(Case);

		if (TUniquePtr<FSolvedWall>* Found = Cache.Find(Key))
		{
			return *Found->Get();
		}

		TUniquePtr<FSolvedWall> Solved = MakeUnique<FSolvedWall>();

		SolveWallCase(Case, Solved->Wall, Solved->Result);

		return *Cache.Add(Key, MoveTemp(Solved)).Get();
	}

	/** The cached answer, with this caller's fixture checks run over it. */
	const FSolvedWall& RunWallCase(FAutomationTestBase& Test, const FWallCase& Case)
	{
		const FSolvedWall& Solved = SolvedWallCase(Case);

		CheckWallFixture(Test, Case, Solved.Wall, Solved.Result);

		return Solved;
	}

	/** One log line per case, pass or fail. */
	void ReportWallCase(
		FAutomationTestBase& Test,
		const FWallCase& Case,
		const FWall& Wall,
		const FWallResult& Result)
	{
		Test.AddInfo(FString::Printf(
			TEXT("case %02d %-44s expected %-10s | laid %d, cut %d, passes %d(+%d), fell %d (%d ")
			TEXT("stranded) %s, worst %.6g%s, pre-cascade worst %.17g%s"),
			Case.Number, Case.Title, VerdictName(Case.Verdict),
			Result.PiecesLaid, Result.PiecesCut, Result.IntactPasses, Result.CutPasses,
			Result.Fallen.Num(), Result.Stranded, *DescribePieces(Wall, Result.Fallen),
			Result.Worst,
			Result.WorstPieceA == INDEX_NONE
				? TEXT("")
				: *FString::Printf(
					TEXT(" at c%d/%g-c%d/%g"),
					Wall.CourseOf[Result.WorstPieceA], Wall.CellOf[Result.WorstPieceA],
					Wall.CourseOf[Result.WorstPieceB], Wall.CellOf[Result.WorstPieceB]),
			Result.PreCascadeWorst,
			Result.PreCascadeWorstPieceA == INDEX_NONE
				? TEXT("")
				: *FString::Printf(
					TEXT(" at c%d/%g-c%d/%g"),
					Wall.CourseOf[Result.PreCascadeWorstPieceA],
					Wall.CellOf[Result.PreCascadeWorstPieceA],
					Wall.CourseOf[Result.PreCascadeWorstPieceB],
					Wall.CellOf[Result.PreCascadeWorstPieceB])));
	}

	/* The catalogue. */

	/*
	 * --- A: one brick out. ---
	 * Thirty courses because the half-seated joint reaches 1.0 at eighteen courses of cover
	 * (ARCHING_DESIGN.md); a ten-course wall would stand whatever the model does.
	 */
	constexpr int32 TallCourses = 30;
	constexpr int32 StandardCells = 12;

	const FWallRegion Case2Cuts[] = { { 1, 1, 5.25, 5.75 } };
	const FWallRegion Case3Cuts[] = { { 1, 1, 11.00, 11.50 } };
	const FWallRegion Case4Cuts[] = { { 0, 0, 4.75, 5.25 } };

	const FWallRegion Case5Cuts[] =
	{
		{ 1, 1, 1.25, 1.75 },
		{ 1, 1, 3.25, 3.75 },
		{ 1, 1, 5.25, 5.75 },
		{ 1, 1, 7.25, 7.75 },
		{ 1, 1, 9.25, 9.75 },
	};

	/* --- B: openings and depth of cover. ------------------------------------------- */

	constexpr int32 CoveredCourses = 12;

	const FWallRegion TwoCellOpening[] = { { 1, 3, 4.75, 6.25 } };
	const FWallRegion FourCellOpening[] = { { 1, 3, 3.75, 7.25 } };

	/*
	 * CASE 8, STANDS (ruled 2026-08-11): one course over case 7's four-cell hole jams into its
	 * abutments as a flat arch. The verdict rests on the LP oracle (lambda* = 324.73, DESIGN §8), not
	 * on the model agreeing.
	 */

	/*
	 * CASE 9, STANDS (ruled 2026-08-12): a ten-cell opening under 60 cm of bonded masonry is a deep
	 * beam (span/depth 3.6), not an arch. LP lambda* = 36.56. Production's worst joint reads 0.985,
	 * one retune from 1.0; SpanIsReadInTheJointNotInTheOutcome pins it.
	 */
	const FWallRegion Case9Cuts[] = { { 1, 3, 1.75, 11.25 } };

	/*
	 * CASE 10, STANDS (ruled 2026-08-12): the panel over a free-end cut cantilevers 84.4 cm off one
	 * jamb (0.055 MPa; LP lambda* = 35.82). The router used to strand it (zero cascade passes:
	 * unrouted, not overloaded). The survivor region is asserted inverted.
	 */
	const FWallRegion Case10Cuts[] = { { 1, 3, 7.75, 11.50 } };
	const FWallRegion Case10Stands[] = { { 0, 3, -1.0, 7.75 } };

	/* --- C: spanning between supports. --------------------------------------------- */

	/*
	 * CASE 11, STANDS (ruled 2026-08-08): no thrust line fits, but 60 cm of cover spans as a deep
	 * beam. M/Z = 1.995e6 uu.cm / 6150 cm3 = 0.0324 MPa, ~5% of mean bond. The BS 5977 arching gate
	 * is a serviceability rule, not a collapse predictor. Against case 12 it isolates pier width.
	 */
	const FWallRegion Case11Cuts[] = { { 0, 3, 2.75, 8.25 } };

	/*
	 * CASE 12, STANDS: case 11's span on a one-cell pier. Pier overturning ~88 N.m against 316-632
	 * N.m of mean flexural bond; only a rigid-block reading condemns it. The model reads no pier-width
	 * term (DESIGN §7 item 6); the LP separates 11 from 12 (128.12 vs 89.12).
	 */
	const FWallRegion Case12Cuts[] = { { 0, 3, 0.75, 6.25 } };

	/* --- D: corbelling and the projecting header. ----------------------------------- */

	constexpr int32 CorbelCells = 8;
	constexpr int32 CorbelFirstCourse = 6;
	constexpr double QuarterBrickStepCm = HalfCellCm * 0.5;
	constexpr double HalfBrickStepCm = HalfCellCm;

	/*
	 * CASE 14, STANDS: a bonded four-step corbel (half-cell steps) reads 0.195 at its bottom rung.
	 * Each rung is the lesser of the bonded-depth section t D^2/6 and the 179.48 cm3 bed patch. The
	 * number is asserted, since "stands" alone accepts 0.195 and 0.0001 alike.
	 */

	/** Load on a corbel step s steps below the top, brick weights. */
	constexpr double CorbelLadderForceBrickWeights(int32 StepsBelowTop)
	{
		return 1.0 + StepsBelowTop + StepsBelowTop * (StepsBelowTop + 1) / 4.0;
	}

	/** Moment on that step, brick-weight.cm. */
	constexpr double CorbelLadderMomentBrickWeightCm(int32 StepsBelowTop)
	{
		const double S = StepsBelowTop;

		const double SumOfForces = S + S * (S - 1.0) / 2.0 + (S - 1.0) * S * (S + 1.0) / 12.0;

		return (HalfCellCm * 0.5) * (S + 1.0) + HalfCellCm * SumOfForces;
	}

	/** Half-seat patch of a corbelled end brick: area cm2, modulus cm3. */
	constexpr double HalfSeatAreaSqCm = BrickDepthCm * BrickDepthCm;

	constexpr double HalfSeatModulusCm3 =
		(4.0 / 3.0) * (BrickDepthCm * 0.5) * (BrickDepthCm * 0.5) * (BrickDepthCm * 0.5);

	/** Bending on that patch less the compression closing it; never negative. */
	double HalfSeatTensionMPa(double MomentBrickWeightCm, double ForceBrickWeights)
	{
		const double BendingMPa = MomentBrickWeightCm * FullBrickWeightUu
			/ (HalfSeatModulusCm3 * ForceUnitsPerMPaSqCmHere);

		const double NormalMPa = ForceBrickWeights * FullBrickWeightUu
			/ (HalfSeatAreaSqCm * ForceUnitsPerMPaSqCmHere);

		return FMath::Max(0.0, BendingMPa - NormalMPa);
	}

	/** Bending on the deep-beam section t D^2 / 6. */
	double CompositeTensionMPa(double MomentBrickWeightCm, int32 CoursesOfDepth)
	{
		const double DepthCm = CoursesOfDepth * CoursePitchCm;

		const double ModulusCm3 = BrickDepthCm * DepthCm * DepthCm / 6.0;

		return MomentBrickWeightCm * FullBrickWeightUu / (ModulusCm3 * ForceUnitsPerMPaSqCmHere);
	}

	/** Bottom-rung reading of a k-step corbel. Min, not sum: composite action is an alternative path. */
	double CorbelBottomRungUtilisation(int32 Steps)
	{
		const double MomentBrickWeightCm = CorbelLadderMomentBrickWeightCm(Steps - 1);
		const double ForceBrickWeights = CorbelLadderForceBrickWeights(Steps - 1);

		return FMath::Min(
			HalfSeatTensionMPa(MomentBrickWeightCm, ForceBrickWeights),
			CompositeTensionMPa(MomentBrickWeightCm, Steps)) / MortarFlexuralBondMPa;
	}

	/*
	 * CASE 16, STANDS: an unloaded header half a cell out reads 0.058 on its 10.25 cm patch (bending
	 * 0.00836 MPa less self-weight compression 0.00254). Case 15 differs only by load on the tail;
	 * SuperimposedLoadIsReadInTheJointNotInTheOutcome separates them per joint.
	 */

	/* --- E: bond pattern and head-joint shear. --------------------------------------- */

	const FWallRegion Case18Cuts[] = { { 5, 5, 4.75, 5.25 } };

	/* --- F: losing the base, and the staircase void. --------------------------------- */

	/*
	 * CASE 19, STANDS (ruled 2026-08-12, the closest call): 135 cm of footing removed; nine courses
	 * cantilever over it at 0.125 MPa, ~0.21x the mean bond. LP lambda* = 12.38. The router used to
	 * drop 34 without breaking a joint (unroutability, not strength). Survivor region asserted inverted.
	 */
	const FWallRegion Case19Cuts[] = { { 0, 0, -0.50, 5.25 } };
	const FWallRegion Case19Stands[] = { { 0, 9, 7.75, 13.0 } };

	/*
	 * CASE 20: a staircase cut leaves two bricks with no bed patch (c3/4.5 and c5/2.5). Ruled LOCAL
	 * LOSS of those two, then re-ruled STANDS at slice 4 when the LP held them (see the row below).
	 * Case20Falls now names them as survivors.
	 */
	const FWallRegion Case20Cuts[] =
	{
		{ 1, 1, 0.5, 6.5 },
		{ 2, 2, 0.5, 5.5 },
		{ 3, 3, 0.5, 4.5 },
		{ 4, 4, 0.5, 3.5 },
		{ 5, 5, 0.5, 2.5 },
		{ 6, 6, 0.5, 1.5 },
	};

	const FWallRegion Case20Falls[] =
	{
		{ 3, 3, 4.40, 4.60 },
		{ 5, 5, 2.40, 2.60 },
	};

	/*
	 * --- G: openings too big for their cover. ---
	 * The only COLLAPSE rows, counter-cases to case 9 sized on the deep-beam criterion so no strength
	 * basis acquits them: sigma = (w L^2 / 8) / (t D^2 / 6), against a 0.8 MPa mean-basis ceiling.
	 * Case 9 reads 0.089 MPa; cases 21 and 22 read 1.20 and 1.16.
	 *
	 * The LP oracle stands both (lambda* 5.511 and 8.41) through a mechanism not yet identified;
	 * the discriminating ladders are deferred in CURRENT_STATE. The verdicts rest on the hand statics.
	 */

	/*
	 * CASE 21: two courses (15 cm) over an eighteen-cell opening, the cover counter-case.
	 * sigma = 461.80 N.m / 384.375 cm3 = 1.2014 MPa. Case21Falls is the seatless courses 4-5, a lower
	 * bound; Case21Stands names both jambs so a model that always answers "falls" fails the row.
	 */
	const FWallRegion Case21Cuts[] = { { 1, 3, 1.75, 19.25 } };
	const FWallRegion Case21Falls[] = { { 4, 5, 1.75, 19.25 } };
	const FWallRegion Case21Stands[] = { { 0, 3, -1.0, 1.75 }, { 0, 3, 19.25, 22.0 } };

	/*
	 * CASE 22: case 9's 60 cm cover over a 35-cell opening, varying span alone.
	 * sigma = 7,161.3 N.m / 6,150 cm3 = 1.1644 MPa. Case22Falls is a lower bound (the seatless course
	 * plus the core above); Case22Stands names both jambs. The LP solve takes 88,810 of 100,000 max
	 * pivots, so a pivot-path change could flip it to a refusal (CURRENT_STATE).
	 */
	const FWallRegion Case22Cuts[] = { { 1, 3, 1.75, 36.25 } };
	const FWallRegion Case22Falls[] = { { 4, 4, 1.75, 36.25 }, { 5, 11, 6.25, 31.75 } };
	const FWallRegion Case22Stands[] = { { 0, 3, -1.0, 1.25 }, { 0, 3, 36.75, 39.0 } };

	/** The twenty-two cases, built field by field so each value is named. */
	TArray<FWallCase> AllWallCases()
	{
		TArray<FWallCase> Cases;

		auto Add = [&Cases](
			int32 Number,
			const TCHAR* Title,
			EVerdict Verdict,
			int32 Courses,
			int32 Cells,
			TArrayView<const FWallRegion> Cuts,
			TArrayView<const FWallRegion> MustFall,
			TArrayView<const FWallRegion> MustStand,
			const TCHAR* Isolates) -> FWallCase&
		{
			FWallCase Case;
			Case.Number = Number;
			Case.Title = Title;
			Case.Verdict = Verdict;
			Case.Courses = Courses;
			Case.Cells = Cells;
			Case.Cuts = Cuts;
			Case.MustFall = MustFall;
			Case.MustStand = MustStand;
			Case.Isolates = Isolates;

			return Cases.Add_GetRef(Case);
		};

		// A: one brick out.

		Add(1, TEXT("Intact wall"), EVerdict::Stands,
			TallCourses, StandardCells, {}, {}, {}, nullptr);

		Add(2, TEXT("One brick out, mid-wall"), EVerdict::Stands,
			TallCourses, StandardCells, Case2Cuts, {}, {}, TEXT("bond, against case 18"));

		// Player-reported; ruled 2026-08-06 that a free-end deletion must not fell the wall.
		Add(3, TEXT("One brick out at the free end"), EVerdict::Stands,
			TallCourses, StandardCells, Case3Cuts, {}, {}, nullptr);

		Add(4, TEXT("One brick out of the bottom course"), EVerdict::Stands,
			TallCourses, StandardCells, Case4Cuts, {}, {}, nullptr);

		Add(5, TEXT("Alternate bricks out of one course"), EVerdict::Stands,
			TallCourses, StandardCells, Case5Cuts, {}, {}, nullptr);

		// B: openings and depth of cover.

		Add(6, TEXT("Two-brick opening, deep cover"), EVerdict::Stands,
			CoveredCourses, StandardCells, TwoCellOpening, {}, {}, nullptr);

		// The section B baseline.
		Add(7, TEXT("Four-brick opening, eight courses over"), EVerdict::Stands,
			CoveredCourses, StandardCells, FourCellOpening, {}, {},
			TEXT("span vs 9 — NOW IN THE READING; cover vs 8 and abutment vs 10 NO LONGER SEPARATE"));

		// See the CASE 8 block.
		Add(8, TEXT("Four-brick opening, one course over"), EVerdict::Stands,
			5, StandardCells, FourCellOpening, {}, {},
			TEXT("depth of cover against case 7 — NO LONGER SEPARATES, on either outcome or reading"));

		// See the CASE 9 block.
		Add(9, TEXT("Ten-brick opening, eight courses over"), EVerdict::Stands,
			CoveredCourses, 14, Case9Cuts, {}, {},
			TEXT("span against case 7 — NOW IN THE READING, not the outcome"));

		// Green since slice 3b/4: under the 200-block cap the equilibrium LP carries what the router stranded.
		Add(10, TEXT("Opening at a free end, no abutment"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case10Cuts, {}, Case10Stands,
			TEXT("abutment against case 7 — NO LONGER SEPARATES; see the CASE 10 block"));

		// C: spanning between supports. Derivations above Case11Cuts and Case12Cuts.
		Add(11, TEXT("Wall on two piers, six-brick clear span"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case11Cuts, {}, {},
			TEXT("pier width against case 12 — IN THE MARGIN, not the outcome"));

		Add(12, TEXT("The same span on a one-brick pier"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case12Cuts, {}, {},
			TEXT("pier width against case 11 — IN THE MARGIN, not the outcome"));

		// D: corbelling and the projecting header.

		{
			FWallCase& Case = Add(13, TEXT("Corbel, quarter brick per course"), EVerdict::Stands,
				10, CorbelCells, {}, {}, {},
				TEXT("corbel projection against case 14 — IN THE READING, not the outcome"));

			Case.CorbelFromCourse = CorbelFirstCourse;
			Case.CorbelStepCm = QuarterBrickStepCm;
		}

		// See the CASE 14 block.
		{
			FWallCase& Case = Add(14, TEXT("Corbel, half brick per course"), EVerdict::Stands,
				10, CorbelCells, {}, {}, {},
				TEXT("corbel projection against case 13 — IN THE READING, not the outcome"));

			Case.CorbelFromCourse = CorbelFirstCourse;
			Case.CorbelStepCm = HalfBrickStepCm;
		}

		{
			FWallCase& Case = Add(15, TEXT("Header out half a brick, six courses on top"),
				EVerdict::Stands, 10, StandardCells, {}, {}, {},
				TEXT("superimposed load against case 16 — IN THE READING, not the outcome"));

			Case.ProjectingCourse = 3;
		}

		// See the CASE 16 block.
		{
			FWallCase& Case = Add(16, TEXT("The same header at the top, nothing on it"),
				EVerdict::Stands, 10, StandardCells, {}, {}, {},
				TEXT("superimposed load against case 15 — IN THE READING, not the outcome"));

			Case.ProjectingCourse = 9;
		}

		// E: bond pattern and head-joint shear.

		{
			FWallCase& Case = Add(17, TEXT("Stack bond, intact"), EVerdict::Stands,
				10, StandardCells, {}, {}, {}, nullptr);

			Case.Bond = EBond::Stack;
		}

		{
			FWallCase& Case = Add(18, TEXT("Stack bond, one brick out"), EVerdict::Stands,
				10, StandardCells, Case18Cuts, {}, {}, TEXT("bond, against case 2"));

			Case.Bond = EBond::Stack;
		}

		// F: losing the base. Case 19 is green since slice 3b/4, like case 10.
		Add(19, TEXT("Bottom course out under half the wall"),
			EVerdict::Stands, 10, StandardCells, Case19Cuts, {}, Case19Stands, nullptr);

		/*
		 * Re-ruled STANDS at slice 4: the equilibrium LP holds the two teeth ~129x clear (lambda*
		 * 218.42). The teeth are the survivor region. A purely local mechanism would need per-region
		 * interrogation (PROMOTION_DESIGN §3.5).
		 */
		Add(20, TEXT("Staircase void"), EVerdict::Stands,
			CoveredCourses, 14, Case20Cuts, {}, Case20Falls, nullptr);

		// G: openings too big for their cover.

		/*
		 * Known red since the 2026-08-14 mean re-anchor: the model stands it at pre-cascade 0.935,
		 * because production's composite relief reads 3.1x below the hand sigma. Goes green when that
		 * gap closes, never by weakening this row. DropsToday/StrandsToday pin the wrong answer.
		 */
		{
			FWallCase& Case = Add(21, TEXT("Eighteen-brick opening, two courses over"),
				EVerdict::Collapse,
				6, 22, Case21Cuts, Case21Falls, Case21Stands,
				TEXT("thin cover made decisive — NOT a one-variable pair against case 9: cover 60 cm ")
				TEXT("to 15 AND span 1.84x, both moved"));

			Case.DropsToday = 0;
			Case.StrandsToday = 0;
		}

		// The one green Collapse row; its pre-cascade margin is pinned because it is near 1.
		Add(22, TEXT("Thirty-five-brick opening, eight courses over"), EVerdict::Collapse,
			CoveredCourses, 39, Case22Cuts, Case22Falls, Case22Stands,
			TEXT("span against case 9 — 7.77 m against 2.15, and it FALLS"))
			.PreCascadeWorstToday = 2.9133264370386338;

		return Cases;
	}

	/** Every piece the regions name, in handle order. */
	TArray<int32> PiecesInRegions(const FWall& Wall, TArrayView<const FWallRegion> Regions)
	{
		TArray<int32> Named;

		for (int32 Piece = 0; Piece < Wall.NumPieces(); ++Piece)
		{
			if (Wall.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			if (AnyRegionContains(Regions, Wall.CourseOf[Piece], Wall.CellOf[Piece]))
			{
				Named.Add(Piece);
			}
		}

		return Named;
	}

	/*
	 * The cases as playable levels. Production gets the geometry and cuts only; verdicts stay here,
	 * since an expected outcome in production would be an assertion there.
	 */

	/** A case's level name. Two digits so `wall-2` cannot read as `wall-20`. */
	FString LevelNameForCase(int32 Number)
	{
		return FString::Printf(TEXT("wall-%02d"), Number);
	}

	FString LevelMapNameForCase(int32 Number)
	{
		return FString::Printf(TEXT("Lvl_Wall%02d"), Number);
	}

	/** The verdict token a level's caption must carry; the only machine-checked part of the caption. */
	FString VerdictClaimFor(EVerdict Verdict)
	{
		return FString::Printf(TEXT("Expected: %s"), VerdictName(Verdict));
	}

	/** Required in a red row's caption, forbidden in a green one's. */
	const TCHAR* const ModelDisagreesMarker = TEXT("THE MODEL CURRENTLY DISAGREES");

	/**
	 * Whether the model produced this row's verdict, as a bool: the same three shapes the Catalogue
	 * test asserts. The caption test's known-red tripwire keeps the two in step. A Collapse needs a
	 * breaking pass, since falling with none is unroutability, not collapse.
	 */
	bool ModelAgreesWithVerdict(const FWallCase& Case, const FWall& Wall, const FWallResult& Result)
	{
		switch (Case.Verdict)
		{
		case EVerdict::Stands:
			return Result.Fallen.Num() == 0
				&& Result.CutPasses == 0
				&& Wall.Structure.NumLivePieces() == Result.PiecesLaid - Result.PiecesCut;

		case EVerdict::LocalLoss:
		{
			const TArray<int32> Named = PiecesInRegions(Wall, Case.MustFall);

			if (Named.Num() == 0 || Named.Num() != Result.Fallen.Num())
			{
				return false;
			}

			for (const int32 Piece : Named)
			{
				if (!Result.Fallen.Contains(Piece))
				{
					return false;
				}
			}

			return true;
		}

		default:
		{
			const TArray<int32> MustFall = PiecesInRegions(Wall, Case.MustFall);
			const TArray<int32> MustStand = PiecesInRegions(Wall, Case.MustStand);

			if (MustFall.Num() == 0 || Result.CutPasses == 0)
			{
				return false;
			}

			for (const int32 Piece : MustFall)
			{
				if (!Result.Fallen.Contains(Piece))
				{
					return false;
				}
			}

			for (const int32 Piece : MustStand)
			{
				if (Result.Fallen.Contains(Piece))
				{
					return false;
				}
			}

			return true;
		}
		}
	}

	/** The production spec for this case's wall. */
	DestructionWallCases::FWallSpec ProductionSpecOf(const FWallCase& Case)
	{
		DestructionWallCases::FWallSpec Spec;

		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm);
		Spec.JointThicknessCm = JointCm;
		Spec.DensityGramsPerCubicCm = ClayBrickDensityGramsPerCubicCm;
		Spec.CoursesHigh = Case.Courses;
		Spec.Cells = Case.Cells;

		Spec.Bond = Case.Bond == EBond::Stack
			? DestructionWallCases::EWallBond::Stack
			: DestructionWallCases::EWallBond::Running;

		Spec.CorbelFromCourse = Case.CorbelFromCourse;
		Spec.CorbelStepCm = Case.CorbelStepCm;
		Spec.ProjectingCourse = Case.ProjectingCourse;
		Spec.Strength = GeneralPurposeMortar;

		return Spec;
	}

	/** The same regions as production's type. */
	TArray<DestructionWallCases::FWallRegion> ProductionRegionsOf(
		TArrayView<const FWallRegion> Regions)
	{
		TArray<DestructionWallCases::FWallRegion> Out;
		Out.Reserve(Regions.Num());

		for (const FWallRegion& Region : Regions)
		{
			DestructionWallCases::FWallRegion Theirs;

			Theirs.CourseLo = Region.CourseLo;
			Theirs.CourseHi = Region.CourseHi;
			Theirs.CellLo = Region.CellLo;
			Theirs.CellHi = Region.CellHi;

			Out.Add(Theirs);
		}

		return Out;
	}

	inline FString Bits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	inline FString VectorBits(const FVector& Value)
	{
		return FString::Printf(TEXT("(%.17g, %.17g, %.17g)"), Value.X, Value.Y, Value.Z);
	}

	/**
	 * Whether a laid layout is the fixture's wall, bit for bit and in handle order (readings here
	 * name joint indices). Reports only the first disagreement.
	 */
	bool LayoutMatchesFixture(
		FAutomationTestBase& Test,
		const FString& Where,
		const DestructionLayout::FBrickLayout& Laid,
		const FWall& Fixture)
	{
		bool bSameWall = true;

		bSameWall &= Test.TestTrue(
			*FString::Printf(
				TEXT("%s: production laid %d pieces and %d joints; the fixture lays %d and %d"),
				*Where, Laid.Structure.NumPieces(), Laid.Structure.NumConnections(),
				Fixture.Structure.NumPieces(), Fixture.Structure.NumConnections()),
			Laid.Structure.NumPieces() == Fixture.Structure.NumPieces()
				&& Laid.Structure.NumConnections() == Fixture.Structure.NumConnections()
				&& Laid.Boxes.Num() == Fixture.Boxes.Num());

		int32 FirstWrongPiece = INDEX_NONE;
		FString WhyPieceWrong;

		const int32 CommonPieces = FMath::Min(Laid.Boxes.Num(), Fixture.Boxes.Num());

		for (int32 Piece = 0; Piece < CommonPieces; ++Piece)
		{
			const DestructionLayout::FPieceBox& Mine = Laid.Boxes[Piece];
			const DestructionLayout::FPieceBox& Theirs = Fixture.Boxes[Piece];

			const bool bSame = Mine.CentreCm == Theirs.CentreCm
				&& Mine.ExtentCm == Theirs.ExtentCm
				&& Laid.Structure.GetPiece(Piece).MassKg
					== Fixture.Structure.GetPiece(Piece).MassKg
				&& Laid.Structure.GetPiece(Piece).bIsGrounded
					== Fixture.Structure.GetPiece(Piece).bIsGrounded;

			if (!bSame)
			{
				FirstWrongPiece = Piece;

				WhyPieceWrong = FString::Printf(
					TEXT("production has it at %s, half-extent %s, %s kg, %s; the fixture has it at ")
					TEXT("%s, half-extent %s, %s kg, %s"),
					*VectorBits(Mine.CentreCm), *VectorBits(Mine.ExtentCm),
					*Bits(Laid.Structure.GetPiece(Piece).MassKg),
					Laid.Structure.GetPiece(Piece).bIsGrounded ? TEXT("grounded") : TEXT("free"),
					*VectorBits(Theirs.CentreCm), *VectorBits(Theirs.ExtentCm),
					*Bits(Fixture.Structure.GetPiece(Piece).MassKg),
					Fixture.Structure.GetPiece(Piece).bIsGrounded ? TEXT("grounded") : TEXT("free"));

				break;
			}
		}

		bSameWall &= Test.TestTrue(
			*FString::Printf(
				TEXT("%s: every brick must be BIT-IDENTICAL to the fixture's and carry the same ")
				TEXT("handle — piece %d is the first that is not: %s"),
				*Where, FirstWrongPiece,
				FirstWrongPiece == INDEX_NONE ? TEXT("none is") : *WhyPieceWrong),
			FirstWrongPiece == INDEX_NONE);

		int32 FirstWrongJoint = INDEX_NONE;
		FString WhyJointWrong;

		const int32 CommonJoints =
			FMath::Min(Laid.Structure.NumConnections(), Fixture.Structure.NumConnections());

		for (int32 Joint = 0; Joint < CommonJoints; ++Joint)
		{
			const FConnection& Mine = Laid.Structure.GetConnection(Joint);
			const FConnection& Theirs = Fixture.Structure.GetConnection(Joint);

			const bool bSame = Mine.PieceA == Theirs.PieceA
				&& Mine.PieceB == Theirs.PieceB
				&& Mine.InterfaceNormal == Theirs.InterfaceNormal
				&& Mine.InterfaceAreaSqCm == Theirs.InterfaceAreaSqCm
				&& Mine.InterfaceCentreCm == Theirs.InterfaceCentreCm
				&& Mine.InterfaceHalfExtentCm == Theirs.InterfaceHalfExtentCm;

			if (!bSame)
			{
				FirstWrongJoint = Joint;

				WhyJointWrong = FString::Printf(
					TEXT("production joins %d-%d, normal %s, %s cm2, centred %s, half-extent %s; the ")
					TEXT("fixture joins %d-%d, normal %s, %s cm2, centred %s, half-extent %s"),
					Mine.PieceA, Mine.PieceB, *VectorBits(Mine.InterfaceNormal),
					*Bits(Mine.InterfaceAreaSqCm), *VectorBits(Mine.InterfaceCentreCm),
					*VectorBits(Mine.InterfaceHalfExtentCm),
					Theirs.PieceA, Theirs.PieceB, *VectorBits(Theirs.InterfaceNormal),
					*Bits(Theirs.InterfaceAreaSqCm), *VectorBits(Theirs.InterfaceCentreCm),
					*VectorBits(Theirs.InterfaceHalfExtentCm));

				break;
			}
		}

		bSameWall &= Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the connection set must be the fixture's, IN ORDER — joint %d is the first ")
				TEXT("that is not: %s"),
				*Where, FirstWrongJoint,
				FirstWrongJoint == INDEX_NONE ? TEXT("none is") : *WhyJointWrong),
			FirstWrongJoint == INDEX_NONE);

		return bSameWall;
	}
}

/**
 * Every catalogue case against its own verdict, asserted on outcome. A failure names the case, its
 * isolated variable, and the fallen set in (course, cell) terms.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceCatalogueTest,
	"DestructionGame.Acceptance.Wall.Catalogue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace WallAcceptanceTestSupport;

	// A profile retune turns these red rather than silently moving every case.
	TestEqual(TEXT("FIXTURE: general purpose mortar's cohesion is EN 1996-1-1 Table 3.4's f_vk0"),
		GeneralPurposeMortar.ShearCohesionMPa, MortarShearCohesionMPa);

	TestEqual(TEXT("FIXTURE: general purpose mortar's flexural bond is Table 3.2's f_xk1"),
		GeneralPurposeMortar.TensileStrengthMPa, MortarFlexuralBondMPa);

	TestEqual(TEXT("FIXTURE: the fixture's brick density is ClayBrick's published one"),
		ClayBrick.DensityGramsPerCubicCm, ClayBrickDensityGramsPerCubicCm);

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty-two cases"), Cases.Num(), 22);

	for (const FWallCase& Case : Cases)
	{
		const FSolvedWall& Solved = RunWallCase(*this, Case);

		const FWall& Wall = Solved.Wall;
		const FWallResult& Result = Solved.Result;

		ReportWallCase(*this, Case, Wall, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		const FString Where = FString::Printf(
			TEXT("case %d (%s)%s%s"),
			Case.Number, Case.Title,
			Case.Isolates == nullptr ? TEXT("") : TEXT(" [isolates "),
			Case.Isolates == nullptr ? TEXT("") : *FString::Printf(TEXT("%s]"), Case.Isolates));

		switch (Case.Verdict)
		{
		case EVerdict::Stands:
		{
			// Both halves: a wall can sever every joint and still have nothing fall.
			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means nothing left the structure; %d piece(s) did, %s"),
					*Where, Result.Fallen.Num(), *DescribePieces(Wall, Result.Fallen)),
				Result.Fallen.Num(), 0);

			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means no joint over the cut gave; the cascade ran %d breaking ")
					TEXT("pass(es)"),
					*Where, Result.CutPasses),
				Result.CutPasses, 0);

			TestEqual(
				*FString::Printf(
					TEXT("%s: STANDS means the piece count is what was laid less what was cut; ")
					TEXT("laid %d, cut %d, live %d"),
					*Where, Result.PiecesLaid, Result.PiecesCut, Wall.Structure.NumLivePieces()),
				Wall.Structure.NumLivePieces(), Result.PiecesLaid - Result.PiecesCut);

			// On a red row, the survivor region pins which bricks drop, not just how many.
			if (Case.MustStand.Num() > 0)
			{
				const TArray<int32> Survivors = PiecesInRegions(Wall, Case.MustStand);

				TestTrue(
					*FString::Printf(
						TEXT("%s: FIXTURE the named survivor region must name at least one brick, or ")
						TEXT("the claim below is vacuous"),
						*Where),
					Survivors.Num() > 0);

				TArray<int32> WronglyFallen;

				for (const int32 Piece : Survivors)
				{
					if (Result.Fallen.Contains(Piece))
					{
						WronglyFallen.Add(Piece);
					}
				}

				TestEqual(
					*FString::Printf(
						TEXT("%s: STANDS means the named masonry keeps its footing, and on a row the ")
						TEXT("model gets wrong this is what pins WHICH bricks it drops rather than how ")
						TEXT("many; %d of %d named survivor(s) came down, %s"),
						*Where, WronglyFallen.Num(), Survivors.Num(),
						*DescribePieces(Wall, WronglyFallen)),
					WronglyFallen.Num(), 0);
			}

			// CutPasses is structurally zero on a no-cut row, so check the intact cascade instead.
			if (Case.Cuts.Num() == 0)
			{
				TestEqual(
					*FString::Printf(
						TEXT("%s: STANDS means no joint gave as the wall was BUILT either — this row ")
						TEXT("cuts nothing, so the intact cascade is the whole case and it ran %d ")
						TEXT("breaking pass(es)"),
						*Where, Result.IntactPasses),
					Result.IntactPasses, 0);
			}

			break;
		}

		case EVerdict::LocalLoss:
		{
			// Identity, not count: the same number falling elsewhere is wrong.
			const TArray<int32> Named = PiecesInRegions(Wall, Case.MustFall);

			TestTrue(
				*FString::Printf(
					TEXT("%s: FIXTURE the named local-loss set must name at least one brick"),
					*Where),
				Named.Num() > 0);

			bool bExact = Named.Num() == Result.Fallen.Num();

			for (const int32 Piece : Named)
			{
				bExact = bExact && Result.Fallen.Contains(Piece);
			}

			TestTrue(
				*FString::Printf(
					TEXT("%s: LOCAL LOSS means exactly %s came down and nothing else did; what ")
					TEXT("came down was %s"),
					*Where, *DescribePieces(Wall, Named), *DescribePieces(Wall, Result.Fallen)),
				bExact);

			break;
		}

		default:
		{
			// Two-sided, so a model that always answers "falls" fails.
			const TArray<int32> MustFall = PiecesInRegions(Wall, Case.MustFall);
			const TArray<int32> MustStand = PiecesInRegions(Wall, Case.MustStand);

			TestTrue(
				*FString::Printf(
					TEXT("%s: FIXTURE the named collapse region must name at least one brick"),
					*Where),
				MustFall.Num() > 0);

			TArray<int32> WronglyStanding;

			for (const int32 Piece : MustFall)
			{
				if (!Result.Fallen.Contains(Piece))
				{
					WronglyStanding.Add(Piece);
				}
			}

			TestEqual(
				*FString::Printf(
					TEXT("%s: COLLAPSE means every brick of %s came down; %d of them did not, %s"),
					*Where, *DescribePieces(Wall, MustFall),
					WronglyStanding.Num(), *DescribePieces(Wall, WronglyStanding)),
				WronglyStanding.Num(), 0);

			TArray<int32> WronglyFallen;

			for (const int32 Piece : MustStand)
			{
				if (Result.Fallen.Contains(Piece))
				{
					WronglyFallen.Add(Piece);
				}
			}

			TestEqual(
				*FString::Printf(
					TEXT("%s: COLLAPSE must not take the masonry beside it; %d of %d named ")
					TEXT("survivor(s) came down, %s"),
					*Where, WronglyFallen.Num(), MustStand.Num(),
					*DescribePieces(Wall, WronglyFallen)),
				WronglyFallen.Num(), 0);

			/*
			 * A collapse must be a strength verdict: at least one joint gave. Cases 10 and 19 once fell
			 * in zero passes (unroutability), which the region checks above cannot tell apart.
			 */
			TestTrue(
				*FString::Printf(
					TEXT("%s: COLLAPSE means the masonry GAVE — at least one joint must have broken ")
					TEXT("for the pieces to come down; the cascade ran %d breaking pass(es), so ")
					TEXT("either nothing broke and this row's collapse is unroutability like cases ")
					TEXT("10 and 19 rather than strength, or the cut alone took the ground from ")
					TEXT("under %d piece(s)"),
					*Where, Result.CutPasses, MustFall.Num()),
				Result.CutPasses > 0);

			break;
		}
		}

		/*
		 * A known red pinned at its measured size, so a regression inside it still shows. A fixed row
		 * fails here on purpose; delete its DropsToday, caption marker and known-red entry together.
		 */
		if (Case.DropsToday != INDEX_NONE)
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: CHARACTERISATION of a KNOWN RED — the model drops %d piece(s) here ")
					TEXT("today against the %s the catalogue asks for, and that count is pinned so a ")
					TEXT("regression inside a known failure is visible. It dropped %d: %s. If a slice ")
					TEXT("just fixed this row, delete its DropsToday; if nothing here was meant to ")
					TEXT("change, the model's answer has moved and something else moved it."),
					*Where, Case.DropsToday, VerdictName(Case.Verdict),
					Result.Fallen.Num(), *DescribePieces(Wall, Result.Fallen)),
				Result.Fallen.Num(), Case.DropsToday);
		}

		/*
		 * The wall must have been past capacity when it went, and the margin is pinned so a drift toward
		 * 1.0 fails before the verdict flips.
		 */
		if (Case.PreCascadeWorstToday > 0.0)
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s: the cascade must have started PAST capacity for a %s to be a strength ")
					TEXT("verdict — the worst joint read %.17g before any joint was allowed to give"),
					*Where, VerdictName(Case.Verdict), Result.PreCascadeWorst),
				Result.PreCascadeWorst > 1.0);

			TestTrue(
				*FString::Printf(
					TEXT("%s: CHARACTERISATION of the MARGIN — the worst joint read %.17g of capacity ")
					TEXT("before the cascade started, pinned at %.17g. A count cannot see this move: ")
					TEXT("the same bricks come down at 1.01x as at 8x, and this row is the one thing ")
					TEXT("that fails while a verdict is still standing but has stopped being safe"),
					*Where, Result.PreCascadeWorst, Case.PreCascadeWorstToday),
				FMath::IsNearlyEqual(
					Result.PreCascadeWorst, Case.PreCascadeWorstToday, 1.0e-9));
		}
	}

	return true;
}

/**
 * Cases 7 and 9: both stand, but the wider opening changes what its jamb does. Asserts the governing
 * joint, its axis (case 7 flexural tension on the composite section, case 9 Mohr-Coulomb shear from
 * arch thrust), the measured magnitudes, and two strength-free span terms: only the wide jamb takes
 * in-plane thrust, and its reaction is larger. Both walls break nothing, so neither reading comes
 * off a structure the router failed on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceSpanTest,
	"DestructionGame.Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceSpanTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	// Slack against re-association of a few doubles.
	constexpr double UtilisationTolerance = 1.0e-9;

	// Naming an axis is only as good as the strengths it divides by, so pin them.
	TestEqual(TEXT("FIXTURE: general purpose mortar's friction coefficient is the profile's mu"),
		DestructionProfiles::GeneralPurposeMortar.FrictionCoefficient, MortarFrictionCoefficient);

	TestEqual(TEXT("FIXTURE: general purpose mortar's shear truncation is the profile's"),
		DestructionProfiles::GeneralPurposeMortar.MaxShearStrengthMPa, MortarMaxShearStrengthMPa);

	TestEqual(TEXT("FIXTURE: general purpose mortar's compressive strength is the profile's"),
		DestructionProfiles::GeneralPurposeMortar.CompressiveStrengthMPa,
		MortarCompressiveStrengthMPa);

	/* --- the two walls --------------------------------------------------------------------- */

	const TArray<FWallCase> Cases = AllWallCases();

	auto RunNumbered = [this, &Cases](int32 Number) -> const FSolvedWall*
	{
		for (const FWallCase& Case : Cases)
		{
			if (Case.Number != Number)
			{
				continue;
			}

			const FSolvedWall& Solved = RunWallCase(*this, Case);
			ReportWallCase(*this, Case, Solved.Wall, Solved.Result);

			return Solved.Result.bLaid ? &Solved : nullptr;
		}

		AddError(FString::Printf(TEXT("FIXTURE: the catalogue has no case %d"), Number));

		return nullptr;
	};

	const FSolvedWall* const Narrow = RunNumbered(7);
	const FSolvedWall* const Wide = Narrow == nullptr ? nullptr : RunNumbered(9);

	if (Wide == nullptr)
	{
		return true;
	}

	const FWall& NarrowWall = Narrow->Wall;
	const FWallResult& NarrowResult = Narrow->Result;

	const FWall& WideWall = Wide->Wall;
	const FWallResult& WideResult = Wide->Result;

	AddInfo(FString::Printf(
		TEXT("span pair: case 7 reads %.17g, case 9 reads %.17g, ratio %.17g"),
		NarrowResult.Worst, WideResult.Worst,
		NarrowResult.Worst > 0.0 ? WideResult.Worst / NarrowResult.Worst : 0.0));

	/* --- neither wall comes down, which is the ruling --------------------------------------- */

	TestEqual(
		*FString::Printf(
			TEXT("THE RULING: a four-cell opening under eight courses stands, %d piece(s) came down %s"),
			NarrowResult.Fallen.Num(), *DescribePieces(NarrowWall, NarrowResult.Fallen)),
		NarrowResult.Fallen.Num(), 0);

	TestEqual(
		*FString::Printf(
			TEXT("THE RULING: a TEN-cell opening under the same cover stands too — that is the ")
			TEXT("2026-08-12 re-ruling of case 9 — and %d piece(s) came down %s"),
			WideResult.Fallen.Num(), *DescribePieces(WideWall, WideResult.Fallen)),
		WideResult.Fallen.Num(), 0);

	// Nothing broken either, so both readings come off an intact, fully routed wall.
	TestEqual(
		*FString::Printf(
			TEXT("BREAK-NOTHING: case 7's reading must come off a wall with every joint intact, or ")
			TEXT("the magnitude below is measured on a structure the cut already changed; the cascade ")
			TEXT("ran %d breaking pass(es)"),
			NarrowResult.CutPasses),
		NarrowResult.CutPasses, 0);

	TestEqual(
		*FString::Printf(
			TEXT("BREAK-NOTHING: and so must case 9's — this is the half of the header's ")
			TEXT("distinguishing count that separates this pair from 7-vs-10; the cascade ran %d ")
			TEXT("breaking pass(es)"),
			WideResult.CutPasses),
		WideResult.CutPasses, 0);

	/* --- and each reading is taken at the jamb beside its own opening ------------------------ */

	auto DescribeWorst = [](const FWall& Wall, const FWallResult& Result)
	{
		return Result.WorstPieceA == INDEX_NONE
			? FString(TEXT("no joint at all"))
			: FString::Printf(
				TEXT("c%d/%g-c%d/%g"),
				Wall.CourseOf[Result.WorstPieceA], Wall.CellOf[Result.WorstPieceA],
				Wall.CourseOf[Result.WorstPieceB], Wall.CellOf[Result.WorstPieceB]);
	};

	auto WorstJointIs = [](
		const FWall& Wall, const FWallResult& Result,
		int32 CourseA, double CellA, int32 CourseB, double CellB)
	{
		return Result.WorstPieceA != INDEX_NONE
			&& Wall.CourseOf[Result.WorstPieceA] == CourseA
			&& Wall.CourseOf[Result.WorstPieceB] == CourseB
			&& FMath::IsNearlyEqual(Wall.CellOf[Result.WorstPieceA], CellA, 1.0e-9)
			&& FMath::IsNearlyEqual(Wall.CellOf[Result.WorstPieceB], CellB, 1.0e-9);
	};

	TestTrue(
		*FString::Printf(
			TEXT("THE GOVERNING JOINT of case 7 must be c2/8-c3/7.5, the bed joint in the RIGHT JAMB ")
			TEXT("one course below the head of the opening, or the magnitude below is right about the ")
			TEXT("wrong joint and the pair is no longer comparing one mechanism; it is %s"),
			*DescribeWorst(NarrowWall, NarrowResult)),
		WorstJointIs(NarrowWall, NarrowResult, 2, 8.0, 3, 7.5));

	TestTrue(
		*FString::Printf(
			TEXT("THE GOVERNING JOINT of case 9 must be c3/11.5-c4/11, the bed joint AT the head where ")
			TEXT("the first course of cover lands on the RIGHT JAMB — one course higher than case 7's, ")
			TEXT("which the pair's header argues about at length; it is %s"),
			*DescribeWorst(WideWall, WideResult)),
		WorstJointIs(WideWall, WideResult, 3, 11.5, 4, 11.0));

	/*
	 * --- the magnitudes, pinned as measured ---
	 * Outputs of the whole-wall composite walk, not re-derivable here. Case 7 is tension (the old pin
	 * / 7 after the mean re-anchor); case 9's tension clamps to zero at the kern and shear governs.
	 */
	constexpr double NarrowWorst = 0.26944083288948872 / 7.0;
	constexpr double WideWorst = 0.26329211195559277;

	TestTrue(
		*FString::Printf(
			TEXT("CHARACTERISATION: case 7's jamb reads %.17g and the fixture reads %.17g. This is a ")
			TEXT("measured number, not a derived one — the whole-wall composite walk produces it — and ")
			TEXT("it is pinned because a 2x drift in it moves no verdict and would otherwise pass the ")
			TEXT("suite in silence"),
			NarrowWorst, NarrowResult.Worst),
		FMath::IsNearlyEqual(NarrowResult.Worst, NarrowWorst, UtilisationTolerance));

	TestTrue(
		*FString::Printf(
			TEXT("CHARACTERISATION: case 9's jamb reads %.17g and the fixture reads %.17g"),
			WideWorst, WideResult.Worst),
		FMath::IsNearlyEqual(WideResult.Worst, WideWorst, UtilisationTolerance));

	// Watched apart from the pin so the log names the event that matters.
	TestTrue(
		*FString::Printf(
			TEXT("THE FLAP WATCH: case 9's jamb reads %.17g and must stay UNDER 1.0. VACUOUS at the ")
			TEXT("mean basis (re-anchor 2026-08-13) — the reading sits ~3.8x clear and is the jamb ")
			TEXT("SLIDING under the arch thrust, not the flexural tension the characteristic-era ")
			TEXT("0.985 knife-edge was about — and kept because the day it crosses, this wall drops ")
			TEXT("its head, case 9's 2026-08-12 STANDS ruling becomes a catalogue red and the oracle ")
			TEXT("sweep's pinned AgreeStands relation flips. That is a re-derivation, not a retune"),
			WideResult.Worst),
		WideResult.Worst < 1.0);

	/* --- which AXIS each reading is, derived from the joint's own force and moment ------------- */

	/** Find the joint carrying the worst reading; its loads come straight off production's accessors. */
	auto JointLoad = [](const FWall& Wall, const FWallResult& Result, int32& OutJoint)
	{
		OutJoint = INDEX_NONE;

		for (int32 Joint = 0; Joint < Wall.Structure.NumConnections(); ++Joint)
		{
			const FConnection& Connection = Wall.Structure.GetConnection(Joint);

			if (Connection.PieceA == Result.WorstPieceA && Connection.PieceB == Result.WorstPieceB)
			{
				OutJoint = Joint;
				break;
			}
		}
	};

	int32 NarrowJoint = INDEX_NONE;
	int32 WideJoint = INDEX_NONE;

	JointLoad(NarrowWall, NarrowResult, NarrowJoint);
	JointLoad(WideWall, WideResult, WideJoint);

	if (NarrowJoint == INDEX_NONE || WideJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: could not find the connection carrying a wall's worst reading"));

		return true;
	}

	const FVector NarrowForceUu = NarrowWall.Structure.GetConnectionForce(NarrowJoint);
	const FVector NarrowMomentUuCm = NarrowWall.Structure.GetConnectionMoment(NarrowJoint);
	const double NarrowDepthCm = NarrowWall.Structure.GetConnectionCompositeDepthCm(NarrowJoint);

	const FVector WideForceUu = WideWall.Structure.GetConnectionForce(WideJoint);
	const double WideAreaSqCm = WideWall.Structure.GetConnection(WideJoint).InterfaceAreaSqCm;

	// Case 7: |M_y| over the composite section t.D^2/6, against mean flexural bond.
	const double NarrowCompositeModulusCm3 =
		BrickDepthCm * NarrowDepthCm * NarrowDepthCm / 6.0;

	const double NarrowTensionUtilisation =
		FMath::Abs(NarrowMomentUuCm.Y)
		/ (NarrowCompositeModulusCm3 * ForceUnitsPerMPaSqCmHere) / MortarFlexuralBondMPa;

	TestTrue(
		*FString::Printf(
			TEXT("THE AXIS: case 7's jamb must be governed by FLEXURAL TENSION on the composite ")
			TEXT("section — |M_y| %.8g uu.cm over t.D^2/6 = %.8g cm3 (D = %.8g cm) against f_x1 ")
			TEXT("%.8g MPa is %.17g, and production reads %.17g. If these part company the reading ")
			TEXT("has changed axis and every sentence about this pair has to be re-derived"),
			FMath::Abs(NarrowMomentUuCm.Y), NarrowCompositeModulusCm3, NarrowDepthCm,
			MortarFlexuralBondMPa, NarrowTensionUtilisation, NarrowResult.Worst),
		FMath::IsNearlyEqual(NarrowResult.Worst, NarrowTensionUtilisation, UtilisationTolerance));

	// Case 9: in-plane shear stress against cohesion + friction x compression, on the joint's own patch.
	const double WideNormalStressMPa =
		FMath::Abs(WideForceUu.Z) / (WideAreaSqCm * ForceUnitsPerMPaSqCmHere);

	const double WideShearStressMPa =
		FVector(WideForceUu.X, WideForceUu.Y, 0.0).Size()
		/ (WideAreaSqCm * ForceUnitsPerMPaSqCmHere);

	const double WideShearCapacityMPa = FMath::Min(
		MortarShearCohesionMPa + MortarFrictionCoefficient * WideNormalStressMPa,
		MortarMaxShearStrengthMPa);

	const double WideShearUtilisation = WideShearStressMPa / WideShearCapacityMPa;

	TestTrue(
		*FString::Printf(
			TEXT("THE AXIS: case 9's jamb must be governed by MOHR-COULOMB SHEAR — %.8g MPa of ")
			TEXT("in-plane stress against a capacity of f_v0 %.8g + mu %.8g x %.8g MPa of ")
			TEXT("compression = %.8g MPa is %.17g, and production reads %.17g. The squeezed edge's ")
			TEXT("COMPRESSION, which DESIGN §6 and CURRENT_STATE both named for this reading before ")
			TEXT("2026-08-14, is %.8g — thirteen times under it"),
			WideShearStressMPa, MortarShearCohesionMPa, MortarFrictionCoefficient,
			WideNormalStressMPa, WideShearCapacityMPa, WideShearUtilisation, WideResult.Worst,
			2.0 * WideNormalStressMPa / MortarCompressiveStrengthMPa),
		FMath::IsNearlyEqual(WideResult.Worst, WideShearUtilisation, UtilisationTolerance));

	/* --- the pair's own claim: the span is read in the joint ---------------------------------- */

	// Clear span: n cell pitches between brick centres, less one brick; mean of even and odd courses.
	auto MeanClearSpanCm = [](double EvenCellGap, double OddCellGap)
	{
		return 0.5
			* ((EvenCellGap * CellPitchCm - BrickLengthCm) + (OddCellGap * CellPitchCm - BrickLengthCm));
	};

	/* Case 7 cuts cells 4..7 of the even courses and 4.5..6.5 of the odd ones. */
	const double NarrowSpanCm = MeanClearSpanCm(8.0 - 3.0, 7.5 - 3.5);

	/* Case 9 cuts cells 2..11 of the even courses and 2.5..10.5 of the odd ones. */
	const double WideSpanCm = MeanClearSpanCm(12.0 - 1.0, 11.5 - 1.5);

	const double SpanRatio = WideSpanCm / NarrowSpanCm;

	// Strength-free span term: only the wide jamb takes in-plane thrust.
	const double NarrowThrustUu = FVector(NarrowForceUu.X, NarrowForceUu.Y, 0.0).Size();
	const double WideThrustUu = FVector(WideForceUu.X, WideForceUu.Y, 0.0).Size();

	TestTrue(
		*FString::Printf(
			TEXT("SPAN: the wider opening must push its jamb SIDEWAYS where the narrow one does not ")
			TEXT("— %.8g cm of clear opening delivers %.8g uu of in-plane thrust into its jamb and ")
			TEXT("%.8g cm delivers %.8g uu. This is the pair's span discrimination since the ")
			TEXT("2026-08-14 re-anchor retired the reading-ratio floor, and it carries no strength ")
			TEXT("at all"),
			NarrowSpanCm, NarrowThrustUu, WideSpanCm, WideThrustUu),
		WideThrustUu > 0.0 && NarrowThrustUu <= 0.0);

	// The jamb's normal reaction grows with span too.
	const double NarrowReactionUu = FMath::Abs(NarrowForceUu.Z);
	const double WideReactionUu = FMath::Abs(WideForceUu.Z);

	const double ReactionRatio =
		NarrowReactionUu > 0.0 ? WideReactionUu / NarrowReactionUu : 0.0;

	TestTrue(
		*FString::Printf(
			TEXT("SPAN: and a wider opening must deliver a BIGGER reaction into its jamb — %.8g uu ")
			TEXT("at %.8g cm of clear opening against %.8g uu at %.8g cm. A model with no span term ")
			TEXT("reads them the same"),
			NarrowReactionUu, NarrowSpanCm, WideReactionUu, WideSpanCm),
		NarrowReactionUu > 0.0 && WideReactionUu > NarrowReactionUu);

	/*
	 * Pinned as a finding, not physics: w.L/2 wants at least linear growth, the model gives 1.90x for
	 * 2.69x of span. Fires when equilibrium promotion (DESIGN §7 step 4) fixes the span term.
	 */
	constexpr double PinnedReactionRatio = 1.9045707413720012;

	TestTrue(
		*FString::Printf(
			TEXT("CHARACTERISATION: the reaction ratio is %.17g against a span ratio of %.17g — ")
			TEXT("SUBLINEAR, where a deep beam's w.L/2 requires at least linear. The pin is %.17g. ")
			TEXT("This row PASSES and records a known-weak span term; it is not an endorsement"),
			ReactionRatio, SpanRatio, PinnedReactionRatio),
		FMath::IsNearlyEqual(ReactionRatio, PinnedReactionRatio, UtilisationTolerance)
			&& ReactionRatio < SpanRatio);

	return true;
}

/**
 * Cases 13 and 14: both corbels stand, but doubling the step raises the joint reading. The expected
 * value is derived from the grid (CorbelBottomRungUtilisation), cross-checked against ARCHING_DESIGN
 * and Core.Structure.AStaircaseVoidCondemnsTheCorbel, and the governing joint is asserted first.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceCorbelProjectionTest,
	"DestructionGame.Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceCorbelProjectionTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	// Slack against re-association of a few doubles.
	constexpr double UtilisationTolerance = 1.0e-9;

	// Cross-check figures are the characteristic-basis values / 7 (f_x1 0.10 -> 0.70; pure tension).
	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: five steps should reproduce ARCHING_DESIGN's published 0.219 / 7, the ")
			TEXT("closed form gives %.8g"),
			CorbelBottomRungUtilisation(5)),
		FMath::IsNearlyEqual(CorbelBottomRungUtilisation(5), 0.219 / 7.0, 0.02 * (0.219 / 7.0)));

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: eleven steps should reproduce the staircase anchor 0.0527187818..., ")
			TEXT("the closed form gives %.17g"),
			CorbelBottomRungUtilisation(11)),
		FMath::IsNearlyEqual(
			CorbelBottomRungUtilisation(11), 0.36903147272727271 / 7.0, UtilisationTolerance));

	/* --- the two walls -------------------------------------------------------------------- */

	const TArray<FWallCase> Cases = AllWallCases();

	auto RunNumbered = [this, &Cases](int32 Number) -> const FSolvedWall*
	{
		for (const FWallCase& Case : Cases)
		{
			if (Case.Number != Number)
			{
				continue;
			}

			const FSolvedWall& Solved = RunWallCase(*this, Case);
			ReportWallCase(*this, Case, Solved.Wall, Solved.Result);

			return Solved.Result.bLaid ? &Solved : nullptr;
		}

		AddError(FString::Printf(TEXT("FIXTURE: the catalogue has no case %d"), Number));

		return nullptr;
	};

	const FSolvedWall* const Quarter = RunNumbered(13);
	const FSolvedWall* const Half = Quarter == nullptr ? nullptr : RunNumbered(14);

	if (Half == nullptr)
	{
		return true;
	}

	const FWall& QuarterWall = Quarter->Wall;
	const FWallResult& QuarterResult = Quarter->Result;

	const FWall& HalfWall = Half->Wall;
	const FWallResult& HalfResult = Half->Result;

	/* --- neither corbel comes down, which is the ruling ------------------------------------- */

	TestEqual(
		*FString::Printf(
			TEXT("THE RULING: a quarter-brick corbel stands, %d piece(s) came down %s"),
			QuarterResult.Fallen.Num(), *DescribePieces(QuarterWall, QuarterResult.Fallen)),
		QuarterResult.Fallen.Num(), 0);

	TestEqual(
		*FString::Printf(
			TEXT("THE RULING: a HALF-brick corbel stands too — that is the 2026-08-07 correction to ")
			TEXT("case 14 — and %d piece(s) came down %s"),
			HalfResult.Fallen.Num(), *DescribePieces(HalfWall, HalfResult.Fallen)),
		HalfResult.Fallen.Num(), 0);

	/* --- and the reading is the four-step ladder on the four courses standing over it -------- */

	const bool bWorstIsTheBottomRung =
		HalfResult.WorstPieceA != INDEX_NONE
		&& HalfWall.CourseOf[HalfResult.WorstPieceA] == CorbelFirstCourse - 1
		&& HalfWall.CourseOf[HalfResult.WorstPieceB] == CorbelFirstCourse
		&& FMath::IsNearlyEqual(HalfWall.CellOf[HalfResult.WorstPieceB], 7.5, 1.0e-9);

	TestTrue(
		*FString::Printf(
			TEXT("THE GOVERNING JOINT must be the bed joint under the lowest corbelled course, c%d/7.25 ")
			TEXT("carrying c%d/7.5, or the magnitude below is right about the wrong joint; it is %s"),
			CorbelFirstCourse - 1, CorbelFirstCourse,
			HalfResult.WorstPieceA == INDEX_NONE
				? TEXT("no joint at all")
				: *FString::Printf(
					TEXT("c%d/%g-c%d/%g"),
					HalfWall.CourseOf[HalfResult.WorstPieceA], HalfWall.CellOf[HalfResult.WorstPieceA],
					HalfWall.CourseOf[HalfResult.WorstPieceB], HalfWall.CellOf[HalfResult.WorstPieceB])),
		bWorstIsTheBottomRung);

	const double ExpectedHalf = CorbelBottomRungUtilisation(4);

	TestTrue(
		*FString::Printf(
			TEXT("A FOUR-STEP HALF-BRICK CORBEL reads its bottom rung at %.17g of f_xk1 — 112.5 ")
			TEXT("brick-weight-cm over the 1537.5 cm3 of four courses acting together — and the wall ")
			TEXT("reads %.17g"),
			ExpectedHalf, HalfResult.Worst),
		FMath::IsNearlyEqual(HalfResult.Worst, ExpectedHalf, UtilisationTolerance));

	// No `Worst < 1.0` check: Worst skips given joints, so it is tautological after the cascade.

	/* --- the pair's own claim: projection is still measured --------------------------------- */

	TestTrue(
		*FString::Printf(
			TEXT("CORBEL STEP: doubling the projection per course must read HARDER on the joint — a ")
			TEXT("quarter brick reads %.8g and a half brick reads %.8g, a factor of %.4g. A model ")
			TEXT("with no projection term reads them the same."),
			QuarterResult.Worst, HalfResult.Worst,
			QuarterResult.Worst > 0.0 ? HalfResult.Worst / QuarterResult.Worst : 0.0),
		HalfResult.Worst > QuarterResult.Worst);

	// At least twice, so a last-bit difference cannot pass. Measured 2.8x.
	TestTrue(
		*FString::Printf(
			TEXT("CORBEL STEP: and by a real margin, not the last bit — %.8g against %.8g is a ")
			TEXT("factor of %.4g and must be at least 2"),
			QuarterResult.Worst, HalfResult.Worst,
			QuarterResult.Worst > 0.0 ? HalfResult.Worst / QuarterResult.Worst : 0.0),
		QuarterResult.Worst > 0.0 && HalfResult.Worst >= 2.0 * QuarterResult.Worst);

	return true;
}

/**
 * Cases 15 and 16: the same header at course 3 (six courses on its tail) and course 9 (nothing on
 * it). Both stand, but the load on the tail relieves the header's bed joint. Case 16's reading is
 * derived from the grid and its governing joint asserted; case 15 is bounded above instead.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceSuperimposedLoadTest,
	"DestructionGame.Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceSuperimposedLoadTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	// Slack against re-association of a few doubles.
	constexpr double UtilisationTolerance = 1.0e-9;

	/* --- the derivation, worked forward from the grid ---------------------------------------- */

	// Half a cell overhangs, so 10.25 cm of the brick bears.
	constexpr double HeaderSeatLengthCm = BrickLengthCm - HalfCellCm;

	// Centre of mass is half the overhang outboard of the seat edge.
	constexpr double HeaderArmCm = (BrickLengthCm - HeaderSeatLengthCm) * 0.5;

	constexpr double HeaderSeatAreaSqCm = HeaderSeatLengthCm * BrickDepthCm;

	constexpr double HeaderSeatModulusCm3 =
		BrickDepthCm * HeaderSeatLengthCm * HeaderSeatLengthCm / 6.0;

	const double BendingMPa = FullBrickWeightUu * HeaderArmCm
		/ (HeaderSeatModulusCm3 * ForceUnitsPerMPaSqCmHere);

	const double OwnWeightMPa = FullBrickWeightUu
		/ (HeaderSeatAreaSqCm * ForceUnitsPerMPaSqCmHere);

	// Self-weight closes the joint the bending opens, so they subtract.
	const double ExpectedUnloaded =
		FMath::Max(0.0, BendingMPa - OwnWeightMPa) / MortarFlexuralBondMPa;

	// Cross-checks are the characteristic-basis figures / 7 (mean re-anchor 2026-08-13).
	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: WALL_CASES.html's characteristic-era 0.058204 / 7 for case 16; the ")
			TEXT("derivation gives %.8g"),
			ExpectedUnloaded),
		FMath::IsNearlyEqual(ExpectedUnloaded, 0.058204 / 7.0, 1.0e-6));

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: the same arithmetic on a CUT half-seated brick is the waist anchor ")
			TEXT("0.0083148340..., the derivation gives %.17g"),
			ExpectedUnloaded),
		FMath::IsNearlyEqual(ExpectedUnloaded, 0.058203838191552663 / 7.0, UtilisationTolerance));

	/* --- the two walls ---------------------------------------------------------------------- */

	const TArray<FWallCase> Cases = AllWallCases();

	auto RunNumbered = [this, &Cases](int32 Number) -> const FSolvedWall*
	{
		for (const FWallCase& Case : Cases)
		{
			if (Case.Number != Number)
			{
				continue;
			}

			const FSolvedWall& Solved = RunWallCase(*this, Case);
			ReportWallCase(*this, Case, Solved.Wall, Solved.Result);

			return Solved.Result.bLaid ? &Solved : nullptr;
		}

		AddError(FString::Printf(TEXT("FIXTURE: the catalogue has no case %d"), Number));

		return nullptr;
	};

	const FSolvedWall* const Loaded = RunNumbered(15);
	const FSolvedWall* const Bare = Loaded == nullptr ? nullptr : RunNumbered(16);

	if (Bare == nullptr)
	{
		return true;
	}

	const FWall& LoadedWall = Loaded->Wall;
	const FWallResult& LoadedResult = Loaded->Result;

	const FWall& BareWall = Bare->Wall;
	const FWallResult& BareResult = Bare->Result;

	/* --- neither header comes down, which is the revision ------------------------------------ */

	TestEqual(
		*FString::Printf(
			TEXT("THE CATALOGUE: a header with six courses on its tail stands, %d piece(s) came ")
			TEXT("down %s"),
			LoadedResult.Fallen.Num(), *DescribePieces(LoadedWall, LoadedResult.Fallen)),
		LoadedResult.Fallen.Num(), 0);

	TestEqual(
		*FString::Printf(
			TEXT("THE CATALOGUE: a header with NOTHING on its tail stands too — that is the ")
			TEXT("2026-08-07 revision to case 16 — and %d piece(s) came down %s"),
			BareResult.Fallen.Num(), *DescribePieces(BareWall, BareResult.Fallen)),
		BareResult.Fallen.Num(), 0);

	/* --- and the reading is the header's own bed joint ---------------------------------------- */

	// Header and seat centres: half a brick in from their right faces.
	const double HeaderCentreCm =
		FlushRightFaceCm(StandardCells) + HalfCellCm - BrickLengthCm * 0.5;

	const double SeatCentreCm = FlushRightFaceCm(StandardCells) - BrickLengthCm * 0.5;

	const double HeaderCell = HeaderCentreCm / CellPitchCm;
	const double SeatCell = SeatCentreCm / CellPitchCm;

	constexpr int32 BareHeaderCourse = 9;

	const bool bWorstIsTheHeaderSeat =
		BareResult.WorstPieceA != INDEX_NONE
		&& BareWall.CourseOf[BareResult.WorstPieceA] == BareHeaderCourse - 1
		&& BareWall.CourseOf[BareResult.WorstPieceB] == BareHeaderCourse
		&& FMath::IsNearlyEqual(BareWall.CellOf[BareResult.WorstPieceA], SeatCell, 1.0e-9)
		&& FMath::IsNearlyEqual(BareWall.CellOf[BareResult.WorstPieceB], HeaderCell, 1.0e-9);

	TestTrue(
		*FString::Printf(
			TEXT("THE GOVERNING JOINT must be the header's own bed joint, c%d/%g carrying c%d/%g, ")
			TEXT("or the magnitude below is right about the wrong joint; it is %s"),
			BareHeaderCourse - 1, SeatCell, BareHeaderCourse, HeaderCell,
			BareResult.WorstPieceA == INDEX_NONE
				? TEXT("no joint at all")
				: *FString::Printf(
					TEXT("c%d/%g-c%d/%g"),
					BareWall.CourseOf[BareResult.WorstPieceA], BareWall.CellOf[BareResult.WorstPieceA],
					BareWall.CourseOf[BareResult.WorstPieceB], BareWall.CellOf[BareResult.WorstPieceB])),
		bWorstIsTheHeaderSeat);

	TestTrue(
		*FString::Printf(
			TEXT("A HEADER WITH NOTHING ON IT reads its bed joint at %.17g of f_xk1 — 0.0083591 MPa ")
			TEXT("of bending less 0.0025387 MPa of its own weight closing the joint — and the wall ")
			TEXT("reads %.17g"),
			ExpectedUnloaded, BareResult.Worst),
		FMath::IsNearlyEqual(BareResult.Worst, ExpectedUnloaded, UtilisationTolerance));

	/* --- the pair's own claim: superimposed load is still measured ---------------------------- */

	TestTrue(
		*FString::Printf(
			TEXT("SUPERIMPOSED LOAD: six courses standing on the header's tail must read EASIER on ")
			TEXT("the joint — loaded reads %.8g (worst at %s) and bare reads %.8g, a factor of ")
			TEXT("%.4g. A model with no superimposed-load term reads them the same, and one that ")
			TEXT("counted the extra load without its compression reads the loaded wall HIGHER."),
			LoadedResult.Worst,
			LoadedResult.WorstPieceA == INDEX_NONE
				? TEXT("no joint at all")
				: *FString::Printf(
					TEXT("c%d/%g-c%d/%g"),
					LoadedWall.CourseOf[LoadedResult.WorstPieceA],
					LoadedWall.CellOf[LoadedResult.WorstPieceA],
					LoadedWall.CourseOf[LoadedResult.WorstPieceB],
					LoadedWall.CellOf[LoadedResult.WorstPieceB]),
			BareResult.Worst,
			LoadedResult.Worst > 0.0 ? BareResult.Worst / LoadedResult.Worst : 0.0),
		BareResult.Worst > LoadedResult.Worst);

	/*
	 * At least 3x (measured 4.51x), so a last-bit difference cannot pass. Case 15's Worst is the worst
	 * joint anywhere, so bounding it also bounds the header's joint.
	 */
	constexpr double MinimumSeparation = 3.0;

	TestTrue(
		*FString::Printf(
			TEXT("SUPERIMPOSED LOAD: and by a real margin, not the last bit — %.8g against %.8g is ")
			TEXT("a factor of %.4g and must be at least %g"),
			LoadedResult.Worst, BareResult.Worst,
			LoadedResult.Worst > 0.0 ? BareResult.Worst / LoadedResult.Worst : 0.0,
			MinimumSeparation),
		LoadedResult.Worst > 0.0 && BareResult.Worst >= MinimumSeparation * LoadedResult.Worst);

	return true;
}

/**
 * Case 18: a stack-bond column hanging over a cut brick should read the same head-joint shear at any
 * height, since each course adds one brick and two head joints. Per pair: 2667.198625 uu /
 * (2 x 66.625 cm2) / 10000 / 0.9 MPa = 0.0022240556 (shear, no friction on a vertical joint).
 *
 * Known red: the model puts the whole column on its foot, reading (courses - 6) x that figure; a
 * passing characterisation pins it there. Heights 12 and 20 keep the head joint governing over the
 * bed joint beside the hole (they cross at eleven courses; margin asserted).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceStackBondHeightTest,
	"DestructionGame.Acceptance.Wall.StackBondColumnShearIsHeightIndependent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceStackBondHeightTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	constexpr double HangingColumnShearUtilisation =
		FullBrickWeightUu / (2.0 * HeadJointAreaSqCm) / ForceUnitsPerMPaSqCmHere
		/ MortarShearCohesionMPa;

	// Slack against re-association of a few doubles.
	constexpr double UtilisationTolerance = 1e-9;

	/** n courses hang n - (CutCourse + 1) bricks. */
	const int32 CutCourse = Case18Cuts[0].CourseLo;

	// Both past the eleven-course crossing; re-measure before lowering either.
	const int32 Heights[] = { 12, 20 };

	constexpr int32 HeightCount = static_cast<int32>(UE_ARRAY_COUNT(Heights));

	double Measured[HeightCount] = { 0.0, 0.0 };
	bool bRan[HeightCount] = { false, false };

	for (int32 Index = 0; Index < HeightCount; ++Index)
	{
		FWallCase Case;
		Case.Number = 18;
		Case.Title = TEXT("Stack bond, one brick out");
		Case.Verdict = EVerdict::Stands;
		Case.Courses = Heights[Index];
		Case.Cells = 12;
		Case.Bond = EBond::Stack;
		Case.Cuts = Case18Cuts;

		const FSolvedWall& Solved = RunWallCase(*this, Case);

		const FWall& Wall = Solved.Wall;
		const FWallResult& Result = Solved.Result;

		ReportWallCase(*this, Case, Wall, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		bRan[Index] = true;
		Measured[Index] = Result.Worst;

		// The head joint must govern, with margin, or this measures an ordinary bed joint.
		double WorstHead = 0.0;
		double WorstBed = 0.0;
		int32 WorstBedA = INDEX_NONE;
		int32 WorstBedB = INDEX_NONE;

		for (int32 Joint = 0; Joint < Wall.Structure.NumConnections(); ++Joint)
		{
			const FConnection& Connection = Wall.Structure.GetConnection(Joint);

			if (Connection.HasGiven())
			{
				continue;
			}

			const double Utilisation = Wall.Structure.GetConnectionUtilisation(Joint);

			// Same course at both ends: head joint.
			if (Wall.CourseOf[Connection.PieceA] == Wall.CourseOf[Connection.PieceB])
			{
				WorstHead = FMath::Max(WorstHead, Utilisation);
			}
			else if (Utilisation > WorstBed)
			{
				WorstBed = Utilisation;
				WorstBedA = Connection.PieceA;
				WorstBedB = Connection.PieceB;
			}
		}

		TestEqual(
			*FString::Printf(
				TEXT("%d courses: the wall must still stand, %d piece(s) came down %s"),
				Heights[Index], Result.Fallen.Num(), *DescribePieces(Wall, Result.Fallen)),
			Result.Fallen.Num(), 0);

		TestTrue(
			*FString::Printf(
				TEXT("%d courses: the worst joint should be a HEAD joint of the hanging column, ")
				TEXT("it is c%d/%g-c%d/%g"),
				Heights[Index],
				Wall.CourseOf[Result.WorstPieceA], Wall.CellOf[Result.WorstPieceA],
				Wall.CourseOf[Result.WorstPieceB], Wall.CellOf[Result.WorstPieceB]),
			Result.WorstPieceA != INDEX_NONE
				&& Wall.CourseOf[Result.WorstPieceA] == Wall.CourseOf[Result.WorstPieceB]);

		// Tripwire, not physics: measured 1.285 and 1.300.
		constexpr double MinimumHeadOverBed = 1.15;

		TestTrue(
			*FString::Printf(
				TEXT("%d courses: the hanging column's HEAD joint must govern with margin — it ")
				TEXT("reads %.8g against the worst BED joint's %.8g at c%d/%g-c%d/%g, a ratio of ")
				TEXT("%.4g, and below %.2f this test is measuring the wrong joint"),
				Heights[Index], WorstHead, WorstBed,
				WorstBedA == INDEX_NONE ? -1 : Wall.CourseOf[WorstBedA],
				WorstBedA == INDEX_NONE ? -1.0 : Wall.CellOf[WorstBedA],
				WorstBedB == INDEX_NONE ? -1 : Wall.CourseOf[WorstBedB],
				WorstBedB == INDEX_NONE ? -1.0 : Wall.CellOf[WorstBedB],
				WorstBed > 0.0 ? WorstHead / WorstBed : 0.0, MinimumHeadOverBed),
			WorstBed > 0.0 && WorstHead >= MinimumHeadOverBed * WorstBed);

		// The red: each course should shed its weight into its own head-joint pair.
		TestTrue(
			*FString::Printf(
				TEXT("%d courses: the hanging column should read %.8g of head-joint shear ")
				TEXT("capacity, it reads %.8g"),
				Heights[Index], HangingColumnShearUtilisation, Result.Worst),
			FMath::IsNearlyEqual(Result.Worst, HangingColumnShearUtilisation, UtilisationTolerance));

		// The wrong answer pinned at its measured size, so a differently-wrong model fails here.
		const int32 HangingBricks = Heights[Index] - (CutCourse + 1);

		TestTrue(
			*FString::Printf(
				TEXT("%d courses: CHARACTERISATION of the known red — the model should read the ")
				TEXT("per-pair figure %.8g times the %d brick(s) hanging over the hole, %.17g, and ")
				TEXT("it reads %.17g. This row PASSES: it holds the defect at its measured size"),
				Heights[Index], HangingColumnShearUtilisation, HangingBricks,
				HangingBricks * HangingColumnShearUtilisation, Result.Worst),
			FMath::IsNearlyEqual(
				Result.Worst, HangingBricks * HangingColumnShearUtilisation, UtilisationTolerance));
	}

	if (bRan[0] && bRan[1])
	{
		// The property itself, independent of the absolute value; no tuning satisfies it for a foot-loaded model.
		TestTrue(
			*FString::Printf(
				TEXT("HEIGHT-INDEPENDENT: %d courses reads %.8g and %d courses reads %.8g, a ")
				TEXT("factor of %.4g over a height ratio of %.4g — the model reads the column's ")
				TEXT("whole weight at its foot, so its separation is the ratio of the HANGING ")
				TEXT("BRICK COUNTS (%d against %d) and not of the heights"),
				Heights[0], Measured[0], Heights[1], Measured[1],
				Measured[0] > 0.0 ? Measured[1] / Measured[0] : 0.0,
				static_cast<double>(Heights[1]) / static_cast<double>(Heights[0]),
				Heights[0] - (CutCourse + 1), Heights[1] - (CutCourse + 1)),
			FMath::IsNearlyEqual(Measured[0], Measured[1], UtilisationTolerance));
	}

	return true;
}

/**
 * The fixture's bricklayer lays the same wall as Layout::RunningBond, so this file is not a second
 * definition of a wall. Compared as box sets, since emit order is the producer's business.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceFixtureAgreesWithProducerTest,
	"DestructionGame.Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceFixtureAgreesWithProducerTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace WallAcceptanceTestSupport;

	constexpr int32 Courses = 10;
	constexpr int32 Cells = 12;

	/** Slack against re-association only. */
	constexpr double PlacementToleranceCm = 1e-6;

	FRunningBondSpec Spec;
	Spec.BrickSizeCm = FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm);
	Spec.JointThicknessCm = JointCm;
	Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
	Spec.CoursesHigh = Courses;
	Spec.BricksPerCourse = Cells;
	Spec.End = EWallEnd::Flush;
	Spec.Strength = GeneralPurposeMortar;

	FBrickLayout Produced;

	if (!TestTrue(TEXT("FIXTURE: RunningBond should lay the reference wall"),
			RunningBond(Spec, Produced)))
	{
		return true;
	}

	FWallCase Case;
	Case.Number = 0;
	Case.Title = TEXT("the reference wall");
	Case.Courses = Courses;
	Case.Cells = Cells;

	FWall Wall;
	LayWall(Case, Wall);

	TestEqual(
		FString::Printf(
			TEXT("the fixture should lay the same number of bricks as RunningBond; it laid %d ")
			TEXT("against %d"),
			Wall.NumPieces(), Produced.Boxes.Num()),
		Wall.NumPieces(), Produced.Boxes.Num());

	TestEqual(
		FString::Printf(
			TEXT("the fixture should emit the same number of joints as RunningBond; it emitted %d ")
			TEXT("against %d"),
			Wall.Structure.NumConnections(), Produced.Structure.NumConnections()),
		Wall.Structure.NumConnections(), Produced.Structure.NumConnections());

	int32 Unmatched = 0;
	FString FirstUnmatched;

	for (int32 Piece = 0; Piece < Wall.NumPieces(); ++Piece)
	{
		bool bFound = false;

		for (const FPieceBox& Reference : Produced.Boxes)
		{
			if (Reference.CentreCm.Equals(Wall.Boxes[Piece].CentreCm, PlacementToleranceCm)
				&& Reference.ExtentCm.Equals(Wall.Boxes[Piece].ExtentCm, PlacementToleranceCm))
			{
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			++Unmatched;

			if (FirstUnmatched.IsEmpty())
			{
				FirstUnmatched = FString::Printf(
					TEXT("c%d/%g centred (%g, %g, %g) half-size (%g, %g, %g)"),
					Wall.CourseOf[Piece], Wall.CellOf[Piece],
					Wall.Boxes[Piece].CentreCm.X, Wall.Boxes[Piece].CentreCm.Y,
					Wall.Boxes[Piece].CentreCm.Z,
					Wall.Boxes[Piece].ExtentCm.X, Wall.Boxes[Piece].ExtentCm.Y,
					Wall.Boxes[Piece].ExtentCm.Z);
			}
		}
	}

	TestEqual(
		FString::Printf(
			TEXT("every brick the fixture lays should be one RunningBond lays; %d were not, the ")
			TEXT("first is %s"),
			Unmatched, FirstUnmatched.IsEmpty() ? TEXT("none") : *FirstUnmatched),
		Unmatched, 0);

	return true;
}

/**
 * Production's DestructionWallCases lays every catalogue wall bit-identical to the fixture, in
 * handle order, with the same (course, cell) grid and the same bricks named by each region. Readings
 * here are pinned to 1e-9, so a one-ulp difference moves many tests. The two bricklayers are
 * independent copies; this is the only test comparing them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceProducerMatchesFixtureTest,
	"DestructionGame.Acceptance.Wall.TheProducerLaysTheWallTheFixtureLays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceProducerMatchesFixtureTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/** Region bounds sit at least 0.125 cells from any centre, so this is ample. */
	constexpr double CellTolerance = 1.0e-9;

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty-two cases"), Cases.Num(), 22);

	for (const FWallCase& Case : Cases)
	{
		const FString Where = FString::Printf(TEXT("case %d (%s)"), Case.Number, Case.Title);

		FWall Fixture;
		LayWall(Case, Fixture);

		if (Fixture.NumPieces() == 0)
		{
			AddError(FString::Printf(TEXT("%s: FIXTURE laid no bricks at all"), *Where));

			continue;
		}

		DestructionWallCases::FWallLayout Laid;

		if (!DestructionWallCases::Build(ProductionSpecOf(Case), Laid))
		{
			AddError(FString::Printf(
				TEXT("%s: production must lay this wall before it can be compared to the fixture ")
				TEXT("every reading in this file was taken on"),
				*Where));

			continue;
		}

		/* --- the same wall, brick for brick, joint for joint, handle for handle ------------- */

		LayoutMatchesFixture(*this, Where, Laid.Layout, Fixture);

		/* --- and the same grid under it ----------------------------------------------------- */

		int32 FirstWrongGrid = INDEX_NONE;
		FString WhyGridWrong;

		const int32 CommonGrid = FMath::Min(
			FMath::Min(Laid.CourseOf.Num(), Laid.CellOf.Num()), Fixture.NumPieces());

		for (int32 Piece = 0; Piece < CommonGrid; ++Piece)
		{
			if (Laid.CourseOf[Piece] == Fixture.CourseOf[Piece]
				&& FMath::IsNearlyEqual(Laid.CellOf[Piece], Fixture.CellOf[Piece], CellTolerance))
			{
				continue;
			}

			FirstWrongGrid = Piece;

			WhyGridWrong = FString::Printf(
				TEXT("production puts it at c%d/%s, the fixture at c%d/%s"),
				Laid.CourseOf[Piece], *Bits(Laid.CellOf[Piece]),
				Fixture.CourseOf[Piece], *Bits(Fixture.CellOf[Piece]));

			break;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: production must hand back one (course, cell) per piece — %d courses and ")
				TEXT("%d cells for %d pieces"),
				*Where, Laid.CourseOf.Num(), Laid.CellOf.Num(), Laid.Layout.Boxes.Num()),
			Laid.CourseOf.Num() == Laid.Layout.Boxes.Num()
				&& Laid.CellOf.Num() == Laid.Layout.Boxes.Num());

		TestTrue(
			*FString::Printf(
				TEXT("%s: every piece must sit on the same (course, cell) the fixture puts it on, or ")
				TEXT("every region in this file names different bricks — piece %d is the first that ")
				TEXT("does not: %s"),
				*Where, FirstWrongGrid,
				FirstWrongGrid == INDEX_NONE ? TEXT("none is") : *WhyGridWrong),
			FirstWrongGrid == INDEX_NONE);

		/* --- and the regions resolve to the same bricks -------------------------------------- */

		struct FRegionCheck
		{
			const TCHAR* What;
			TArrayView<const FWallRegion> Regions;
		};

		const FRegionCheck RegionChecks[] =
		{
			{ TEXT("cut"), Case.Cuts },
			{ TEXT("must-fall"), Case.MustFall },
			{ TEXT("must-stand"), Case.MustStand },
		};

		for (const FRegionCheck& Check : RegionChecks)
		{
			if (Check.Regions.Num() == 0)
			{
				continue;
			}

			const TArray<int32> Expected = PiecesInRegions(Fixture, Check.Regions);

			const TArray<DestructionWallCases::FWallRegion> Theirs =
				ProductionRegionsOf(Check.Regions);

			TArray<int32> Named;
			DestructionWallCases::PiecesInRegions(Laid, Theirs, Named);

			TestTrue(
				*FString::Printf(
					TEXT("%s: the %s regions must name exactly the bricks the fixture names, in ")
					TEXT("handle order — the fixture names %d %s, production names %d %s"),
					*Where, Check.What,
					Expected.Num(), *DescribePieces(Fixture, Expected),
					Named.Num(), *DescribePieces(Fixture, Named)),
				Named == Expected);
		}
	}

	return true;
}

/**
 * Every case is a joinable level with the case's title, the fixture's wall in handle order, and
 * exactly the case's cut bricks (compared sorted). Lives here because only this file knows what each
 * case was measured on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceLevelsTest,
	"DestructionGame.Acceptance.Wall.EveryCaseIsAPlayableLevel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceLevelsTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;
	using namespace DestructionScenarios;

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty-two cases"), Cases.Num(), 22);

	for (const FWallCase& Case : Cases)
	{
		const FString LevelName = LevelNameForCase(Case.Number);
		const FString MapName = LevelMapNameForCase(Case.Number);

		const FString Where =
			FString::Printf(TEXT("case %d (%s) as '%s'"), Case.Number, Case.Title, *LevelName);

		/* --- ONE: there is a row, and it is this case ---------------------------------------- */

		const int32 Index = IndexOfName(FName(*LevelName));

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: the walls the user drew are the whole reason the acceptance set ")
					TEXT("exists, and until one is a catalogue row it is a fixture nobody can stand ")
					TEXT("in front of. There is no row named '%s'."),
					*Where, *LevelName),
				Catalogue().IsValidIndex(Index)))
		{
			continue;
		}

		const FScenario& Row = Catalogue()[Index];

		TestEqual(
			*FString::Printf(
				TEXT("%s: must be joinable from its own map, '%s'"), *Where, *MapName),
			FString(Row.MapName), MapName);

		// Verbatim, so the catalogue, acceptance row and banner are one case.
		TestEqual(
			*FString::Printf(TEXT("%s: must carry the case's own title"), *Where),
			FString(Row.Title), FString(Case.Title));

		/* --- TWO: it lays the wall the case was measured on ---------------------------------- */

		FWall Fixture;
		LayWall(Case, Fixture);

		if (Fixture.NumPieces() == 0)
		{
			AddError(FString::Printf(TEXT("%s: FIXTURE laid no bricks at all"), *Where));

			continue;
		}

		DestructionLayout::FBrickLayout Built;
		TArray<int32> BuiltCut;

		if (!Build(Row, Built, BuiltCut))
		{
			AddError(FString::Printf(
				TEXT("%s: the row must build — a catalogue row that cannot be laid is a level that ")
				TEXT("cannot be joined"),
				*Where));

			continue;
		}

		LayoutMatchesFixture(*this, Where, Built, Fixture);

		/* --- THREE: and it cuts the bricks the case cuts, and only those ---------------------- */

		const TArray<int32> Expected = CutPieces(Case, Fixture);

		TArray<int32> Actual = BuiltCut;
		Actual.Sort();

		TestTrue(
			*FString::Printf(
				TEXT("%s: the level must cut exactly the bricks the case cuts — the case cuts %d %s, ")
				TEXT("the level cuts %d %s. A level that lays the right wall and cuts the wrong brick ")
				TEXT("shows a plausible collapse that has nothing to do with the case it is named ")
				TEXT("after."),
				*Where,
				Expected.Num(), *DescribePieces(Fixture, Expected),
				Actual.Num(), *DescribePieces(Fixture, Actual)),
			Actual == Expected);

		// Derived from the case, not listed.
		if (Case.Cuts.Num() == 0)
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: this case cuts nothing, so its level must cut nothing"), *Where),
				Row.CutCentresCm.Num(), 0);
		}
		else
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s: this case cuts %d brick(s), so its level must name at least one cut ")
					TEXT("centre; it names %d"),
					*Where, Expected.Num(), Row.CutCentresCm.Num()),
				Row.CutCentresCm.Num() > 0);
		}
	}

	return true;
}

/**
 * No level captions a verdict the solver does not produce without admitting it. Each caption carries
 * exactly one `Expected: <VERDICT>` token, and the disagreement marker exactly when the model
 * disagrees (computed per row, not listed). The known-red list is a tripwire on the predicate itself.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceCaptionTest,
	"DestructionGame.Acceptance.Wall.EveryLevelsCaptionTellsTheTruth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceCaptionTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;
	using namespace DestructionScenarios;

	/** Rows the model gets wrong today; checks the predicate, does not drive the captions. */
	const int32 KnownDisagreements[] = { 21 };

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty-two cases"), Cases.Num(), 22);

	TArray<int32> Disagreed;

	for (const FWallCase& Case : Cases)
	{
		const FString LevelName = LevelNameForCase(Case.Number);

		const FString Where =
			FString::Printf(TEXT("case %d (%s) as '%s'"), Case.Number, Case.Title, *LevelName);

		// Read before the row lookup so the tripwire runs even when a row is missing.
		const FSolvedWall& Solved = RunWallCase(*this, Case);

		const FWall& Wall = Solved.Wall;
		const FWallResult& Result = Solved.Result;

		if (!Result.bLaid)
		{
			continue;
		}

		const bool bAgrees = ModelAgreesWithVerdict(Case, Wall, Result);

		if (!bAgrees)
		{
			Disagreed.Add(Case.Number);
		}

		const int32 Index = IndexOfName(FName(*LevelName));

		if (!Catalogue().IsValidIndex(Index))
		{
			AddError(FString::Printf(
				TEXT("%s: there is no row to caption. A level with nothing saying what should happen ")
				TEXT("leaves a human unable to tell a correct wall from a broken one."),
				*Where));

			continue;
		}

		const FString Caption = FString(Catalogue()[Index].Expectation);

		/* --- ONE: it claims its own verdict, and only its own -------------------------------- */

		const EVerdict Verdicts[] = { EVerdict::Stands, EVerdict::LocalLoss, EVerdict::Collapse };

		FString Claimed;
		int32 ClaimCount = 0;

		for (const EVerdict Verdict : Verdicts)
		{
			if (Caption.Contains(VerdictClaimFor(Verdict), ESearchCase::CaseSensitive))
			{
				++ClaimCount;
				Claimed = VerdictName(Verdict);
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: its caption must claim exactly one verdict, written '%s'; it claims %d ")
				TEXT("(%s). The caption reads: \"%s\""),
				*Where, *VerdictClaimFor(Case.Verdict), ClaimCount,
				ClaimCount == 0 ? TEXT("none") : *Claimed, *Caption),
			ClaimCount == 1
				&& Caption.Contains(VerdictClaimFor(Case.Verdict), ESearchCase::CaseSensitive));

		/* --- TWO: and it admits a disagreement exactly when there is one --------------------- */

		const bool bAdmits = Caption.Contains(ModelDisagreesMarker, ESearchCase::CaseSensitive);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the model %s this row's verdict of %s, so its caption %s say '%s'. A level ")
				TEXT("captioned with an outcome the solver does not produce is a lie told to somebody ")
				TEXT("standing in front of the counter-example; an admission left behind after the ")
				TEXT("model was fixed is the same lie the other way round. %d piece(s) came down %s. ")
				TEXT("The caption reads: \"%s\""),
				*Where,
				bAgrees ? TEXT("AGREES with") : TEXT("DISAGREES with"),
				VerdictName(Case.Verdict),
				bAgrees ? TEXT("must NOT") : TEXT("MUST"),
				ModelDisagreesMarker,
				Result.Fallen.Num(), *DescribePieces(Wall, Result.Fallen), *Caption),
			bAdmits, !bAgrees);
	}

	/* --- and the predicate above is the catalogue's, checked against the known reds ---------- */

	TArray<int32> Known;
	Known.Append(KnownDisagreements, UE_ARRAY_COUNT(KnownDisagreements));

	auto NumberList = [](TArrayView<const int32> Numbers)
	{
		FString Text;

		for (const int32 Number : Numbers)
		{
			if (!Text.IsEmpty())
			{
				Text += TEXT(", ");
			}

			Text += FString::FromInt(Number);
		}

		return Text.IsEmpty() ? FString(TEXT("none")) : Text;
	};

	const FString DisagreedText = NumberList(Disagreed);
	const FString KnownText = NumberList(Known);

	// Set equality split by direction: growth is a regression, shrinkage is a fix with paperwork owed.
	TArray<int32> Grew;

	for (const int32 Number : Disagreed)
	{
		if (!Known.Contains(Number))
		{
			Grew.Add(Number);
		}
	}

	TArray<int32> Shrank;

	for (const int32 Number : Known)
	{
		if (!Disagreed.Contains(Number))
		{
			Shrank.Add(Number);
		}
	}

	TestTrue(
		*FString::Printf(
			TEXT("TRIPWIRE — THE SET GREW: case(s) {%s} read WRONG here and are not on the known-red ")
			TEXT("list. Nothing on that list was expected to grow on its own, so this is a ")
			TEXT("REGRESSION until proved otherwise: the model now disagrees somewhere it used to ")
			TEXT("agree, or this test's reading of a verdict has drifted from Acceptance.Wall.")
			TEXT("Catalogue's and a level is being captioned by the wrong rule. (It read {%s}, the ")
			TEXT("known reds are {%s}.) A verdict that was deliberately re-ruled lands here too — if ")
			TEXT("that is what happened, the same change must add the row to the list."),
			*NumberList(Grew), *DisagreedText, *KnownText),
		Grew.Num() == 0);

	TestTrue(
		*FString::Printf(
			TEXT("TRIPWIRE — THE SET CHANGED SHAPE: case(s) {%s} are on the known-red list and the ")
			TEXT("model now AGREES with them. That is a row being FIXED or an expectation being ")
			TEXT("corrected, not a regression — and it is unfinished paperwork: the same change owes ")
			TEXT("this list, the level caption's '%s' marker, and the row's DropsToday anchor in ")
			TEXT("Acceptance.Wall.Catalogue. (It read {%s}, the known reds are {%s}.)"),
			*NumberList(Shrank), ModelDisagreesMarker, *DisagreedText, *KnownText),
		Shrank.Num() == 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
