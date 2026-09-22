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
 * The wall acceptance set: twenty-two configurations, each with an outcome expected from how real
 * masonry behaves rather than from what the solver computes. The catalogue is
 * claude_plans/WALL_CASES.html (agreed 2026-08-06); this file is it as a parameterised table, so
 * adding a case is adding numbers. Cases 21-22 were user-directed 2026-08-12, built 2026-08-13
 * (section G).
 *
 * Five cases once formed matched pairs differing by one variable each, so a disagreement pointed
 * at one term in the force calculation. All five re-ruled to STANDS by 2026-08-12 and the pair
 * assertion (Acceptance.Wall.MatchedPairs) retired; each discrimination moved to a per-joint
 * reading or the LP oracle rather than an outcome:
 *
 *   13 vs 14  CorbelProjectionIsReadInTheJointNotInTheOutcome  0.070 to 0.195, 2.8x
 *   15 vs 16  SuperimposedLoadIsReadInTheJointNotInTheOutcome  0.0018 vs 0.0582, 32x
 *    7 vs  9  SpanIsReadInTheJointNotInTheOutcome              0.269 vs 0.985, 3.66x
 *   11 vs 12  LP oracle, OracleSweepFull.RigidBlock.WallsAndLadders  128.12 vs 89.12
 *    7 vs 10  LP oracle                                        296.22 vs 35.82, 8.27x
 *    7 vs  8  nowhere: a downward-routing solver reads cover as load, not arch capacity (CASE 8
 *             block, section B). No case here now refuses arching for lack of cover.
 *
 * Three verdicts, three assertion shapes (DESIGN.md §4, outcome not mechanism):
 *
 *   STANDS      nothing left and no joint gave. Both halves, since "no piece fell" alone passes a
 *               wall that severed half its joints and stayed leaning together.
 *   LOCAL LOSS  the fallen set is exactly the named set, by identity not count.
 *   COLLAPSE    every piece of a named region fell and every piece of a named survivor region
 *               kept the ground. Two-sided.
 *
 * Cases 21-22 (2026-08-13) are the set's only Collapse rows; before them the collapse arm was
 * dead code. Their bite was re-proven by mutation on arrival (TRAPS.md rows).
 *
 * Displacement is never a break assertion (DESIGN.md §4): two pieces can sever and stay resting in
 * place. What is read is whether a piece still has a path to the earth after the cascade.
 *
 * No ticking world, deliberately. Gravity is on (mass x 980), everything connected, assertion on
 * outcome. A world would add only the solver-to-Chaos wire, covered in StructureIntegrationTest
 * and identical across all rows; promote a row there if it ever needs watching fall.
 *
 * Named namespace, named differently from every other in this module: an anonymous namespace is
 * per-translation-unit and a unity build merges files (CURRENT_STATE.md). The using directives sit
 * inside each RunTest body for the same reason.
 *
 * Nothing is imported from the code under test except the producer. Grid, brick weight, N-to-uu
 * conversion and every strength are re-derived below, so a wrong production constant makes this
 * file disagree. The one exception is Layout::MakeInterface (which boxes share a face);
 * Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays pins the fixture's bricklaying against
 * Layout::RunningBond so this cannot become a second, drifting wall producer.
 */
namespace WallAcceptanceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* The coordinating grid, re-derived here. UK metric standard brick with a 1 cm joint: brick
	 * plus joint is one cell along the wall (22.5 cm) and one course up (7.5 cm), and running bond
	 * offsets alternate courses by half a cell. Table positions are quoted in cells (centre X /
	 * 22.5), the unit the bond is built on. */
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
	 * Shortest closing piece the fixture lays at a course's left end; a shorter remainder is
	 * dropped. A 2 cm sliver has a plausible mass but an implausible joint and would be the first
	 * thing to break. 4 cm is under the 10.25 cm half bat a flush wall closes with, over anything
	 * laid by accident.
	 */
	constexpr double MinClosingPieceCm = 4.0;

	/** g/cm3. ClayBrick's published density, spelled out so a changed profile shows up here. */
	constexpr double ClayBrickDensityGramsPerCubicCm = 1.9;

	/**
	 * Weight from mass, derived not imported. Gravity is 980 cm/s2 and mass is kg, so kg x 980 is
	 * weight in Unreal force units: DESIGN.md §3's 1 N = 100 uu is already inside 980, and applying
	 * it again is the 100x error the units section prevents. Lands on 2.72163125 kg for a brick.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double FullBrickWeightUu =
		ClayBrickDensityGramsPerCubicCm * BrickLengthCm * BrickDepthCm * BrickHeightCm / 1000.0
		* GravityCmPerSecondSquared;

	/** Unreal force units per MPa per cm2, written out not imported. Production's boundary is
	 * ConnectionStrength.h's ForceUnitsPerMPaSqCm; a test that read it would agree with a wrong
	 * one, so this writes the number itself and disagrees instead. */
	constexpr double ForceUnitsPerMPaSqCmHere = 10000.0;

	/* Mean shear bond f_v0 for general-purpose mortar, asserted against the profile (re-anchor
	 * 2026-08-13: Gooch et al. 2023 M4/M6 triplet means avg 1.117, 2025 regression intercepts
	 * 0.58-1.04; 0.90 is the centre. Retired characteristic f_vk0 was EN 1996-1-1 Table 3.4's 0.20). */
	constexpr double MortarShearCohesionMPa = 0.9;

	/* Mean flexural bond f_x1, asserted against the profile (re-anchor 2026-08-13: twelve M4/M6
	 * batch means avg 0.571, bracketed with UK NA Table NA.6's 0.4 x 1.89 = 0.76; 0.70 is the
	 * centre. Retired characteristic f_xk1 was EN 1996-1-1 Table 3.2's 0.10). */
	constexpr double MortarFlexuralBondMPa = 0.7;

	/* Rest of the Mohr-Coulomb triple, needed since 2026-08-14 to say which axis a reading is: mu
	 * is the centre of the measured means (Gooch et al. 2025), the truncation is 0.1.f_b against a
	 * 20 MPa unit, the compressive strength the declared-class figure. Asserted in
	 * SpanIsReadInTheJointNotInTheOutcome, the only test that reads them. */
	constexpr double MortarFrictionCoefficient = 0.75;
	constexpr double MortarMaxShearStrengthMPa = 2.0;
	constexpr double MortarCompressiveStrengthMPa = 10.0;

	/** A head joint: the end face of a brick, 10.25 cm through the wall by 6.5 cm high. */
	constexpr double HeadJointAreaSqCm = BrickDepthCm * BrickHeightCm;

	enum class EBond : uint8
	{
		/** Alternate courses offset half a cell, half bats filling the ends flush. */
		Running,

		/** Every course identical, so head joints line up through the whole wall. */
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
	 * A rectangle in (course, cell) space, the vocabulary the whole table speaks. A piece is inside
	 * when its course is within the inclusive range and its cell is strictly between the two bounds.
	 * Strict on purpose: brick centres are exact quarter-cell multiples, so every bound below is at
	 * least an eighth of a cell (2.8 cm) clear of any centre and no tolerance question arises.
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
		 * A course whose end brick is pushed half a cell past the wall face, or INDEX_NONE. The
		 * projecting-header fixture: a flush odd course closes with a half bat; pushing the face out
		 * and closing with a full brick puts a whole brick where the bat was, half bearing on the
		 * course below and half over air.
		 */
		int32 ProjectingCourse = INDEX_NONE;

		/** Everything the player deletes, applied AFTER the intact wall has been checked. */
		TArrayView<const FWallRegion> Cuts;

		/**
		 * What must lose its path to earth. Exact for a local loss (the fallen set is precisely
		 * these pieces, so wrong bricks falling is a failure); a lower bound for a collapse, paired
		 * with MustStand as the upper one, since a collapse's exact boundary is a model detail.
		 */
		TArrayView<const FWallRegion> MustFall;

		/** What must keep it. Read only for a collapse; a local loss gets exactness instead. */
		TArrayView<const FWallRegion> MustStand;

		/**
		 * How many pieces the model drops here today: a characterisation of a wrong answer, set on
		 * the three known-red rows and INDEX_NONE elsewhere. Not an expectation. MustFall/MustStand
		 * are what a real wall does; this is what the solver does instead, measured off a run. It
		 * exists because an already-red row absorbs a regression silently: case 20 should drop two,
		 * drops nine, and would fail in the same words if a change made it drop ninety.
		 *
		 * Since 2026-08-12 two of the three (cases 10 and 19) sit on STANDS rows: the model still
		 * drops 12 and 34, so here the pin points at pieces that should never have left the wall.
		 * The field still means "what the solver does, never what it should do".
		 *
		 * When the row is fixed, delete this anchor in the same edit (it will fail, which is the
		 * reminder). Never update it to a new wrong number without saying why the answer moved.
		 */
		int32 DropsToday = INDEX_NONE;

		/**
		 * How many live pieces the solver cannot route here today. Zero is the claim; anything else
		 * is a characterised defect on the row that has it.
		 *
		 * DESIGN.md §4 requires a collapse test to assert nothing is Stranded when it goes, so a
		 * solver limit cannot wear a collapse's clothes. Asserting it first (2026-08-09) found three
		 * of the then-six red rows stranded. Case 12's pin died with its 2026-08-09 rewrite; two
		 * exceptions remain (cases 10 and 19), and eighteen of the twenty make the plain zero claim.
		 * Neither exception can grow by one piece without failing.
		 *
		 * Both are now ruled STANDS: the sweep measured production reaching its 12 and 34 drops in
		 * zero cascade passes at worst readings 0.300 and 0.318, no joint near capacity. So the
		 * stranding is the same finding as the drop count said twice: nothing broke, the router had
		 * nowhere to send the load. Retired by the loop-division rule DESIGN.md §5.1 records as
		 * absent; when it lands these two go to zero.
		 */
		int32 StrandsToday = 0;

		/**
		 * Worst reading the cascade started from, or 0.0 for a row that does not pin it. A count
		 * says a wall came down; this says how hard it was pushed. Case 22, the one green Collapse
		 * row, collapses on a knife edge: the mean re-anchor cut its margin from 8.2x to just over
		 * 1, so one modest routing change now flips it. The drop count cannot see that coming.
		 *
		 * Read before any joint gives, which is what makes it different from FWallResult::Worst (a
		 * post-cascade statement about the survivors).
		 */
		double PreCascadeWorstToday = 0.0;
	};

	/*
	 * The fixture's own bricklayer. Not Layout::RunningBond because six of the twenty-two cases are
	 * not running-bond rectangles (two corbel, two carry a projecting header, two are stack bond)
	 * and RunningBond lays only one shape; dropping them would lose two matched pairs.
	 *
	 * One rule, not six special cases: each course is two numbers (right face, length of rightmost
	 * piece), laid right to left on the 22.5 cm pitch, closing with what is left at the left face:
	 *
	 *      running bond    right face fixed; odd courses close with a half bat at the right
	 *      stack bond      right face fixed; every course closes with a full brick
	 *      corbel          right face steps out once per course from CorbelFromCourse
	 *      header          one course's right face is half a cell further out
	 *
	 * Laying right-to-left keeps the cut piece at the left end, clear of every corbel and header.
	 * A flush running-bond wall laid here must match Layout::RunningBond brick for brick
	 * (Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays), or this is a second definition of a wall.
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

	/** Mass from geometry, derived not imported. cm3 x g/cm3 is grams, / 1000 is kg. No force
	 * conversion here: 1 N = 100 uu is a property of forces and mass goes into Unreal unconverted.
	 * Lands on 2.72163125 for a full brick. */
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

				/* The box centre is the centre of mass (a brick is homogeneous). Without it the wall
				 * has no eccentricity and every corbel reads as though its weight acted through the
				 * middle of its support. Asserted as a fixture precondition below. */
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

		/* Pairs: neighbours along a course, and every piece of the course below. Offering the whole
		 * course below rather than computing spans keeps the mixed-size and corbelled cases honest:
		 * MakeInterface refuses the pairs that are diagonals, and that decision belongs to it.
		 * Courses two apart never reach (6.5 cm brick on a 7.5 cm pitch). */
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
	 * Which live pieces have lost their path to earth: the outcome, not the mechanism. A piece
	 * comes down when the solve says nothing holds it. Stranded counts as fallen like Falling does
	 * (the piece is not being carried); whether the solver could not route it or has nothing to
	 * route through is a separate question, reported below, never folded into the verdict.
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

	/**
	 * How many live pieces the solver could not route at all. A precondition, never a verdict.
	 * Stranded means the solver declined to divide load round a loop, so it counts as fallen in
	 * FallenPieces. But a verdict decided by that is about the solver's limit, not the wall, and
	 * reading it as physics is how a solver limitation wears a collapse's clothes (DESIGN.md §4).
	 * So it is counted separately, printed on every row, and asserted zero before any verdict.
	 */
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

		/** How many pieces the wall had already dropped BEFORE the player cut anything. */
		int32 IntactFallen = 0;

		/** Read as built, before anything is removed or broken: HasCompleteGeometry is a
		 * conjunction over what is still in the structure, so a removed piece and a given joint
		 * both stop counting. Asking after the cut would let the fixture defect this catches through. */
		bool bCompleteGeometryAsBuilt = false;

		TArray<int32> Fallen;

		/** Of those, how many the solver could not route rather than could not hold up. */
		int32 Stranded = 0;

		double Worst = 0.0;
		int32 WorstPieceA = INDEX_NONE;
		int32 WorstPieceB = INDEX_NONE;

		/**
		 * Worst reading the cascade started from, different from `Worst`. `Worst` is read after the
		 * cascade, by which time WorstUtilisation has skipped every given joint, so on a collapse it
		 * reports the survivors. This is the loads solved once on the structure the cascade is about
		 * to get (post-cut for a cutting row, as built otherwise) with no joint yet given.
		 *
		 * It exists because the collapse rows' margin was unmeasured: case 22's was taken by hand
		 * once at retired data and recomputed nowhere across the mean re-anchor.
		 */
		double PreCascadeWorst = 0.0;
		int32 PreCascadeWorstPieceA = INDEX_NONE;
		int32 PreCascadeWorstPieceB = INDEX_NONE;
	};

	/**
	 * Lay it, cut it, run the cascade, record what came down. No assertions.
	 *
	 * The intact wall is solved first, not for decoration: a wall already falling before the cut
	 * measures nothing. What is done with that reading is CheckWallFixture's business (a cutting row
	 * must have stood before the cut; a row that cuts nothing reads its as-built state as a verdict).
	 *
	 * Pure, so it can be cached: six tests ask for the same twenty walls, once laid and cascaded
	 * ~sixty times a run. A wall's answer may be shared (a function of the case); the assertions
	 * may not, since a fixture failure firing only for whichever test asked first would move on
	 * reorder. So this half is cached and the checking half below re-runs against the cached answer.
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

		/* The as-built reading, before the first cascade breaks anything. For a row that cuts
		 * nothing this is what the cascade started from; a cutting row overwrites it below with the
		 * post-cut one. */
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

	/**
	 * Everything a row must satisfy before its verdict means anything, asserted once per caller. A
	 * wall laid without geometry, or one that fell before the cut, gives a verdict about the fixture
	 * rather than the physics, sending a reader chasing a bug that is not there.
	 */
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

		/*
		 * Nothing may be Stranded, the precondition that makes a verdict honest. A Stranded piece is
		 * one the solver declined to route round a loop, not one the wall failed to hold up.
		 * FallenPieces folds the two together (a piece nothing carries comes down either way), so
		 * without this a row could name the right bricks for the wrong reason. Written against the
		 * row's own figure not a bare zero, since two rows are not zero (FWallCase::StrandsToday).
		 */
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

	/** One wall and what it did — the unit the cache hands out and every caller reads. */
	struct FSolvedWall
	{
		FWall Wall;
		FWallResult Result;
	};

	/**
	 * Everything the solve reads off a case and nothing else, so it is a valid cache key. LayWall
	 * reads courses, cells and (via CourseGeometry) bond, corbel and projecting course; CutPieces
	 * reads the cut regions. Two cases agreeing on these are the same wall cut the same way. The
	 * case number is deliberately excluded (StackBondColumnShearIsHeightIndependent builds its own
	 * case 18). Verdicts, titles and fall regions are excluded too: the solver never sees them.
	 */
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

	/** The answer for this wall, laid and cascaded once however many tests ask for it. Held by
	 * pointer because a TMap moves its values when it grows and every caller holds a reference
	 * across its test. It outlives the run, safe because the answer is a pure function of the key:
	 * a second run in the same process reads what the first computed. */
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

	/** The cached answer, with this caller's own copy of the fixture preconditions run over it. */
	const FSolvedWall& RunWallCase(FAutomationTestBase& Test, const FWallCase& Case)
	{
		const FSolvedWall& Solved = SolvedWallCase(Case);

		CheckWallFixture(Test, Case, Solved.Wall, Solved.Result);

		return Solved;
	}

	/** One line per case, whether it passed or not, so the whole set reads off the log. */
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

	/* --- A: one brick out. ---------------------------------------------------------
	 *
	 * Thirty courses, not the ten the drawing shows: the height is the point. ARCHING_DESIGN.md
	 * works the half-seated joint at 0.058203838 of capacity per brick weight, so a ten-course wall
	 * says "stands" whatever the model does. The joint reaches 1.0 at eighteen courses of cover, so
	 * thirty courses with the cut in course 1 is firmly past the line (~28 brick weights, design's
	 * 1.62971). The physical claim is height-independent; the fixture must be tall enough to disagree.
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
	 * CASE 8, re-ruled STANDS 2026-08-11: the one course over the hole stands, and the catalogue
	 * loses its last "no room to arch" row.
	 *
	 * Geometry: five courses, twelve cells, case 7's four-cell cut through courses 1..3 (one course
	 * of cover). The row used to claim LOCAL LOSS of the two seatless bricks (c4/5, c4/6) on the
	 * reading that a single course cannot arch; red for its whole life because the model drops
	 * nothing. Three independent methods on the 2026-08-11 sweep:
	 *
	 *     catalogue   LOCAL LOSS on flexure of a single spanning course
	 *     production  drops/breaks nothing; worst joint 0.218869 in the jamb, not over the hole
	 *     LP oracle   STANDS at lambda* = 324.732096 (RigidBlockOracleSweepTest, 2026-08-11)
	 *
	 * User ruled STANDS 2026-08-11 (DESIGN §8): a flat course jams into its abutments in head-joint
	 * compression, a flat arch that needs abutments not cover. lambda* = 324.73 is far from
	 * strength-governed (compression is 100x tension), and it reads 18.0 even discounted by both
	 * sweep-header slants (/3, /6). The model's agreement is not confirmation (at this height the
	 * springing has almost no pre-compression and the Mohr-Coulomb thrust check goes toothless,
	 * RESULTS.md §6.2); the verdict rests on the LP.
	 *
	 * Cost: case 8 was the last "no room to arch" discriminator; after it no case refuses arching
	 * for lack of cover. The 7-vs-8 pair left the retired Acceptance.Wall.MatchedPairs, and a
	 * replacement must starve the abutment (case 10's shape, itself since ruled STANDS). If physical
	 * evidence contradicts this, it comes back through DESIGN §8, not a quiet edit.
	 */

	/*
	 * CASE 9, re-ruled STANDS 2026-08-12: the ten-cell opening spans as a deep beam, retiring the
	 * third arching-gate verdict.
	 *
	 * Geometry, cm: cut { 1, 3, 1.75, 11.25 } takes 28 of 174 laid; span ~9.5 cells; 60 cm of
	 * masonry (eight courses) spans the hole. The row used to claim COLLAPSE on the BS 5977 arching
	 * gate (2.1 m span wants 1.07 m rise; this has 0.60 m); red for life because the model drops
	 * nothing. Outlier of three methods on the 2026-08-12 sweep:
	 *
	 *     catalogue   COLLAPSE, on the arching gate
	 *     production  STANDS, zero cascade passes; worst joint 0.98502040901419818 where cover lands
	 *                 on the right jamb
	 *     LP oracle   STANDS at lambda* = 36.5639285 (OracleSweepFull.RigidBlock.WallsAndLadders)
	 *
	 * User ruled STANDS 2026-08-12 (DESIGN §8). Independent hand check: 60 cm of bonded masonry over
	 * a 213.75 cm span is a deep beam at span/depth 3.6, not loose bricks looking for an arch. Eight
	 * courses over 9.5 cells gives extreme fibre 0.088 MPa, 0.88x characteristic f_xk1 (0.10); the
	 * hand 0.88 and production's 0.985 agree, and lambda* = 36.56 survives both discounts at 2.03.
	 *
	 * Cost: third verdict to leave on the arching gate's retirement (cases 11, 8, then 9); after it
	 * no case refuses a span for want of rise. A replacement must starve the abutment (CURRENT_STATE).
	 * The row goes green, so its pins are deleted (the model already stands this wall) and it leaves
	 * the caption test's known-disagreement list in the same edit.
	 *
	 * Watch: production's 0.98502040901 is one retune from 1.0; the day it crosses over, this row and
	 * the sweep's AgreeStands relation both flip. Pinned by
	 * Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome so the crossing fails loudly.
	 */
	const FWallRegion Case9Cuts[] = { { 1, 3, 1.75, 11.25 } };

	/*
	 * CASE 10, re-ruled STANDS 2026-08-12: the free-end panel cantilevers, and production's
	 * "collapse" is an absent mechanism, not a strength verdict.
	 *
	 * Geometry: case 7's cut extended to the free right end, { 1, 3, 7.75, 11.50 }, twelve of 150
	 * laid. Jamb on the left, nothing on the right; 3.75 cells (84.4 cm) hangs past the support. The
	 * row used to claim COLLAPSE of { 4, 11, 8.00, 11.50 } with { 0, 3, -1.0, 7.75 } standing, on
	 * "no abutment cannot arch" (true, but not the question). On the 2026-08-12 sweep:
	 *
	 *     catalogue   COLLAPSE, the overhanging panel comes down
	 *     production  drops 12, three stranded, zero cascade passes, worst 0.299975 in the jamb
	 *     LP oracle   STANDS at lambda* = 35.8172298
	 *
	 * User ruled STANDS 2026-08-12. What decided it is the zero passes: production's router could
	 * not find a path for the twelve pieces (load only goes down and there is nothing under them),
	 * unrouted not overloaded. Physically 84.4 cm of bonded 60 cm panel off one jamb is a cantilever
	 * (the 2026-08-06 free-end ruling's composite action), reading 0.055 MPa, 0.55x characteristic
	 * f_xk1; lambda* = 35.82 stands at 1.99 discounted.
	 *
	 * New shape: a STANDS row the model still disagrees with. Stays on the known-red list and caption
	 * marker, keeps `DropsToday = 12` / `StrandsToday = 3`, goes green at DESIGN §7 evolution step 4.
	 *
	 * The survivor region is restored verbatim: a STANDS verdict says nothing falls anywhere, so
	 * { 0, 3, -1.0, 7.75 } is stronger than "twelve fell" (counts are blind to which twelve).
	 * Asserted inverted; verified all twelve sit in courses 4..9 at cells 9.0+, clear of it. The
	 * 7-vs-10 pair left the retired Acceptance.Wall.MatchedPairs, not relocated onto a production
	 * reading (that would claim an abutment term the model lacks); its home is the LP, 296.22 vs 35.82.
	 */
	const FWallRegion Case10Cuts[] = { { 1, 3, 7.75, 11.50 } };
	const FWallRegion Case10Stands[] = { { 0, 3, -1.0, 7.75 } };

	/* --- C: spanning between supports. --------------------------------------------- */

	/*
	 * CASE 11, ruled twice 2026-08-08: a published gate said LOCAL LOSS, the physics says STANDS,
	 * physics is the keeper. Both rulings recorded because which is right is this row's whole
	 * content. The first rested on the BS 5977 arching gate (300 mm of masonry above a 45-degree
	 * triangle's apex; this fails it by its whole height); the second, that a serviceability
	 * never-crack line is not a collapse predictor. Worked honestly, the wall holds.
	 *
	 * Geometry, cm: cut { 0, 3, 2.75, 8.25 }, 22 of 150 laid; span 113.5-136.0; 60 cm of cover;
	 * piers three cells (66.5 cm) of bearing each. No thrust line fits, so no arch, but it stands as
	 * a deep beam (span/depth 2.3) the gate never asks about, carrying its 44 bricks:
	 *
	 *     W   44 x 2.72163125 kg x 980 = 1.1735e5 uu
	 *     M   W L / 8 = 1.995e6 uu.cm at midspan
	 *     Z   t D^2 / 6 = 10.25 x 60^2 / 6 = 6150 cm3
	 *     f   M / Z = 0.0324 MPa
	 *
	 * ~5% of the mean bond (0.70 MPa since the 2026-08-13 re-anchor); the model's worst joint agrees
	 * at 0.0517 of mean f_x1 by a different route. Hence STANDS, no fall or survivor region. Paired
	 * against case 12 it isolates pier width (three cells vs one); on 2026-08-09 the one-cell pier
	 * held too (above Case12Cuts), so the pair separates in the margin, as 13/14 and 15/16 did.
	 *
	 * Case 8 (once the last "no room to arch" discriminator) was also ruled STANDS 2026-08-11, so no
	 * case now refuses arching for lack of cover: what an arch needs is an abutment to receive the
	 * thrust, case 10's variable.
	 */
	const FWallRegion Case11Cuts[] = { { 0, 3, 2.75, 8.25 } };

	/*
	 * CASE 12, rewritten 2026-08-09: case 11's span on a one-cell pier, and worked honestly the pier
	 * holds. The old row cut ten of twelve cells, varying span and pier at once and naming no
	 * survivors (a model always answering "falls" passed it); the 2026-08-08 review approved
	 * rewriting it so pier width is the one variable against case 11.
	 *
	 * Geometry: case 11's wall with its cut shifted left, cut { 0, 3, 0.75, 6.25 }, 22 of 150 laid;
	 * pier a bonded column four courses to the springing; the right abutment grown to five cells so
	 * any failure is the narrow side alone; span and 60 cm cover identical to case 11. One narrow
	 * pier not two, because two one-cell piers leave nothing standing to name on failure.
	 *
	 * The panel is settled by case 11 (M/Z ~= 0.033 MPa). The pier: expected thrust to shove it over,
	 * worked honestly:
	 *
	 *     H       min thrust at r ~= 50 cm: H = W L/(8r) ~= 400 N, a third of the panel weight.
	 *             (Solver's kern-limited r = d_e/3 reads H = 998 N, 2.5x harsher, DESIGN §5.4.)
	 *     hinge   course-0/1 bed joint: overturning ~= 88 N.m (kern: 220).
	 *     rigid   restoring N x b/2 = 59-113 N.m; a rigid stack sits at the line, 0.7-1.3.
	 *     bonded  mean flexural bond adds 316-632 N.m, 4-8x the demand. Sliding never governs.
	 *
	 * Only the rigid-block / kern-thrust reading condemns it, the never-crack stack already rejected
	 * three times here (cases 14, 16, the free-end ruling). Verdict STANDS, no fall or survivor
	 * region. The model agrees (worst joint 0.362067, within 0.03% of case 11's 0.362193) but carries
	 * thrust as springing shear only (DESIGN §7 item 6), so it reads no pier-width term; the
	 * separation lives only in the arithmetic, awaiting the leaning stack and LP oracle. The oracle
	 * has since paid that debt: 128.12 on three cells vs 89.12 on one,
	 * OracleSweepFull.RigidBlock.WallsAndLadders.
	 */
	const FWallRegion Case12Cuts[] = { { 0, 3, 0.75, 6.25 } };

	/* --- D: corbelling and the projecting header. ----------------------------------- */

	constexpr int32 CorbelCells = 8;
	constexpr int32 CorbelFirstCourse = 6;
	constexpr double QuarterBrickStepCm = HalfCellCm * 0.5;
	constexpr double HalfBrickStepCm = HalfCellCm;

	/*
	 * CASE 14, re-derived: a bonded four-step corbel stands at 0.19516 and nothing comes down.
	 *
	 * COLLAPSE until 2026-08-07, on a rigid-body overturning reading (resultant outside the bed
	 * below, geometrically true here). Overruled because this project models an uncracked bonded
	 * section, and the user ruled 2026-08-06 that a free-end deletion must not fell a wall. Any rule
	 * honouring that honours this corbel: Core.Structure.ACorbelResistsWithItsWholeDepth stands a
	 * five-step raking corbel at 0.219, the same fixture with different provenance.
	 *
	 * CourseGeometry pushes each course from 6 up one more half cell (11.25 cm), closing with a full
	 * brick, so each of the four end bricks overlaps one piece below over 10.25 cm, centre of mass
	 * 5.625 cm outboard: four steps of the staircase fixture. The ladder is the staircase topology
	 * (each step takes its weight, the step above, and half the next brick). Section is the lesser of
	 * bonded depth W = t D^2/6 and the bed patch's 179.4817708 cm3:
	 *
	 *     course   s   F (weights)   M (weight.cm)   courses over   reads
	 *        9     0        1             5.625            1        0.058203838   (patch governs)
	 *        8     1        2.5          22.5              2        0.156128700
	 *        7     2        4.5          56.25             3        0.173476333
	 *        6     3        7           112.5              4        0.195160875   <- the worst
	 *
	 * 0.195 of f_xk1 is a fifth of capacity: STANDS, no fall or survivor region. Asserted below (not
	 * as a comment) since "stands" is satisfied by 0.195 and 0.0001 alike. Cross-checked against two
	 * figures this file did not produce: 0.21858 for five steps and 0.36903147272727271 for the
	 * eleven-step staircase (ARCHING_DESIGN.md's 0.219 and
	 * Core.Structure.AStaircaseVoidCondemnsTheCorbel), both asserted below so a drift fails here.
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
	 * What the bottom rung of a k-step bonded corbel reads, as a fraction of f_xk1. The bottom rung
	 * is k-1 steps below the top with k courses over its bed joint, so k is the only argument. The
	 * min keeps the model nested: composite action is an alternative path for the moment, not an
	 * extra one, so it may only help.
	 */
	double CorbelBottomRungUtilisation(int32 Steps)
	{
		const double MomentBrickWeightCm = CorbelLadderMomentBrickWeightCm(Steps - 1);
		const double ForceBrickWeights = CorbelLadderForceBrickWeights(Steps - 1);

		return FMath::Min(
			HalfSeatTensionMPa(MomentBrickWeightCm, ForceBrickWeights),
			CompositeTensionMPa(MomentBrickWeightCm, Steps)) / MortarFlexuralBondMPa;
	}

	/*
	 * CASE 16, re-derived: a bonded header with nothing on it stands at 0.058204. LOCAL LOSS until
	 * 2026-08-07, revised in the catalogue without this file following; corrected here by re-deriving
	 * the number. The header projects half a cell past the face, keeping 10.25 cm of bearing, centre
	 * of mass 5.625 cm outboard. On a 10.25 x 10.25 cm patch (modulus 179.4817708 cm3):
	 *
	 *     2667.198625 x 5.625 / 179.4817708 = 0.0083591 MPa   bending, opening the outer edge
	 *     2667.198625 / 105.0625            = 0.0025387 MPa   its own weight, closing it
	 *     tension 0.0058204 / 0.1 (f_xk1)   = 0.058204        of flexural bond capacity
	 *
	 * Six percent of capacity, not a brick falling off. The abandoned "local loss" was the rigid-body
	 * overturning error case 14 was corrected for the same day: an uncracked bonded section carries
	 * it. No MustFall or MustStand.
	 *
	 * The pair moved too: 15 and 16 differ only in what sits on the header's tail and both now stand,
	 * so as an outcome pair they discriminate nothing (as 13/14 stopped). What separates them is one
	 * joint, 0.00184 vs 0.05820, 32x from superimposed load (case 15's tension driven to zero,
	 * compression governing), asserted in
	 * Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome. Removed from the retired
	 * Acceptance.Wall.MatchedPairs rather than left unsatisfiable.
	 */

	/* --- E: bond pattern and head-joint shear. --------------------------------------- */

	const FWallRegion Case18Cuts[] = { { 5, 5, 4.75, 5.25 } };

	/* --- F: losing the base, and the staircase void. --------------------------------- */

	/*
	 * CASE 19, re-ruled STANDS 2026-08-12: the underpinned half cantilevers. The closest call of the
	 * three, ruled STANDS knowingly.
	 *
	 * Geometry is six whole cells (an earlier draft's 5.75 mixed a piece count into a cell span,
	 * 4% light). { 0, 0, -0.50, 5.25 } cuts course 0's six whole bricks (cells 0..5), 135 cm of
	 * footing gone under a ten-course, twelve-cell wall, nine courses over the void. The row claimed
	 * COLLAPSE of { 1, 9, -1.00, 4.60 } with { 0, 9, 7.75, 13.0 } standing. On the 2026-08-12 sweep:
	 *
	 *     catalogue   COLLAPSE, masonry over the void has no path to earth
	 *     production  drops 34, six stranded, zero cascade passes, worst 0.31804; nothing broke
	 *     LP oracle   STANDS at lambda* = 12.3824832, lowest of the fifteen walls, still above 1
	 *
	 * User ruled STANDS 2026-08-12, the set's closest call; the hand check straddles rather than
	 * settles it. Nine courses over 6.0 cells is 54 brick weights at a 67.5 cm lever, 0.125 MPa:
	 * 1.25x characteristic f_xk1 (over), 0.31x f_xk2 (under), ~0.21x the mean basis. One published
	 * number condemns, two acquit. It is an end cantilever with no second support, so of the three
	 * ruled 2026-08-12 the most plausibly wrong; anchored by real practice underpinning ~1 m bays.
	 * The same zero as case 10 applies: 34 dropped without breaking a joint, unroutability not
	 * strength. If physical evidence contradicts this, come back through DESIGN §8.
	 *
	 * The survivor region { 0, 9, 7.75, 13.0 } is restored verbatim: stronger than "34 fell" because
	 * a routing change shifting the spreading front three cells right leaves every count pin
	 * unchanged (counts cannot see a translation, the region can). Asserted inverted; verified all 34
	 * sit at cell 4.5 and below, clear of it. Stays red inverted (expected STANDS, measured 34
	 * dropped), keeps its pins (`DropsToday = 34`, `StrandsToday = 6`), goes green at evolution step 4.
	 */
	const FWallRegion Case19Cuts[] = { { 0, 0, -0.50, 5.25 } };
	const FWallRegion Case19Stands[] = { { 0, 9, 7.75, 13.0 } };

	/*
	 * CASE 20 — the staircase void, the one case drafted with a question mark. User ruled 2026-08-06:
	 * LOCAL LOSS, the loose toothed bricks at the cut edge drop and the wall's mass stands.
	 *
	 * The raking cut is one region per course, each course above cut one cell less far right, so the
	 * surviving masonry steps left over the hole. Which bricks are teeth, worked from the geometry:
	 * exactly two survive with no bed patch, course 3/cell 4.5 and course 5/cell 2.5; everything else
	 * keeps at least one patch (a corbel, not a tooth). Naming by identity not count matters: a rule
	 * dropping the corbelled half-seats instead falls the same number and is wrong. The fall set in
	 * WALL_CASES.html is not transcribed (its region overlaps its own cuts, rendering a stray brick);
	 * the prose is what was agreed.
	 *
	 * Re-examined and confirmed 2026-08-12 (a ruling that moved nothing, recorded because rows 9, 10,
	 * 19 beside it all moved). Unlike cases 10 and 19 this is a strength verdict: one cascade pass,
	 * pre-cascade peak 42.71 (a joint 42x over), survivors' worst 0.296506 after the cascade. The LP
	 * gives lambda* = 82.629597 but a global load factor has no local vocabulary; it cannot say
	 * whether nine bricks or two come down. The doubt survives: true count is between the two named
	 * and the model's nine, awaiting equilibrium promotion (DESIGN §7 step 4).
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

	/* --- G: the counter-cases — an opening that is genuinely too big for what covers it. ------ */

	/*
	 * SECTION G, user-directed 2026-08-12, built 2026-08-13: the two rows that put a falling verdict
	 * back in the set, the catalogue's first two `Collapse` rows.
	 *
	 * Five re-rulings in five days took every falling verdict out (cases 11, 8, 9, 10, 19), each
	 * costing a discriminator. The user directed two counter-cases to case 9, one starving the cover
	 * and one growing the span, each sized so the verdict clears the mean-basis ceiling rather than
	 * sitting where a ruling would be a judgement call.
	 *
	 * Both are sized on the deep-beam criterion every §8 ruling since case 11 argues with:
	 *
	 *     w = (courses of cover) x (brick weight) / (cell pitch)     load per cm of span
	 *     M = w L^2 / 8                                              simply supported
	 *     Z = t D^2 / 6                                              t = 10.25, D = cover
	 *     sigma = M / Z                                              proportional to L^2/D
	 *
	 * Read against characteristic f_xk1 = 0.10, f_xk2 = 0.40, and the mean basis 0.4-0.8 MPa (DESIGN
	 * §3). A verdict clears the ceiling when sigma exceeds 0.8, since then no basis acquits it:
	 *
	 *     case 8     1 course over 4 cells     L = 79.75   D = 7.5    0.098 MPa   0.12x
	 *     case 9     8 courses over 10 cells   L = 214.75  D = 60     0.089 MPa   0.11x
	 *     case 21    2 courses over 18 cells   L = 394.75  D = 15     1.201 MPa   1.50x
	 *     case 22    8 courses over 35 cells   L = 777.25  D = 60     1.164 MPa   1.46x
	 *
	 * (last column against 0.8 MPa). Stood rows sit at an eighth of the ceiling, new rows 12x above
	 * it. Case 9's 0.089 MPa is reconciled through sigma ~ L^2/D (the same formula, catching a slip,
	 * not a second method); what was measured on case 9 is production's 0.985, pinned in
	 * Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome. The three independent methods are hand
	 * statics, production, and the LP oracle, and on case 21 the LP disagrees.
	 *
	 * THE LP DISAGREES ON CASE 21 AND WHAT IT PRICES IS NOT KNOWN. It stands case 21 at lambda* =
	 * 5.511 (2026-08-13), the first time the LP is the outlier, pinned as `OracleStandsProductionFalls`
	 * in OracleSweepFull.RigidBlock.WallsAndLadders. Established: the LP is not pricing the argued
	 * mechanism. A 15 cm panel on characteristic bond fails at lambda = 0.10/1.2014 = 0.0832, so 5.511
	 * is 66x stronger; what carries the wall in the LP's world is unidentified (the earlier claim was
	 * withdrawn 2026-08-13). Two measured ladders on the case-21 family (L = 394.75 + (cells-22) x 22.5):
	 *
	 *     cover, held at 22 cells   2 crs 5.511   4 crs 7.683   6 crs 9.183   8 crs REFUSED
	 *     span, held at 2 courses   18 c 10.408   22 c 5.511   27 c 3.102   32 c 1.985
	 *                               40 c  1.143   45 c 0.863
	 *
	 * The priced hypothesis (a full-height arch diving through the jambs into fixed course 0) matches
	 * at 22 cells (H = W L/(8r) = 1,539 N vs 8,440 N, 5.48 against 5.511) but walked along the cover
	 * ladder predicts lambda* falling where it rises (out by 1.9-2.5x): curve-fitted, not identified.
	 * So the row records an open disagreement. The verdict does not wait on it (sized on the mean-basis
	 * criterion; lambda* crosses 1.0 only between 40 and 45 cells, still standing 5.2 discounted). Two
	 * discriminating ladders (rise, abutment) are specified and deferred in CURRENT_STATE; until one
	 * runs, every attribution here is a hypothesis.
	 *
	 * Case 22 has an oracle verdict (2026-08-15): STANDS at lambda* = 8.4149459982219277 (its own
	 * block). Production is unambiguous on both: 45 and 254 down, three and two passes, joints 3.8429
	 * and 8.2241 over before the cascade, not the zero-pass unroutability of cases 10 and 19.
	 *
	 * The pre-cascade readings are one-off SolveLoads reads (worst finite utilisation before any joint
	 * gives), different from the post-cascade FWallResult::Worst:
	 *
	 *   CASE 21 — the slow sweep's wall-21 row does this each run: printed 3.842883954008586 at
	 *           characteristic data, 0.93542327561664174 at mean data (not the predicted 0.549,
	 *           because the worst joint's governing axis flipped, a knife-edge 6.5% under the line).
	 *           Reported not asserted. The flipped axis is recorded as squeezed-edge compression,
	 *           unverified (case 9's jamb and case 22's joint both decomposed to Mohr-Coulomb shear);
	 *           decomposing it is logged in CURRENT_STATE.
	 *   CASE 22 — no sweep row (the oracle refuses the fixture), so 8.2241 was taken by hand at
	 *           characteristic data. Computed here each run: SolveWallCase reads the pre-cascade fields
	 *           pinned at 2.9133264370386338 (`FWallCase::PreCascadeWorstToday`). The collapse is three
	 *           passes and 316 down (254 at characteristic data; the marginal cascade sheds more, and
	 *           the region pins hold because Falls/Stands are containment claims).
	 *
	 * Case 21's reading is quoted as evidence, asserted nowhere; case 22's is asserted twice (past 1.0,
	 * and at its measured size). This file also asserts case 22's drop/survivor sets and pass counts
	 * and case 21's inverted DropsToday/StrandsToday zeros.
	 */

	/*
	 * CASE 21 — two courses over an eighteen-cell opening: the cover counter-case. Case 8's ruling
	 * cost the set its last case refusing arching for want of cover. User's constraint is a minimum
	 * of two courses (2026-08-12), so cover is fixed at two and span moved until the verdict clears
	 * the ceiling.
	 *
	 * Geometry, cm: six courses of 22 cells, 135 laid; cut { 1, 3, 1.75, 19.25 }, 83 live; jambs
	 * two cells, case 9's exactly; span mean 394.75; 15.00 cm (two courses) of cover. On section G's
	 * three lines:
	 *
	 *     w      2 courses x 26.67198625 N / 22.5 cm  = 2.3708432 N/cm
	 *     W      w x 394.75                           = 935.89 N  (35.1 brick weights)
	 *     M      W L / 8                              = 461.80 N.m
	 *     Z      t D^2 / 6 = 10.25 x 15^2 / 6         = 384.375 cm3
	 *     sigma  M / Z                                = 1.2014 MPa
	 *
	 * 1.50x the 0.8 MPa ceiling, 3.00x f_xk2, 12.01x characteristic f_xk1: every basis condemns it.
	 * Reconciled with case 9 via sigma ~ L^2/D (0.0889 x 13.516 = 1.2016, an arithmetic check). The
	 * flat arch that stood case 8 does not rescue it: even at the most generous rise H = 6,159 N is
	 * 0.154 MPa of shear against Mohr-Coulomb 0.209 (case 8's identical check reads 6%), 12x nearer
	 * its limit and held by the bond term DESIGN §5.2 says is gone once the joint cracks. Production
	 * drops the cover; the LP stands it through an unidentified mechanism (5.511 is 66x the bond,
	 * section G's ladder block).
	 *
	 * `Case21Falls` = { 4, 5, 1.75, 19.25 }, 35 pieces, the seatless set (every course-4 brick from
	 * cell 2 to 19 sits on cut cells, the case-20 shape; course 5 rides on it), a lower bound: where
	 * a two-course panel tears over each jamb the catalogue cannot rule. `Case21Stands` names courses
	 * 0..3 of both jambs, making this a COLLAPSE not "everything falls", so a model answering "falls"
	 * to everything fails the row. Production agrees (2026-08-13): 45 down, zero stranded, three
	 * cascade passes, worst joint 3.8429 pre-cascade. The zero/three separate it from cases 10 and 19
	 * (zero-pass unroutability); the three is asserted so this cannot become that.
	 */
	const FWallRegion Case21Cuts[] = { { 1, 3, 1.75, 19.25 } };
	const FWallRegion Case21Falls[] = { { 4, 5, 1.75, 19.25 } };
	const FWallRegion Case21Stands[] = { { 0, 3, -1.0, 1.75 }, { 0, 3, 19.25, 22.0 } };

	/*
	 * CASE 22 — case 9's cover over a thirty-five-cell opening: the span counter-case. Case 9's
	 * fixture with only the span moved (same brick, mortar, bond, toothed reveal, eight courses of
	 * cover, two-cell jambs, same cut); only the opening is wider.
	 *
	 * Geometry, cm: twelve courses of 39 cells, 474 laid; cut { 1, 3, 1.75, 36.25 }, 371 live; span
	 * mean 777.25 (3.62x case 9); 60.00 cm of cover, case 9's to the millimetre. Arithmetic:
	 *
	 *     w      8 courses x 26.67198625 N / 22.5 cm  = 9.4833730 N/cm
	 *     W      w x 777.25                           = 7,371.5 N  (276.4 brick weights)
	 *     M      W L / 8                              = 7,161.3 N.m
	 *     Z      t D^2 / 6 = 10.25 x 60^2 / 6         = 6,150 cm3
	 *     sigma  M / Z                                = 1.1644 MPa
	 *
	 * 1.46x the 0.8 MPa ceiling, 2.91x f_xk2, 11.64x characteristic f_xk1. Reconciliation is exact
	 * with cover held (sigma ~ L^2: 0.0889 x 13.098 = 1.1644). The pair against case 9 is a genuine
	 * one-variable pair separating on outcome, the first since 2026-08-12: 0.089 MPa standing vs
	 * 1.164 falling from span alone. Deep-beam action still holds (span/depth 13.0, an ordinary beam
	 * W L/8 prices, if anything charitable).
	 *
	 * `Case22Falls` = { 4, 4, 1.75, 36.25 } (the seatless course) + { 5, 11, 6.25, 31.75 } (the core
	 * of the seven above, 4.5 cells clear of each reveal where a stepping edge can hang), a lower
	 * bound, 214 of the 254 drops. The second bound was tightened 2026-08-13: the drop set narrows
	 * half a cell a course (the corbel step), so 6.25..31.75 gives a whole cell of margin. `Case22Stands`
	 * names courses 0..3 of both jambs, tighter than case 21 by one piece a side (the top odd-course
	 * jamb brick is half seated and drops). Production agrees (2026-08-13): 254 down, zero stranded,
	 * two cascade passes, worst joint 8.2241 pre-cascade. The two is asserted (collapse arm requires
	 * a given joint, or this is cases 10/19 again).
	 *
	 * The oracle answered this fixture for the first time 2026-08-15 (its earlier refusal was measured
	 * at characteristic strengths, one day before the mean re-anchor): lambda* = 8.4149459982219277,
	 * 371 blocks / 900 joints, 88,810 pivots, `bAnswered` true. So the third method dissents (catalogue
	 * and hand statics/production agree, LP stands at 8.41x), case 21's shape on the same fixture with
	 * the same rigid-plastic scope limit (DESIGN §8). The verdict does not move: PROMOTION_DESIGN §4
	 * shows no brittleness treatment brings a reading this size under 1.0, and case 21 is the precedent.
	 * A sweep row (OracleStandsProductionFalls with a window around lambda*) is specified not built,
	 * doubling an opt-in group's cost for a second reading of case 21's disagreement. Hazard: 88,810
	 * pivots is 89% of MaxPivots 100,000, so any pivot-path change could flip it to a refusal; nothing
	 * watches it, CURRENT_STATE carries it. The old extrapolation ("near 2.6") had the direction right
	 * and the number wrong (measured 8.41 is 3.2x), on data the re-anchor moved underneath it.
	 */
	const FWallRegion Case22Cuts[] = { { 1, 3, 1.75, 36.25 } };
	const FWallRegion Case22Falls[] = { { 4, 4, 1.75, 36.25 }, { 5, 11, 6.25, 31.75 } };
	const FWallRegion Case22Stands[] = { { 0, 3, -1.0, 1.25 }, { 0, 3, 36.75, 39.0 } };

	/**
	 * The catalogue, built rather than aggregate-initialised so every field is named at its value.
	 * Twenty-two cases, twenty-one rectangles with holes: 1-5 the deletions that fire on a click,
	 * 6-10 doorways, 11-12 a wall on piers, 13-16 corbelling, 17-18 the bond, 19-20 where collapse is
	 * right, 21-22 openings too big for their cover.
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
		 * Case 3 is the one a player reported; user ruled 2026-08-06 that a free-end deletion must not
		 * fell the wall. ARCHING_DESIGN.md slices 1-4 don't reach it (the surviving brick overhangs
		 * with nothing to abut, so the arch is refused and the cantilever ladder starts), so it stays
		 * red until composite vertical action lands. Not a defect in the row.
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

		/*
		 * The wall the other three rows of section B compare against. As of 2026-08-12 none of the
		 * three pairs separates on outcome (cover via case 8, span and abutment via cases 9 and 10);
		 * the file header lists where each discrimination went.
		 */
		Add(7, TEXT("Four-brick opening, eight courses over"), EVerdict::Stands,
			CoveredCourses, StandardCells, FourCellOpening, {}, {},
			TEXT("span vs 9 — NOW IN THE READING; cover vs 8 and abutment vs 10 NO LONGER SEPARATE"));

		/*
		 * STANDS, re-ruled 2026-08-11 (the catalogue moved). Asked LOCAL LOSS on "one course cannot
		 * arch"; LP stands it at lambda* = 324.73 through head-joint compression, production drops
		 * nothing, user ruled STANDS (DESIGN §8). Full derivation in the CASE 8 block.
		 */
		Add(8, TEXT("Four-brick opening, one course over"), EVerdict::Stands,
			5, StandardCells, FourCellOpening, {}, {},
			TEXT("depth of cover against case 7 — NO LONGER SEPARATES, on either outcome or reading"));

		/*
		 * STANDS, re-ruled 2026-08-12 (goes green). Asked COLLAPSE on the arching gate; LP stands it
		 * at lambda* = 36.56, hand deep-beam 0.88x characteristic, production already at 0.985. Three
		 * methods, the catalogue the outlier. Full derivation in the CASE 9 block.
		 */
		Add(9, TEXT("Ten-brick opening, eight courses over"), EVerdict::Stands,
			CoveredCourses, 14, Case9Cuts, {}, {},
			TEXT("span against case 7 — NOW IN THE READING, not the outcome"));

		/*
		 * The three known reds each characterise today's wrong answer (FWallCase::DropsToday), and
		 * since 2026-08-12 they don't all point the same way: 20 is a LOCAL LOSS of two teeth the
		 * model over-drops; 10 and 19 are STANDS rows the model drops 12 and 34 on (inverted red).
		 * All go green at DESIGN §7 evolution step 4. Cases 12, 8, 9 left by their expectation moving.
		 */
		/*
		 * GREEN since slice 3b/4 (2026-08-27): below the 200-block cap the equilibrium LP is the sole
		 * break authority, stands this wall (lambda* 111.5) and carries the panel the router could
		 * only strand, so production now agrees with STANDS. Old `DropsToday = 12` / `StrandsToday = 3`
		 * deleted per FWallCase::DropsToday. The survivor region stays as a tautological identity pin.
		 */
		Add(10, TEXT("Opening at a free end, no abutment"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case10Cuts, {}, Case10Stands,
			TEXT("abutment against case 7 — NO LONGER SEPARATES; see the CASE 10 block"));

		/* C — spanning between supports. */

		/*
		 * STANDS, ruled to LOCAL LOSS then back the same day (2026-08-08); the second is the keeper.
		 * The local-loss reading was BS 5977's 300-mm gate, which this fails by its whole height;
		 * honest physics is a deep beam (span/depth 2.3, ~0.03-0.04 MPa), well under bond. Full
		 * derivation above Case11Cuts.
		 */
		Add(11, TEXT("Wall on two piers, six-brick clear span"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case11Cuts, {}, {},
			TEXT("pier width against case 12 — IN THE MARGIN, not the outcome"));

		/*
		 * STANDS, rewritten and re-ruled 2026-08-09. Old row cut ten of twelve cells, varying span
		 * and pier at once; this is case 11's span on a one-cell pier, and the arithmetic above
		 * Case12Cuts says the pier takes the thrust (~400 N against 4-8x bonded capacity). The
		 * rigid-body overturning reading is rejected here for the third time (cases 14, 16 the others).
		 */
		Add(12, TEXT("The same span on a one-brick pier"), EVerdict::Stands,
			CoveredCourses, StandardCells, Case12Cuts, {}, {},
			TEXT("pier width against case 11 — IN THE MARGIN, not the outcome"));

		/* D — corbelling, and the header that is held down by what sits on it. */

		{
			FWallCase& Case = Add(13, TEXT("Corbel, quarter brick per course"), EVerdict::Stands,
				10, CorbelCells, {}, {}, {},
				TEXT("corbel projection against case 14 — IN THE READING, not the outcome"));

			Case.CorbelFromCourse = CorbelFirstCourse;
			Case.CorbelStepCm = QuarterBrickStepCm;
		}

		/*
		 * STANDS, per the user's 2026-08-07 ruling. Drafted as a collapse of the four projecting
		 * bricks; the bottom rung reads 0.195160875 of f_xk1, nothing near 1.0. Full derivation
		 * above section D.
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
		 * STANDS, per the catalogue's 2026-08-07 revision. Drafted as a local loss of the projecting
		 * header; its bed joint reads 0.058203838 of f_xk1, a sixth of capacity. Full derivation
		 * above section E.
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

		/*
		 * STANDS, re-ruled 2026-08-12. Asked COLLAPSE of everything over 135 cm of missing footing;
		 * LP stands it at lambda* = 12.38 (lowest of the fifteen, still above 1), production drops 34
		 * without breaking a joint (unroutability, not strength). Closest call of the three ruled that
		 * day; full derivation in the CASE 19 block.
		 */
		/*
		 * GREEN since slice 3b/4 (2026-08-27), like case 10: below the cap the equilibrium LP stands
		 * this wall (lambda* 48.0) and carries the half the router could only strand, so production
		 * agrees with STANDS. Old `DropsToday = 34` / `StrandsToday = 6` deleted. Survivor region
		 * stays as an identity pin.
		 */
		Add(19, TEXT("Bottom course out under half the wall"),
			EVerdict::Stands, 10, StandardCells, Case19Cuts, {}, Case19Stands, nullptr);

		/*
		 * Re-ruled STANDS at slice 4 (2026-08-27), the doubt settled by the LP. Through 2026-08-12 a
		 * LOCAL LOSS of two teeth production over-answered by dropping nine; the equilibrium LP stands
		 * the whole wall (lambda* 218.42) and holds the two teeth ~129x clear, so the true count is
		 * zero (the two-tooth loss was the per-joint heuristic's error). `DropsToday = 9` deleted.
		 *
		 * STANDS not a row-21 inverted red because the LP and production agree here (unlike row 21,
		 * whose COLLAPSE the LP contradicts). A genuinely-local two-tooth mechanism, if one exists,
		 * needs per-region interrogation the global solve can't express (PROMOTION_DESIGN §3.5, §12
		 * D7), a later slice. The teeth are named as the survivor region to pin their identity.
		 */
		Add(20, TEXT("Staircase void"), EVerdict::Stands,
			CoveredCourses, 14, Case20Cuts, {}, Case20Falls, nullptr);

		/* G — openings too big for what covers them. The set's only two falling verdicts. */

		/*
		 * COLLAPSE, user-directed 2026-08-12, and since the 2026-08-14 mean re-anchor an inverted red
		 * joining rows 10 and 19: the ruled Collapse stands (hand deep-beam 1.2014 MPa is 1.71x mean
		 * f_x1) but at mean strengths the model now STANDS it, pre-cascade worst 0.93542327561664174
		 * (not the predicted /7 = 0.549 because the worst joint's governing axis flipped, unverified),
		 * 6.5% under the line. The cause is the composite-relief gap: production's tension sigma sits
		 * 3.1x below the hand sigma. Goes green when that closes (evolution step 4 or the composite/f_x2
		 * rework), never by weakening this row.
		 *
		 * The pins hold the wrong answer at its measured size (rows-10/19 convention): `DropsToday = 0`,
		 * `StrandsToday = 0`, `Case21Stands` naming the jambs, so a regression fails loudly. The LP's
		 * stand (17.24 mean, 5.511 characteristic, a rigid-plastic scope limit) agrees with production,
		 * both disagreeing with the ruled Collapse, which is what this inverted red records.
		 *
		 * `Isolates` says "not a pair" deliberately: against case 9 this moves both cover (60 to 15 cm)
		 * and span (1.84x), a fixture where thin cover is decisive. Case 22 is the genuine one-variable
		 * article (case 9's cover, only the span moved).
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

		/*
		 * COLLAPSE, user-directed 2026-08-12, the catalogue's one green Collapse row. Case 9's eight
		 * courses of cover over a span grown 3.62x, so the pair varies span alone: 1.1644 MPa is 1.66x
		 * mean f_x1.
		 *
		 * The margin is measured and pinned, not the fallout plan's /7 = ~1.175: the flip-measured
		 * (2026-08-14) reading is 2.9133264370386338, 2.5x, because the worst joint's axis flipped to
		 * Mohr-Coulomb shear (sliding under 35,706 N of arch thrust, flexural tension zero, so /7 could
		 * never re-base it). Still collapses in three passes shedding 316 pieces (254 at characteristic
		 * data). Pinned at `FWallCase::PreCascadeWorstToday`.
		 *
		 * The set's only outcome pair since the 2026-08-12 rulings retired the old five. The oracle
		 * answered 2026-08-15 and stands it at lambda* = 8.4149459982219277, a third LP-vs-catalogue
		 * disagreement of case 21's shape; the CASE 22 block carries it.
		 */
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
	 * The twenty-two as playable levels: what a scenario row for one of these cases must be. The
	 * configurations are the user's, and until now only the fixture bricklayer above could lay them,
	 * which lives in a test file. The three tests at the bottom are the acceptance criteria for
	 * moving the geometry into production and leaving the verdicts here.
	 *
	 * The verdicts do not move: `EVerdict`, `MustFall`, `MustStand`, `Isolates` are claims about what
	 * the solver ought to conclude. A level needs only the geometry and cuts; moving an expected
	 * outcome into production would be putting an assertion there.
	 */

	/**
	 * A case's level name, the number not the title: a scenario name is typed on a URL and a map
	 * name is a filename, so both want to be short and punctuation-free (the prose title is carried
	 * in `Title`). Two digits so `wall-2` cannot read as `wall-20`.
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
	 * The one machine-checkable claim a level's caption makes. The caption stays prose, but the
	 * verdict it claims cannot be (a level captioned with an outcome the solver doesn't produce lies
	 * to a player at the counter-example). So it carries one token naming its verdict, spelled the
	 * way this file spells verdicts.
	 */
	FString VerdictClaimFor(EVerdict Verdict)
	{
		return FString::Printf(TEXT("Expected: %s"), VerdictName(Verdict));
	}

	/**
	 * The token a caption carries when the model disagrees with its own claim. A caption naming only
	 * the expected verdict on a red row is worse than silence, so the honest ones say both and the
	 * agreeing ones must not, so the admission doesn't outlive the disagreement. Since 2026-08-12 it
	 * no longer implies "too optimistic": cases 10 and 19 are `Expected: STANDS` yet drop 12 and 34.
	 */
	const TCHAR* const ModelDisagreesMarker = TEXT("THE MODEL CURRENTLY DISAGREES");

	/**
	 * Whether the model produced the verdict this row claims. The same three shapes
	 * `Acceptance.Wall.Catalogue` asserts one at a time, but a second reading not a shared one so
	 * each catalogue assertion prints its own diagnosis. Held against the catalogue by the known-red
	 * tripwire in the caption test, so a drifted predicate fails there not by captioning a level wrongly.
	 *
	 * The `Stands` arm decides cases 10 and 19 since their 2026-08-12 re-ruling: both ruled STANDS,
	 * both drop pieces, so `Fallen.Num() == 0` is false and it reports a disagreement (this arm's first
	 * false ever). The `default` (Collapse) arm first ran 2026-08-13 with cases 21 and 22 (dead code
	 * before), deciding them by `MustFall` contained in the fallen set and `MustStand` disjoint; its
	 * bite is proven by the survivor-widening mutation on case 21 (TRAPS.md).
	 *
	 * It also carries the pass-count claim on both falling shapes: a Collapse reaching the ground in
	 * zero breaking passes is cases 10/19's unroutability wearing a collapse's outcome, so agreeing
	 * would tell a player the wall failed on strength when no joint broke. `CutPasses` is structurally
	 * 0 on a no-cut row, so it also refuses a Collapse that cuts nothing (no such row exists).
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
	 * Compare a laid layout against the fixture's wall, brick for brick and in handle order. Handle
	 * order is part of the claim and the half that looks fine when wrong: a different lay sequence
	 * renumbers every joint while geometry still checks, and every reading here is about a particular
	 * joint index. Only the first disagreement is reported, so a fully-moved producer doesn't bury
	 * everything in thousands of failures.
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
 * The acceptance set itself: every case in the catalogue, its own verdict, one row at a time. The
 * assertion is on outcome, its shape following the verdict (file header for why the middle verdict
 * exists and why displacement is never used). A failure names the case, its isolated variable, and
 * both fallen sets in (course, cell) terms.
 *
 * Most rows are expected red: these are the acceptance criteria for claude_plans/ARCHING_DESIGN.md,
 * not a regression net. The signature CURRENT_STATE.md predicts is every falling half passing while
 * every standing half fails, what a model that only says "falls" looks like.
 *
 * Several rows are green on arrival, flagged so none is mistaken for work this suite drove. 1 and 17
 * are intact-wall anchors. 18 is a stack-bond column at a few percent (its verdict passes but the
 * interesting property has its own test below). 8, 9, 12, 13, 14, 16 all stand, a correction not an
 * achievement (each section-B block carries its arithmetic and doubts).
 *
 * Since 2026-08-12 two rows are red in a new direction: cases 10 and 19, re-ruled COLLAPSE to STANDS,
 * drop 12 and 34 pieces in zero passes, so the STANDS "nothing left" half fails. Reading these as
 * "wanted it harder" is backwards: the wall is ruled to stand and the solver dropped a third of it.
 *
 * The pairs those corrections stopped separating have each moved onto what still separates them,
 * except 7 vs 8 and 7 vs 10 (losses, not relocations); the file header lists all five. No ticking
 * world; see the file header.
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
	 * The profile this file's arithmetic assumes, checked not imported: every expected value below
	 * follows from these three figures, so a retune turns this row red rather than silently moving
	 * every case.
	 */
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
			/*
			 * Both halves: "nothing fell" alone passes a wall that severed every joint over the cut
			 * and stayed leaning together; "no joint gave" alone passes a wall whose pieces were
			 * never in the load path.
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

			/*
			 * On a no-cut row the intact solve is the only solve. `CutPasses` is structurally zero on
			 * the six no-cut rows (1, 13-17) since the block that sets it is inside
			 * `if (Case.Cuts.Num() > 0)`, so the "no joint gave" half asserted nothing on exactly the
			 * rows whose as-built state is the case. `CheckWallFixture` makes this harder on the
			 * cutting rows; this is the same claim on the rows it skips, a verdict here not a fixture
			 * check.
			 */
			/*
			 * A STANDS row's survivor region is asserted too, useful only while red. STANDS already
			 * implies nothing falls, so this can't fail on a passing row. It buys the identity on cases
			 * 10 and 19: ruled to stand, dropping 12 and 34, the count-based line above reads the same
			 * for any twelve or thirty-four. Asserted inverted (the named masonry contains no fallen
			 * piece). The section-B blocks record the drop sets checked against; these become
			 * tautologies at evolution step 4.
			 */
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
			/*
			 * Identity, not a count: a bounded loss means the named set, so the same number falling
			 * elsewhere is a different, wrong answer a counting test would pass.
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
			 * Two-sided: a wall down because everything is down says nothing about the isolated term.
			 * The named survivor region stops a model that always answers "falls" from passing every
			 * collapse row free; the last row that couldn't name one (old case 12) was rewritten out
			 * 2026-08-09.
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

			/*
			 * And the collapse must have been a strength verdict, the claim both section-G blocks rest
			 * on, unasserted until 2026-08-13. Cases 10 and 19 are why: both drop a third of a wall in
			 * zero passes with no joint near capacity (unroutability, not failure), so the user re-ruled
			 * them STANDS. The two assertions above can't tell those apart, so the pass count is
			 * asserted here. One-sided on purpose: at least one joint gave, not how many passes (today
			 * three for case 21, two for case 22, in their blocks). Proven to bite by the TRAPS
			 * `ReseatSpannedGroups` early-return mutation: both rows go stranded-heavy and fire at
			 * "0 breaking pass(es)", exactly the reading this refuses.
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
		 * The three known reds are pinned to today's wrong answer. Not a physics claim (the row above
		 * has already failed); it adds that the failure has a fixed size, since a red row is otherwise
		 * a hole in the net (case 20 fails identically whether it drops nine or ninety). Measured
		 * 2026-08-09, one number per red row.
		 *
		 * The pin keeps the 2026-08-12 re-rulings honest: cases 10 and 19 hold their failure at 12 and
		 * 34 exactly rather than drifting while red. A count is not a shape (a routing change moving
		 * the drop set without resizing it passes every count); identity is held by the survivor region
		 * above, complementary since the pin sees a failure grow and the region sees it move. A fixed
		 * row fails here on purpose: the fixing slice deletes `DropsToday`, the caption marker and the
		 * known-red entry in the same edit (case 9 went that way 2026-08-12).
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
		 * A row may also pin the pre-cascade margin, not a count. First assertion: the wall really was
		 * past capacity when it went (a Collapse from under 1.0 is pieces the router lost, `StrandsToday`'s
		 * trap one layer up). Second: the margin at its measured size, so a change taking case 22 from
		 * 1.17x to 1.01x (still collapsing, one retune from inverting the only green Collapse row) fails
		 * loudly.
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
 * Cases 7 and 9, the part a verdict cannot say: widening an opening changes what its jamb is doing,
 * not merely how hard, and both walls stand anyway.
 *
 * 7 vs 9 was the catalogue's last outcome pair until the user re-ruled case 9 to STANDS 2026-08-12
 * (CASE 9 block), making the "greater half loses more" claim unsatisfiable and retiring
 * `Acceptance.Wall.MatchedPairs`. This is where the span discrimination went, a stronger reading:
 * by how much and in which direction, not merely "these differ".
 *
 * Not the 7-vs-8 trap, the standard this had to clear. The cover pair 7 vs 8 was deleted, not
 * relocated: production reads case 7 at 0.269 and case 8 at 0.219, separating 1.23x the WRONG way
 * (a downward-routing solver reads cover as load, not arch capacity). This row clears it on three
 * counts:
 *
 *   Direction is right: a longer span delivers a larger abutment reaction, and the wider jamb
 *   carries more (normal force 56,010.7 uu vs 106,676.3 uu, no strengths involved).
 *
 *   Same kind of joint both sides: both are the same deep beam, both worst joints bed joints in the
 *   right jamb near the head. The pair varies span and cell count (12 vs 14) and nothing else. The
 *   failure mode differs since the 2026-08-14 re-anchor, the pair's finding (axis section below).
 *
 *   Neither reading is off a wall the router failed on (both drop/break/strand nothing). This
 *   disqualified the abutment pair 7 vs 10 (case 10's 0.300 comes dropping twelve in zero passes);
 *   that discrimination went to the LP oracle (296.22 vs 35.82, 8.27x).
 *
 * The magnitudes are a characterisation: outputs of the whole-wall composite-depth walk, not
 * re-derivable without re-implementing the thing under test, so pinned as measured.
 *
 * The span-law arm is retired (2026-08-14). It asserted `ReadingRatio >= SpanRatio` on a deep
 * beam's reaction growing at least linearly with span, which doesn't survive the axes parting (a
 * ratio on different axes is two reactions times two unrelated strengths). The span law holds on
 * none of three workings (worst-axis stresses, same axis, honest reaction 1.9046x for 2.6928x of
 * span, sublinear). Its replacement needs no strength: the wider opening puts 270,024 uu of thrust
 * through its jamb where the narrow puts none, a span term the model has.
 *
 * The governing axes, named and derived. Each worst joint is asserted by (course, cell) first, and
 * they differ: case 7's c2/8-c3/7.5, case 9's c3/11.5-c4/11, both in the right jamb near the head.
 * Since 2026-08-14 the axis is asserted too, because prose got it wrong (DESIGN.md §6, CURRENT_STATE
 * and this file called case 9's post-flip reading squeezed-edge compression, which reads 13x below
 * production). It is Mohr-Coulomb shear:
 *
 *     case 7   |M_y| / (t.D^2/6) / f_x1, composite deep-beam section (D = 35.128 cm)
 *              = 0.038491547555641249 exactly — tension
 *     case 9   |F_xy| / A / (f_v0 + mu.|F_z|/A), bare Mohr-Coulomb on the joint's own patch
 *              = 0.26329211195559277 exactly — shear, the jamb sliding under arch thrust
 *
 * Both asserted to 1e-9 below, one formula over published quantities and strengths.
 *
 * The old flap watch is kept but vacuous: production once read case 9's jamb at 0.98502040901419818,
 * one retune from 1.0; at mean strengths the axis moved and it sits ~3.8x clear. It stays because
 * the day it crosses, case 9's STANDS becomes a catalogue red and the sweep's `AgreeStands` flips,
 * but it now watches a shear reading.
 *
 * Green on arrival: pins behaviour the model already produces, existing because the case 9 ruling
 * deleted an assertion (and, via MatchedPairs, a whole test). No ticking world; see the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceSpanTest,
	"DestructionGame.Acceptance.Wall.SpanIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceSpanTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/* Slack against a re-association of a few doubles, as the corbel/header reading tests use it.
	 * The smallest difference either row should see is in the second decimal place. */
	constexpr double UtilisationTolerance = 1.0e-9;

	/*
	 * The Mohr-Coulomb triple, checked not imported: this test names each reading's axis, only as
	 * good as the strengths it divides by. Catalogue pins the two already used; the other three are
	 * pinned here, the only place that reads them.
	 */
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

	/* Short-circuited deliberately: a case 7 that could not be laid says nothing about case 9. */
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

	/*
	 * Neither wall breaks a joint either, which the header claimed but had not asserted. "Both drop,
	 * break and strand nothing" is why these readings are honest readings of a fully routed structure,
	 * the reason this pair could relocate where 7-vs-10 could not. Dropped and stranded are above; the
	 * broken half was prose.
	 */
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
	 * --- the magnitudes, pinned as measured ---------------------------------------------------
	 *
	 * Mean re-anchor (2026-08-13), measured at the flip (2026-08-14): the pair's two readings parted
	 * axes, what TRAPS' strength-basis entry warns about. Case 7's jamb stayed tension-governed and
	 * reads the old pin / 7. Case 9's tension fell to zero (production clamps the moment at the kern)
	 * and the shear axis came forward to the 0.26329211195559277 pinned below. The axis is derived and
	 * asserted below not named in prose, because prose called it squeezed-edge compression, which
	 * reads 0.0203072, 13x under production.
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

	/*
	 * The crossing is watched separately from the value: the pin above fails on any drift, this one
	 * fails in the words of the event that matters, so the log says what it means.
	 */
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

	/**
	 * The two joints' loads, straight off production's accessors. Nothing re-implements the solver:
	 * GetConnectionForce/Moment/CompositeDepthCm report what the routing layer decided, and this
	 * file applies one formula per axis against the published strengths, enough to say which axis
	 * ComputeUtilisation returned (a bare utilisation cannot).
	 */
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

	/*
	 * Case 7 is flexural tension on the composite section: the deep beam over the bed joint has
	 * section t.D^2/6 (t = 10.25 cm, D the depth production reports), stress |M_y| over it, capacity
	 * the mean flexural bond.
	 */
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

	/*
	 * Case 9 is Mohr-Coulomb shear on the joint's own patch: the jamb pushed sideways by arch thrust,
	 * shear stress the in-plane force over area, capacity cohesion plus friction times compression.
	 * No bending or composite section, why calling it a bending/compression reading was wrong.
	 */
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

	/*
	 * The clear spans, computed from the cell grid: a gap of n cell pitches between surviving brick
	 * centres leaves n * CellPitch - BrickLength of open air. Each toothed reveal gives two values
	 * (even and odd courses stop at different cells); the mean is what the beam spans.
	 */
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

	/*
	 * The span term, on a strength-free quantity. The narrow jamb carries a purely vertical force;
	 * the wide one carries ~2,700 N of thrust too, a different mechanism, not a bigger number. What
	 * the retired `ReadingRatio >= SpanRatio` row couldn't express: a solver with no span term
	 * delivers no thrust and fails here, and no strength can satisfy it since none appears.
	 */
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

	/*
	 * The reaction grows too, strength-free: the normal force through the joint is the reaction the
	 * cover delivers into that jamb, the closest like-for-like the two walls have.
	 */
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
	 * The sublinearity is pinned as a finding, not asserted as physics: w.L/2 wants at least linear;
	 * the model grows the reaction 1.9046x for 2.6928x of span, so the published relation fails here.
	 * Pinned so the day equilibrium promotion (DESIGN §7 step 4) gives the reaction an honest span
	 * term, this fires and someone re-derives.
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
 * Cases 13 and 14, the part a verdict cannot say: doubling the corbel step doubles the joint
 * reading, and both corbels stand anyway.
 *
 * 13 and 14 were an outcome pair until the user ruled 2026-08-07 that a bonded corbel resists with
 * its full depth, making case 14's collapse wrong. Both halves now stand, so this moves the pair
 * onto a quantity that still separates them, stronger than an outcome pair.
 *
 * The expected value is derived from the fixture not read off the solver: case 14's four corbelled
 * courses each keep one 10.25 x 10.25 bed patch, mass 5.625 cm outboard, so the ladders and section
 * are written above from the grid, density and f_xk1. A wrong constant makes this disagree.
 * Cross-checked against two figures this file did not produce: the same four lines give 0.21858 at
 * five steps (ARCHING_DESIGN's 0.219) and 0.36903147272727271 at eleven
 * (Core.Structure.AStaircaseVoidCondemnsTheCorbel), held at 2% and 1e-9.
 *
 * The thing measured must be the thing governing: the worst joint is asserted to be the bed joint
 * under the lowest corbelled course before its magnitude is read.
 *
 * Green on arrival: pins behaviour arching slice 5 already produces, existing because the ruling
 * deleted an assertion. No ticking world; see the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceCorbelProjectionTest,
	"DestructionGame.Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceCorbelProjectionTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/* Slack against a re-association of a few doubles. One course of depth moves the four-step
	 * reading 0.1735 to 0.2186, seven orders above this tolerance. */
	constexpr double UtilisationTolerance = 1.0e-9;

	/* --- the derivation, cross-checked against two figures produced elsewhere ---------------- */

	/* Both cross-check figures are the characteristic-basis publications / 7 (the 2026-08-14 mean
	 * re-anchor moved f_x1 0.10 -> 0.70 and these ladders are pure tension). */
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

	/* Short-circuited: a case 13 that could not be laid says nothing about case 14. */
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

	/*
	 * The outcome follows from the number: 0.195 is a fifth of capacity, and "stands" is because of
	 * this. The `Worst < 1.0` row was deleted 2026-08-09 as a tautology: `Worst` skips given joints
	 * and SolveAndBreak's last pass broke nothing, so every joint left is at most 1.0; it could only
	 * fail at exactly 1.0, which the exact-value assertion above already catches louder.
	 */

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
	 * Twice, not merely more: a strict inequality alone is satisfiable by a last-bit difference. The
	 * step doubles, the arm doubles with it, measured separation 2.8x; two is the floor.
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
 * Cases 15 and 16, the part a verdict cannot say: putting six courses on a header's tail divides
 * what its own bed joint reads by thirty, and both headers stand anyway.
 *
 * 15 and 16 were an outcome pair until the 2026-08-07 revision made case 16 stand too (an uncracked
 * bonded bed joint carries a half-cell projection at 0.0582 of f_xk1; the local-loss verdict was a
 * rigid-body overturning reading, case 14's correction the same day). Both now discriminate nothing,
 * so this moves the pair onto a quantity that still separates them.
 *
 * The two walls are the same wall with the header at a different height (one course pushed half a
 * cell out, closed with a full brick): case 15 course 3 (six courses on the tail), case 16 course 9
 * (nothing). The header's bed joint geometry is identical, a one-variable comparison.
 *
 * The expected value is derived from the fixture not read off the solver: seat length, arm, patch
 * modulus t.L^2/6, weight and f_xk1, none imported. Cross-checked against two figures this file did
 * not produce: WALL_CASES.html's 0.058204 (at 1e-6) and the waist anchor 0.058203838191552663 that
 * Core.Structure.AdoptedWallLoadsItsWaistEccentrically pins on a different fixture.
 *
 * The thing measured must govern: a ten-course wall's base compression could carry the worst joint
 * (case 17 reads 0.00109 at its foot), so case 16's worst joint is asserted to be the header's own
 * bed joint before its magnitude is read. Case 15's is not pinned; instead an upper bound (its worst
 * joint is at most a tenth of case 16's), stronger than identity and surviving a later migration.
 *
 * Green on arrival: pins behaviour the model already produces, existing because the case 16 revision
 * deleted an assertion. No ticking world; see the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceSuperimposedLoadTest,
	"DestructionGame.Acceptance.Wall.SuperimposedLoadIsReadInTheJointNotInTheOutcome",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceSuperimposedLoadTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/* Slack against a re-association of a few doubles. The quantity is 0.058, seven orders above
	 * this tolerance. */
	constexpr double UtilisationTolerance = 1.0e-9;

	/* --- the derivation, worked forward from the grid ---------------------------------------- */

	/*
	 * What the header keeps and what hangs over. CourseGeometry pushes the right face out half a cell
	 * and closes with a full brick, so of the brick's 21.5 cm exactly 11.25 cm is over air and 10.25
	 * cm bears below. (Matching the wall depth is a coincidence of proportions, not a relation.)
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

	/* Both cross-checks are the characteristic-basis figures / 7 (mean re-anchor 2026-08-13; the
	 * html's 0.058204 is the characteristic-era quote). */
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

	/* Short-circuited: a case 15 that could not be laid says nothing about case 16. */
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

	/*
	 * Where the header and its seat are, in cells, walked off the bricklayer. The projecting course's
	 * rightmost brick is full, right face half a cell past the flush face; the even course below is
	 * flush and full. Both centres are half a brick in from their right faces.
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
	 * The outcome follows from the number rather than the other way round: 0.058 is a sixth of
	 * capacity, and the catalogue's "stands" verdict is because of this. The `Worst < 1.0` row that
	 * used to say so was deleted 2026-08-09 as a tautology, for the reason written out beside case
	 * 14's copy in Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome. The verdict
	 * follows from the exact-value assertion above, which pins 0.058203838191552663 to 1e-9.
	 */

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
	 * Three times, not merely more: a strict inequality alone is satisfiable by a last-bit difference.
	 * Measured separation 4.51x, the loaded joint's tension driven to zero and compression governing.
	 * The floor was ten (measured 32) in the characteristic era; the 2026-08-13 re-anchor shrank it by
	 * the /7 the bare reading moved, the loaded side being compression-governed (the ratio isn't
	 * axis-invariant, TRAPS strength basis). Three is the re-derived floor, capped above by the wall's
	 * base compression (0.00109) at ~7.6.
	 *
	 * `Worst` for case 15 is the worst joint anywhere, so bounding it above bounds the header's joint
	 * regardless of which carries the maximum, safe against a later slice relieving the header.
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
 * Case 18, the part a verdict cannot say: a hanging stack-bond column carries the same fraction of
 * its head joints' shear capacity at any height.
 *
 * Case 18's verdict is "stands", which is easy (the column sits at a few percent), so the catalogue
 * row passes even with the load path wrong. The trustworthy property is that the ratio doesn't move
 * with height: each course adds one brick of weight and two head joints, which cancel. Two heights
 * agreeing is far stronger than one, since a model piling the whole column onto its foot also reads
 * a plausible few percent and grows linearly with height, which one height can't see.
 *
 * The arithmetic, worked independently. A full brick weighs 2667.198625 uu. A stack-bond brick with
 * its bed joint cut is bonded to a neighbour each side over a head joint of 66.625 cm2, load
 * parallel, so it is shear split between two joints:
 *
 *     2667.198625 / (2 x 66.625) / 10000 uu per MPa.cm2  =  0.00200165 MPa
 *     0.00200165 / 0.9 MPa (mean f_v0)                   =  0.0022240556
 *
 * (0.01000825 on the retired characteristic f_vk0 = 0.2, a clean x2/9 since capacity is bare
 * cohesion.) No friction: a vertical load on a vertical joint puts no compression across it. This
 * disagrees with the brief's 0.0200 (WALL_CASES and CURRENT_STATE divided by flexural bond, but the
 * joint is in shear).
 *
 * Heights are 12 and 20 because the head joint must be worst at both, which it wasn't at the old 10
 * and 16 (measured 2026-08-14). `FWallResult::Worst` is the worst joint anywhere, so this test is
 * only about the column while the column reads worst. At the re-anchor the head reading moved x2/9
 * with f_v0 while the bed joint beside the hole didn't, so they crossed:
 *
 *      courses   head joint    bed joint c5/6-c6/6   governs
 *          7     0.00222406    0.00906354            BED
 *          8     0.00444811    0.00904987            BED
 *          9     0.00667217    0.00903624            BED
 *         10     0.00889622    0.00902265            BED    <- the old lower rung, vacuous
 *         11     0.01112028    0.00900910            head, by 1.23x
 *         12     0.01334433    0.01038752            head, by 1.28x
 *         16     0.02224056    0.01720933            head, by 1.29x
 *         20     0.03113678    0.02395029            head, by 1.30x
 *         30     0.05337733    0.04045825            head, by 1.32x
 *
 * The head reading is (courses - 6) x the per-pair figure exactly (the cut is in course 5, so n
 * courses hang n - 6 bricks and the model puts all on the foot's head-joint pair). The bed
 * competitor grows sublinearly, so the head takes over at eleven courses. 12 and 20 are both clear
 * of the crossing, margin asserted below.
 *
 * Red are the absolute rows and the height-independence row (the model reads 6x and 14x the per-pair
 * figure); the characterisation beside them passes, pinning the wrong answer at (courses - 6) x that
 * figure, the defect being the column's whole weight arriving at its foot. No ticking world.
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

	/* Slack against a re-association of a few doubles. The value is ~0.01 and the model's answer is
	 * several times it, four orders of headroom. */
	constexpr double UtilisationTolerance = 1e-9;

	/**
	 * The course the cut is in, read off Case18Cuts. A wall of n courses hangs n - (CutCourse + 1)
	 * bricks, the per-pair multiplier; deriving it from the cut stops the characterisation below
	 * meaning something else if the cut moves.
	 */
	const int32 CutCourse = Case18Cuts[0].CourseLo;

	/*
	 * Two heights, the same cut. Twelve courses hangs six bricks and twenty hangs fourteen, so a model
	 * concentrating the column on its foot reads 2.33x at the taller wall while the correct answer
	 * doesn't move. Both are past the eleven-course crossing (header ladder); don't lower a rung
	 * without re-measuring.
	 */
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

		/*
		 * The column must be hanging, or the number below measures an ordinary bed joint and the test
		 * is vacuous. Two assertions: a head joint says the column governs, and by how much is what
		 * fails while the crossing is one retune away (the first was true at the old heights and still
		 * let it go vacuous).
		 */
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

			/* One course index for both ends is a head joint; two is a bed joint. */
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

		/*
		 * And it must govern with room to spare. 1.15 is a tripwire, not a physical claim:
		 * measured margins are 1.285 at twelve courses and 1.300 at twenty, so this fires before
		 * the bed joint takes over (at the old ten-course rung the ratio was 0.986).
		 */
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

		/*
		 * The red: every course the column gains sheds its weight into its own head-joint pair, so the
		 * joints at the foot never see more than one brick, the same number at any height.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%d courses: the hanging column should read %.8g of head-joint shear ")
				TEXT("capacity, it reads %.8g"),
				Heights[Index], HangingColumnShearUtilisation, Result.Worst),
			FMath::IsNearlyEqual(Result.Worst, HangingColumnShearUtilisation, UtilisationTolerance));

		/*
		 * And the wrong answer is pinned at its measured size, as the catalogue's `DropsToday` pins a
		 * drop count. This passes, turning "some larger number" into a named defect: the reading is the
		 * per-pair figure times the hanging column's brick count. A differently-wrong model fails here
		 * rather than hiding in the already-red rows above.
		 */
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
		/*
		 * The property, asserted without reference to either value. This holds even if the absolute
		 * figure above is wrong, and is the half a model piling the column onto its foot cannot satisfy
		 * however tuned. Both rungs are past the eleven-course crossing, so this compares the same joint
		 * on the same axis at two heights, what the old 10-and-16 pair had stopped being.
		 */
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
 * The fixture's own bricklayer lays the wall Layout::RunningBond lays. Without this the file is a
 * second definition of a wall: nineteen outcomes measured against a subtly wrong wall would measure
 * the wrong thing and still read as a number. The fixture exists only because six cases are not
 * running-bond rectangles; on the shape both can lay, they must agree brick for brick.
 *
 * On boxes not handles, because a producer's emit order is its own business: the box sets must match
 * and each box must match somewhere. No ticking world.
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
 * The production bricklayer lays exactly the twenty walls this file measures. The bricklayer above
 * is test-only, so no level reaches it, which is why the user's walls could not be catalogue rows
 * until the geometry existed in production. Every reading here is about a particular arrangement to
 * seventeen digits (0.195160875 at a corbel bottom rung, 0.058203838191552663 at a bare header,
 * 0.01000825 of head-joint shear, plus three red rows naming bricks by (course, cell)), so a
 * producer differing by one ulp is a dozen tests changing answers at once, several already red.
 *
 * Compared: piece count, every box centre/extent, mass, grounded flag, and the whole connection set
 * in order (pairing, normal, area, centre, half-extent), exact `==` since "bit-identical" is the
 * claim. Handle order is in the list because a different lay sequence renumbers every joint and break
 * stamp while geometry checks pass, the failure that looks like nothing.
 *
 * The grid and regions move with it: `FWallRegion` is the vocabulary every cut and outcome uses, so
 * production must hand back each (course, cell) and name the same bricks, or twenty levels cut the
 * wrong bricks while laying the right wall.
 *
 * Still live: the bricklayer was copied not moved (`LayWall` calls nothing in DestructionWallCases;
 * the producer's landing commit was 853 insertions, zero deletions here), so this compares two
 * independent bricklayers, the only test that does.
 * `Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays` stands beside it (production vs RunningBond).
 * No ticking world; boxes and doubles, no solve.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWallAcceptanceProducerMatchesFixtureTest,
	"DestructionGame.Acceptance.Wall.TheProducerLaysTheWallTheFixtureLays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FWallAcceptanceProducerMatchesFixtureTest::RunTest(const FString& Parameters)
{
	using namespace WallAcceptanceTestSupport;

	/**
	 * A cell index held to a tolerance not a bit. Every region bound is at least an eighth of a cell
	 * (0.125) clear of any brick centre, eight orders above anything that changes which region a piece
	 * falls in. The course number is an integer, held exactly.
	 */
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
 * Every one of the twenty is a level a human can join, laying the same wall and cutting the same
 * bricks the acceptance row measures.
 *
 * The check lives here because the verdicts do, so this is the only place that knows what case 8 is.
 * A test in DestructionScenariosTest could assert a `wall-08` row exists and builds, not that it
 * builds the wall case 8 was measured against. The include direction stays test-includes-production.
 *
 * Asserted: each case has a row carrying its title verbatim; the wall it builds is the fixture's
 * brick for brick in handle order; the bricks it cuts are exactly the case's cut regions. The cut is
 * the half that matters: a level laying the right wall but cutting a brick two cells over shows a
 * plausible collapse unrelated to its case, the failure the acceptance set exists to make visible.
 * Cases 1 and 17 cut nothing, asserted from the case's empty cut list.
 *
 * Compared sorted, since a cut's order is the row's business (every cut goes in one batch and solve).
 * No ticking world; what a level does is World.Scenarios.*'s business, already covered.
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

		/*
		 * The title is the case's, verbatim: the catalogue, acceptance row and player banner must be
		 * three views of one case, not three descriptions that merely agree today.
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
		 * The intact rows cut nothing, derived from the case not listed: cases 1 and 17 are whole
		 * walls, and a row taking a brick out of one turns a regression anchor into a different case.
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
 * No level puts a verdict on screen that the solver doesn't produce.
 *
 * A caption needs a test because a red row captioned with only its expected verdict lies to a player
 * at the counter-example, worse than silence (the caption is the only thing telling them what they
 * see). Since 2026-08-12 cases 10 and 19 lie the other way: `Expected: STANDS` with the disagreement
 * marker, re-ruled to stand while the model drops 12 and 34. So a caption may say what's expected and
 * that the model disagrees, but may not claim a verdict the solver doesn't produce without admitting it.
 *
 * One token is pinned, not prose matching: `Expected: STANDS/LOCAL LOSS/COLLAPSE` spelled as
 * `VerdictName` spells it. Matching sentences would make the caption unwritable; matching nothing
 * lets it drift (cases 8 and 16 went stale for a day). Which rows must admit a disagreement is
 * computed, never listed, so a slice fixing case 20 (or a re-ruling handing a row to the model, as
 * case 8's did) turns this red until the caption catches up. A hardcoded list would rot silently.
 *
 * The known-red set is a tripwire on this test's own predicate: `ModelAgreesWithVerdict` could drift
 * from the catalogue while every catalogue row still failed identically, so requiring it to name
 * precisely the known-red rows makes that drift visible. No ticking world; same twenty walls.
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
	 * The rows the model gets wrong today. Not what decides which caption admits a disagreement (that
	 * is computed per row below); a check on the predicate that computes it, since a drifted second
	 * reading would caption a level wrongly while every catalogue failure stayed identical.
	 *
	 * Cases 11, 12, 8, 9 left by an expectation moving (re-rulings 2026-08-08/-09/-11/-12). Cases 10
	 * and 19 joined the same day inverted: re-ruled STANDS while the model drops 12 and 34, so
	 * membership no longer means "catalogue expects more damage than the model produces". Case 21
	 * joined at the 2026-08-14 flip: ruled COLLAPSE but the model now STANDS it (worst
	 * 0.93542327561664174, a knife edge). Case 22 is not on it (its collapse survived the re-anchor
	 * and the model still produces it).
	 */
	/*
	 * Shrank to {21} at slice 4 (2026-08-27). Cases 10, 19, 20 left the day the equilibrium LP became
	 * the break authority below the 200-block cap: it stands all three (lambda* 111.5 / 48.0 / 218.42)
	 * where the router only stranded or over-dropped, so the model agrees with their STANDS verdicts.
	 * Case 21 stays, its ruled COLLAPSE still contradicted by the LP (stands at 17.24). This edit is
	 * the paperwork the tripwire demands: the list, the captions' markers, and the rows'
	 * DropsToday/StrandsToday anchors all came off together.
	 */
	const int32 KnownDisagreements[] = { 21 };

	const TArray<FWallCase> Cases = AllWallCases();

	TestEqual(TEXT("FIXTURE: the catalogue is twenty-two cases"), Cases.Num(), 22);

	TArray<int32> Disagreed;

	for (const FWallCase& Case : Cases)
	{
		const FString LevelName = LevelNameForCase(Case.Number);

		const FString Where =
			FString::Printf(TEXT("case %d (%s) as '%s'"), Case.Number, Case.Title, *LevelName);

		/*
		 * What the model does with this wall, read before the row is looked up and not skipped when
		 * there is no row yet: the tripwire below checks the predicate, and one only exercised once the
		 * levels existed would be unproven when needed. Run this way it reproduces the known reds.
		 */
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

	/*
	 * Two failures, not one, because the set's two ways to move mean opposite things (the single
	 * `Disagreed == Known` row this replaces said only "these differ"):
	 *
	 *   GREW    a row the model used to get right now reads wrong. Expectations didn't move, so the
	 *           solver regressed or a verdict was re-ruled and this list hasn't caught up. Read as a
	 *           regression until the diff proves otherwise.
	 *   SHRANK  a known-red row now reads right. A slice fixed it, or an expectation was corrected;
	 *           paperwork owed: this list, the caption marker, and the row's `DropsToday` anchor.
	 *
	 * Case 11 is why: ruled down and back the same day, both edits moved this set while the solver
	 * didn't change, and "these differ" made good and bad news look identical. Together they are still
	 * exactly set equality, split by direction.
	 */
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
