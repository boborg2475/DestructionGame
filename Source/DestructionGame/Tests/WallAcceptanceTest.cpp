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
 * THREE OF THE FIVE ARE OUTCOME PAIRS; 13 vs 14 AND 15 vs 16 ARE NOT, SINCE THE 2026-08-07
 * REVISIONS. Both corbels stand and both headers stand, so an outcome pair between either two can
 * no longer separate anything, and both have been removed from FWallAcceptanceMatchedPairsTest
 * rather than left there as rows that read the right answer off two identical verdicts. WHAT THE
 * PAIRS STILL ISOLATE IS THE READING, and each is asserted by name in a test of its own below:
 *
 *   13 vs 14  Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome — doubling the step
 *             per course takes the worst joint from 0.070 to 0.195, a factor of 2.8.
 *   15 vs 16  Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome — the SAME joint of
 *             the SAME geometry reads 0.0018 with six courses on the header's tail and 0.0582 with
 *             nothing on it, a factor of 32.
 *
 * In both, the term is still being measured — just against a quantity a verdict cannot carry.
 * Deleting either pair outright would have lost that.
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

	/** EN 1996-1-1 Table 3.2's f_xk1 for general purpose mortar, asserted against the profile. */
	constexpr double MortarFlexuralBondMPa = 0.1;

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
	 * Case 8's one course of cover, and the only thing that may drop: the two MIDDLE bricks.
	 *
	 * WORKED OFF THE GEOMETRY RATHER THAN OFF THE DRAWING, the same way case 20's teeth are. The
	 * opening cuts cells 4.5, 5.5 and 6.5 out of odd course 3, and an even-course brick at cell k
	 * sits on the odd course below at cells k - 0.5 and k + 0.5, so of the four bricks of course 4
	 * standing over the hole
	 *
	 *     cell 4   over cut 4.5 and INTACT 3.5   one bed patch — a corbel
	 *     cell 5   over cut 4.5 and cut 5.5      NO bed patch at all
	 *     cell 6   over cut 5.5 and cut 6.5      NO bed patch at all
	 *     cell 7   over cut 6.5 and INTACT 7.5   one bed patch — a corbel
	 *
	 * NAMING ALL FOUR WAS DRIFT AND IT IS CORRECTED HERE. This row was written against the
	 * catalogue's first draft — "a single course cannot arch, it is a beam in flexure over four
	 * bricks" — and WALL_CASES.html has since been corrected on exactly the reading case 20 was
	 * corrected on: the two over the jambs keep one patch each and hold at about 0.27, and ONE
	 * PATCH IS A CORBEL RATHER THAN A TOOTH. The agreed catalogue names two, so this names two.
	 *
	 * THE ROW IS STILL RED AFTER THE CORRECTION, and red for the reason it was always meant to be:
	 * the model drops NOTHING here. Slice 2 took it from 2 dropped to 0 by re-seating the seatless
	 * pair through the head joints of the bricks either side, and a single spanning course flexing
	 * over four bricks is a claim about those head joints that no slice yet makes.
	 */
	const FWallRegion Case8Falls[] = { { 4, 4, 4.75, 6.25 } };

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

	/* ================================================================================
	 * CASE 14, RE-DERIVED: A BONDED FOUR-STEP CORBEL STANDS AT 0.19516 AND NOTHING COMES DOWN.
	 * ================================================================================
	 *
	 * THE VERDICT ON THIS ROW WAS COLLAPSE UNTIL 2026-08-07 AND THE USER RULED IT WRONG. The
	 * catalogue's reasoning was a RIGID-BODY OVERTURNING reading — "the resultant walks outside the
	 * bed below" — and that reading is geometrically true here and stated below because it is worth
	 * knowing which claim was abandoned. What overrules it is that this project models an UNCRACKED
	 * BONDED SECTION, and the same user had already ruled, on 2026-08-06, that a brick deleted at a
	 * free end must not bring a wall down. Any rule local enough to honour that also honours this
	 * corbel: `Core.Structure.ACorbelResistsWithItsWholeDepth` asserts a FIVE-step raking corbel
	 * standing at 0.219, and a five-step corbel and a four-step one are the same fixture with a
	 * different provenance — one cut that way, one laid that way. A threshold sited between them
	 * would be a number tuned to make two contradictory statements both come out true.
	 *
	 * WHAT THE FIXTURE ACTUALLY PRESENTS, WALKED OFF THE BRICKLAYER ABOVE RATHER THAN ASSUMED.
	 * CourseGeometry pushes each course from 6 upward one more half cell (11.25 cm) out and closes
	 * it with a FULL brick, so the end brick of course c spans
	 *
	 *     course 6   158.00 .. 179.50      course 8   180.50 .. 202.00
	 *     course 7   169.25 .. 190.75      course 9   191.75 .. 213.25
	 *
	 * and the end brick of the flush course 5 below is the odd course's HALF BAT at 158.00..168.25.
	 * Every one of the four therefore overlaps exactly ONE piece below it, over exactly 10.25 cm —
	 * the same square half seat the raking staircase corbel presents — with its centre of mass
	 * 5.625 cm outboard of that patch's centroid. THAT is what makes this the staircase's fixture:
	 * four steps of it instead of eleven. (It is also the overturning claim: the mass acts 5.625 cm
	 * out on a patch only 5.125 cm wide, so the resultant IS outside the bearing. Bond carries it.)
	 *
	 * THE LADDER, AND IT IS THE STAIRCASE LADDER BECAUSE IT IS THE STAIRCASE TOPOLOGY. Each step
	 * takes its own weight, ALL of the step above (which has nowhere else to go) and half of the
	 * next brick along, whose own share grows the same way; the moment carries across the 11.25 cm
	 * the corbel has stepped out. Written for s steps below the top and asserted, not imported.
	 *
	 * AND THE SECTION IS THE DEPTH OF BONDED MASONRY STANDING OVER THE JOINT, W = t*D^2/6, taken at
	 * the LESSER of that and the bed patch's own 179.4817708 cm3 — the top step has one course over
	 * it, D = 7.5 cm, W = 96.09 cm3, SHALLOWER than the patch, so the patch governs there and the
	 * step keeps the 0.058203838191552663 that `AdoptedWallLoadsItsWaistEccentrically` pins:
	 *
	 *     course   s   F (weights)   M (weight.cm)   courses over   reads
	 *        9     0        1             5.625            1        0.058203838   (patch governs)
	 *        8     1        2.5          22.5              2        0.156128700
	 *        7     2        4.5          56.25             3        0.173476333
	 *        6     3        7           112.5              4        0.195160875   <- the worst
	 *
	 * 0.195 OF f_xk1 IS A CORBEL STANDING AT A FIFTH OF WHAT HOLDS IT, so the verdict follows the
	 * number rather than the other way round: STANDS, with no fall region and no survivor region to
	 * name, because nothing comes down. The arithmetic is asserted in its own test below rather
	 * than left as a comment, since a verdict of "stands" is satisfied by 0.195 and by 0.0001 alike
	 * and only one of them is this fixture.
	 *
	 * CROSS-CHECKED AGAINST TWO FIGURES THIS FILE DID NOT PRODUCE. The same four lines of
	 * arithmetic give 0.21858 for a five-step corbel and 0.36903147272727271 for the eleven-step
	 * staircase — ARCHING_DESIGN.md's published 0.219 and the anchor
	 * `Core.Structure.AStaircaseVoidCondemnsTheCorbel` pins to seventeen digits. Both are asserted
	 * below as loose cross-checks, so a derivation that drifted fails HERE rather than agreeing
	 * with itself.
	 */

	/** What one step of a raking corbel carries, in brick weights, s steps below the top. */
	constexpr double CorbelLadderForceBrickWeights(int32 StepsBelowTop)
	{
		return 1.0 + StepsBelowTop + StepsBelowTop * (StepsBelowTop + 1) / 4.0;
	}

	/** What bends it, in brick-weight-centimetres: its own 5.625 cm arm plus everything above. */
	constexpr double CorbelLadderMomentBrickWeightCm(int32 StepsBelowTop)
	{
		const double S = StepsBelowTop;

		const double SumOfForces = S + S * (S - 1.0) / 2.0 + (S - 1.0) * S * (S + 1.0) / 12.0;

		return (HalfCellCm * 0.5) * (S + 1.0) + HalfCellCm * SumOfForces;
	}

	/** The square half seat a corbelled end brick keeps, and its own section modulus, cm2 and cm3. */
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

	/** Pure bending on the deep-beam section: W = t * D^2 / 6 through the masonry standing over it. */
	double CompositeTensionMPa(double MomentBrickWeightCm, int32 CoursesOfDepth)
	{
		const double DepthCm = CoursesOfDepth * CoursePitchCm;

		const double ModulusCm3 = BrickDepthCm * DepthCm * DepthCm / 6.0;

		return MomentBrickWeightCm * FullBrickWeightUu / (ModulusCm3 * ForceUnitsPerMPaSqCmHere);
	}

	/**
	 * What the bottom rung of a k-step bonded corbel reads, as a fraction of f_xk1.
	 *
	 * The bottom rung is k - 1 steps below the top and has k courses of masonry over its bed joint,
	 * so k is the only argument. The `min` is what keeps the model nested: composite action is an
	 * ALTERNATIVE way of carrying the moment rather than an extra one, so it may only ever help.
	 */
	double CorbelBottomRungUtilisation(int32 Steps)
	{
		const double MomentBrickWeightCm = CorbelLadderMomentBrickWeightCm(Steps - 1);
		const double ForceBrickWeights = CorbelLadderForceBrickWeights(Steps - 1);

		return FMath::Min(
			HalfSeatTensionMPa(MomentBrickWeightCm, ForceBrickWeights),
			CompositeTensionMPa(MomentBrickWeightCm, Steps)) / MortarFlexuralBondMPa;
	}

	/* ================================================================================
	 * CASE 16, RE-DERIVED: A BONDED HEADER WITH NOTHING ON IT STANDS AT 0.058204.
	 * ================================================================================
	 *
	 * THE VERDICT ON THIS ROW WAS LOCAL LOSS UNTIL 2026-08-07 AND IT WAS REVISED IN THE CATALOGUE
	 * WITHOUT THIS FILE FOLLOWING. That is the drift the catalogue exists to prevent, and it is
	 * corrected here by re-deriving the number rather than by flipping the word.
	 *
	 * The header projects half a cell (11.25 cm) past the face, so of its 21.5 cm it keeps
	 * 21.5 - 11.25 = 10.25 cm of bearing on the brick below, and its centre of mass sits half the
	 * unseated length — 5.625 cm — outboard of that patch's centroid. On a 10.25 x 10.25 cm patch,
	 * whose section modulus about the bending axis is 10.25 x 10.25^2 / 6 = 179.4817708 cm3:
	 *
	 *     2667.198625 x 5.625 / 179.4817708 = 0.0083591 MPa   bending, opening the outer edge
	 *     2667.198625 / 105.0625            = 0.0025387 MPa   its own weight, closing it
	 *     tension 0.0058204 / 0.1 (f_xk1)   = 0.058204        of flexural bond capacity
	 *
	 * — a brick standing at six percent of what holds it, not a brick falling off. The abandoned
	 * "local loss" was a RIGID-BODY OVERTURNING reading (the resultant is 5.625 cm out on a patch
	 * only 5.125 cm wide, so it does lie outside the bearing) and it is the same error case 14 was
	 * corrected for on the same day: this project models an UNCRACKED BONDED SECTION, where cured
	 * mortar carries exactly that. The overturning claim is written down here because it is worth
	 * knowing which reading was abandoned, not because it is still live.
	 *
	 * SO THERE IS NO FALL REGION TO NAME, and the row carries no MustFall and no MustStand.
	 *
	 * AND THE PAIR HAD TO MOVE WITH IT. 15 and 16 differ ONLY in what sits on the header's tail,
	 * and both now stand — so as an OUTCOME pair they discriminate nothing, exactly as 13 and 14
	 * stopped doing. WHAT STILL SEPARATES THEM IS THE READING ON ONE JOINT: the model reads
	 * 0.00184 for case 15 against 0.05820 for case 16, a factor of 32 from superimposed load alone,
	 * with case 15's tension driven to zero and compression left governing. That is asserted in
	 * Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome below, and the row has been
	 * removed from FWallAcceptanceMatchedPairsTest rather than left there unsatisfiable.
	 */

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
				10, CorbelCells, {}, {}, {},
				TEXT("corbel projection against case 14 — IN THE READING, not the outcome"));

			Case.CorbelFromCourse = CorbelFirstCourse;
			Case.CorbelStepCm = QuarterBrickStepCm;
		}

		/*
		 * STANDS, ON THE USER'S RULING OF 2026-08-07 AND ON THE ARITHMETIC ABOVE. Drafted as a
		 * collapse taking the four projecting bricks; the bottom rung reads 0.195160875 of f_xk1
		 * and nothing in the ladder is near 1.0. No fall region and no survivor region, because
		 * there is nothing to name — see the block above section D for the whole derivation and
		 * for what the abandoned overturning reading claimed instead.
		 */
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

		/*
		 * STANDS, ON THE CATALOGUE'S 2026-08-07 REVISION AND ON THE ARITHMETIC ABOVE. Drafted as a
		 * local loss taking the projecting header; the header's own bed joint reads 0.058203838 of
		 * f_xk1, which is a sixth of what holds it. No fall region and no survivor region, because
		 * there is nothing to name — see the block above section E for the whole derivation and for
		 * what the abandoned overturning reading claimed instead.
		 */
		{
			FWallCase& Case = Add(16, TEXT("The same header at the top, nothing on it"),
				EVerdict::Stands, 10, StandardCells, {}, {}, {},
				TEXT("superimposed load against case 15 — IN THE READING, not the outcome"));

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

	/* ================================================================================
	 * THE TWENTY AS PLAYABLE LEVELS — what a scenario row for one of these cases must be.
	 * ================================================================================
	 *
	 * These twenty configurations are the ones the user drew and reviewed, and until now the only
	 * thing that could lay them was the fixture bricklayer above — which lives in a test file, so
	 * no level could ever reach it. The three tests at the bottom of this file are the acceptance
	 * criteria for moving the GEOMETRY into production and leaving the VERDICTS here.
	 *
	 * THE VERDICTS DO NOT MOVE, and that is not a detail of the sequencing. `EVerdict`, `MustFall`,
	 * `MustStand` and `Isolates` are claims about what the solver ought to conclude; they are
	 * acceptance-test property. A level needs the geometry and the cuts and nothing else, and
	 * moving an expected outcome into production would be putting an assertion there.
	 */

	/**
	 * WHAT A CASE'S LEVEL IS CALLED, AND WHY IT IS THE NUMBER RATHER THAN THE TITLE.
	 *
	 * A scenario name is typed on a URL and a map name becomes a filename on disk, so both want to
	 * be short, stable and free of punctuation; a case title is prose with commas and hyphens in it
	 * and is carried VERBATIM in the row's own `Title` instead. Two digits so `wall-2` cannot sort
	 * or read as `wall-20`.
	 */
	FString LevelNameForCase(int32 Number)
	{
		return FString::Printf(TEXT("wall-%02d"), Number);
	}

	FString LevelMapNameForCase(int32 Number)
	{
		return FString::Printf(TEXT("Lvl_Wall%02d"), Number);
	}

	/**
	 * THE ONE MACHINE-CHECKABLE CLAIM A LEVEL'S CAPTION MAKES.
	 *
	 * A caption is prose and has to stay prose — it is the only thing telling a player what they
	 * are looking at. What cannot be prose is the VERDICT it claims, because a level captioned with
	 * an outcome the solver does not produce is a lie told to somebody standing in front of the
	 * counter-example. So the caption carries exactly one token naming its verdict, spelled the way
	 * this file spells verdicts, and the rest of the sentence is the writer's.
	 */
	FString VerdictClaimFor(EVerdict Verdict)
	{
		return FString::Printf(TEXT("Expected: %s"), VerdictName(Verdict));
	}

	/**
	 * AND THE TOKEN A CAPTION MUST CARRY WHEN THE MODEL DOES NOT AGREE WITH ITS OWN CLAIM.
	 *
	 * Six rows are red today. A caption that named only the expected verdict on one of those would
	 * be worse than silence — so the honest ones say both, and the ones the model agrees with must
	 * NOT say it, which is what stops the admission outliving the disagreement.
	 */
	const TCHAR* const ModelDisagreesMarker = TEXT("THE MODEL CURRENTLY DISAGREES");

	/**
	 * Whether the model produced the verdict this row claims.
	 *
	 * DELIBERATELY THE SAME THREE SHAPES `Acceptance.Wall.Catalogue` ASSERTS ONE AT A TIME, and it
	 * is a second reading of them rather than a shared one because the catalogue's assertions are
	 * SEPARATE on purpose — each prints its own diagnosis — and folding them into one predicate
	 * would change six known-red failure messages. The cost is a copy, and the copy is held against
	 * the catalogue by the known-red tripwire in the caption test: it must name exactly the six
	 * rows that are red, so a predicate that drifted from the catalogue fails there rather than
	 * quietly captioning a level wrongly.
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

			if (MustFall.Num() == 0)
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

	/** The production spec that must lay this case's wall, transcribed field for field. */
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

	/** The same rectangles, in production's own vocabulary. */
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
	 * Compare a laid layout against the fixture's wall, BRICK FOR BRICK AND IN HANDLE ORDER.
	 *
	 * HANDLE ORDER IS PART OF THE CLAIM AND IT IS THE HALF THAT LOOKS FINE WHEN IT IS WRONG. A
	 * builder that lays the same bricks in a different sequence renumbers every joint and every
	 * break stamp while every geometric check still passes — and every reading in this file, in
	 * `Core.Structure` and in the design documents is a statement about a particular joint index.
	 *
	 * THE FIRST DISAGREEMENT IS REPORTED, NOT ALL OF THEM. A wall is up to 375 pieces and 1,000
	 * joints; a producer that moved every brick would otherwise print thousands of failures and
	 * bury everything else in the run.
	 *
	 * @return true if they are the same wall.
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
 * SEVERAL ROWS ARE GREEN ON ARRIVAL AND EACH IS SAID TO BE, so nobody mistakes them for work this
 * suite drove. 1 and 17 are intact walls and are regression anchors. 18 is a stack-bond column
 * that really does sit at a few percent — its verdict passes and the property that made the case
 * interesting does NOT, which is why that property has a test of its own below. 12 passes because
 * everything falls and it names no survivors, recorded beside it. And 13, 14 and 16 all stand,
 * which is a CORRECTION rather than an achievement: 14 was drafted as a collapse and 16 as a local
 * loss, and both were revised in the catalogue on 2026-08-07 on the same reading — a bonded
 * section resists what a rigid block could not. Both of those pairs therefore stopped separating
 * on outcome, so what each isolates has moved out of the pair test and into a test of its own,
 * named in the block above.
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
		GeneralPurposeMortar.TensileStrengthMPa, MortarFlexuralBondMPa);

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
 * THE MATCHED PAIRS THAT STILL SEPARATE ON OUTCOME, EACH AS ONE CLAIM ABOUT ONE VARIABLE.
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
	 * FOUR PAIRS, AND IN EVERY ONE OF THEM EXACTLY ONE HALF STANDS. That is what makes the claim
	 * below writable as one line: the halves differ in outcome, so a solver that answers them the
	 * same way has no term between them, whichever way round it answers.
	 *
	 * TWO OF THE CATALOGUE'S FIVE PAIRS ARE DELIBERATELY ABSENT AND THIS IS THE ONLY PLACE TO SAY
	 * SO. Both were rows here until 2026-08-07, when case 14's collapse verdict and case 16's
	 * local-loss verdict were each revised to "stands" — a bonded corbel resists with its full
	 * depth, and a bonded header with nothing on it reads a sixth of f_xk1. Both halves of both
	 * pairs now stand, so the shape this test asserts — one half loses nothing, the other loses
	 * something — is UNSATISFIABLE for either by construction, and a row asserting it would be red
	 * forever for a reason nobody could fix. Neither has been dropped; both have MOVED:
	 *
	 *     13 vs 14   Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome
	 *     15 vs 16   Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome
	 *
	 * Each makes the same claim about the same variable against the quantity that still separates
	 * its two halves, which is the joint reading. Leaving dead rows here would have been quieter
	 * and worse.
	 */
	const FMatchedPair Pairs[] =
	{
		{ TEXT("DEPTH OF COVER  - eight courses over a four-brick hole against one"),      7, 8 },
		{ TEXT("SPAN            - four bricks of opening against ten, at the same cover"), 7, 9 },
		{ TEXT("ABUTMENT        - a jamb either side against a cut through to the end"),   7, 10 },
		{ TEXT("PIER WIDTH      - three cells of bearing against one"),                    11, 12 },
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
 * CASES 13 AND 14, THE PART A VERDICT CANNOT SAY: DOUBLING THE CORBEL STEP DOUBLES THE JOINT
 * READING, AND BOTH CORBELS STAND ANYWAY.
 *
 * WHY THIS TEST EXISTS AT ALL. 13 and 14 were an OUTCOME pair — one stands, one collapses — until
 * the user ruled on 2026-08-07 that a bonded corbel resists with its full depth and case 14's
 * collapse verdict was wrong. Both halves now stand. A pair whose two halves answer identically
 * discriminates NOTHING, and the honest options were to delete it or to move it onto a quantity
 * that still separates the two. This is that move, and it is the stronger of the two readings: an
 * outcome pair could only ever say "these differ", while this says BY HOW MUCH AND WHY.
 *
 * THE EXPECTED VALUE IS DERIVED FROM THE FIXTURE, NOT READ BACK OFF THE SOLVER. Case 14's four
 * corbelled courses each keep one 10.25 x 10.25 bed patch with their mass 5.625 cm outboard of it,
 * so the load ladder, the moment ladder and the deep-beam section are all written above from the
 * grid, the published density and the published f_xk1 — none of them imported from production.
 * The whole point of the row is that a wrong constant makes this DISAGREE.
 *
 * AND THE DERIVATION IS CROSS-CHECKED AGAINST TWO FIGURES THIS FILE DID NOT PRODUCE, which is what
 * stops it agreeing with itself. The same four lines give 0.21858 at five steps — ARCHING_DESIGN's
 * published 0.219 — and 0.36903147272727271 at eleven, which is the staircase anchor
 * `Core.Structure.AStaircaseVoidCondemnsTheCorbel` pins to seventeen digits. Those are held at 2%
 * and at 1e-9 respectively, IN THE TEST, so a re-derivation that drifted fails here rather than
 * being quietly tuned into agreement.
 *
 * THE THING BEING MEASURED MUST BE THE THING GOVERNING, WHICH IS ASSERTED SEPARATELY. `Worst` is
 * the worst joint ANYWHERE in the wall, and a ten-course wall's base compression or a head joint
 * could in principle carry it; then the number would be right for the wrong joint. So the worst
 * joint is asserted to be the bed joint under the LOWEST corbelled course — course 5's half bat at
 * cell 7.25 carrying course 6's projecting brick at cell 7.5 — before its magnitude is read.
 *
 * GREEN ON ARRIVAL, AND SAID SO PLAINLY. This pins behaviour arching slice 5 already produces; it
 * drove nothing. What it is for is that the ruling above deleted an assertion, and an assertion
 * deleted without one put back in its place is a hole in the net.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceCorbelProjectionTest,
	"DestructionGame.Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceCorbelProjectionTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/*
	 * SLACK AGAINST A RE-ASSOCIATION OF A HANDFUL OF DOUBLES AND NOTHING ELSE. One course of depth
	 * either way moves the four-step reading from 0.1735 to 0.2186, so this is seven orders of
	 * magnitude below the smallest difference the row is meant to be able to see.
	 */
	constexpr double UtilisationTolerance = 1.0e-9;

	/* --- the derivation, cross-checked against two figures produced elsewhere ---------------- */

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: five steps should reproduce ARCHING_DESIGN's published 0.219, the ")
			TEXT("closed form gives %.8g"),
			CorbelBottomRungUtilisation(5)),
		FMath::IsNearlyEqual(CorbelBottomRungUtilisation(5), 0.219, 0.02 * 0.219));

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: eleven steps should reproduce the staircase anchor 0.36903147272727271, ")
			TEXT("the closed form gives %.17g"),
			CorbelBottomRungUtilisation(11)),
		FMath::IsNearlyEqual(
			CorbelBottomRungUtilisation(11), 0.36903147272727271, UtilisationTolerance));

	/* --- the two walls -------------------------------------------------------------------- */

	const TArray<FWallCase> Cases = AllWallCases();

	auto RunNumbered = [this, &Cases](int32 Number, FWall& OutWall, FWallResult& OutResult) -> bool
	{
		for (const FWallCase& Case : Cases)
		{
			if (Case.Number != Number)
			{
				continue;
			}

			RunWallCase(*this, Case, OutWall, OutResult);
			ReportWallCase(*this, Case, OutWall, OutResult);

			return OutResult.bLaid;
		}

		AddError(FString::Printf(TEXT("FIXTURE: the catalogue has no case %d"), Number));

		return false;
	};

	FWall QuarterWall;
	FWallResult QuarterResult;

	FWall HalfWall;
	FWallResult HalfResult;

	if (!RunNumbered(13, QuarterWall, QuarterResult) || !RunNumbered(14, HalfWall, HalfResult))
	{
		return true;
	}

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

	/*
	 * AND THE OUTCOME FOLLOWS FROM THE NUMBER RATHER THAN THE OTHER WAY ROUND. 0.195 is a fifth of
	 * capacity; the verdict in the catalogue is "stands" BECAUSE of this, not beside it.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("and that is why the verdict is STANDS: the worst joint in the wall is %.6g of ")
			TEXT("capacity and must be under 1"),
			HalfResult.Worst),
		HalfResult.Worst < 1.0);

	/* --- the pair's own claim: projection is still measured --------------------------------- */

	TestTrue(
		*FString::Printf(
			TEXT("CORBEL STEP: doubling the projection per course must read HARDER on the joint — a ")
			TEXT("quarter brick reads %.8g and a half brick reads %.8g, a factor of %.4g. A model ")
			TEXT("with no projection term reads them the same."),
			QuarterResult.Worst, HalfResult.Worst,
			QuarterResult.Worst > 0.0 ? HalfResult.Worst / QuarterResult.Worst : 0.0),
		HalfResult.Worst > QuarterResult.Worst);

	/*
	 * TWICE, NOT MERELY MORE. A strict inequality alone is satisfiable by a last-bit difference,
	 * which is not a projection term working; the step doubles, the arm doubles with it, and the
	 * measured separation is a factor of 2.8. Two is the floor and it is a long way under that.
	 */
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
 * CASES 15 AND 16, THE PART A VERDICT CANNOT SAY: PUTTING SIX COURSES ON A HEADER'S TAIL DIVIDES
 * WHAT ITS OWN BED JOINT READS BY THIRTY, AND BOTH HEADERS STAND ANYWAY.
 *
 * WHY THIS TEST EXISTS AT ALL. 15 and 16 were an OUTCOME pair — one stands, one drops its header —
 * until the catalogue was revised on 2026-08-07 and case 16 became "stands" too: an uncracked
 * bonded bed joint carries a half-cell projection at 0.0582 of f_xk1, and the local-loss verdict it
 * replaced was a rigid-body overturning reading that ignored the bond. That is the same correction
 * case 14 got on the same day, and it has the same consequence — a pair whose two halves answer
 * identically discriminates NOTHING. The honest options were to delete the pair or to move it onto
 * a quantity that still separates the two. This is that move, and it is the stronger reading: an
 * outcome pair could only ever have said "these differ", while this says BY HOW MUCH AND WHY.
 *
 * THE TWO WALLS ARE THE SAME WALL WITH THE HEADER AT A DIFFERENT HEIGHT — ten courses, twelve
 * cells, one course pushed half a cell out and closed with a full brick instead of a half bat. In
 * case 15 that course is course 3, so six courses stand on the header's tail; in case 16 it is
 * course 9, the top, so nothing does. THE GEOMETRY OF THE HEADER'S OWN BED JOINT IS IDENTICAL in
 * the two, which is what makes this a one-variable comparison: same 10.25 x 10.25 cm bearing patch,
 * same 5.625 cm arm, same brick. Only the superimposed load differs.
 *
 * THE EXPECTED VALUE IS DERIVED FROM THE FIXTURE, NOT READ BACK OFF THE SOLVER. The seat length is
 * what is left of a 21.5 cm brick when half a 22.5 cm cell hangs over air; the arm is half of what
 * hangs over; the section is that patch's own t.L^2/6; the weight is the published density times
 * the published dimensions times Unreal's gravity; and the capacity is EN 1996-1-1's f_xk1. None of
 * them is imported from production, so a wrong constant there makes this DISAGREE.
 *
 * AND IT IS CROSS-CHECKED AGAINST TWO FIGURES THIS FILE DID NOT PRODUCE, which is what stops it
 * agreeing with itself: WALL_CASES.html's own quoted 0.058204 for this case, held at 1e-6 so the
 * catalogue's six figures are enough and a real error is not; and the waist anchor
 * 0.058203838191552663 that `Core.Structure.AdoptedWallLoadsItsWaistEccentrically` pins to
 * seventeen digits on a completely different fixture — one brick, half seated, by cut rather than
 * by laying. A re-derivation that drifted fails HERE rather than being tuned into agreement.
 *
 * THE THING BEING MEASURED MUST BE THE THING GOVERNING, WHICH IS ASSERTED SEPARATELY. `Worst` is
 * the worst joint ANYWHERE in the wall, and a ten-course wall's base compression could in principle
 * carry it — case 17's intact stack-bond wall of the same height reads 0.00109 at its foot — so the
 * number would then be right for the wrong joint. Case 16's worst joint is therefore asserted to BE
 * the header's own bed joint, by (course, cell), before its magnitude is read.
 *
 * CASE 15's GOVERNING JOINT IS DELIBERATELY *NOT* PINNED, and that is not laziness. What is claimed
 * of case 15 is an UPPER bound — the worst joint anywhere in that wall is at most a tenth of case
 * 16's — and an upper bound over every joint says something strictly stronger about the header's
 * own joint than pinning where the maximum happens to sit. It also stays true if a later slice
 * drives the header's tension further down and the maximum migrates to the foot of the wall, which
 * pinning the identity would turn into a spurious failure. (It reads 0.00184 at c2/11-c3/11.5
 * today, which is the header's bed joint, and the failure message prints where it actually is.)
 *
 * GREEN ON ARRIVAL, AND SAID SO PLAINLY. This pins behaviour the model already produces; it drove
 * nothing. What it is for is that the case 16 revision deleted an assertion, and an assertion
 * deleted without one put back in its place is a hole in the net.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceSuperimposedLoadTest,
	"DestructionGame.Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceSuperimposedLoadTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/*
	 * SLACK AGAINST A RE-ASSOCIATION OF A HANDFUL OF DOUBLES AND NOTHING ELSE. The quantity is
	 * 0.058, so this is seven orders of magnitude below it and eight below the factor of thirty the
	 * row is meant to be able to see.
	 */
	constexpr double UtilisationTolerance = 1.0e-9;

	/* --- the derivation, worked forward from the grid ---------------------------------------- */

	/*
	 * WHAT THE HEADER KEEPS AND WHAT HANGS OVER. CourseGeometry pushes the projecting course's
	 * right face out by HALF A CELL and closes it with a FULL brick rather than the half bat a
	 * flush odd course closes with, so of the brick's 21.5 cm exactly 11.25 cm is over air and
	 * 10.25 cm is still bearing on the course below. (That it lands on the same 10.25 cm as the
	 * wall's depth is a coincidence of this brick's proportions, not a relation — it is written as
	 * a length along the wall because that is what it is.)
	 */
	constexpr double HeaderSeatLengthCm = BrickLengthCm - HalfCellCm;

	/* The mass sits at the middle of the brick, so it is half the unseated part outboard. */
	constexpr double HeaderArmCm = (BrickLengthCm - HeaderSeatLengthCm) * 0.5;

	constexpr double HeaderSeatAreaSqCm = HeaderSeatLengthCm * BrickDepthCm;

	/* W = t.L^2/6 about the axis the header bends about: through the wall, along its length. */
	constexpr double HeaderSeatModulusCm3 =
		BrickDepthCm * HeaderSeatLengthCm * HeaderSeatLengthCm / 6.0;

	const double BendingMPa = FullBrickWeightUu * HeaderArmCm
		/ (HeaderSeatModulusCm3 * ForceUnitsPerMPaSqCmHere);

	const double OwnWeightMPa = FullBrickWeightUu
		/ (HeaderSeatAreaSqCm * ForceUnitsPerMPaSqCmHere);

	/* Its own weight CLOSES the joint the bending opens, so the two subtract. */
	const double ExpectedUnloaded =
		FMath::Max(0.0, BendingMPa - OwnWeightMPa) / MortarFlexuralBondMPa;

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: WALL_CASES.html quotes 0.058204 for case 16, the derivation gives ")
			TEXT("%.8g"),
			ExpectedUnloaded),
		FMath::IsNearlyEqual(ExpectedUnloaded, 0.058204, 1.0e-6));

	TestTrue(
		*FString::Printf(
			TEXT("CROSS-CHECK: the same arithmetic on a CUT half-seated brick is the waist anchor ")
			TEXT("0.058203838191552663, the derivation gives %.17g"),
			ExpectedUnloaded),
		FMath::IsNearlyEqual(ExpectedUnloaded, 0.058203838191552663, UtilisationTolerance));

	/* --- the two walls ---------------------------------------------------------------------- */

	const TArray<FWallCase> Cases = AllWallCases();

	auto RunNumbered = [this, &Cases](int32 Number, FWall& OutWall, FWallResult& OutResult) -> bool
	{
		for (const FWallCase& Case : Cases)
		{
			if (Case.Number != Number)
			{
				continue;
			}

			RunWallCase(*this, Case, OutWall, OutResult);
			ReportWallCase(*this, Case, OutWall, OutResult);

			return OutResult.bLaid;
		}

		AddError(FString::Printf(TEXT("FIXTURE: the catalogue has no case %d"), Number));

		return false;
	};

	FWall LoadedWall;
	FWallResult LoadedResult;

	FWall BareWall;
	FWallResult BareResult;

	if (!RunNumbered(15, LoadedWall, LoadedResult) || !RunNumbered(16, BareWall, BareResult))
	{
		return true;
	}

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

	/*
	 * WHERE THE HEADER AND ITS SEAT ARE, IN CELLS, WALKED OFF THE BRICKLAYER RATHER THAN COUNTED
	 * OFF THE LOG. The projecting course's rightmost brick is a full brick whose right face is half
	 * a cell past the flush face; the course below it is flush and closes with a full brick too,
	 * since it is even. Both centres are half a brick in from their own right faces.
	 */
	const double HeaderCentreCm =
		FlushRightFaceCm(StandardCells) + HalfCellCm - BrickLengthCm * 0.5;

	const double SeatCentreCm = FlushRightFaceCm(StandardCells) - BrickLengthCm * 0.5;

	const double HeaderCell = HeaderCentreCm / CellPitchCm;
	const double SeatCell = SeatCentreCm / CellPitchCm;

	/* Case 16 pushes course 9 out, so its seat is the end brick of course 8. */
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

	/*
	 * AND THE OUTCOME FOLLOWS FROM THE NUMBER RATHER THAN THE OTHER WAY ROUND. 0.058 is a sixth of
	 * capacity; the verdict in the catalogue is "stands" BECAUSE of this, not beside it.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("and that is why the verdict is STANDS: the worst joint in the wall is %.6g of ")
			TEXT("capacity and must be under 1"),
			BareResult.Worst),
		BareResult.Worst < 1.0);

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
	 * TEN TIMES, NOT MERELY MORE. A strict inequality alone is satisfiable by a last-bit
	 * difference, which is not a superimposed-load term working; the measured separation is a
	 * factor of 32, with the loaded joint's tension driven to exactly zero and compression left
	 * governing. Ten is the floor and the measurement is three times clear of it — and it is
	 * headroom in BOTH directions, because the ceiling is the ten-course wall's own base
	 * compression at 0.00109, which caps the achievable factor at about 53.
	 *
	 * NOTE WHAT THIS IS A BOUND ON. `Worst` for case 15 is the worst joint ANYWHERE in that wall,
	 * so bounding it above bounds the header's own joint above by the same number, whichever joint
	 * happens to be carrying the maximum. That is the direction that makes the claim safe against a
	 * later slice relieving the header further.
	 */
	constexpr double MinimumSeparation = 10.0;

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

/**
 * THE PRODUCTION BRICKLAYER LAYS EXACTLY THE TWENTY WALLS THIS FILE HAS BEEN MEASURING.
 *
 * =====================================================================================
 * WHY THIS TEST EXISTS AT ALL
 * =====================================================================================
 *
 * The bricklayer above is TEST-ONLY, so no level can reach it — which is why the twenty walls the
 * user drew and reviewed cannot be catalogue rows until the geometry is production. Moving it is
 * the cheap part. The expensive part is that every reading in this file is a statement about a
 * particular arrangement of particular bricks, worked to seventeen digits: 0.195160875 at a
 * four-step corbel's bottom rung, 0.058203838191552663 at a bare header's bed joint, 0.01000825 of
 * head-joint shear, and six known-red rows whose failure messages name bricks by (course, cell). A
 * producer that differs from the fixture by one ulp anywhere is not "a fixture that moved", it is
 * a dozen tests changing their answers at once for no visible reason — and four of them are
 * ALREADY RED, so a changed message there would hide a regression inside a known failure.
 *
 * =====================================================================================
 * WHAT IS COMPARED, AND WHY HANDLE ORDER IS IN THE LIST
 * =====================================================================================
 *
 * The piece count, every box centre and extent, every mass, every grounded flag, and the whole
 * connection set IN ORDER — pairing, normal, area, centre and half-extent. Exact `==` throughout,
 * because "bit-identical" is the claim and a tolerance lets exactly the drift this exists to
 * refuse through.
 *
 * A builder that laid the same bricks in a different sequence would renumber every joint and every
 * break stamp while every geometric check still passed. That is the failure that looks like
 * nothing, so it is asserted rather than assumed.
 *
 * AND THE GRID AND THE REGIONS MOVE WITH IT. `FWallRegion` is the vocabulary every cut and every
 * named outcome in this file is written in, and resolving one needs each piece's (course, cell) —
 * so production must hand those back and must name the same bricks the fixture names, or twenty
 * levels cut the wrong bricks while laying the right wall.
 *
 * =====================================================================================
 * IT GOES QUIET AFTER THE MOVE, DELIBERATELY
 * =====================================================================================
 *
 * Once the fixture is a call to production this compares a thing with itself, exactly as
 * `Core.Corbel.MatchesTheFixtureBitForBit` does. The value is spent at the moment of the move.
 * What remains standing afterwards is `Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays` —
 * which becomes the claim that this producer lays the wall `Layout::RunningBond` lays on the shape
 * both can lay, and is the reason this file cannot become a second definition of what a wall is.
 *
 * NEEDS A TICKING WORLD: NO. Boxes and doubles; no solve at all.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceProducerMatchesFixtureTest,
	"DestructionGame.Acceptance.Wall.TheProducerLaysTheWallTheFixtureLays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceProducerMatchesFixtureTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/**
	 * A CELL INDEX IS AN INDEX, NOT A READING, so it is held to a tolerance rather than to a bit.
	 *
	 * Every region bound in this file is at least an eighth of a cell — 0.125 — clear of any brick
	 * centre, so this is eight orders of magnitude below anything that could change which region a
	 * piece falls in. The course number is an integer and is held exactly.
	 */
	constexpr double CellTolerance = 1.0e-9;

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty cases"), Cases.Num(), 20);

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
 * EVERY ONE OF THE TWENTY IS A LEVEL A HUMAN CAN JOIN, LAYING THE SAME WALL AND CUTTING THE SAME
 * BRICKS THE ACCEPTANCE ROW MEASURES.
 *
 * =====================================================================================
 * WHY THE CHECK LIVES HERE RATHER THAN BESIDE THE CATALOGUE
 * =====================================================================================
 *
 * The verdicts stay in this file, so the only place that knows what case 8 IS, is this file. A
 * test living in `Tests/DestructionScenariosTest.cpp` could assert that a row called `wall-08`
 * exists and builds; it could not assert that the wall it builds is the wall case 8's expected
 * outcome was measured against, which is the only claim worth making. The include direction is
 * still test-includes-production: this reads `World/DestructionScenarios.h`, and nothing in
 * production reads anything here.
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND WHY THE CUT IS THE HALF THAT MATTERS
 * =====================================================================================
 *
 * Each case must have a row; the row must carry the case's title VERBATIM, so a level and a
 * catalogue entry cannot describe different things; the wall it builds must be the fixture's wall
 * brick for brick AND in handle order; and the bricks it resolves to cut must be exactly the
 * bricks the case's cut regions name.
 *
 * MOST OF THESE ROWS CUT, AND THAT IS THE POINT OF THE LEVEL. The wall stands, the player looks at
 * it for the row's own hold, and then the bricks go and they watch what the wall does about it. A
 * level that laid the right wall and cut a brick two cells over would show a plausible collapse
 * that has nothing to do with the case it is named after — which is exactly the failure the whole
 * acceptance set exists to make visible, relocated to where nobody is looking for it. Cases 1 and
 * 17 are intact walls and must cut nothing, and that is asserted from the case's own empty cut
 * list rather than from a list of numbers here.
 *
 * THE SET IS COMPARED SORTED, BECAUSE THE ORDER OF A CUT IS THE ROW'S OWN BUSINESS: every cut
 * brick goes in one batch and one solve, so nothing downstream can see the sequence. WHICH bricks
 * is the whole claim.
 *
 * NEEDS A TICKING WORLD: NO. The catalogue and the producer are both world-free; what a level does
 * with them is `World.Scenarios.*`'s business and is already covered.
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

	TestEqual(TEXT("FIXTURE: the catalogue is twenty cases"), Cases.Num(), 20);

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
					TEXT("%s: the twenty walls the user drew are the whole reason the acceptance set ")
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

		/*
		 * THE TITLE IS THE CASE'S, VERBATIM. The catalogue in WALL_CASES.html, the acceptance row
		 * and the banner a player reads have to be three views of one case rather than three
		 * descriptions that agree today.
		 */
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

		/*
		 * AND THE INTACT ROWS CUT NOTHING, DERIVED FROM THE CASE RATHER THAN LISTED. Cases 1 and 17
		 * are whole walls; a row that quietly took a brick out of one would turn a regression anchor
		 * into a different case.
		 */
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
 * NO LEVEL PUTS A VERDICT ON SCREEN THAT THE SOLVER DOES NOT PRODUCE.
 *
 * =====================================================================================
 * WHY A CAPTION NEEDS A TEST AT ALL
 * =====================================================================================
 *
 * Six of these twenty rows are red today — 8, 9, 10, 12, 19 and 20 — and a level captioned "the
 * course over the doorway drops and the wall stands" while the model drops nothing is a LIE TOLD TO
 * SOMEBODY STANDING IN FRONT OF THE COUNTER-EXAMPLE. That is worse than silence: the player can
 * see the wall, and the caption is the only thing telling them whether what they are looking at is
 * the answer or the bug.
 *
 * So a caption may say what is expected, and it may say that the model disagrees, and it may say
 * only what the level does — but it may not claim a verdict the solver does not currently produce
 * without admitting it.
 *
 * =====================================================================================
 * THE CONVENTION, AND WHY IT IS A TOKEN RATHER THAN PROSE MATCHING
 * =====================================================================================
 *
 * A caption is prose and has to stay prose. What is pinned is one token in it: `Expected: STANDS`,
 * `Expected: LOCAL LOSS` or `Expected: COLLAPSE`, spelled the way `VerdictName` spells it, exactly
 * one of the three, and matching the verdict the acceptance row asserts. Everything else in the
 * sentence is the writer's. Matching whole sentences would have made the caption unwritable;
 * matching nothing would have let it drift the day a verdict changed — which has already happened
 * twice in this project, when cases 8 and 16 were corrected in WALL_CASES.html and left stale in
 * this file for a day.
 *
 * AND WHICH ROWS MUST ADMIT A DISAGREEMENT IS COMPUTED, NEVER LISTED. The row is RUN, its verdict
 * is evaluated, and the marker is required exactly when the model got it wrong — so a slice that
 * fixes case 8 turns this red until the caption stops claiming a disagreement that no longer
 * exists. A hardcoded list of six numbers would have rotted silently in the other direction.
 *
 * THE KNOWN-RED SET IS A TRIPWIRE ON THIS TEST'S OWN PREDICATE, not a second copy of the rule.
 * `ModelAgreesWithVerdict` is a second reading of the three verdict shapes `Acceptance.Wall.
 * Catalogue` asserts one at a time, so it could drift from the catalogue and caption a level
 * wrongly while every catalogue row still failed exactly as before. Requiring it to name precisely
 * the six rows CURRENT_STATE records as red is what makes that drift visible.
 *
 * NEEDS A TICKING WORLD: NO. It solves the same twenty walls the catalogue solves.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceCaptionTest,
	"DestructionGame.Acceptance.Wall.EveryLevelsCaptionTellsTheTruth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceCaptionTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;
	using namespace DestructionScenarios;

	/**
	 * THE SIX ROWS THE MODEL GETS WRONG TODAY, as CURRENT_STATE.md records them.
	 *
	 * This is NOT what decides which caption must admit a disagreement — that is computed per row
	 * below. It is a check on the predicate that computes it: a second reading of the verdict rules
	 * that drifted from the catalogue's would caption a level wrongly while every catalogue
	 * failure stayed word for word identical, which is the one failure this file could not
	 * otherwise see.
	 */
	const int32 KnownDisagreements[] = { 8, 9, 10, 12, 19, 20 };

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty cases"), Cases.Num(), 20);

	TArray<int32> Disagreed;

	for (const FWallCase& Case : Cases)
	{
		const FString LevelName = LevelNameForCase(Case.Number);

		const FString Where =
			FString::Printf(TEXT("case %d (%s) as '%s'"), Case.Number, Case.Title, *LevelName);

		/*
		 * WHAT THE MODEL ACTUALLY DOES WITH THIS WALL, READ BEFORE THE ROW IS LOOKED UP AND
		 * DELIBERATELY NOT SKIPPED WHEN THERE IS NO ROW YET.
		 *
		 * The tripwire at the bottom is a check on the predicate rather than on the captions, and a
		 * predicate that was only exercised once the levels existed would be unproven at exactly the
		 * moment somebody was relying on it to caption twenty of them. Run this way it reproduces
		 * the six known reds on the day it is written.
		 */
		FWall Wall;
		FWallResult Result;

		RunWallCase(*this, Case, Wall, Result);

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

	TestTrue(
		*FString::Printf(
			TEXT("TRIPWIRE: the rows this test reads as wrong must be exactly the six ")
			TEXT("Acceptance.Wall.Catalogue fails on — it read {%s}, the known reds are {%s}. If a ")
			TEXT("slice fixed one, its caption must stop admitting a disagreement and this list must ")
			TEXT("shrink with it; if they differ any other way, this test's reading of a verdict has ")
			TEXT("drifted from the catalogue's and a level is being captioned by the wrong rule."),
			*DisagreedText, *KnownText),
		Disagreed == Known);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
