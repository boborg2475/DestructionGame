// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named for what it holds. An anonymous namespace is private
 * to a TRANSLATION UNIT rather than to a file, and a unity build merges many files into one — at
 * which point two anonymous namespaces in the blob are the SAME namespace and identically-named
 * helpers in files that never refer to each other are a hard compile error.
 */
namespace StructureThrustTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace StructureArchingTestSupport;

	/*
	 * THE WALL, THE 0.866 CONSTANT, THE H/V ORACLE AND THE TWO SEAT-COUNTING HELPERS ALL LIVE IN
	 * ArchingWallTestSupport.h, because slice 4's cover test needs exactly the same 30 x 40 flush
	 * wall and a second copy of a wall definition is two fixtures that drift.
	 */

	/**
	 * THE OPENING IS ONE COURSE TALL, AND THAT IS A DELIBERATE CHOICE ABOUT WHAT ELSE IS IN SHOT.
	 *
	 * A multi-course opening has a REVEAL — a jamb brick one course below the spanning course,
	 * keeping one patch on the jamb and overhanging 5.625 cm into the opening where its neighbour
	 * was cut away. There is no head joint on its eccentric side, so the arch is correctly refused,
	 * and it peels as a genuine cantilever taking the springing's seat with it. CURRENT_STATE
	 * records that as what slice 2's closed load leak now lands on in acceptance cases 7, 9 and 11;
	 * it is the same shape as the free-end wedge and it belongs to slice 5. A one-course cut has no
	 * reveal at all, so nothing in this file is measuring it.
	 */
	constexpr int32 CutCourse = 1;

	/** The course whose bricks lose their seats: the one immediately above the cut. */
	constexpr int32 SpannedCourse = CutCourse + 1;

	/**
	 * HOW MUCH MASONRY STANDS OVER THE OPENING, cm — courses 2 through 39, one pitch each.
	 *
	 * This is the quantity that caps the arching depth, and the reason the opening is cut at
	 * course 1: 285 cm of cover is the deepest this wall has, so `d_e` is limited by the 0.866
	 * angle on a narrow opening and by the cover on a wide one, and the two cases below sit one
	 * either side of that crossover.
	 */
	constexpr double CoverAboveTheCutCm = CoverAboveCutCm(ScenarioWallCourses, CutCourse);

	/** Everything an intact piece is carrying, read off the joints it rests on. */
	inline double TotalCarriedUu(const FStructure& Structure, int32 Piece)
	{
		double TotalUu = 0.0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			if (Structure.GetJointRole(Joint, Piece) == EJointRole::BedBeneath
				&& !Structure.GetConnection(Joint).HasGiven())
			{
				TotalUu += FMath::Abs(Structure.GetConnectionForce(Joint).Z);
			}
		}

		return TotalUu;
	}

	/*
	 * THE THREE-CELL FIXTURE SLICE 2 LANDED, REBUILT HERE so the head joint it newly loads can be
	 * measured against the figure CURRENT_STATE records by hand. Same 7 x 30 wall, same cut.
	 */
	constexpr int32 SpannedHoleFirstCutBrickIndex = 1;
	constexpr int32 SpannedHoleCellCount = 3;
}

/**
 * AN ARCH PUSHES SIDEWAYS, THAT THRUST IS REAL, AND THE SPRINGING HAS TO CARRY IT IN SHEAR ON ITS
 * OWN BED JOINT — SO AN OPENING TOO WIDE FOR ITS ABUTMENTS TO RESIST COMES DOWN.
 *
 * WHY THIS SLICE IS URGENT RATHER THAN NEXT. After slice 2 and before slice 3 the solver spans an
 * opening of ANY width with no check whatever that the abutment can take the horizontal reaction.
 * That is the indestructible failure ARCHING_DESIGN warns about, it is worse than the behaviour
 * this work started from, and nothing in the suite can currently see it: every bed joint in the
 * model carries a purely vertical force, so the shear axis of a springing reads exactly zero
 * however wide the hole is.
 *
 * THE PHYSICS, STRAIGHT FROM ARCHING_DESIGN.md:
 *
 *     d_e = min( cover above the span , 0.866 * L )       arching depth
 *     r   = d_e / 3                                       thrust line rise, kern-limited
 *     W   = the load the solver already accumulated       NOT a triangle
 *     H   = W * L / (8r)      V = W / 2                   per abutment
 *
 * and the horizontal thrust is then shear on the abutment's bed joint against `0.2 + 0.6*sigma_n`,
 * truncated at the profile's ceiling. NO NEW AXIS, NO NEW STRENGTH, NO NEW PROFILE DATA:
 * ClassifyForce already calls a horizontal force on a bed joint shear, and the Mohr-Coulomb
 * capacity that resists it is the one every joint in the model already has.
 *
 * WHAT IS ASSERTED, AND WHY EACH FORM WAS CHOSEN.
 *
 *   - THE THRUST EXISTS. Each springing's bed joint must carry a non-zero HORIZONTAL force. This
 *     is the red claim and everything else is downstream of it; today the vector is (0, 0, +/-F)
 *     exactly, so the whole file rests on this one row first.
 *
 *   - SIGMA H = 0 ACROSS THE ARCH, which is trap 2 and the one thing nothing else could catch.
 *     Every joint is evaluated independently, so applying +H at one springing and forgetting the
 *     other gives the structure a net horizontal force out of nowhere while every joint still
 *     reads plausibly. Asserted as two facts — equal magnitudes and opposite signs — rather than
 *     assumed. Note it is VACUOUSLY TRUE while both are zero, which is why the non-zero row above
 *     comes first and is what makes this one mean anything.
 *
 *   - H/V = 3L/(4 d_e) AT EACH SPRINGING. W cancels out of that ratio, so it is a statement about
 *     the geometry of the thrust line and nothing else — no wall weight, no load distribution, no
 *     definition of which columns count. On the TEN-CELL case it is tighter still: there the
 *     0.866 angle governs `d_e` rather than the cover, `L` cancels as well, and the ratio is
 *     3/(4*0.866) = 0.86605 whatever the span and whatever the cover-walk of slice 4 eventually
 *     measures. That is the anchor of this file.
 *
 *   - THE SHEAR AXIS GOVERNS, ASSERTED BEFORE ANY NUMBER IS CLAIMED. ComputeUtilisation returns
 *     the worst of three, so a fixture aimed at the thrust would silently measure compression if
 *     compression happened to be higher — and on an arched springing the compression axis reads
 *     2|sigma_n|/f_c, which is a perfectly plausible small number sitting right next to the one
 *     being asserted. Both are printed and the comparison is a row of its own.
 *
 *   - THE ORDERING CHANGED AT THE 2026-08-14 MEAN RE-ANCHOR FLIP: BOTH WIDTHS NOW STAND. Slidingat
 *     the springing still governs and compression is still a kilometre and a half away, but the
 *     capacity moved (c 0.20 -> 0.90, mu 0.6 -> 0.75) and at mean strengths this wall affords
 *     the thrust at any width that fits it — the algebra at the case table shows a ~33-cell
 *     opening would be needed against a 30-cell wall. Both rows now pin under-capacity readings
 *     and the re-derived design cross-checks (0.274 and 0.606, re-derived through the design's
 *     own implied seat stress); the too-wide-to-arch discriminator is OWED A REPLACEMENT
 *     FIXTURE (thin cover high in a tall wall — see CURRENT_STATE), measured in the green
 *     phase, never tuned.
 *
 *   - DRY STONE MUST NEVER ARCH, AT ANY SPAN. `ShearCohesionMPa` is exactly 0.0 and friction is
 *     0.7, against a thrust ratio that cannot fall below 0.866 — so the springing reads
 *     0.866/0.7 = 1.237 of capacity at every width, load-independently, and no dry-stone opening
 *     can ever stand. That is right: you cannot span a hole in a dry-stone wall with a flat arch,
 *     you need a curved ring or a lintel. It is the strongest row in this file because it needs
 *     NO new field and NO per-material branch to be correct — it falls straight out of the
 *     existing profile data — so an implementation that hard-coded mortar's numbers fails it.
 *
 *   - AND THE RE-SEAT HEAD JOINT, WHICH IS THE ONLY JOINT SLICE 2 NEWLY LOADED AND WHICH NOTHING
 *     ASSERTS. CURRENT_STATE records it by hand at "about 0.56" of shear on the three-cell
 *     fixture and says out loud that no test reads it. Slice 3's thrust arrives at exactly that
 *     joint, so an unasserted joint that two slices both load is where a factor of two hides.
 *     What is pinned is the CONSERVATION statement — the shear it carries is exactly the hanger's
 *     own column, measured off the intact wall — plus that it is still intact and still
 *     self-consistent with the force it reports. Deliberately NOT the 0.56 literal: if the thrust
 *     is carried through the arch ring the same joint gains normal compression and reads about
 *     0.35, which is a legitimate implementation and not a regression, so the row is written to
 *     survive it while still failing if the load through it moves.
 *
 * NEVER A DISPLACEMENT, ANYWHERE. Two pieces can sever and stay resting exactly where they were,
 * so how far anything moved would say nothing. The mechanism claims are utilisation ratios on
 * named joints; the outcome claim is a count of joints that FAILED UNDER LOAD and of pieces left
 * with no path to the ground.
 *
 * NEEDS A TICKING WORLD: NO. FStructure is plain arithmetic over a graph and Layout is plain
 * arithmetic over boxes; nothing here needs an actor, a tick or a renderer. Slices 1 and 2 needed
 * none either.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureThrustTest,
	"DestructionGame.Core.Structure.AnArchThrustsAndTheSpringingMustCarryIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureThrustTest::RunTest(const FString& Parameters)
{
	using namespace StructureArchingTestSupport;
	using namespace StructureThrustTestSupport;
	using namespace StaircaseWallTestSupport;

	/*
	 * THE EXPECTED NUMBERS ARE RATIOS OF PUBLISHED STRENGTHS, so they mean what they say only
	 * while the profile still carries the figures they were derived against. Asserted rather than
	 * imported: a test that read the profile would agree with a wrong profile.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against the mean f_v0 = 0.9 MPa (re-anchor 2026-08-13; Gooch et al. 2023/2025), the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.9);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean friction 0.75, the profile carries %g"),
			GeneralPurposeMortar.FrictionCoefficient),
		GeneralPurposeMortar.FrictionCoefficient == 0.75);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against the mean-basis 2.0 MPa shear ceiling (0.1 x f_b), the profile carries %g"),
			GeneralPurposeMortar.MaxShearStrengthMPa),
		GeneralPurposeMortar.MaxShearStrengthMPa == 2.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: dry stone must have EXACTLY zero cohesion — it carries %g — or the ")
			TEXT("row that says it can never arch is measuring a weak bond instead of no bond"),
			DryStone.ShearCohesionMPa),
		DryStone.ShearCohesionMPa == 0.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against dry stone friction 0.7, the profile carries %g"),
			DryStone.FrictionCoefficient),
		DryStone.FrictionCoefficient == 0.7);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	AddInfo(FString::Printf(
		TEXT("FIXTURE: %d x %d flush wall, cut one course tall at course %d, so the cover above ")
		TEXT("the opening is %g cm"),
		ScenarioWallBricksPerCourse, ScenarioWallCourses, CutCourse, CoverAboveTheCutCm));

	/*
	 * ===================================================================================
	 * PART 1 — TWO WIDTHS, AND THE THRUST IS WHAT THE SPRINGINGS MUST AFFORD.
	 * (Both stand at mean strengths; the characteristic basis had twenty cells falling.)
	 * ===================================================================================
	 */

	/**
	 * One opening, and everything about it this file has an opinion on.
	 *
	 * BOTH ARE CENTRED ON x = 315, one cell left of the wall's own centre, so the two differ in
	 * WIDTH and in nothing else: same wall, same course, same cover, same distance from either
	 * end. A comparison between them is therefore a comparison of span.
	 */
	struct FThrustCase
	{
		const TCHAR* Description;

		/** How many coordinating cells of course 1 the player deletes. */
		int32 CellCount;

		/** The first FULL brick of course 1 to delete — the flush half bat comes before index 0. */
		int32 FirstCutBrickIndex;

		/** ARCHING_DESIGN.md's own figure for the springing, kept as a loose cross-check. */
		double DesignUtilisation;

		/** Whether the design says this opening is too wide for its abutments. */
		bool bMustComeDown;

		/**
		 * Whether the COVER (rather than the 0.866 angle) governs d_e here. Decides the H/V
		 * tolerance: the angle-governed ratio is the exact constant 3/(4 x 0.866) and gets 2%;
		 * the cover-governed one moves with how span and cover are measured and gets 12%.
		 * Used to ride on bMustComeDown, which stopped being the same question at the mean
		 * re-anchor: H/V is geometry and does not move when strengths do.
		 */
		bool bCoverGoverns;
	};

	/*
	 * MEAN RE-ANCHOR (2026-08-13). The springing capacity is c + mu sigma, and both moved
	 * (0.20 -> 0.90, 0.6 -> 0.75), so the design cross-checks are re-derived through the
	 * design's own implied seat stress: 0.763 at ten cells implies sigma = 0.3738 MPa
	 * (0.86605 sigma / (0.2 + 0.6 sigma) = 0.763), which at the new capacity reads
	 * 0.86605 x 0.3738 / (0.9 + 0.75 x 0.3738) = 0.274; 1.365 at twenty implies
	 * sigma = 0.7475, which reads 1.18424 x 0.7475 / (0.9 + 0.5606) = 0.606.
	 *
	 * THE TWENTY-CELL COLLAPSE ARM IS THEREFORE GONE: at mean strengths this wall affords the
	 * thrust at every width that fits it (the same algebra needs H/V >= 1.95, i.e. a ~33-cell
	 * opening against a 30-cell wall). The file loses its "too wide for its abutments"
	 * discriminator, and the REPLACEMENT IS OWED, specified in CURRENT_STATE: a wide opening
	 * under thin cover high in a tall wall, where H/V = 3L/(4 cover) grows without the seat
	 * stress growing with it — to be laid and MEASURED in the green phase, never tuned.
	 */
	const TArray<FThrustCase> Cases = {
		/*
		 * TEN CELLS, 225 cm. 0.866 * L is 194.85, which is less than the 285 cm of cover, so the
		 * ANGLE governs the arching depth here and the cover cap never binds. That matters for
		 * more than tidiness: it means this row reads the same whether or not the bounded upward
		 * cover walk of slice 4 exists yet, so the tight ratio below is a fair thing to ask of
		 * slice 3 on its own.
		 */
		{ TEXT("a 10-cell opening"), 10, 9, 0.274, false, false },

		/*
		 * TWENTY CELLS, 450 cm. 0.866 * L is 389.7, so here the COVER governs at 285 and the
		 * thrust ratio climbs to 1.184. On the retired characteristic basis this was the
		 * over-capacity row (design 1.365, measured ~1.29); at mean strengths it stands at
		 * ~0.61 and what it pins is the cover-governed H/V and the springing reading.
		 */
		{ TEXT("a 20-cell opening"), 20, 4, 0.606, false, true },
	};

	for (const FThrustCase& Case : Cases)
	{
		FBrickLayout Cut;

		if (!RunningBond(ScenarioWallSpec(), Cut) || Cut.Boxes.Num() != ScenarioWallPieceCount)
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: a flush %d x %d wall should lay as %d pieces, got %d"),
				Case.Description, ScenarioWallBricksPerCourse, ScenarioWallCourses,
				ScenarioWallPieceCount, Cut.Boxes.Num()));

			continue;
		}

		bool bCutLaid = true;

		for (int32 Cell = 0; Cell < Case.CellCount; ++Cell)
		{
			const double BrickXCm = ArchWallOddBrickXCm(Case.FirstCutBrickIndex + Cell);
			const int32 Piece = StaircasePieceAt(Cut.Boxes, BrickXCm, ArchWallCourseZCm(CutCourse));

			if (Piece == INDEX_NONE || !Cut.Structure.RemovePiece(Piece))
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: there should be a brick at x %g in course %d to delete"),
					Case.Description, BrickXCm, CutCourse));

				bCutLaid = false;
				break;
			}
		}

		if (!bCutLaid)
		{
			continue;
		}

		Cut.Structure.SolveLoads();

		/*
		 * THE SPAN, AND THE TWO REASONABLE READINGS OF IT. The CLEAR opening is the width of the
		 * masonry that was taken out; measured seat centroid to seat centroid it is 11.25 cm
		 * wider, because each springing keeps half a cell of bearing. The tolerances below are
		 * loose enough to cover the difference and are documented as such — and on the ten-cell
		 * case it does not arise at all, since L cancels out of the ratio there.
		 */
		const double ClearSpanCm = Case.CellCount * BrickPitchCm;
		const double SeatToSeatSpanCm = ClearSpanCm + BondOffsetCm;

		const double ArchingDepthCm =
			FMath::Min(CoverAboveTheCutCm, ArchingDepthPerSpan * ClearSpanCm);

		const double ExpectedThrustPerReaction = ThrustPerReaction(ClearSpanCm, CoverAboveTheCutCm);

		AddInfo(FString::Printf(
			TEXT("%s: L = %g cm clear (%g seat to seat), cover %g cm, so d_e = %g, r = %g and ")
			TEXT("H/V must be %s (%s if the span is measured seat to seat, %s if the cover cap ")
			TEXT("is deferred to slice 4)"),
			Case.Description, ClearSpanCm, SeatToSeatSpanCm, CoverAboveTheCutCm,
			ArchingDepthCm, ArchingDepthCm / 3.0, *Bits(ExpectedThrustPerReaction),
			*Bits(ThrustPerReaction(SeatToSeatSpanCm, CoverAboveTheCutCm)),
			*Bits(3.0 / (4.0 * ArchingDepthPerSpan))));

		/**
		 * One end of the arch: the half-seated brick the thrust is delivered to the ground
		 * through.
		 */
		struct FSpringingCase
		{
			const TCHAR* Side;

			/** Where the springing brick is, and where the one seat it kept is. */
			double BrickXCm;
			double SeatXCm;

			/** Which way it overhangs: +1 is toward increasing X, i.e. INTO the opening. */
			double EccentricSign;
		};

		const FSpringingCase Springings[2] = {
			{
				TEXT("LEFT springing"),
				ArchWallEvenBrickXCm(Case.FirstCutBrickIndex),
				ArchWallOddBrickXCm(Case.FirstCutBrickIndex - 1),
				+1.0
			},
			{
				TEXT("RIGHT springing"),
				ArchWallEvenBrickXCm(Case.FirstCutBrickIndex + Case.CellCount),
				ArchWallOddBrickXCm(Case.FirstCutBrickIndex + Case.CellCount),
				-1.0
			},
		};

		int32 SpringingJoints[2] = { INDEX_NONE, INDEX_NONE };
		double HorizontalUu[2] = { 0.0, 0.0 };
		double VerticalUu[2] = { 0.0, 0.0 };
		bool bBothSpringingsRead = true;

		for (int32 End = 0; End < 2; ++End)
		{
			const FSpringingCase& Springing = Springings[End];

			const FString Where = FString::Printf(
				TEXT("%s, %s"), Case.Description, Springing.Side);

			const int32 Brick = StaircasePieceAt(
				Cut.Boxes, Springing.BrickXCm, ArchWallCourseZCm(SpannedCourse));
			const int32 Seat = StaircasePieceAt(
				Cut.Boxes, Springing.SeatXCm, ArchWallCourseZCm(CutCourse));

			if (Brick == INDEX_NONE || Seat == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: no brick at x %g in course %d, or no seat at x %g in course %d"),
					*Where, Springing.BrickXCm, SpannedCourse, Springing.SeatXCm, CutCourse));

				bBothSpringingsRead = false;
				continue;
			}

			const int32 BedJoint = TheOneIntactSeatBeneath(Cut.Structure, Brick);

			if (BedJoint == INDEX_NONE
				|| BedJoint != JointBetweenPieces(Cut.Structure, Brick, Seat))
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: it must rest on EXACTLY ONE bed joint (it rests on %d) and ")
					TEXT("that joint must be the one to the brick at x %g"),
					*Where, IntactSeatsBeneath(Cut.Structure, Brick), Springing.SeatXCm));

				bBothSpringingsRead = false;
				continue;
			}

			SpringingJoints[End] = BedJoint;

			const FConnection& Bed = Cut.Structure.GetConnection(BedJoint);

			/*
			 * THE SPRINGING IS THE SAME HALF SEAT SLICES 1 AND 2 ALREADY ARCH — 10.25 x 10.25,
			 * loaded 5.625 cm off its own centroid, against a kern that reaches 1.7083 cm.
			 * Arbitrated against the producer rather than assumed, so that the number and the
			 * reason for it fail together.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the surviving seat should be %g cm2 with half-extents ")
					TEXT("(%g, %g); MakeInterface emitted %g cm2 with (%g, %g)"),
					*Where, HalfSeatAreaSqCm, HalfSeatHalfExtentCm, HalfSeatHalfExtentCm,
					Bed.InterfaceAreaSqCm,
					Bed.InterfaceHalfExtentCm.X, Bed.InterfaceHalfExtentCm.Y),
				FMath::IsNearlyEqual(Bed.InterfaceAreaSqCm, HalfSeatAreaSqCm, 1.0e-9)
					&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.X, HalfSeatHalfExtentCm, 1.0e-9)
					&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.Y, HalfSeatHalfExtentCm, 1.0e-9));

			const double EccentricityCm =
				Cut.Boxes[Brick].CentreCm.X - Bed.InterfaceCentreCm.X;

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: it should overhang its seat by %g cm INTO the opening, it ")
					TEXT("overhangs %g"),
					*Where, HalfSeatEccentricityCm, EccentricityCm),
				FMath::IsNearlyEqual(
					EccentricityCm, Springing.EccentricSign * HalfSeatEccentricityCm, 1.0e-9));

			/*
			 * AND THE OPENING IS GENUINELY SPANNED, WHICH IS WHAT MAKES THIS AN ARCH AT ALL. The
			 * bricks in the middle of the hole have no seat whatever and are re-seated onto the
			 * group's abutments by slice 2; if any of them were Stranded or Falling instead, this
			 * fixture would be measuring a collapse rather than a thrust and every number below
			 * would be about a different structure.
			 */
			const int32 Neighbour = StaircasePieceAt(
				Cut.Boxes,
				Springing.BrickXCm + Springing.EccentricSign * BrickPitchCm,
				ArchWallCourseZCm(SpannedCourse));

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the piece over the opening beside it must be re-seated and ")
					TEXT("Supported (it reads %d, Supported is %d) or there is no arch to thrust"),
					*Where,
					Neighbour == INDEX_NONE
						? -1
						: static_cast<int32>(Cut.Structure.GetPieceSupport(Neighbour)),
					static_cast<int32>(EPieceSupport::Supported)),
				Neighbour != INDEX_NONE
					&& Cut.Structure.GetPieceSupport(Neighbour) == EPieceSupport::Supported);

			/*
			 * AND WHAT THE RE-SEAT HEAD JOINT BESIDE IT IS CARRYING — REPORTED, DELIBERATELY NOT
			 * ASSERTED, BECAUSE NEITHER THIS SLICE NOR ANY PLANNED ONE OWNS IT.
			 *
			 * Slice 2 routes the whole group's load outward through these head joints, so the one
			 * next to a springing carries roughly half the group: about (cells-1)/2 columns, in
			 * PURE shear against 0.2 MPa of cohesion with no normal force to buy friction with. On
			 * the three-cell fixture that is one column and 0.56; on a ten-cell opening it is four
			 * and a half columns, and the joint is far past capacity long before the springing
			 * this file is measuring is. That is why the ten-cell wall does not in fact stand
			 * today, and it is a slice 2 consequence rather than a missing thrust — printed here
			 * so whoever reads this file's red output is not left thinking the thrust check is
			 * what decided it.
			 */
			if (Neighbour != INDEX_NONE)
			{
				const int32 ReseatJoint = JointBetweenPieces(Cut.Structure, Brick, Neighbour);

				if (ReseatJoint != INDEX_NONE)
				{
					AddInfo(FString::Printf(
						TEXT("%s: FOR INFORMATION ONLY — the re-seat head joint %d beside it ")
						TEXT("carries (%s, %s, %s) uu, %.2f brick weights, and reads %s"),
						*Where, ReseatJoint,
						*Bits(Cut.Structure.GetConnectionForce(ReseatJoint).X),
						*Bits(Cut.Structure.GetConnectionForce(ReseatJoint).Y),
						*Bits(Cut.Structure.GetConnectionForce(ReseatJoint).Z),
						FMath::Abs(Cut.Structure.GetConnectionForce(ReseatJoint).Z) / BrickWeightUu,
						*Bits(Cut.Structure.GetConnectionUtilisation(ReseatJoint))));
				}
			}

			// --- what the joint carries -------------------------------------------------------

			const FVector ForceUu = Cut.Structure.GetConnectionForce(BedJoint);
			const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(BedJoint);

			const double Utilisation = Cut.Structure.GetConnectionUtilisation(BedJoint);

			const FBedJointReading Published = ReadBedJoint(
				ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
				GeneralPurposeMortar);

			/*
			 * THE HORIZONTAL COMPONENT IS SIGNED AS THE JOINT STORES IT, AND THAT IS DELIBERATE.
			 * ConnectionLoad's convention is that the force belonging to a joint is the force
			 * acting on PieceB; both springings name the brick below first and the springing brick
			 * second, so the two entries are directly comparable and an arch that thrusts outward
			 * gives them opposite signs. Which sign belongs to which end is under-determined by
			 * anything the model checks, so it is PRINTED rather than asserted; what is asserted
			 * is that they are equal and opposite, which is the claim ARCHING_DESIGN's trap 2
			 * makes.
			 */
			HorizontalUu[End] = ForceUu.X;
			VerticalUu[End] = FMath::Abs(ForceUu.Z);

			const double ThrustRatio = VerticalUu[End] > 0.0
				? FMath::Abs(ForceUu.X) / VerticalUu[End]
				: 0.0;

			AddInfo(FString::Printf(
				TEXT("%s: joint %d carries (%s, %s, %s) uu — V = %.2f brick weights, H/V = %s — ")
				TEXT("and publishes %s uu.cm. sigma_n %s MPa, shear %s MPa against a capacity of ")
				TEXT("%s MPa"),
				*Where, BedJoint, *Bits(ForceUu.X), *Bits(ForceUu.Y), *Bits(ForceUu.Z),
				VerticalUu[End] / BrickWeightUu, *Bits(ThrustRatio), *Bits(MomentUuCm.Size()),
				*Bits(Published.NormalStressMPa),
				*Bits(FVector(ForceUu.X, ForceUu.Y, 0.0).Size()
					/ (Bed.InterfaceAreaSqCm * ForceUnitsPerMPaPerSqCm)),
				*Bits(GeneralPurposeMortar.ShearCohesionMPa
					+ GeneralPurposeMortar.FrictionCoefficient
						* FMath::Abs(Published.NormalStressMPa))));

			AddInfo(FString::Printf(
				TEXT("%s: reads %s — tension %s, compression %s, SHEAR %s; ARCHING_DESIGN says %g"),
				*Where, *Bits(Utilisation), *Bits(Published.TensionUtilisation),
				*Bits(Published.CompressionUtilisation), *Bits(Published.ShearUtilisation),
				Case.DesignUtilisation));

			/*
			 * THE SEAT IS IN COMPRESSION, which is the gate the whole thrust line depends on: no
			 * compression, no thrust line, no arch. On a bed joint under gravity there had better
			 * be some or the fixture is not describing a wall.
			 */
			TestTrue(
				FString::Printf(TEXT("%s: FIXTURE: the seat must be in COMPRESSION, sigma_n is %s MPa"),
					*Where, *Bits(Published.NormalStressMPa)),
				Published.NormalStressMPa < 0.0);

			/*
			 * AND THE MOHR-COULOMB CEILING IS NOT WHAT IS DECIDING. ARCHING_DESIGN says the 1.3 MPa
			 * truncation is never reached in this regime — capacity is about 0.55 and 0.81 MPa on
			 * the two cases — so if it ever were, the span limit would be governed by the cap
			 * rather than by friction and both figures below would mean something else.
			 */
			const double UntruncatedCapacityMPa = GeneralPurposeMortar.ShearCohesionMPa
				+ GeneralPurposeMortar.FrictionCoefficient * FMath::Abs(Published.NormalStressMPa);

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the shear ceiling must NOT be reached — Mohr-Coulomb gives ")
					TEXT("%s MPa against the profile's %g MPa cap"),
					*Where, *Bits(UntruncatedCapacityMPa),
					GeneralPurposeMortar.MaxShearStrengthMPa),
				UntruncatedCapacityMPa < GeneralPurposeMortar.MaxShearStrengthMPa);

			/* THE RED CLAIM, and everything after it is downstream of this one row. */
			TestTrue(
				FString::Printf(
					TEXT("%s: the springing must carry a HORIZONTAL thrust; its force is ")
					TEXT("(%s, %s, %s) uu"),
					*Where, *Bits(ForceUu.X), *Bits(ForceUu.Y), *Bits(ForceUu.Z)),
				FMath::Abs(ForceUu.X) > 0.0);

			/*
			 * H/V = 3L/(4 d_e), AND W CANCELS OUT OF IT. See ThrustPerReaction: the ratio is a
			 * statement about the geometry of the thrust line alone, so it does not depend on what
			 * the wall above weighs or on how the solver divided it.
			 *
			 * THE TOLERANCE IS 2% ON THE TEN-CELL CASE AND 12% ON THE TWENTY. On ten cells the
			 * angle governs the depth, so L cancels as well and the answer is 3/(4*0.866) whatever
			 * anyone measures — 2% is slack for 0.866 against sqrt(3)/2 and for nothing else. On
			 * twenty cells the cover governs, so the answer moves with how the span and the cover
			 * are measured (clear opening or seat to seat, 38 courses of cover or 39) and 12%
			 * spans every reasonable reading of both. It does NOT span the 0.866 a deferred cover
			 * cap would give, which is stated in the message rather than tolerated.
			 */
			const double ThrustRatioTolerance = Case.bCoverGoverns ? 0.12 : 0.02;

			TestTrue(
				FString::Printf(
					TEXT("%s: the thrust must be H/V = 3L/(4 d_e) = %s; the joint reports H = %s uu ")
					TEXT("against V = %s uu, a ratio of %s. (A W that excluded the springings' own ")
					TEXT("columns would read about %s; a deferred cover cap would read %s.)"),
					*Where, *Bits(ExpectedThrustPerReaction), *Bits(ForceUu.X),
					*Bits(VerticalUu[End]), *Bits(ThrustRatio),
					*Bits(ExpectedThrustPerReaction * Case.CellCount
						/ static_cast<double>(Case.CellCount + 1)),
					*Bits(3.0 / (4.0 * ArchingDepthPerSpan))),
				FMath::Abs(ThrustRatio - ExpectedThrustPerReaction)
					<= ThrustRatioTolerance * ExpectedThrustPerReaction);

			/*
			 * THE AXIS, BEFORE ANY CLAIM ABOUT THE NUMBER. On an arched springing the compression
			 * axis reads 2|sigma_n|/f_c — a plausible small number sitting right beside the one
			 * being asserted — so a fixture aimed at the thrust would measure compression instead
			 * the moment compression happened to be higher, and would do it silently.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: SHEAR must be the governing axis — shear %s against compression %s ")
					TEXT("and tension %s — or this row is measuring the wrong thing"),
					*Where, *Bits(Published.ShearUtilisation),
					*Bits(Published.CompressionUtilisation), *Bits(Published.TensionUtilisation)),
				Published.ShearUtilisation > Published.CompressionUtilisation
					&& Published.ShearUtilisation > Published.TensionUtilisation);

			/*
			 * AND THE JOINT'S OWN READING AGREES WITH MOHR-COULOMB ON THE FORCE AND MOMENT IT
			 * REPORTS. This is what pins that the thrust is being evaluated AS SHEAR ON THE BED
			 * JOINT against `c + mu*sigma_n` rather than through some second, private rule — the
			 * whole design claim is that no new axis and no new strength are needed.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: it must read what beam theory and Mohr-Coulomb say, %s, and it reads %s"),
					*Where, *Bits(Published.Worst), *Bits(Utilisation)),
				FMath::Abs(Utilisation - Published.Worst)
					<= 1.0e-12 * FMath::Max(Published.Worst, 1.0e-12));

			/* THE ORDERING, WHICH IS THE CLAIM THE DESIGN SAYS IS SOLID. */
			if (Case.bMustComeDown)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s: %d cells is too wide for its abutment — the springing must be ")
						TEXT("OVER capacity in shear, it reads %s"),
						*Where, Case.CellCount, *Bits(Published.ShearUtilisation)),
					Published.ShearUtilisation > 1.0);
			}
			else
			{
				TestTrue(
					FString::Printf(
						TEXT("%s: %d cells must still stand — the springing must be UNDER capacity ")
						TEXT("in shear, it reads %s"),
						*Where, Case.CellCount, *Bits(Published.ShearUtilisation)),
					Published.ShearUtilisation < 1.0);
			}

			/*
			 * AND LOOSELY AGAINST THE DESIGN'S OWN NUMBER — a factor of two either way, which is
			 * an order-of-magnitude cross-check and nothing more. ARCHING_DESIGN says its 356.3 cm
			 * critical span is the least trustworthy figure in the document and asks for the
			 * ordering to be pinned instead; a factor of two still catches a missing 100x, a
			 * missing division by three in the rise, or a thrust taken as W rather than as
			 * W*L/(8r).
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: ARCHING_DESIGN predicts %g for this springing and it reads %s — a ")
					TEXT("cross-check, not a target, so it allows a factor of two"),
					*Where, Case.DesignUtilisation, *Bits(Published.ShearUtilisation)),
				Published.ShearUtilisation >= 0.5 * Case.DesignUtilisation
					&& Published.ShearUtilisation <= 2.0 * Case.DesignUtilisation);
		}

		if (!bBothSpringingsRead)
		{
			continue;
		}

		/*
		 * TRAP 2 — SIGMA H = 0 ACROSS THE ARCH, AND NOTHING IN THE MODEL WOULD NOTICE IF IT WERE
		 * NOT. Every joint is evaluated independently, so applying +H at one springing and
		 * forgetting the other gives the structure a net horizontal force out of nowhere while
		 * every joint still reads perfectly plausibly. Asserted as two separate facts: the
		 * magnitudes match, and the directions oppose.
		 *
		 * IT IS VACUOUS WHILE BOTH ARE ZERO, which is precisely today's state — so the non-zero
		 * row above is what makes this one mean anything, and they have to be read together.
		 */
		const double HorizontalSumUu = HorizontalUu[0] + HorizontalUu[1];
		const double LargerThrustUu = FMath::Max(
			FMath::Abs(HorizontalUu[0]), FMath::Abs(HorizontalUu[1]));

		AddInfo(FString::Printf(
			TEXT("%s: the two springings carry H = %s and %s uu, summing to %s; V = %s and %s uu"),
			Case.Description, *Bits(HorizontalUu[0]), *Bits(HorizontalUu[1]),
			*Bits(HorizontalSumUu), *Bits(VerticalUu[0]), *Bits(VerticalUu[1])));

		TestTrue(
			FString::Printf(
				TEXT("%s: the two ends of the arch must push in OPPOSITE directions — they carry ")
				TEXT("%s and %s uu"),
				Case.Description, *Bits(HorizontalUu[0]), *Bits(HorizontalUu[1])),
			HorizontalUu[0] * HorizontalUu[1] < 0.0);

		TestTrue(
			FString::Printf(
				TEXT("%s: and SIGMA H MUST BE ZERO across the arch — %s + %s = %s uu, against a ")
				TEXT("thrust of %s"),
				Case.Description, *Bits(HorizontalUu[0]), *Bits(HorizontalUu[1]),
				*Bits(HorizontalSumUu), *Bits(LargerThrustUu)),
			FMath::Abs(HorizontalSumUu) <= 1.0e-9 * FMath::Max(LargerThrustUu, 1.0));

		/*
		 * ===========================================================================
		 * AND THE OUTCOME, WHERE A ROW CLAIMS ONE.
		 * (No mortared row claims a collapse since the mean re-anchor — the arm is
		 * kept for the owed replacement fixture.)
		 * ===========================================================================
		 *
		 * A SINGLE SEVERED JOINT IS NOT A COLLAPSE, so the outcome claim is a count of pieces left
		 * with no path to the ground — and specifically that the two springings are among the
		 * joints that FAILED UNDER LOAD, which is the mechanism this slice adds rather than any
		 * old way of the wall falling over.
		 *
		 * COUNTED BY BREAK PASS AND NOT BY HasGiven. GetBreakPass's contract spells the encoding
		 * out: a joint that went WITH A REMOVED PIECE has HasGiven true and a pass of INDEX_NONE,
		 * because it never snapped. The player's own click always takes joints with it.
		 */
		const int32 BreakingPasses = Cut.Structure.SolveAndBreak();

		int32 JointsBrokenByLoad = 0;

		for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
		{
			if (Cut.Structure.GetConnection(Joint).HasGiven()
				&& Cut.Structure.GetBreakPass(Joint) != INDEX_NONE)
			{
				++JointsBrokenByLoad;
			}
		}

		int32 Unrouted = 0;

		for (int32 Piece = 0; Piece < Cut.Structure.NumPieces(); ++Piece)
		{
			if (!Cut.Structure.IsPieceRemoved(Piece)
				&& !Cut.Structure.IsPieceSupported(Piece))
			{
				++Unrouted;
			}
		}

		AddInfo(FString::Printf(
			TEXT("%s: the cascade ran %d passes, %d of %d joints failed under load, and %d of the ")
			TEXT("%d pieces it did not delete are left with no path to the ground"),
			Case.Description, BreakingPasses, JointsBrokenByLoad,
			Cut.Structure.NumConnections(), Unrouted,
			Cut.Structure.NumPieces() - Case.CellCount));

		if (Case.bMustComeDown)
		{
			/*
			 * THE MASONRY OVER THE OPENING IS WHAT MUST LOSE ITS PATH TO THE GROUND — at minimum
			 * the CellCount + 1 bricks of the spanned course itself, which is the span the arch
			 * was carrying. Stated as a floor rather than an exact count because how far up the
			 * wall the loss travels is not something this slice claims.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: an opening too wide for its abutments must COME DOWN — at least the ")
					TEXT("%d bricks over it should lose their path to the ground; %d pieces did"),
					Case.Description, Case.CellCount + 1, Unrouted),
				Unrouted >= Case.CellCount + 1);

			for (int32 End = 0; End < 2; ++End)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s, %s: and the springing itself must be one of the joints that ")
						TEXT("failed under load; joint %d broke in pass %d"),
						Case.Description, Springings[End].Side, SpringingJoints[End],
						Cut.Structure.GetBreakPass(SpringingJoints[End])),
					Cut.Structure.GetBreakPass(SpringingJoints[End]) != INDEX_NONE);
			}
		}
	}

	/*
	 * ===================================================================================
	 * PART 2 — THE RE-SEAT HEAD JOINT, WHICH TWO SLICES LOAD AND NOTHING ASSERTS.
	 * ===================================================================================
	 *
	 * Slice 2 re-seats a piece with no bed patch onto the head joints that take it closer to an
	 * abutment, and that head joint is the ONLY joint the slice newly loads. CURRENT_STATE records
	 * what it carries by hand — "about 0.56" of shear on the three-cell fixture — and says out
	 * loud that no test reads it. Slice 3's thrust arrives at exactly this joint.
	 *
	 * WHAT IS PINNED IS CONSERVATION, NOT THE 0.56. The hanger has one route to the ground, so the
	 * shear through that head joint is the hanger's whole column — and the column is measured off
	 * the INTACT wall, where nothing above the spanned course has changed. That claim is tight and
	 * it is independent of any modelling choice. The utilisation is asserted only to be under
	 * capacity and self-consistent, because if slice 3 carries the thrust along the arch ring the
	 * same joint gains normal compression and reads about 0.35 instead — a legitimate
	 * implementation, not a regression, and a row written against the literal would forbid it.
	 */
	{
		FBrickLayout Intact;
		FBrickLayout Cut;

		if (!RunningBond(ArchWallSpec(), Intact) || Intact.Boxes.Num() != ArchWallPieceCount
			|| !RunningBond(ArchWallSpec(), Cut) || Cut.Boxes.Num() != ArchWallPieceCount)
		{
			AddError(TEXT("HEAD JOINT: FIXTURE: the 7 x 30 wall did not lay twice"));
		}
		else
		{
			Intact.Structure.SolveLoads();

			bool bCutLaid = true;

			for (int32 Cell = 0; Cell < SpannedHoleCellCount; ++Cell)
			{
				const double BrickXCm =
					ArchWallOddBrickXCm(SpannedHoleFirstCutBrickIndex + Cell);
				const int32 Piece =
					StaircasePieceAt(Cut.Boxes, BrickXCm, ArchWallCourseZCm(CutCourse));

				if (Piece == INDEX_NONE || !Cut.Structure.RemovePiece(Piece))
				{
					AddError(TEXT("HEAD JOINT: FIXTURE: the three-cell cut could not be made"));
					bCutLaid = false;
					break;
				}
			}

			if (bCutLaid)
			{
				Cut.Structure.SolveLoads();

				/*
				 * A three-cell cut leaves four bricks over the hole: two SPRINGINGS at the edges
				 * with one seat each, and two HANGERS in the middle with none. Each hanger is one
				 * head joint from its nearer springing, and that joint is the subject.
				 */
				const int32 Springing = StaircasePieceAt(
					Cut.Boxes, ArchWallEvenBrickXCm(SpannedHoleFirstCutBrickIndex),
					ArchWallCourseZCm(SpannedCourse));

				const int32 Hanger = StaircasePieceAt(
					Cut.Boxes, ArchWallEvenBrickXCm(SpannedHoleFirstCutBrickIndex + 1),
					ArchWallCourseZCm(SpannedCourse));

				const int32 IntactHanger = StaircasePieceAt(
					Intact.Boxes, ArchWallEvenBrickXCm(SpannedHoleFirstCutBrickIndex + 1),
					ArchWallCourseZCm(SpannedCourse));

				const int32 HeadJoint = Springing == INDEX_NONE || Hanger == INDEX_NONE
					? INDEX_NONE
					: JointBetweenPieces(Cut.Structure, Springing, Hanger);

				if (HeadJoint == INDEX_NONE || IntactHanger == INDEX_NONE)
				{
					AddError(TEXT("HEAD JOINT: FIXTURE: the springing, hanger or head joint is missing"));
				}
				else
				{
					const FConnection& Head = Cut.Structure.GetConnection(HeadJoint);

					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: FIXTURE: it must be an intact HEAD joint of %g cm2; ")
							TEXT("it is role %d, area %g"),
							HeadJointAreaSqCm,
							static_cast<int32>(Cut.Structure.GetJointRole(HeadJoint, Hanger)),
							Head.InterfaceAreaSqCm),
						Cut.Structure.GetJointRole(HeadJoint, Hanger) == EJointRole::Head
							&& !Head.HasGiven()
							&& FMath::IsNearlyEqual(
								Head.InterfaceAreaSqCm, HeadJointAreaSqCm, 1.0e-9));

					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: FIXTURE: the hanger must have NO seat at all (it has ")
							TEXT("%d) and must still be Supported (it reads %d)"),
							IntactSeatsBeneath(Cut.Structure, Hanger),
							static_cast<int32>(Cut.Structure.GetPieceSupport(Hanger))),
						IntactSeatsBeneath(Cut.Structure, Hanger) == 0
							&& Cut.Structure.GetPieceSupport(Hanger) == EPieceSupport::Supported);

					const double IntactColumnUu = TotalCarriedUu(Intact.Structure, IntactHanger);

					const FVector ForceUu = Cut.Structure.GetConnectionForce(HeadJoint);
					const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(HeadJoint);

					FVector UnitNormal = Head.InterfaceNormal;
					UnitNormal.Normalize();

					const FHeadJointReading Published = ReadHeadJoint(
						ForceUu, UnitNormal, Head.InterfaceAreaSqCm, GeneralPurposeMortar);

					const double Utilisation = Cut.Structure.GetConnectionUtilisation(HeadJoint);

					AddInfo(FString::Printf(
						TEXT("HEAD JOINT: joint %d carries (%s, %s, %s) uu against the hanger's ")
						TEXT("intact column of %s uu (%.4f brick weights); sigma_n %s MPa, shear ")
						TEXT("%s MPa against a capacity of %s; it reads %s"),
						HeadJoint, *Bits(ForceUu.X), *Bits(ForceUu.Y), *Bits(ForceUu.Z),
						*Bits(IntactColumnUu), IntactColumnUu / BrickWeightUu,
						*Bits(Published.NormalStressMPa), *Bits(Published.ShearStressMPa),
						*Bits(Published.ShearCapacityMPa), *Bits(Utilisation)));

					/*
					 * CONSERVATION, AND IT IS THE TIGHT CLAIM. The hanger has exactly one route to
					 * the ground, so everything standing on it leaves through this joint — and
					 * nothing above the spanned course changed when the hole was cut, so that
					 * column is the same number in both walls to the last bit.
					 */
					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: the shear through it must be the hanger's whole ")
							TEXT("column, %s uu; it carries %s uu vertically"),
							*Bits(IntactColumnUu), *Bits(FMath::Abs(ForceUu.Z))),
						FMath::Abs(FMath::Abs(ForceUu.Z) - IntactColumnUu)
							<= 1.0e-9 * IntactColumnUu);

					/*
					 * A RE-SEATED PIECE IS STATICALLY INDETERMINATE by slice 2's own rule — the
					 * group only routes when something seated stands on both sides, so the single
					 * head joint left in its load path is the bookkeeping route for a vertical
					 * share and not a claim that the brick hangs off it. Treated as determinate it
					 * would carry its whole column across 11.25 cm and snap.
					 */
					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: it must publish NO moment — a re-seated piece is ")
							TEXT("indeterminate — and it publishes %s uu.cm"),
							*Bits(MomentUuCm.Size())),
						MomentUuCm == FVector::ZeroVector);

					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: it must read what Mohr-Coulomb says on the force it ")
							TEXT("reports, %s, and it reads %s"),
							*Bits(Published.Worst), *Bits(Utilisation)),
						FMath::Abs(Utilisation - Published.Worst)
							<= 1.0e-12 * FMath::Max(Published.Worst, 1.0e-12));

					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: and it must still be INTACT — the three-cell wall ")
							TEXT("stands, so nothing slice 3 adds here may take it over capacity; ")
							TEXT("it reads %s"),
							*Bits(Utilisation)),
						Utilisation < 1.0);
				}
			}
		}
	}

	/*
	 * ===================================================================================
	 * PART 3 — DRY STONE CANNOT ARCH, AT ANY SPAN, AND NO NEW DATA SAYS SO.
	 * ===================================================================================
	 *
	 * `ShearCohesionMPa` is EXACTLY 0.0 and friction is 0.7, so a dry joint's whole shear capacity
	 * is 0.7 * sigma_n — and the thrust ratio 3L/(4 d_e) cannot fall below 3/(4*0.866) = 0.866,
	 * because d_e is capped at 0.866*L. The springing therefore reads
	 *
	 *     (0.866 * sigma_n) / (0.7 * sigma_n)  =  1.2372
	 *
	 * at EVERY span and under EVERY load, since sigma_n cancels. That is why two widths are cut
	 * rather than one: if the answer moved with the load or the span, the cancellation is not
	 * happening and the implementation has a term in it that should not be there.
	 *
	 * TWO CELLS AND FIVE, NOT ONE AND FIVE, AND THE ONE-CELL ROW WAS THE TEST'S MISTAKE RATHER
	 * THAN THE MODEL'S. A one-cell opening leaves NO piece without a seat — the two bricks over it
	 * each keep half a bed patch — so there is no group, slice 2 never spans anything, and slice 3
	 * develops no thrust there at all. That is not an omission: it is exactly the topology
	 * `Structure.AMissingBrickIsBridgedNotCantilevered` pins at shear EXACTLY zero, reading
	 * 2|sigma_n|/f_c to 1e-12, and one brick out of a wall is slice 1's moment restraint rather
	 * than an arch. Asking the same topology for 1.2372 in one wall and 0.0 in another would be
	 * satisfiable only by branching on the material, which is the regression DESIGN.md §2 names by
	 * name. TWO cells is the narrowest opening that leaves a seatless brick, so it is the
	 * narrowest thing in this model that is an arch at all — and against five cells it still
	 * demonstrates the cancellation, which is the whole reason for cutting two widths.
	 *
	 * IT IS THE STRONGEST ROW IN THIS FILE because it needs no new field and no per-material
	 * branch to be right — it falls out of the existing profile data — so an implementation that
	 * hard-coded mortar's 0.2 and 0.6 passes everything above and fails here.
	 *
	 * PHYSICALLY IT IS ALSO THE RIGHT ANSWER. You cannot span a hole in a dry-stone wall with a
	 * flat arch; you need a curved ring or a lintel.
	 */
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		Spec.JointThicknessCm = MortarJointCm;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;

		/**
		 * TWENTY COURSES, AND THE FIGURE IS 0.866 * 112.5 = 97.425 cm.
		 *
		 * The five-cell opening spans 112.5 cm, so the ANGLE wants 97.425 cm of arching depth, and
		 * `d_e = min(cover, 0.866 * L)` only leaves the angle governing while the wall actually has
		 * that much masonry over the cut. Twelve courses have 75 cm and would hand the cover the
		 * decision the moment slice 4's upward walk lands — at which point this row would read
		 * 3*112.5/(4*75)/0.7 = 1.6071 rather than 1.2372, and the failure would look like a
		 * regression in the thrust rather than a fixture that was always too short.
		 *
		 * Cut at course 1, the cover is (20 - 1 - 1) * 7.5 = 135 cm, which clears 97.425 by 39%.
		 * Fifteen courses would clear it by 0.075 cm and that is not a margin, it is a coincidence.
		 */
		Spec.CoursesHigh = 20;
		Spec.BricksPerCourse = 12;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = DryStone;

		/**
		 * THE UNIT'S DENSITY IS DELIBERATELY THE CLAY BRICK'S, and it does not matter, which is
		 * the row's own claim: the answer is a ratio of two stresses that are both linear in the
		 * load, so it is load-independent and therefore density-independent. Cutting two widths
		 * changes the load by a factor of several and the assertion is that the reading does not
		 * move.
		 */
		struct FDryStoneCase
		{
			const TCHAR* Description;
			int32 CellCount;
			int32 FirstCutBrickIndex;
		};

		const TArray<FDryStoneCase> DryStoneCases = {
			/*
			 * TWO CELLS IS THE NARROWEST ARCH THIS MODEL HAS. It leaves exactly one brick with no
			 * seat whatever, which is the smallest group slice 2 can span, and both its abutments
			 * are half-seated bricks two cells apart. One cell leaves no seatless brick at all and
			 * is therefore not an arch — see the header above.
			 */
			{ TEXT("dry stone, a TWO-cell opening"), 2, 5 },
			{ TEXT("dry stone, a FIVE-cell opening"), 5, 3 },
		};

		/*
		 * 0.866 / 0.7 = 1.237215440448697, and sigma_n has cancelled out of it.
		 *
		 * MEAN RE-ANCHOR INVARIANT (2026-08-13): dry stone's row does not move — c and
		 * tension stay exact zeros, mu stays 0.7 — so this identity and everything the
		 * dry-stone cases read must come back BIT-IDENTICAL when the mortar rows flip.
		 * If a dry row moves at the flip, the flip touched a row it must not have.
		 */
		const double DryStoneSpringingUtilisation =
			(3.0 / (4.0 * ArchingDepthPerSpan)) / DryStone.FrictionCoefficient;

		for (const FDryStoneCase& Case : DryStoneCases)
		{
			FBrickLayout Cut;

			if (!RunningBond(Spec, Cut))
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: the dry stone wall did not lay"), Case.Description));

				continue;
			}

			bool bCutLaid = true;

			for (int32 Cell = 0; Cell < Case.CellCount; ++Cell)
			{
				const double BrickXCm = ArchWallOddBrickXCm(Case.FirstCutBrickIndex + Cell);
				const int32 Piece =
					StaircasePieceAt(Cut.Boxes, BrickXCm, ArchWallCourseZCm(CutCourse));

				if (Piece == INDEX_NONE || !Cut.Structure.RemovePiece(Piece))
				{
					AddError(FString::Printf(
						TEXT("%s: FIXTURE: there should be a brick at x %g in course %d"),
						Case.Description, BrickXCm, CutCourse));

					bCutLaid = false;
					break;
				}
			}

			if (!bCutLaid)
			{
				continue;
			}

			Cut.Structure.SolveLoads();

			const int32 Brick = StaircasePieceAt(
				Cut.Boxes, ArchWallEvenBrickXCm(Case.FirstCutBrickIndex),
				ArchWallCourseZCm(SpannedCourse));

			const int32 BedJoint = Brick == INDEX_NONE
				? INDEX_NONE
				: TheOneIntactSeatBeneath(Cut.Structure, Brick);

			if (BedJoint == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: the left springing must rest on exactly one bed joint"),
					Case.Description));

				continue;
			}

			/*
			 * AND THE OPENING IS GENUINELY SPANNED, WHICH IS WHAT MAKES IT AN ARCH AT ALL — the
			 * brick over the hole beside the springing must have NO seat and still be Supported.
			 * This is the row that would have caught the one-cell case the header above describes:
			 * a one-cell opening leaves nothing seatless, so no group forms, no thrust is
			 * developed, and the springing quietly reads zero shear instead of 1.2372.
			 */
			const int32 Neighbour = StaircasePieceAt(
				Cut.Boxes, ArchWallEvenBrickXCm(Case.FirstCutBrickIndex) + BrickPitchCm,
				ArchWallCourseZCm(SpannedCourse));

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the brick over the opening beside the springing must have ")
					TEXT("NO seat (it has %d) and must still be Supported (it reads %d, Supported ")
					TEXT("is %d) — otherwise there is no group, no arch and nothing to thrust"),
					Case.Description,
					Neighbour == INDEX_NONE
						? -1
						: IntactSeatsBeneath(Cut.Structure, Neighbour),
					Neighbour == INDEX_NONE
						? -1
						: static_cast<int32>(Cut.Structure.GetPieceSupport(Neighbour)),
					static_cast<int32>(EPieceSupport::Supported)),
				Neighbour != INDEX_NONE
					&& IntactSeatsBeneath(Cut.Structure, Neighbour) == 0
					&& Cut.Structure.GetPieceSupport(Neighbour) == EPieceSupport::Supported);

			const FConnection& Bed = Cut.Structure.GetConnection(BedJoint);

			const FVector ForceUu = Cut.Structure.GetConnectionForce(BedJoint);
			const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(BedJoint);

			const FBedJointReading Published = ReadBedJoint(
				ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm, DryStone);

			const double ThrustRatio = FMath::Abs(ForceUu.Z) > 0.0
				? FMath::Abs(ForceUu.X) / FMath::Abs(ForceUu.Z)
				: 0.0;

			AddInfo(FString::Printf(
				TEXT("%s: joint %d carries (%s, %s, %s) uu — H/V = %s — sigma_n %s MPa against a ")
				TEXT("capacity of %s MPa; shear %s, compression %s"),
				Case.Description, BedJoint, *Bits(ForceUu.X), *Bits(ForceUu.Y), *Bits(ForceUu.Z),
				*Bits(ThrustRatio), *Bits(Published.NormalStressMPa),
				*Bits(DryStone.FrictionCoefficient * FMath::Abs(Published.NormalStressMPa)),
				*Bits(Published.ShearUtilisation), *Bits(Published.CompressionUtilisation)));

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the seat must be in COMPRESSION or there is no friction to ")
					TEXT("borrow; sigma_n is %s MPa"),
					Case.Description, *Bits(Published.NormalStressMPa)),
				Published.NormalStressMPa < 0.0);

			TestTrue(
				FString::Printf(
					TEXT("%s: the springing must read %s in shear — 0.866/0.7, with sigma_n ")
					TEXT("cancelled out, so it is the same at every span and every load — and it ")
					TEXT("reads %s"),
					Case.Description, *Bits(DryStoneSpringingUtilisation),
					*Bits(Published.ShearUtilisation)),
				FMath::Abs(Published.ShearUtilisation - DryStoneSpringingUtilisation)
					<= 0.02 * DryStoneSpringingUtilisation);

			TestTrue(
				FString::Printf(
					TEXT("%s: so a dry-stone opening can NEVER arch — the springing must be over ")
					TEXT("capacity, it reads %s"),
					Case.Description, *Bits(Published.ShearUtilisation)),
				Published.ShearUtilisation > 1.0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
