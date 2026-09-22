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
 * Named, not anonymous. An anonymous namespace is private to a translation unit, not a file,
 * and a unity build merges files into one — at which point two anonymous namespaces are the
 * same, and identically-named helpers in unrelated files are a hard compile error.
 */
namespace StructureThrustTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace StructureArchingTestSupport;

	/*
	 * The wall, the 0.866 constant, the H/V oracle and the two seat-counting helpers live in
	 * ArchingWallTestSupport.h, since slice 4's cover test needs the same 30 x 40 flush wall and a
	 * second copy would be two fixtures that drift.
	 */

	/**
	 * The opening is one course tall, deliberately. A multi-course opening has a reveal — a jamb
	 * brick one course below the spanning course, overhanging 5.625 cm into the opening with no
	 * head joint on its eccentric side, so the arch is refused and it peels as a genuine cantilever
	 * (CURRENT_STATE; slice 5). A one-course cut has no reveal, so nothing here measures it.
	 */
	constexpr int32 CutCourse = 1;

	/** The course whose bricks lose their seats: the one immediately above the cut. */
	constexpr int32 SpannedCourse = CutCourse + 1;

	/**
	 * How much masonry stands over the opening, cm — courses 2 through 39, one pitch each. This
	 * caps the arching depth, and is why the opening is cut at course 1: 285 cm is the deepest
	 * cover this wall has, so d_e is angle-limited on a narrow opening and cover-limited on a wide
	 * one, and the two cases below sit either side of that crossover.
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
	 * The three-cell fixture slice 2 landed, rebuilt here so the head joint it newly loads can be
	 * measured against the figure CURRENT_STATE records by hand. Same 7 x 30 wall, same cut.
	 */
	constexpr int32 SpannedHoleFirstCutBrickIndex = 1;
	constexpr int32 SpannedHoleCellCount = 3;
}

/**
 * An arch pushes sideways, that thrust is real, and the springing carries it in shear on its
 * own bed joint — so an opening too wide for its abutments comes down. Before this slice the
 * solver spanned any width with no check the abutment can take the horizontal reaction, the
 * indestructible failure ARCHING_DESIGN warns of, invisible because every bed joint carried a
 * purely vertical force.
 *
 * The physics (ARCHING_DESIGN.md): d_e = min(cover, 0.866*L); r = d_e/3; W is the load the
 * solver accumulated (not a triangle); H = W*L/(8r), V = W/2 per abutment. The thrust is then
 * shear on the abutment's bed joint against 0.2 + 0.6*sigma_n, truncated at the ceiling — no
 * new axis, strength or profile data.
 *
 * Asserted: the thrust exists (each springing's bed joint carries a non-zero horizontal force,
 * the red claim); sigma H = 0 across the arch (trap 2, equal magnitudes and opposite signs,
 * vacuous while both are zero so the non-zero row comes first); H/V = 3L/(4 d_e) at each
 * springing (W cancels, and on the ten-cell case the angle governs d_e so L cancels too, giving
 * the exact 3/(4*0.866) = 0.86605); the shear axis governs, asserted before any number; and dry
 * stone never arches at any span (cohesion 0, so 0.866/0.7 = 1.237 at every width,
 * load-independently, no per-material branch). The re-seat head joint is reported not asserted
 * (no slice owns it).
 *
 * At the 2026-08-14 mean re-anchor both widths now stand (capacity moved c 0.20->0.90, mu
 * 0.6->0.75); the too-wide discriminator is owed a replacement fixture (CURRENT_STATE). Never a
 * displacement anywhere; outcome is a count of joints failed under load and pieces with no path
 * to ground. No ticking world needed.
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
	 * The expected numbers are ratios of published strengths, so they mean what they say only while
	 * the profile still carries the figures they were derived against. Asserted, not imported: a
	 * test that read the profile would agree with a wrong profile.
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
	 * PART 1 — two widths, and the thrust is what the springings must afford. Both stand at mean
	 * strengths; the characteristic basis had twenty cells falling.
	 */

	/**
	 * One opening, and everything this file has an opinion on. Both are centred on x = 315, one
	 * cell left of the wall's centre, so the two differ in width and nothing else — a comparison
	 * between them is a comparison of span.
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
		 * Whether the cover (rather than the 0.866 angle) governs d_e here. Decides the H/V tolerance:
		 * the angle-governed ratio is the exact 3/(4 x 0.866) and gets 2%; the cover-governed one moves
		 * with how span and cover are measured and gets 12%. H/V is geometry and doesn't move when
		 * strengths do.
		 */
		bool bCoverGoverns;
	};

	/*
	 * Mean re-anchor (2026-08-13). The springing capacity is c + mu sigma, and both moved (0.20 ->
	 * 0.90, 0.6 -> 0.75), so the design cross-checks are re-derived through the design's own implied
	 * seat stress: 0.763 at ten cells implies sigma = 0.3738, reading 0.274 at the new capacity;
	 * 1.365 at twenty implies sigma = 0.7475, reading 0.606. The twenty-cell collapse arm is
	 * therefore gone: at mean strengths this wall affords the thrust at every width that fits it (a
	 * ~33-cell opening against a 30-cell wall would be needed). The replacement is owed
	 * (CURRENT_STATE): a wide opening under thin cover high in a tall wall, measured in the green
	 * phase, never tuned.
	 */
	const TArray<FThrustCase> Cases = {
		/*
		 * Ten cells, 225 cm. 0.866*L is 194.85, less than the 285 cm cover, so the angle governs the
		 * depth and the cover cap never binds. So this row reads the same whether or not slice 4's cover
		 * walk exists yet, and the tight ratio below is a fair thing to ask of slice 3 alone.
		 */
		{ TEXT("a 10-cell opening"), 10, 9, 0.274, false, false },

		/*
		 * Twenty cells, 450 cm. 0.866*L is 389.7, so the cover governs at 285 and the thrust ratio
		 * climbs to 1.184. On the retired characteristic basis this was the over-capacity row (design
		 * 1.365); at mean strengths it stands at ~0.61 and pins the cover-governed H/V and springing.
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
		 * The span, and its two readings. The clear opening is the width taken out; seat centroid to
		 * seat centroid it's 11.25 cm wider, since each springing keeps half a cell of bearing. The
		 * tolerances below cover the difference, and on the ten-cell case it doesn't arise (L cancels).
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
		 * One end of the arch: the half-seated brick the thrust is delivered to the ground through.
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
			 * The springing is the same half seat slices 1 and 2 arch — 10.25 x 10.25, loaded 5.625 cm off
			 * its centroid, against a kern reaching 1.7083 cm. Arbitrated against the producer, so the number
			 * and its reason fail together.
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
			 * And the opening is genuinely spanned, which makes this an arch at all. The middle bricks have
			 * no seat and are re-seated onto the group's abutments by slice 2; if any were Stranded or
			 * Falling this would be measuring a collapse, not a thrust.
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
			 * What the re-seat head joint beside it carries — reported, not asserted, since no slice owns
			 * it. Slice 2 routes the group's load outward through these head joints, so the one by a
			 * springing carries about half the group (~(cells-1)/2 columns) in pure shear against 0.2 MPa
			 * cohesion with no normal force. On three cells that's one column and 0.56; on ten cells it's
			 * far past capacity long before the springing is — a slice 2 consequence, not a missing thrust,
			 * printed so red output isn't misread as the thrust check deciding it.
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
			 * The horizontal component is signed as the joint stores it. Per ConnectionLoad the force is
			 * the one acting on PieceB; both springings name the brick below first, so the two entries are
			 * comparable and an outward-thrusting arch gives them opposite signs. Which sign is which end is
			 * under-determined, so it's printed; what's asserted is equal and opposite (trap 2).
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
			 * The seat is in compression, the gate the thrust line depends on: no compression, no thrust
			 * line, no arch. On a bed joint under gravity there had better be some.
			 */
			TestTrue(
				FString::Printf(TEXT("%s: FIXTURE: the seat must be in COMPRESSION, sigma_n is %s MPa"),
					*Where, *Bits(Published.NormalStressMPa)),
				Published.NormalStressMPa < 0.0);

			/*
			 * And the Mohr-Coulomb ceiling isn't deciding. ARCHING_DESIGN says the 1.3 MPa truncation is
			 * never reached here (capacity ~0.55 and 0.81 MPa on the two cases); if it were, the span limit
			 * would be governed by the cap, not friction, and both figures below would mean something else.
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
			 * H/V = 3L/(4 d_e), and W cancels out (see ThrustPerReaction): a statement about the thrust
			 * line's geometry alone, independent of the wall's weight or the split. Tolerance 2% on ten
			 * cells (angle governs, L cancels, so it's 3/(4*0.866) whatever anyone measures) and 12% on
			 * twenty (cover governs, so it moves with how span and cover are measured). 12% does not span
			 * the 0.866 a deferred cover cap would give, which is stated in the message, not tolerated.
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
			 * The axis, before any claim about the number. On an arched springing the compression axis reads
			 * 2|sigma_n|/f_c — a plausible small number beside the one asserted — so a fixture aimed at the
			 * thrust would silently measure compression the moment it happened to be higher.
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
			 * And the joint's own reading agrees with Mohr-Coulomb on the force and moment it reports. This
			 * pins that the thrust is evaluated as shear on the bed joint against c + mu*sigma_n, not through
			 * a second private rule — the design claim is that no new axis or strength is needed.
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
			 * And loosely against the design's own number — a factor of two either way, an order-of-magnitude
			 * cross-check. ARCHING_DESIGN calls its 356.3 cm critical span its least trustworthy figure and
			 * asks for the ordering instead; a factor of two still catches a missing 100x, a missing
			 * division by three in the rise, or a thrust taken as W rather than W*L/(8r).
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
		 * Trap 2 — sigma H = 0 across the arch, and nothing in the model would notice if it weren't.
		 * Every joint is evaluated independently, so applying +H at one springing and forgetting the
		 * other gives a net horizontal force out of nowhere while every joint reads plausibly. Asserted
		 * as two facts: equal magnitudes and opposing directions. Vacuous while both are zero (today's
		 * state), so the non-zero row above is what makes this mean anything.
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
		 * And the outcome, where a row claims one. (No mortared row claims a collapse since the mean
		 * re-anchor; the arm is kept for the owed replacement fixture.) A single severed joint isn't a
		 * collapse, so the claim is a count of pieces left with no path to ground — and that the two
		 * springings are among the joints that failed under load, the mechanism this slice adds. Counted
		 * by break pass, not HasGiven: a joint that went with a removed piece has HasGiven true and a
		 * pass of INDEX_NONE, since it never snapped.
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
			 * The masonry over the opening must lose its path to ground — at minimum the CellCount + 1
			 * bricks of the spanned course, the span the arch carried. A floor, not an exact count, since
			 * how far up the loss travels isn't claimed here.
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
	 * PART 2 — the re-seat head joint, which two slices load and nothing asserts. Slice 2 re-seats a
	 * seatless piece onto the head joints that take it closer to an abutment, the only joint it newly
	 * loads (CURRENT_STATE records "about 0.56" of shear on the three-cell fixture and that no test
	 * reads it). Slice 3's thrust arrives at exactly this joint. What's pinned is conservation, not
	 * the 0.56: the hanger has one route to ground, so the shear is its whole column, measured off
	 * the intact wall. The utilisation is only asserted under capacity and self-consistent, since if
	 * slice 3 carries the thrust along the ring the joint gains compression and reads ~0.35 — a
	 * legitimate implementation a literal row would forbid.
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
				 * A three-cell cut leaves four bricks over the hole: two springings at the edges with one seat
				 * each, two hangers in the middle with none. Each hanger is one head joint from its nearer
				 * springing, and that joint is the subject.
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
					 * Conservation, the tight claim. The hanger has exactly one route to ground, so everything on it
					 * leaves through this joint — and nothing above the spanned course changed when the hole was
					 * cut, so that column is the same number in both walls to the last bit.
					 */
					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: the shear through it must be the hanger's whole ")
							TEXT("column, %s uu; it carries %s uu vertically"),
							*Bits(IntactColumnUu), *Bits(FMath::Abs(ForceUu.Z))),
						FMath::Abs(FMath::Abs(ForceUu.Z) - IntactColumnUu)
							<= 1.0e-9 * IntactColumnUu);

					/*
					 * A re-seated piece is statically indeterminate by slice 2's rule: the group only routes when
					 * something seated stands on both sides, so the single head joint left is the bookkeeping route
					 * for a vertical share, not a claim the brick hangs off it. Treated as determinate it would carry
					 * its whole column across 11.25 cm and snap.
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
	 * PART 3 — dry stone cannot arch, at any span, and no new data says so. Cohesion is exactly 0
	 * and friction 0.7, so a dry joint's shear capacity is 0.7*sigma_n, and the thrust ratio
	 * 3L/(4 d_e) can't fall below 3/(4*0.866) = 0.866 (d_e capped at 0.866*L). The springing reads
	 * (0.866*sigma_n)/(0.7*sigma_n) = 1.2372 at every span and load, sigma_n cancelling. Two widths
	 * are cut so that if the answer moved with load or span the cancellation isn't happening. Two
	 * cells and five, not one and five: a one-cell opening leaves no seatless piece, so no group,
	 * no thrust (that topology is Structure.AMissingBrickIsBridgedNotCantilevered at shear exactly
	 * zero); asking the same topology for 1.2372 in one wall and 0.0 in another needs a material
	 * branch, the regression DESIGN.md §2 names. It's the strongest row here: it needs no new
	 * field, so an implementation that hard-coded mortar's numbers fails it. And physically right —
	 * you can't span a dry-stone hole with a flat arch; you need a curved ring or a lintel.
	 */
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		Spec.JointThicknessCm = MortarJointCm;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;

		/**
		 * Twenty courses; the figure is 0.866*112.5 = 97.425 cm. The five-cell opening spans 112.5 cm,
		 * so the angle wants 97.425 cm of depth, and d_e = min(cover, 0.866*L) leaves the angle governing
		 * only while the wall has that much cover. Twelve courses (75 cm) would hand the cover the
		 * decision when slice 4's walk lands, reading 3*112.5/(4*75)/0.7 = 1.6071 not 1.2372 — looking
		 * like a thrust regression rather than a too-short fixture. Cut at course 1 the cover is 135 cm,
		 * clearing 97.425 by 39%; fifteen courses would clear it by 0.075 cm, a coincidence not a margin.
		 */
		Spec.CoursesHigh = 20;
		Spec.BricksPerCourse = 12;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = DryStone;

		/**
		 * The unit's density is the clay brick's, and it doesn't matter — the row's own claim: the answer
		 * is a ratio of two stresses both linear in the load, so it's load- and density-independent.
		 * Cutting two widths changes the load by several and the assertion is the reading doesn't move.
		 */
		struct FDryStoneCase
		{
			const TCHAR* Description;
			int32 CellCount;
			int32 FirstCutBrickIndex;
		};

		const TArray<FDryStoneCase> DryStoneCases = {
			/*
			 * Two cells is the narrowest arch this model has: it leaves exactly one seatless brick, the
			 * smallest group slice 2 can span, with half-seated abutments two cells apart. One cell leaves no
			 * seatless brick and is not an arch (see the header).
			 */
			{ TEXT("dry stone, a TWO-cell opening"), 2, 5 },
			{ TEXT("dry stone, a FIVE-cell opening"), 5, 3 },
		};

		/*
		 * 0.866/0.7 = 1.237215440448697, sigma_n cancelled. Mean re-anchor invariant (2026-08-13): dry
		 * stone's row doesn't move (c and tension stay exact zeros, mu stays 0.7), so this identity and
		 * every dry-stone reading must come back bit-identical when the mortar rows flip. If a dry row
		 * moves at the flip, the flip touched a row it must not have.
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
			 * And the opening is genuinely spanned, which makes it an arch at all — the brick over the hole
			 * beside the springing must have no seat and still be Supported. This would catch the one-cell
			 * case: nothing seatless, so no group forms, no thrust develops, and the springing reads zero
			 * shear instead of 1.2372.
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
