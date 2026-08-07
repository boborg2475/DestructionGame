// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE WALL ACCEPTANCE SET — nineteen configurations with an expected outcome for each, drawn
 * from how real masonry behaves rather than from what the solver computes.
 *
 * The catalogue is claude_plans/WALL_CASES.html and the user agreed it on 2026-08-06. This file
 * is that catalogue as a parameterised table: ADDING A CASE IS ADDING NUMBERS, NOT CODE. Case 20
 * is written; case 20's own uncertainty is recorded beside it.
 *
 * WHY A SPREAD RATHER THAN ONE FIXTURE. The arching argument cannot be settled by one collapsing
 * wall. FIVE MATCHED PAIRS differ by exactly one variable each — 7 vs 8 is depth of cover, 7 vs 9
 * is span, 7 vs 10 is the abutment, 13 vs 14 is corbel projection, 15 vs 16 is whether
 * superimposed compression suppresses bending tension — so a disagreement between the two halves
 * of a pair points at ONE TERM in the force calculation, which a single wall never can. A solver
 * that answers both halves of a pair the same way has no such term at all, and the second test in
 * this file says exactly that in one line per pair.
 *
 * THREE VERDICTS, THREE ASSERTION SHAPES, per DESIGN.md §4's outcome-not-mechanism rule:
 *
 *   STANDS      nothing left the structure AND no joint anywhere gave. Both halves, because
 *               "no piece fell" alone passes for a wall that severed half its joints and stayed
 *               leaning together.
 *   LOCAL LOSS  the set of pieces that lost the ground is EXACTLY the named set. Identity, not
 *               a count: a test that only counted would be satisfied by the wrong bricks falling,
 *               and "the course over the doorway drops but the wall is fine" is the whole point
 *               of having a middle verdict at all.
 *   COLLAPSE    every piece of a named region lost the ground, and every piece of a named
 *               SURVIVOR region kept it. Two-sided, because a wall that comes down because
 *               everything comes down is not evidence that the span term works.
 *
 * DISPLACEMENT IS NOT USED AS A BREAK ASSERTION ANYWHERE HERE, and it could not be: DESIGN.md §4
 * is explicit that two pieces can sever and stay resting exactly in place. What is read instead is
 * whether a piece still has a path to the earth after the cascade — which is what the binding
 * pushes to physics, so it is the outcome and not the mechanism.
 *
 * NEEDS A TICKING WORLD: NO, DELIBERATELY. Everything DESIGN.md §4 asks of an integration test is
 * here — gravity is on (weight is mass x 980 and there is no way to switch it off), everything is
 * connected, and the assertion is on outcome — and the one thing a world would add is the WIRE
 * from the solver's answer to Chaos, which Tests/StructureIntegrationTest.cpp already covers three
 * times over and which is identical for all nineteen rows. Nineteen worlds of up to 375 brick
 * actors would cost minutes to say nothing new. If a row ever needs to be watched falling, promote
 * that ONE row into the integration file rather than moving this table.
 *
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. See CURRENT_STATE.md; the `using namespace` lives inside each RunTest body for
 * the same reason.
 *
 * NOTHING HERE IS IMPORTED FROM THE CODE UNDER TEST EXCEPT THE PRODUCER ITSELF. The grid, the
 * brick weight, the newton-to-Unreal conversion and every strength are re-derived below, so a
 * wrong constant in production makes this file DISAGREE with it rather than agree with it. The one
 * deliberate exception is Layout::MakeInterface, which decides whether two boxes share a face:
 * re-implementing that would be re-implementing the thing under test, and
 * Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays pins the fixture's own bricklaying against
 * Layout::RunningBond so this file cannot quietly become a second, drifting wall producer.
 */
namespace WallAcceptanceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * THE COORDINATING GRID, RE-DERIVED HERE. UK metric standard brick with a 1 cm joint, so a
	 * brick plus a joint is one CELL along the wall (22.5 cm) and one COURSE up (7.5 cm), and
	 * running bond offsets alternate courses by half a cell. Every position in the table below is
	 * quoted in CELLS — a piece's cell index is its centre X divided by 22.5 — because that is the
	 * unit the bond is built on and the only one in which "one brick out, mid-wall" is a number
	 * somebody can check by eye.
	 */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickDepthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double JointCm = 1.0;

	constexpr double CellPitchCm = BrickLengthCm + JointCm;
	constexpr double CoursePitchCm = BrickHeightCm + JointCm;
	constexpr double HalfCellCm = CellPitchCm * 0.5;

	/** What is left of a brick when a joint is taken out of it and the remainder halved. */
	constexpr double HalfBatLengthCm = (BrickLengthCm - JointCm) * 0.5;

	/** Every wall in this file has its left face here, so cell 0 is the first full brick. */
	constexpr double LeftFaceCm = -BrickLengthCm * 0.5;

	/**
	 * The shortest closing piece the fixture will lay at a course's left end.
	 *
	 * A course laid right-to-left rarely divides exactly, and what is left at the left end is a
	 * cut brick. Below this it is dropped instead, because a two-centimetre sliver is a piece with
	 * a plausible mass and an implausible joint, and it would sit at the FAR end from everything
	 * under test while being the first thing to break. 4 cm is comfortably under the 10.25 cm half
	 * bat a flush wall really closes with and comfortably over anything that would be laid by
	 * accident.
	 */
	constexpr double MinClosingPieceCm = 4.0;

	/** g/cm3. ClayBrick's published density, spelled out so a changed profile shows up here. */
	constexpr double ClayBrickDensityGramsPerCubicCm = 1.9;

	/**
	 * WEIGHT FROM MASS, DERIVED RATHER THAN IMPORTED.
	 *
	 * Unreal's gravity is 980 cm/s2 and mass is in kilograms, so kg x 980 IS the weight in Unreal
	 * force units — DESIGN.md §3's 1 N = 100 uu is already inside that number and applying it again
	 * is the 100x error the units section exists to prevent. Density first, then dimensions, which
	 * is the one association that lands exactly on 2.72163125 kg.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double FullBrickWeightUu =
		ClayBrickDensityGramsPerCubicCm * BrickLengthCm * BrickDepthCm * BrickHeightCm / 1000.0
		* GravityCmPerSecondSquared;

	/**
	 * Unreal force units per MPa per cm2, SPELLED OUT RATHER THAN IMPORTED.
	 *
	 * Production has one named boundary for this, Core/ConnectionStrength.h's
	 * ForceUnitsPerMPaSqCm. A test that read it would agree with a wrong one, so this writes the
	 * number itself and disagrees instead.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 10000.0;

	/** EN 1996-1-1 Table 3.4's f_vk0 for general purpose mortar, asserted against the profile. */
	constexpr double MortarShearCohesionMPa = 0.2;

	/** A head joint: the end face of a brick, 10.25 cm through the wall by 6.5 cm high. */
	constexpr double HeadJointAreaSqCm = BrickDepthCm * BrickHeightCm;

	enum class EBond : uint8
	{
		/** Alternate courses offset half a cell, half bats filling the ends flush. */
		Running,

		/** Every course identical, so every head joint lines up through the whole wall. */
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
	 * A rectangle in (course, cell) space — the one vocabulary the whole table speaks.
	 *
	 * A piece is in the region when its course is within the inclusive course range and its cell
	 * index is STRICTLY between the two cell bounds. Strict on purpose: every brick centre in
	 * every wall here is an exact multiple of a quarter cell, so a bound placed on a quarter-cell
	 * boundary would be a tolerance question, and every bound written below is at least an eighth
	 * of a cell (2.8 cm) clear of any brick centre.
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

		/**
		 * A course whose end brick is pushed half a cell past the face of the wall, or INDEX_NONE.
		 *
		 * This is the projecting-header fixture. A flush odd course normally closes with a half
		 * bat; pushing the face out half a cell and closing with a FULL brick instead puts a whole
		 * brick where the bat was, half of it bearing on the course below and half of it over air.
		 */
		int32 ProjectingCourse = INDEX_NONE;

		/** Everything the player deletes, applied AFTER the intact wall has been checked. */
		TArrayView<const FWallRegion> Cuts;

		/**
		 * What must lose its path to the earth.
		 *
		 * EXACT for a local loss — the fallen set must be precisely the pieces named here, so the
		 * wrong bricks falling is a failure. A LOWER BOUND for a collapse, paired with MustStand
		 * as the upper one, because the exact boundary of a collapse is a model detail and
		 * over-claiming it would make the row fail for a reason nobody asked about.
		 */
		TArrayView<const FWallRegion> MustFall;

		/** What must keep it. Read only for a collapse; a local loss gets exactness instead. */
		TArrayView<const FWallRegion> MustStand;
	};

	/* ================================================================================
	 * THE FIXTURE'S OWN BRICKLAYER.
	 * ================================================================================
	 *
	 * WHY NOT Layout::RunningBond. Six of the nineteen cases are not running-bond rectangles:
	 * two corbel out, two carry a projecting header, and two are stack bond. RunningBond lays one
	 * shape, so either this file gets a second producer or six cases get dropped — and the six
	 * dropped ones include two of the five matched pairs, which are the whole reason the set
	 * exists.
	 *
	 * THE BRICKLAYER IS ONE RULE, NOT SIX SPECIAL CASES. Every course is described by two numbers
	 * — where its right face is, and how long its rightmost piece is — and then laid RIGHT TO
	 * LEFT on the 22.5 cm pitch, closing with whatever is left at the wall's left face. Running
	 * bond, stack bond, a corbel and a projecting header are all values of those two numbers:
	 *
	 *      running bond    right face fixed; odd courses close with a half bat at the right
	 *      stack bond      right face fixed; every course closes with a full brick
	 *      corbel          right face steps out once per course from CorbelFromCourse
	 *      header          one course's right face is half a cell further out
	 *
	 * Laying right-to-left rather than left-to-right is what keeps the cut piece at the LEFT end,
	 * far from every corbel and header under test, instead of in the middle of what is being
	 * measured.
	 *
	 * AND IT IS CHECKED AGAINST THE REAL PRODUCER. A flush running-bond wall laid here must be the
	 * same wall Layout::RunningBond lays, brick for brick — see
	 * Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays. Without that, this file is a second
	 * definition of what a wall is and every number below is measured against the wrong one.
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

	/**
	 * MASS FROM GEOMETRY, DERIVED HERE RATHER THAN IMPORTED.
	 *
	 * Density is g/cm3 and dimensions are cm, so cm3 x g/cm3 is grams and grams / 1000 is
	 * kilograms. No force conversion belongs here: 1 N = 100 uu is a property of forces and mass
	 * goes into Unreal unconverted. Density first for the same reason production does it — that is
	 * the association that lands exactly on 2.72163125 for a full brick.
	 */
	double PieceMassKgHere(const FPieceBox& Box)
	{
		return ClayBrickDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/** A wall, plus where every piece of it sits in (course, cell) terms. */
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

				/*
				 * THE BOX'S CENTRE IS THE CENTRE OF MASS, because a brick is a homogeneous solid
				 * and its mass came off that same box. Without it the wall has no eccentricity at
				 * all and every corbel in it reads as though its weight acted through the middle
				 * of its support — which is exactly the state HasCompleteGeometry exists to make
				 * askable, and it is asserted as a fixture precondition below.
				 */
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

		/*
		 * THE PAIRS: neighbours along a course, and every piece of the course below. Offering the
		 * whole course below rather than working out which pieces something spans is what keeps
		 * the mixed-size and corbelled cases honest — MakeInterface refuses the pairs that turn out
		 * to be diagonals, and that decision belongs to it and to nothing written here. Courses
		 * two apart are never offered because a 6.5 cm brick on a 7.5 cm pitch cannot reach.
		 */
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

	/**
	 * Which live pieces have lost their path to the earth.
	 *
	 * THIS IS THE OUTCOME, NOT THE MECHANISM. A joint severing is a step; what the player sees is
	 * which bricks come down, and a piece comes down exactly when the solve says nothing is
	 * holding it any more. Stranded counts as fallen for the same reason Falling does: the piece
	 * is not being carried, and whether the solver could not route it or genuinely has nothing to
	 * route it through is a different question — asked in the report line below, never folded into
	 * the verdict.
	 */
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

	/** The worst utilisation over the joints still in the structure, and which pair carries it. */
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

	/** A piece list as (course, cell), capped so a total collapse does not fill the log. */
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

	/** What one case's wall did, once it has been laid, cut and solved to a standstill. */
	struct FWallResult
	{
		bool bLaid = false;

		int32 PiecesLaid = 0;
		int32 PiecesCut = 0;

		/** Passes that broke at least one joint, as built and then after the cut. */
		int32 IntactPasses = 0;
		int32 CutPasses = 0;

		/** Whether the intact wall stood — only meaningful for a case that cuts something. */
		bool bIntactStood = false;

		TArray<int32> Fallen;
		double Worst = 0.0;
		int32 WorstPieceA = INDEX_NONE;
		int32 WorstPieceB = INDEX_NONE;
	};

	/**
	 * Lay it, check it stands, cut it, let the cascade run, and report what came down.
	 *
	 * THE INTACT WALL IS CHECKED FIRST AND IT IS NOT DECORATION. A case whose wall was already
	 * falling apart before the player touched it measures nothing, and every "stands" row would
	 * fail for a reason that has nothing to do with the case. It is skipped only for the rows that
	 * cut NOTHING — the corbels, the header and the intact walls — where the as-built state IS the
	 * case under test.
	 */
	void RunWallCase(FAutomationTestBase& Test, const FWallCase& Case, FWall& OutWall, FWallResult& OutResult)
	{
		LayWall(Case, OutWall);

		OutResult.PiecesLaid = OutWall.NumPieces();

		if (OutResult.PiecesLaid == 0)
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
			OutWall.Structure.HasCompleteGeometry());

		OutResult.IntactPasses = OutWall.Structure.SolveAndBreak();
		OutResult.bIntactStood = OutResult.IntactPasses == 0 && FallenPieces(OutWall).Num() == 0;

		if (Case.Cuts.Num() > 0)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("case %d (%s): FIXTURE the wall must stand before the player cuts it; it ")
					TEXT("broke joints in %d pass(es) and dropped %d piece(s) as built"),
					Case.Number, Case.Title,
					OutResult.IntactPasses, FallenPieces(OutWall).Num()),
				OutResult.bIntactStood);

			const TArray<int32> Cut = CutPieces(Case, OutWall);

			OutResult.PiecesCut = Cut.Num();

			if (Cut.Num() == 0)
			{
				Test.AddError(FString::Printf(
					TEXT("case %d (%s): FIXTURE the cut regions named no brick at all"),
					Case.Number, Case.Title));

				return;
			}

			for (const int32 Piece : Cut)
			{
				OutWall.Structure.RemovePiece(Piece);
			}

			OutResult.CutPasses = OutWall.Structure.SolveAndBreak();
		}

		OutResult.Fallen = FallenPieces(OutWall);
		OutResult.Worst = WorstUtilisation(OutWall, OutResult.WorstPieceA, OutResult.WorstPieceB);
		OutResult.bLaid = true;
	}

	/** One line per case, whether it passed or not, so the whole set reads off the log. */
	void ReportWallCase(
		FAutomationTestBase& Test,
		const FWallCase& Case,
		const FWall& Wall,
		const FWallResult& Result)
	{
		Test.AddInfo(FString::Printf(
			TEXT("case %02d %-44s expected %-10s | laid %d, cut %d, passes %d(+%d), fell %d %s, ")
			TEXT("worst %.6g%s"),
			Case.Number, Case.Title, VerdictName(Case.Verdict),
			Result.PiecesLaid, Result.PiecesCut, Result.IntactPasses, Result.CutPasses,
			Result.Fallen.Num(), *DescribePieces(Wall, Result.Fallen),
			Result.Worst,
			Result.WorstPieceA == INDEX_NONE
				? TEXT("")
				: *FString::Printf(
					TEXT(" at c%d/%g-c%d/%g"),
					Wall.CourseOf[Result.WorstPieceA], Wall.CellOf[Result.WorstPieceA],
					Wall.CourseOf[Result.WorstPieceB], Wall.CellOf[Result.WorstPieceB])));
	}

	/* ================================================================================
	 * THE CATALOGUE.
	 * ================================================================================ */

	/* --- A: one brick out. ---------------------------------------------------------
	 *
	 * THIRTY COURSES, NOT THE TEN THE DRAWING SHOWS, AND THE HEIGHT IS THE WHOLE POINT.
	 * ARCHING_DESIGN.md works the half-seated joint out at 0.058203838 of capacity PER BRICK
	 * WEIGHT it carries, so a brick with nine courses over it reads 0.52 and a wall ten courses
	 * tall says "stands" whatever the model does — the row would assert nothing and pass forever.
	 * The joint reaches 1.0 at 17.18 brick weights, i.e. at eighteen courses of cover, so thirty
	 * courses with the cut in course 1 puts it firmly past the line (about 28 brick weights, which
	 * is the 1.62971 that design records). The physical claim — a real wall does not notice one
	 * brick — is height-independent; the fixture has to be tall enough to be able to disagree.
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

	/**
	 * Case 8's one course of cover, and the only thing that may drop: the whole four bricks.
	 *
	 * THE CATALOGUE SAYS THE COURSE DROPS, NOT PART OF IT — "a single course cannot arch, it is a
	 * beam in flexure over four bricks" — so all four are named. Recorded because the model today
	 * drops only the two MIDDLE ones: the bricks at cells 4 and 7 each keep one bed patch on the
	 * jamb and hold at about 0.27, while the two at cells 5 and 6 have no patch at all. Which of
	 * the two readings is right is a question about whether a half-seated brick over a doorway is
	 * really carried, and it is exactly what this row exists to force.
	 */
	const FWallRegion Case8Falls[] = { { 4, 4, 3.75, 7.25 } };

	/** Case 9: ten cells of opening in a fourteen-cell wall, two cells of jamb either side. */
	const FWallRegion Case9Cuts[] = { { 1, 3, 1.75, 11.25 } };
	const FWallRegion Case9Falls[] = { { 4, 11, 1.75, 11.25 } };
	const FWallRegion Case9Stands[] = { { 0, 3, -1.0, 1.75 }, { 0, 3, 11.25, 15.0 } };

	/** Case 10: the same four cells of cover as case 7, cut through to the free right end. */
	const FWallRegion Case10Cuts[] = { { 1, 3, 7.75, 11.50 } };
	const FWallRegion Case10Falls[] = { { 4, 11, 8.00, 11.50 } };
	const FWallRegion Case10Stands[] = { { 0, 3, -1.0, 7.75 } };

	/* --- C: spanning between supports. --------------------------------------------- */

	const FWallRegion Case11Cuts[] = { { 0, 3, 2.75, 8.25 } };

	/**
	 * Case 12 names no survivors, and it is the weakest row in the set because of it.
	 *
	 * The catalogue's own picture has the entire wall coming down, so there is nothing beside the
	 * collapse to claim, and a row whose expected outcome is "everything" cannot be quietly wrong
	 * about which pieces it named. It passes today, and it would pass for a model that always
	 * answers "falls" — which is why it is CASE 11 that carries this pair's diagnostic weight and
	 * why the pair test reads the two together rather than trusting either alone.
	 */
	const FWallRegion Case12Cuts[] = { { 0, 3, 0.75, 10.75 } };
	const FWallRegion Case12Falls[] = { { 4, 11, -2.0, 14.0 } };

	/* --- D: corbelling and the projecting header. ----------------------------------- */

	constexpr int32 CorbelCells = 8;
	constexpr int32 CorbelFirstCourse = 6;
	constexpr double QuarterBrickStepCm = HalfCellCm * 0.5;
	constexpr double HalfBrickStepCm = HalfCellCm;

	/*
	 * Case 14's four projecting bricks, named one course at a time.
	 *
	 * A course that steps out half a cell puts its end brick directly over the end brick of the
	 * course below, so the end brick keeps ONE bed patch of 10.25 cm and hangs 11.25 cm past it —
	 * the corbel this project already photographs elsewhere. Their cell indices are 7.5, 8.0, 8.5
	 * and 9.0; the SECOND piece of a higher corbel course lands on some of those same cells, which
	 * is why these are four one-course regions rather than one four-course one.
	 */
	const FWallRegion Case14Falls[] =
	{
		{ 6, 6, 7.40, 7.60 },
		{ 7, 7, 7.90, 8.10 },
		{ 8, 8, 8.40, 8.60 },
		{ 9, 9, 8.90, 9.10 },
	};

	/** Everything below the corbel. The wall behind a corbel that peels is not part of it. */
	const FWallRegion Case14Stands[] = { { 0, CorbelFirstCourse - 1, -2.0, 8.0 } };

	/**
	 * Case 16: the projecting header itself, at cell 11.5 of the top course, and nothing else.
	 *
	 * THIS ROW MAY BE THE ONE THAT IS WRONG, AND THE ARITHMETIC SAYS SO RATHER THAN A HUNCH.
	 * The header keeps a 105.0625 cm2 bed patch and its weight acts 5.625 cm outboard of that
	 * patch's centroid, so on a 179.4817708 cm3 section it reads
	 *
	 *     2667.198625 x 5.625 / 179.4817708 = 0.0083591 MPa   bending, opening the inner edge
	 *     2667.198625 / 105.0625            = 0.0025387 MPa   its own weight, closing it
	 *     tension 0.0058204 / 0.1 (f_xk1)   = 0.058204        of flexural bond capacity
	 *
	 * — which is a brick standing at six percent of what holds it, not a brick falling off. The
	 * catalogue's "local loss" is an OVERTURNING reading (the resultant is 5.625 cm out on a patch
	 * only 5.125 cm wide, so it lies outside the bearing), and this project's model is an uncracked
	 * bonded section, where cured mortar carries exactly that. Both readings are defensible and
	 * they disagree; the expectation the user agreed is encoded, and the disagreement is recorded
	 * here so nobody implements a change to satisfy it before it has been re-ruled.
	 *
	 * WHAT THIS ROW HAS ALREADY PROVED, whichever way that goes: the pair 15/16 differ ONLY in
	 * what sits on the header's tail, and the model reads 0.00184 for case 15 against 0.05820 for
	 * case 16 — a factor of 32 from superimposed load alone, with case 15's tension driven to zero
	 * and compression left governing. The term this pair was written to test EXISTS and works. What
	 * is in dispute is only where its threshold sits.
	 */
	const FWallRegion Case16Falls[] = { { 9, 9, 11.40, 11.60 } };

	/* --- E: bond pattern and head-joint shear. --------------------------------------- */

	const FWallRegion Case18Cuts[] = { { 5, 5, 4.75, 5.25 } };

	/* --- F: losing the base, and the staircase void. --------------------------------- */

	/**
	 * Case 19's survivor region reaches two cells clear of the cut, and it is meant to.
	 *
	 * Six bricks of base gone; the masonry over them has no path to the earth and must come down.
	 * What must NOT come down is the half of the wall that still has its footing, and cell 8 is
	 * two and a half cells right of where the cut ends — so a failure here is the 33.69 degree
	 * spreading front ARCHING_DESIGN.md describes walking further across the wall than the missing
	 * support can account for, which is the defect and not a boundary quibble.
	 */
	const FWallRegion Case19Cuts[] = { { 0, 0, -0.50, 5.25 } };
	const FWallRegion Case19Falls[] = { { 1, 9, -1.00, 4.60 } };
	const FWallRegion Case19Stands[] = { { 0, 9, 7.75, 13.0 } };

	/*
	 * CASE 20 — THE STAIRCASE VOID, and it is the one case in the set that was drafted with a
	 * question mark. The user ruled on 2026-08-06: LOCAL LOSS, the loose toothed bricks at the cut
	 * edge drop and the mass of the wall stands.
	 *
	 * THE RAKING CUT, one region per course, reading up. Each course above the last is cut one
	 * cell less far to the right, so the surviving masonry to the RIGHT of the void steps left
	 * over the hole as it rises — that is the overhang in the screenshot.
	 *
	 * WHICH BRICKS ARE THE TEETH, WORKED OUT FROM THE GEOMETRY RATHER THAN FROM THE DRAWING. A
	 * brick in an even course at cell k sits on the odd course below at cells k - 0.5 and k + 0.5;
	 * an odd-course brick at cell k + 0.5 sits on cells k and k + 1. Walking the surviving pieces
	 * against the cuts, exactly TWO of them are left with NO bed patch at all:
	 *
	 *     course 3, cell 4.5   sits over cut cells 4 and 5
	 *     course 5, cell 2.5   sits over cut cells 2 and 3
	 *
	 * Everything else along the cut face keeps at least one patch — course 2 cell 6, course 4
	 * cell 4 and course 6 cell 2 are all half seated, which is a corbel and not a tooth. Those two
	 * are the named set, and naming them by IDENTITY rather than by count is the point: a rule
	 * that dropped the corbelled half-seats instead would fall the same NUMBER of bricks and be
	 * completely wrong.
	 *
	 * THE FALL SET DRAWN IN WALL_CASES.html IS NOT USABLE and this deliberately does not
	 * transcribe it: its region overlaps its own cut regions almost entirely, so what it renders
	 * is one brick in course 7 that nothing in the prose claims. The prose is what was agreed.
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

	/**
	 * The catalogue, built rather than aggregate-initialised so every field is named at its value.
	 *
	 * TWENTY CASES, NINETEEN OF THEM RECTANGLES WITH HOLES IN. Nothing here is a shape somebody
	 * invented to break the solver: cases 1-5 are the deletions that fire on every click, 6-10 are
	 * doorways, 11-12 are a wall on piers, 13-16 are corbelling, 17-18 are the bond, and 19-20 are
	 * where collapse is the right answer.
	 */
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

		/* A — one brick out. Nothing here should do more than settle. */

		Add(1, TEXT("Intact wall"), EVerdict::Stands,
			TallCourses, StandardCells, {}, {}, {}, nullptr);

		Add(2, TEXT("One brick out, mid-wall"), EVerdict::Stands,
			TallCourses, StandardCells, Case2Cuts, {}, {}, TEXT("bond, against case 18"));

		/*
		 * CASE 3 IS THE ONE A PLAYER REPORTED, and the user ruled on 2026-08-06 that a brick
		 * deleted at a free end must not bring the wall down. ARCHING_DESIGN.md's slices 1-4 do
		 * not reach it — the surviving brick overhangs OUTWARD with nothing beyond it to abut
		 * against, so the arch is correctly refused and the cantilever ladder starts — so this row
		 * stays red until composite vertical action lands. It is not a defect in the row.
		 */
		Add(3, TEXT("One brick out at the free end"), EVerdict::Stands,
			TallCourses, StandardCells, Case3Cuts, {}, {}, nullptr);

		Add(4, TEXT("One brick out of the bottom course"), EVerdict::Stands,
			TallCourses, StandardCells, Case4Cuts, {}, {}, nullptr);

		Add(5, TEXT("Alternate bricks out of one course"), EVerdict::Stands,
			TallCourses, StandardCells, Case5Cuts, {}, {}, nullptr);

		/* B — openings and depth of cover. */

		Add(6, TEXT("Two-brick opening, deep cover"), EVerdict::Stands,
			CoveredCourses, StandardCells, TwoCellOpening, {}, {}, nullptr);

		Add(7, TEXT("Four-brick opening, eight courses over"), EVerdict::Stands,
			CoveredCourses, StandardCells, FourCellOpening, {}, {},
			TEXT("cover vs 8, span vs 9, abutment vs 10"));

		Add(8, TEXT("Four-brick opening, one course over"), EVerdict::LocalLoss,
			5, StandardCells, FourCellOpening, Case8Falls, {}, TEXT("depth of cover, against case 7"));

		Add(9, TEXT("Ten-brick opening, eight courses over"), EVerdict::Collapse,
			CoveredCourses, 14, Case9Cuts, Case9Falls, Case9Stands, TEXT("span, against case 7"));

		Add(10, TEXT("Opening at a free end, no abutment"), EVerdict::Collapse,
			CoveredCourses, StandardCells, Case10Cuts, Case10Falls, Case10Stands,
			TEXT("abutment, against case 7"));

		/* C — spanning between supports. */

		Add(11, TEXT("Wall on two piers, six-brick clear span"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case11Cuts, {}, {}, TEXT("pier width, against case 12"));

		Add(12, TEXT("The same span on one-brick piers"), EVerdict::Collapse,
			CoveredCourses, StandardCells, Case12Cuts, Case12Falls, {},
			TEXT("pier width, against case 11"));

		/* D — corbelling, and the header that is held down by what sits on it. */

		{
			FWallCase& Case = Add(13, TEXT("Corbel, quarter brick per course"), EVerdict::Stands,
				10, CorbelCells, {}, {}, {}, TEXT("corbel projection, against case 14"));

			Case.CorbelFromCourse = CorbelFirstCourse;
			Case.CorbelStepCm = QuarterBrickStepCm;
		}

		{
			FWallCase& Case = Add(14, TEXT("Corbel, half brick per course"), EVerdict::Collapse,
				10, CorbelCells, {}, Case14Falls, Case14Stands,
				TEXT("corbel projection, against case 13"));

			Case.CorbelFromCourse = CorbelFirstCourse;
			Case.CorbelStepCm = HalfBrickStepCm;
		}

		{
			FWallCase& Case = Add(15, TEXT("Header out half a brick, six courses on top"),
				EVerdict::Stands, 10, StandardCells, {}, {}, {},
				TEXT("superimposed load, against case 16"));

			Case.ProjectingCourse = 3;
		}

		{
			FWallCase& Case = Add(16, TEXT("The same header at the top, nothing on it"),
				EVerdict::LocalLoss, 10, StandardCells, {}, Case16Falls, {},
				TEXT("superimposed load, against case 15"));

			Case.ProjectingCourse = 9;
		}

		/* E — bond pattern and head-joint shear. Both stand, and that is the finding. */

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

		/* F — losing the base, and the staircase void. */

		Add(19, TEXT("Bottom course out under half the wall"), EVerdict::Collapse,
			10, StandardCells, Case19Cuts, Case19Falls, Case19Stands, nullptr);

		Add(20, TEXT("Staircase void"), EVerdict::LocalLoss,
			CoveredCourses, 14, Case20Cuts, Case20Falls, {}, nullptr);

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
}

/**
 * THE ACCEPTANCE SET ITSELF: every case in the catalogue, its own verdict, one row at a time.
 *
 * THE ASSERTION IS ON OUTCOME AND ITS SHAPE FOLLOWS THE VERDICT — see the file header for why the
 * middle verdict exists and why displacement is not used anywhere. A failure names the case, the
 * variable its matched pair isolates, and both the expected and the actual fallen set in
 * (course, cell) terms, so the log says which bricks came down rather than how many.
 *
 * MOST OF THIS IS EXPECTED TO BE RED, AND THAT IS CORRECT. These are the acceptance criteria for
 * the arching work described in claude_plans/ARCHING_DESIGN.md, not a regression net over
 * something finished. The signature to look for is the one CURRENT_STATE.md predicts: every
 * falling half of a pair passing while every standing half fails, which is what a model that can
 * only ever say "falls" looks like.
 *
 * SIX ROWS ARE GREEN ON ARRIVAL AND EACH IS SAID TO BE, so nobody mistakes them for work this
 * suite drove. 1 and 17 are intact walls and are regression anchors. 18 is a stack-bond column
 * that really does sit at a few percent — its verdict passes and the property that made the case
 * interesting does NOT, which is why that property has a test of its own below. 12 passes because
 * everything falls and it names no survivors, recorded beside it. And 13 and 14 pass TOGETHER,
 * which is the one matched pair the solver already answers: a quarter-brick corbel step leaves the
 * end brick on two bed patches and reads 0.081, a half-brick step leaves it on one and takes the
 * four projecting bricks while the wall behind them stands. The moment work bought that pair.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
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

	/*
	 * THE PROFILE THIS FILE'S ARITHMETIC ASSUMES, CHECKED RATHER THAN IMPORTED. Every expected
	 * value below is a consequence of these three published figures; a retune of any of them
	 * should turn this row red rather than silently move every case in the catalogue.
	 */
	TestEqual(TEXT("FIXTURE: general purpose mortar's cohesion is EN 1996-1-1 Table 3.4's f_vk0"),
		GeneralPurposeMortar.ShearCohesionMPa, MortarShearCohesionMPa);

	TestEqual(TEXT("FIXTURE: general purpose mortar's flexural bond is Table 3.2's f_xk1"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.1);

	TestEqual(TEXT("FIXTURE: the fixture's brick density is ClayBrick's published one"),
		ClayBrick.DensityGramsPerCubicCm, ClayBrickDensityGramsPerCubicCm);

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty cases"), Cases.Num(), 20);

	for (const FWallCase& Case : Cases)
	{
		FWall Wall;
		FWallResult Result;

		RunWallCase(*this, Case, Wall, Result);

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
			/*
			 * BOTH HALVES, AND NEITHER IS DECORATION. "Nothing fell" alone passes for a wall that
			 * severed every joint over the cut and stayed leaning together; "no joint gave" alone
			 * passes for a wall whose pieces were never in the load path to begin with.
			 */
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

			break;
		}

		case EVerdict::LocalLoss:
		{
			/*
			 * IDENTITY, NOT A COUNT. The named set is what a bounded loss MEANS: the same number of
			 * bricks falling from somewhere else is a completely different — and wrong — answer,
			 * and a test that counted would call it a pass.
			 */
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
			/*
			 * TWO-SIDED, because a wall that comes down because EVERYTHING comes down says nothing
			 * about the term this row is meant to isolate. The named survivor region is what stops
			 * a model that always answers "falls" from passing every collapse row for free — and
			 * case 12 is the one row with no survivors to name, which is recorded beside it rather
			 * than papered over.
			 */
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

			break;
		}
		}
	}

	return true;
}

/**
 * THE FIVE MATCHED PAIRS, EACH AS ONE CLAIM ABOUT ONE VARIABLE.
 *
 * WHY THIS IS A SEPARATE TEST FROM THE CATALOGUE. The catalogue asks each row whether it got the
 * right answer; this asks whether the solver HAS THE TERM AT ALL, and the two can disagree in the
 * direction that matters. A model with no lateral compression path gets every falling half right
 * and every standing half wrong, and reading twenty individual failures is not the same as reading
 * "cover: both halves of 7 and 8 answered the same way". The pairing is the diagnostic and it
 * deserves to fail by name.
 *
 * THE CLAIM IS DELIBERATELY WEAKER THAN THE CATALOGUE'S. It asserts only that the standing half
 * loses NOTHING while the falling half loses SOMETHING — so it can still pass when the exact
 * fallen set is wrong, and it fails only when the variable is genuinely not modelled. That is what
 * makes it readable as a diagnosis rather than as a second copy of the same assertions.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceMatchedPairsTest,
	"DestructionGame.Acceptance.Wall.MatchedPairs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceMatchedPairsTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	struct FMatchedPair
	{
		const TCHAR* Variable;
		int32 StandingCase;
		int32 LosingCase;
	};

	/*
	 * FIVE PAIRS, AND IN EVERY ONE OF THEM EXACTLY ONE HALF STANDS. That is what makes the claim
	 * below writable as one line: the halves differ in outcome, so a solver that answers them the
	 * same way has no term between them, whichever way round it answers.
	 */
	const FMatchedPair Pairs[] =
	{
		{ TEXT("DEPTH OF COVER  - eight courses over a four-brick hole against one"),      7, 8 },
		{ TEXT("SPAN            - four bricks of opening against ten, at the same cover"), 7, 9 },
		{ TEXT("ABUTMENT        - a jamb either side against a cut through to the end"),   7, 10 },
		{ TEXT("PIER WIDTH      - three cells of bearing against one"),                    11, 12 },
		{ TEXT("CORBEL STEP     - a quarter brick per course against a half"),             13, 14 },
		{ TEXT("SUPERIMPOSED LOAD - six courses on the header's tail against none"),       15, 16 },
	};

	const TArray<FWallCase> Cases = AllWallCases();

	auto FellCount = [this, &Cases](int32 Number, bool& bOutRan) -> int32
	{
		for (const FWallCase& Case : Cases)
		{
			if (Case.Number != Number)
			{
				continue;
			}

			FWall Wall;
			FWallResult Result;

			RunWallCase(*this, Case, Wall, Result);
			bOutRan = Result.bLaid;

			return Result.Fallen.Num();
		}

		AddError(FString::Printf(TEXT("FIXTURE: the catalogue has no case %d"), Number));
		bOutRan = false;

		return 0;
	};

	for (const FMatchedPair& Pair : Pairs)
	{
		bool bStandingRan = false;
		bool bLosingRan = false;

		const int32 StandingFell = FellCount(Pair.StandingCase, bStandingRan);
		const int32 LosingFell = FellCount(Pair.LosingCase, bLosingRan);

		if (!bStandingRan || !bLosingRan)
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: case %d must stand and case %d must not, so the two halves have to ")
				TEXT("differ; case %d dropped %d piece(s) and case %d dropped %d"),
				Pair.Variable, Pair.StandingCase, Pair.LosingCase,
				Pair.StandingCase, StandingFell, Pair.LosingCase, LosingFell),
			StandingFell == 0 && LosingFell > 0);
	}

	return true;
}

/**
 * CASE 18, THE PART A VERDICT CANNOT SAY: A HANGING STACK-BOND COLUMN CARRIES THE SAME FRACTION OF
 * ITS HEAD JOINTS' SHEAR CAPACITY AT ANY HEIGHT.
 *
 * WHY THIS IS ITS OWN TEST. Case 18's verdict is "stands", and stands is easy — the column sits at
 * a few percent of capacity, so a wall that got the load path completely wrong would still pass
 * the catalogue row. The finding that overturned this case's drafted verdict was ARITHMETIC, and
 * the property that makes it trustworthy is that the ratio does not move with height: every course
 * added to the column brings one brick of weight and two more head joints, and those cancel
 * exactly. TWO HEIGHTS SAYING THE SAME NUMBER IS A FAR STRONGER CLAIM THAN ONE HEIGHT SAYING IT,
 * because a model that simply piles the whole column onto the joints at its foot also reads a
 * plausible few percent — and grows linearly with height, which one height cannot see.
 *
 * THE ARITHMETIC, WORKED INDEPENDENTLY. A full brick weighs 1.9 x 21.5 x 10.25 x 6.5 / 1000 kg
 * x 980 cm/s2 = 2667.198625 Unreal force units. In stack bond a brick whose bed joint has been cut
 * away is bonded to a neighbour on each side over a head joint of 10.25 x 6.5 = 66.625 cm2, and
 * the load is parallel to both, so it is shear, split by area between two equal joints:
 *
 *     2667.198625 / (2 x 66.625) / 10000 uu per MPa.cm2  =  0.00200165 MPa
 *     0.00200165 / 0.2 MPa (f_vk0)                       =  0.01000825
 *
 * There is no friction to add: a vertical load on a vertical joint puts no compression across it,
 * so the capacity is bare cohesion.
 *
 * AND THE NUMBER DISAGREES WITH THE ONE IN THE BRIEF, WHICH IS WORTH SAYING OUT LOUD. WALL_CASES
 * and CURRENT_STATE both quote 0.0200 for this case, reached by dividing the same load over the
 * same two joints by 0.1 MPa — but 0.1 is f_xk1, the FLEXURAL BOND strength, and the joint here is
 * in shear against f_vk0 = 0.2. The repo's own 0.0200165 (Tests/ConnectionStrengthTest.cpp) is a
 * different quantity again: one brick weight over ONE head joint, which is MOMENTS_DESIGN case
 * (b)'s fixture, and it coincides only because the two errors are each a factor of two. The value
 * asserted here is the one that follows from the published strengths this project actually ships.
 *
 * NEEDS A TICKING WORLD: NO.
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

	/*
	 * Slack against a re-association of a handful of doubles, not against a wrong answer. The
	 * value is about 0.01 and the model's answer today is expected to be several times it, so this
	 * is four orders of magnitude of headroom on the thing being distinguished.
	 */
	constexpr double UtilisationTolerance = 1e-9;

	/*
	 * TWO HEIGHTS, THE SAME CUT. Ten courses leaves four bricks hanging above the hole and sixteen
	 * leaves ten, so a model that concentrates the column on the joints at its foot reads two and a
	 * half times as much at the taller wall while the correct answer does not move at all.
	 */
	const int32 Heights[] = { 10, 16 };

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

		FWall Wall;
		FWallResult Result;

		RunWallCase(*this, Case, Wall, Result);
		ReportWallCase(*this, Case, Wall, Result);

		if (!Result.bLaid)
		{
			continue;
		}

		bRan[Index] = true;
		Measured[Index] = Result.Worst;

		/*
		 * THE COLUMN MUST ACTUALLY BE HANGING, or the number below is measuring an ordinary bed
		 * joint and the whole test is vacuous. The worst joint in a hanging stack-bond wall is a
		 * head joint: the bottom bed joint of a sixteen-course column reads about 0.0018 against
		 * 10 MPa, which is a fiftieth of what a head joint reads under the same load.
		 */
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

		TestTrue(
			*FString::Printf(
				TEXT("%d courses: the hanging column should read %.8g of head-joint shear ")
				TEXT("capacity, it reads %.8g"),
				Heights[Index], HangingColumnShearUtilisation, Result.Worst),
			FMath::IsNearlyEqual(Result.Worst, HangingColumnShearUtilisation, UtilisationTolerance));
	}

	if (bRan[0] && bRan[1])
	{
		/*
		 * THE PROPERTY, ASSERTED WITHOUT REFERENCE TO EITHER VALUE. This holds even if the
		 * absolute figure above is wrong, and it is the half that a model piling the column onto
		 * its foot cannot satisfy however it is tuned.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("HEIGHT-INDEPENDENT: %d courses reads %.8g and %d courses reads %.8g, a ")
				TEXT("factor of %.4g"),
				Heights[0], Measured[0], Heights[1], Measured[1],
				Measured[0] > 0.0 ? Measured[1] / Measured[0] : 0.0),
			FMath::IsNearlyEqual(Measured[0], Measured[1], UtilisationTolerance));
	}

	return true;
}

/**
 * THE FIXTURE'S OWN BRICKLAYER LAYS THE WALL Layout::RunningBond LAYS.
 *
 * WITHOUT THIS THE WHOLE FILE IS A SECOND DEFINITION OF WHAT A WALL IS. Nineteen expected outcomes
 * measured against a wall that is subtly not the game's wall would all be measuring the wrong
 * thing, plausibly, and every one of them would still read as a number. The fixture exists only
 * because six cases are not running-bond rectangles; on the shape both producers CAN lay they have
 * to agree brick for brick.
 *
 * ON BOXES RATHER THAN ON HANDLES, because the order a producer emits pieces in is its own
 * business and this makes no claim about it — the sets of boxes have to match, and each box has to
 * match somewhere.
 *
 * NEEDS A TICKING WORLD: NO.
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

	/** Exact arithmetic on both sides; this is slack against a re-association and nothing else. */
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

#endif // WITH_DEV_AUTOMATION_TESTS
